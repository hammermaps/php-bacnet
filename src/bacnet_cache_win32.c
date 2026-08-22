#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "zend_exceptions.h"
#include <lmdb.h>

#include <stdio.h>
#include <string.h>

#include "../php_bacnet.h"
#include "bacnet_cache.h"
#include "bacnet_platform.h"

/* Windows NTS L1 entries are shared by FastCGI processes via a named mapping.
 * L2 uses the bundled LMDB source, matching the Unix cache data guarantees. */
#define PHP_BACNET_WIN_CACHE_ENTRIES 256u
#define PHP_BACNET_WIN_CACHE_KEY_MAX 192u
#define PHP_BACNET_WIN_CACHE_VALUE_MAX 4096u
#define PHP_BACNET_WIN_CACHE_MAGIC 0x42414357u
#define PHP_BACNET_WIN_LMDB_MAGIC 0x42414348u
#define PHP_BACNET_WIN_LMDB_VERSION 2u

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

typedef struct {
	uint32_t magic;
	uint32_t version;
	uint32_t length;
	uint64_t expires_at_ms;
	uint64_t checksum;
} php_bacnet_win_lmdb_value;

typedef enum {
	PHP_BACNET_WIN_L2_NONE,
	PHP_BACNET_WIN_L2_LMDB,
	PHP_BACNET_WIN_L2_CALLBACK
} php_bacnet_win_l2;

struct php_bacnet_cache {
	bool enabled;
	bool part_enabled[PHP_BACNET_CACHE_PARTITION_COUNT];
	double ttl[PHP_BACNET_CACHE_PARTITION_COUNT];
	uint32_t max_entries[PHP_BACNET_CACHE_PARTITION_COUNT];
	double negative_whois_ttl;
	char namespace_name[96];
	char shm_name[128];
	char lmdb_path[512];
	size_t l1_max_bytes;
	size_t l2_max_bytes;
	size_t lmdb_map_size;
	php_bacnet_win_l2 l2;
	zval backend;
	bool backend_active;
	bool in_callback;
	uint64_t tick, hits, misses, stores, expirations, evictions, invalidations, refreshes,
		backend_errors;
	HANDLE mapping;
	HANDLE mutex;
	php_bacnet_win_cache_shared *shared;
	MDB_env *env;
	MDB_dbi dbi;
};

static uint64_t php_bacnet_win_hash_bytes(const uint8_t *data, size_t length) {
	uint64_t hash = 1469598103934665603ULL;
	for (size_t i = 0; i < length; i++) {
		hash ^= data[i];
		hash *= 1099511628211ULL;
	}
	return hash;
}

static bool php_bacnet_win_callback_call(php_bacnet_cache *cache, const char *method, uint32_t argc,
										 zval *args, zval *retval) {
	if (!cache->backend_active || cache->in_callback)
		return false;
	zval callable;
	array_init_size(&callable, 2);
	Z_TRY_ADDREF(cache->backend);
	add_next_index_zval(&callable, &cache->backend);
	add_next_index_string(&callable, method);
	cache->in_callback = true;
	int result = call_user_function(EG(function_table), NULL, &callable, retval, argc, args);
	cache->in_callback = false;
	zval_ptr_dtor(&callable);
	if (result == FAILURE || EG(exception)) {
		if (EG(exception))
			zend_clear_exception();
		cache->backend_errors++;
		return false;
	}
	return true;
}

static void php_bacnet_win_lmdb_prefix(php_bacnet_cache *cache,
									   php_bacnet_cache_partition partition, char *output,
									   size_t output_size) {
	snprintf(output, output_size, "%s|%u|", cache->namespace_name, (unsigned)partition);
}

static bool php_bacnet_win_open_lmdb(php_bacnet_cache *cache) {
	if (mdb_env_create(&cache->env) != MDB_SUCCESS)
		return false;
	mdb_env_set_mapsize(cache->env, cache->lmdb_map_size);
	mdb_env_set_maxdbs(cache->env, 1);
	if (mdb_env_open(cache->env, cache->lmdb_path, 0, 0600) != MDB_SUCCESS)
		goto fail;
	MDB_txn *txn = NULL;
	if (mdb_txn_begin(cache->env, NULL, 0, &txn) != MDB_SUCCESS ||
		mdb_dbi_open(txn, "bacnet", MDB_CREATE, &cache->dbi) != MDB_SUCCESS ||
		mdb_txn_commit(txn) != MDB_SUCCESS) {
		if (txn)
			mdb_txn_abort(txn);
		goto fail;
	}
	return true;
fail:
	mdb_env_close(cache->env);
	cache->env = NULL;
	return false;
}

