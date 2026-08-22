--TEST--
Bacnet Client provides the shared-memory and LMDB cache on Windows
--SKIPIF--
<?php
if (!extension_loaded('bacnet')) die('skip bacnet extension not loaded');
if (PHP_OS_FAMILY !== 'Windows') die('skip Windows only');
?>
--INI--
bacnet.cache_enabled=0
--FILE--
<?php
$path = sys_get_temp_dir() . DIRECTORY_SEPARATOR . 'php-bacnet-windows-phpt-cache';
@mkdir($path, 0700, true);
ini_set('bacnet.cache_lmdb_path', $path);
$client = new Bacnet\Client('127.0.0.1', 47931, 10);
$options = $client->getCacheOptions();
echo $options['l1_backend'], PHP_EOL;
echo $options['l2_backend'], PHP_EOL;
var_dump($options['enabled']);
$client->setCacheOptions([
    'enabled' => true,
    'state_enabled' => true,
    'state_ttl' => 2.5,
    'state_max_entries' => 3,
]);
$options = $client->getCacheOptions();
var_dump($options['state_enabled'], $options['state_ttl']);
var_dump($options['state_max_entries']);
echo $options['l2_backend'], PHP_EOL;
var_dump($client->getCacheStats()['l2_available']);
$client->setCacheOptions(['l2_backend' => 'none']);
echo $client->getCacheOptions()['l2_backend'], PHP_EOL;
$client->setCacheOptions(['l2_backend' => 'lmdb']);
echo $client->getCacheOptions()['l2_backend'], PHP_EOL;
unset($client);
@unlink($path . DIRECTORY_SEPARATOR . 'data.mdb');
@unlink($path . DIRECTORY_SEPARATOR . 'lock.mdb');
@rmdir($path);
?>
--EXPECT--
shared_memory
lmdb
bool(false)
bool(true)
float(2.5)
int(3)
lmdb
bool(true)
none
lmdb
