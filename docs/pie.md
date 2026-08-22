# Installation mit PIE

`php-bacnet` wird für Linux und macOS als PIE-Quellpaket bereitgestellt. Für
Windows stehen vorkompilierte NTS- und ZTS-DLL-Artefakte aus den GitHub-Releases bereit.

## Voraussetzungen

- PHP 8.4 oder neuer inklusive passendem `phpize` und `php-config` (Linux/macOS)
- C-Compiler, `make` und CMake
- keine systemweite LMDB-Bibliothek; die fest gepinnte LMDB-Quelle wird mitgebaut

PIE baut die mitgelieferte `bacnet-stack`-Bibliothek während `configure`
automatisch als statische Bibliothek. Deshalb ist kein manueller Aufruf von
`scripts/build-deps.sh` erforderlich.

## Installation

Nach der Veröffentlichung bei Packagist und eines GitHub-Release-Assets im
PIE-Quellformat erfolgt die Installation mit:

```bash
pie install hammermaps/php-bacnet
```

Für eine bestimmte PHP-Installation werden deren Werkzeuge angegeben:

```bash
pie install hammermaps/php-bacnet \
  --with-phpize-path=/usr/bin/phpize8.5 \
  --with-php-config-path=/usr/bin/php-config8.5
```

Die Extension heißt `bacnet`; PIE aktiviert sie nach erfolgreicher Installation
für die gewählte PHP-Installation.

Unter Windows installiert PIE die DLL, deren PHP-Version, Architektur und
Thread-Safety-Modus (NTS oder ZTS) zur gewählten PHP-Installation passen.

## Release-Artefakt

Der Release-Workflow erzeugt `php_bacnet-<version>-src.tgz`. Es enthält den
rekursiv ausgecheckten `bacnet-stack`, denn ein normales GitHub-Quellarchiv
enthält Submodule nicht vollständig. PIE lädt dieses Artefakt vor dem Build.

Lokal lässt sich das Paket prüfen:

```bash
PIE_DIST_DIR=/tmp/php-bacnet-dist ./scripts/package-pie-source.sh 0.3.1
tar -tzf /tmp/php-bacnet-dist/php_bacnet-0.3.1-src.tgz | head
```
