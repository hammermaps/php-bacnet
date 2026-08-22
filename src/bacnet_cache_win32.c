#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "zend_exceptions.h"

#include <stdio.h>
#include <string.h>

#include "../php_bacnet.h"
#include "bacnet_cache.h"
#include "bacnet_platform.h"

/*
 * Windows NTS baseline: a bounded, process-local L1 cache.  The Unix build
 * keeps its POSIX shared-memory and LMDB implementation in bacnet_cache.c.
 * This avoids shipping a third-party LMDB DLL and retains the cache API for
 * Windows users.  Cross-process cache coherence is intentionally unavailable.
 */
#define PHP_BACNET_WIN_CACHE_ENTRIES 256u
#define PHP_BACNET_WIN_CACHE_KEY_MAX 192u
#define PHP_BACNET_WIN_CACHE_VALUE_MAX 4096u

typedef struct {
	bool used;
	php_bacnet_cache_partition partition;
	uint32_t length;
	uint64_t expires_at_ms;
	uint64_t touched;
	char key[PHP_BACNET_WIN_CACHE_KEY_MAX];
	uint8_t value[PHP_BACNET_WIN_CACHE_VALUE_MAX];
} php_bacnet_win_cache_entry;

struct php_bacnet_cache {
	bool enabled;
	bool part_enabled[PHP_BACNET_CACHE_PARTITION_COUNT];
	double ttl[PHP_BACNET_CACHE_PARTITION_COUNT];
	uint32_t max_entries[PHP_BACNET_CACHE_PARTITION_COUNT];
	double negative_whois_ttl;
	char namespace_name[96];
	char shm_name[128];
	char lmdb_path[512];
	uint64_t tick, hits, misses, stores, expirations, evictions, invalidations, refreshes;
	php_bacnet_win_cache_entry entries[PHP_BACNET_WIN_CACHE_ENTRIES];
};

static const char *php_bacnet_win_partition_name(php_bacnet_cache_partition partition) {
	static const char *const names[] = {"state",  "object", "object_list",
										"device", "ip",		"negative"};
	return partition < PHP_BACNET_CACHE_PARTITION_COUNT ? names[partition] : "unknown";
}

php_bacnet_cache *php_bacnet_cache_create(const char *iface, uint16_t port) {
	php_bacnet_cache *cache = pemalloc(sizeof(*cache), 1);
	memset(cache, 0, sizeof(*cache));
	cache->enabled = BACNET_G(cache_enabled);
	cache->part_enabled[PHP_BACNET_CACHE_STATE] = BACNET_G(cache_state_enabled);
	cache->part_enabled[PHP_BACNET_CACHE_OBJECT] = BACNET_G(cache_object_enabled);
	cache->part_enabled[PHP_BACNET_CACHE_OBJECT_LIST] = BACNET_G(cache_object_list_enabled);
	cache->part_enabled[PHP_BACNET_CACHE_DEVICE] = BACNET_G(cache_device_enabled);
	cache->part_enabled[PHP_BACNET_CACHE_IP] = BACNET_G(cache_ip_enabled);
	cache->part_enabled[PHP_BACNET_CACHE_NEGATIVE] = BACNET_G(cache_negative_enabled);
	cache->ttl[PHP_BACNET_CACHE_STATE] = BACNET_G(cache_state_ttl);
	cache->ttl[PHP_BACNET_CACHE_OBJECT] = BACNET_G(cache_object_ttl);
	cache->ttl[PHP_BACNET_CACHE_OBJECT_LIST] = BACNET_G(cache_object_list_ttl);
	cache->ttl[PHP_BACNET_CACHE_DEVICE] = BACNET_G(cache_device_ttl);
	cache->ttl[PHP_BACNET_CACHE_IP] = BACNET_G(cache_ip_ttl);
	cache->ttl[PHP_BACNET_CACHE_NEGATIVE] = BACNET_G(cache_negative_read_ttl);
	cache->negative_whois_ttl = BACNET_G(cache_negative_whois_ttl);
	for (uint32_t i = 0; i < PHP_BACNET_CACHE_PARTITION_COUNT; i++)
		cache->max_entries[i] = PHP_BACNET_WIN_CACHE_ENTRIES;
	snprintf(cache->namespace_name, sizeof(cache->namespace_name), "%s:%u", iface ? iface : "auto",
			 port);
	snprintf(cache->shm_name, sizeof(cache->shm_name), "windows-process-local");
	snprintf(cache->lmdb_path, sizeof(cache->lmdb_path), "unsupported-on-windows");
	return cache;
}

void php_bacnet_cache_destroy(php_bacnet_cache *cache) {
	if (cache)
		pefree(cache, 1);
}

bool php_bacnet_cache_partition_enabled(php_bacnet_cache *cache,
										php_bacnet_cache_partition partition) {
	return cache && cache->enabled && partition < PHP_BACNET_CACHE_PARTITION_COUNT &&
		   cache->part_enabled[partition];
}

double php_bacnet_cache_partition_ttl(php_bacnet_cache *cache,
									  php_bacnet_cache_partition partition) {
	return cache && partition < PHP_BACNET_CACHE_PARTITION_COUNT ? cache->ttl[partition] : 0;
}

double php_bacnet_cache_negative_whois_ttl(php_bacnet_cache *cache) {
	return cache ? cache->negative_whois_ttl : 0;
}

