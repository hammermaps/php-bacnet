--TEST--
Bacnet\Value factory methods and writeProperty method signatures (Phase 5)
--EXTENSIONS--
bacnet
--FILE--
<?php
// --- Bacnet\Value factory: primitive types ---

$vb = Bacnet\Value::boolean(true);
assert($vb instanceof Bacnet\Value, 'boolean() instance');

$vu = Bacnet\Value::unsignedInt(255);
assert($vu instanceof Bacnet\Value, 'unsignedInt() instance');

$vi = Bacnet\Value::signedInt(-7);
assert($vi instanceof Bacnet\Value, 'signedInt() instance');

$vr = Bacnet\Value::real(22.5);
assert($vr instanceof Bacnet\Value, 'real() instance');

$ve = Bacnet\Value::enumerated(3);
assert($ve instanceof Bacnet\Value, 'enumerated() instance');

$vs = Bacnet\Value::characterString('hello');
assert($vs instanceof Bacnet\Value, 'characterString() instance');

echo "primitive factories: OK\n";

// --- Bacnet\Value factory: composite types ---

$bs = new Bacnet\BitString([true, false, true, true]);
$vbs = Bacnet\Value::bitString($bs);
assert($vbs instanceof Bacnet\Value, 'bitString() instance');

$d = new Bacnet\Date(2024, 6, 29, 6);
$vd = Bacnet\Value::date($d);
assert($vd instanceof Bacnet\Value, 'date() instance');

$t = new Bacnet\Time(14, 30, 0, 0);
$vt = Bacnet\Value::time($t);
assert($vt instanceof Bacnet\Value, 'time() instance');

$oid = new Bacnet\ObjectIdentifier(Bacnet\ObjectType::ANALOG_VALUE, 1);
$vo = Bacnet\Value::objectIdentifier($oid);
assert($vo instanceof Bacnet\Value, 'objectIdentifier() instance');

echo "composite factories: OK\n";

// --- Factory methods are static ---
$rm = new ReflectionMethod('Bacnet\Value', 'real');
assert($rm->isStatic(), 'real() is static');
$rm2 = new ReflectionMethod('Bacnet\Value', 'characterString');
assert($rm2->isStatic(), 'characterString() is static');
echo "factory methods are static: OK\n";

// --- Device::writeProperty signature ---
$rm = new ReflectionMethod('Bacnet\Device', 'writeProperty');
assert($rm->getNumberOfRequiredParameters() === 4,
    'Device::writeProperty requires 4 params, got ' . $rm->getNumberOfRequiredParameters());
assert($rm->getNumberOfParameters() === 6,
    'Device::writeProperty has 6 total params, got ' . $rm->getNumberOfParameters());
$params = $rm->getParameters();
assert($params[0]->getName() === 'objectType',   'param 0: objectType');
assert($params[1]->getName() === 'instance',      'param 1: instance');
assert($params[2]->getName() === 'property',      'param 2: property');
assert($params[3]->getName() === 'value',         'param 3: value');
assert($params[4]->getName() === 'priority',      'param 4: priority');
assert($params[5]->getName() === 'arrayIndex',    'param 5: arrayIndex');
echo "Device::writeProperty signature: OK\n";

// --- ObjectRef::writeProperty signature ---
$rm = new ReflectionMethod('Bacnet\ObjectRef', 'writeProperty');
assert($rm->getNumberOfRequiredParameters() === 2,
    'ObjectRef::writeProperty requires 2 params, got ' . $rm->getNumberOfRequiredParameters());
assert($rm->getNumberOfParameters() === 4,
    'ObjectRef::writeProperty has 4 total params, got ' . $rm->getNumberOfParameters());
$params = $rm->getParameters();
assert($params[0]->getName() === 'property',   'ObjectRef param 0: property');
assert($params[1]->getName() === 'value',      'ObjectRef param 1: value');
assert($params[2]->getName() === 'priority',   'ObjectRef param 2: priority');
assert($params[3]->getName() === 'arrayIndex', 'ObjectRef param 3: arrayIndex');
echo "ObjectRef::writeProperty signature: OK\n";

// --- Device::readProperty still works ---
assert(method_exists('Bacnet\Device', 'readProperty'), 'Device::readProperty exists');
$rm = new ReflectionMethod('Bacnet\Device', 'readProperty');
assert($rm->getNumberOfRequiredParameters() === 3,
    'Device::readProperty requires 3 params');
echo "Device::readProperty unaffected: OK\n";

// --- Priority default value is 16 ---
$params = (new ReflectionMethod('Bacnet\Device', 'writeProperty'))->getParameters();
assert($params[4]->isDefaultValueAvailable(), 'priority has default');
assert($params[4]->getDefaultValue() === 16, 'priority default is 16');
assert($params[5]->isDefaultValueAvailable(), 'arrayIndex has default');
assert($params[5]->getDefaultValue() === null, 'arrayIndex default is null');
echo "default param values: OK\n";

// --- BitString round-trip through Value factory ---
$bs2 = new Bacnet\BitString([false, true, false, false, true]);
assert($bs2->getLength() === 5, 'BitString length preserved');
assert($bs2->getBit(1) === true,  'bit 1 set');
assert($bs2->getBit(0) === false, 'bit 0 clear');
$vbs2 = Bacnet\Value::bitString($bs2);
assert($vbs2 instanceof Bacnet\Value, 'bitString() value OK after round-trip');
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
