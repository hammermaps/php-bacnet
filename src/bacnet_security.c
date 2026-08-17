#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "zend_exceptions.h"
#include "Zend/zend_smart_str.h"
#include <arpa/inet.h>
#include <math.h>
#include <string.h>
#include <time.h>

#ifndef BACDL_BIP
#define BACDL_BIP
#endif
#include "bacnet/bacdef.h"
#include "bacnet/bacenum.h"
#include "bacnet/npdu.h"

#include "../php_bacnet.h"
#include "bacnet_security.h"

#define SECURITY_MAX_CIDRS 128
#define SECURITY_DUPLICATES 4096

typedef struct { uint32_t network, mask; } security_cidr;
typedef struct {
    uint32_t ip;
    double tokens, who_tokens, write_tokens;
    double updated, who_updated, write_updated, last_seen;
    double violation_started, blocked_until;
    uint32_t violations;
    uint64_t accepted, rate_drops, acl_drops, blocked_drops, malformed;
    bool used;
} security_source;
typedef struct { uint32_t ip; uint64_t hash; double expires; bool used; } security_duplicate;
typedef struct {
    bool enabled;
    double per_source_rate, global_rate, who_is_rate, write_rate;
    uint32_t per_source_burst, global_burst, who_is_burst, write_burst;
    uint32_t flood_violations, max_sources;
    double flood_window, block_duration, duplicate_window, source_ttl, log_interval;
    char *allowed_networks, *denied_networks;
    security_cidr allowed[SECURITY_MAX_CIDRS], denied[SECURITY_MAX_CIDRS];
    uint16_t allowed_count, denied_count;
} security_options;
typedef struct {
    uint64_t accepted_packets, rate_drops, acl_drops, blocked_drops;
    uint64_t malformed_pdus, queue_overflows, deduplicated_writes;
} security_stats;

struct php_bacnet_security {
    security_options options;
    security_stats stats;
    security_source *sources;
    uint32_t source_count;
    double global_tokens, global_updated, last_log;
    uint64_t pending_warnings;
    security_duplicate duplicates[SECURITY_DUPLICATES];
    uint16_t duplicate_cursor;
};

static double security_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static uint32_t security_source_ip(const BACNET_ADDRESS *source)
{
    if (!source || source->mac_len < 4) return 0;
    return ((uint32_t)source->mac[0] << 24) | ((uint32_t)source->mac[1] << 16)
        | ((uint32_t)source->mac[2] << 8) | source->mac[3];
}

static void security_ip_string(uint32_t ip, char out[16])
{
    snprintf(out, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255,
        (ip >> 8) & 255, ip & 255);
}

static bool security_parse_cidr(const char *value, security_cidr *cidr)
{
    char copy[64], *slash;
    struct in_addr addr;
    size_t len = strlen(value);
    if (len == 0 || len >= sizeof(copy)) return false;
    memcpy(copy, value, len + 1);
    slash = strchr(copy, '/');
    long prefix = 32;
    if (slash) {
        char *end = NULL;
        *slash++ = '\0';
        prefix = strtol(slash, &end, 10);
        if (!*slash || !end || *end || prefix < 0 || prefix > 32) return false;
    }
    if (inet_pton(AF_INET, copy, &addr) != 1) return false;
    uint32_t ip = ntohl(addr.s_addr);
    cidr->mask = prefix == 0 ? 0 : UINT32_MAX << (32 - prefix);
    cidr->network = ip & cidr->mask;
    return true;
}

static bool security_parse_cidr_list(
    const char *list, security_cidr *out, uint16_t *count, const char *option)
{
    *count = 0;
    if (!list || !*list) return true;
    char *copy = estrdup(list), *save = NULL;
    for (char *item = php_strtok_r(copy, ", ", &save); item;
         item = php_strtok_r(NULL, ", ", &save)) {
        if (*count >= SECURITY_MAX_CIDRS || !security_parse_cidr(item, &out[*count])) {
            zend_value_error("Invalid IPv4 CIDR '%s' in %s", item, option);
            efree(copy);
            return false;
        }
        (*count)++;
    }
    efree(copy);
    return true;
}

