--TEST--
Bacnet\Value factory methods and writeProperty method signatures (Phase 5)
--EXTENSIONS--
bacnet
--FILE--
<?php
function php_bacnet_expect(bool $condition, string $message = "Expectation failed"): void {
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

// --- Bacnet\Value factory: primitive types ---

$vb = Bacnet\Value::boolean(true);
php_bacnet_expect($vb instanceof Bacnet\Value, 'boolean() instance');

$vu = Bacnet\Value::unsignedInt(255);
php_bacnet_expect($vu instanceof Bacnet\Value, 'unsignedInt() instance');

$vi = Bacnet\Value::signedInt(-7);
php_bacnet_expect($vi instanceof Bacnet\Value, 'signedInt() instance');

$vr = Bacnet\Value::real(22.5);
php_bacnet_expect($vr instanceof Bacnet\Value, 'real() instance');

$ve = Bacnet\Value::enumerated(3);
php_bacnet_expect($ve instanceof Bacnet\Value, 'enumerated() instance');

$vs = Bacnet\Value::characterString('hello');
php_bacnet_expect($vs instanceof Bacnet\Value, 'characterString() instance');

echo "primitive factories: OK\n";

// --- Bacnet\Value factory: composite types ---

$bs = new Bacnet\BitString([true, false, true, true]);
$vbs = Bacnet\Value::bitString($bs);
php_bacnet_expect($vbs instanceof Bacnet\Value, 'bitString() instance');

$d = new Bacnet\Date(2024, 6, 29, 6);
$vd = Bacnet\Value::date($d);
php_bacnet_expect($vd instanceof Bacnet\Value, 'date() instance');

$t = new Bacnet\Time(14, 30, 0, 0);
$vt = Bacnet\Value::time($t);
php_bacnet_expect($vt instanceof Bacnet\Value, 'time() instance');

$oid = new Bacnet\ObjectIdentifier(Bacnet\ObjectType::ANALOG_VALUE, 1);
$vo = Bacnet\Value::objectIdentifier($oid);
php_bacnet_expect($vo instanceof Bacnet\Value, 'objectIdentifier() instance');

echo "composite factories: OK\n";

// --- Factory methods are static ---
$rm = new ReflectionMethod('Bacnet\Value', 'real');
php_bacnet_expect($rm->isStatic(), 'real() is static');
$rm2 = new ReflectionMethod('Bacnet\Value', 'characterString');
php_bacnet_expect($rm2->isStatic(), 'characterString() is static');
echo "factory methods are static: OK\n";

// --- Device::writeProperty signature ---
$rm = new ReflectionMethod('Bacnet\Device', 'writeProperty');
php_bacnet_expect($rm->getNumberOfRequiredParameters() === 4,
    'Device::writeProperty requires 4 params, got ' . $rm->getNumberOfRequiredParameters());
php_bacnet_expect($rm->getNumberOfParameters() === 6,
    'Device::writeProperty has 6 total params, got ' . $rm->getNumberOfParameters());
$params = $rm->getParameters();
php_bacnet_expect($params[0]->getName() === 'objectType',   'param 0: objectType');
php_bacnet_expect($params[1]->getName() === 'instance',      'param 1: instance');
php_bacnet_expect($params[2]->getName() === 'property',      'param 2: property');
php_bacnet_expect($params[3]->getName() === 'value',         'param 3: value');
php_bacnet_expect($params[4]->getName() === 'priority',      'param 4: priority');
php_bacnet_expect($params[5]->getName() === 'arrayIndex',    'param 5: arrayIndex');
echo "Device::writeProperty signature: OK\n";

// --- ObjectRef::writeProperty signature ---
$rm = new ReflectionMethod('Bacnet\ObjectRef', 'writeProperty');
php_bacnet_expect($rm->getNumberOfRequiredParameters() === 2,
    'ObjectRef::writeProperty requires 2 params, got ' . $rm->getNumberOfRequiredParameters());
php_bacnet_expect($rm->getNumberOfParameters() === 4,
    'ObjectRef::writeProperty has 4 total params, got ' . $rm->getNumberOfParameters());
$params = $rm->getParameters();
php_bacnet_expect($params[0]->getName() === 'property',   'ObjectRef param 0: property');
php_bacnet_expect($params[1]->getName() === 'value',      'ObjectRef param 1: value');
php_bacnet_expect($params[2]->getName() === 'priority',   'ObjectRef param 2: priority');
php_bacnet_expect($params[3]->getName() === 'arrayIndex', 'ObjectRef param 3: arrayIndex');
echo "ObjectRef::writeProperty signature: OK\n";

// --- Device::readProperty still works ---
php_bacnet_expect(method_exists('Bacnet\Device', 'readProperty'), 'Device::readProperty exists');
$rm = new ReflectionMethod('Bacnet\Device', 'readProperty');
php_bacnet_expect($rm->getNumberOfRequiredParameters() === 3,
    'Device::readProperty requires 3 params');
echo "Device::readProperty unaffected: OK\n";

// --- Priority default value is 16 ---
$params = (new ReflectionMethod('Bacnet\Device', 'writeProperty'))->getParameters();
php_bacnet_expect($params[4]->isDefaultValueAvailable(), 'priority has default');
php_bacnet_expect($params[4]->getDefaultValue() === 16, 'priority default is 16');
php_bacnet_expect($params[5]->isDefaultValueAvailable(), 'arrayIndex has default');
php_bacnet_expect($params[5]->getDefaultValue() === null, 'arrayIndex default is null');
echo "default param values: OK\n";

// --- BitString round-trip through Value factory ---
$bs2 = new Bacnet\BitString([false, true, false, false, true]);
php_bacnet_expect($bs2->getLength() === 5, 'BitString length preserved');
php_bacnet_expect($bs2->getBit(1) === true,  'bit 1 set');
php_bacnet_expect($bs2->getBit(0) === false, 'bit 0 clear');
$vbs2 = Bacnet\Value::bitString($bs2);
php_bacnet_expect($vbs2 instanceof Bacnet\Value, 'bitString() value OK after round-trip');
echo "BitString round-trip: OK\n";

echo "All Phase 5 tests passed.\n";
?>
--EXPECT--
primitive factories: OK
composite factories: OK
factory methods are static: OK
Device::writeProperty signature: OK
ObjectRef::writeProperty signature: OK
Device::readProperty unaffected: OK
default param values: OK
BitString round-trip: OK
All Phase 5 tests passed.
