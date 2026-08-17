# Mixed-Modus

`Bacnet\MixedServer` kombiniert BACnet/IP-Server und -Client in einem
langlaufenden PHP-Prozess. Beide Seiten verwenden denselben UDP-Socket. Das ist
besonders für Gateways geeignet, die eigene Objekte bereitstellen und zugleich
Werte externer Geräte lesen oder schreiben müssen.

## Abgrenzung der Betriebsarten

| Klasse | Eingehende Anfragen | Ausgehende Anfragen | Socket |
|---|---|---|---|
| `Bacnet\Client` | Nein | Ja | Ein eigener Socket |
| `Bacnet\Server` | Ja | Nein | Ein eigener Socket |
| `Bacnet\MixedServer` | Ja | Ja | Ein gemeinsam genutzter Socket |

Pro PHP-Prozess darf weiterhin nur eine dieser Instanzen existieren. Bestehende
`Client`- und `Server`-Programme müssen nicht geändert werden.

Der Client-Anteil verwendet POSIX Shared Memory als festen L1 und LMDB als
Standard-L2. Eingehende Server-Callbacks werden nicht gecacht. Cacheoptionen
und ein eigenes PHP-L2 können direkt am `MixedServer` gesetzt werden; siehe
[client-cache.md](client-cache.md).

## Grundaufbau

```php
$mixed = new Bacnet\MixedServer(
    deviceId: 5,
    bindInterface: 'net3',
    port: 47808,
);

$mixed->addLocalObject(new Bacnet\ObjectIdentifier(
    Bacnet\ObjectType::ANALOG_VALUE,
    1,
));

$mixed->onReadProperty(static function ($object, $property, $arrayIndex) {
    return match ($property) {
        Bacnet\Property::OBJECT_NAME => 'proxy_test_value',
        Bacnet\Property::PRESENT_VALUE => 42.0,
        default => null,
    };
});

$devices = $mixed->whoIs(lowLimit: 200, highLimit: 200, timeoutMs: 1000);
if ($devices !== []) {
    $value = $devices[0]->readProperty(
        Bacnet\ObjectType::ANALOG_VALUE,
        10110,
        Bacnet\Property::PRESENT_VALUE,
    );
}

while (true) {
    $mixed->poll(100);
}
```

Die von `whoIs()` gelieferten `Device`-Objekte behalten eine Referenz auf den
`MixedServer`. Ihre Lese- und Schreibmethoden verwenden deshalb automatisch
denselben Socket. Die eigene Geräte-ID wird aus Discovery-Ergebnissen entfernt.

## Queue und Event-Loop

Client-Aufrufe sind synchron. Während `whoIs()`, `readProperty()` oder
`writeProperty()` auf Antworten warten, werden gleichzeitig eintreffende
Server-PDUs in einer Queue mit maximal 32 Einträgen gesichert. Anschließende
`poll()`-Aufrufe verarbeiten jeweils ein PDU.

Vor dem Einreihen gelten dieselben ACL-, Rate-, Block- und PDU-Prüfungen wie
bei `Server::poll()`. Ein Paket-Flood verdrängt daher keine zulässigen Anfragen
aus der 32-PDU-Queue. Verworfene Pakete erscheinen in
`getSecurityStats()`; Warnungen werden höchstens einmal pro konfiguriertem
Logintervall aggregiert ausgegeben.

Instanz-Overrides werden wie beim normalen Server gesetzt:

```php
$mixed->setSecurityOptions([
    'allowed_networks' => ['192.168.202.0/24'],
    'per_source_rate' => 50,
    'who_is_rate' => 2,
]);

$stats = $mixed->getSecurityStats(includeSources: false);
```

Die vollständige Options- und Statistikreferenz steht unter
[Server-Sicherheit](./server-security.md).

```php
do {
    $mixed->poll($mixed->getPendingPduCount() > 0 ? 0 : 100);
} while ($mixed->getPendingPduCount() > 0);
```

Verwende kurze Client-Timeouts und führe umfangreiche Discovery- oder
Objektlisten-Abfragen nicht in jedem Event-Loop-Durchlauf aus. Bei dauerhaft
mehr als 32 eingehenden Anfragen können weitere PDUs verworfen werden.

## Daemon- und Redis-Betrieb

Ein zentraler Daemon kann Redis-Aufträge seriell entgegennehmen, mit dem
`MixedServer` ausführen und Antworten über eine Korrelations-ID zurückgeben.
Nur der Daemon greift dabei auf BACnet zu; HTTP-Worker öffnen keine eigenen
BACnet-Sockets. Zwischen Redis-Aufträgen muss der Daemon regelmäßig `poll()`
aufrufen und die PDU-Queue leeren.

## Fehlerbehandlung

- `TimeoutException`: Das entfernte Gerät hat nicht rechtzeitig geantwortet.
- `DeviceException`: Das Gerät hat einen BACnet-Fehler zurückgegeben.
- `Error`: Im Prozess existiert bereits ein Client oder Server.
- `getPendingPduCount() > 0`: Gepufferte Serveranfragen zeitnah mit `poll()` abarbeiten.

UDP `47808` sollte verwendet werden, weil manche Geräte I-Am-Antworten immer an
den BACnet-Standardport senden. Vollständige Beispiele liegen unter
[`examples/`](../examples/README.md).

## Sicherheitsgrenze

Die eingebauten Limits begrenzen Fehlkonfigurationen und einfache Floods, sind
aber keine kryptografische Authentisierung. Klassisches BACnet/IP bleibt
unverschlüsselt und unauthentifiziert. VLAN und Firewall bilden weiterhin die
primäre Netzgrenze. BACnet/SC ist die spätere kryptografische Lösung und liegt
außerhalb des Umfangs dieser Erweiterungsversion.
