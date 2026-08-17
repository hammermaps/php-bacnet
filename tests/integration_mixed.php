<?php
declare(strict_types=1);

if (!extension_loaded('bacnet')) {
    fwrite(STDERR, "Die Extension bacnet ist nicht geladen.\n");
    exit(2);
}

$interface = getenv('BACNET_TEST_INTERFACE');
$targetRaw = getenv('BACNET_TEST_DEVICE_ID');
if ($interface === false || $interface === '' || $targetRaw === false || $targetRaw === '') {
    fwrite(STDERR, "BACNET_TEST_INTERFACE und BACNET_TEST_DEVICE_ID sind erforderlich.\n");
    exit(2);
}

$localDeviceId = filter_var(getenv('BACNET_TEST_LOCAL_DEVICE_ID') ?: 4_194_000, FILTER_VALIDATE_INT);
$targetDeviceId = filter_var($targetRaw, FILTER_VALIDATE_INT);
$instance = filter_var(getenv('BACNET_TEST_INSTANCE') ?: $targetDeviceId, FILTER_VALIDATE_INT);
if (!is_int($localDeviceId) || !is_int($targetDeviceId) || !is_int($instance)) {
    fwrite(STDERR, "Ungültige Geräte-ID oder Objektinstanz.\n");
    exit(2);
}

$typeName = getenv('BACNET_TEST_OBJECT_TYPE') ?: 'device';
$propertyName = getenv('BACNET_TEST_PROPERTY') ?: 'object_name';
$types = [
    'analog_input' => Bacnet\ObjectType::ANALOG_INPUT,
    'analog_output' => Bacnet\ObjectType::ANALOG_OUTPUT,
    'analog_value' => Bacnet\ObjectType::ANALOG_VALUE,
    'binary_input' => Bacnet\ObjectType::BINARY_INPUT,
    'binary_output' => Bacnet\ObjectType::BINARY_OUTPUT,
    'binary_value' => Bacnet\ObjectType::BINARY_VALUE,
    'device' => Bacnet\ObjectType::DEVICE,
];
$properties = [
    'object_name' => Bacnet\Property::OBJECT_NAME,
    'present_value' => Bacnet\Property::PRESENT_VALUE,
];
if (!isset($types[$typeName], $properties[$propertyName])) {
    fwrite(STDERR, "Nicht unterstützter Objekttyp oder Property.\n");
    exit(2);
}

$mixed = new Bacnet\MixedServer($localDeviceId, $interface, 47_808);
$localObject = new Bacnet\ObjectIdentifier(Bacnet\ObjectType::ANALOG_VALUE, 1);
$mixed->addLocalObject($localObject);
$mixed->onReadProperty(static fn ($object, $property, $index) => match ($property) {
    Bacnet\Property::OBJECT_NAME => 'mixed_integration_value',
    Bacnet\Property::PRESENT_VALUE => 42.0,
    default => null,
});

$devices = $mixed->whoIs($targetDeviceId, $targetDeviceId, 2_000);
if ($devices === []) {
    throw new RuntimeException("Zielgerät {$targetDeviceId} wurde nicht gefunden.");
}

$value = $devices[0]->readProperty(
    $types[$typeName],
    $instance,
    $properties[$propertyName],
);
printf(
    "Mixed discovery/read: device=%d address=%s value=%s pending=%d\n",
    $devices[0]->getDeviceId(),
    $devices[0]->getAddress(),
    is_scalar($value) || $value === null ? var_export($value, true) : get_debug_type($value),
    $mixed->getPendingPduCount(),
);

while ($mixed->getPendingPduCount() > 0) {
    $mixed->poll(0);
}
echo "Mixed queue drained: OK\n";