bool php_bacnet_cache_get(php_bacnet_cache *cache, php_bacnet_cache_partition partition,
						  const char *key, uint8_t *data, uint32_t *length) {
	if (!php_bacnet_cache_partition_enabled(cache, partition) || !key || !data || !length)
		return false;
	uint64_t now = php_bacnet_platform_wall_ms();
	for (uint32_t i = 0; i < PHP_BACNET_WIN_CACHE_ENTRIES; i++) {
		php_bacnet_win_cache_entry *entry = &cache->entries[i];
		if (!entry->used || entry->partition != partition || strcmp(entry->key, key))
			continue;
		if (entry->expires_at_ms <= now) {
			entry->used = false;
			cache->expirations++;
			break;
		}
		if (*length < entry->length)
			break;
		memcpy(data, entry->value, entry->length);
		*length = entry->length;
		entry->touched = ++cache->tick;
		cache->hits++;
		return true;
	}
	cache->misses++;
	return false;
}

void php_bacnet_cache_put(php_bacnet_cache *cache, php_bacnet_cache_partition partition,
						  const char *key, const uint8_t *data, uint32_t length,
						  double ttl_seconds) {
	if (!php_bacnet_cache_partition_enabled(cache, partition) || !key || !data || !length ||
		length > PHP_BACNET_WIN_CACHE_VALUE_MAX || strlen(key) >= PHP_BACNET_WIN_CACHE_KEY_MAX ||
		ttl_seconds <= 0)
		return;
	php_bacnet_win_cache_entry *target = NULL;
	for (uint32_t i = 0; i < PHP_BACNET_WIN_CACHE_ENTRIES; i++) {
		if (cache->entries[i].used && cache->entries[i].partition == partition &&
			!strcmp(cache->entries[i].key, key)) {
			target = &cache->entries[i];
			break;
		}
		if (!target && !cache->entries[i].used)
			target = &cache->entries[i];
	}
	if (!target) {
		target = &cache->entries[0];
		for (uint32_t i = 1; i < PHP_BACNET_WIN_CACHE_ENTRIES; i++)
			if (cache->entries[i].touched < target->touched)
				target = &cache->entries[i];
		cache->evictions++;
	}
	target->used = true;
	target->partition = partition;
	target->length = length;
	target->expires_at_ms = php_bacnet_platform_wall_ms() + (uint64_t)(ttl_seconds * 1000.0);
	target->touched = ++cache->tick;
	memcpy(target->key, key, strlen(key) + 1);
	memcpy(target->value, data, length);
	cache->stores++;
}

void php_bacnet_cache_clear(php_bacnet_cache *cache, int partition) {
	if (!cache)
		return;
	for (uint32_t i = 0; i < PHP_BACNET_WIN_CACHE_ENTRIES; i++)
		if (cache->entries[i].used &&
			(partition < 0 || cache->entries[i].partition == (php_bacnet_cache_partition)partition))
			cache->entries[i].used = false;
	cache->invalidations++;
}

void php_bacnet_cache_invalidate(php_bacnet_cache *cache, php_bacnet_cache_partition partition,
								 const char *scope) {
	(void)scope;
	php_bacnet_cache_clear(cache, (int)partition);
}

void php_bacnet_cache_refresh(php_bacnet_cache *cache, php_bacnet_cache_partition partition,
							  const char *scope) {
	(void)partition;
	(void)scope;
	if (cache)
		cache->refreshes++;
}

bool php_bacnet_cache_set_options(php_bacnet_cache *cache, HashTable *options,
								  zend_string **error) {
	(void)options;
	if (!cache) {
		*error =
			zend_string_init("Cache nicht initialisiert", strlen("Cache nicht initialisiert"), 0);
		return false;
	}
	return true;
}

void php_bacnet_cache_get_options(php_bacnet_cache *cache, zval *return_value) {
	array_init(return_value);
	add_assoc_bool(return_value, "enabled", cache && cache->enabled);
	add_assoc_string(return_value, "l1_backend", "process_memory");
	add_assoc_string(return_value, "l2_backend", "none");
	add_assoc_string(return_value, "namespace", cache ? cache->namespace_name : "");
	add_assoc_string(return_value, "shm_name", cache ? cache->shm_name : "");
	add_assoc_string(return_value, "lmdb_path", cache ? cache->lmdb_path : "");
}

void php_bacnet_cache_get_stats(php_bacnet_cache *cache, bool include_entries, bool reset,
								zval *return_value) {
	array_init(return_value);
	if (!cache)
		return;
	add_assoc_long(return_value, "hits", (zend_long)cache->hits);
	add_assoc_long(return_value, "misses", (zend_long)cache->misses);
	add_assoc_long(return_value, "stores", (zend_long)cache->stores);
	add_assoc_long(return_value, "expirations", (zend_long)cache->expirations);
	add_assoc_long(return_value, "evictions", (zend_long)cache->evictions);
	add_assoc_long(return_value, "invalidations", (zend_long)cache->invalidations);
	add_assoc_long(return_value, "refreshes", (zend_long)cache->refreshes);
	if (include_entries) {
		zend_long entries = 0;
		for (uint32_t i = 0; i < PHP_BACNET_WIN_CACHE_ENTRIES; i++)
			entries += cache->entries[i].used;
		add_assoc_long(return_value, "entries", entries);
	}
	if (reset)
		cache->hits = cache->misses = cache->stores = cache->expirations = cache->evictions =
			cache->invalidations = cache->refreshes = 0;
}

bool php_bacnet_cache_set_backend(php_bacnet_cache *cache, zval *backend) {
	(void)cache;
	(void)backend;
	zend_throw_exception(
		NULL, "Benutzerdefinierte Cache-Backends werden unter Windows noch nicht unterstützt", 0);
	return false;
}
