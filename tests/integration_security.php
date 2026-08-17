<?php
declare(strict_types=1);

if (!extension_loaded('sockets') || !extension_loaded('pcntl')) {
    fwrite(STDERR, "SKIP: sockets und pcntl werden benötigt\n");
    exit(0);
}

$interface = getenv('BACNET_TEST_INTERFACE') ?: 'net3';
$localIp = getenv('BACNET_TEST_LOCAL_IP') ?: '192.168.202.5';
$port = (int) (getenv('BACNET_TEST_PORT') ?: 47_819);

function packet(string $npduAndApdu): string
{
    return "\x81\x0a" . pack('n', 4 + strlen($npduAndApdu)) . $npduAndApdu;
}

function whoIsPacket(): string
{
    return packet("\x01\x00\x10\x08");
}

function writePacket(int $invokeId = 7): string
{
    $objectId = (2 << 22) | 1; // ANALOG_VALUE:1
    $apdu = "\x00\x05" . chr($invokeId) . "\x0f"
        . "\x0c" . pack('N', $objectId)
        . "\x19\x55" // context property 85: PRESENT_VALUE
        . "\x3e\x44" . pack('G', 22.5) . "\x3f";
    return packet("\x01\x00" . $apdu);
}

function sender(string $localIp): Socket
{
    $socket = socket_create(AF_INET, SOCK_DGRAM, SOL_UDP);
    if ($socket === false || !socket_bind($socket, $localIp, 0)) {
        throw new RuntimeException('UDP-Sender konnte nicht gebunden werden');
    }
    return $socket;
}

function sendAndPoll(Socket $socket, string $target, int $port, string $packet, object $server): void
{
    socket_sendto($socket, $packet, strlen($packet), 0, $target, $port);
    $server->poll(100);
}

function assertCounter(object $server, string $counter, int $minimum): void
{
    $stats = $server->getSecurityStats();
    if ($stats[$counter] < $minimum) {
        throw new RuntimeException("$counter: erwartet >= $minimum, erhalten {$stats[$counter]}");
    }
    printf("%-24s %d OK\n", $counter, $stats[$counter]);
}

$server = new Bacnet\Server(4_193_910, $interface, $port);
$udp = sender($localIp);
$base = [
    'allowed_networks' => ["$localIp/32"],
    'denied_networks' => [],
    'per_source_rate' => 10_000,
    'per_source_burst' => 100,
    'global_rate' => 10_000,
    'global_burst' => 100,
    'who_is_rate' => 10_000,
    'who_is_burst' => 100,
    'write_rate' => 10_000,
    'write_burst' => 100,
    'log_interval_seconds' => 3600,
];

// Per-source bucket
$server->setSecurityOptions(array_merge($base, ['per_source_rate' => 0.001, 'per_source_burst' => 2]));
$server->getSecurityStats(reset: true);
for ($i = 0; $i < 5; $i++) sendAndPoll($udp, $localIp, $port, whoIsPacket(), $server);
assertCounter($server, 'rate_drops', 3);

// Global bucket
$server->setSecurityOptions(array_merge($base, ['global_rate' => 0.001, 'global_burst' => 2]));
$server->getSecurityStats(reset: true);
for ($i = 0; $i < 5; $i++) sendAndPoll($udp, $localIp, $port, whoIsPacket(), $server);
assertCounter($server, 'rate_drops', 3);

// Who-Is bucket
$server->setSecurityOptions(array_merge($base, ['who_is_rate' => 0.001, 'who_is_burst' => 2]));
$server->getSecurityStats(reset: true);
for ($i = 0; $i < 5; $i++) sendAndPoll($udp, $localIp, $port, whoIsPacket(), $server);
assertCounter($server, 'rate_drops', 3);

// Temporäre Sperre nach zwei Verletzungen
$server->setSecurityOptions(array_merge($base, [
    'per_source_rate' => 0.001,
    'per_source_burst' => 1,
    'flood_violations' => 2,
    'flood_window_seconds' => 10,
    'block_duration_seconds' => 1,
]));
$server->getSecurityStats(reset: true);
for ($i = 0; $i < 4; $i++) sendAndPoll($udp, $localIp, $port, whoIsPacket(), $server);
assertCounter($server, 'blocked_drops', 1);

// Allow- und Denylisten; Deny gewinnt
$server->setSecurityOptions(array_merge($base, ['allowed_networks' => ['203.0.113.0/24']]));
$server->getSecurityStats(reset: true);
sendAndPoll($udp, $localIp, $port, whoIsPacket(), $server);
assertCounter($server, 'acl_drops', 1);
$server->setSecurityOptions(array_merge($base, ['denied_networks' => ["$localIp/32"]]));
$server->getSecurityStats(reset: true);
sendAndPoll($udp, $localIp, $port, whoIsPacket(), $server);
assertCounter($server, 'acl_drops', 1);

// Erfolgreicher Write wird nur einmal an PHP dispatcht, aber zweimal bestätigt.
$writes = 0;
$server->setSecurityOptions($base);
$server->addLocalObject(new Bacnet\ObjectIdentifier(Bacnet\ObjectType::ANALOG_VALUE, 1));
$server->onWriteProperty(static function () use (&$writes): void { $writes++; });
$server->getSecurityStats(reset: true);
sendAndPoll($udp, $localIp, $port, writePacket(), $server);
sendAndPoll($udp, $localIp, $port, writePacket(), $server);
if ($writes !== 1) throw new RuntimeException("Write-Callback lief $writes-mal statt einmal");
assertCounter($server, 'deduplicated_writes', 1);

socket_close($udp);
unset($server);
gc_collect_cycles();

// MixedServer filtert vor seiner 32-PDU-Queue.
$mixed = new Bacnet\MixedServer(4_193_911, $interface, $port);
$mixed->setSecurityOptions(array_merge($base, ['who_is_rate' => 0.001, 'who_is_burst' => 1]));
$mixed->getSecurityStats(reset: true);
$pid = pcntl_fork();
if ($pid === 0) {
    usleep(50_000);
    $childSocket = sender($localIp);
    for ($i = 0; $i < 100; $i++) {
        socket_sendto($childSocket, whoIsPacket(), strlen(whoIsPacket()), 0, $localIp, $port);
    }
    socket_close($childSocket);
    exit(0);
}
$mixed->whoIs(timeoutMs: 300);
pcntl_waitpid($pid, $status);
if ($mixed->getPendingPduCount() > 1) {
    throw new RuntimeException('Flood belegte mehr Queue-Plätze als der konfigurierte Burst');
}
assertCounter($mixed, 'rate_drops', 1);
assertCounter($mixed, 'queue_overflows', 0);
echo "UDP security integration: OK\n";