static void php_bacnet_win_close_lmdb(php_bacnet_cache *cache) {
	if (!cache->env)
		return;
	mdb_dbi_close(cache->env, cache->dbi);
	mdb_env_close(cache->env);
	cache->env = NULL;
}

static void php_bacnet_win_close_shared(php_bacnet_cache *cache) {
	if (cache->shared)
		UnmapViewOfFile(cache->shared);
	if (cache->mapping)
		CloseHandle(cache->mapping);
	if (cache->mutex)
		CloseHandle(cache->mutex);
	cache->shared = NULL;
	cache->mapping = NULL;
	cache->mutex = NULL;
}

static void php_bacnet_win_key(php_bacnet_cache *cache, php_bacnet_cache_partition partition,
							   const char *key, char *output, size_t output_size) {
	snprintf(output, output_size, "%s|%u|%s", cache->namespace_name, (unsigned)partition, key);
}

static bool php_bacnet_win_lmdb_get(php_bacnet_cache *cache, const char *key, uint8_t *data,
									uint32_t *length) {
	MDB_txn *txn = NULL;
	MDB_val db_key = {strlen(key), (void *)key}, value;
	if (!cache->env || mdb_txn_begin(cache->env, NULL, MDB_RDONLY, &txn) != MDB_SUCCESS)
		return false;
	bool hit = false;
	if (mdb_get(txn, cache->dbi, &db_key, &value) == MDB_SUCCESS &&
		value.mv_size >= sizeof(php_bacnet_win_lmdb_value)) {
		php_bacnet_win_lmdb_value header;
		memcpy(&header, value.mv_data, sizeof(header));
		const uint8_t *payload = (const uint8_t *)value.mv_data + sizeof(header);
		if (header.magic == PHP_BACNET_WIN_LMDB_MAGIC &&
			header.version == PHP_BACNET_WIN_LMDB_VERSION &&
			header.expires_at_ms > php_bacnet_platform_wall_ms() &&
			header.length == value.mv_size - sizeof(header) && header.length <= *length &&
			header.checksum == php_bacnet_win_hash_bytes(payload, header.length)) {
			memcpy(data, payload, header.length);
			*length = header.length;
			hit = true;
		}
	}
	mdb_txn_abort(txn);
	return hit;
}

static void php_bacnet_win_lmdb_put(php_bacnet_cache *cache, const char *key, const uint8_t *data,
									uint32_t length, uint64_t expiry) {
	if (!cache->env || length > PHP_BACNET_WIN_CACHE_VALUE_MAX)
		return;
	uint8_t buffer[sizeof(php_bacnet_win_lmdb_value) + PHP_BACNET_WIN_CACHE_VALUE_MAX];
	php_bacnet_win_lmdb_value header = {PHP_BACNET_WIN_LMDB_MAGIC, PHP_BACNET_WIN_LMDB_VERSION,
										length, expiry, php_bacnet_win_hash_bytes(data, length)};
	memcpy(buffer, &header, sizeof(header));
	memcpy(buffer + sizeof(header), data, length);
	MDB_txn *txn = NULL;
	MDB_val db_key = {strlen(key), (void *)key};
	MDB_val value = {sizeof(header) + length, buffer};
	if (mdb_txn_begin(cache->env, NULL, 0, &txn) != MDB_SUCCESS ||
		mdb_put(txn, cache->dbi, &db_key, &value, 0) != MDB_SUCCESS ||
		mdb_txn_commit(txn) != MDB_SUCCESS) {
		if (txn)
			mdb_txn_abort(txn);
	}
}

