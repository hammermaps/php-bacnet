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