static void security_options_from_ini(security_options *o)
{
    memset(o, 0, sizeof(*o));
    o->enabled = BACNET_G(server_security_enabled);
    o->per_source_rate = BACNET_G(server_per_source_rate);
    o->per_source_burst = BACNET_G(server_per_source_burst) > 0
        && (uint64_t)BACNET_G(server_per_source_burst) <= UINT32_MAX
        ? (uint32_t)BACNET_G(server_per_source_burst) : 0;
    o->global_rate = BACNET_G(server_global_rate);
    o->global_burst = BACNET_G(server_global_burst) > 0
        && (uint64_t)BACNET_G(server_global_burst) <= UINT32_MAX
        ? (uint32_t)BACNET_G(server_global_burst) : 0;
    o->who_is_rate = BACNET_G(server_who_is_rate);
    o->who_is_burst = BACNET_G(server_who_is_burst) > 0
        && (uint64_t)BACNET_G(server_who_is_burst) <= UINT32_MAX
        ? (uint32_t)BACNET_G(server_who_is_burst) : 0;
    o->write_rate = BACNET_G(server_write_rate);
    o->write_burst = BACNET_G(server_write_burst) > 0
        && (uint64_t)BACNET_G(server_write_burst) <= UINT32_MAX
        ? (uint32_t)BACNET_G(server_write_burst) : 0;
    o->flood_violations = BACNET_G(server_flood_violations) > 0
        && (uint64_t)BACNET_G(server_flood_violations) <= UINT32_MAX
        ? (uint32_t)BACNET_G(server_flood_violations) : 0;
    o->flood_window = BACNET_G(server_flood_window);
    o->block_duration = BACNET_G(server_block_duration);
    o->duplicate_window = BACNET_G(server_duplicate_window);
    o->max_sources = BACNET_G(server_max_sources) > 0
        && BACNET_G(server_max_sources) <= 65535
        ? (uint32_t)BACNET_G(server_max_sources) : 0;
    o->source_ttl = BACNET_G(server_source_ttl);
    o->log_interval = BACNET_G(server_log_interval);
    o->allowed_networks = estrdup(BACNET_G(server_allowed_networks) ?: "");
    o->denied_networks = estrdup(BACNET_G(server_denied_networks) ?: "");
}

static bool security_valid_number(double value) { return isfinite(value) && value > 0; }

static bool security_validate_options(security_options *o)
{
    if (!security_valid_number(o->per_source_rate) || !o->per_source_burst
        || !security_valid_number(o->global_rate) || !o->global_burst
        || !security_valid_number(o->who_is_rate) || !o->who_is_burst
        || !security_valid_number(o->write_rate) || !o->write_burst
        || !o->flood_violations || !security_valid_number(o->flood_window)
        || !security_valid_number(o->block_duration)
        || !security_valid_number(o->duplicate_window)
        || !o->max_sources || o->max_sources > 65535
        || !security_valid_number(o->source_ttl)
        || !security_valid_number(o->log_interval)) {
        zend_value_error("Security rates, bursts, windows and limits must be positive");
        return false;
    }
    return security_parse_cidr_list(o->allowed_networks, o->allowed,
               &o->allowed_count, "allowed_networks")
        && security_parse_cidr_list(o->denied_networks, o->denied,
               &o->denied_count, "denied_networks");
}

php_bacnet_security *php_bacnet_security_create(void)
{
    php_bacnet_security *s = ecalloc(1, sizeof(*s));
    security_options_from_ini(&s->options);
    if (!security_validate_options(&s->options)) {
        efree(s->options.allowed_networks); efree(s->options.denied_networks); efree(s);
        return NULL;
    }
    s->sources = ecalloc(s->options.max_sources, sizeof(*s->sources));
    s->global_tokens = s->options.global_burst;
    s->global_updated = s->last_log = security_now();
    return s;
}

