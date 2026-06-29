--TEST--
Bacnet\ObjectType and Bacnet\Property enums + ObjectIdentifier
--SKIPIF--
<?php
if (!extension_loaded('bacnet')) die('skip bacnet extension not loaded');
?>
--FILE--
<?php
// ── ObjectType enum ──────────────────────────────────────────────────────
$cases = Bacnet\ObjectType::cases();
echo 'ObjectType case count: ' . count($cases) . PHP_EOL;

// Spot-check specific cases and values
echo Bacnet\ObjectType::ANALOG_INPUT->name  . ':' . Bacnet\ObjectType::ANALOG_INPUT->value  . PHP_EOL;
echo Bacnet\ObjectType::ANALOG_OUTPUT->name . ':' . Bacnet\ObjectType::ANALOG_OUTPUT->value . PHP_EOL;
echo Bacnet\ObjectType::ANALOG_VALUE->name  . ':' . Bacnet\ObjectType::ANALOG_VALUE->value  . PHP_EOL;
echo Bacnet\ObjectType::BINARY_INPUT->name  . ':' . Bacnet\ObjectType::BINARY_INPUT->value  . PHP_EOL;
echo Bacnet\ObjectType::BINARY_OUTPUT->name . ':' . Bacnet\ObjectType::BINARY_OUTPUT->value . PHP_EOL;
echo Bacnet\ObjectType::BINARY_VALUE->name  . ':' . Bacnet\ObjectType::BINARY_VALUE->value  . PHP_EOL;
echo Bacnet\ObjectType::DEVICE->name        . ':' . Bacnet\ObjectType::DEVICE->value        . PHP_EOL;
echo Bacnet\ObjectType::SCHEDULE->name      . ':' . Bacnet\ObjectType::SCHEDULE->value      . PHP_EOL;
echo Bacnet\ObjectType::TREND_LOG->name     . ':' . Bacnet\ObjectType::TREND_LOG->value     . PHP_EOL;

// from() must work for int-backed enum
$ot = Bacnet\ObjectType::from(2);
echo $ot->name . PHP_EOL;

// tryFrom() returns null for unknown value
$ot2 = Bacnet\ObjectType::tryFrom(999);
var_dump($ot2);

// ── Property enum ────────────────────────────────────────────────────────
echo 'Property PRESENT_VALUE:' . Bacnet\Property::PRESENT_VALUE->value . PHP_EOL;
echo 'Property OBJECT_NAME:'   . Bacnet\Property::OBJECT_NAME->value   . PHP_EOL;
echo 'Property UNITS:'         . Bacnet\Property::UNITS->value         . PHP_EOL;
echo 'Property OBJECT_LIST:'   . Bacnet\Property::OBJECT_LIST->value   . PHP_EOL;

// ── ObjectIdentifier ─────────────────────────────────────────────────────
$oid = new Bacnet\ObjectIdentifier(Bacnet\ObjectType::ANALOG_VALUE, 5);
echo $oid->getType()->name   . PHP_EOL;   // ANALOG_VALUE
echo $oid->getType()->value  . PHP_EOL;   // 2
echo $oid->getInstance()     . PHP_EOL;   // 5
echo (string)$oid            . PHP_EOL;   // 2:5

// Public properties set in constructor
echo $oid->type->name        . PHP_EOL;   // ANALOG_VALUE
echo $oid->instance          . PHP_EOL;   // 5

// Device:1234
$dev = new Bacnet\ObjectIdentifier(Bacnet\ObjectType::DEVICE, 1234);
echo (string)$dev            . PHP_EOL;   // 8:1234

// Instance out of range → exception
try {
    new Bacnet\ObjectIdentifier(Bacnet\ObjectType::BINARY_VALUE, 4194304);
    echo 'FAIL' . PHP_EOL;
} catch (Bacnet\Exception $e) {
    echo 'caught: ' . $e->getMessage() . PHP_EOL;
}

// Negative instance → exception
try {
    new Bacnet\ObjectIdentifier(Bacnet\ObjectType::SCHEDULE, -1);
    echo 'FAIL' . PHP_EOL;
} catch (Bacnet\Exception $e) {
    echo 'caught: ' . $e->getMessage() . PHP_EOL;
}

echo "done" . PHP_EOL;
?>
--EXPECT--
ObjectType case count: 14
ANALOG_INPUT:0
ANALOG_OUTPUT:1
ANALOG_VALUE:2
BINARY_INPUT:3
BINARY_OUTPUT:4
BINARY_VALUE:5
DEVICE:8
SCHEDULE:17
TREND_LOG:20
ANALOG_VALUE
NULL
Property PRESENT_VALUE:85
Property OBJECT_NAME:77
Property UNITS:117
Property OBJECT_LIST:76
ANALOG_VALUE
2
5
2:5
ANALOG_VALUE
5
8:1234
caught: Invalid BACnet object instance 4194304 (must be 0..4194303)
caught: Invalid BACnet object instance -1 (must be 0..4194303)
done
