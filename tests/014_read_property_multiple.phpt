--TEST--
Bacnet\Server beantwortet ReadPropertyMultiple einschliesslich PROP_ALL und APDU-Abort
--EXTENSIONS--
bacnet
--SKIPIF--
<?php
if (!extension_loaded('sockets')) {
    die('skip sockets wird benötigt');
}
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

function rpRequest(int $invokeId, int $type, int $instance, int $property, ?int $arrayIndex = null): string {
    $request = "\x0c" . pack('N', ($type << 22) | $instance) . "\x19" . chr($property);
    if ($arrayIndex !== null) {
        $request .= "\x29" . chr($arrayIndex);
    }
    $apdu = "\x00\x05" . chr($invokeId) . "\x0c" . $request;
    return "\x81\x0a" . pack('n', strlen($apdu) + 6) . "\x01\x00" . $apdu;
}

function expect(bool $condition, string $message): void {
    if (!$condition) {
        throw new RuntimeException($message);
    }
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

function packetRoundTrip(Bacnet\Server $server, Socket $client, string $localIp, int $port, string $packet): string {
    socket_sendto($client, $packet, strlen($packet), 0, $localIp, $port);
    $server->poll(1000);
    socket_set_option($client, SOL_SOCKET, SO_RCVTIMEO, ['sec' => 1, 'usec' => 0]);
    $response = '';
    $source = '';
    $sourcePort = 0;
    if (socket_recvfrom($client, $response, 2048, 0, $source, $sourcePort) === false) {
        throw new RuntimeException('Keine BACnet-Antwort empfangen');
    }
    return $response;
}

$interface = (string)getenv('BACNET_TEST_INTERFACE');
$localIp = (string)getenv('BACNET_TEST_LOCAL_IP');
$port = (int)(getenv('BACNET_TEST_PORT') ?: 47819);
$server = new Bacnet\Server(4_193_901, $interface, $port);
$server->setDeviceInfo([
    'vendorId' => 1,
    'vendorName' => 'Test Vendor',
    'modelName' => 'Test Model',
    'objectName' => 'test-device',
    'description' => 'Loopback test device',
    'firmwareRevision' => '0.1.1-test',
    'applicationSoftwareVersion' => '0.1.1-test',
]);
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
expect(substr($response, 6, 3) === "\x30\x21\x0e", 'RPM-ACK für mehrere Objekte fehlt.');
expect(str_contains($response, 'object-1'), 'Objektkontext für object-1 ging verloren.');
expect(str_contains($response, pack('G', 10.0)), 'Present_Value von object-1 fehlt.');
expect(str_contains($response, pack('G', 20.0)), 'Present_Value von object-2 fehlt.');
echo "mehrere Eigenschaften und Objekte: OK\n";

/* PROP_ALL liefert die definierten Eigenschaften des lokalen Objekts. */
$response = rpmRoundTrip($server, $client, $localIp, $port, 0x22, rpmObject(2, 1, [8]));
expect(substr($response, 6, 3) === "\x30\x22\x0e", 'RPM-ACK für PROP_ALL fehlt.');
expect(str_contains($response, 'object-1'), 'PROP_ALL enthält keinen Object_Name.');
expect(str_contains($response, pack('G', 10.0)), 'PROP_ALL enthält keinen Present_Value.');
echo "PROP_ALL: OK\n";

/* Unbekannte Eigenschaften werden als PropertyAccessError kodiert. */
$response = rpmRoundTrip($server, $client, $localIp, $port, 0x23, rpmObject(2, 1, [250]));
expect(substr($response, 6, 3) === "\x30\x23\x0e", 'RPM-ACK für unbekannte Property fehlt.');
expect(str_contains($response, "\x5e\x91\x02\x91\x20\x5f"), 'PropertyAccessError fehlt.');
echo "Property-Fehler: OK\n";

/* Eine RPM-Antwort oberhalb des implementierten APDU-Puffers wird BACnet-konform abgebrochen. */
$response = rpmRoundTrip($server, $client, $localIp, $port, 0x24,
    rpmObject(2, 1, array_fill(0, 250, 85)));
expect(substr($response, 6, 3) === "\x71\x24\x04", 'APDU-Abort fehlt.');
echo "APDU-Abort: OK\n";

/* Das DEVICE ist implizit vorhanden; Object_List bleibt bei add/remove aktuell. */
$deviceObjectId = pack('N', (8 << 22) | 4_193_901);
$analogOneObjectId = pack('N', (2 << 22) | 1);
$analogTwoObjectId = pack('N', (2 << 22) | 2);
$response = packetRoundTrip($server, $client, $localIp, $port, rpRequest(0x31, 8, 4_193_901, 76, 0));
expect(substr($response, 6, 3) === "\x30\x31\x0c", 'ReadProperty-ACK für Object_List[0] fehlt.');
expect(str_contains($response, "\x21\x03"), 'Object_List[0] enthält nicht drei Objekte.');
$response = packetRoundTrip($server, $client, $localIp, $port, rpRequest(0x32, 8, 4_193_901, 76, 1));
expect(str_contains($response, "\xc4" . $deviceObjectId), 'Object_List[1] ist nicht das DEVICE.');
$response = packetRoundTrip($server, $client, $localIp, $port, rpRequest(0x33, 8, 4_193_901, 76));
expect(str_contains($response, "\xc4" . $deviceObjectId), 'Object_List enthält DEVICE nicht.');
expect(str_contains($response, "\xc4" . $analogOneObjectId), 'Object_List enthält ANALOG_VALUE:1 nicht.');
expect(str_contains($response, "\xc4" . $analogTwoObjectId), 'Object_List enthält ANALOG_VALUE:2 nicht.');
$server->removeLocalObject(new Bacnet\ObjectIdentifier(Bacnet\ObjectType::ANALOG_VALUE, 2));
$response = packetRoundTrip($server, $client, $localIp, $port, rpRequest(0x34, 8, 4_193_901, 76, 0));
expect(str_contains($response, "\x21\x02"), 'Object_List wurde nach removeLocalObject nicht aktualisiert.');
echo "Object_List dynamisch: OK\n";

/* DEVICE-Metadaten und die dynamische Liste unterstützter Objekttypen. */
$response = packetRoundTrip($server, $client, $localIp, $port, rpRequest(0x35, 8, 4_193_901, 121));
expect(str_contains($response, 'Test Vendor'), 'Vendor_Name aus setDeviceInfo fehlt.');
$response = packetRoundTrip($server, $client, $localIp, $port, rpRequest(0x36, 8, 4_193_901, 70));
expect(str_contains($response, 'Test Model'), 'Model_Name aus setDeviceInfo fehlt.');
$response = packetRoundTrip($server, $client, $localIp, $port, rpRequest(0x37, 8, 4_193_901, 96));
expect(substr($response, 6, 3) === "\x30\x37\x0c", 'ReadProperty-ACK für Object_Types_Supported fehlt.');
expect(str_contains($response, "\x83\x07\x20\x80"), 'Object_Types_Supported enthält DEVICE und ANALOG_VALUE nicht.');
echo "DEVICE-Metadaten und Typen: OK\n";

/* Jeder nicht implementierte bestätigte Dienst erhält einen BACnet Reject. */
$unsupportedApdu = "\x00\x05\x38\x1a";
$unsupportedPacket = "\x81\x0a" . pack('n', strlen($unsupportedApdu) + 6) . "\x01\x00" . $unsupportedApdu;
$response = packetRoundTrip($server, $client, $localIp, $port, $unsupportedPacket);
expect(substr($response, 6, 3) === "\x60\x38\x09", 'Reject unrecognized-service fehlt.');
echo "Reject unbekannter Dienst: OK\n";

/* announce() sendet unabhängig von setAutoIAm() ein I-Am. */
$server->setAutoIAm(false);
expect($server->announce() === null, 'announce() schlug fehl.');
echo "I-Am Broadcast: OK\n";
?>
--EXPECT--
mehrere Eigenschaften und Objekte: OK
PROP_ALL: OK
Property-Fehler: OK
APDU-Abort: OK
Object_List dynamisch: OK
DEVICE-Metadaten und Typen: OK
Reject unbekannter Dienst: OK
I-Am Broadcast: OK
