--TEST--
Bacnet exception hierarchy
--SKIPIF--
<?php
if (!extension_loaded('bacnet')) die('skip bacnet extension not loaded');
?>
--FILE--
<?php
// Exception hierarchy
var_dump(is_a('Bacnet\TimeoutException', 'Bacnet\Exception', true));
var_dump(is_a('Bacnet\DeviceException',  'Bacnet\Exception', true));
var_dump(is_a('Bacnet\Exception',        '\Exception',       true));

// DeviceException properties
$e = new Bacnet\DeviceException('test', 0);
$e->errorClass = 3;
$e->errorCode  = 77;
var_dump($e->errorClass);
var_dump($e->errorCode);

echo "done\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
int(3)
int(77)
done