void php_bacnet_security_destroy(php_bacnet_security *s)
{
    if (!s) return;
    efree(s->options.allowed_networks); efree(s->options.denied_networks);
    efree(s->sources); efree(s);
}

static bool security_numeric(zval *zv, double *value)
{
    if (Z_TYPE_P(zv) == IS_LONG) *value = (double)Z_LVAL_P(zv);
    else if (Z_TYPE_P(zv) == IS_DOUBLE) *value = Z_DVAL_P(zv);
    else return false;
    return true;
}

static bool security_network_array(zval *zv, char **result, const char *key)
{
    if (Z_TYPE_P(zv) != IS_ARRAY) { zend_value_error("%s must be an array of IPv4 CIDRs", key); return false; }
    smart_str list = {0}; zval *item;
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(zv), item) {
        if (Z_TYPE_P(item) != IS_STRING) { smart_str_free(&list); zend_value_error("%s must contain only strings", key); return false; }
        if (list.s && ZSTR_LEN(list.s)) smart_str_appendc(&list, ',');
        smart_str_append(&list, Z_STR_P(item));
    } ZEND_HASH_FOREACH_END();
    smart_str_0(&list);
    *result = list.s ? estrdup(ZSTR_VAL(list.s)) : estrdup("");
    smart_str_free(&list);
    return true;
}

bool php_bacnet_security_configure(php_bacnet_security *s, HashTable *overrides)
{
    security_options n = s->options;
    n.allowed_networks = estrdup(s->options.allowed_networks);
    n.denied_networks = estrdup(s->options.denied_networks);
    zend_string *key; zval *value;
    ZEND_HASH_FOREACH_STR_KEY_VAL(overrides, key, value) {
        if (!key) { zend_value_error("Security option keys must be strings"); goto fail; }
        const char *k = ZSTR_VAL(key); double number;
        if (!strcmp(k, "enabled")) {
            if (Z_TYPE_P(value) != IS_TRUE && Z_TYPE_P(value) != IS_FALSE) { zend_value_error("enabled must be bool"); goto fail; }
            n.enabled = zend_is_true(value);
        } else if (!strcmp(k, "allowed_networks") || !strcmp(k, "denied_networks")) {
            char *networks = NULL;
            if (!security_network_array(value, &networks, k)) goto fail;
            char **target = !strcmp(k, "allowed_networks") ? &n.allowed_networks : &n.denied_networks;
            efree(*target); *target = networks;
        } else {
            if (!security_numeric(value, &number)) { zend_value_error("%s must be int or float", k); goto fail; }
#define SET_DBL(name, field) if (!strcmp(k, name)) n.field = number
#define SET_UINT(name, field) if (!strcmp(k, name)) n.field = (number > 0 && number <= UINT32_MAX && floor(number) == number) ? (uint32_t)number : 0
            if (0) {}
            else SET_DBL("per_source_rate", per_source_rate);
            else SET_UINT("per_source_burst", per_source_burst);
            else SET_DBL("global_rate", global_rate);
            else SET_UINT("global_burst", global_burst);
            else SET_DBL("who_is_rate", who_is_rate);
            else SET_UINT("who_is_burst", who_is_burst);
            else SET_DBL("write_rate", write_rate);
            else SET_UINT("write_burst", write_burst);
            else SET_UINT("flood_violations", flood_violations);
            else SET_DBL("flood_window_seconds", flood_window);
            else SET_DBL("block_duration_seconds", block_duration);
            else SET_DBL("duplicate_window_seconds", duplicate_window);
            else SET_UINT("max_sources", max_sources);
            else SET_DBL("source_ttl_seconds", source_ttl);
            else SET_DBL("log_interval_seconds", log_interval);
            else { zend_value_error("Unknown security option '%s'", k); goto fail; }
#undef SET_DBL
#undef SET_UINT
        }
    } ZEND_HASH_FOREACH_END();
    if (!security_validate_options(&n)) goto fail;
    security_source *sources = ecalloc(n.max_sources, sizeof(*sources));
    efree(s->sources); efree(s->options.allowed_networks); efree(s->options.denied_networks);
    s->options = n; s->sources = sources; s->source_count = 0;
    memset(s->duplicates, 0, sizeof(s->duplicates));
    s->global_tokens = n.global_burst;
    s->global_updated = s->last_log = security_now();
    s->pending_warnings = 0;
    return true;
fail:
    efree(n.allowed_networks); efree(n.denied_networks); return false;
}

