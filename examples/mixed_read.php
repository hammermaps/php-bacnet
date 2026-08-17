<?php
declare(strict_types=1);

if (PHP_SAPI !== 'cli' || !extension_loaded('bacnet')) {
    fwrite(STDERR, "CLI und die PHP-Erweiterung bacnet werden benötigt.\n");
    exit(1);
}

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

if ($argc !== 5 || !isset($types[$argv[2]], $properties[$argv[4]])) {
    fwrite(STDERR, "Aufruf: php examples/mixed_read.php <device-id> <object-type> <instance> <property>\n");
    exit(2);
}

$targetDeviceId = filter_var($argv[1], FILTER_VALIDATE_INT, [
    'options' => ['min_range' => 0, 'max_range' => 4_194_302],
]);
$instance = filter_var($argv[3], FILTER_VALIDATE_INT, [
    'options' => ['min_range' => 0, 'max_range' => 4_194_302],
]);
if (!is_int($targetDeviceId) || !is_int($instance)) {
    fwrite(STDERR, "Geräte-ID und Instanz müssen gültige BACnet-Instanzen sein.\n");
    exit(2);
}

$interface = getenv('BACNET_DEMO_INTERFACE') ?: 'net3';
$localDeviceId = filter_var(getenv('BACNET_DEMO_DEVICE_ID') ?: 5, FILTER_VALIDATE_INT, [
    'options' => ['min_range' => 0, 'max_range' => 4_194_302],
]);
if (!is_int($localDeviceId)) {
    fwrite(STDERR, "BACNET_DEMO_DEVICE_ID ist ungültig.\n");
    exit(2);
}

try {
    $mixed = new Bacnet\MixedServer($localDeviceId, $interface, 47_808);
    $devices = $mixed->whoIs($targetDeviceId, $targetDeviceId, 2_000);
    if ($devices === []) {
        throw new RuntimeException("Gerät {$targetDeviceId} wurde nicht gefunden.");
    }

    $value = $devices[0]->readProperty(
        $types[$argv[2]],
        $instance,
        $properties[$argv[4]],
    );
    printf(
        "Device %d, %s:%d, %s = %s\n",
        $targetDeviceId,
        $argv[2],
        $instance,
        $argv[4],
        is_scalar($value) || $value === null ? var_export($value, true) : (string) $value,
    );

    while ($mixed->getPendingPduCount() > 0) {
        $mixed->poll(0);
    }
} catch (Throwable $error) {
    fwrite(STDERR, "Mixed-Read fehlgeschlagen: {$error->getMessage()}\n");
    exit(1);
}
