# php-bacnet — API-Referenz

> Vollständige Dokumentation der `bacnet`-PHP-Erweiterung — Version 0.1.0  
> Stil: [php.net](https://www.php.net/manual/de/) Referenzhandbuch

---

## Inhaltsverzeichnis

1. [Einleitung](#1-einleitung)
2. [Installation](#2-installation)
3. [Konfiguration (php.ini)](#3-konfiguration-phpini)
4. [Typzuordnung — BACnet ↔ PHP](#4-typzuordnung--bacnet--php)
5. [Klassen](#5-klassen)
   - [Bacnet\Client](#bacnetclient)
   - [Bacnet\Device](#bacnetdevice)
   - [Bacnet\ObjectRef](#bacnetobjectref)
   - [Bacnet\Server](#bacnetserver)
   - [Bacnet\Value](#bacnetvalue)
   - [Bacnet\BitString](#bacnetbitstring)
   - [Bacnet\Date](#bacnetdate)
   - [Bacnet\Time](#bacnettime)
   - [Bacnet\ObjectIdentifier](#bacnetobjectidentifier)
   - [Bacnet\ScheduleEntry](#bacnetscheduleentry)
   - [Bacnet\WeeklySchedule](#bacnetweeklyschedule)
   - [Bacnet\TrendLogRecord](#bacnettrendlogrecord)
6. [Enumeratoren](#6-enumeratoren)
   - [Bacnet\ObjectType](#bacnetobjecttype)
   - [Bacnet\Property](#bacnetproperty)
7. [Ausnahmen](#7-ausnahmen)
   - [Bacnet\Exception](#bacnetexception)
   - [Bacnet\TimeoutException](#bacnettimeoutexception)
   - [Bacnet\DeviceException](#bacnetdeviceexception)
8. [Vollständige Beispiele](#8-vollständige-beispiele)

---

## 1. Einleitung

**php-bacnet** ist eine native PHP 8.4+-Erweiterung für die BACnet/IP-Kommunikation.
Sie kapselt die [bacnet-stack](https://github.com/bacnet-stack/bacnet-stack)-C-Bibliothek
und stellt eine typsichere, objektorientierte API im Namensraum `Bacnet\` bereit.

**Unterstützte Funktionalität:**

| Funktion | Beschreibung |
|----------|-------------|
| `whoIs()` | Geräte im Netzwerk per Broadcast finden (Who-Is / I-Am) |
| `readProperty()` | Beliebige Eigenschaften von BACnet-Objekten lesen |
| `writeProperty()` | Eigenschaften schreiben (mit optionaler Priorität) |
| Server-Modus | Eigene BACnet-Objekte bereitstellen, Callbacks für Read/Write |
| Alle Grundtypen | REAL, UNSIGNED INT, ENUMERATED, CHARACTER STRING, BIT STRING, DATE, TIME, OBJECT IDENTIFIER |
| WeeklySchedule | WEEKLY_SCHEDULE lesen (Dekodierung) |
| TrendLog | LOG_BUFFER als `TrendLogRecord[]` lesen |

**Einschränkungen in v0.1.0:**

- Genau ein `Client` oder `Server` pro PHP-Prozess (prozess-globaler UDP-Socket).
- `whoIs()` sammelt maximal 64 Geräte pro Aufruf.
- Nur NTS (Non-Thread-Safe) — kein ZTS/FPM-Thread-Mode.
- `writeWeeklySchedule()` ist noch nicht implementiert.

---

## 2. Installation

```bash
# 1. Submodul initialisieren (einmalig)
git submodule update --init --recursive

# 2. bacnet-stack als statische Bibliothek bauen
./scripts/build-deps.sh

# 3. Erweiterung kompilieren
phpize8.5
./configure --with-bacnet --with-php-config=php-config8.5
make -j$(nproc)

# 4. Laden prüfen
php -d extension=modules/bacnet.so -m | grep bacnet

# 5. Dauerhaft aktivieren (php.ini oder conf.d)
echo "extension=bacnet.so" | sudo tee /etc/php/8.5/cli/conf.d/30-bacnet.ini
```

**Voraussetzungen:** PHP 8.4 oder 8.5 NTS, `php-dev`, `build-essential`, `cmake`, Linux (GCC/Autotools).

---

## 3. Konfiguration (php.ini)

Alle Einstellungen gelten per Prozess und können zur Laufzeit via `ini_set()` geändert werden
(`PHP_INI_ALL`).

| Direktive | Standard | Beschreibung |
|-----------|---------|-------------|
| `bacnet.default_port` | `47808` | UDP-Port für den BACnet/IP-Socket. Standard-BACnet-Port ist `0xBAC0` = 47808. |
| `bacnet.default_timeout_ms` | `3000` | Wartezeit für Einzelanfragen in Millisekunden. |
| `bacnet.default_interface` | `"0.0.0.0"` | Netzwerk-Interface-Name (z. B. `"eth0"`). `"0.0.0.0"` bedeutet Auto-Erkennung. |

```ini
; /etc/php/8.5/cli/conf.d/30-bacnet.ini
extension=bacnet.so
bacnet.default_port       = 47808
bacnet.default_timeout_ms = 3000
bacnet.default_interface  = 0.0.0.0
```

> **Hinweis:** `bip_init()` erwartet einen Interface-*Namen* (z. B. `"eth0"`), keine IP-Adresse.
> Das Übergeben von `"0.0.0.0"` führt zu einer automatischen Erkennung des ersten
> Nicht-Loopback-Interfaces.

---

## 4. Typzuordnung — BACnet ↔ PHP

Beim Lesen von Eigenschaften (`readProperty`) gibt die Erweiterung PHP-Werte zurück,
die dem BACnet-Anwendungsdaten-Tag entsprechen:

| BACnet-Tag | PHP-Typ | Beispielwert |
|-----------|---------|-------------|
| `NULL` | `null` | `null` |
| `BOOLEAN` | `bool` | `true` |
| `UNSIGNED INT` | `int` | `42` |
| `SIGNED INT` | `int` | `-5` |
| `REAL` | `float` | `21.5` |
| `DOUBLE` | `float` | `21.5` |
| `ENUMERATED` | `int` | `1` (= ACTIVE bei BinaryValue) |
| `CHARACTER STRING` | `string` | `"Temperatur-Sensor"` |
| `BIT STRING` | `Bacnet\BitString` | Objekt mit 4 Bits |
| `DATE` | `Bacnet\Date` | Objekt mit Jahr/Monat/Tag/Wochentag |
| `TIME` | `Bacnet\Time` | Objekt mit Stunde/Minute/Sekunde/Hundertstel |
| `OBJECT IDENTIFIER` | `Bacnet\ObjectIdentifier` | Objekt mit Typ und Instanz |
| Array (`?arrayIndex`) | `array` | Indexed Array mit den Elementen |

---

## 5. Klassen

---

### Bacnet\Client

```
Bacnet\Client
```

Verwaltet einen UDP-BACnet/IP-Socket für den **Client-Modus** (Geräteerkennung,
ReadProperty, WriteProperty). Pro PHP-Prozess darf nur eine Instanz von `Client`
oder `Server` existieren.

---

#### Bacnet\Client::__construct()

```php
public function __construct(
    ?string $interface = null,
    ?int    $port      = null,
    ?int    $timeoutMs = null,
): void
```

Initialisiert den BACnet/IP-Stack und öffnet den UDP-Socket.

**Parameter**

| Parameter | Typ | Beschreibung |
|-----------|-----|-------------|
| `$interface` | `?string` | Interface-Name (z. B. `"eth0"`, `"wlp3s0"`). `null` oder `"0.0.0.0"` für Auto-Erkennung. |
| `$port` | `?int` | Lokaler UDP-Port. `null` verwendet `bacnet.default_port` (Standard: 47808). |
| `$timeoutMs` | `?int` | Standard-Timeout für alle folgenden Anfragen in ms. `null` verwendet `bacnet.default_timeout_ms`. |

**Rückgabewert**

Es wird kein Wert zurückgegeben.

**Fehler**

| Ausnahme | Bedingung |
|----------|-----------|
| `\Error` | Es existiert bereits ein `Client` oder `Server` in diesem Prozess. |
| `Bacnet\Exception` | `bip_init()` schlägt fehl (falsches Interface, Port bereits belegt). |

**Hinweise**

- Der Konstruktor setzt intern einen Singleton-Guard. Innerhalb desselben
  PHP-Prozesses kann kein zweites `Client`- oder `Server`-Objekt angelegt werden.
- In PHP-FPM wird der Guard pro Request zurückgesetzt (via `RINIT`), sodass jeder
  Worker-Prozess genau einen Client pro Request erstellen kann.
- Broadcasts werden immer an Port 47808 (Standard-BACnet-Port) gesendet,
  unabhängig vom eigenen `$port`.

**Beispiel**

```php
<?php
// Auto-Erkennung, Standard-Port, Standard-Timeout
$client = new Bacnet\Client();

// Explizites Interface, alternativer Port (z. B. um Konflikt mit laufendem Server zu vermeiden)
$client = new Bacnet\Client('eth0', 47809, 5000);

// Per INI konfigurierter Interface-Standard überschreiben
ini_set('bacnet.default_timeout_ms', '1000');
$client = new Bacnet\Client('wlp3s0');
```

---

#### Bacnet\Client::whoIs()

```php
public function whoIs(
    ?int $lowLimit  = null,
    ?int $highLimit = null,
    ?int $timeoutMs = null,
): array
```

Sendet einen **Who-Is**-Broadcast und sammelt alle eingehenden **I-Am**-Antworten.

**Parameter**

| Parameter | Typ | Beschreibung |
|-----------|-----|-------------|
| `$lowLimit` | `?int` | Untere Gerätinstanz-Grenze (0 .. 4194302). `null` = 0. |
| `$highLimit` | `?int` | Obere Gerätinstanz-Grenze (0 .. 4194302). `null` = 4194302. |
| `$timeoutMs` | `?int` | Wartezeit auf Antworten in ms. `null` verwendet `bacnet.default_timeout_ms`. |

**Rückgabewert**

`Bacnet\Device[]` — Array mit allen gefundenen Geräten. Leeres Array, wenn kein Gerät antwortet.

> **Hinweis:** Es werden maximal 64 Geräte pro Aufruf gesammelt (`BACNET_MAX_COLLECTED_DEVICES`).

**Fehler**

| Ausnahme | Bedingung |
|----------|-----------|
| `Bacnet\Exception` | Der Client ist nicht initialisiert oder die APDU-Kodierung schlägt fehl. |

**Hinweise**

- `whoIs()` wartet immer die volle `$timeoutMs`-Zeit, auch wenn Geräte früher antworten,
  um alle Geräte im Netzwerk zu erfassen.
- Für eine gezielte Suche nach einem einzelnen Gerät können `$lowLimit` und `$highLimit`
  auf dieselbe Geräteinstanz gesetzt werden.
- Duplikate (mehrere I-Am vom selben Gerät) werden automatisch gefiltert.

**Beispiel**

```php
<?php
$client = new Bacnet\Client('eth0');

// Alle Geräte im Netzwerk finden
$allDevices = $client->whoIs();
foreach ($allDevices as $device) {
    printf("Gerät %d @ %s\n", $device->getDeviceId(), $device->getAddress());
}

// Gezielt ein einzelnes Gerät suchen
$devices = $client->whoIs(lowLimit: 1234, highLimit: 1234, timeoutMs: 2000);
if (empty($devices)) {
    throw new RuntimeException('Gerät 1234 nicht gefunden');
}
$device = $devices[0];
```

---

### Bacnet\Device

```
Bacnet\Device
```

Repräsentiert ein entdecktes BACnet-Gerät. Instanzen werden ausschließlich
von `Client::whoIs()` erstellt. Ein `Device`-Objekt enthält die Netzwerkadresse
des Geräts und eine Referenz auf den erzeugenden `Client`.

---

#### Bacnet\Device::getDeviceId()

```php
public function getDeviceId(): int
```

Gibt die BACnet-Geräteinstanznummer zurück (0 .. 4194302).

---

#### Bacnet\Device::getAddress()

```php
public function getAddress(): string
```

Gibt die IP-Adresse des Geräts zurück (Dotted-Notation, z. B. `"192.168.1.100"`).

---

#### Bacnet\Device::getMaxApdu()

```php
public function getMaxApdu(): int
```

Gibt die maximale APDU-Länge zurück, die das Gerät akzeptiert (in Bytes, aus dem I-Am-PDU).
Typische Werte: 480, 1024, 1476.

---

#### Bacnet\Device::getVendorId()

```php
public function getVendorId(): int
```

Gibt die BACnet-Hersteller-ID zurück (aus dem I-Am-PDU, z. B. 260 für Siemens).

---

#### Bacnet\Device::readProperty()

```php
public function readProperty(
    Bacnet\ObjectType $objectType,
    int               $instance,
    Bacnet\Property   $property,
    ?int              $arrayIndex = null,
): mixed
```

Liest eine einzelne Eigenschaft eines BACnet-Objekts von diesem Gerät (unicast ReadProperty-Request).

**Parameter**

| Parameter | Typ | Beschreibung |
|-----------|-----|-------------|
| `$objectType` | `Bacnet\ObjectType` | Typ des Objekts (z. B. `ObjectType::ANALOG_VALUE`). |
| `$instance` | `int` | Instanznummer des Objekts (0 .. 4194302). |
| `$property` | `Bacnet\Property` | Zu lesende Eigenschaft (z. B. `Property::PRESENT_VALUE`). |
| `$arrayIndex` | `?int` | Array-Index für Array-Eigenschaften (z. B. OBJECT_LIST). `null` liest das gesamte Array. |

**Rückgabewert**

PHP-Wert entsprechend der Typzuordnung (s. [Abschnitt 4](#4-typzuordnung--bacnet--php)).

**Fehler**

| Ausnahme | Bedingung |
|----------|-----------|
| `Bacnet\TimeoutException` | Keine Antwort innerhalb des konfigurierten Timeouts. |
| `Bacnet\DeviceException` | Das Gerät hat mit einem BACnet-Fehler geantwortet (Error-PDU). `$e->errorClass` und `$e->errorCode` enthalten die Fehlerdetails. |
| `Bacnet\Exception` | Interne Fehler (Client nicht initialisiert, APDU-Kodierung). |

**Beispiel**

```php
<?php
$client = new Bacnet\Client('eth0');
[$device] = $client->whoIs(1234, 1234);

// Einfacher Wert lesen
$temp = $device->readProperty(Bacnet\ObjectType::ANALOG_VALUE, 1, Bacnet\Property::PRESENT_VALUE);
echo "Temperatur: $temp °C\n"; // z. B. 21.5

// Gerätename lesen
$name = $device->readProperty(Bacnet\ObjectType::DEVICE, 1234, Bacnet\Property::OBJECT_NAME);
echo "Gerätename: $name\n";

// Array-Eigenschaft: drittes Element der Object-Liste lesen
$thirdObject = $device->readProperty(
    Bacnet\ObjectType::DEVICE, 1234, Bacnet\Property::OBJECT_LIST, arrayIndex: 3
);

// Status-Flags als BitString
$flags = $device->readProperty(Bacnet\ObjectType::ANALOG_VALUE, 1, Bacnet\Property::STATUS_FLAGS);
if ($flags instanceof Bacnet\BitString) {
    $inAlarm    = $flags->getBit(0);
    $fault      = $flags->getBit(1);
    $overridden = $flags->getBit(2);
    $outOfSvc   = $flags->getBit(3);
}

// Fehlerbehandlung
try {
    $value = $device->readProperty(Bacnet\ObjectType::ANALOG_VALUE, 999, Bacnet\Property::PRESENT_VALUE);
} catch (Bacnet\TimeoutException) {
    echo "Objekt nicht erreichbar\n";
} catch (Bacnet\DeviceException $e) {
    printf("BACnet-Fehler: Klasse=%d Code=%d\n", $e->errorClass, $e->errorCode);
}
```

---

#### Bacnet\Device::writeProperty()

```php
public function writeProperty(
    Bacnet\ObjectType $objectType,
    int               $instance,
    Bacnet\Property   $property,
    Bacnet\Value      $value,
    int               $priority   = 16,
    ?int              $arrayIndex = null,
): void
```

Schreibt eine Eigenschaft eines BACnet-Objekts (unicast WriteProperty-Request).

**Parameter**

| Parameter | Typ | Beschreibung |
|-----------|-----|-------------|
| `$objectType` | `Bacnet\ObjectType` | Typ des Ziel-Objekts. |
| `$instance` | `int` | Instanznummer des Ziel-Objekts. |
| `$property` | `Bacnet\Property` | Zu schreibende Eigenschaft. |
| `$value` | `Bacnet\Value` | Zu schreibender Wert (erzeugt via Factory-Methoden von `Value`). |
| `$priority` | `int` | Schreibpriorität 1 (höchste) bis 16 (niedrigste, Standard). Ignoriert für Objekte ohne Priority-Array. |
| `$arrayIndex` | `?int` | Array-Index, wenn nur ein Element geschrieben werden soll. |

**Rückgabewert**

Es wird kein Wert zurückgegeben.

**Fehler**

| Ausnahme | Bedingung |
|----------|-----------|
| `Bacnet\TimeoutException` | Keine Antwort (Simple-ACK oder Error) innerhalb des Timeouts. |
| `Bacnet\DeviceException` | Das Gerät hat mit einem BACnet-Fehler geantwortet (z. B. `WRITE_ACCESS_DENIED`, Klasse=2 Code=40). |
| `Bacnet\Exception` | Interne Fehler. |

**BACnet-Schreibprioritäten (BACnet/1-16)**

| Priorität | Bedeutung |
|-----------|-----------|
| 1 | Manual-Life Safety |
| 2 | Automatic-Life Safety |
| 3–7 | (reserviert) |
| 8 | Manual Operator |
| 9–15 | (Anwendungsdefiniert) |
| 16 | Available (niedrigste, Standardwert) |

**Beispiel**

```php
<?php
$client = new Bacnet\Client('eth0');
[$device] = $client->whoIs(1234, 1234);

// Analogwert setzen
$device->writeProperty(
    Bacnet\ObjectType::ANALOG_VALUE,
    1,
    Bacnet\Property::PRESENT_VALUE,
    Bacnet\Value::real(22.5),
    priority: 16,
);

// Binärausgang mit Priorität 8 (Manual Operator) steuern
$device->writeProperty(
    Bacnet\ObjectType::BINARY_OUTPUT,
    1,
    Bacnet\Property::PRESENT_VALUE,
    Bacnet\Value::enumerated(1), // 1 = ACTIVE
    priority: 8,
);

// Zeichenkette schreiben
$device->writeProperty(
    Bacnet\ObjectType::ANALOG_VALUE,
    1,
    Bacnet\Property::DESCRIPTION,
    Bacnet\Value::characterString('Außentemperatur'),
);
```

---

### Bacnet\ObjectRef

```
Bacnet\ObjectRef
```

Bündelt `Device` + `ObjectType` + `instance` für komfortablen wiederholten Zugriff
auf dasselbe BACnet-Objekt. Alle Methoden delegieren intern an `Device::readProperty()`
bzw. `Device::writeProperty()`.

---

#### Bacnet\ObjectRef::__construct()

```php
public function __construct(
    Bacnet\Device     $device,
    Bacnet\ObjectType $type,
    int               $instance,
): void
```

**Parameter**

| Parameter | Typ | Beschreibung |
|-----------|-----|-------------|
| `$device` | `Bacnet\Device` | Das Gerät, auf dem das Objekt liegt. |
| `$type` | `Bacnet\ObjectType` | Typ des BACnet-Objekts. |
| `$instance` | `int` | Instanznummer (0 .. 4194302). |

---

#### Bacnet\ObjectRef::readProperty()

```php
public function readProperty(
    Bacnet\Property $property,
    ?int            $arrayIndex = null,
): mixed
```

Entspricht `Device::readProperty()`, ohne Objekt-Typ und Instanz angeben zu müssen.

**Fehler:** `Bacnet\TimeoutException`, `Bacnet\DeviceException`, `Bacnet\Exception`

---

#### Bacnet\ObjectRef::writeProperty()

```php
public function writeProperty(
    Bacnet\Property $property,
    Bacnet\Value    $value,
    int             $priority   = 16,
    ?int            $arrayIndex = null,
): void
```

Entspricht `Device::writeProperty()`, ohne Objekt-Typ und Instanz angeben zu müssen.

**Fehler:** `Bacnet\TimeoutException`, `Bacnet\DeviceException`, `Bacnet\Exception`

---

#### Bacnet\ObjectRef::writePresentValue()

```php
public function writePresentValue(mixed $value): void
```

Schreibt `PRESENT_VALUE` mit automatischer Typerkennung. Akzeptiert direkt PHP-Werte
oder eine `Bacnet\Value`-Instanz.

**Typzuordnung** (bei nativen PHP-Typen):

| PHP-Typ | Objekt-Typ | BACnet-Tag |
|---------|-----------|-----------|
| `float` | beliebig | REAL |
| `int` | `BINARY_*` | ENUMERATED |
| `int` | `ANALOG_*` | REAL (cast) |
| `int` | andere | UNSIGNED INT |
| `bool` | beliebig | BOOLEAN |
| `string` | beliebig | CHARACTER STRING |
| `Bacnet\Value` | beliebig | Tag des Value-Objekts |

**Priorität:** immer 16 (niedrigste).

**Fehler:** `Bacnet\TimeoutException`, `Bacnet\DeviceException`, `Bacnet\Exception`

**Beispiel**

```php
<?php
$av = new Bacnet\ObjectRef($device, Bacnet\ObjectType::ANALOG_VALUE, 1);
$bv = new Bacnet\ObjectRef($device, Bacnet\ObjectType::BINARY_VALUE, 1);

$av->writePresentValue(23.5);  // → REAL
$bv->writePresentValue(1);     // → ENUMERATED (da BINARY_VALUE)
$bv->writePresentValue(true);  // → BOOLEAN
```

---

#### Bacnet\ObjectRef::writeActive()

```php
public function writeActive(): void
```

Schreibt `PRESENT_VALUE = ENUMERATED(1)` (= ACTIVE) mit Priorität 16.
Kurzform für Binär-Objekte.

---

#### Bacnet\ObjectRef::writeInactive()

```php
public function writeInactive(): void
```

Schreibt `PRESENT_VALUE = ENUMERATED(0)` (= INACTIVE) mit Priorität 16.

---

#### Bacnet\ObjectRef::readTrendLog()

```php
public function readTrendLog(): array
```

Liest `LOG_BUFFER` und gibt die dekodierten Einträge als `Bacnet\TrendLogRecord[]` zurück.
Geeignet für `TREND_LOG`-Objekte.

**Rückgabewert:** `Bacnet\TrendLogRecord[]`

**Fehler:** `Bacnet\TimeoutException`, `Bacnet\DeviceException`, `Bacnet\Exception`

---

#### Bacnet\ObjectRef::readWeeklySchedule()

```php
public function readWeeklySchedule(): Bacnet\WeeklySchedule
```

Liest `WEEKLY_SCHEDULE` von einem `SCHEDULE`-Objekt.
In v0.1.0 gibt die Methode ein leeres `WeeklySchedule`-Objekt zurück
(vollständige Sequenz-Dekodierung ist für v0.2.0 geplant).

**Rückgabewert:** `Bacnet\WeeklySchedule`

---

#### Bacnet\ObjectRef::writeWeeklySchedule()

```php
public function writeWeeklySchedule(Bacnet\WeeklySchedule $schedule): void
```

> **Hinweis:** Diese Methode ist in v0.1.0 noch nicht implementiert und wirft eine `Bacnet\Exception`.

---

### Bacnet\Server

```
Bacnet\Server
```

Stellt einen eigenen BACnet/IP-Knoten bereit. Der Server empfängt eingehende
BACnet-Anfragen und ruft registrierte PHP-Callbacks auf.
Wie `Client` verwendet er einen prozess-globalen UDP-Socket.

---

#### Bacnet\Server::__construct()

```php
public function __construct(
    int    $deviceId,
    string $bindInterface = '0.0.0.0',
    int    $port          = 47808,
): void
```

**Parameter**

| Parameter | Typ | Beschreibung |
|-----------|-----|-------------|
| `$deviceId` | `int` | BACnet-Geräteinstanznummer für diesen Server (0 .. 4194302). |
| `$bindInterface` | `string` | Interface-Name oder `"0.0.0.0"` für Auto-Erkennung. |
| `$port` | `int` | UDP-Port (Standard: 47808). |

**Fehler**

| Ausnahme | Bedingung |
|----------|-----------|
| `\Error` | Es existiert bereits ein `Client` oder `Server` in diesem Prozess. |
| `Bacnet\Exception` | Initialisierung fehlgeschlagen. |

---

#### Bacnet\Server::addLocalObject()

```php
public function addLocalObject(Bacnet\ObjectIdentifier $oid): void
```

Registriert ein lokales BACnet-Objekt. ReadProperty- und WriteProperty-Requests
für dieses Objekt werden an die installierten Callbacks weitergeleitet.

**Beispiel**

```php
<?php
$server = new Bacnet\Server(deviceId: 9001, bindInterface: 'eth0');
$server->addLocalObject(new Bacnet\ObjectIdentifier(Bacnet\ObjectType::ANALOG_VALUE, 1));
$server->addLocalObject(new Bacnet\ObjectIdentifier(Bacnet\ObjectType::BINARY_VALUE, 1));
```

---

#### Bacnet\Server::removeLocalObject()

```php
public function removeLocalObject(Bacnet\ObjectIdentifier $oid): void
```

Entfernt ein zuvor registriertes lokales Objekt.

---

#### Bacnet\Server::onReadProperty()

```php
public function onReadProperty(callable $handler): void
```

Registriert einen Callback für eingehende ReadProperty-Anfragen.

**Callback-Signatur:**

```php
function(
    Bacnet\ObjectIdentifier $oid,
    Bacnet\Property|int     $property,
    ?int                    $arrayIndex,
): mixed
```

Der Rückgabewert wird als BACnet-Anwendungsdatenwert kodiert und als
ReadProperty-ACK zurückgesendet. Akzeptierte Typen: `bool`, `int`, `float`,
`string`, `null`, `Bacnet\Value`, `Bacnet\BitString`, `Bacnet\Date`,
`Bacnet\Time`, `Bacnet\ObjectIdentifier`.

**Beispiel**

```php
<?php
$values = [1 => 21.5, 2 => 19.0];

$server->onReadProperty(function (
    Bacnet\ObjectIdentifier $oid,
    Bacnet\Property|int     $property,
    ?int                    $arrayIndex,
) use (&$values): mixed {
    if ($oid->getInstance() === 1 && $property === Bacnet\Property::PRESENT_VALUE) {
        return $values[$oid->getInstance()] ?? 0.0;
    }
    if ($property === Bacnet\Property::OBJECT_NAME) {
        return "Sensor-{$oid->getInstance()}";
    }
    return null;
});
```

---

#### Bacnet\Server::onWriteProperty()

```php
public function onWriteProperty(callable $handler): void
```

Registriert einen Callback für eingehende WriteProperty-Anfragen.

**Callback-Signatur:**

```php
function(
    Bacnet\ObjectIdentifier $oid,
    Bacnet\Property|int     $property,
    mixed                   $value,
    ?int                    $arrayIndex,
): void
```

**Beispiel**

```php
<?php
$server->onWriteProperty(function (
    Bacnet\ObjectIdentifier $oid,
    Bacnet\Property|int     $property,
    mixed                   $value,
    ?int                    $arrayIndex,
) use (&$values): void {
    if ($property === Bacnet\Property::PRESENT_VALUE) {
        $values[$oid->getInstance()] = $value;
        echo "Schreibe [{$oid}] = $value\n";
    }
});
```

---

#### Bacnet\Server::setAutoIAm()

```php
public function setAutoIAm(bool $enabled): void
```

Aktiviert oder deaktiviert automatische I-Am-Antworten auf Who-Is-Broadcasts.
Standardmäßig aktiviert.

---

#### Bacnet\Server::poll()

```php
public function poll(int $timeoutMs = 0): void
```

Verarbeitet maximal ein eingehendes PDU vom Socket und ruft ggf. den passenden Callback auf.

**Parameter**

| Parameter | Typ | Beschreibung |
|-----------|-----|-------------|
| `$timeoutMs` | `int` | Wartezeit in ms. `0` = nicht-blockierend (sofortige Rückkehr wenn nichts eingeht). |

**Typisches Event-Loop-Muster:**

```php
<?php
$server = new Bacnet\Server(deviceId: 9001, bindInterface: 'eth0');
$server->addLocalObject(new Bacnet\ObjectIdentifier(Bacnet\ObjectType::ANALOG_VALUE, 1));

$server->onReadProperty(function ($oid, $property, $arrayIndex) {
    return match ($property) {
        Bacnet\Property::PRESENT_VALUE => 42.0,
        Bacnet\Property::OBJECT_NAME   => 'Mein Sensor',
        default                         => null,
    };
});

// Event-Loop
while (true) {
    $server->poll(timeoutMs: 100);  // 100 ms warten auf PDU
    // Eigene Logik (Sensorwerte aktualisieren etc.)
}
```

---

### Bacnet\Value

```
final class Bacnet\Value
```

Unveränderliches Wrapper-Objekt für einen typisierten BACnet-Anwendungsdatenwert.
Alle Instanzen werden über statische Factory-Methoden erzeugt.
Verwendet bei `Device::writeProperty()` und `ObjectRef::writeProperty()`.

---

#### Bacnet\Value::boolean()

```php
public static function boolean(bool $value): Bacnet\Value
```

Erzeugt einen `BOOLEAN`-Wert.

---

#### Bacnet\Value::unsignedInt()

```php
public static function unsignedInt(int $value): Bacnet\Value
```

Erzeugt einen `UNSIGNED INT`-Wert (0 .. 4294967295).
Für Zähler, Instanznummern usw.

---

#### Bacnet\Value::signedInt()

```php
public static function signedInt(int $value): Bacnet\Value
```

Erzeugt einen `SIGNED INT`-Wert (−2147483648 .. 2147483647).

---

#### Bacnet\Value::real()

```php
public static function real(float $value): Bacnet\Value
```

Erzeugt einen `REAL`-Wert (32-Bit IEEE 754). Für Analog-Messwerte, Sollwerte usw.

---

#### Bacnet\Value::enumerated()

```php
public static function enumerated(int $value): Bacnet\Value
```

Erzeugt einen `ENUMERATED`-Wert. Für Binär-Objekte (0 = INACTIVE, 1 = ACTIVE),
Multi-State-Objekte und andere Aufzählungstypen.

---

#### Bacnet\Value::characterString()

```php
public static function characterString(string $value): Bacnet\Value
```

Erzeugt einen `CHARACTER STRING`-Wert (UTF-8 / ANSI X3.4).

---

#### Bacnet\Value::bitString()

```php
public static function bitString(Bacnet\BitString $value): Bacnet\Value
```

Erzeugt einen `BIT STRING`-Wert aus einem `BitString`-Objekt.

---

#### Bacnet\Value::date()

```php
public static function date(Bacnet\Date $value): Bacnet\Value
```

Erzeugt einen `DATE`-Wert.

---

#### Bacnet\Value::time()

```php
public static function time(Bacnet\Time $value): Bacnet\Value
```

Erzeugt einen `TIME`-Wert.

---

#### Bacnet\Value::objectIdentifier()

```php
public static function objectIdentifier(Bacnet\ObjectIdentifier $value): Bacnet\Value
```

Erzeugt einen `OBJECT IDENTIFIER`-Wert.

**Vollständiges Beispiel:**

```php
<?php
// Analogwert schreiben
$device->writeProperty($ot, 1, $prop, Bacnet\Value::real(21.5));

// Binärwert: ACTIVE
$device->writeProperty($ot, 1, $prop, Bacnet\Value::enumerated(1));

// Zeichenkette schreiben
$device->writeProperty($ot, 1, $prop, Bacnet\Value::characterString('Heizung'));

// Datum schreiben
$device->writeProperty($ot, 1, $prop,
    Bacnet\Value::date(new Bacnet\Date(2026, 6, 30, 2)) // 2026-06-30, Di
);
```

---

### Bacnet\BitString

```
final class Bacnet\BitString
```

Repräsentiert einen BACnet `BIT STRING`. Wird typischerweise für `STATUS_FLAGS`
(Bit 0 = in-alarm, Bit 1 = fault, Bit 2 = overridden, Bit 3 = out-of-service) und
ähnliche Eigenschaften verwendet.

---

#### Bacnet\BitString::__construct()

```php
public function __construct(array $bits): void
```

**Parameter**

| Parameter | Typ | Beschreibung |
|-----------|-----|-------------|
| `$bits` | `bool[]` | Array von Bits (MSB zuerst). |

**Beispiel:** `new BitString([true, false, false, false])` = 0b1000 (Bit 0 = 1, Bits 1-3 = 0)

---

#### Bacnet\BitString::getBit()

```php
public function getBit(int $index): bool
```

Gibt den Wert eines einzelnen Bits zurück (0-basierter Index, MSB zuerst).

**Fehler:** Wirft `\ValueError` bei ungültigem Index.

---

#### Bacnet\BitString::getLength()

```php
public function getLength(): int
```

Gibt die Anzahl der verwendeten Bits zurück.

---

#### Bacnet\BitString::toArray()

```php
public function toArray(): array
```

Gibt alle Bits als `bool[]`-Array zurück.

**Beispiel**

```php
<?php
$flags = $device->readProperty(
    Bacnet\ObjectType::ANALOG_VALUE, 1, Bacnet\Property::STATUS_FLAGS
);

if ($flags instanceof Bacnet\BitString) {
    [$inAlarm, $fault, $overridden, $outOfService] = $flags->toArray();

    if ($inAlarm)      echo "⚠ Alarm aktiv\n";
    if ($fault)        echo "⚠ Fehler vorhanden\n";
    if ($outOfService) echo "ℹ Außer Betrieb\n";
}
```

---

### Bacnet\Date

```
final class Bacnet\Date
```

Repräsentiert einen BACnet `DATE`-Wert.

---

#### Bacnet\Date::__construct()

```php
public function __construct(
    int $year,
    int $month,
    int $day,
    int $weekday,
): void
```

**Parameter**

| Parameter | Typ | Beschreibung |
|-----------|-----|-------------|
| `$year` | `int` | Jahreszahl (n. Chr.), z. B. 2026. `0xFF` = Wildcard. |
| `$month` | `int` | Monat 1..12. `0xFF` = Wildcard. |
| `$day` | `int` | Tag 1..31. `0xFF` = Wildcard. |
| `$weekday` | `int` | Wochentag 1=Mo .. 7=So. `0xFF` = Wildcard. |

**Eigenschaften:** `$year`, `$month`, `$day`, `$weekday` (lesbar, nicht schreibbar)

**Beispiel**

```php
<?php
$date = new Bacnet\Date(2026, 6, 30, 2); // 30. Juni 2026, Dienstag
$any  = new Bacnet\Date(0xFF, 0xFF, 0xFF, 0xFF); // Wildcard-Datum
```

---

### Bacnet\Time

```
final class Bacnet\Time
```

Repräsentiert einen BACnet `TIME`-Wert.

---

#### Bacnet\Time::__construct()

```php
public function __construct(
    int $hour,
    int $minute,
    int $second,
    int $hundredths,
): void
```

**Parameter**

| Parameter | Typ | Beschreibung |
|-----------|-----|-------------|
| `$hour` | `int` | Stunde 0..23. `0xFF` = Wildcard. |
| `$minute` | `int` | Minute 0..59. `0xFF` = Wildcard. |
| `$second` | `int` | Sekunde 0..59. `0xFF` = Wildcard. |
| `$hundredths` | `int` | Hundertstel 0..99. `0xFF` = Wildcard. |

**Eigenschaften:** `$hour`, `$minute`, `$second`, `$hundredths` (lesbar, nicht schreibbar)

**Beispiel**

```php
<?php
$t    = new Bacnet\Time(14, 30, 0, 0);   // 14:30:00.00
$any  = new Bacnet\Time(0xFF, 0xFF, 0xFF, 0xFF); // Wildcard
```

---

### Bacnet\ObjectIdentifier

```
final class Bacnet\ObjectIdentifier
```

Repräsentiert ein BACnet `OBJECT IDENTIFIER`-Wertpaar aus `ObjectType` und Instanznummer.

---

#### Bacnet\ObjectIdentifier::__construct()

```php
public function __construct(
    Bacnet\ObjectType $type,
    int               $instance,
): void
```

**Fehler:** `Bacnet\Exception` wenn `$instance` außerhalb 0..4194302.

---

#### Bacnet\ObjectIdentifier::getType()

```php
public function getType(): Bacnet\ObjectType
```

---

#### Bacnet\ObjectIdentifier::getInstance()

```php
public function getInstance(): int
```

---

#### Bacnet\ObjectIdentifier::__toString()

```php
public function __toString(): string
```

Gibt eine lesbare Darstellung zurück, z. B. `"ANALOG_VALUE:1"` oder `"8:1234"`.

**Eigenschaften:** `$type` (`?Bacnet\ObjectType`), `$instance` (`int`) — lesbar

**Beispiel**

```php
<?php
$oid = new Bacnet\ObjectIdentifier(Bacnet\ObjectType::ANALOG_VALUE, 1);
echo $oid;                    // "ANALOG_VALUE:1"
echo $oid->getInstance();     // 1
echo $oid->getType()->name;   // "ANALOG_VALUE"
echo $oid->getType()->value;  // 2
```

---

### Bacnet\ScheduleEntry

```
class Bacnet\ScheduleEntry
```

Repräsentiert einen Zeitplan-Eintrag (TimeValue) innerhalb eines BACnet `DailySchedule`.

---

#### Bacnet\ScheduleEntry::__construct()

```php
public function __construct(Bacnet\Time $startTime, mixed $value): void
```

| Eigenschaft | Typ | Beschreibung |
|------------|-----|-------------|
| `$startTime` | `Bacnet\Time` | Uhrzeit, ab der der Wert gilt. |
| `$value` | `mixed` | Zu setzender Wert (gleiche Typen wie bei `readProperty`). |

---

#### Bacnet\ScheduleEntry::getStartTime()

```php
public function getStartTime(): Bacnet\Time
```

---

#### Bacnet\ScheduleEntry::getValue()

```php
public function getValue(): mixed
```

---

### Bacnet\WeeklySchedule

```
class Bacnet\WeeklySchedule
```

Enthält sieben `ScheduleEntry[]`-Arrays (einen pro Wochentag) für ein BACnet `WEEKLY_SCHEDULE`.

---

#### Bacnet\WeeklySchedule::__construct()

```php
public function __construct(
    array $monday    = [],
    array $tuesday   = [],
    array $wednesday = [],
    array $thursday  = [],
    array $friday    = [],
    array $saturday  = [],
    array $sunday    = [],
): void
```

Jedes Array enthält `Bacnet\ScheduleEntry`-Objekte, sortiert nach Startzeit (aufsteigend).

---

#### Bacnet\WeeklySchedule::getDay()

```php
public function getDay(int $weekday): array
```

Gibt das Tages-Array zurück. `$weekday`: 1 = Montag … 7 = Sonntag.

**Rückgabewert:** `Bacnet\ScheduleEntry[]`

**Eigenschaften:** `$monday`, `$tuesday`, `$wednesday`, `$thursday`, `$friday`, `$saturday`, `$sunday`

---

### Bacnet\TrendLogRecord

```
class Bacnet\TrendLogRecord
```

Ein dekodierter Eintrag aus einem BACnet TrendLog (`LOG_BUFFER`).

| Eigenschaft | Typ | Beschreibung |
|------------|-----|-------------|
| `$timestamp` | `mixed` | Zeitstempel des Eintrags (Dekodierung je nach Geräteformat). |
| `$value` | `mixed` | Aufgezeichneter Wert. |
| `$statusFlags` | `int` | Bit-kodierte Statusflags (0 = normal). |

---

## 6. Enumeratoren

---

### Bacnet\ObjectType

```
enum Bacnet\ObjectType: int
```

BACnet-Objekttypen (Subset; Standardwerte gem. ASHRAE 135-2020).

| Case | Wert | Beschreibung |
|------|------|-------------|
| `ANALOG_INPUT` | 0 | Analoger Eingang (Messwert) |
| `ANALOG_OUTPUT` | 1 | Analoger Ausgang (Steuerbefehl) |
| `ANALOG_VALUE` | 2 | Analoger Softwarewert |
| `BINARY_INPUT` | 3 | Binärer Eingang |
| `BINARY_OUTPUT` | 4 | Binärer Ausgang |
| `BINARY_VALUE` | 5 | Binärer Softwarewert |
| `DEVICE` | 8 | Geräteobjekt (jedes Gerät hat genau eines) |
| `EVENT_ENROLLMENT` | 9 | Ereignisregistrierung |
| `MULTI_STATE_INPUT` | 13 | Mehrwertiger Eingang |
| `MULTI_STATE_OUTPUT` | 14 | Mehrwertiger Ausgang |
| `NOTIFICATION_CLASS` | 15 | Benachrichtigungsklasse |
| `SCHEDULE` | 17 | Zeitprogramm |
| `MULTI_STATE_VALUE` | 19 | Mehrwertiger Softwarewert |
| `TREND_LOG` | 20 | Aufzeichnungsobjekt |

---

### Bacnet\Property

```
enum Bacnet\Property: int
```

BACnet-Eigenschaften (Subset; Standardwerte gem. ASHRAE 135-2020).

| Case | Wert | Häufige Verwendung |
|------|------|--------------------|
| `OBJECT_IDENTIFIER` | 75 | Eindeutige Kennung des Objekts |
| `OBJECT_NAME` | 77 | Lesbarer Name des Objekts |
| `OBJECT_TYPE` | 79 | Typ des Objekts |
| `DESCRIPTION` | 28 | Freitext-Beschreibung |
| `PRESENT_VALUE` | 85 | Aktueller Wert (lesen/schreiben) |
| `STATUS_FLAGS` | 111 | BitString: in-alarm/fault/overridden/out-of-service |
| `EVENT_STATE` | 36 | Alarm-/Normalstatus |
| `RELIABILITY` | 103 | Zuverlässigkeitsstatus |
| `OUT_OF_SERVICE` | 81 | Objekt außer Betrieb (Bool) |
| `UNITS` | 117 | Maßeinheit (Enumerationswert) |
| `PRIORITY_ARRAY` | 87 | Prioritätsfelder (16 Einträge) |
| `RELINQUISH_DEFAULT` | 104 | Rückfallwert nach Freigabe |
| `OBJECT_LIST` | 76 | Liste aller Objekte (nur Device) |
| `WEEKLY_SCHEDULE` | 123 | Wochenplan (SCHEDULE) |
| `LOG_BUFFER` | 131 | Aufzeichnungspuffer (TREND_LOG) |
| `RECORD_COUNT` | 141 | Anzahl der Einträge im Log |
| `TOTAL_RECORD_COUNT` | 145 | Gesamtzahl aller je aufgezeichneten Einträge |
| `NOTIFICATION_CLASS` | 17 | Klasse für Ereignisbenachrichtigungen |
| `SCHEDULE_DEFAULT` | 174 | Standardwert wenn kein Eintrag aktiv |
| `EFFECTIVE_PERIOD` | 32 | Gültigkeitszeitraum (SCHEDULE) |
| `WEEKLY_SCHEDULE` | 123 | Wochenplan |

---

## 7. Ausnahmen

---

### Bacnet\Exception

```
class Bacnet\Exception extends \Exception
```

Basisklasse für alle php-bacnet-Ausnahmen. Wird bei internen Fehlern geworfen
(Kodierung, nicht initialisierter Client, unbekannte BACnet-Antwort).

---

### Bacnet\TimeoutException

```
class Bacnet\TimeoutException extends Bacnet\Exception
```

Wird geworfen, wenn innerhalb des konfigurierten Timeouts keine gültige Antwort
vom Gerät eingegangen ist.

**Typische Ursachen:**
- Gerät nicht erreichbar oder ausgeschaltet
- Falsches Netzwerk-Interface konfiguriert
- Firewall blockiert UDP-Port 47808
- Zu kurzer Timeout

---

### Bacnet\DeviceException

```
class Bacnet\DeviceException extends Bacnet\Exception
```

Wird geworfen, wenn das Gerät eine gültige **Error-PDU** zurückgesendet hat.

**Eigenschaften**

| Eigenschaft | Typ | Beschreibung |
|------------|-----|-------------|
| `$errorClass` | `int` | BACnet Error Class (ASHRAE 135 Abschnitt 18.11). |
| `$errorCode` | `int` | BACnet Error Code. |

**Wichtige Error Classes**

| Wert | Klasse |
|------|--------|
| 0 | `DEVICE` |
| 1 | `OBJECT` |
| 2 | `PROPERTY` |
| 3 | `RESOURCES` |
| 4 | `SECURITY` |
| 5 | `SERVICES` |
| 6 | `VT` |

**Häufige Error Codes**

| Code | Bedeutung |
|------|-----------|
| 9 | `INCONSISTENT_PARAMETERS` — falscher Datentyp o. Ä. |
| 31 | `UNKNOWN_OBJECT` — Objekt existiert nicht auf dem Gerät |
| 32 | `UNKNOWN_PROPERTY` — Eigenschaft wird von diesem Objekt nicht unterstützt |
| 40 | `WRITE_ACCESS_DENIED` — Schreiben nicht erlaubt |
| 44 | `VALUE_OUT_OF_RANGE` — Wert außerhalb des gültigen Bereichs |

**Beispiel**

```php
<?php
try {
    $device->readProperty(Bacnet\ObjectType::ANALOG_VALUE, 999, Bacnet\Property::PRESENT_VALUE);
} catch (Bacnet\TimeoutException $e) {
    echo "Timeout: " . $e->getMessage() . "\n";
} catch (Bacnet\DeviceException $e) {
    echo "Gerätefehler: Klasse={$e->errorClass} Code={$e->errorCode}\n";
    // z. B. Klasse=1, Code=31 → OBJECT / UNKNOWN_OBJECT
} catch (Bacnet\Exception $e) {
    echo "Interner Fehler: " . $e->getMessage() . "\n";
}
```

---

## 8. Vollständige Beispiele

### Beispiel 1 — Geräte suchen und Werte lesen

```php
<?php
declare(strict_types=1);

$client = new Bacnet\Client(interface: 'eth0', timeoutMs: 3000);

// Alle Geräte im Segment finden
$devices = $client->whoIs();

foreach ($devices as $device) {
    printf("\n=== Gerät %d (%s) ===\n", $device->getDeviceId(), $device->getAddress());

    try {
        $name = $device->readProperty(
            Bacnet\ObjectType::DEVICE,
            $device->getDeviceId(),
            Bacnet\Property::OBJECT_NAME,
        );
        echo "Name: $name\n";

        $temp = $device->readProperty(
            Bacnet\ObjectType::ANALOG_VALUE, 1, Bacnet\Property::PRESENT_VALUE
        );
        printf("Temperatur: %.1f °C\n", $temp);

    } catch (Bacnet\TimeoutException) {
        echo "Gerät antwortet nicht auf ReadProperty\n";
    } catch (Bacnet\DeviceException $e) {
        printf("Fehler (Klasse=%d Code=%d)\n", $e->errorClass, $e->errorCode);
    }
}
```

---

### Beispiel 2 — Werte schreiben mit ObjectRef

```php
<?php
declare(strict_types=1);

$client = new Bacnet\Client('eth0');
[$device] = $client->whoIs(lowLimit: 1234, highLimit: 1234)
    ?: throw new RuntimeException('Gerät 1234 nicht gefunden');

// Sollwert-Regler über ObjectRef
$sollwert = new Bacnet\ObjectRef($device, Bacnet\ObjectType::ANALOG_VALUE, 1);
$heizung  = new Bacnet\ObjectRef($device, Bacnet\ObjectType::BINARY_OUTPUT, 1);

echo "Aktueller Sollwert: " . $sollwert->readProperty(Bacnet\Property::PRESENT_VALUE) . " °C\n";

$sollwert->writePresentValue(22.0); // REAL, Prio 16
$heizung->writeActive();            // ENUMERATED(1), Prio 16

echo "Sollwert auf 22.0 °C gesetzt, Heizung eingeschaltet\n";

// Nach 10 Minuten zurücknehmen (Priorität 16 freigeben = null schreiben)
// sleep(600);
// $heizung->writeProperty(Bacnet\Property::PRESENT_VALUE, Bacnet\Value::enumerated(0));
```

---

### Beispiel 3 — Status-Flags überwachen

```php
<?php
declare(strict_types=1);

$client = new Bacnet\Client('eth0');
[$device] = $client->whoIs(1234, 1234);

$sensor = new Bacnet\ObjectRef($device, Bacnet\ObjectType::ANALOG_INPUT, 1);

while (true) {
    try {
        $pv    = $sensor->readProperty(Bacnet\Property::PRESENT_VALUE);
        $flags = $sensor->readProperty(Bacnet\Property::STATUS_FLAGS);

        printf("[%s] Wert: %.2f", date('H:i:s'), $pv);

        if ($flags instanceof Bacnet\BitString) {
            if ($flags->getBit(0)) print " [ALARM]";
            if ($flags->getBit(1)) print " [FAULT]";
            if ($flags->getBit(3)) print " [OUT-OF-SERVICE]";
        }
        echo "\n";

    } catch (Bacnet\TimeoutException) {
        echo "Verbindung unterbrochen\n";
    }

    sleep(5);
}
```

---

### Beispiel 4 — Einfacher BACnet-Server

```php
<?php
declare(strict_types=1);

$server = new Bacnet\Server(deviceId: 9001, bindInterface: 'eth0');

// Lokale Objekte anmelden
$server->addLocalObject(new Bacnet\ObjectIdentifier(Bacnet\ObjectType::ANALOG_VALUE, 1));
$server->addLocalObject(new Bacnet\ObjectIdentifier(Bacnet\ObjectType::BINARY_VALUE, 1));

// In-memory Datenpunkte
$store = [
    'av1' => 21.5,
    'bv1' => 0,
];

$server->onReadProperty(function (
    Bacnet\ObjectIdentifier $oid,
    Bacnet\Property|int     $property,
    ?int                    $arrayIndex,
) use (&$store): mixed {
    return match (true) {
        $oid->getType() === Bacnet\ObjectType::ANALOG_VALUE
            && $property === Bacnet\Property::PRESENT_VALUE => $store['av1'],

        $oid->getType() === Bacnet\ObjectType::BINARY_VALUE
            && $property === Bacnet\Property::PRESENT_VALUE => $store['bv1'],

        $property === Bacnet\Property::OBJECT_NAME =>
            "PHP-Server-{$oid->getInstance()}",

        default => null,
    };
});

$server->onWriteProperty(function (
    Bacnet\ObjectIdentifier $oid,
    Bacnet\Property|int     $property,
    mixed                   $value,
    ?int                    $arrayIndex,
) use (&$store): void {
    if ($property !== Bacnet\Property::PRESENT_VALUE) return;

    if ($oid->getType() === Bacnet\ObjectType::ANALOG_VALUE) {
        $store['av1'] = (float)$value;
    } elseif ($oid->getType() === Bacnet\ObjectType::BINARY_VALUE) {
        $store['bv1'] = (int)$value;
    }

    printf("[%s] Schreibe %s = %s\n", date('H:i:s'), $oid, var_export($value, true));
});

echo "BACnet-Server Device 9001 läuft (eth0:47808)\n";

while (true) {
    $server->poll(timeoutMs: 100);
}
```

---

### Beispiel 5 — TrendLog auslesen

```php
<?php
declare(strict_types=1);

$client = new Bacnet\Client('eth0');
[$device] = $client->whoIs(1234, 1234);

$log = new Bacnet\ObjectRef($device, Bacnet\ObjectType::TREND_LOG, 1);

$records = $log->readTrendLog();
printf("TrendLog enthält %d Einträge:\n", count($records));

foreach ($records as $i => $rec) {
    printf(
        "  [%d] Zeitstempel=%s Wert=%s Flags=%d\n",
        $i,
        is_object($rec->timestamp) ? (string)$rec->timestamp : var_export($rec->timestamp, true),
        var_export($rec->value, true),
        $rec->statusFlags,
    );
}
```

---

*Dokumentation generiert für php-bacnet v0.1.0 — bacnet-stack 1.5.0 (5afc5c9a)*
