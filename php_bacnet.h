#ifndef PHP_BACNET_H
#define PHP_BACNET_H

extern zend_module_entry bacnet_module_entry;
#define phpext_bacnet_ptr &bacnet_module_entry

#define PHP_BACNET_VERSION "0.1.0"
#define PHP_BACNET_EXTNAME "bacnet"

#include "php.h"
#include "zend_exceptions.h"

/* Class entry pointers — set in MINIT via php_bacnet_register_classes() */
extern zend_class_entry *bacnet_ce_client;
extern zend_class_entry *bacnet_ce_device;
extern zend_class_entry *bacnet_ce_object_ref;
extern zend_class_entry *bacnet_ce_object_identifier;
extern zend_class_entry *bacnet_ce_bit_string;
extern zend_class_entry *bacnet_ce_date;
extern zend_class_entry *bacnet_ce_time;
extern zend_class_entry *bacnet_ce_value;
extern zend_class_entry *bacnet_ce_object_type_enum;
extern zend_class_entry *bacnet_ce_property_enum;
extern zend_class_entry *bacnet_ce_server;
extern zend_class_entry *bacnet_ce_mixed;
extern zend_class_entry *bacnet_ce_schedule_entry;
extern zend_class_entry *bacnet_ce_weekly_schedule;
extern zend_class_entry *bacnet_ce_trend_log_record;
extern zend_class_entry *bacnet_ce_exception;
extern zend_class_entry *bacnet_ce_timeout_exception;
extern zend_class_entry *bacnet_ce_device_exception;
extern zend_class_entry *bacnet_ce_cache_backend;

/* Extension globals (NTS only — no ZTS) */
typedef struct _zend_bacnet_globals {
    zend_long default_port;
    zend_long default_timeout_ms;
    char *default_interface;
    bool server_security_enabled;
    double server_per_source_rate;
    zend_long server_per_source_burst;
    double server_global_rate;
    zend_long server_global_burst;
    double server_who_is_rate;
    zend_long server_who_is_burst;
    double server_write_rate;
    zend_long server_write_burst;
    zend_long server_flood_violations;
    double server_flood_window;
    double server_block_duration;
    double server_duplicate_window;
    char *server_allowed_networks;
    char *server_denied_networks;
    zend_long server_max_sources;
    double server_source_ttl;
    double server_log_interval;
    bool cache_enabled;
    char *cache_l2_backend;
    char *cache_namespace;
    char *cache_lmdb_path;
    char *cache_shm_name;
    zend_long cache_l1_max_bytes;
    zend_long cache_l2_max_bytes;
    zend_long cache_lmdb_map_size;
    zend_long cache_coherence_interval_ms;
    double cache_log_interval;
    bool cache_state_enabled;
    double cache_state_ttl;
    zend_long cache_state_max_entries;
    bool cache_object_enabled;
    double cache_object_ttl;
    zend_long cache_object_max_entries;
    bool cache_object_list_enabled;
    double cache_object_list_ttl;
    zend_long cache_object_list_max_entries;
    bool cache_device_enabled;
    double cache_device_ttl;
    zend_long cache_device_max_entries;
    bool cache_ip_enabled;
    double cache_ip_ttl;
    zend_long cache_ip_max_entries;
    bool cache_negative_enabled;
    double cache_negative_whois_ttl;
    double cache_negative_read_ttl;
    zend_long cache_negative_max_entries;
    uint8_t next_invoke_id;
    /*
     * Singleton guard: bacnet-stack's bip_init() binds a process-global UDP
     * socket. Only one Bacnet\Client may exist per PHP process at a time.
     * Reset to 0 in RINIT so FPM workers can create a new Client per request.
     */
    bool client_initialized;
} zend_bacnet_globals;

ZEND_EXTERN_MODULE_GLOBALS(bacnet)
#define BACNET_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(bacnet, v)

#endif /* PHP_BACNET_H */