static void security_add_options(security_options *o, zval *result)
{
    add_assoc_bool(result, "enabled", o->enabled);
    add_assoc_double(result, "per_source_rate", o->per_source_rate);
    add_assoc_long(result, "per_source_burst", o->per_source_burst);
    add_assoc_double(result, "global_rate", o->global_rate);
    add_assoc_long(result, "global_burst", o->global_burst);
    add_assoc_double(result, "who_is_rate", o->who_is_rate);
    add_assoc_long(result, "who_is_burst", o->who_is_burst);
    add_assoc_double(result, "write_rate", o->write_rate);
    add_assoc_long(result, "write_burst", o->write_burst);
    add_assoc_long(result, "flood_violations", o->flood_violations);
    add_assoc_double(result, "flood_window_seconds", o->flood_window);
    add_assoc_double(result, "block_duration_seconds", o->block_duration);
    add_assoc_double(result, "duplicate_window_seconds", o->duplicate_window);
    add_assoc_long(result, "max_sources", o->max_sources);
    add_assoc_double(result, "source_ttl_seconds", o->source_ttl);
    add_assoc_double(result, "log_interval_seconds", o->log_interval);
    zval allowed, denied; array_init(&allowed); array_init(&denied);
    char *allowed_copy=estrdup(o->allowed_networks),*denied_copy=estrdup(o->denied_networks),*save=NULL;
    for(char *v=php_strtok_r(allowed_copy,", ",&save);v;v=php_strtok_r(NULL,", ",&save))add_next_index_string(&allowed,v);
    save=NULL;for(char *v=php_strtok_r(denied_copy,", ",&save);v;v=php_strtok_r(NULL,", ",&save))add_next_index_string(&denied,v);
    efree(allowed_copy);efree(denied_copy);
    add_assoc_zval(result, "allowed_networks", &allowed); add_assoc_zval(result, "denied_networks", &denied);
}

void php_bacnet_security_options_to_array(php_bacnet_security *s, zval *result)
{ array_init(result); security_add_options(&s->options, result); }

static uint32_t security_active_sources(php_bacnet_security *s, double now, uint32_t *blocked)
{
    uint32_t active=0; *blocked=0;
    for (uint32_t i=0;i<s->options.max_sources;i++) if (s->sources[i].used) {
        if (now - s->sources[i].last_seen > s->options.source_ttl) {
            memset(&s->sources[i], 0, sizeof(s->sources[i]));
            s->source_count--;
            continue;
        }
        active++;
        if (s->sources[i].blocked_until > now) (*blocked)++;
    }
    return active;
}

