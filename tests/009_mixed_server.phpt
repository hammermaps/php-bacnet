--TEST--
Bacnet\MixedServer exposes compatible server and client APIs
--EXTENSIONS--
bacnet
--FILE--
<?php
function php_bacnet_expect(bool $condition, string $message = "Expectation failed"): void {
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

php_bacnet_expect(class_exists(Bacnet\MixedServer::class));
php_bacnet_expect(is_subclass_of(Bacnet\MixedServer::class, Bacnet\Server::class));

$serverMethods = [
    'addLocalObject',
    'removeLocalObject',
    'onReadProperty',
    'onWriteProperty',
    'setAutoIAm',
    'poll',
];
foreach ($serverMethods as $method) {
    php_bacnet_expect(method_exists(Bacnet\MixedServer::class, $method));
}
php_bacnet_expect(method_exists(Bacnet\MixedServer::class, 'whoIs'));
php_bacnet_expect(method_exists(Bacnet\MixedServer::class, 'getPendingPduCount'));

$method = new ReflectionMethod(Bacnet\MixedServer::class, 'whoIs');
php_bacnet_expect($method->getNumberOfRequiredParameters() === 0);
php_bacnet_expect($method->getNumberOfParameters() === 4);

$uninitialized = (new ReflectionClass(Bacnet\MixedServer::class))
    ->newInstanceWithoutConstructor();
php_bacnet_expect($uninitialized->getPendingPduCount() === 0);
try {
    $uninitialized->whoIs(timeoutMs: 1);
    echo "missing exception\n";
} catch (Bacnet\Exception $exception) {
    echo "mixed server API: OK\n";
}
?>
--EXPECT--
mixed server API: OK
