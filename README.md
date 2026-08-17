# php-bacnet — BACnet/IP Extension for PHP 8.4+

[![PHP Version](https://img.shields.io/badge/PHP-8.4%2B%20NTS-blue)](#anforderungen)
[![bacnet-stack](https://img.shields.io/badge/bacnet--stack-1.5.0-green)](#)
[![License](https://img.shields.io/badge/License-BSD--3-blue)](./LICENSE)

**php-bacnet** ist eine native C-Erweiterung für PHP 8.4 und 8.5, die vollständige
BACnet/IP-Kommunikation bereitstellt. Sie kapselt die
[bacnet-stack](https://github.com/bacnet-stack/bacnet-stack)-Bibliothek und stellt eine
typsichere, objektorientierte API im Namensraum `Bacnet\` bereit.

> Vollständige API-Dokumentation: **[docs/api-reference.md](./docs/api-reference.md)**  
> Build & Installation: **[docs/installation.md](./docs/installation.md)**

---

## Überblick

BACnet (Building Automation and Control Networks) ist der international standardisierte
Kommunikationsstandard für Gebäudeautomation (ASHRAE 135 / ISO 16484-5). Diese Erweiterung
ermöglicht PHP-Anwendungen, als vollständige BACnet/IP-Knoten zu agieren:

| Modus | Beschreibung |
|-------|-------------|
| **Client** | Geräte per Who-Is/I-Am entdecken, Eigenschaften lesen und schreiben |
| **Server** | Eigene BACnet-Objekte bereitstellen, Callbacks für Read/Write-Anfragen |
| **Mixed** | Server und Client über einen gemeinsamen UDP-Socket betreiben |

---

## Unterstützte Objekttypen

| Objekttyp | ID | Beschreibung |
|-----------|----|-------------|
| `ANALOG_INPUT` | 0 | Analoger Messwert (Sensor) |
| `ANALOG_OUTPUT` | 1 | Analoger Ausgang (Aktor) |
| `ANALOG_VALUE` | 2 | Analoger Softwarewert / Sollwert |
| `BINARY_INPUT` | 3 | Binärer Eingang |
| `BINARY_OUTPUT` | 4 | Binärer Ausgang |
| `BINARY_VALUE` | 5 | Binärer Softwarewert |
| `DEVICE` | 8 | Geräteobjekt |
| `MULTI_STATE_INPUT` | 13 | Mehrwertiger Eingang |
| `MULTI_STATE_OUTPUT` | 14 | Mehrwertiger Ausgang |
| `MULTI_STATE_VALUE` | 19 | Mehrwertiger Softwarewert |
| `SCHEDULE` | 17 | Zeitprogramm (WEEKLY_SCHEDULE lesen) |
| `TREND_LOG` | 20 | Aufzeichnungsobjekt (LOG_BUFFER lesen) |

---

## Features

- **Typsichere OOP-API** — `Bacnet\Client`, `Bacnet\Device`, `Bacnet\ObjectRef`, `Bacnet\Server`, `Bacnet\MixedServer`
- **Explizite BACnet-Typen** — `Value::real()`, `Value::enumerated()`, `Value::characterString()` usw.
- **Komplexe BACnet-Datentypen** — `BitString`, `Date`, `Time`, `ObjectIdentifier`
- **Geräteentdeckung** — `whoIs()` mit optionalem Instanzbereich
- **Server-Modus** — PHP-Callbacks für `onReadProperty` / `onWriteProperty`
- **Mixed-Modus** — Server und ausgehende Client-Anfragen über einen gemeinsamen UDP-Socket
- **Komfort-API** — `ObjectRef::writePresentValue()`, `writeActive()`, `writeInactive()`
- **INI-Konfiguration** — Port, Timeout und Interface per `php.ini` konfigurierbar
- **Keine externen Laufzeitabhängigkeiten** — bacnet-stack wird als statische Bibliothek eingebettet

---

## Schnellstart

```php
<?php
declare(strict_types=1);

// Geräte im Netzwerk entdecken
$client = new Bacnet\Client(interface: 'eth0', timeoutMs: 3000);

$devices = $client->whoIs();
foreach ($devices as $device) {
    printf("Gerät %d @ %s (MaxAPDU=%d)\n",
        $device->getDeviceId(),
        $device->getAddress(),
        $device->getMaxApdu(),
    );
}

// Eigenschaft lesen
[$device] = $client->whoIs(lowLimit: 1234, highLimit: 1234);

$temp = $device->readProperty(
    Bacnet\ObjectType::ANALOG_VALUE,
    1,
    Bacnet\Property::PRESENT_VALUE,
);
printf("Temperatur: %.1f °C\n", $temp);

// Eigenschaft schreiben
$device->writeProperty(
    Bacnet\ObjectType::ANALOG_VALUE,
    1,
    Bacnet\Property::PRESENT_VALUE,
    Bacnet\Value::real(22.0),
    priority: 16,
);

// Komfort-API via ObjectRef
$sensor = new Bacnet\ObjectRef($device, Bacnet\ObjectType::ANALOG_VALUE, 1);
$sensor->writePresentValue(23.5);  // Typ wird automatisch erkannt

$relay = new Bacnet\ObjectRef($device, Bacnet\ObjectType::BINARY_OUTPUT, 1);
$relay->writeActive();    // ENUMERATED(1)
$relay->writeInactive();  // ENUMERATED(0)
```

---

## Mixed-Modus

Ein `MixedServer` stellt lokale Objekte bereit und kann über denselben Socket
andere Geräte entdecken, lesen und schreiben:

```php
$mixed = new Bacnet\MixedServer(5, 'net3', 47808);
$mixed->addLocalObject(new Bacnet\ObjectIdentifier(
    Bacnet\ObjectType::ANALOG_VALUE,
    1,
));

[$device] = $mixed->whoIs(200, 200, 1000);
$value = $device->readProperty(
    Bacnet\ObjectType::ANALOG_VALUE,
    10110,
    Bacnet\Property::PRESENT_VALUE,
);

while (true) {
    $mixed->poll(100);
}
```

Während synchroner Client-Aufrufe werden eingehende Server-PDUs gepuffert und
anschließend von `poll()` verarbeitet. Details und Event-Loop-Muster stehen in
**[docs/mixed-mode.md](./docs/mixed-mode.md)**.

Ausführbare Beispiele:

```bash
php examples/mixed_daemon.php
php examples/mixed_read.php 200 analog_value 10110 present_value
```

---

## Installation

Vollständige Anleitung: **[docs/installation.md](./docs/installation.md)**

```bash
# Submodul initialisieren und bacnet-stack bauen
git submodule update --init --recursive
./scripts/build-deps.sh

# Extension bauen (PHP 8.5)
phpize8.5
./configure --with-bacnet --with-php-config=php-config8.5
make -j$(nproc)

# Extension dauerhaft aktivieren
sudo cp modules/bacnet.so $(php-config8.5 --extension-dir)/
echo "extension=bacnet.so" | sudo tee /etc/php/8.5/cli/conf.d/30-bacnet.ini
php8.5 -m | grep bacnet
```

---

## Konfiguration (php.ini)

| Direktive | Standard | Beschreibung |
|-----------|---------|-------------|
| `bacnet.default_port` | `47808` | UDP-Port (Standard-BACnet-Port = 0xBAC0) |
| `bacnet.default_timeout_ms` | `3000` | Request-Timeout in Millisekunden |
| `bacnet.default_interface` | `0.0.0.0` | Interface-Name (`"eth0"`) oder Auto-Erkennung |

```ini
extension=bacnet.so
bacnet.default_port       = 47808
bacnet.default_timeout_ms = 3000
bacnet.default_interface  = eth0
```

---

## Dokumentation

| Datei | Beschreibung |
|-------|-------------|
| [docs/api-reference.md](./docs/api-reference.md) | Vollständige PHP API-Referenz (php.net-Stil) |
| [docs/installation.md](./docs/installation.md) | Build- und Installationsanleitung |
| [docs/faq.md](./docs/faq.md) | Häufige Fragen zu Installation, Discovery und Fehlersuche |
| [docs/mixed-mode.md](./docs/mixed-mode.md) | Architektur, Queue und Daemon-Betrieb des Mixed-Modus |
| [examples/README.md](./examples/README.md) | Ausführbare Client-/Server-Demos |
| [stubs/bacnet.stub.php](./stubs/bacnet.stub.php) | IDE/PHPStan Stubs |
| [CHANGELOG.md](./CHANGELOG.md) | Versionshistorie |

---

## Anforderungen

- PHP **8.4** oder **8.5** — NTS-Build (Non-Thread-Safe), mit Dev-Headers (`php8.5-dev`)
- Linux (GCC, Autotools)
- `build-essential`, `cmake` ≥ 3.16
- Netzwerkzugang auf UDP-Port 47808 (BACnet/IP)

---

## Lizenz

BSD-3-Clause — siehe [LICENSE](./LICENSE).
Kompatibel mit der [bacnet-stack BSD-3-Lizenz](https://github.com/bacnet-stack/bacnet-stack/blob/master/LICENSE).

---

## Danksagungen

- **[bacnet-stack](https://github.com/bacnet-stack/bacnet-stack)** — eingebetteter BACnet-Protokoll-Stack
- **[PHP Internals Book](https://www.phpinternalsbook.com/)** — Grundlagenwerk für PHP-Extension-Entwicklung
