# Plan: Zend Thread Safety (ZTS)

## Ziel

`php-bacnet` unterstützt zusätzlich ZTS-Varianten für PHP 8.4 und 8.5 unter
Linux und Windows. Die öffentliche PHP-API und das BACnet-Verhalten
bleiben unverändert. NTS- und ZTS-Binaries werden getrennt gebaut und
ausgeliefert.

## Ausgangslage

Die Erweiterung verwendet bereits PHP-Modul-Globals und aktualisiert den
TSRM-Cache in `RINIT`. Der eingebettete `bacnet-stack` hält jedoch Socket,
Netzwerkadressen und Service-Handler als veränderliche C-`static`-Globals.
Diese wären bei mehreren PHP-Threads geteilt und sind ohne zusätzliche
Synchronisierung nicht sicher.

PHP trennt Modul-Globals über TSRM je Thread. Echte veränderliche C-Globals
müssen daher durch die Erweiterung geschützt oder in kontextgebundenen Zustand
überführt werden.

Referenzen:

- [PHP Internals: Extensions](https://wiki.php.net/internals/extensions)
- [PHP Internals Book: Managing global state](https://www.phpinternalsbook.com/php7/extensions_design/globals_management.html)

## Umsetzungsschritte

### 1. Zielplattform und Kompatibilität festlegen

- PHP 8.4 und PHP 8.5 als ZTS unter Linux und Windows unterstützen.
- macOS erst nach einem erfolgreichen Build und Test ergänzen.
- Die bestehende PHP-API bleibt unverändert.
- Ein PHP-Interpreter darf nur ein Modul mit passender PHP-API und passendem
  ZTS-Modus laden.

### 2. Global-State-Audit

- Alle veränderlichen Globals der Erweiterung und des `bacnet-stack` erfassen.
- Insbesondere `bip-init.c` (UDP-Sockets, Interface, Broadcast), TSM,
  BACnet-Service-Handler, COV-Handler sowie Objekt- und Callback-Lebenszeiten
  prüfen.
- Unveränderliche C-Konstanten bleiben unverändert; nur veränderlicher Zustand
  wird umgebaut oder geschützt.

### 3. Zentralen Transportmanager einführen

- Einen pro Prozess existierenden, mutex-geschützten BACnet-Transport
  einführen.
- Socket-Erzeugung, Senden, Empfangen, Handler-Registrierung und
  `bip_cleanup()` ausschließlich unter diesem Lock ausführen.
- Referenzzählung und ein definierter Lifecycle verhindern, dass ein Thread
  den Socket schließt, während ein anderer ihn verwendet.
- Die bisher threadlokale Singleton-Prüfung durch eine prozessweite
  Transportverwaltung ergänzen.

### 4. Request- und Callback-Isolation sicherstellen

- PHP-`zval`s und `zend_fcall_info_cache` ausschließlich im besitzenden Thread
  verwenden.
- Der Transport hält nur C-kopierte PDUs und Ereignisse vor; `poll()` führt
  PHP-Callbacks im aufrufenden PHP-Thread aus.
- Synchrone Requests per Invoke-ID und Warteschlange dem richtigen Aufrufer
  zuordnen.
- COV-Ereignisse thread-sicher puffern und beim jeweiligen `poll()`
  verarbeiten.

### 5. Semantik dokumentieren

- Ein BACnet/IP-Endpunkt bleibt pro Prozess geteilt.
- Parallele Netzwerkaufrufe werden serialisiert, bis der `bacnet-stack`
  vollständig kontextfähig ist.
- Langlaufende COV- und Server-Prozesse erhalten eine eindeutige
  Ownership-Regel.
- Native Hintergrundthreads führen niemals PHP-Callbacks aus.

### 6. Build und Paketierung anpassen

- In `composer.json` `support-zts` erst nach erfolgreicher Umsetzung auf
  `true` setzen.
- README sowie Installations- und PIE-Dokumentation um ZTS-Build-Anweisungen
  ergänzen.
- PECL-/PIE-Metadaten und Release-Artefakte auf getrennte NTS- und ZTS-Builds
  ausrichten.

### 7. Tests und CI

- ZTS-PHP 8.4 und 8.5 aus Quellcode mit `--enable-zts` bauen oder gleichwertige
  verifizierte ZTS-Runtimes einsetzen.
- Die vollständige PHPT-Suite für NTS und ZTS ausführen.
- Paralleltests für Discovery, `readProperty()`, Server-/Client-Nutzung,
  Socket-Lifecycle, COV-Queue und Cache-Zugriffe ergänzen.
- TSAN für nativen Code und Valgrind weiterhin für NTS ausführen.
- Negativtest: NTS-Modul in ZTS-PHP und ZTS-Modul in NTS-PHP müssen erwartbar
  abgelehnt werden.

## Umsetzungsstand

Mit Version 0.3.1 ist der zentrale, mutex-geschützte Transportmanager
implementiert. Socket-Lifecycle, Senden, Empfangen und `bip_cleanup()` sind
prozessweit serialisiert. Nicht zugeordnete PDUs werden kopiert und im
besitzenden Client gepuffert; `poll()` führt eventuelle PHP-Callbacks erst
nach dem Entsperren aus. Native Hintergrundthreads werden nicht verwendet.

Die GitHub-Matrix baut PHP 8.4 und 8.5 unter Linux und Windows jeweils als NTS
und ZTS. Sie ist die maßgebliche Freigabeprüfung für diesen Zweig.

## Freigabekriterien

- Keine Datenrennen oder Deadlocks in Paralleltests.
- Identisches öffentliches API-Verhalten für NTS und ZTS.
- Grüne ZTS-CI für PHP 8.4 und 8.5.
- Erst nach Erfüllung dieser Kriterien `support-zts` in den Paketmetadaten
  aktivieren und eine neue Version veröffentlichen.

## Empfohlene Reihenfolge

Zunächst wird der mutex-geschützte gemeinsame Transport umgesetzt. Das bietet
eine sichere, API-kompatible ZTS-Variante. Eine vollständige
Kontext-Isolierung des `bacnet-stack` ist ein deutlich größerer, separater
Folgeschritt.
