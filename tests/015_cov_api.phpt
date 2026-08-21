--TEST--
Bacnet COV subscription and notification API
--EXTENSIONS--
bacnet
--FILE--
<?php
function php_bacnet_expect(bool $condition, string $message): void {
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

$subscribe = new ReflectionMethod(Bacnet\Device::class, 'subscribeCOV');
php_bacnet_expect($subscribe->getNumberOfRequiredParameters() === 3, 'subscribe required parameters');
php_bacnet_expect($subscribe->getNumberOfParameters() === 6, 'subscribe parameter count');
php_bacnet_expect($subscribe->getReturnType()?->getName() === 'int', 'subscribe return type');

$unsubscribe = new ReflectionMethod(Bacnet\Device::class, 'unsubscribeCOV');
php_bacnet_expect($unsubscribe->getNumberOfRequiredParameters() === 4, 'unsubscribe required parameters');
php_bacnet_expect($unsubscribe->getNumberOfParameters() === 4, 'unsubscribe parameter count');
php_bacnet_expect($unsubscribe->getReturnType()?->getName() === 'void', 'unsubscribe return type');

$handler = new ReflectionMethod(Bacnet\Client::class, 'onCovNotification');
php_bacnet_expect($handler->getNumberOfRequiredParameters() === 1, 'handler parameter count');
php_bacnet_expect($handler->getParameters()[0]->getType()?->getName() === 'callable', 'handler type');
php_bacnet_expect(method_exists(Bacnet\Client::class, 'poll'), 'client poll exists');
php_bacnet_expect(method_exists(Bacnet\MixedServer::class, 'onCovNotification'), 'mixed handler exists');

echo "COV API: OK\n";
?>
--EXPECT--
COV API: OK