static void php_bacnet_win_lmdb_delete_prefix(php_bacnet_cache *cache, const char *prefix) {
	if (!cache->env)
		return;
	MDB_txn *txn = NULL;
	MDB_cursor *cursor = NULL;
	size_t prefix_length = strlen(prefix);
	if (mdb_txn_begin(cache->env, NULL, 0, &txn) != MDB_SUCCESS ||
		mdb_cursor_open(txn, cache->dbi, &cursor) != MDB_SUCCESS) {
		if (txn)
			mdb_txn_abort(txn);
		return;
	}
	MDB_val key = {prefix_length, (void *)prefix}, value;
	int result = mdb_cursor_get(cursor, &key, &value, MDB_SET_RANGE);
	while (result == MDB_SUCCESS && key.mv_size >= prefix_length &&
		   !memcmp(key.mv_data, prefix, prefix_length)) {
		mdb_cursor_del(cursor, 0);
		result = mdb_cursor_get(cursor, &key, &value, MDB_NEXT);
	}
	mdb_cursor_close(cursor);
	if (mdb_txn_commit(txn) != MDB_SUCCESS)
		mdb_txn_abort(txn);
}

static void php_bacnet_win_lmdb_prune(php_bacnet_cache *cache,
									  php_bacnet_cache_partition partition) {
	if (!cache->env)
		return;
	char partition_prefix[128], namespace_prefix[112], victim[512] = {0};
	size_t victim_length = 0;
	uint64_t oldest = UINT64_MAX;
	uint32_t count = 0;
	php_bacnet_win_lmdb_prefix(cache, partition, partition_prefix, sizeof(partition_prefix));
	MDB_txn *txn = NULL;
	MDB_cursor *cursor = NULL;
	if (mdb_txn_begin(cache->env, NULL, 0, &txn) != MDB_SUCCESS ||
		mdb_cursor_open(txn, cache->dbi, &cursor) != MDB_SUCCESS) {
		if (txn)
			mdb_txn_abort(txn);
		return;
	}
	MDB_val key = {strlen(partition_prefix), partition_prefix}, value;
	int result = mdb_cursor_get(cursor, &key, &value, MDB_SET_RANGE);
	while (result == MDB_SUCCESS && key.mv_size >= strlen(partition_prefix) &&
		   !memcmp(key.mv_data, partition_prefix, strlen(partition_prefix))) {
		php_bacnet_win_lmdb_value header = {0};
		if (value.mv_size >= sizeof(header))
			memcpy(&header, value.mv_data, sizeof(header));
		if (header.magic != PHP_BACNET_WIN_LMDB_MAGIC ||
			header.version != PHP_BACNET_WIN_LMDB_VERSION ||
			header.expires_at_ms <= php_bacnet_platform_wall_ms()) {
			mdb_cursor_del(cursor, 0);
			cache->expirations++;
			result = mdb_cursor_get(cursor, &key, &value, MDB_NEXT);
			continue;
		}
		count++;
		if (header.expires_at_ms < oldest && key.mv_size < sizeof(victim)) {
			oldest = header.expires_at_ms;
			victim_length = key.mv_size;
			memcpy(victim, key.mv_data, key.mv_size);
		}
		result = mdb_cursor_get(cursor, &key, &value, MDB_NEXT);
	}
	if (count > cache->max_entries[partition] && victim_length) {
		MDB_val old_key = {victim_length, victim};
		mdb_del(txn, cache->dbi, &old_key, NULL);
		cache->evictions++;
	}
	mdb_cursor_close(cursor);
	if (cache->l2_max_bytes > 0 && mdb_cursor_open(txn, cache->dbi, &cursor) == MDB_SUCCESS) {
		snprintf(namespace_prefix, sizeof(namespace_prefix), "%s|", cache->namespace_name);
		key.mv_size = strlen(namespace_prefix);
		key.mv_data = namespace_prefix;
		result = mdb_cursor_get(cursor, &key, &value, MDB_SET_RANGE);
		size_t bytes = 0;
		victim_length = 0;
		oldest = UINT64_MAX;
		while (result == MDB_SUCCESS && key.mv_size >= strlen(namespace_prefix) &&
			   !memcmp(key.mv_data, namespace_prefix, strlen(namespace_prefix))) {
			php_bacnet_win_lmdb_value header = {0};
			if (value.mv_size >= sizeof(header))
				memcpy(&header, value.mv_data, sizeof(header));
			bytes += value.mv_size;
			if (header.magic == PHP_BACNET_WIN_LMDB_MAGIC && header.expires_at_ms < oldest &&
				key.mv_size < sizeof(victim)) {
				oldest = header.expires_at_ms;
				victim_length = key.mv_size;
				memcpy(victim, key.mv_data, key.mv_size);
			}
			result = mdb_cursor_get(cursor, &key, &value, MDB_NEXT);
		}
		mdb_cursor_close(cursor);
		if (bytes > cache->l2_max_bytes && victim_length) {
			MDB_val old_key = {victim_length, victim};
			mdb_del(txn, cache->dbi, &old_key, NULL);
			cache->evictions++;
		}
	}
	if (mdb_txn_commit(txn) != MDB_SUCCESS)
		mdb_txn_abort(txn);
}

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
	cache->max_entries[PHP_BACNET_CACHE_STATE] = BACNET_G(cache_state_max_entries);
	cache->max_entries[PHP_BACNET_CACHE_OBJECT] = BACNET_G(cache_object_max_entries);
	cache->max_entries[PHP_BACNET_CACHE_OBJECT_LIST] = BACNET_G(cache_object_list_max_entries);
	cache->max_entries[PHP_BACNET_CACHE_DEVICE] = BACNET_G(cache_device_max_entries);
	cache->max_entries[PHP_BACNET_CACHE_IP] = BACNET_G(cache_ip_max_entries);
	cache->max_entries[PHP_BACNET_CACHE_NEGATIVE] = BACNET_G(cache_negative_max_entries);
	cache->l1_max_bytes = (size_t)BACNET_G(cache_l1_max_bytes);
	cache->l2_max_bytes = (size_t)BACNET_G(cache_l2_max_bytes);
	cache->lmdb_map_size = (size_t)BACNET_G(cache_lmdb_map_size);
	if (BACNET_G(cache_namespace) && *BACNET_G(cache_namespace))
		snprintf(cache->namespace_name, sizeof(cache->namespace_name), "%s",
				 BACNET_G(cache_namespace));
	else
		snprintf(cache->namespace_name, sizeof(cache->namespace_name), "%s:%u",
				 iface ? iface : "auto", port);
	snprintf(cache->lmdb_path, sizeof(cache->lmdb_path), "%s", BACNET_G(cache_lmdb_path));
	ZVAL_UNDEF(&cache->backend);
	cache->l2 = !BACNET_G(cache_l2_backend) || strcmp(BACNET_G(cache_l2_backend), "none")
					? PHP_BACNET_WIN_L2_LMDB
					: PHP_BACNET_WIN_L2_NONE;
	if (cache->enabled && !php_bacnet_win_open_shared(cache))
		cache->enabled = false;
	if (cache->enabled && cache->l2 == PHP_BACNET_WIN_L2_LMDB)
		php_bacnet_win_open_lmdb(cache);
	return cache;
}

