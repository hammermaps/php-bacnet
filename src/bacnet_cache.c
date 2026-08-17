#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "zend_exceptions.h"
#include "../php_bacnet.h"
#include "bacnet_cache.h"

#include <lmdb.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CACHE_MAGIC 0x42414348u
#define CACHE_VERSION 2u
#define CACHE_SHM_SLOTS 2048u
#define CACHE_KEY_MAX 192u
#define CACHE_VALUE_MAX 4096u

static const char *partition_names[PHP_BACNET_CACHE_PARTITION_COUNT] = {
    "state", "object", "object_list", "device", "ip", "negative"
};

typedef enum { L2_NONE, L2_LMDB, L2_CALLBACK } l2_kind;

typedef struct {
    uint8_t used;
    uint8_t partition;
    uint16_t key_length;
    uint32_t value_length;
    uint64_t expires_at_ms;
    uint64_t touched;
    char key[CACHE_KEY_MAX];
    uint8_t value[CACHE_VALUE_MAX];
} cache_slot;

typedef struct {
    uint32_t magic;
    uint32_t version;
    pthread_mutex_t mutex;
    uint64_t tick;
    uint64_t generation[PHP_BACNET_CACHE_PARTITION_COUNT];
    cache_slot slots[CACHE_SHM_SLOTS];
} cache_shm;

typedef struct {
    uint64_t hits, misses, stores, expirations, evictions, invalidations;
    uint64_t refreshes, negative_hits, backend_errors, allocation_failures;
} cache_stats;

typedef struct {
    uint32_t magic;
    uint32_t length;
    uint64_t expires_at_ms;
    uint64_t checksum;
} cache_value_header;

struct php_bacnet_cache {
    bool enabled;
    bool part_enabled[PHP_BACNET_CACHE_PARTITION_COUNT];
    double ttl[PHP_BACNET_CACHE_PARTITION_COUNT];
    uint32_t max_entries[PHP_BACNET_CACHE_PARTITION_COUNT];
    char namespace_name[96];
    char shm_name[128];
    char lmdb_path[512];
    size_t l1_max_bytes, l2_max_bytes, lmdb_map_size;
    uint32_t coherence_interval_ms;
    double log_interval;
    double negative_whois_ttl;
    double negative_read_ttl;
    uint64_t last_log_ms;
    l2_kind l2;
    int shm_fd;
    cache_shm *shm;
    MDB_env *env;
    MDB_dbi dbi;
    zval backend;
    bool backend_active;
    bool in_callback;
    uint64_t last_coherence_ms[PHP_BACNET_CACHE_PARTITION_COUNT];
    uint64_t generations[PHP_BACNET_CACHE_PARTITION_COUNT];
    cache_stats stats;
};

static uint64_t wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t hash_string(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h;
}

static uint64_t hash_bytes(const uint8_t *data,size_t length)
{
    uint64_t h=1469598103934665603ULL;
    for(size_t i=0;i<length;i++){h^=data[i];h*=1099511628211ULL;}
    return h;
}

static void cache_warn(php_bacnet_cache *c, const char *message)
{
    uint64_t now = wall_ms();
    c->stats.backend_errors++;
    if (!c->last_log_ms || now - c->last_log_ms >= (uint64_t)(c->log_interval * 1000.0)) {
        php_error_docref(NULL, E_WARNING, "BACnet cache: %s", message);
        c->last_log_ms = now;
    }
}

static bool lock_shm(cache_shm *shm)
{
    if (!shm) return false;
    int rc = pthread_mutex_lock(&shm->mutex);
#ifdef EOWNERDEAD
    if (rc == EOWNERDEAD) { pthread_mutex_consistent(&shm->mutex); return true; }
#endif
    return rc == 0;
}

static void unlock_shm(cache_shm *shm) { if (shm) pthread_mutex_unlock(&shm->mutex); }

