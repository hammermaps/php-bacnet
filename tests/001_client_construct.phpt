--TEST--
Bacnet\Client: construction, singleton guard, destruction
--SKIPIF--
<?php
if (!extension_loaded('bacnet')) die('skip bacnet extension not loaded');
?>
--FILE--
<?php
// Basic construction
$c = new Bacnet\Client();
var_dump($c instanceof Bacnet\Client);
unset($c);

// Singleton reuse after destroy
$c2 = new Bacnet\Client();
var_dump($c2 instanceof Bacnet\Client);

// Singleton guard
try {
    $c3 = new Bacnet\Client();
    echo "FAIL: no exception thrown\n";
} catch (\Error $e) {
    echo "OK singleton guard\n";
}

unset($c2);

echo "done\n";
?>
--EXPECT--
bool(true)
bool(true)
OK singleton guard
done
