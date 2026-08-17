--TEST--
Bacnet\Server security reads INI values and MixedServer inherits API
--EXTENSIONS--
bacnet
--INI--
bacnet.server_per_source_rate=17.5
bacnet.server_write_burst=7
bacnet.server_allowed_networks=10.0.0.0/8,192.168.0.0/16
--FILE--
<?php
assert(is_subclass_of(Bacnet\MixedServer::class, Bacnet\Server::class));
foreach (['setSecurityOptions', 'getSecurityOptions', 'getSecurityStats'] as $method) {
    assert(method_exists(Bacnet\MixedServer::class, $method));
}
$server = new Bacnet\Server(4_193_901, '0.0.0.0', random_int(49000, 59000));
$options = $server->getSecurityOptions();
assert($options['per_source_rate'] === 17.5);
assert($options['write_burst'] === 7);
assert($options['allowed_networks'] === ['10.0.0.0/8', '192.168.0.0/16']);
echo "INI and inheritance: OK\n";
?>
--EXPECT--
INI and inheritance: OK
