--TEST--
Bacnet Client cache defaults, validation and callback backend
--SKIPIF--
<?php if (!extension_loaded('bacnet')) die('skip bacnet extension not loaded'); ?>
--INI--
bacnet.cache_enabled=0
bacnet.cache_lmdb_path=/tmp/php-bacnet-phpt-cache
--FILE--
<?php
@mkdir('/tmp/php-bacnet-phpt-cache', 0700);
final class MemoryBackend implements Bacnet\CacheBackendInterface {
    public function get(string $namespace, string $partition, string $key): ?string { return null; }
    public function set(string $namespace, string $partition, string $key, string $payload, int $expiresAtMs, int $maxEntries): void {}
    public function invalidate(string $namespace, string $partition, string $scope): void {}
    public function clear(string $namespace, ?string $partition): void {}
    public function getGeneration(string $namespace, string $partition): int { return 0; }
    public function bumpGeneration(string $namespace, string $partition): int { return 1; }
}

$loopback = PHP_OS_FAMILY === 'Windows' ? '127.0.0.1' : 'lo';
$client = new Bacnet\Client($loopback, 47930, 10);
$options = $client->getCacheOptions();
echo $options['l1_backend'], PHP_EOL;
echo $options['l2_backend'], PHP_EOL;
var_dump($options['object_enabled'], $options['state_enabled'], $options['negative_enabled']);
$client->setCacheOptions(['enabled' => true, 'state_enabled' => true, 'state_ttl' => 2.5]);
var_dump($client->getCacheOptions()['state_enabled']);
var_dump($client->getCacheStats()['l1_available']);
$client->setCacheBackend(new MemoryBackend());
echo $client->getCacheOptions()['l2_backend'], PHP_EOL;
var_dump(array_key_exists('hits', $client->getCacheStats()));
try { $client->setCacheOptions(['unknown' => 1]); } catch (ValueError $e) { echo "ValueError\n"; }
echo (new ReflectionMethod(Bacnet\Client::class, 'whoIs'))->getNumberOfParameters(), PHP_EOL;
echo (new ReflectionMethod(Bacnet\Device::class, 'readProperty'))->getNumberOfParameters(), PHP_EOL;
unset($client);
@unlink('/tmp/php-bacnet-phpt-cache/data.mdb');
@unlink('/tmp/php-bacnet-phpt-cache/lock.mdb');
@rmdir('/tmp/php-bacnet-phpt-cache');
?>
--EXPECT--
shared_memory
lmdb
bool(true)
bool(false)
bool(false)
bool(true)
bool(true)
callback
bool(true)
ValueError
4
5
