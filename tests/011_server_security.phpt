--TEST--
Bacnet\Server security defaults, overrides, validation and statistics
--EXTENSIONS--
bacnet
--FILE--
<?php
$uninitialized = (new ReflectionClass(Bacnet\Server::class))->newInstanceWithoutConstructor();
try {
    $uninitialized->getSecurityOptions();
    assert(false);
} catch (Bacnet\Exception) {
    echo "uninitialized guard: OK\n";
}

$port = random_int(49000, 59000);
$server = new Bacnet\Server(4_193_900, '0.0.0.0', $port);
$defaults = $server->getSecurityOptions();
assert($defaults['enabled'] === true);
assert($defaults['per_source_rate'] === 50.0);
assert($defaults['per_source_burst'] === 100);
assert($defaults['global_rate'] === 500.0);
assert($defaults['who_is_rate'] === 2.0);
assert($defaults['write_rate'] === 5.0);
assert($defaults['max_sources'] === 1024);

$server->setSecurityOptions([
    'per_source_rate' => 25,
    'write_rate' => 3.5,
    'allowed_networks' => ['192.168.202.0/24'],
]);
$options = $server->getSecurityOptions();
assert($options['per_source_rate'] === 25.0);
assert($options['write_rate'] === 3.5);
assert($options['allowed_networks'] === ['192.168.202.0/24']);
assert($options['global_rate'] === 500.0);

$stats = $server->getSecurityStats(includeSources: true, reset: true);
foreach (['accepted_packets', 'rate_drops', 'acl_drops', 'blocked_drops',
          'malformed_pdus', 'queue_overflows', 'deduplicated_writes',
          'active_sources', 'blocked_sources', 'sources'] as $key) {
    assert(array_key_exists($key, $stats));
}

foreach ([
    ['unknown' => 1],
    ['per_source_rate' => 0],
    ['max_sources' => 1.5],
    ['allowed_networks' => ['not-a-cidr']],
] as $invalid) {
    try {
        $server->setSecurityOptions($invalid);
        assert(false);
    } catch (ValueError) {
        echo "ValueError: OK\n";
    }
}
echo "security API: OK\n";
?>
--EXPECT--
uninitialized guard: OK
ValueError: OK
ValueError: OK
ValueError: OK
ValueError: OK
security API: OK
