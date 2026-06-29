# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**php-bacnet** is a PHP 8.4/8.5 C extension providing native BACnet/IP communication by wrapping the [bacnet-stack](https://github.com/bacnet-stack/bacnet-stack) C library. The extension exposes a type-safe OOP API under the `Bacnet\` namespace, supporting both client mode (device discovery, read/write properties) and server mode (respond to BACnet requests via PHP callbacks).

The extension is **fully implemented**. See `docs/api-reference.md` for the complete PHP API reference and `docs/installation.md` for build and deployment instructions.

## Build Commands

```bash
# 1. Initialize submodule (first time only)
git submodule update --init --recursive

# 2. Build bacnet-stack static library
./scripts/build-deps.sh
# Output: deps/bacnet-stack/build/libbacnet-stack.a

# 3. Build the PHP extension (PHP 8.5)
phpize8.5
./configure --with-bacnet --with-php-config=php-config8.5
make -j$(nproc)

# For PHP 8.4:
# phpize8.4 && ./configure --with-bacnet --with-php-config=php-config8.4 && make

# 4. Verify the extension loads
php8.5 -d extension=modules/bacnet.so -m | grep bacnet
php8.5 -d extension=modules/bacnet.so -r "phpinfo();"
```

## Testing

```bash
# Run all PHPT tests
make test TESTS=tests/ REPORT_EXIT_STATUS=1

# Run a single test
php run-tests.php tests/001_client_construct.phpt

# Memory leak check (requires Valgrind + BACnet simulator running)
ZEND_DONT_UNLOAD_MODULES=1 valgrind --leak-check=full --show-leak-kinds=definite,indirect \
  php run-tests.php tests/
```

Tests requiring network access use `--SKIPIF--` guards and depend on a local BACnet simulator (see `tests/README.md` once created). Target: 0 definite Valgrind leaks for 1000 create/destroy cycles.

## Architecture

The extension is NTS-only (no ZTS/thread-safety required). Source layout once implemented:

| File | Purpose |
|------|---------|
| `bacnet.c` | Module entry (`zend_module_entry`), INI registration, MINIT/MSHUTDOWN/RINIT/RSHUTDOWN, MINFO |
| `php_bacnet.h` | Header — all `zend_class_entry *` pointers declared as `extern` |
| `src/bacnet_client.c/h` | C wrapper: UDP socket via bacnet-stack (`bip_init`, `datalink_receive`, `datalink_send_pdu`), synchronous send/wait loop, broadcast collector |
| `src/bacnet_classes.c/h` | `php_bacnet_register_classes()` — registers all Zend classes, enums, exceptions in MINIT |
| `src/bacnet_types.c/h` | Internal structs and `zend_object_handlers` for Value, BitString, Date, Time, ObjectIdentifier |
| `src/bacnet_helpers.c/h` | BACnet APDU ↔ PHP `zval` type mapping/codec |
| `deps/bacnet-stack/` | Git submodule — built as a static lib, never modified |
| `tests/*.phpt` | PHPT tests (one per step) |
| `stubs/bacnet.stub.php` | IDE/PHPStan stubs for all classes and enums |

### PHP API Surface (`Bacnet\` namespace)

**Classes:** `Client`, `Device`, `ObjectRef`, `Server`, `ObjectIdentifier`, `BitString`, `Date`, `Time`, `Value`, `ScheduleEntry`, `WeeklySchedule`, `TrendLogRecord`

**Enums:** `ObjectType` (int-backed, e.g. `ANALOG_VALUE=2`, `DEVICE=8`, `SCHEDULE=17`), `Property` (int-backed, e.g. `PRESENT_VALUE=85`)

**Exceptions:** `Exception` → `TimeoutException`, `DeviceException` (adds `errorClass`/`errorCode` properties)

See `docs/api-reference.md` for the complete reference.

### INI Settings

| Setting | Default | Description |
|---------|---------|-------------|
| `bacnet.default_port` | `47808` | BACnet/IP UDP port |
| `bacnet.default_timeout_ms` | `3000` | Request timeout |
| `bacnet.default_interface` | `0.0.0.0` | Bind interface |

### Internal Object Pattern

Every PHP class with C state uses the embedded-struct pattern:
```c
typedef struct {
    php_bacnet_client *client;  // C state first
    zend_object std;            // zend_object LAST
} php_bacnet_client_obj;
```
`free_obj` must call `zend_object_std_dtor`, release parent `zval` references via `zval_ptr_dtor`, and free C resources. Use `efree`/`emalloc` for request-scoped memory, `pefree`/`pemalloc` for persistent client structures.

### C-Layer Key Functions

- `php_bacnet_client_create(iface, port)` — calls `bip_init`, opens UDP socket
- `php_bacnet_client_destroy(client)` — calls `datalink_cleanup`
- `php_bacnet_send_and_wait(...)` — synchronous unicast request/response with invoke-ID matching
- `php_bacnet_broadcast_and_collect(...)` — Who-Is/I-Am broadcast collector (fixed 64-entry buffer for MVP)

## Documentation

| Datei | Inhalt |
|-------|--------|
| `docs/api-reference.md` | Vollständige PHP API-Referenz (php.net-Stil) |
| `docs/installation.md` | Build- und Installationsanleitung |
| `docs/01_project_setup_and_build_system.md` | Implementierungsplan Phase 1 |
| … | Phasen 2–10 analog |

## bacnet-stack Submodule

Pinned commit: `5afc5c9a54b2579f61ec5959c58a8ce595bb55e8` (bacnet-stack-1.5.0-109-g5afc5c9a5)

Do not update without a changelog entry and deliberate `git submodule update`.

The library is built as a static `-fPIC` archive via `scripts/build-deps.sh`
(cmake, `BACDL_BIP=ON`, `BUILD_SHARED_LIBS=OFF`). Output: `deps/bacnet-stack/build/libbacnet-stack.a`

**Important:** `bip_init()` requires an interface *name* (e.g. `"eth0"`), not an IP address.
Pass `NULL` to auto-detect. Passing `"0.0.0.0"` triggers auto-detection, `""` ebenfalls.
`bip_set_broadcast_port(0xBAC0)` muss nach `bip_set_port()` gesetzt werden, damit Broadcasts
immer Port 47808 erreichen — unabhängig vom eigenen Client-Port.

## Requirements

- PHP 8.4 or 8.5 NTS (with dev headers: `php8.5-dev` / `php8.4-dev`)
- `build-essential`, `cmake`
- Linux (GCC/Autotools)
- Valgrind (for memory checks, optional)

## Skill routing

When the user's request matches an available skill, invoke it via the Skill tool. When in doubt, invoke the skill.

Key routing rules:
- Product ideas/brainstorming → invoke /office-hours
- Strategy/scope → invoke /plan-ceo-review
- Architecture → invoke /plan-eng-review
- Design system/plan review → invoke /design-consultation or /plan-design-review
- Full review pipeline → invoke /autoplan
- Bugs/errors → invoke /investigate
- QA/testing site behavior → invoke /qa or /qa-only
- Code review/diff check → invoke /review
- Visual polish → invoke /design-review
- Ship/deploy/PR → invoke /ship or /land-and-deploy
- Save progress → invoke /context-save
- Resume context → invoke /context-restore
- Author a backlog-ready spec/issue → invoke /spec
