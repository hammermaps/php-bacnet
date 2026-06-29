# php-bacnet — Build- und Installationsanleitung

Diese Anleitung beschreibt den vollständigen Weg von den Quellen bis zur laufenden
PHP-Erweiterung in verschiedenen PHP-Laufzeitumgebungen (CLI, PHP-FPM, Apache mod_php).

---

## Inhaltsverzeichnis

1. [Voraussetzungen](#1-voraussetzungen)
2. [Quellen beziehen](#2-quellen-beziehen)
3. [bacnet-stack bauen](#3-bacnet-stack-bauen)
4. [PHP-Erweiterung kompilieren](#4-php-erweiterung-kompilieren)
5. [Installation in die PHP-Laufzeit](#5-installation-in-die-php-laufzeit)
   - [CLI](#51-cli)
   - [PHP-FPM](#52-php-fpm)
   - [Apache mod_php](#53-apache-modphp)
6. [Konfiguration (php.ini)](#6-konfiguration-phpini)
7. [Netzwerk-Voraussetzungen](#7-netzwerk-voraussetzungen)
8. [Installation prüfen](#8-installation-prüfen)
9. [Fehlersuche](#9-fehlersuche)

---

## 1. Voraussetzungen

### Systemsoftware

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    git \
    pkg-config
```

### PHP-Dev-Headers

Die Erweiterung muss mit den exakt gleichen Headers gebaut werden wie die Ziel-PHP-Runtime.

**PHP 8.5:**
```bash
sudo apt-get install -y php8.5-dev php8.5-cli
```

**PHP 8.4:**
```bash
sudo apt-get install -y php8.4-dev php8.4-cli
```

> **Wichtig:** Nur **NTS** (Non-Thread-Safe) wird unterstützt. Prüfen:
> ```bash
> php8.5 -i | grep "Thread Safety"
> # → Thread Safety => disabled
> ```
> ZTS-Builds (`Thread Safety => enabled`) sind nicht kompatibel.

### Netzwerk

UDP-Port **47808** (0xBAC0) muss auf dem Interface erreichbar sein:
```bash
sudo ufw allow 47808/udp   # Ubuntu/Debian mit ufw
# oder:
sudo iptables -A INPUT -p udp --dport 47808 -j ACCEPT
```

---

## 2. Quellen beziehen

```bash
git clone https://github.com/<your-org>/php-bacnet.git
cd php-bacnet

# Git-Submodul initialisieren (bacnet-stack)
git submodule update --init --recursive

# Submodul-Version prüfen
git -C deps/bacnet-stack log --oneline -1
# → 5afc5c9a5 ... (bacnet-stack-1.5.0)
```

---

## 3. bacnet-stack bauen

Das Build-Skript kompiliert die statische Bibliothek mit den korrekten Flags:

```bash
./scripts/build-deps.sh
```

Was das Skript tut:
- `cmake -DBACDL_BIP=ON -DBUILD_SHARED_LIBS=OFF -DCMAKE_C_FLAGS="-fPIC"` im Verzeichnis `deps/bacnet-stack`
- Ausgabe: `deps/bacnet-stack/build/libbacnet-stack.a`

**Manuelle Kontrolle:**
```bash
ls -lh deps/bacnet-stack/build/libbacnet-stack.a
# → -rw-r--r-- ... 2.3M ... libbacnet-stack.a
```

> Fehler beim Build? Häufige Ursachen:
> - `cmake` nicht installiert → `sudo apt-get install cmake`
> - Veraltetes CMake (< 3.16) → PPA oder manuell installieren
> - Submodul nicht initialisiert → `git submodule update --init --recursive`

---

## 4. PHP-Erweiterung kompilieren

### PHP 8.5

```bash
# Alten Build bereinigen (nach PHP-Versionsänderung immer!)
phpize8.5 --clean

# Build-System vorbereiten
phpize8.5

# Konfigurieren
./configure --with-bacnet --with-php-config=php-config8.5

# Kompilieren (alle CPU-Kerne nutzen)
make -j$(nproc)

# Ergebnis prüfen
ls -lh modules/bacnet.so
```

### PHP 8.4

```bash
phpize8.4 --clean
phpize8.4
./configure --with-bacnet --with-php-config=php-config8.4
make -j$(nproc)
```

### Debug-Build (für Entwicklung)

```bash
./configure --with-bacnet --with-php-config=php-config8.5 \
    --enable-debug \
    CFLAGS="-O0 -g3 -Wall -Wextra"
make -j$(nproc)
```

### Schnellprüfung ohne Installation

```bash
php8.5 -d extension=modules/bacnet.so -m | grep bacnet
# → bacnet

php8.5 -d extension=modules/bacnet.so -r "
    \$c = new Bacnet\Client();
    echo 'OK: ' . get_class(\$c) . PHP_EOL;
"
# → OK: Bacnet\Client
```

---

## 5. Installation in die PHP-Laufzeit

### 5.1 CLI

**Schritt 1: Extension-Verzeichnis ermitteln**
```bash
php-config8.5 --extension-dir
# → /usr/lib/php/20250924
```

**Schritt 2: Bibliothek kopieren**
```bash
sudo cp modules/bacnet.so $(php-config8.5 --extension-dir)/bacnet.so
```

**Schritt 3: INI-Datei anlegen**
```bash
echo "extension=bacnet.so" | sudo tee /etc/php/8.5/cli/conf.d/30-bacnet.ini
```

**Schritt 4: Laden prüfen**
```bash
php8.5 -m | grep bacnet
# → bacnet

php8.5 -r "phpinfo();" | grep -A5 "bacnet"
```

---

### 5.2 PHP-FPM

**Schritt 1: Bibliothek installieren** (gleich wie CLI, falls noch nicht geschehen)
```bash
sudo cp modules/bacnet.so $(php-config8.5 --extension-dir)/bacnet.so
```

**Schritt 2: FPM-spezifische INI-Datei anlegen**
```bash
echo "extension=bacnet.so" | sudo tee /etc/php/8.5/fpm/conf.d/30-bacnet.ini
```

**Schritt 3: FPM neu starten**
```bash
sudo systemctl restart php8.5-fpm
```

**Schritt 4: Laden prüfen**
```bash
# Via PHP-Datei im Webserver-Root:
echo "<?php phpinfo();" > /var/www/html/phpinfo.php
# Im Browser aufrufen und nach "bacnet" suchen

# Oder via FPM-Log:
sudo journalctl -u php8.5-fpm -n 50 | grep -i bacnet
```

> **Hinweis zu FPM und Singleton:** Die Erweiterung bindet einen prozess-globalen UDP-Socket
> via `bip_init()`. Der Singleton-Guard wird in `RINIT` zurückgesetzt, sodass jeder
> FPM-Worker-Prozess genau **einen** `Bacnet\Client` oder `Bacnet\Server` pro Request
> erstellen kann. Innerhalb eines Requests darf kein zweites Client-/Server-Objekt erzeugt werden.

---

### 5.3 Apache mod_php

> **Empfehlung:** mod_php läuft als Apache-Modul im Prefork-MPM. Jeder Kind-Prozess
> kann einen `Bacnet\Client` halten. Der Prefork-MPM ist kompatibel; Worker-MPM (ZTS) ist
> **nicht** unterstützt.

**Schritt 1: Bibliothek installieren**
```bash
sudo cp modules/bacnet.so $(php-config8.5 --extension-dir)/bacnet.so
```

**Schritt 2: Apache-INI anlegen**
```bash
echo "extension=bacnet.so" | sudo tee /etc/php/8.5/apache2/conf.d/30-bacnet.ini
```

**Schritt 3: Apache neu starten**
```bash
sudo systemctl restart apache2
```

**Schritt 4: Laden prüfen**
```php
<?php
// /var/www/html/bacnet-check.php
if (extension_loaded('bacnet')) {
    echo "php-bacnet ist geladen. Version: ";
    $r = new ReflectionExtension('bacnet');
    echo $r->getVersion() . PHP_EOL;
} else {
    echo "FEHLER: php-bacnet nicht geladen" . PHP_EOL;
}
```

---

## 6. Konfiguration (php.ini)

Alle Einstellungen gelten per Prozess und können per `ini_set()` zur Laufzeit überschrieben
werden (`PHP_INI_ALL`).

### Verfügbare Direktiven

| Direktive | Typ | Standard | Beschreibung |
|-----------|-----|---------|-------------|
| `bacnet.default_port` | `int` | `47808` | UDP-Port für den BACnet/IP-Socket. Standardport ist `0xBAC0 = 47808`. |
| `bacnet.default_timeout_ms` | `int` | `3000` | Wartezeit pro Request-/Response-Zyklus in Millisekunden. |
| `bacnet.default_interface` | `string` | `"0.0.0.0"` | Netzwerk-Interface-Name (z. B. `"eth0"`, `"enp3s0"`). `"0.0.0.0"` = automatische Erkennung. |

### Beispiel-Konfiguration

```ini
; /etc/php/8.5/cli/conf.d/30-bacnet.ini

extension=bacnet.so

; Standard-BACnet-Port (nicht ändern ohne Grund)
bacnet.default_port = 47808

; Timeout erhöhen für langsame Geräte oder hohe Netzwerklast
bacnet.default_timeout_ms = 5000

; Explizites Interface für Mehrhaushalts-Server oder VLANs
bacnet.default_interface = eth0
```

### Laufzeit-Überschreibung

```php
<?php
// Timeout für diese Anfrage erhöhen
ini_set('bacnet.default_timeout_ms', '10000');

$client = new Bacnet\Client();  // nutzt 10 s Timeout
```

### Konstruktor-Parameter überschreiben INI

```php
<?php
// Konstruktor-Parameter haben immer Vorrang vor INI-Werten
$client = new Bacnet\Client(
    interface:  'eth1',    // überschreibt bacnet.default_interface
    port:       47809,     // überschreibt bacnet.default_port
    timeoutMs:  2000,      // überschreibt bacnet.default_timeout_ms
);
```

### Mehrere PHP-Versionen parallel

Wenn PHP 8.4 und 8.5 parallel installiert sind, muss die Extension für jede Version
separat gebaut und konfiguriert werden:

```bash
# Für PHP 8.4
phpize8.4 --clean && phpize8.4
./configure --with-bacnet --with-php-config=php-config8.4
make -j$(nproc)
sudo cp modules/bacnet.so $(php-config8.4 --extension-dir)/
echo "extension=bacnet.so" | sudo tee /etc/php/8.4/cli/conf.d/30-bacnet.ini

# Für PHP 8.5
phpize8.5 --clean && phpize8.5
./configure --with-bacnet --with-php-config=php-config8.5
make -j$(nproc)
sudo cp modules/bacnet.so $(php-config8.5 --extension-dir)/
echo "extension=bacnet.so" | sudo tee /etc/php/8.5/cli/conf.d/30-bacnet.ini
```

---

## 7. Netzwerk-Voraussetzungen

### Firewall

BACnet/IP nutzt UDP-Broadcasts auf Port 47808:

```bash
# UFW (Ubuntu/Debian)
sudo ufw allow 47808/udp comment "BACnet/IP"

# firewalld (CentOS/RHEL/Fedora)
sudo firewall-cmd --permanent --add-port=47808/udp
sudo firewall-cmd --reload

# iptables direkt
sudo iptables -A INPUT  -p udp --dport 47808 -j ACCEPT
sudo iptables -A OUTPUT -p udp --dport 47808 -j ACCEPT
```

### Alternativer Client-Port

Um Portkonflikte zu vermeiden (z. B. wenn bereits ein BACnet-Server auf 47808 läuft),
kann der Client auf einem anderen Port lauschen:

```php
<?php
// Client auf Port 47809, Broadcasts gehen trotzdem an 47808
$client = new Bacnet\Client(port: 47809);
```

> Die Erweiterung setzt intern immer `bip_set_broadcast_port(0xBAC0)`, damit
> Who-Is-Broadcasts unabhängig vom eigenen Port an den Standard-BACnet-Port 47808 gehen.

### Broadcast-Erreichbarkeit prüfen

```bash
# Lauscht ein BACnet-Gerät auf Port 47808?
sudo tcpdump -i eth0 -n udp port 47808

# Eigene IP und Broadcast-Adresse prüfen
ip addr show eth0
# Broadcasts gehen an die .255-Adresse des Subnetzes
```

---

## 8. Installation prüfen

### Schnelltest CLI

```bash
php8.5 -d extension=modules/bacnet.so -r "
echo 'Extension geladen: ', extension_loaded('bacnet') ? 'JA' : 'NEIN', PHP_EOL;
echo 'Version: ', phpversion('bacnet'), PHP_EOL;
\$info = new ReflectionExtension('bacnet');
echo 'Klassen: ', implode(', ', \$info->getClassNames()), PHP_EOL;
"
```

Erwartete Ausgabe:
```
Extension geladen: JA
Version: 0.1.0
Klassen: Bacnet\Client, Bacnet\Device, Bacnet\ObjectRef, Bacnet\Server, ...
```

### Who-Is Test (erfordert BACnet-Gerät im Netzwerk)

```bash
php8.5 -d extension=modules/bacnet.so -r "
\$client = new Bacnet\Client('eth0', timeoutMs: 2000);
\$devices = \$client->whoIs();
echo count(\$devices), ' Gerät(e) gefunden:', PHP_EOL;
foreach (\$devices as \$d) {
    echo '  Gerät ', \$d->getDeviceId(), ' @ ', \$d->getAddress(), PHP_EOL;
}
"
```

### phpinfo()-Ausgabe

```bash
php8.5 -d extension=modules/bacnet.so -r "phpinfo();" | grep -A 10 "bacnet"
```

Erwartete Ausgabe:
```
BACnet/IP support => enabled
Extension version => 0.1.0
bacnet-stack      => 1.5.0 (5afc5c9a)

bacnet.default_interface => 0.0.0.0 => 0.0.0.0
bacnet.default_port      => 47808   => 47808
bacnet.default_timeout_ms => 3000   => 3000
```

---

## 9. Fehlersuche

### „PHP API mismatch" / Falsches API-Modul

```
PHP Warning: PHP Startup: bacnet: Unable to initialize module
Module compiled with module API=20240924
PHP    compiled with module API=20250924
```

**Ursache:** Die Extension wurde mit einer anderen PHP-Version gebaut als die aktive Runtime.

**Lösung:**
```bash
phpize8.5 --clean
phpize8.5
./configure --with-bacnet --with-php-config=php-config8.5
make -j$(nproc)
```

---

### „bip_init failed"

```
Fatal error: Uncaught Bacnet\Exception: bip_init failed on interface 'eth0' port 47808
```

**Ursachen und Lösungen:**

| Ursache | Prüfung | Lösung |
|---------|---------|--------|
| Interface existiert nicht | `ip link show eth0` | Korrekten Interface-Namen verwenden (z. B. `enp3s0`) |
| Port bereits belegt | `ss -ulnp | grep 47808` | Anderen Port wählen oder belegenden Prozess beenden |
| Fehlende Rechte | Prozess als root starten | `sudo php script.php` oder `CAP_NET_BIND_SERVICE` |
| Falsche IP statt Name | `new Client("192.168.1.1")` | Interface-*Namen* übergeben: `new Client("eth0")` |

---

### „Module already initialized" / Zweiter Client

```
Fatal error: Uncaught Error: Bacnet\Client or Server already initialized in this process
```

**Ursache:** Es wurde versucht, einen zweiten `Bacnet\Client` oder `Bacnet\Server` im
selben PHP-Prozess zu erstellen.

**Lösung:** Genau ein Client-Objekt pro Request anlegen und weitergeben (z. B. via
Dependency Injection). In FPM wird der Guard nach jedem Request automatisch zurückgesetzt.

---

### Who-Is findet keine Geräte

**Checkliste:**
1. Interface-Name korrekt? → `ip link` zeigt alle Interfaces
2. Firewall erlaubt UDP 47808? → `sudo ufw status`
3. BACnet-Gerät im gleichen Subnetz? → Broadcasts überqueren keine Router
4. Timeout zu kurz? → `timeoutMs: 5000` testen
5. Gerät antwortet unicast? → Wireshark/tcpdump prüfen

```bash
# BACnet-Traffic live mitschneiden
sudo tcpdump -i eth0 -n -v udp port 47808
```

---

### Valgrind-Speicherprüfung (Entwicklung)

```bash
# Extension darf nicht entladen werden (sonst keine Leak-Berichte)
export ZEND_DONT_UNLOAD_MODULES=1

valgrind \
    --leak-check=full \
    --show-leak-kinds=definite,indirect \
    --track-origins=yes \
    --error-exitcode=1 \
    php8.5 -d extension=modules/bacnet.so tests/your_test.php
```

Ziel: **0 definite Leaks** nach 1000 Create/Destroy-Zyklen.

---

*Dokumentation für php-bacnet v0.1.0 — bacnet-stack 1.5.0 (5afc5c9a)*
