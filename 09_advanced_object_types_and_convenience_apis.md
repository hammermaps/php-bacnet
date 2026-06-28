# Step 10: Testing, QA & Release-Vorbereitung

**Ziel:** Extension ist getestet, dokumentiert und für PECL/Release paketierbar. Memory-Safety ist nachgewiesen.

## Aktionen

1.  **PHPT-Tests (`tests/`):**
    Erstelle für jeden Step mindestens einen `.phpt`-Test:
    - `001_client_construct.phpt`
    - `002_whois_discovery.phpt`  (benötigt BACnet-Simulator oder mock?)
    - `003_read_property.phpt`
    - `004_write_property.phpt`
    - `005_complex_types.phpt`
    - `006_server_poll.phpt`
    - `007_exceptions.phpt`
    
    > Für Tests ohne echte Hardware: Nutze `SKIPIF` mit `extension_loaded('bacnet')`. Netzwerk-Tests werden als `--POST--` oder `--STDIN--` mit einem lokalen BACnet-Simulator (z. B. `bacnet-stack` Beispiel-Device) durchgeführt. Beschreibe in `tests/README.md`, wie der Simulator gestartet wird.

2.  **Memory-Leak-Tests:**
    ```bash
    ZEND_DONT_UNLOAD_MODULES=1 valgrind --leak-check=full --show-leak-kinds=definite,indirect \
      php run-tests.php tests/
    ```
    - Fixe alle `definite lost`-Leaks.
    - Typische Fallstricke: Nicht freigegebene `zend_string`, vergessene `zval_ptr_dtor`, `efree` vs `pefree` bei persistenten Client-Strukturen.

3.  **GitHub Actions CI (`.github/workflows/ci.yml`):**
    ```yaml
    name: CI
    on: [push, pull_request]
    jobs:
      build:
        runs-on: ubuntu-latest
        steps:
          - uses: actions/checkout@v4
            with:
              submodules: recursive
          - name: Install dependencies
            run: sudo apt-get update && sudo apt-get install -y build-essential cmake php-dev valgrind
          - name: Build bacnet-stack
            run: ./scripts/build-deps.sh
          - name: phpize & configure
            run: phpize && ./configure --with-bacnet
          - name: Build
            run: make
          - name: Run tests
            run: make test TESTS=tests/ REPORT_EXIT_STATUS=1
          - name: Memory check (optional)
            run: |
              # Starte BACnet-Simulator im Hintergrund (wenn möglich)
              # Valgrind-Test ...
    ```

4.  **IDE-Stubs (`stubs/bacnet.stub.php`):**
    Erstelle eine vollständige PHP-Stub-Datei mit allen Klassen, Enums, Methoden und Typ-Hinweisen für PHPStan/IDE:
    ```php
    <?php
    namespace Bacnet {
        enum ObjectType: int { /* ... */ }
        enum Property: int { /* ... */ }
        class Client { /* ... */ }
        class Device { /* ... */ }
        // ... etc
    }
    ```

5.  **Dokumentation:**
    - `README.md`: Installationsanleitung (inkl. Submodule), minimales Code-Beispiel.
    - `API.md`: Alle Klassen und Methoden mit Beschreibung.
    - `CHANGELOG.md`: Version 0.1.0 – Initial Release.

6.  **PECL-Vorbereitung:**
    - `package.xml` mit `<name>bacnet</name>`, Version, Lead, Dependencies.
    - `LICENSE` (BSD-3 oder MIT, abgestimmt mit `bacnet-stack`-Lizenz – diese ist BSD-like).

7.  **Finaler Build-Check:**
    - Kompiliert sauber mit `-Wall -Wextra` (minimiere Warnings).
    - Funktioniert unter PHP 8.4 und 8.5 (API-Check: `PHP_VERSION_ID`).

## Akzeptanzkriterien

- [ ] `make test` läuft erfolgreich durch (ggf. mit lokalem Simulator).
- [ ] Valgrind-Report zeigt 0x definite leaks bei normaler Nutzung (Client create/destroy, 1000 Requests).
- [ ] `README.md` enthält Copy-Paste-Beispiel für Who-Is + ReadProperty.
- [ ] `stubs/bacnet.stub.php` ist vollständig und fehlerfrei.
- [ ] Extension lädt in PHP 8.4 und 8.5 ohne `undefined symbol`-Fehler.
