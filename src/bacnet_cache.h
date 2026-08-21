#ifndef PHP_BACNET_CACHE_H
#define PHP_BACNET_CACHE_H

#include "php.h"
#include <stdint.h>
#include <stdbool.h>

typedef enum {
	PHP_BACNET_CACHE_STATE = 0,
	PHP_BACNET_CACHE_OBJECT,
	PHP_BACNET_CACHE_OBJECT_LIST,
	PHP_BACNET_CACHE_DEVICE,
	PHP_BACNET_CACHE_IP,
	PHP_BACNET_CACHE_NEGATIVE,
	PHP_BACNET_CACHE_PARTITION_COUNT
} php_bacnet_cache_partition;

typedef struct php_bacnet_cache php_bacnet_cache;

php_bacnet_cache *php_bacnet_cache_create(const char *iface, uint16_t port);
void php_bacnet_cache_destroy(php_bacnet_cache *cache);
bool php_bacnet_cache_get(php_bacnet_cache *cache, php_bacnet_cache_partition partition,
						  const char *key, uint8_t *data, uint32_t *length);
void php_bacnet_cache_put(php_bacnet_cache *cache, php_bacnet_cache_partition partition,
						  const char *key, const uint8_t *data, uint32_t length,
						  double ttl_seconds);
void php_bacnet_cache_invalidate(php_bacnet_cache *cache, php_bacnet_cache_partition partition,
								 const char *scope);
void php_bacnet_cache_refresh(php_bacnet_cache *cache, php_bacnet_cache_partition partition,
							  const char *scope);
void php_bacnet_cache_clear(php_bacnet_cache *cache, int partition);
bool php_bacnet_cache_partition_enabled(php_bacnet_cache *cache,
										php_bacnet_cache_partition partition);
double php_bacnet_cache_partition_ttl(php_bacnet_cache *cache,
									  php_bacnet_cache_partition partition);
double php_bacnet_cache_negative_whois_ttl(php_bacnet_cache *cache);
bool php_bacnet_cache_set_options(php_bacnet_cache *cache, HashTable *options, zend_string **error);
void php_bacnet_cache_get_options(php_bacnet_cache *cache, zval *return_value);
void php_bacnet_cache_get_stats(php_bacnet_cache *cache, bool include_entries, bool reset,
								zval *return_value);
bool php_bacnet_cache_set_backend(php_bacnet_cache *cache, zval *backend);

#endif
