# Step 1: Projekt-Setup & Build-System

**Ziel:** Repository anlegen, `bacnet-stack` als statische Library einbinden und ein kompilierbares Extension-Grundgerüst (`config.m4`) erzeugen.

## Aktionen

1.  **Repository-Init:**
    ```bash
    mkdir php-bacnet
    cd php-bacnet
    git init
    ```

2.  **Git-Submodule:**
    ```bash
    git submodule add https://github.com/bacnet-stack/bacnet-stack.git deps/bacnet-stack
    ```

3.  **Build-Skript `scripts/build-deps.sh`:**
    ```bash
    #!/usr/bin/env bash
    set -e
    cd " $ (dirname " $ 0")/../deps/bacnet-stack"
    mkdir -p build
    cd build
    cmake .. \
      -DBUILD_SHARED_LIBS=OFF \
      -DBUILD_APPS=OFF \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_TESTING=OFF
    cmake --build . --parallel
    ```
    > Ausgabe: `deps/bacnet-stack/build/src/libbacnet-stack.a` (oder `libbacnet.a` – Pfad im `config.m4` ggf. anpassen).

4.  **Extension-Dateien anlegen (leer):**
    - `config.m4`
    - `php_bacnet.h`
    - `bacnet.c` (Hauptmodul)
    - `src/bacnet_client.c` / `src/bacnet_client.h`
    - `src/bacnet_classes.c` / `src/bacnet_classes.h`
    - `src/bacnet_types.c` / `src/bacnet_types.h` (für Value/BitString/Date/Time/ObjectIdentifier)
    - `src/bacnet_helpers.c` / `src/bacnet_helpers.h` (Codec/Mapping)

5.  **`config.m4` implementieren:**
    ```m4
    PHP_ARG_WITH([bacnet], [for BACnet support], [AS_HELP_STRING([--with-bacnet], [Enable BACnet support])])

    if test " $ PHP_BACNET" != "no"; then
      AC_MSG_CHECKING([PHP version])
      PHP_BACNET_VERSION=` $ PHP_CONFIG --version`
      if test -z " $ PHP_BACNET_VERSION"; then
        AC_MSG_ERROR([php-config not found])
      fi
      PHP_BACNET_VERNUM=` $ PHP_CONFIG --vernum`
      if test " $ PHP_BACNET_VERNUM" -lt "80400"; then
        AC_MSG_ERROR([PHP 8.4+ required])
      fi
      AC_MSG_RESULT([ $ PHP_BACNET_VERSION])

      BACNET_DIR=" $ PWD/deps/bacnet-stack"
      BACNET_BUILD_DIR=" $ BACNET_DIR/build/src"

      PHP_ADD_INCLUDE( $ BACNET_DIR/include)
      PHP_ADD_INCLUDE( $ BACNET_DIR/src)
      PHP_ADD_LIBRARY_WITH_PATH(bacnet-stack, $BACNET_BUILD_DIR, BACNET_SHARED_LIBADD)

      PHP_SUBST(BACNET_SHARED_LIBADD)

      PHP_NEW_EXTENSION(bacnet, [
        bacnet.c
        src/bacnet_client.c
        src/bacnet_classes.c
        src/bacnet_types.c
        src/bacnet_helpers.c
      ], $ext_shared,,,-DZEND_ENABLE_STATIC_TSMRPM=0)
    fi
    ```
    > Hinweis: Wir bauen **NTS** (ZTS nicht notwendig). Keine Thread-Safe-Makros nutzen.

6.  **Erst-Build testen:**
    ```bash
    ./scripts/build-deps.sh
    phpize
    ./configure --with-bacnet
    make
    ```
    Erwartung: `modules/bacnet.so` wird ohne Fehler gebaut.

## Akzeptanzkriterien

- [ ] `deps/bacnet-stack/build/src/libbacnet-stack.a` existiert.
- [ ] `make` produziert `modules/bacnet.so`.
- [ ] `php -d extension=modules/bacnet.so -m | grep bacnet` zeigt die Extension.
