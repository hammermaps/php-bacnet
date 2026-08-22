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
 * Windows NTS L1 cache. Entries reside in a named file mapping and are
 * protected by a named mutex, so independent FastCGI processes share them.
 * A persistent L2 needs a separately packaged database library and is kept
 * disabled until such a library is provided by the build.
 */
#define PHP_BACNET_WIN_CACHE_ENTRIES 256u
#define PHP_BACNET_WIN_CACHE_KEY_MAX 192u
#define PHP_BACNET_WIN_CACHE_VALUE_MAX 4096u
#define PHP_BACNET_WIN_CACHE_MAGIC 0x42414357u

typedef struct {
	bool used;
	php_bacnet_cache_partition partition;
	uint32_t length;
	uint64_t expires_at_ms;
	uint64_t touched;
	char key[PHP_BACNET_WIN_CACHE_KEY_MAX];
	uint8_t value[PHP_BACNET_WIN_CACHE_VALUE_MAX];
} php_bacnet_win_cache_entry;

typedef struct {
	uint32_t magic;
	uint64_t tick;
	php_bacnet_win_cache_entry entries[PHP_BACNET_WIN_CACHE_ENTRIES];
} php_bacnet_win_cache_shared;

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
	HANDLE mapping;
	HANDLE mutex;
	php_bacnet_win_cache_shared *shared;
};

static uint64_t php_bacnet_win_hash_string(const char *value) {
	uint64_t hash = 1469598103934665603ULL;
	while (*value) {
		hash ^= (unsigned char)*value++;
		hash *= 1099511628211ULL;
	}
	return hash;
}

static bool php_bacnet_win_lock(php_bacnet_cache *cache) {
	DWORD result = WaitForSingleObject(cache->mutex, 5000);
	return result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
}

static void php_bacnet_win_unlock(php_bacnet_cache *cache) {
	ReleaseMutex(cache->mutex);
}

static bool php_bacnet_win_open_shared(php_bacnet_cache *cache) {
	char mutex_name[160];
	uint64_t hash = php_bacnet_win_hash_string(cache->namespace_name);
	snprintf(cache->shm_name, sizeof(cache->shm_name), "Local\\php_bacnet_%016llx",
			 (unsigned long long)hash);
	snprintf(mutex_name, sizeof(mutex_name), "%s_mutex", cache->shm_name);
	cache->mutex = CreateMutexA(NULL, FALSE, mutex_name);
	cache->mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
										sizeof(*cache->shared), cache->shm_name);
	if (!cache->mutex || !cache->mapping)
		return false;
	cache->shared =
		MapViewOfFile(cache->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*cache->shared));
	if (!cache->shared)
		return false;
	if (php_bacnet_win_lock(cache)) {
		if (cache->shared->magic != PHP_BACNET_WIN_CACHE_MAGIC) {
			memset(cache->shared, 0, sizeof(*cache->shared));
			cache->shared->magic = PHP_BACNET_WIN_CACHE_MAGIC;
		}
		php_bacnet_win_unlock(cache);
	}
	return true;
}

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
	snprintf(cache->lmdb_path, sizeof(cache->lmdb_path), "unsupported-on-windows");
	if (cache->enabled && !php_bacnet_win_open_shared(cache))
		cache->enabled = false;
	return cache;
}

void php_bacnet_cache_destroy(php_bacnet_cache *cache) {
	if (cache) {
		if (cache->shared)
			UnmapViewOfFile(cache->shared);
		if (cache->mapping)
			CloseHandle(cache->mapping);
		if (cache->mutex)
			CloseHandle(cache->mutex);
		pefree(cache, 1);
	}
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
	if (!cache->shared || !php_bacnet_win_lock(cache))
		return false;
	for (uint32_t i = 0; i < PHP_BACNET_WIN_CACHE_ENTRIES; i++) {
		php_bacnet_win_cache_entry *entry = &cache->shared->entries[i];
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
		php_bacnet_win_unlock(cache);
		return true;
	}
	php_bacnet_win_unlock(cache);
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
	if (!cache->shared || !php_bacnet_win_lock(cache))
		return;
	php_bacnet_win_cache_entry *target = NULL;
	for (uint32_t i = 0; i < PHP_BACNET_WIN_CACHE_ENTRIES; i++) {
		if (cache->shared->entries[i].used && cache->shared->entries[i].partition == partition &&
			!strcmp(cache->shared->entries[i].key, key)) {
			target = &cache->shared->entries[i];
			break;
		}
		if (!target && !cache->shared->entries[i].used)
			target = &cache->shared->entries[i];
	}
	if (!target) {
		target = &cache->shared->entries[0];
		for (uint32_t i = 1; i < PHP_BACNET_WIN_CACHE_ENTRIES; i++)
			if (cache->shared->entries[i].touched < target->touched)
				target = &cache->shared->entries[i];
		cache->evictions++;
	}
	target->used = true;
	target->partition = partition;
	target->length = length;
	target->expires_at_ms = php_bacnet_platform_wall_ms() + (uint64_t)(ttl_seconds * 1000.0);
	target->touched = ++cache->shared->tick;
	memcpy(target->key, key, strlen(key) + 1);
	memcpy(target->value, data, length);
	cache->stores++;
	php_bacnet_win_unlock(cache);
}

