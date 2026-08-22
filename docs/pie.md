# Installation mit PIE

`php-bacnet` wird für Linux und macOS als PIE-Quellpaket bereitgestellt.
Windows wird nicht unterstützt, da dafür vorkompilierte DLL-Artefakte notwendig
wären.

## Voraussetzungen

- PHP 8.4 oder neuer inklusive passendem `phpize` und `php-config`
- C-Compiler, `make` und CMake
- LMDB-Entwicklungspaket (z. B. `liblmdb-dev` auf Debian/Ubuntu)

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

## Release-Artefakt

Der Release-Workflow erzeugt `php_bacnet-<version>-src.tgz`. Es enthält den
rekursiv ausgecheckten `bacnet-stack`, denn ein normales GitHub-Quellarchiv
enthält Submodule nicht vollständig. PIE lädt dieses Artefakt vor dem Build.

Lokal lässt sich das Paket prüfen:

```bash
PIE_DIST_DIR=/tmp/php-bacnet-dist ./scripts/package-pie-source.sh 0.1.3
tar -tzf /tmp/php-bacnet-dist/php_bacnet-0.1.3-src.tgz | head
```
