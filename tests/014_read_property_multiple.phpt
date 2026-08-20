--TEST--
Bacnet\Server beantwortet ReadPropertyMultiple einschliesslich PROP_ALL und APDU-Abort
--EXTENSIONS--
bacnet
 sockets
--SKIPIF--
<?php
if (!getenv('BACNET_TEST_INTERFACE') || !getenv('BACNET_TEST_LOCAL_IP')) {
    die('skip BACNET_TEST_INTERFACE und BACNET_TEST_LOCAL_IP sind erforderlich');
}
?>
--FILE--
<?php
declare(strict_types=1);

function rpmObject(int $type, int $instance, array $properties): string {
    $result = "\x0c" . pack('N', ($type << 22) | $instance) . "\x1e";
    foreach ($properties as $property) {
        $result .= "\x09" . chr($property);
    }
    return $result . "\x1f";
}

function rpmRequest(int $invokeId, string $objects): string {
    $apdu = "\x00\x05" . chr($invokeId) . "\x0e" . $objects;
    return "\x81\x0a" . pack('n', strlen($apdu) + 6) . "\x01\x00" . $apdu;
}

function rpmRoundTrip(Bacnet\Server $server, Socket $client, string $localIp, int $port, int $invokeId, string $objects): string {
    $packet = rpmRequest($invokeId, $objects);
    socket_sendto($client, $packet, strlen($packet), 0, $localIp, $port);
    $server->poll(1000);
    socket_set_option($client, SOL_SOCKET, SO_RCVTIMEO, ['sec' => 1, 'usec' => 0]);
    $response = '';
    $source = '';
    $sourcePort = 0;
    $received = socket_recvfrom($client, $response, 2048, 0, $source, $sourcePort);
    if ($received === false) {
        throw new RuntimeException('Keine RPM-Antwort empfangen');
    }
    return $response;
}

$interface = (string)getenv('BACNET_TEST_INTERFACE');
$localIp = (string)getenv('BACNET_TEST_LOCAL_IP');
$port = (int)(getenv('BACNET_TEST_PORT') ?: 47819);
$server = new Bacnet\Server(4_193_901, $interface, $port);
foreach ([1, 2] as $instance) {
    $server->addLocalObject(new Bacnet\ObjectIdentifier(Bacnet\ObjectType::ANALOG_VALUE, $instance));
}
$server->onReadProperty(static fn ($oid, $property) => match ($property) {
    Bacnet\Property::OBJECT_NAME => 'object-' . $oid->instance,
    Bacnet\Property::PRESENT_VALUE => (float)($oid->instance * 10),
    default => null,
});
$client = socket_create(AF_INET, SOCK_DGRAM, SOL_UDP);
if ($client === false || !socket_bind($client, $localIp, 47808)) {
    throw new RuntimeException('UDP-Testclient konnte nicht an Port 47808 gebunden werden');
}

/* Zwei Eigenschaften eines Objekts sowie ein weiteres Objekt. */
$response = rpmRoundTrip($server, $client, $localIp, $port, 0x21,
    rpmObject(2, 1, [77, 85]) . rpmObject(2, 2, [85]));
assert(substr($response, 6, 3) === "\x30\x21\x0e");
assert(str_contains($response, 'object-1'));
assert(str_contains($response, pack('G', 10.0)));
assert(str_contains($response, pack('G', 20.0)));
echo "mehrere Eigenschaften und Objekte: OK\n";

/* PROP_ALL liefert die definierten Eigenschaften des lokalen Objekts. */
$response = rpmRoundTrip($server, $client, $localIp, $port, 0x22, rpmObject(2, 1, [8]));
assert(substr($response, 6, 3) === "\x30\x22\x0e");
assert(str_contains($response, 'object-1'));
assert(str_contains($response, pack('G', 10.0)));
echo "PROP_ALL: OK\n";

/* Unbekannte Eigenschaften werden als PropertyAccessError kodiert. */
$response = rpmRoundTrip($server, $client, $localIp, $port, 0x23, rpmObject(2, 1, [250]));
assert(substr($response, 6, 3) === "\x30\x23\x0e");
assert(str_contains($response, "\x5e\x91\x02\x91\x20\x5f"));
echo "Property-Fehler: OK\n";

/* Eine RPM-Antwort oberhalb von 480 Byte wird BACnet-konform abgebrochen. */
$response = rpmRoundTrip($server, $client, $localIp, $port, 0x24,
    rpmObject(2, 1, array_fill(0, 80, 85)));
assert(substr($response, 6, 3) === "\x71\x24\x04");
echo "APDU-Abort: OK\n";
?>
--EXPECT--
mehrere Eigenschaften und Objekte: OK
PROP_ALL: OK
Property-Fehler: OK
APDU-Abort: OK