static bool open_shm(php_bacnet_cache *c)
{
    bool owner = false;
    c->shm_fd = shm_open(c->shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (c->shm_fd >= 0) owner = true;
    else if (errno == EEXIST) c->shm_fd = shm_open(c->shm_name, O_RDWR, 0600);
    if (c->shm_fd < 0) return false;
    if (owner && ftruncate(c->shm_fd, sizeof(cache_shm)) != 0) goto fail;
    c->shm = mmap(NULL, sizeof(cache_shm), PROT_READ | PROT_WRITE, MAP_SHARED, c->shm_fd, 0);
    if (c->shm == MAP_FAILED) { c->shm = NULL; goto fail; }
    if (owner) {
        memset(c->shm, 0, sizeof(*c->shm));
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
#ifdef PTHREAD_MUTEX_ROBUST
        pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
#endif
        pthread_mutex_init(&c->shm->mutex, &attr);
        pthread_mutexattr_destroy(&attr);
        c->shm->version = CACHE_VERSION;
        __sync_synchronize();
        c->shm->magic = CACHE_MAGIC;
    } else if (c->shm->magic != CACHE_MAGIC || c->shm->version != CACHE_VERSION) {
        munmap(c->shm, sizeof(cache_shm)); c->shm = NULL; goto fail;
    }
    return true;
fail:
    close(c->shm_fd); c->shm_fd = -1; return false;
}

static bool open_lmdb(php_bacnet_cache *c)
{
    struct stat st;
    if (stat(c->lmdb_path, &st) != 0 || !S_ISDIR(st.st_mode) || access(c->lmdb_path, W_OK) != 0) return false;
    if (mdb_env_create(&c->env) != MDB_SUCCESS) return false;
    mdb_env_set_mapsize(c->env, c->lmdb_map_size);
    mdb_env_set_maxdbs(c->env, 2);
    if (mdb_env_open(c->env, c->lmdb_path, 0, 0640) != MDB_SUCCESS) {
        mdb_env_close(c->env); c->env = NULL; return false;
    }
    MDB_txn *txn = NULL;
    if (mdb_txn_begin(c->env, NULL, 0, &txn) != MDB_SUCCESS
        || mdb_dbi_open(txn, "bacnet", MDB_CREATE, &c->dbi) != MDB_SUCCESS
        || mdb_txn_commit(txn) != MDB_SUCCESS) {
        if (txn) mdb_txn_abort(txn);
        mdb_env_close(c->env); c->env = NULL; return false;
    }
    return true;
}

static void make_full_key(php_bacnet_cache *c, int partition, const char *key,
    char *out, size_t out_size)
{
    const char *backend=c->l2==L2_CALLBACK?"callback":c->l2==L2_LMDB?"lmdb":"none";
    snprintf(out, out_size, "v%u|%s|%s|%s|%s", CACHE_VERSION, c->namespace_name,
        backend, partition_names[partition], key);
}

static bool callback_call(php_bacnet_cache *c, const char *method, uint32_t argc,
    zval *args, zval *retval)
{
    if (!c->backend_active || c->in_callback) return false;
    zval callable;
    array_init_size(&callable, 2);
    Z_TRY_ADDREF(c->backend);
    add_next_index_zval(&callable, &c->backend);
    add_next_index_string(&callable, method);
    c->in_callback = true;
    int rc = call_user_function(EG(function_table), NULL, &callable, retval, argc, args);
    c->in_callback = false;
    zval_ptr_dtor(&callable);
    if (rc == FAILURE || EG(exception)) {
        if (EG(exception)) zend_clear_exception();
        cache_warn(c, "PHP-Backend ist fehlgeschlagen; Netzwerkzugriff wird fortgesetzt");
        return false;
    }
    return true;
}

static void shm_clear_partition(php_bacnet_cache *c, int partition)
{
    if(!c||!c->shm||!lock_shm(c->shm))return;
    for(uint32_t i=0;i<CACHE_SHM_SLOTS;i++)
        if(c->shm->slots[i].used&&(partition<0||c->shm->slots[i].partition==partition))c->shm->slots[i].used=0;
    if(partition<0)for(int p=0;p<PHP_BACNET_CACHE_PARTITION_COUNT;p++)c->shm->generation[p]++;
    else c->shm->generation[partition]++;
    unlock_shm(c->shm);
}

static void callback_check_generation(php_bacnet_cache *c, int partition)
{
    uint64_t now=wall_ms();
    if(!c->backend_active||c->in_callback||now-c->last_coherence_ms[partition]<c->coherence_interval_ms)return;
    c->last_coherence_ms[partition]=now;
    zval args[2],retval;ZVAL_STRING(&args[0],c->namespace_name);ZVAL_STRING(&args[1],partition_names[partition]);ZVAL_UNDEF(&retval);
    if(callback_call(c,"getGeneration",2,args,&retval)&&Z_TYPE(retval)==IS_LONG){
        uint64_t generation=(uint64_t)Z_LVAL(retval);
        if(c->generations[partition]&&c->generations[partition]!=generation){shm_clear_partition(c,partition);c->stats.invalidations++;}
        c->generations[partition]=generation;
    }
    zval_ptr_dtor(&args[0]);zval_ptr_dtor(&args[1]);zval_ptr_dtor(&retval);
}

php_bacnet_cache *php_bacnet_cache_create(const char *iface, uint16_t port)
{
    php_bacnet_cache *c = pecalloc(1, sizeof(*c), 1);
    c->shm_fd = -1;
    c->enabled = BACNET_G(cache_enabled);
    c->part_enabled[PHP_BACNET_CACHE_STATE] = BACNET_G(cache_state_enabled);
    c->part_enabled[PHP_BACNET_CACHE_OBJECT] = BACNET_G(cache_object_enabled);
    c->part_enabled[PHP_BACNET_CACHE_OBJECT_LIST] = BACNET_G(cache_object_list_enabled);
    c->part_enabled[PHP_BACNET_CACHE_DEVICE] = BACNET_G(cache_device_enabled);
    c->part_enabled[PHP_BACNET_CACHE_IP] = BACNET_G(cache_ip_enabled);
    c->part_enabled[PHP_BACNET_CACHE_NEGATIVE] = BACNET_G(cache_negative_enabled);
    c->ttl[0] = BACNET_G(cache_state_ttl); c->ttl[1] = BACNET_G(cache_object_ttl);
    c->ttl[2] = BACNET_G(cache_object_list_ttl); c->ttl[3] = BACNET_G(cache_device_ttl);
    c->ttl[4] = BACNET_G(cache_ip_ttl); c->ttl[5] = BACNET_G(cache_negative_read_ttl);
    c->max_entries[0] = BACNET_G(cache_state_max_entries);
    c->max_entries[1] = BACNET_G(cache_object_max_entries);
    c->max_entries[2] = BACNET_G(cache_object_list_max_entries);
    c->max_entries[3] = BACNET_G(cache_device_max_entries);
    c->max_entries[4] = BACNET_G(cache_ip_max_entries);
    c->max_entries[5] = BACNET_G(cache_negative_max_entries);
    c->l1_max_bytes = BACNET_G(cache_l1_max_bytes);
    c->l2_max_bytes = BACNET_G(cache_l2_max_bytes);
    c->lmdb_map_size = BACNET_G(cache_lmdb_map_size);
    c->coherence_interval_ms = BACNET_G(cache_coherence_interval_ms);
    c->log_interval = BACNET_G(cache_log_interval);
    c->negative_whois_ttl = BACNET_G(cache_negative_whois_ttl);
    c->negative_read_ttl = BACNET_G(cache_negative_read_ttl);
    snprintf(c->namespace_name, sizeof(c->namespace_name), "%s", BACNET_G(cache_namespace) && *BACNET_G(cache_namespace)
        ? BACNET_G(cache_namespace) : "default");
    if (!BACNET_G(cache_namespace) || !*BACNET_G(cache_namespace))
        snprintf(c->namespace_name, sizeof(c->namespace_name), "%s:%u", iface ? iface : "auto", port);
    snprintf(c->lmdb_path, sizeof(c->lmdb_path), "%s", BACNET_G(cache_lmdb_path));
    if (BACNET_G(cache_shm_name) && *BACNET_G(cache_shm_name))
        snprintf(c->shm_name, sizeof(c->shm_name), "%s", BACNET_G(cache_shm_name));
    else snprintf(c->shm_name, sizeof(c->shm_name), "/php_bacnet_%016llx_v%u",
        (unsigned long long)hash_string(c->namespace_name),CACHE_VERSION);
    ZVAL_UNDEF(&c->backend);
    c->l2 = L2_LMDB;
    if (BACNET_G(cache_l2_backend)) {
        if (!*BACNET_G(cache_l2_backend) || !strcmp(BACNET_G(cache_l2_backend), "none")) c->l2 = L2_NONE;
        else if (strcmp(BACNET_G(cache_l2_backend), "lmdb")) c->l2 = L2_LMDB;
    }
    if (c->enabled && !open_shm(c)) cache_warn(c, "Shared-Memory-L1 konnte nicht geöffnet werden");
    if (c->enabled && c->l2 == L2_LMDB && !open_lmdb(c))
        cache_warn(c, "LMDB-L2 ist nicht verfügbar; Shared-Memory-L1 bleibt aktiv");
    return c;
}

void php_bacnet_cache_destroy(php_bacnet_cache *c)
{
    if (!c) return;
    if (c->backend_active) zval_ptr_dtor(&c->backend);
    if (c->env) { mdb_dbi_close(c->env, c->dbi); mdb_env_close(c->env); }
    if (c->shm) munmap(c->shm, sizeof(cache_shm));
    if (c->shm_fd >= 0) close(c->shm_fd);
    pefree(c, 1);
}

bool php_bacnet_cache_partition_enabled(php_bacnet_cache *c, php_bacnet_cache_partition p)
{ return c && c->enabled && p < PHP_BACNET_CACHE_PARTITION_COUNT && c->part_enabled[p]; }
double php_bacnet_cache_partition_ttl(php_bacnet_cache *c, php_bacnet_cache_partition p)
{ return c && p < PHP_BACNET_CACHE_PARTITION_COUNT ? c->ttl[p] : 0; }
double php_bacnet_cache_negative_whois_ttl(php_bacnet_cache *c)
{ return c ? c->negative_whois_ttl : 0; }

static bool shm_get(php_bacnet_cache *c, int p, const char *full, uint8_t *data, uint32_t *length)
{
    if (!c->shm || !lock_shm(c->shm)) return false;
    uint64_t now = wall_ms(); bool found = false;
    for (uint32_t i = 0; i < CACHE_SHM_SLOTS; i++) {
        cache_slot *s = &c->shm->slots[i];
        if (s->used && s->partition == p && !strcmp(s->key, full)) {
            if (s->expires_at_ms <= now) { s->used = 0; c->stats.expirations++; break; }
            if (*length >= s->value_length) {
                memcpy(data, s->value, s->value_length); *length = s->value_length;
                s->touched = ++c->shm->tick; found = true;
            }
            break;
        }
    }
    unlock_shm(c->shm); return found;
}

static void shm_put(php_bacnet_cache *c, int p, const char *full,
    const uint8_t *data, uint32_t length, uint64_t expiry)
{
    if (!c->shm || length > CACHE_VALUE_MAX || strlen(full) >= CACHE_KEY_MAX || !lock_shm(c->shm)) {
        if (length > CACHE_VALUE_MAX) c->stats.allocation_failures++; return;
    }
    cache_slot *target = NULL, *lru = NULL, *global_lru = NULL;
    uint64_t oldest = UINT64_MAX, global_oldest = UINT64_MAX; uint32_t count = 0;
    for (uint32_t i = 0; i < CACHE_SHM_SLOTS; i++) {
        cache_slot *s = &c->shm->slots[i];
        if (s->used && s->partition == p) count++;
        if (s->used && !strcmp(s->key, full)) { target = s; break; }
        if (!s->used && !target) target = s;
        if (s->used && s->partition == p && s->touched < oldest) { oldest = s->touched; lru = s; }
        if (s->used && s->touched < global_oldest) { global_oldest=s->touched; global_lru=s; }
    }
    if ((!target || (target->used && strcmp(target->key, full))) || count >= c->max_entries[p]) {
        target = lru ? lru : global_lru; if (target) c->stats.evictions++;
    }
    if (target) {
        memset(target, 0, sizeof(*target)); target->used = 1; target->partition = p;
        target->key_length = strlen(full); memcpy(target->key, full, target->key_length + 1);
        target->value_length = length; if (length) memcpy(target->value, data, length);
        target->expires_at_ms = expiry; target->touched = ++c->shm->tick;
    }
    unlock_shm(c->shm);
}

static bool lmdb_get(php_bacnet_cache *c, const char *full, uint8_t *data, uint32_t *length)
{
    if (!c->env) return false;
    MDB_txn *txn; MDB_val key = { strlen(full), (void *)full }, val;
    if (mdb_txn_begin(c->env, NULL, MDB_RDONLY, &txn) != MDB_SUCCESS) return false;
    int rc = mdb_get(txn, c->dbi, &key, &val);
    if (rc == MDB_SUCCESS && val.mv_size >= sizeof(cache_value_header)) {
        cache_value_header header;memcpy(&header,val.mv_data,sizeof(header));
        size_t payload=val.mv_size-sizeof(header);const uint8_t *stored=(uint8_t *)val.mv_data+sizeof(header);
        if(header.magic==CACHE_MAGIC&&header.length==payload&&header.expires_at_ms>wall_ms()
            &&payload<=*length&&header.checksum==hash_bytes(stored,payload)){
            memcpy(data,stored,payload); *length=payload;
            mdb_txn_abort(txn); return true;
        }
    }
    mdb_txn_abort(txn); return false;
}

static void lmdb_put(php_bacnet_cache *c, const char *full, const uint8_t *data,
    uint32_t length, uint64_t expiry)
{
    if (!c->env) return;
    uint8_t buf[sizeof(cache_value_header) + CACHE_VALUE_MAX];
    if (length > CACHE_VALUE_MAX) return;
    cache_value_header header={CACHE_MAGIC,length,expiry,hash_bytes(data,length)};
    memcpy(buf,&header,sizeof(header));if(length)memcpy(buf+sizeof(header),data,length);
    MDB_txn *txn=NULL; MDB_val key = { strlen(full), (void *)full }, val = { sizeof(header) + length, buf };
    if (mdb_txn_begin(c->env, NULL, 0, &txn) != MDB_SUCCESS
        || mdb_put(txn, c->dbi, &key, &val, 0) != MDB_SUCCESS
        || mdb_txn_commit(txn) != MDB_SUCCESS) {
        if (txn) mdb_txn_abort(txn); cache_warn(c, "LMDB-Schreibvorgang fehlgeschlagen");
    }
}

static void lmdb_prune(php_bacnet_cache *c,int partition)
{
    if(!c->env)return;
    MDB_txn *txn=NULL;MDB_cursor *cursor=NULL;char prefix[320],oldest_key[512]={0};size_t oldest_key_len=0;
    snprintf(prefix,sizeof(prefix),"v%u|%s|lmdb|%s|",CACHE_VERSION,c->namespace_name,partition_names[partition]);
    if(mdb_txn_begin(c->env,NULL,0,&txn)!=MDB_SUCCESS||mdb_cursor_open(txn,c->dbi,&cursor)!=MDB_SUCCESS){if(txn)mdb_txn_abort(txn);return;}
    MDB_val key={strlen(prefix),prefix},value;uint64_t oldest_expiry=UINT64_MAX;uint32_t count=0;int rc=mdb_cursor_get(cursor,&key,&value,MDB_SET_RANGE);
    while(rc==MDB_SUCCESS&&key.mv_size>=strlen(prefix)&&!memcmp(key.mv_data,prefix,strlen(prefix))){
        cache_value_header header={0};if(value.mv_size>=sizeof(header))memcpy(&header,value.mv_data,sizeof(header));
        if(header.magic!=CACHE_MAGIC||header.expires_at_ms<=wall_ms()){mdb_cursor_del(cursor,0);c->stats.expirations++;rc=mdb_cursor_get(cursor,&key,&value,MDB_NEXT);continue;}
        count++;if(header.expires_at_ms<oldest_expiry&&key.mv_size<sizeof(oldest_key)){oldest_expiry=header.expires_at_ms;oldest_key_len=key.mv_size;memcpy(oldest_key,key.mv_data,key.mv_size);}
        rc=mdb_cursor_get(cursor,&key,&value,MDB_NEXT);
    }
    mdb_cursor_close(cursor);cursor=NULL;
    if(count>c->max_entries[partition]&&oldest_key_len){MDB_val victim={oldest_key_len,oldest_key};mdb_del(txn,c->dbi,&victim,NULL);c->stats.evictions++;}

    snprintf(prefix,sizeof(prefix),"v%u|%s|lmdb|",CACHE_VERSION,c->namespace_name);oldest_key_len=0;oldest_expiry=UINT64_MAX;size_t bytes=0;
    if(mdb_cursor_open(txn,c->dbi,&cursor)==MDB_SUCCESS){key.mv_size=strlen(prefix);key.mv_data=prefix;rc=mdb_cursor_get(cursor,&key,&value,MDB_SET_RANGE);
        while(rc==MDB_SUCCESS&&key.mv_size>=strlen(prefix)&&!memcmp(key.mv_data,prefix,strlen(prefix))){cache_value_header header={0};if(value.mv_size>=sizeof(header))memcpy(&header,value.mv_data,sizeof(header));bytes+=value.mv_size;
            if(header.magic==CACHE_MAGIC&&header.expires_at_ms<oldest_expiry&&key.mv_size<sizeof(oldest_key)){oldest_expiry=header.expires_at_ms;oldest_key_len=key.mv_size;memcpy(oldest_key,key.mv_data,key.mv_size);}rc=mdb_cursor_get(cursor,&key,&value,MDB_NEXT);}mdb_cursor_close(cursor);}
    if(bytes>c->l2_max_bytes&&oldest_key_len){MDB_val victim={oldest_key_len,oldest_key};mdb_del(txn,c->dbi,&victim,NULL);c->stats.evictions++;}
    mdb_txn_commit(txn);
}

bool php_bacnet_cache_get(php_bacnet_cache *c, php_bacnet_cache_partition p,
    const char *key, uint8_t *data, uint32_t *length)
{
    if (!php_bacnet_cache_partition_enabled(c, p)) return false;
    if(c->l2==L2_CALLBACK)callback_check_generation(c,p);
    char full[512]; make_full_key(c, p, key, full, sizeof(full));
    uint32_t cap = *length;
    if (shm_get(c, p, full, data, length)) { c->stats.hits++; if (p == PHP_BACNET_CACHE_NEGATIVE) c->stats.negative_hits++; return true; }
    *length = cap;
    bool hit = false;
    if (c->l2 == L2_LMDB) hit = lmdb_get(c, full, data, length);
    else if (c->l2 == L2_CALLBACK && c->backend_active) {
        zval args[3], retval; ZVAL_STRING(&args[0], c->namespace_name);
        ZVAL_STRING(&args[1], partition_names[p]); ZVAL_STRING(&args[2], key); ZVAL_UNDEF(&retval);
        if (callback_call(c, "get", 3, args, &retval) && Z_TYPE(retval) == IS_STRING
            && Z_STRLEN(retval) <= cap) { memcpy(data, Z_STRVAL(retval), Z_STRLEN(retval)); *length = Z_STRLEN(retval); hit = true; }
        zval_ptr_dtor(&args[0]); zval_ptr_dtor(&args[1]); zval_ptr_dtor(&args[2]); zval_ptr_dtor(&retval);
    }
    if (hit) { shm_put(c, p, full, data, *length, wall_ms() + (uint64_t)(c->ttl[p] * 1000)); c->stats.hits++; }
    else c->stats.misses++;
    return hit;
}

void php_bacnet_cache_put(php_bacnet_cache *c, php_bacnet_cache_partition p,
    const char *key, const uint8_t *data, uint32_t length, double ttl_seconds)
{
    if (!php_bacnet_cache_partition_enabled(c, p) || ttl_seconds <= 0) return;
    char full[512]; make_full_key(c, p, key, full, sizeof(full));
    uint64_t expiry = wall_ms() + (uint64_t)(ttl_seconds * 1000.0);
    shm_put(c, p, full, data, length, expiry);
    if (c->l2 == L2_LMDB) { lmdb_put(c, full, data, length, expiry); lmdb_prune(c,p); }
    else if (c->l2 == L2_CALLBACK && c->backend_active) {
        zval args[6], retval; ZVAL_STRING(&args[0], c->namespace_name);
        ZVAL_STRING(&args[1], partition_names[p]); ZVAL_STRING(&args[2], key);
        ZVAL_STRINGL(&args[3], (const char *)data, length); ZVAL_LONG(&args[4], expiry);
        ZVAL_LONG(&args[5], c->max_entries[p]); ZVAL_UNDEF(&retval);
        callback_call(c, "set", 6, args, &retval);
        for (int i=0;i<4;i++) zval_ptr_dtor(&args[i]); zval_ptr_dtor(&retval);
        zval generation_args[2],generation_retval;
        ZVAL_STRING(&generation_args[0],c->namespace_name);ZVAL_STRING(&generation_args[1],partition_names[p]);ZVAL_UNDEF(&generation_retval);
        if(callback_call(c,"bumpGeneration",2,generation_args,&generation_retval)&&Z_TYPE(generation_retval)==IS_LONG)c->generations[p]=Z_LVAL(generation_retval);
        zval_ptr_dtor(&generation_args[0]);zval_ptr_dtor(&generation_args[1]);zval_ptr_dtor(&generation_retval);
    }
    c->stats.stores++;
}

void php_bacnet_cache_clear(php_bacnet_cache *c, int partition)
{
    if (!c) return;
    shm_clear_partition(c,partition); c->stats.invalidations++;
    if (c->env) {
        char prefix[320];
        if (partition < 0) snprintf(prefix,sizeof(prefix),"v%u|%s|lmdb|",CACHE_VERSION,c->namespace_name);
        else snprintf(prefix,sizeof(prefix),"v%u|%s|lmdb|%s|",CACHE_VERSION,c->namespace_name,partition_names[partition]);
        MDB_txn *txn=NULL; MDB_cursor *cursor=NULL;
        if(mdb_txn_begin(c->env,NULL,0,&txn)==MDB_SUCCESS && mdb_cursor_open(txn,c->dbi,&cursor)==MDB_SUCCESS){
            MDB_val key={strlen(prefix),prefix},value; int rc=mdb_cursor_get(cursor,&key,&value,MDB_SET_RANGE);
            while(rc==MDB_SUCCESS && key.mv_size>=strlen(prefix) && !memcmp(key.mv_data,prefix,strlen(prefix))){
                mdb_cursor_del(cursor,0);rc=mdb_cursor_get(cursor,&key,&value,MDB_NEXT);
            }
            mdb_cursor_close(cursor);mdb_txn_commit(txn);
        } else if(txn) mdb_txn_abort(txn);
    }
    if (c->l2 == L2_CALLBACK && c->backend_active) {
        zval args[2], retval; ZVAL_STRING(&args[0],c->namespace_name);
        if (partition < 0) ZVAL_NULL(&args[1]); else ZVAL_STRING(&args[1],partition_names[partition]);
        ZVAL_UNDEF(&retval); callback_call(c,"clear",2,args,&retval);
        zval_ptr_dtor(&args[0]); zval_ptr_dtor(&args[1]); zval_ptr_dtor(&retval);
        int first=partition<0?0:partition,last=partition<0?PHP_BACNET_CACHE_PARTITION_COUNT-1:partition;
        for(int p=first;p<=last;p++){zval gargs[2],grv;ZVAL_STRING(&gargs[0],c->namespace_name);ZVAL_STRING(&gargs[1],partition_names[p]);ZVAL_UNDEF(&grv);
            if(callback_call(c,"bumpGeneration",2,gargs,&grv)&&Z_TYPE(grv)==IS_LONG)c->generations[p]=Z_LVAL(grv);
            zval_ptr_dtor(&gargs[0]);zval_ptr_dtor(&gargs[1]);zval_ptr_dtor(&grv);}
    }
}

void php_bacnet_cache_invalidate(php_bacnet_cache *c, php_bacnet_cache_partition p, const char *scope)
{
    (void)scope; php_bacnet_cache_clear(c, p);
}
void php_bacnet_cache_refresh(php_bacnet_cache *c, php_bacnet_cache_partition p, const char *scope)
{
    if (c) c->stats.refreshes++;
    php_bacnet_cache_invalidate(c, p, scope);
}

static int partition_from_name(const char *name)
{ for(int i=0;i<PHP_BACNET_CACHE_PARTITION_COUNT;i++) if(!strcmp(name,partition_names[i])) return i; return -1; }

bool php_bacnet_cache_set_options(php_bacnet_cache *c, HashTable *options, zend_string **error)
{
    zend_string *key; zval *value; bool reopen_shm=false,reopen_lmdb=false;
    bool was_enabled=c->enabled;
    ZEND_HASH_FOREACH_STR_KEY_VAL(options,key,value) {
        if (!key) { const char *message="Cache-Optionen müssen String-Schlüssel verwenden";
            *error=zend_string_init(message,strlen(message),0); return false; }
        const char *k=ZSTR_VAL(key);
        if (!strcmp(k,"enabled") && (Z_TYPE_P(value)==IS_TRUE||Z_TYPE_P(value)==IS_FALSE)) c->enabled=zend_is_true(value);
        else if (!strcmp(k,"namespace") && Z_TYPE_P(value)==IS_STRING && Z_STRLEN_P(value)<sizeof(c->namespace_name)) {
            snprintf(c->namespace_name,sizeof(c->namespace_name),"%s",Z_STRVAL_P(value));reopen_shm=true;
        }
        else if (!strcmp(k,"shm_name") && Z_TYPE_P(value)==IS_STRING && Z_STRLEN_P(value)>1
            && Z_STRVAL_P(value)[0]=='/' && Z_STRLEN_P(value)<sizeof(c->shm_name)) {
            snprintf(c->shm_name,sizeof(c->shm_name),"%s",Z_STRVAL_P(value));reopen_shm=true;
        }
        else if (!strcmp(k,"lmdb_path") && Z_TYPE_P(value)==IS_STRING && Z_STRLEN_P(value)<sizeof(c->lmdb_path)) {
            snprintf(c->lmdb_path,sizeof(c->lmdb_path),"%s",Z_STRVAL_P(value));reopen_lmdb=true;
        }
        else if (!strcmp(k,"l1_max_bytes") && Z_TYPE_P(value)==IS_LONG && Z_LVAL_P(value)>0) c->l1_max_bytes=Z_LVAL_P(value);
        else if (!strcmp(k,"l2_max_bytes") && Z_TYPE_P(value)==IS_LONG && Z_LVAL_P(value)>0) c->l2_max_bytes=Z_LVAL_P(value);
        else if (!strcmp(k,"lmdb_map_size") && Z_TYPE_P(value)==IS_LONG && Z_LVAL_P(value)>=1048576) {c->lmdb_map_size=Z_LVAL_P(value);reopen_lmdb=true;}
        else if (!strcmp(k,"coherence_interval_ms") && Z_TYPE_P(value)==IS_LONG && Z_LVAL_P(value)>=0) c->coherence_interval_ms=Z_LVAL_P(value);
        else if (!strcmp(k,"log_interval") && (Z_TYPE_P(value)==IS_LONG||Z_TYPE_P(value)==IS_DOUBLE) && zval_get_double(value)>=0) c->log_interval=zval_get_double(value);
        else if (!strcmp(k,"negative_whois_ttl") && (Z_TYPE_P(value)==IS_LONG||Z_TYPE_P(value)==IS_DOUBLE) && zval_get_double(value)>=0) c->negative_whois_ttl=zval_get_double(value);
        else if (!strcmp(k,"negative_read_ttl") && (Z_TYPE_P(value)==IS_LONG||Z_TYPE_P(value)==IS_DOUBLE) && zval_get_double(value)>=0) {c->negative_read_ttl=zval_get_double(value);c->ttl[PHP_BACNET_CACHE_NEGATIVE]=c->negative_read_ttl;}
        else if (!strcmp(k,"l2_backend") && Z_TYPE_P(value)==IS_STRING) {
            const char *v=Z_STRVAL_P(value);
            if(!strcmp(v,"lmdb")) c->l2=L2_LMDB; else if(!strcmp(v,"none")) c->l2=L2_NONE;
            else { *error=zend_strpprintf(0,"Ungültiges L2-Backend: %s",v); return false; }
            if(c->backend_active){zval_ptr_dtor(&c->backend);ZVAL_UNDEF(&c->backend);c->backend_active=false;}
        } else {
            bool matched=false;
            for(int p=0;p<PHP_BACNET_CACHE_PARTITION_COUNT;p++) {
                char option[64]; snprintf(option,sizeof(option),"%s_enabled",partition_names[p]);
                if(!strcmp(k,option)&&(Z_TYPE_P(value)==IS_TRUE||Z_TYPE_P(value)==IS_FALSE)){c->part_enabled[p]=zend_is_true(value);matched=true;break;}
                snprintf(option,sizeof(option),"%s_ttl",partition_names[p]);
                if(!strcmp(k,option)&&(Z_TYPE_P(value)==IS_LONG||Z_TYPE_P(value)==IS_DOUBLE)){double d=zval_get_double(value);if(d<0){*error=zend_strpprintf(0,"%s darf nicht negativ sein",k);return false;}c->ttl[p]=d;matched=true;break;}
                snprintf(option,sizeof(option),"%s_max_entries",partition_names[p]);
                if(!strcmp(k,option)&&Z_TYPE_P(value)==IS_LONG&&Z_LVAL_P(value)>0){c->max_entries[p]=Z_LVAL_P(value);matched=true;break;}
            }
            if(!matched){*error=zend_strpprintf(0,"Unbekannte oder ungültige Cache-Option: %s",k);return false;}
        }
    } ZEND_HASH_FOREACH_END();
    if(!was_enabled&&c->enabled){reopen_shm=true;if(c->l2==L2_LMDB)reopen_lmdb=true;}
    if(reopen_shm){if(c->shm)munmap(c->shm,sizeof(cache_shm));if(c->shm_fd>=0)close(c->shm_fd);c->shm=NULL;c->shm_fd=-1;if(c->enabled&&!open_shm(c))cache_warn(c,"Shared-Memory-L1 konnte nicht neu geöffnet werden");}
    if(reopen_lmdb){if(c->env){mdb_dbi_close(c->env,c->dbi);mdb_env_close(c->env);c->env=NULL;}if(c->enabled&&c->l2==L2_LMDB&&!open_lmdb(c))cache_warn(c,"LMDB-L2 konnte nicht neu geöffnet werden");}
    php_bacnet_cache_clear(c,-1); return true;
}

void php_bacnet_cache_get_options(php_bacnet_cache *c, zval *rv)
{
    array_init(rv); add_assoc_bool(rv,"enabled",c->enabled);
    const char *l2=c->l2==L2_LMDB?"lmdb":c->l2==L2_CALLBACK?"callback":"none";
    add_assoc_string(rv,"l1_backend","shared_memory"); add_assoc_string(rv,"l2_backend",(char*)l2);
    add_assoc_string(rv,"namespace",c->namespace_name); add_assoc_string(rv,"shm_name",c->shm_name);
    add_assoc_string(rv,"lmdb_path",c->lmdb_path); add_assoc_long(rv,"l1_max_bytes",c->l1_max_bytes);
    add_assoc_long(rv,"l2_max_bytes",c->l2_max_bytes); add_assoc_long(rv,"lmdb_map_size",c->lmdb_map_size);
    add_assoc_long(rv,"coherence_interval_ms",c->coherence_interval_ms);add_assoc_double(rv,"log_interval",c->log_interval);
    add_assoc_double(rv,"negative_whois_ttl",c->negative_whois_ttl);add_assoc_double(rv,"negative_read_ttl",c->negative_read_ttl);
    for(int p=0;p<PHP_BACNET_CACHE_PARTITION_COUNT;p++) { char k[64];
        snprintf(k,sizeof(k),"%s_enabled",partition_names[p]);add_assoc_bool(rv,k,c->part_enabled[p]);
        snprintf(k,sizeof(k),"%s_ttl",partition_names[p]);add_assoc_double(rv,k,c->ttl[p]);
        snprintf(k,sizeof(k),"%s_max_entries",partition_names[p]);add_assoc_long(rv,k,c->max_entries[p]); }
}

void php_bacnet_cache_get_stats(php_bacnet_cache *c, bool include_entries, bool reset, zval *rv)
{
    array_init(rv); add_assoc_long(rv,"hits",c->stats.hits);add_assoc_long(rv,"misses",c->stats.misses);
    add_assoc_long(rv,"stores",c->stats.stores);add_assoc_long(rv,"expirations",c->stats.expirations);
    add_assoc_long(rv,"evictions",c->stats.evictions);add_assoc_long(rv,"invalidations",c->stats.invalidations);
    add_assoc_long(rv,"refreshes",c->stats.refreshes);
    add_assoc_long(rv,"negative_hits",c->stats.negative_hits);add_assoc_long(rv,"backend_errors",c->stats.backend_errors);
    add_assoc_long(rv,"allocation_failures",c->stats.allocation_failures);
    add_assoc_bool(rv,"l1_available",c->shm!=NULL);add_assoc_bool(rv,"l2_available",c->l2==L2_CALLBACK?c->backend_active:(c->l2==L2_LMDB?c->env!=NULL:true));
    uint64_t l1_entries=0,l1_bytes=0;
    zval entries;if(include_entries)array_init(&entries);
    if(c->shm&&lock_shm(c->shm)){for(uint32_t i=0;i<CACHE_SHM_SLOTS;i++){cache_slot *s=&c->shm->slots[i];if(!s->used)continue;
        l1_entries++;l1_bytes+=s->value_length;
        if(include_entries&&zend_hash_num_elements(Z_ARRVAL(entries))<128){zval item;array_init(&item);
            add_assoc_string(&item,"partition",(char *)partition_names[s->partition]);add_assoc_string(&item,"key",s->key);
            add_assoc_long(&item,"bytes",s->value_length);add_assoc_long(&item,"expires_at_ms",s->expires_at_ms);add_next_index_zval(&entries,&item);}}
        unlock_shm(c->shm);}
    add_assoc_long(rv,"l1_entries",l1_entries);add_assoc_long(rv,"l1_bytes",l1_bytes);
    if(c->env){MDB_txn *txn=NULL;MDB_stat stat;if(mdb_txn_begin(c->env,NULL,MDB_RDONLY,&txn)==MDB_SUCCESS&&mdb_stat(txn,c->dbi,&stat)==MDB_SUCCESS)add_assoc_long(rv,"l2_entries",stat.ms_entries);else add_assoc_null(rv,"l2_entries");if(txn)mdb_txn_abort(txn);}
    else add_assoc_null(rv,"l2_entries");add_assoc_null(rv,"l2_bytes");
    if(include_entries)add_assoc_zval(rv,"entries",&entries);
    if(reset) memset(&c->stats,0,sizeof(c->stats));
}

bool php_bacnet_cache_set_backend(php_bacnet_cache *c, zval *backend)
{
    if(c->backend_active)zval_ptr_dtor(&c->backend);ZVAL_COPY(&c->backend,backend);
    c->backend_active=true;c->l2=L2_CALLBACK;php_bacnet_cache_clear(c,-1);return true;
}
