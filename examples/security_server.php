<?php
declare(strict_types=1);

use Bacnet\ObjectIdentifier;
use Bacnet\ObjectType;
use Bacnet\Property;
use Bacnet\Server;

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

/** @return string[] */
function envCidrs(string $name, string $default = ''): array
{
    $raw = getenv($name);
    $value = $raw === false ? $default : $raw;
    if (trim($value) === '') {
        return [];
    }

    return array_values(array_filter(array_map('trim', explode(',', $value)), 'strlen'));
}

$interface = getenv('BACNET_DEMO_INTERFACE') ?: 'net3';
$deviceId = envInt('BACNET_DEMO_DEVICE_ID', 9_001, 0, 4_194_302);
$port = envInt('BACNET_DEMO_PORT', 47_808, 1, 65_535);
$statsSeconds = envInt('BACNET_DEMO_STATS_SECONDS', 10, 1, 3_600);
$includeSources = getenv('BACNET_DEMO_INCLUDE_SOURCES') === '1';
$allowWrites = getenv('BACNET_DEMO_ALLOW_WRITE') === '1';

$server = new Server($deviceId, $interface, $port);
$server->setSecurityOptions([
    'allowed_networks' => envCidrs(
        'BACNET_DEMO_ALLOWED_NETWORKS',
        '192.168.202.0/24',
    ),
    'denied_networks' => envCidrs('BACNET_DEMO_DENIED_NETWORKS'),
]);

$server->addLocalObject(new ObjectIdentifier(ObjectType::ANALOG_VALUE, 1));
$presentValue = 21.5;
$server->onReadProperty(static function (
    ObjectIdentifier $object,
    Property|int $property,
    ?int $arrayIndex,
) use (&$presentValue): mixed {
    return match ($property) {
        Property::OBJECT_NAME => 'security_demo',
        Property::PRESENT_VALUE => $presentValue,
        default => null,
    };
});

if ($allowWrites) {
    $server->onWriteProperty(static function (
        ObjectIdentifier $object,
        Property|int $property,
        mixed $value,
        ?int $arrayIndex,
    ) use (&$presentValue): void {
        if ($object->getType() !== ObjectType::ANALOG_VALUE
            || $object->getInstance() !== 1
            || $property !== Property::PRESENT_VALUE
            || !is_float($value)
            || $value < 10.0
            || $value > 30.0) {
            return;
        }
        $presentValue = $value;
        printf("[%s] PRESENT_VALUE = %.2f\n", date('c'), $presentValue);
    });
}

printf(
    "Security-Server Device %d auf %s:%d; Writes %s.\n",
    $deviceId,
    $interface,
    $port,
    $allowWrites ? 'aktiv' : 'deaktiviert',
);
echo json_encode($server->getSecurityOptions(), JSON_THROW_ON_ERROR), PHP_EOL;

$nextStats = monotonicTime() + $statsSeconds;
while (true) {
    $server->poll(100);
    if (monotonicTime() >= $nextStats) {
        $stats = $server->getSecurityStats(includeSources: $includeSources);
        fwrite(STDOUT, json_encode($stats, JSON_THROW_ON_ERROR) . PHP_EOL);
        $nextStats = monotonicTime() + $statsSeconds;
    }
}

function monotonicTime(): float
{
    return hrtime(true) / 1_000_000_000;
}
