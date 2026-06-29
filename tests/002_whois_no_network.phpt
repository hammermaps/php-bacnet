--TEST--
Bacnet\Client::whoIs returns array (no network — empty result is correct)
--SKIPIF--
<?php
if (!extension_loaded('bacnet')) die('skip bacnet extension not loaded');
?>
--FILE--
<?php
$c = new Bacnet\Client();

// whoIs with a very short timeout — no simulator running, should return empty array
$devices = $c->whoIs(timeoutMs: 100);

var_dump(is_array($devices));
var_dump($devices === []);  // empty when no simulator

// Verify return type contract
foreach ($devices as $d) {
    var_dump($d instanceof Bacnet\Device);
    var_dump(is_int($d->getDeviceId()));
    var_dump(is_string($d->getAddress()));
}

echo "done\n";
?>
--EXPECT--
bool(true)
bool(true)
done
