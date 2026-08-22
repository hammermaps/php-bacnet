#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"		   /* REGISTER_INI_ENTRIES, UNREGISTER_INI_ENTRIES */
#include "ext/standard/info.h" /* php_info_print_table_* */
#include "php_bacnet.h"
#include "src/bacnet_classes.h"
#include "src/bacnet_transport.h"

ZEND_DECLARE_MODULE_GLOBALS(bacnet)

/* INI entries */
PHP_INI_BEGIN()
STD_PHP_INI_ENTRY("bacnet.default_port", "47808", PHP_INI_ALL, OnUpdateLong, default_port,
				  zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.default_timeout_ms", "3000", PHP_INI_ALL, OnUpdateLong,
				  default_timeout_ms, zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.default_interface", "0.0.0.0", PHP_INI_ALL, OnUpdateString,
				  default_interface, zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_BOOLEAN("bacnet.server_security_enabled", "1", PHP_INI_ALL, OnUpdateBool,
					server_security_enabled, zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.server_per_source_rate", "50", PHP_INI_ALL, OnUpdateReal,
				  server_per_source_rate, zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.server_per_source_burst", "100", PHP_INI_ALL, OnUpdateLong,
				  server_per_source_burst, zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.server_global_rate", "500", PHP_INI_ALL, OnUpdateReal, server_global_rate,
				  zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.server_global_burst", "1000", PHP_INI_ALL, OnUpdateLong,
				  server_global_burst, zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.server_who_is_rate", "2", PHP_INI_ALL, OnUpdateReal, server_who_is_rate,
				  zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.server_who_is_burst", "5", PHP_INI_ALL, OnUpdateLong, server_who_is_burst,
				  zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.server_write_rate", "5", PHP_INI_ALL, OnUpdateReal, server_write_rate,
				  zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.server_write_burst", "10", PHP_INI_ALL, OnUpdateLong, server_write_burst,
				  zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.server_flood_violations", "20", PHP_INI_ALL, OnUpdateLong,
				  server_flood_violations, zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.server_flood_window_seconds", "10", PHP_INI_ALL, OnUpdateReal,
				  server_flood_window, zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.server_block_duration_seconds", "60", PHP_INI_ALL, OnUpdateReal,
				  server_block_duration, zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.server_duplicate_window_seconds", "5", PHP_INI_ALL, OnUpdateReal,
				  server_duplicate_window, zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.server_allowed_networks", "", PHP_INI_ALL, OnUpdateString,
				  server_allowed_networks, zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.server_denied_networks", "", PHP_INI_ALL, OnUpdateString,
				  server_denied_networks, zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.server_max_sources", "1024", PHP_INI_ALL, OnUpdateLong,
				  server_max_sources, zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.server_source_ttl_seconds", "300", PHP_INI_ALL, OnUpdateReal,
				  server_source_ttl, zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.server_log_interval_seconds", "60", PHP_INI_ALL, OnUpdateReal,
				  server_log_interval, zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_BOOLEAN("bacnet.cache_enabled", "1", PHP_INI_ALL, OnUpdateBool, cache_enabled,
					zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.cache_l2_backend", "lmdb", PHP_INI_ALL, OnUpdateString, cache_l2_backend,
				  zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.cache_namespace", "", PHP_INI_ALL, OnUpdateString, cache_namespace,
				  zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.cache_lmdb_path", "/var/cache/php-bacnet", PHP_INI_ALL, OnUpdateString,
				  cache_lmdb_path, zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.cache_shm_name", "", PHP_INI_ALL, OnUpdateString, cache_shm_name,
				  zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.cache_l1_max_bytes", "16777216", PHP_INI_ALL, OnUpdateLong,
				  cache_l1_max_bytes, zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.cache_l2_max_bytes", "16777216", PHP_INI_ALL, OnUpdateLong,
				  cache_l2_max_bytes, zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.cache_lmdb_map_size", "67108864", PHP_INI_ALL, OnUpdateLong,
				  cache_lmdb_map_size, zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.cache_coherence_interval_ms", "1000", PHP_INI_ALL, OnUpdateLong,
				  cache_coherence_interval_ms, zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.cache_log_interval_seconds", "60", PHP_INI_ALL, OnUpdateReal,
				  cache_log_interval, zend_bacnet_globals, bacnet_globals)
#define BACNET_CACHE_PARTITION_INI(name, field, enabled, ttl, max_entries)                         \
	STD_PHP_INI_BOOLEAN("bacnet.cache_" name "_enabled", enabled, PHP_INI_ALL, OnUpdateBool,       \
						cache_##field##_enabled, zend_bacnet_globals, bacnet_globals)              \
	STD_PHP_INI_ENTRY("bacnet.cache_" name "_ttl_seconds", ttl, PHP_INI_ALL, OnUpdateReal,         \
					  cache_##field##_ttl, zend_bacnet_globals, bacnet_globals)                    \
	STD_PHP_INI_ENTRY("bacnet.cache_" name "_max_entries", max_entries, PHP_INI_ALL, OnUpdateLong, \
					  cache_##field##_max_entries, zend_bacnet_globals, bacnet_globals)
BACNET_CACHE_PARTITION_INI("state", state, "0", "1", "4096")
BACNET_CACHE_PARTITION_INI("object", object, "1", "300", "4096")
BACNET_CACHE_PARTITION_INI("object_list", object_list, "1", "300", "256")
BACNET_CACHE_PARTITION_INI("device", device, "1", "60", "256")
BACNET_CACHE_PARTITION_INI("ip", ip, "1", "300", "256")
#undef BACNET_CACHE_PARTITION_INI
STD_PHP_INI_BOOLEAN("bacnet.cache_negative_enabled", "0", PHP_INI_ALL, OnUpdateBool,
					cache_negative_enabled, zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.cache_negative_whois_ttl_seconds", "2", PHP_INI_ALL, OnUpdateReal,
				  cache_negative_whois_ttl, zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.cache_negative_read_ttl_seconds", "1", PHP_INI_ALL, OnUpdateReal,
				  cache_negative_read_ttl, zend_bacnet_globals, bacnet_globals)
STD_PHP_INI_ENTRY("bacnet.cache_negative_max_entries", "1024", PHP_INI_ALL, OnUpdateLong,
				  cache_negative_max_entries, zend_bacnet_globals, bacnet_globals)
PHP_INI_END()

static void php_bacnet_init_globals(zend_bacnet_globals *bacnet_globals) {
	bacnet_globals->default_port = PHP_BACNET_DEFAULT_PORT;
	bacnet_globals->default_timeout_ms = 3000;
	bacnet_globals->default_interface = NULL;
	bacnet_globals->server_security_enabled = true;
	bacnet_globals->server_per_source_rate = 50;
	bacnet_globals->server_per_source_burst = 100;
	bacnet_globals->server_global_rate = 500;
	bacnet_globals->server_global_burst = 1000;
	bacnet_globals->server_who_is_rate = 2;
	bacnet_globals->server_who_is_burst = 5;
	bacnet_globals->server_write_rate = 5;
	bacnet_globals->server_write_burst = 10;
	bacnet_globals->server_flood_violations = 20;
	bacnet_globals->server_flood_window = 10;
	bacnet_globals->server_block_duration = 60;
	bacnet_globals->server_duplicate_window = 5;
	bacnet_globals->server_allowed_networks = NULL;
	bacnet_globals->server_denied_networks = NULL;
	bacnet_globals->server_max_sources = 1024;
	bacnet_globals->server_source_ttl = 300;
	bacnet_globals->server_log_interval = 60;
	bacnet_globals->cache_enabled = true;
	bacnet_globals->cache_l2_backend = NULL;
	bacnet_globals->cache_namespace = NULL;
	bacnet_globals->cache_lmdb_path = NULL;
	bacnet_globals->cache_shm_name = NULL;
	bacnet_globals->cache_l1_max_bytes = 16777216;
	bacnet_globals->cache_l2_max_bytes = 16777216;
	bacnet_globals->cache_lmdb_map_size = 67108864;
	bacnet_globals->cache_coherence_interval_ms = 1000;
	bacnet_globals->cache_log_interval = 60;
	bacnet_globals->cache_state_enabled = false;
	bacnet_globals->cache_state_ttl = 1;
	bacnet_globals->cache_state_max_entries = 4096;
	bacnet_globals->cache_object_enabled = true;
	bacnet_globals->cache_object_ttl = 300;
	bacnet_globals->cache_object_max_entries = 4096;
	bacnet_globals->cache_object_list_enabled = true;
	bacnet_globals->cache_object_list_ttl = 300;
	bacnet_globals->cache_object_list_max_entries = 256;
	bacnet_globals->cache_device_enabled = true;
	bacnet_globals->cache_device_ttl = 60;
	bacnet_globals->cache_device_max_entries = 256;
	bacnet_globals->cache_ip_enabled = true;
	bacnet_globals->cache_ip_ttl = 300;
	bacnet_globals->cache_ip_max_entries = 256;
	bacnet_globals->cache_negative_enabled = false;
	bacnet_globals->cache_negative_whois_ttl = 2;
	bacnet_globals->cache_negative_read_ttl = 1;
	bacnet_globals->cache_negative_max_entries = 1024;
	bacnet_globals->next_invoke_id = 1;
	bacnet_globals->client_initialized = 0;
}

/* MINIT */
PHP_MINIT_FUNCTION(bacnet) {
	ZEND_INIT_MODULE_GLOBALS(bacnet, php_bacnet_init_globals, NULL);
	if (!php_bacnet_transport_startup())
		return FAILURE;
	REGISTER_INI_ENTRIES();
	php_bacnet_register_classes();
	return SUCCESS;
}

/* MSHUTDOWN */
PHP_MSHUTDOWN_FUNCTION(bacnet) {
	php_bacnet_transport_shutdown();
	UNREGISTER_INI_ENTRIES();
	return SUCCESS;
}

/* RINIT */
PHP_RINIT_FUNCTION(bacnet) {
#if defined(COMPILE_DL_BACNET) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	BACNET_G(next_invoke_id) = 1;
	BACNET_G(client_initialized) = 0;
	return SUCCESS;
}

/* RSHUTDOWN */
PHP_RSHUTDOWN_FUNCTION(bacnet) {
	return SUCCESS;
}

/* MINFO */
PHP_MINFO_FUNCTION(bacnet) {
	php_info_print_table_start();
	php_info_print_table_header(2, "BACnet/IP support", "enabled");
	php_info_print_table_row(2, "Extension version", PHP_BACNET_VERSION);
	php_info_print_table_row(2, "bacnet-stack", "1.5.1 (3a74c74a)");
	php_info_print_table_end();

	DISPLAY_INI_ENTRIES();
}

static const zend_function_entry bacnet_functions[] = {PHP_FE_END};

zend_module_entry bacnet_module_entry = {
	STANDARD_MODULE_HEADER, PHP_BACNET_EXTNAME,		   bacnet_functions,	  PHP_MINIT(bacnet),
	PHP_MSHUTDOWN(bacnet),	PHP_RINIT(bacnet),		   PHP_RSHUTDOWN(bacnet), PHP_MINFO(bacnet),
	PHP_BACNET_VERSION,		STANDARD_MODULE_PROPERTIES};

#ifdef COMPILE_DL_BACNET
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(bacnet)
#endif