void php_bacnet_cache_clear(php_bacnet_cache *cache, int partition) {
	if (!cache)
		return;
	if (cache->shared && php_bacnet_win_lock(cache)) {
		for (uint32_t i = 0; i < PHP_BACNET_WIN_CACHE_ENTRIES; i++)
			if (cache->shared->entries[i].used &&
				(partition < 0 ||
				 cache->shared->entries[i].partition == (php_bacnet_cache_partition)partition))
				cache->shared->entries[i].used = false;
		php_bacnet_win_unlock(cache);
	}
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
	if (!cache) {
		*error =
			zend_string_init("Cache nicht initialisiert", strlen("Cache nicht initialisiert"), 0);
		return false;
	}
	zend_string *key;
	zval *value;
	ZEND_HASH_FOREACH_STR_KEY_VAL(options, key, value) {
		if (!key) {
			*error =
				zend_string_init("Cache-Optionen müssen String-Schlüssel verwenden",
								 strlen("Cache-Optionen müssen String-Schlüssel verwenden"), 0);
			return false;
		}
		const char *name = ZSTR_VAL(key);
		if (!strcmp(name, "enabled") && (Z_TYPE_P(value) == IS_TRUE || Z_TYPE_P(value) == IS_FALSE))
			cache->enabled = zend_is_true(value);
		else {
			bool matched = false;
			for (int partition = 0; partition < PHP_BACNET_CACHE_PARTITION_COUNT; partition++) {
				char option[64];
				snprintf(option, sizeof(option), "%s_enabled",
						 php_bacnet_win_partition_name(partition));
				if (!strcmp(name, option) &&
					(Z_TYPE_P(value) == IS_TRUE || Z_TYPE_P(value) == IS_FALSE)) {
					cache->part_enabled[partition] = zend_is_true(value);
					matched = true;
					break;
				}
				snprintf(option, sizeof(option), "%s_ttl",
						 php_bacnet_win_partition_name(partition));
				if (!strcmp(name, option) &&
					(Z_TYPE_P(value) == IS_LONG || Z_TYPE_P(value) == IS_DOUBLE) &&
					zval_get_double(value) >= 0) {
					cache->ttl[partition] = zval_get_double(value);
					matched = true;
					break;
				}
			}
			if (!matched) {
				*error = zend_strpprintf(0, "Unbekannte oder ungültige Cache-Option: %s", name);
				return false;
			}
		}
	}
	ZEND_HASH_FOREACH_END();
	if (cache->enabled && !cache->shared && !php_bacnet_win_open_shared(cache)) {
		*error = zend_string_init("Windows Shared Memory konnte nicht geöffnet werden",
								  strlen("Windows Shared Memory konnte nicht geöffnet werden"), 0);
		return false;
	}
	php_bacnet_cache_clear(cache, -1);
	return true;
}

void php_bacnet_cache_get_options(php_bacnet_cache *cache, zval *return_value) {
	array_init(return_value);
	add_assoc_bool(return_value, "enabled", cache && cache->enabled);
	add_assoc_string(return_value, "l1_backend", "shared_memory");
	add_assoc_string(return_value, "l2_backend", "none");
	add_assoc_string(return_value, "namespace", cache ? cache->namespace_name : "");
	add_assoc_string(return_value, "shm_name", cache ? cache->shm_name : "");
	add_assoc_string(return_value, "lmdb_path", cache ? cache->lmdb_path : "");
	if (!cache)
		return;
	for (int partition = 0; partition < PHP_BACNET_CACHE_PARTITION_COUNT; partition++) {
		char key[64];
		snprintf(key, sizeof(key), "%s_enabled", php_bacnet_win_partition_name(partition));
		add_assoc_bool(return_value, key, cache->part_enabled[partition]);
		snprintf(key, sizeof(key), "%s_ttl", php_bacnet_win_partition_name(partition));
		add_assoc_double(return_value, key, cache->ttl[partition]);
	}
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
	add_assoc_bool(return_value, "l1_available", cache->shared != NULL);
	add_assoc_bool(return_value, "l2_available", true);
	if (include_entries) {
		zend_long entries = 0;
		if (cache->shared && php_bacnet_win_lock(cache)) {
			for (uint32_t i = 0; i < PHP_BACNET_WIN_CACHE_ENTRIES; i++)
				entries += cache->shared->entries[i].used;
			php_bacnet_win_unlock(cache);
		}
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
