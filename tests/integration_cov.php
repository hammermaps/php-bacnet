<?php
declare(strict_types=1);

/*
 * Real BACnet COV integration test.
 *
 * This test is opt-in and requires a reachable COV-capable BACnet device.
 * Configure the target through BACNET_TEST_COV_* environment variables.
 */

function php_bacnet_cov_env_int(string $name, int $default): int
{
    $value = getenv($name);
    return $value === false || $value === '' ? $default : (int) $value;
}

$interface = getenv('BACNET_TEST_INTERFACE') ?: 'net3';
$port = php_bacnet_cov_env_int('BACNET_TEST_COV_PORT', 47808);
$deviceIdValue = getenv('BACNET_TEST_COV_DEVICE_ID');
if ($deviceIdValue === false || $deviceIdValue === '') {
    throw new RuntimeException(
        'BACNET_TEST_COV_DEVICE_ID muss für den optionalen COV-Integrationstest gesetzt sein',
    );
}
$deviceId = (int) $deviceIdValue;
$instance = php_bacnet_cov_env_int('BACNET_TEST_COV_INSTANCE', 0);
$observeSeconds = php_bacnet_cov_env_int('BACNET_TEST_COV_SECONDS', 20);
$lifetime = max(30, $observeSeconds + 10);
$values = [];
$subscriptionId = null;

$client = new Bacnet\Client($interface, $port, 3000);
$client->onCovNotification(static function (array $event) use (&$values, $deviceId, $instance): void {
    if ($event['deviceId'] !== $deviceId ||
        $event['objectType'] !== Bacnet\ObjectType::ANALOG_INPUT->value ||
        $event['instance'] !== $instance) {
        return;
    }

    foreach ($event['properties'] as $property) {
        if ($property['property'] === Bacnet\Property::PRESENT_VALUE->value && is_numeric($property['value'])) {
            $value = (float) $property['value'];
            $values[] = $value;
            printf("COV %.6f (Restlaufzeit: %d s)\n", $value, $event['timeRemaining']);
        }
    }
});

try {
    $devices = $client->whoIs($deviceId, $deviceId, 3000, true);
    if (count($devices) !== 1) {
        throw new RuntimeException("BACnet COV-Testgerät {$deviceId} nicht gefunden");
    }

    $device = $devices[0];
    $subscriptionId = $device->subscribeCOV(
        Bacnet\ObjectType::ANALOG_INPUT,
        $instance,
        Bacnet\Property::PRESENT_VALUE,
        $lifetime,
    );

    $deadline = microtime(true) + $observeSeconds;
    while (microtime(true) < $deadline && count($values) < 2) {
        $client->poll(1000);
    }

    if (count($values) < 2) {
        throw new RuntimeException('Zu wenige COV-Temperaturwerte empfangen: ' . count($values));
    }

    printf(
        "COV-Integration: %d Werte, min=%.6f, max=%.6f\n",
        count($values),
        min($values),
        max($values),
    );
} finally {
    if ($subscriptionId !== null && isset($device)) {
        $device->unsubscribeCOV(
            Bacnet\ObjectType::ANALOG_INPUT,
            $instance,
            Bacnet\Property::PRESENT_VALUE,
            $subscriptionId,
        );
        echo "COV-Anmeldung abgemeldet\n";
    }
}
