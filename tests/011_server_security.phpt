--TEST--
Bacnet\Server security defaults, overrides, validation and statistics
--EXTENSIONS--
bacnet
--FILE--
<?php
function php_bacnet_expect(bool $condition, string $message = "Expectation failed"): void {
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

$uninitialized = (new ReflectionClass(Bacnet\Server::class))->newInstanceWithoutConstructor();
try {
    $uninitialized->getSecurityOptions();
    php_bacnet_expect(false);
} catch (Bacnet\Exception) {
    echo "uninitialized guard: OK\n";
}

$port = 47820;
$server = new Bacnet\Server(4_193_900, '0.0.0.0', $port);
$defaults = $server->getSecurityOptions();
php_bacnet_expect($defaults['enabled'] === true);
php_bacnet_expect($defaults['per_source_rate'] === 50.0);
php_bacnet_expect($defaults['per_source_burst'] === 100);
php_bacnet_expect($defaults['global_rate'] === 500.0);
php_bacnet_expect($defaults['who_is_rate'] === 2.0);
php_bacnet_expect($defaults['write_rate'] === 5.0);
php_bacnet_expect($defaults['max_sources'] === 1024);

$server->setSecurityOptions([
    'per_source_rate' => 25,
    'write_rate' => 3.5,
    'allowed_networks' => ['192.168.202.0/24'],
]);
$options = $server->getSecurityOptions();
php_bacnet_expect($options['per_source_rate'] === 25.0);
php_bacnet_expect($options['write_rate'] === 3.5);
php_bacnet_expect($options['allowed_networks'] === ['192.168.202.0/24']);
php_bacnet_expect($options['global_rate'] === 500.0);

$stats = $server->getSecurityStats(includeSources: true, reset: true);
foreach (['accepted_packets', 'rate_drops', 'acl_drops', 'blocked_drops',
          'malformed_pdus', 'queue_overflows', 'deduplicated_writes',
          'active_sources', 'blocked_sources', 'sources'] as $key) {
    php_bacnet_expect(array_key_exists($key, $stats));
}

foreach ([
    ['unknown' => 1],
    ['per_source_rate' => 0],
    ['max_sources' => 1.5],
    ['allowed_networks' => ['not-a-cidr']],
] as $invalid) {
    try {
        $server->setSecurityOptions($invalid);
        php_bacnet_expect(false);
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
