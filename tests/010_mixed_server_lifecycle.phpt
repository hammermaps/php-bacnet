--TEST--
Bacnet\MixedServer lifecycle, callbacks and singleton guard
--EXTENSIONS--
bacnet
--FILE--
<?php
$reflection = new ReflectionClass(Bacnet\MixedServer::class);
$uninitialized = $reflection->newInstanceWithoutConstructor();
assert($uninitialized->getPendingPduCount() === 0);

try {
    $mixed = new Bacnet\MixedServer(4_194_000, '0.0.0.0', 47808);
    assert($mixed instanceof Bacnet\Server);

    $object = new Bacnet\ObjectIdentifier(Bacnet\ObjectType::ANALOG_VALUE, 1);
    $mixed->addLocalObject($object);
    $mixed->onReadProperty(static fn ($oid, $property, $index) => 42.0);
    $mixed->onWriteProperty(static function ($oid, $property, $value, $index): void {});
    $mixed->setAutoIAm(true);
    $mixed->poll(0);

    try {
        new Bacnet\Client();
        assert(false, 'MixedServer must hold the singleton socket');
    } catch (Error $error) {
        assert(str_contains($error->getMessage(), 'Only one'));
    }

    unset($mixed);
} catch (Bacnet\Exception $error) {
    // A network-less test environment may reject bip_init(). API checks above
    // and in 009_mixed_server.phpt remain valid without a socket.
}

echo "mixed lifecycle: OK\n";
?>
--EXPECTF--
%Amixed lifecycle: OK