void php_bacnet_cache_destroy(php_bacnet_cache *cache) {
	if (cache) {
		if (cache->backend_active)
			zval_ptr_dtor(&cache->backend);
		php_bacnet_win_close_shared(cache);
		php_bacnet_win_close_lmdb(cache);
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
	char lmdb_key[512];
	php_bacnet_win_key(cache, partition, key, lmdb_key, sizeof(lmdb_key));
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
	bool l2_hit = false;
	if (cache->l2 == PHP_BACNET_WIN_L2_LMDB)
		l2_hit = php_bacnet_win_lmdb_get(cache, lmdb_key, data, length);
	else if (cache->l2 == PHP_BACNET_WIN_L2_CALLBACK && cache->backend_active) {
		zval args[3], retval;
		ZVAL_STRING(&args[0], cache->namespace_name);
		ZVAL_STRING(&args[1], php_bacnet_win_partition_name(partition));
		ZVAL_STRING(&args[2], key);
		ZVAL_UNDEF(&retval);
		if (php_bacnet_win_callback_call(cache, "get", 3, args, &retval) &&
			Z_TYPE(retval) == IS_STRING && Z_STRLEN(retval) <= *length) {
			memcpy(data, Z_STRVAL(retval), Z_STRLEN(retval));
			*length = Z_STRLEN(retval);
			l2_hit = true;
		}
		zval_ptr_dtor(&args[0]);
		zval_ptr_dtor(&args[1]);
		zval_ptr_dtor(&args[2]);
		zval_ptr_dtor(&retval);
	}
	if (l2_hit) {
		uint64_t expiry =
			php_bacnet_platform_wall_ms() + (uint64_t)(cache->ttl[partition] * 1000.0);
		if (cache->shared && php_bacnet_win_lock(cache)) {
			php_bacnet_win_cache_entry *target = NULL;
			for (uint32_t i = 0; i < PHP_BACNET_WIN_CACHE_ENTRIES; i++) {
				if (!cache->shared->entries[i].used) {
					target = &cache->shared->entries[i];
					break;
				}
			}
			if (target) {
				target->used = true;
				target->partition = partition;
				target->length = *length;
				target->expires_at_ms = expiry;
				target->touched = ++cache->shared->tick;
				memcpy(target->key, key, strlen(key) + 1);
				memcpy(target->value, data, *length);
			}
			php_bacnet_win_unlock(cache);
		}
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
	char lmdb_key[512];
	php_bacnet_win_key(cache, partition, key, lmdb_key, sizeof(lmdb_key));
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
	uint64_t expiry = target->expires_at_ms;
	target->touched = ++cache->shared->tick;
	memcpy(target->key, key, strlen(key) + 1);
	memcpy(target->value, data, length);
	cache->stores++;
	php_bacnet_win_unlock(cache);
	if (cache->l2 == PHP_BACNET_WIN_L2_LMDB) {
		php_bacnet_win_lmdb_put(cache, lmdb_key, data, length, expiry);
		php_bacnet_win_lmdb_prune(cache, partition);
	} else if (cache->l2 == PHP_BACNET_WIN_L2_CALLBACK && cache->backend_active) {
		zval args[6], retval;
		ZVAL_STRING(&args[0], cache->namespace_name);
		ZVAL_STRING(&args[1], php_bacnet_win_partition_name(partition));
		ZVAL_STRING(&args[2], key);
		ZVAL_STRINGL(&args[3], (const char *)data, length);
		ZVAL_LONG(&args[4], expiry);
		ZVAL_LONG(&args[5], cache->max_entries[partition]);
		ZVAL_UNDEF(&retval);
		php_bacnet_win_callback_call(cache, "set", 6, args, &retval);
		for (int i = 0; i < 6; i++)
			zval_ptr_dtor(&args[i]);
		zval_ptr_dtor(&retval);
	}
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
	if (cache->env) {
		char prefix[128];
		if (partition < 0)
			snprintf(prefix, sizeof(prefix), "%s|", cache->namespace_name);
		else
			php_bacnet_win_lmdb_prefix(cache, (php_bacnet_cache_partition)partition, prefix,
									   sizeof(prefix));
		php_bacnet_win_lmdb_delete_prefix(cache, prefix);
	}
	if (cache->l2 == PHP_BACNET_WIN_L2_CALLBACK && cache->backend_active) {
		zval args[2], retval;
		ZVAL_STRING(&args[0], cache->namespace_name);
		if (partition < 0)
			ZVAL_NULL(&args[1]);
		else
			ZVAL_STRING(&args[1], php_bacnet_win_partition_name(partition));
		ZVAL_UNDEF(&retval);
		php_bacnet_win_callback_call(cache, "clear", 2, args, &retval);
		zval_ptr_dtor(&args[0]);
		zval_ptr_dtor(&args[1]);
		zval_ptr_dtor(&retval);
	}
	cache->invalidations++;
}

void php_bacnet_cache_invalidate(php_bacnet_cache *cache, php_bacnet_cache_partition partition,
								 const char *scope) {
	if (!cache)
		return;
	if (!scope || !*scope) {
		php_bacnet_cache_clear(cache, (int)partition);
		return;
	}
	if (cache->shared && php_bacnet_win_lock(cache)) {
		for (uint32_t i = 0; i < PHP_BACNET_WIN_CACHE_ENTRIES; i++) {
			php_bacnet_win_cache_entry *entry = &cache->shared->entries[i];
			if (entry->used && entry->partition == partition &&
				!strncmp(entry->key, scope, strlen(scope)))
				entry->used = false;
		}
		php_bacnet_win_unlock(cache);
	}
	if (cache->env) {
		char prefix[512];
		php_bacnet_win_lmdb_prefix(cache, partition, prefix, sizeof(prefix));
		snprintf(prefix + strlen(prefix), sizeof(prefix) - strlen(prefix), "%s", scope);
		php_bacnet_win_lmdb_delete_prefix(cache, prefix);
	}
	if (cache->l2 == PHP_BACNET_WIN_L2_CALLBACK && cache->backend_active) {
		zval args[3], retval;
		ZVAL_STRING(&args[0], cache->namespace_name);
		ZVAL_STRING(&args[1], php_bacnet_win_partition_name(partition));
		ZVAL_STRING(&args[2], scope);
		ZVAL_UNDEF(&retval);
		php_bacnet_win_callback_call(cache, "invalidate", 3, args, &retval);
		zval_ptr_dtor(&args[0]);
		zval_ptr_dtor(&args[1]);
		zval_ptr_dtor(&args[2]);
		zval_ptr_dtor(&retval);
	}
	cache->invalidations++;
}

void php_bacnet_cache_refresh(php_bacnet_cache *cache, php_bacnet_cache_partition partition,
							  const char *scope) {
	if (cache) {
		cache->refreshes++;
		php_bacnet_cache_invalidate(cache, partition, scope);
	}
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
	bool reopen_shared = false, reopen_lmdb = false;
	bool was_enabled = cache->enabled;
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
		else if (!strcmp(name, "namespace") && Z_TYPE_P(value) == IS_STRING &&
				 Z_STRLEN_P(value) > 0 && Z_STRLEN_P(value) < sizeof(cache->namespace_name)) {
			snprintf(cache->namespace_name, sizeof(cache->namespace_name), "%s", Z_STRVAL_P(value));
			reopen_shared = true;
			reopen_lmdb = true;
		} else if (!strcmp(name, "lmdb_path") && Z_TYPE_P(value) == IS_STRING &&
				   Z_STRLEN_P(value) > 0 && Z_STRLEN_P(value) < sizeof(cache->lmdb_path)) {
			snprintf(cache->lmdb_path, sizeof(cache->lmdb_path), "%s", Z_STRVAL_P(value));
			reopen_lmdb = true;
		} else if (!strcmp(name, "l1_max_bytes") && Z_TYPE_P(value) == IS_LONG &&
				   Z_LVAL_P(value) > 0)
			cache->l1_max_bytes = (size_t)Z_LVAL_P(value);
		else if (!strcmp(name, "l2_max_bytes") && Z_TYPE_P(value) == IS_LONG && Z_LVAL_P(value) > 0)
			cache->l2_max_bytes = (size_t)Z_LVAL_P(value);
		else if (!strcmp(name, "lmdb_map_size") && Z_TYPE_P(value) == IS_LONG &&
				 Z_LVAL_P(value) >= 1048576) {
			cache->lmdb_map_size = (size_t)Z_LVAL_P(value);
			reopen_lmdb = true;
		} else if (!strcmp(name, "l2_backend") && Z_TYPE_P(value) == IS_STRING) {
			const char *backend = Z_STRVAL_P(value);
			if (!strcmp(backend, "lmdb"))
				cache->l2 = PHP_BACNET_WIN_L2_LMDB;
			else if (!strcmp(backend, "none"))
				cache->l2 = PHP_BACNET_WIN_L2_NONE;
			else {
				*error = zend_strpprintf(0, "Ungültiges L2-Backend: %s", backend);
				return false;
			}
			if (cache->backend_active) {
				zval_ptr_dtor(&cache->backend);
				ZVAL_UNDEF(&cache->backend);
				cache->backend_active = false;
			}
			reopen_lmdb = true;
		} else {
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
				snprintf(option, sizeof(option), "%s_max_entries",
						 php_bacnet_win_partition_name(partition));
				if (!strcmp(name, option) && Z_TYPE_P(value) == IS_LONG && Z_LVAL_P(value) > 0) {
					cache->max_entries[partition] = (uint32_t)Z_LVAL_P(value);
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
	if (!was_enabled && cache->enabled) {
		reopen_shared = true;
		reopen_lmdb = true;
	}
	if (reopen_shared)
		php_bacnet_win_close_shared(cache);
	if (reopen_lmdb)
		php_bacnet_win_close_lmdb(cache);
	if (cache->enabled && !cache->shared && !php_bacnet_win_open_shared(cache)) {
		*error = zend_string_init("Windows Shared Memory konnte nicht geöffnet werden",
								  strlen("Windows Shared Memory konnte nicht geöffnet werden"), 0);
		return false;
	}
	if (cache->enabled && cache->l2 == PHP_BACNET_WIN_L2_LMDB && !cache->env) {
		if (!php_bacnet_win_open_lmdb(cache)) {
			*error = zend_string_init("Windows-LMDB konnte nicht geöffnet werden",
									  strlen("Windows-LMDB konnte nicht geöffnet werden"), 0);
			return false;
		}
	}
	php_bacnet_cache_clear(cache, -1);
	return true;
}

void php_bacnet_cache_get_options(php_bacnet_cache *cache, zval *return_value) {
	array_init(return_value);
	add_assoc_bool(return_value, "enabled", cache && cache->enabled);
	add_assoc_string(return_value, "l1_backend", "shared_memory");
	const char *l2 = !cache || cache->l2 == PHP_BACNET_WIN_L2_NONE ? "none"
					 : cache->l2 == PHP_BACNET_WIN_L2_CALLBACK	   ? "callback"
																   : "lmdb";
	add_assoc_string(return_value, "l2_backend", (char *)l2);
	add_assoc_string(return_value, "namespace", cache ? cache->namespace_name : "");
	add_assoc_string(return_value, "shm_name", cache ? cache->shm_name : "");
	add_assoc_string(return_value, "lmdb_path", cache ? cache->lmdb_path : "");
	if (!cache)
		return;
	add_assoc_long(return_value, "l1_max_bytes", (zend_long)cache->l1_max_bytes);
	add_assoc_long(return_value, "l2_max_bytes", (zend_long)cache->l2_max_bytes);
	add_assoc_long(return_value, "lmdb_map_size", (zend_long)cache->lmdb_map_size);
	for (int partition = 0; partition < PHP_BACNET_CACHE_PARTITION_COUNT; partition++) {
		char key[64];
		snprintf(key, sizeof(key), "%s_enabled", php_bacnet_win_partition_name(partition));
		add_assoc_bool(return_value, key, cache->part_enabled[partition]);
		snprintf(key, sizeof(key), "%s_ttl", php_bacnet_win_partition_name(partition));
		add_assoc_double(return_value, key, cache->ttl[partition]);
		snprintf(key, sizeof(key), "%s_max_entries", php_bacnet_win_partition_name(partition));
		add_assoc_long(return_value, key, cache->max_entries[partition]);
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
	add_assoc_bool(return_value, "l2_available",
				   cache->l2 == PHP_BACNET_WIN_L2_CALLBACK ? cache->backend_active
				   : cache->l2 == PHP_BACNET_WIN_L2_LMDB   ? cache->env != NULL
														   : true);
	add_assoc_long(return_value, "backend_errors", (zend_long)cache->backend_errors);
	if (include_entries) {
		zend_long entries = 0;
		if (cache->shared && php_bacnet_win_lock(cache)) {
			for (uint32_t i = 0; i < PHP_BACNET_WIN_CACHE_ENTRIES; i++)
				entries += cache->shared->entries[i].used;
			php_bacnet_win_unlock(cache);
		}
		add_assoc_long(return_value, "l1_entries", entries);
	}
	if (cache->env) {
		MDB_txn *txn = NULL;
		MDB_stat stat;
		if (mdb_txn_begin(cache->env, NULL, MDB_RDONLY, &txn) == MDB_SUCCESS &&
			mdb_stat(txn, cache->dbi, &stat) == MDB_SUCCESS)
			add_assoc_long(return_value, "l2_entries", stat.ms_entries);
		else
			add_assoc_null(return_value, "l2_entries");
		if (txn)
			mdb_txn_abort(txn);
	} else
		add_assoc_null(return_value, "l2_entries");
	if (reset)
		cache->hits = cache->misses = cache->stores = cache->expirations = cache->evictions =
			cache->invalidations = cache->refreshes = 0;
}

bool php_bacnet_cache_set_backend(php_bacnet_cache *cache, zval *backend) {
	if (!cache)
		return false;
	if (cache->backend_active)
		zval_ptr_dtor(&cache->backend);
	ZVAL_COPY(&cache->backend, backend);
	cache->backend_active = true;
	cache->l2 = PHP_BACNET_WIN_L2_CALLBACK;
	php_bacnet_win_close_lmdb(cache);
	php_bacnet_cache_clear(cache, -1);
	return true;
}
