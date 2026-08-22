--TEST--
Bacnet transport is usable with thread-safe PHP builds
--SKIPIF--
<?php
if (!PHP_ZTS) {
    die('skip ZTS only');
}
if (!extension_loaded('bacnet')) {
    die('skip bacnet extension not loaded');
}
?>
--FILE--
<?php
$interface = getenv('BACNET_TEST_INTERFACE') ?: 'lo';
$port = (int) (getenv('BACNET_TEST_PORT') ?: 47819);

$first = new Bacnet\Client($interface, $port, 10);
try {
    new Bacnet\Client($interface, $port, 10);
    echo "singleton guard: missing\n";
} catch (Bacnet\Exception) {
    echo "singleton guard: OK\n";
}
unset($first);

$second = new Bacnet\Client($interface, $port, 10);
unset($second);
echo "ZTS transport lifecycle: OK\n";
?>
--EXPECT--
singleton guard: OK
ZTS transport lifecycle: OK