void php_bacnet_security_stats_to_array(php_bacnet_security *s, bool include, bool reset, zval *result)
{
    double now=security_now(); uint32_t blocked=0, active=security_active_sources(s,now,&blocked);
    array_init(result);
#define STAT(name) add_assoc_long(result, #name, (zend_long)s->stats.name)
    STAT(accepted_packets); STAT(rate_drops); STAT(acl_drops); STAT(blocked_drops);
    STAT(malformed_pdus); STAT(queue_overflows); STAT(deduplicated_writes);
#undef STAT
    add_assoc_long(result,"active_sources",active); add_assoc_long(result,"blocked_sources",blocked);
    if (include) {
        zval sources; array_init(&sources);
        for (uint32_t i=0;i<s->options.max_sources;i++) if (s->sources[i].used) {
            char ip[16]; security_ip_string(s->sources[i].ip,ip); zval row; array_init(&row);
            add_assoc_long(&row,"accepted_packets",s->sources[i].accepted);
            add_assoc_long(&row,"rate_drops",s->sources[i].rate_drops);
            add_assoc_long(&row,"acl_drops",s->sources[i].acl_drops);
            add_assoc_long(&row,"blocked_drops",s->sources[i].blocked_drops);
            add_assoc_long(&row,"malformed_pdus",s->sources[i].malformed);
            add_assoc_bool(&row,"blocked",s->sources[i].blocked_until>now);
            add_assoc_double(&row,"idle_seconds",now-s->sources[i].last_seen);
            add_assoc_zval(&sources,ip,&row);
        }
        add_assoc_zval(result,"sources",&sources);
    }
    if (reset) {
        memset(&s->stats,0,sizeof(s->stats));
        for (uint32_t i=0;i<s->options.max_sources;i++) {
            s->sources[i].accepted=s->sources[i].rate_drops=s->sources[i].acl_drops=0;
            s->sources[i].blocked_drops=s->sources[i].malformed=0;
        }
    }
}

static bool security_match(uint32_t ip, security_cidr *list, uint16_t count)
{ for(uint16_t i=0;i<count;i++) if((ip&list[i].mask)==list[i].network)return true; return false; }

static security_source *security_get_source(php_bacnet_security *s, uint32_t ip, double now)
{
    security_source *free_slot=NULL,*oldest=NULL;
    for(uint32_t i=0;i<s->options.max_sources;i++) {
        security_source *x=&s->sources[i];
        if(x->used&&now-x->last_seen>s->options.source_ttl){memset(x,0,sizeof(*x));s->source_count--;}
        if(x->used&&x->ip==ip){x->last_seen=now;return x;}
        if(!x->used){if(!free_slot)free_slot=x;}
        if(x->used&&(!oldest||x->last_seen<oldest->last_seen))oldest=x;
    }
    security_source *x=free_slot?free_slot:oldest; if(!x)return NULL;
    if(!x->used)s->source_count++;
    memset(x,0,sizeof(*x)); x->used=true;x->ip=ip;x->last_seen=now;
    x->tokens=s->options.per_source_burst;x->who_tokens=s->options.who_is_burst;x->write_tokens=s->options.write_burst;
    x->updated=x->who_updated=x->write_updated=now; return x;
}

static bool security_take(double *tokens,double *updated,double rate,double burst,double now)
{ *tokens=fmin(burst,*tokens+(now-*updated)*rate);*updated=now;if(*tokens<1)return false;*tokens-=1;return true; }

static void security_warn(php_bacnet_security *s,double now)
{
    s->pending_warnings++;
    if(now-s->last_log>=s->options.log_interval){
        php_error_docref(NULL,E_WARNING,"BACnet server security dropped %llu packets since last report",(unsigned long long)s->pending_warnings);
        s->pending_warnings=0;s->last_log=now;
    }
}

static void security_violation(php_bacnet_security *s,security_source *x,double now)
{
    if(now-x->violation_started>s->options.flood_window){x->violation_started=now;x->violations=0;}
    if(x->violations++ == 0)x->violation_started=now;
    if(x->violations>=s->options.flood_violations){x->blocked_until=now+s->options.block_duration;x->violations=0;x->violation_started=now;}
}

