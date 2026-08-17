# php-bacnet Tests

## Running the Test Suite

```bash
# Run all tests (requires the extension to be built first)
php run-tests.php -d extension=modules/bacnet.so tests/

# Run a single test
php run-tests.php -d extension=modules/bacnet.so tests/004_enums.phpt

# Run with verbose diff output
php run-tests.php -d extension=modules/bacnet.so --show-diff tests/
```

## Test Categories

| File | Requires network | Description |
|------|-----------------|-------------|
| `001_client_construct.phpt` | No | Client class API, INI settings, singleton guard |
| `002_whois_no_network.phpt` | No | Who-Is method signature and SKIPIF guard |
| `003_exceptions.phpt`       | No | Exception hierarchy, DeviceException properties |
| `004_enums.phpt`            | No | ObjectType/Property enum cases, ObjectIdentifier |
| `005_read_property_types.phpt` | No | BitString, Date, Time, ObjectRef API |
| `006_write_property.phpt`   | No | Value factory methods, writeProperty signatures |
| `007_server_mode.phpt`      | Optional | Server class API; tries real init, falls back to structural tests |
| `008_advanced_types.phpt`   | No | ScheduleEntry, WeeklySchedule, TrendLogRecord, ObjectRef shortcuts |
| `009_mixed_server.phpt`     | No | MixedServer inheritance, client API and queue status |
| `010_mixed_server_lifecycle.phpt` | Optional | MixedServer callbacks, socket lifecycle and singleton guard |

## Network-Dependent Tests

Tests that actually communicate over BACnet (Who-Is, ReadProperty, WriteProperty) require a
reachable BACnet device or simulator on the local network.

### Option A — bacnet-stack demo device

Build and run the included demo device from the bacnet-stack submodule:

```bash
cd deps/bacnet-stack
cmake -B build-demo -DBUILD_SHARED_LIBS=OFF
cmake --build build-demo --target bacserv
# Start the demo server on port 47808
./build-demo/apps/server/bacserv 1234 &
```

### Option A — YABE (Yet Another BACnet Explorer)

YABE is a free Windows/Wine BACnet explorer that also includes a simulator.
Download from https://sourceforge.net/projects/yetanotherbacnetexplorer/

### Option B — Docker test server

See `tests/bacnet-testserver/` if present — a minimal containerised BACnet device
for integration tests.

### SKIPIF guards

All tests that require network access contain a `--SKIPIF--` section that checks
for a `BACNET_TEST_HOST` environment variable:

```bash
# Enable network tests by pointing at a real device
BACNET_TEST_DEVICE_ID=1234 BACNET_TEST_HOST=192.168.1.100 \
  php run-tests.php -d extension=modules/bacnet.so tests/
```

### Mixed-Modus-Integrationstest

`integration_mixed.php` startet einen MixedServer, registriert einen lokalen
Testwert, entdeckt ein Zielgerät und liest eine Property über denselben Socket:

```bash
BACNET_TEST_INTERFACE=net3 \
BACNET_TEST_LOCAL_DEVICE_ID=5 \
BACNET_TEST_DEVICE_ID=200 \
BACNET_TEST_OBJECT_TYPE=analog_value \
BACNET_TEST_INSTANCE=10110 \
BACNET_TEST_PROPERTY=present_value \
php tests/integration_mixed.php
```

Ohne Objektangaben liest das Script standardmäßig `OBJECT_NAME` des
Device-Objekts. Es gibt außerdem den Queue-Stand aus und leert gepufferte PDUs.

## Memory-Leak Check

```bash
ZEND_DONT_UNLOAD_MODULES=1 valgrind \
  --leak-check=full \
  --show-leak-kinds=definite,indirect \
  --error-exitcode=1 \
  php -d extension=modules/bacnet.so \
  run-tests.php \
  tests/001_client_construct.phpt \
  tests/003_exceptions.phpt \
  tests/004_enums.phpt \
  tests/005_read_property_types.phpt \
  tests/006_write_property.phpt \
  tests/008_advanced_types.phpt
```

Target: **0 definite leaks** for normal create/destroy cycles.
