#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_bacnet.h"
#include "src/bacnet_classes.h"

ZEND_DECLARE_MODULE_GLOBALS(bacnet)

/* INI entries */
PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("bacnet.default_port",       "47808", PHP_INI_ALL,
        OnUpdateLong, default_port, zend_bacnet_globals, bacnet_globals)
    STD_PHP_INI_ENTRY("bacnet.default_timeout_ms", "3000",  PHP_INI_ALL,
        OnUpdateLong, default_timeout_ms, zend_bacnet_globals, bacnet_globals)
    STD_PHP_INI_ENTRY("bacnet.default_interface",  "0.0.0.0", PHP_INI_ALL,
        OnUpdateString, default_interface, zend_bacnet_globals, bacnet_globals)
PHP_INI_END()

static void php_bacnet_init_globals(zend_bacnet_globals *bacnet_globals)
{
    bacnet_globals->default_port       = 47808;
    bacnet_globals->default_timeout_ms = 3000;
    bacnet_globals->default_interface  = NULL;
    bacnet_globals->next_invoke_id     = 1;
}

/* MINIT */
PHP_MINIT_FUNCTION(bacnet)
{
    ZEND_INIT_MODULE_GLOBALS(bacnet, php_bacnet_init_globals, NULL);
    REGISTER_INI_ENTRIES();
    php_bacnet_register_classes();
    return SUCCESS;
}

/* MSHUTDOWN */
PHP_MSHUTDOWN_FUNCTION(bacnet)
{
    UNREGISTER_INI_ENTRIES();
    return SUCCESS;
}

/* RINIT */
PHP_RINIT_FUNCTION(bacnet)
{
#if defined(COMPILE_DL_BACNET) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    BACNET_G(next_invoke_id) = 1;
    return SUCCESS;
}

/* RSHUTDOWN */
PHP_RSHUTDOWN_FUNCTION(bacnet)
{
    return SUCCESS;
}

/* MINFO */
PHP_MINFO_FUNCTION(bacnet)
{
    php_info_print_table_start();
    php_info_print_table_header(2, "BACnet/IP support", "enabled");
    php_info_print_table_row(2, "Extension version", PHP_BACNET_VERSION);
    php_info_print_table_row(2, "bacnet-stack", "1.5.0 (5afc5c9a)");
    php_info_print_table_end();

    DISPLAY_INI_ENTRIES();
}

static const zend_function_entry bacnet_functions[] = {
    PHP_FE_END
};

zend_module_entry bacnet_module_entry = {
    STANDARD_MODULE_HEADER,
    PHP_BACNET_EXTNAME,
    bacnet_functions,
    PHP_MINIT(bacnet),
    PHP_MSHUTDOWN(bacnet),
    PHP_RINIT(bacnet),
    PHP_RSHUTDOWN(bacnet),
    PHP_MINFO(bacnet),
    PHP_BACNET_VERSION,
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_BACNET
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(bacnet)
#endif
