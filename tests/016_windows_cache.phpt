--TEST--
Bacnet Client provides the documented process-local cache fallback on Windows
--SKIPIF--
<?php
if (!extension_loaded('bacnet')) die('skip bacnet extension not loaded');
if (PHP_OS_FAMILY !== 'Windows') die('skip Windows only');
?>
--INI--
bacnet.cache_enabled=0
--FILE--
<?php
$client = new Bacnet\Client('127.0.0.1', 47931, 10);
$options = $client->getCacheOptions();
echo $options['l1_backend'], PHP_EOL;
echo $options['l2_backend'], PHP_EOL;
var_dump($options['enabled']);
$client->setCacheOptions(['enabled' => true, 'state_enabled' => true, 'state_ttl' => 2.5]);
$options = $client->getCacheOptions();
var_dump($options['state_enabled'], $options['state_ttl']);
var_dump(array_key_exists('hits', $client->getCacheStats()));
unset($client);
?>
--EXPECT--
shared_memory
none
bool(false)
bool(true)
float(2.5)
bool(true)