bool php_bacnet_security_accept(php_bacnet_security *s,const BACNET_ADDRESS *source,const uint8_t *pdu,uint16_t pdu_len,php_bacnet_packet_kind *kind)
{
    *kind=PHP_BACNET_PACKET_OTHER;if(!s->options.enabled){s->stats.accepted_packets++;return true;}
    double now=security_now();uint32_t ip=security_source_ip(source);security_source *x=security_get_source(s,ip,now);
    if(!x)return false;
    if(security_match(ip,s->options.denied,s->options.denied_count)||(s->options.allowed_count&&!security_match(ip,s->options.allowed,s->options.allowed_count))){s->stats.acl_drops++;x->acl_drops++;security_warn(s,now);return false;}
    if(x->blocked_until>now){s->stats.blocked_drops++;x->blocked_drops++;security_warn(s,now);return false;}
    BACNET_ADDRESS d,n;BACNET_NPDU_DATA h;int nl=bacnet_npdu_decode((uint8_t*)pdu,pdu_len,&d,&n,&h);
    if(nl<0||h.network_layer_message||pdu_len-(uint16_t)nl<2){s->stats.malformed_pdus++;x->malformed++;security_warn(s,now);return false;}
    const uint8_t *apdu=pdu+nl;uint16_t al=pdu_len-(uint16_t)nl;uint8_t type=apdu[0]&0xf0;
    if(type==PDU_TYPE_UNCONFIRMED_SERVICE_REQUEST&&apdu[1]==SERVICE_UNCONFIRMED_WHO_IS)*kind=PHP_BACNET_PACKET_WHO_IS;
    if(type==PDU_TYPE_CONFIRMED_SERVICE_REQUEST){if(al<4){s->stats.malformed_pdus++;x->malformed++;security_warn(s,now);return false;}if(apdu[3]==SERVICE_CONFIRMED_WRITE_PROPERTY)*kind=PHP_BACNET_PACKET_WRITE;}
    bool ok=security_take(&s->global_tokens,&s->global_updated,s->options.global_rate,s->options.global_burst,now)
        &&security_take(&x->tokens,&x->updated,s->options.per_source_rate,s->options.per_source_burst,now);
    if(ok&&*kind==PHP_BACNET_PACKET_WHO_IS)ok=security_take(&x->who_tokens,&x->who_updated,s->options.who_is_rate,s->options.who_is_burst,now);
    if(ok&&*kind==PHP_BACNET_PACKET_WRITE)ok=security_take(&x->write_tokens,&x->write_updated,s->options.write_rate,s->options.write_burst,now);
    if(!ok){s->stats.rate_drops++;x->rate_drops++;security_violation(s,x,now);security_warn(s,now);return false;}
    s->stats.accepted_packets++;x->accepted++;return true;
}

void php_bacnet_security_malformed(
    php_bacnet_security *s, const BACNET_ADDRESS *source)
{
    double now = security_now();
    security_source *entry = security_get_source(
        s, security_source_ip(source), now);
    s->stats.malformed_pdus++;
    if (entry) entry->malformed++;
    security_warn(s, now);
}
void php_bacnet_security_queue_overflow(php_bacnet_security *s){s->stats.queue_overflows++;security_warn(s,security_now());}

static uint64_t security_hash_write(uint32_t ip,const uint8_t *apdu,uint16_t len)
{ uint64_t h=1469598103934665603ULL;for(int i=0;i<4;i++){h^=(ip>>(i*8))&255;h*=1099511628211ULL;}for(uint16_t i=0;i<len;i++){h^=apdu[i];h*=1099511628211ULL;}return h; }
bool php_bacnet_security_is_duplicate_write(php_bacnet_security *s,const BACNET_ADDRESS *source,const uint8_t *apdu,uint16_t len)
{ if(!s->options.enabled)return false;double now=security_now();uint32_t ip=security_source_ip(source);uint64_t h=security_hash_write(ip,apdu,len);for(int i=0;i<SECURITY_DUPLICATES;i++)if(s->duplicates[i].used&&s->duplicates[i].expires>now&&s->duplicates[i].ip==ip&&s->duplicates[i].hash==h){s->stats.deduplicated_writes++;return true;}return false; }
void php_bacnet_security_record_write(php_bacnet_security *s,const BACNET_ADDRESS *source,const uint8_t *apdu,uint16_t len)
{ if(!s->options.enabled)return;security_duplicate *d=&s->duplicates[s->duplicate_cursor++%SECURITY_DUPLICATES];d->used=true;d->ip=security_source_ip(source);d->hash=security_hash_write(d->ip,apdu,len);d->expires=security_now()+s->options.duplicate_window; }
