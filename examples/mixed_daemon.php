<?php
declare(strict_types=1);

if (PHP_SAPI !== 'cli' || !extension_loaded('bacnet')) {
    fwrite(STDERR, "CLI und die PHP-Erweiterung bacnet werden benötigt.\n");
    exit(1);
}

function envInt(string $name, int $default, int $minimum, int $maximum): int
{
    $raw = getenv($name);
    $value = filter_var($raw === false || $raw === '' ? $default : $raw, FILTER_VALIDATE_INT, [
        'options' => ['min_range' => $minimum, 'max_range' => $maximum],
    ]);
    if (!is_int($value)) {
        throw new InvalidArgumentException("Ungültiger Wert für {$name}.");
    }

    return $value;
}

$interface = getenv('BACNET_DEMO_INTERFACE') ?: 'net3';
$deviceId = envInt('BACNET_DEMO_DEVICE_ID', 5, 0, 4_194_302);
$port = envInt('BACNET_DEMO_PORT', 47_808, 1, 65_535);
$switchSeconds = envInt('BACNET_DEMO_SWITCH_SECONDS', 6, 1, 3_600);
$discoverySeconds = envInt('BACNET_DEMO_DISCOVERY_SECONDS', 60, 5, 3_600);

$mixed = new Bacnet\MixedServer($deviceId, $interface, $port);
$deviceObject = new Bacnet\ObjectIdentifier(Bacnet\ObjectType::DEVICE, $deviceId);
$testObject = new Bacnet\ObjectIdentifier(Bacnet\ObjectType::ANALOG_VALUE, 1);
$mixed->addLocalObject($deviceObject);
$mixed->addLocalObject($testObject);

$currentValue = static fn (): float => (float) (intdiv(time(), $switchSeconds) % 2);
$mixed->onReadProperty(static function (
    Bacnet\ObjectIdentifier $object,
    Bacnet\Property|int $property,
    ?int $arrayIndex,
) use ($deviceId, $currentValue): mixed {
    if ($object->getType() === Bacnet\ObjectType::DEVICE
        && $object->getInstance() === $deviceId) {
        return $property === Bacnet\Property::OBJECT_NAME ? 'proxy' : null;
    }

    if ($object->getType() !== Bacnet\ObjectType::ANALOG_VALUE
        || $object->getInstance() !== 1) {
        return null;
    }

    return match ($property) {
        Bacnet\Property::OBJECT_NAME => 'proxy_test_value',
        Bacnet\Property::PRESENT_VALUE => $currentValue(),
        default => null,
    };
});

printf(
    "MixedServer Device %d auf %s:%d; ANALOG_VALUE:1 wechselt alle %d s.\n",
    $deviceId,
    $interface,
    $port,
    $switchSeconds,
);

$nextDiscovery = 0;
while (true) {
    do {
        $mixed->poll($mixed->getPendingPduCount() > 0 ? 0 : 100);
    } while ($mixed->getPendingPduCount() > 0);

    if (time() < $nextDiscovery) {
        continue;
    }

    try {
        $devices = $mixed->whoIs(timeoutMs: 1_000);
        printf("[%s] %d externe Geräte gefunden.\n", date('c'), count($devices));
    } catch (Bacnet\Exception $error) {
        fprintf(STDERR, "[%s] Discovery fehlgeschlagen: %s\n", date('c'), $error->getMessage());
    }
    $nextDiscovery = time() + $discoverySeconds;
}
