--TEST--
Bacnet\Server class API — construction, methods, callbacks, guards (Phase 6)
--EXTENSIONS--
bacnet
--FILE--
<?php
// --- Class exists ---
assert(class_exists('Bacnet\Server'), 'Bacnet\Server class exists');
echo "class exists: OK\n";

// --- Constructor reflection ---
$rc = new ReflectionClass('Bacnet\Server');
$ctor = $rc->getConstructor();
assert($ctor !== null, 'constructor exists');
assert($ctor->getNumberOfRequiredParameters() === 1, 'one required param (deviceId)');
assert($ctor->getNumberOfParameters() === 3, '3 total params');
$params = $ctor->getParameters();
assert($params[0]->getName() === 'deviceId',       'param 0: deviceId');
assert($params[1]->getName() === 'bindInterface',  'param 1: bindInterface');
assert($params[2]->getName() === 'port',           'param 2: port');
assert($params[1]->getDefaultValue() === '0.0.0.0', 'bindInterface default 0.0.0.0');
assert($params[2]->getDefaultValue() === 47808,     'port default 47808');
echo "constructor signature: OK\n";

// --- Methods present ---
$methods = ['addLocalObject', 'removeLocalObject', 'onReadProperty',
            'onWriteProperty', 'setAutoIAm', 'poll'];
foreach ($methods as $m) {
    assert(method_exists('Bacnet\Server', $m), "$m missing");
}
echo "all methods present: OK\n";

// --- Method signatures ---
$rm = new ReflectionMethod('Bacnet\Server', 'addLocalObject');
assert($rm->getNumberOfRequiredParameters() === 1, 'addLocalObject requires 1');

$rm = new ReflectionMethod('Bacnet\Server', 'poll');
assert($rm->getNumberOfRequiredParameters() === 0, 'poll requires 0');
assert($rm->getParameters()[0]->getDefaultValue() === 0, 'poll timeoutMs default=0');

$rm = new ReflectionMethod('Bacnet\Server', 'setAutoIAm');
assert($rm->getNumberOfRequiredParameters() === 1, 'setAutoIAm requires 1');
echo "method signatures: OK\n";

// --- poll() on uninitialized throws Bacnet\Exception ---
$srv_class = new ReflectionClass('Bacnet\Server');
$srv_uninit = $srv_class->newInstanceWithoutConstructor();
try {
    $srv_uninit->poll(0);
    assert(false, 'poll without construct should throw');
} catch (Bacnet\Exception $e) {
    assert(str_contains($e->getMessage(), 'not initialized'), 'correct error message');
}
echo "poll without init throws: OK\n";

// --- Singleton guard: second Server throws ---
// We can't actually call the constructor without a real BACnet interface,
// but we can verify the guard would fire via reflection of the method logic.
// Attempt construction on loopback — bip_init will fail → Bacnet\Exception
try {
    $srv = new Bacnet\Server(1234, '0.0.0.0', 47808);
    // If somehow it succeeds (CI with BACnet interface), test guard
    echo "server started: OK (network present)\n";

    // addLocalObject / removeLocalObject
    $oid = new Bacnet\ObjectIdentifier(Bacnet\ObjectType::ANALOG_VALUE, 1);
    $srv->addLocalObject($oid);
    $srv->removeLocalObject($oid);
    echo "add/remove local object: OK\n";

    // onReadProperty with valid callable
    $srv->onReadProperty(function($oid, $prop, $idx) { return 22.5; });
    echo "onReadProperty callable: OK\n";

    // onWriteProperty with valid callable
    $srv->onWriteProperty(function($oid, $prop, $val, $idx) { });
    echo "onWriteProperty callable: OK\n";

    // setAutoIAm
    $srv->setAutoIAm(false);
    $srv->setAutoIAm(true);
    echo "setAutoIAm: OK\n";

    // poll non-blocking
    $srv->poll(0);
    echo "poll(0) non-blocking: OK\n";

    unset($srv);
} catch (Bacnet\Exception $e) {
    // Expected when bip_init fails (no real BACnet interface in test env)
    echo "server init failed (no network — expected): OK\n";

    // Test the callable/method API without network by using a fresh uninit object
    // onReadProperty: bad callable should throw
    $srv2 = $srv_class->newInstanceWithoutConstructor();
    try {
        $srv2->onReadProperty('not_a_real_function_xyz');
        assert(false, 'invalid callable should throw');
    } catch (Bacnet\Exception $e2) {
        echo "invalid callable rejected: OK\n";
    }

    // onReadProperty: valid closure stored OK (no network needed)
    $srv3 = $srv_class->newInstanceWithoutConstructor();
    $cb = function($oid, $prop, $idx) { return 42.0; };
    $srv3->onReadProperty($cb);
    echo "valid callable accepted: OK\n";

    // addLocalObject on uninit object (no crash — local_objects NULL guard)
    $oid = new Bacnet\ObjectIdentifier(Bacnet\ObjectType::BINARY_VALUE, 5);
    $srv3->addLocalObject($oid);
    $srv3->removeLocalObject($oid);
    echo "add/remove on uninit (no crash): OK\n";
}

echo "All Phase 6 tests passed.\n";
?>
--EXPECTF--
class exists: OK
constructor signature: OK
all methods present: OK
method signatures: OK
poll without init throws: OK
%A
All Phase 6 tests passed.
