# Step 2: Extension-Skeleton & Lifecycle

**Ziel:** Korrekte `zend_module_entry`, Lifecycle-Hooks, INI-Einstellungen und MINFO für PHP 8.4+ (NTS) implementieren.

## Aktionen

1.  **`php_bacnet.h`:**
    ```c
    #ifndef PHP_BACNET_H
    #define PHP_BACNET_H

    extern zend_module_entry bacnet_module_entry;
    #define phpext_bacnet_ptr &bacnet_module_entry

    #define PHP_BACNET_VERSION "0.1.0"

    #include "php.h"
    #include "php_ini.h"
    #include "ext/standard/info.h"

    /* Externe Zeiger für Zend-Klassen (werden in MINIT gesetzt) */
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
    extern zend_class_entry *bacnet_ce_exception;
    extern zend_class_entry *bacnet_ce_timeout_exception;
    extern zend_class_entry *bacnet_ce_device_exception;

    #endif
    ```

2.  **INI-Einstellungen in `bacnet.c`:**
    ```c
    PHP_INI_BEGIN()
      STD_PHP_INI_ENTRY("bacnet.default_port", "47808", PHP_INI_ALL, OnUpdateLong, default_port, zend_bacnet_globals, bacnet_globals)
      STD_PHP_INI_ENTRY("bacnet.default_timeout_ms", "3000", PHP_INI_ALL, OnUpdateLong, default_timeout_ms, zend_bacnet_globals, bacnet_globals)
      STD_PHP_INI_ENTRY("bacnet.default_interface", "0.0.0.0", PHP_INI_ALL, OnUpdateString, default_interface, zend_bacnet_globals, bacnet_globals)
    PHP_INI_END()
    ```

3.  **Globale Variablen (NTS):**
    ```c
    typedef struct _zend_bacnet_globals {
        zend_long default_port;
        zend_long default_timeout_ms;
        char *default_interface;
        uint8_t next_invoke_id;
    } zend_bacnet_globals;

    ZEND_DECLARE_MODULE_GLOBALS(bacnet)
    ```

4.  **Lifecycle-Handler:**
    - `PHP_MINIT_FUNCTION(bacnet)`: Registriere INI, dann rufe `php_bacnet_register_classes()` aus `bacnet_classes.c` auf (noch leerer Stub).
    - `PHP_MSHUTDOWN_FUNCTION(bacnet)`: UNREGISTER_INI_ENTRIES.
    - `PHP_RINIT_FUNCTION(bacnet)`: `BACNET_G(next_invoke_id) = 1;`
    - `PHP_RSHUTDOWN_FUNCTION(bacnet)`: (leer)
    - `PHP_MINFO_FUNCTION(bacnet)`: Gib Versionsnummer, Build-Info, INI-Werte aus (`php_info_print_table_start` etc.).

5.  **`config.m4` / `config.w32`:**  
    Stelle sicher, dass keine ZTS-Prüfung fehlschlägt. Wir setzen explizit NTS voraus.

## Akzeptanzkriterien

- [ ] `php -d extension=modules/bacnet.so -r "phpinfo();"` zeigt den Block *bacnet* mit INI-Werten.
- [ ] `php -d extension=modules/bacnet.so -r "echo INI_GET('bacnet.default_timeout_ms');"` gibt `3000` aus.
- [ ] Keine Compiler-Warnungen bezüglich fehlender Module-Entry.
