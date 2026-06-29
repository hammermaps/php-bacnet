#ifndef PHP_BACNET_H
#define PHP_BACNET_H

extern zend_module_entry bacnet_module_entry;
#define phpext_bacnet_ptr &bacnet_module_entry

#define PHP_BACNET_VERSION "0.1.0"
#define PHP_BACNET_EXTNAME "bacnet"

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
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
    uint8_t next_invoke_id;
    /*
     * Singleton guard: bacnet-stack's bip_init() binds a process-global UDP
     * socket. Only one Bacnet\Client may exist per PHP process at a time.
     * Reset to 0 in RINIT so FPM workers can create a new Client per request.
     */
    zend_bool client_initialized;
} zend_bacnet_globals;

ZEND_EXTERN_MODULE_GLOBALS(bacnet)
#define BACNET_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(bacnet, v)

#endif /* PHP_BACNET_H */
