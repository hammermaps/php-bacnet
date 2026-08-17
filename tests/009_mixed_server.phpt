--TEST--
Bacnet\MixedServer exposes compatible server and client APIs
--EXTENSIONS--
bacnet
--FILE--
<?php
assert(class_exists(Bacnet\MixedServer::class));
assert(is_subclass_of(Bacnet\MixedServer::class, Bacnet\Server::class));

$serverMethods = [
    'addLocalObject',
    'removeLocalObject',
    'onReadProperty',
    'onWriteProperty',
    'setAutoIAm',
    'poll',
];
foreach ($serverMethods as $method) {
    assert(method_exists(Bacnet\MixedServer::class, $method));
}
assert(method_exists(Bacnet\MixedServer::class, 'whoIs'));
assert(method_exists(Bacnet\MixedServer::class, 'getPendingPduCount'));

$method = new ReflectionMethod(Bacnet\MixedServer::class, 'whoIs');
assert($method->getNumberOfRequiredParameters() === 0);
assert($method->getNumberOfParameters() === 3);

$uninitialized = (new ReflectionClass(Bacnet\MixedServer::class))
    ->newInstanceWithoutConstructor();
assert($uninitialized->getPendingPduCount() === 0);
try {
    $uninitialized->whoIs(timeoutMs: 1);
    echo "missing exception\n";
} catch (Bacnet\Exception $exception) {
    echo "mixed server API: OK\n";
}
?>
--EXPECT--
mixed server API: OK
