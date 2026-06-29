# php-bacnet — Implementation Plan

Generated from design doc + 10-step implementation guides.
Last updated: 2026-06-29

**Implementation order:** Steps 1→3→7 (validate socket + Who-Is first), then 2→4→5→6→8→9→10.

---

## Architecture Notes (review findings)

Before coding, lock these design constraints:

- **One Client per process**: `bip_init()` binds a process-global UDP socket in `bacnet-stack`. Only one `Bacnet\Client` instance can hold the socket per PHP process. Document this clearly in README; consider `zend_throw_error` if a second Client is constructed while one exists (singleton guard).
- **Socket persistence**: The socket FD opened by `bip_init()` must never be closed between calls. `php_bacnet_client_destroy` (called only in `free_obj`) is the only place `datalink_cleanup()` runs. This keeps the fd available for COV subscriptions in v0.2.0.
- **whoIs() returns empty array on timeout** — not a `TimeoutException`. Broadcast responses are unreliable; callers check `count($devices)`. Only throw `TimeoutException` for confirmed request timeouts (readProperty/writeProperty).
- **Reject + Abort PDUs**: `DeviceException` must be thrown for all three negative PDU types: Error PDU, Reject PDU, and Abort PDU — not just Error.
- **64-device collector limit**: `php_bacnet_broadcast_and_collect` uses a fixed 64-entry C array for MVP. Document this limit.
- **FPM lifecycle**: Each FPM worker calls `bip_init()` at Client construction. This binds a new UDP socket per request that needs BACnet. Expected overhead for v0.1.0; document as "use a connection pool or persistent process for high-frequency use."

---

## Phase 0 — Foundation (Step 1 + Step 3 wedge)

> Goal: `libbacnet-stack.a` builds; C wrapper compiles; C standalone test sends Who-Is.

- [ ] `git submodule add https://github.com/bacnet-stack/bacnet-stack.git deps/bacnet-stack`
- [ ] Record submodule commit hash in design doc and CLAUDE.md
- [ ] Create `scripts/build-deps.sh` (cmake, static, no apps, no tests)
- [ ] Verify `deps/bacnet-stack/build/src/libbacnet-stack.a` is produced
- [ ] Create empty source files: `config.m4`, `php_bacnet.h`, `bacnet.c`, `src/bacnet_client.c/h`, `src/bacnet_classes.c/h`, `src/bacnet_types.c/h`, `src/bacnet_helpers.c/h`
- [ ] Implement `config.m4` (PHP 8.4+ check, `PHP_ADD_INCLUDE`, `PHP_ADD_LIBRARY_WITH_PATH`, `PHP_NEW_EXTENSION`)
- [ ] Implement `php_bacnet_client` struct (socket fd, port, bind_addr)
- [ ] Implement `php_bacnet_client_create(iface, port)` — calls `bip_init()`, stores fd
- [ ] Implement `php_bacnet_client_destroy()` — calls `datalink_cleanup()`
- [ ] Implement `php_bacnet_send_and_wait()` — NPDU build, `datalink_send_pdu`, receive loop with timeout
- [ ] Implement `php_bacnet_broadcast_and_collect()` — 64-entry fixed array, collect I-Am responses
- [ ] Implement address helpers: `php_bacnet_address_from_ipport` / `php_bacnet_address_to_ipport`
- [ ] Write `tests/c_client_test.c` — standalone C test, sends Who-Is, receives at least one I-Am against `bacserv`
- [ ] Validate: no Valgrind leaks in `create`/`destroy` cycle on C test
- [ ] Validate: `phpize && ./configure --with-bacnet && make` produces `modules/bacnet.so`

**Acceptance:** `modules/bacnet.so` builds. C test finds a BACnet device against local simulator.

---

## Phase 1 — Extension Skeleton (Step 2)

> Goal: `phpinfo()` shows `bacnet` section with INI values.

- [ ] Implement `php_bacnet.h` — `extern zend_module_entry`, `PHP_BACNET_VERSION "0.1.0"`, all `extern zend_class_entry *` pointers
- [ ] Implement INI entries in `bacnet.c`: `bacnet.default_port` (47808), `bacnet.default_timeout_ms` (3000), `bacnet.default_interface` ("0.0.0.0")
- [ ] Implement `zend_bacnet_globals` struct + `ZEND_DECLARE_MODULE_GLOBALS(bacnet)` (NTS, no ZTS guards)
- [ ] Implement `PHP_MINIT_FUNCTION(bacnet)`: register INI, call `php_bacnet_register_classes()` stub
- [ ] Implement `PHP_MSHUTDOWN_FUNCTION(bacnet)`: `UNREGISTER_INI_ENTRIES`
- [ ] Implement `PHP_RINIT_FUNCTION(bacnet)`: `BACNET_G(next_invoke_id) = 1`
- [ ] Implement `PHP_MINFO_FUNCTION(bacnet)`: version, build info, INI table
- [ ] Run `PHP_VERSION_ID` guard check for 8.4 vs 8.5 Zend API delta (document any `#if PHP_VERSION_ID >= 80500` guards needed)

**Acceptance:** `php -d extension=modules/bacnet.so -r "echo INI_GET('bacnet.default_timeout_ms');"` outputs `3000`.

---

## Phase 2 — Who-Is / Device Discovery (Step 7)

> Goal: `whoIs()` returns `Device[]` against a headless simulator.

- [ ] Implement `Client::whoIs(?int $lowLimit, ?int $highLimit, ?int $timeoutMs): array`
- [ ] Build Who-Is APDU via `WhoIs_encode_apdu()` from `bacnet/whois.h`
- [ ] Set NPDU for unconfirmed broadcast: `data_expecting_reply = false`, `dest` = BROADCAST_NETWORK
- [ ] Call `datalink_send_pdu()` with broadcast address (`255.255.255.255:47808`)
- [ ] Implement collector loop in `php_bacnet_broadcast_and_collect`: receive until `timeoutMs` elapsed
- [ ] Filter: only `PDU_TYPE_UNCONFIRMED_SERVICE` + `SERVICE_CHOICE_I_AM` accepted
- [ ] Decode I-Am via `iam_decode_service_request()` → `device_id`, `max_apdu`, `segmentation`, `vendor_id`
- [ ] Deduplicate by `device_id` (simple C array or HashTable)
- [ ] Create `Bacnet\Device` Zend object for each unique device: copy `BACNET_ADDRESS`, set `device_id`, `ZVAL_COPY` of Client
- [ ] Return PHP array of `Device` objects (empty array on timeout — no exception)
- [ ] Skip malformed I-Am entries silently; continue collecting
- [ ] Validate GH Actions UDP socket: test `bacserv` headless on Ubuntu runner before enabling PHPT network tests

**Acceptance:** `$client->whoIs()` returns `Bacnet\Device[]` against local `bacserv`.

---

## Phase 3 — OOP Model & Zend Classes (Step 4)

> Goal: All PHP classes, enums, exceptions register in MINIT without crash.

- [ ] Implement `php_bacnet_register_classes()` in `src/bacnet_classes.c`
- [ ] Register `Bacnet\Client` with `create_object` / `free_obj` handlers
- [ ] Register `Bacnet\Device` with embedded `BACNET_ADDRESS` + `device_id` + `client_zval`
- [ ] Register `Bacnet\ObjectRef` with `object_type`, `instance`, `device_zval`
- [ ] Register `Bacnet\ObjectIdentifier` (immutable: `type`, `instance`)
- [ ] Register `Bacnet\Value` with internal `BACNET_APPLICATION_DATA_VALUE`
- [ ] Register `Bacnet\BitString`, `Bacnet\Date`, `Bacnet\Time`
- [ ] Register `enum Bacnet\ObjectType: int` — ANALOG_INPUT=0, ANALOG_OUTPUT=1, ANALOG_VALUE=2, BINARY_INPUT=3, BINARY_OUTPUT=4, BINARY_VALUE=5, DEVICE=8, EVENT_ENROLLMENT=9, MULTI_STATE_INPUT=13, MULTI_STATE_OUTPUT=14, NOTIFICATION_CLASS=15, SCHEDULE=17, MULTI_STATE_VALUE=19, TREND_LOG=20
- [ ] Register `enum Bacnet\Property: int` — OBJECT_IDENTIFIER=75, OBJECT_NAME=77, OBJECT_TYPE=79, DESCRIPTION=28, PRESENT_VALUE=85, STATUS_FLAGS=111, EVENT_STATE=36, OUT_OF_SERVICE=81, UNITS=117, NUMBER_OF_STATES=74, STATE_TEXT=110, NOTIFICATION_CLASS=17 + Step 9 additions
- [ ] Register `Bacnet\Exception` (extends `\Exception`)
- [ ] Register `Bacnet\TimeoutException` (extends `Bacnet\Exception`)
- [ ] Register `Bacnet\DeviceException` (extends `Bacnet\Exception`) + `zend_declare_property_long` for `errorClass`, `errorCode`
- [ ] Add method stubs (`ZEND_BEGIN_ARG_INFO_EX`) for all public methods — body: `RETURN_NULL()` for now
- [ ] Implement `free_obj` for all handlers: call `php_bacnet_client_destroy`, `zval_ptr_dtor` on parent refs, `zend_object_std_dtor`

**Acceptance:** `new Bacnet\Client()`, `new Bacnet\ObjectIdentifier(Bacnet\ObjectType::ANALOG_VALUE, 1)`, `Bacnet\ObjectType::SCHEDULE->value === 17` all work.

---

## Phase 4 — ReadProperty & Type Mapping (Step 5)

> Goal: `readProperty()` returns typed PHP values; exceptions on error/timeout.

- [ ] Implement `Device::readProperty(ObjectType $objectType, int $instance, Property $property, ?int $arrayIndex = null): mixed`
- [ ] Implement `ObjectRef::readProperty(Property $property, ?int $arrayIndex = null): mixed`
- [ ] Build ReadProperty APDU via `rp_encode_apdu()` with `BACNET_READ_PROPERTY_DATA`
- [ ] Get `invoke_id` from `BACNET_G(next_invoke_id)` (post-increment); use `php_bacnet_send_and_wait()`
- [ ] Decode Complex-ACK: `rp_ack_decode_service_request()` → iterate `bacapp_decode_application_data()`
- [ ] Throw `Bacnet\DeviceException` on Error PDU: `error_decode_service_request()` → `errorClass`, `errorCode`
- [ ] Throw `Bacnet\DeviceException` on Reject PDU and Abort PDU (with descriptive message)
- [ ] Throw `Bacnet\TimeoutException` when `php_bacnet_send_and_wait()` returns -1 (timeout)
- [ ] Implement `bacapp_value_to_zval()` in `src/bacnet_helpers.c`:
  - BOOLEAN → `IS_TRUE`/`IS_FALSE`
  - UNSIGNED_INT / SIGNED_INT → `IS_LONG`
  - REAL → `IS_DOUBLE`
  - ENUMERATED → `IS_LONG`
  - CHARACTER_STRING → `IS_STRING`
  - OCTET_STRING → `IS_STRING` (binary)
  - BIT_STRING → `new Bacnet\BitString`
  - DATE → `new Bacnet\Date`
  - TIME → `new Bacnet\Time`
  - OBJECT_IDENTIFIER → `new Bacnet\ObjectIdentifier`
  - Sequences → `IS_ARRAY`
- [ ] Implement `Bacnet\BitString`: constructor `array<int,bool>`, `getBit(int): bool`, `toArray(): array`, readonly `length`
- [ ] Implement `Bacnet\Date`: `__construct(int $year, int $month, int $day, int $weekday)` (0xFF = wildcard), readonly properties
- [ ] Implement `Bacnet\Time`: `__construct(int $hour, int $minute, int $second, int $hundredths)`, readonly properties
- [ ] Implement `Bacnet\ObjectIdentifier`: `__construct(ObjectType $type, int $instance)`, readonly `type`, `instance`

**Acceptance:** Float from AnalogValue.PRESENT_VALUE, array of ObjectIdentifier from Device.OBJECT_LIST, DeviceException on unknown-object error, TimeoutException after 3000ms.

---

## Phase 5 — WriteProperty & Value Class (Step 6)

> Goal: `writeProperty()` encodes and sends typed BACnet values.

- [ ] Implement `Bacnet\Value` class with factory methods: `boolean()`, `unsignedInt()`, `signedInt()`, `real()`, `enumerated()`, `characterString()`, `bitString()`, `date()`, `time()`, `objectIdentifier()`
- [ ] Store `BACNET_APPLICATION_DATA_VALUE` + `uint8_t tag` in `php_bacnet_value_obj`
- [ ] Implement `zval_to_bacapp_value()` helper (inverse of `bacapp_value_to_zval`) in `src/bacnet_helpers.c`
- [ ] Build WriteProperty APDU via `wp_encode_apdu()` from `bacnet/wp.h`
- [ ] Fill `BACNET_WRITE_PROPERTY_DATA`: object_type, object_instance, property, array_index, priority, value
- [ ] Implement `Client::writeProperty(ObjectIdentifier $deviceId, ObjectIdentifier $objectId, Property $property, Value $value, int $priority = 16, ?int $arrayIndex = null): void`
- [ ] Implement `Device::writeProperty(ObjectType, int, Property, Value, int, ?int): void`
- [ ] Implement `ObjectRef::writeProperty(Property, Value, int, ?int): void`
- [ ] Throw `Bacnet\DeviceException` / `Bacnet\TimeoutException` per Phase 4 pattern
- [ ] Add `Value::binaryActive()` and `Value::binaryInactive()` convenience shortcuts

**Acceptance:** `writeProperty` with `Value::real(21.3)` succeeds; DeviceException on write-access-denied.

---

## Phase 6 — Docker Distribution (design doc Next Step 5)

> Goal: `docker build` produces a working image; validate primary distribution target early.

- [ ] Create `Dockerfile` — `FROM php:8.4-fpm`, install build-essential + cmake, copy repo, run `scripts/build-deps.sh`, phpize/configure/make, `extension=bacnet.so` in php.ini
- [ ] Create `docker-compose.yml` with a demo service + bacserv simulator service
- [ ] Confirm Docker Hub namespace (`php-bacnet` or personal — resolve Open Question 5)
- [ ] Add `.github/workflows/docker.yml` step to build and push image on tagged release
- [ ] Write `demo.php` — `whoIs()` + `readProperty()` against simulator, `var_dump` output
- [ ] Test: `docker build . && docker run php-bacnet php demo.php` produces Device objects

**Acceptance:** Docker image builds and `demo.php` runs end-to-end in one command.

---

## Phase 7 — Server Mode & Event Loop (Step 8)

> Goal: `Bacnet\Server` responds to Who-Is and ReadProperty from external tools.

- [ ] Create `src/bacnet_server.h/c`
- [ ] Define `php_bacnet_server_obj` struct: `php_bacnet_client *client`, `device_id`, `read_handler` / `write_handler` (zend_fcall_info_cache), `local_objects` HashTable, `auto_iam` flag
- [ ] Implement `Server::__construct(int $deviceId, string $bindInterface, int $port)`
- [ ] Implement `Server::addLocalObject(ObjectIdentifier $oid): void` — insert into `local_objects` HashTable
- [ ] Implement `Server::removeLocalObject(ObjectIdentifier $oid): void`
- [ ] Implement `Server::onReadProperty(callable $handler): void` — store `zend_fcall_info_cache`
- [ ] Implement `Server::onWriteProperty(callable $handler): void`
- [ ] Implement `Server::setAutoIAm(bool $enabled): void`
- [ ] Implement `Server::poll(int $timeoutMs = 0): void`:
  - `datalink_receive()` with `timeoutMs`
  - Parse NPDU/APDU
  - Who-Is → auto I-Am reply if `auto_iam` and device_id in range
  - ReadProperty → check `local_objects`, call `read_handler`, build Complex-ACK
  - WriteProperty → call `write_handler`
  - Missing handler → send Error PDU (`unknown-object`)
- [ ] Implement `zval_to_bacapp_value()` path for encoding PHP callback return values
- [ ] Validate: `poll(0)` is non-blocking

**Acceptance:** External YABE or bacnet-stack Who-Is finds the PHP Server; ReadProperty callback fires and response encodes correctly.

---

## Phase 8 — Advanced Object Types (Step 9)

> Goal: Schedule, TrendLog, EventEnrollment convenience APIs.

- [ ] Implement `Bacnet\ScheduleEntry`: `__construct(Time $startTime, mixed $value)`, `getStartTime()`, `getValue()`
- [ ] Implement `Bacnet\WeeklySchedule`: `__construct(array ...$days)`, `getDay(int $weekday): array`
- [ ] Implement `ObjectRef::readWeeklySchedule(): WeeklySchedule` (reads `WEEKLY_SCHEDULE` property)
- [ ] Implement `ObjectRef::writeWeeklySchedule(WeeklySchedule $schedule): void`
- [ ] Implement `Bacnet\TrendLogRecord`: readonly `DateTime $timestamp`, `mixed $value`, `int $statusFlags`
- [ ] Implement `ObjectRef::readTrendLog(): TrendLogRecord[]` (reads `LOG_BUFFER`, decodes sequence)
- [ ] Add Property enum cases: `EVENT_TYPE=37`, `NOTIFY_TYPE=72`, `EVENT_PARAMETERS=83`, `OBJECT_PROPERTY_REFERENCE=78`, `RECIPIENT_LIST=102`, `RELIABILITY=103`, `ACK_REQUIRED=1`, `WEEKLY_SCHEDULE=123`, `LOG_BUFFER` (see bacnet-stack headers for value)
- [ ] Add `ObjectRef::writeActive()` / `writeInactive()` shortcuts for binary objects

**Acceptance:** Schedule read/write round-trips; TrendLog decodes as `TrendLogRecord[]`; EventEnrollment + NotificationClass usable with generic readProperty/writeProperty.

---

## Phase 9 — Testing, QA & Release (Step 10)

> Goal: CI green, Valgrind clean, PECL package installable.

### PHPT Tests (`tests/`)
- [ ] `001_client_construct.phpt` — construct + destruct, no leak
- [ ] `002_whois_discovery.phpt` — against local bacserv (SKIPIF if no simulator)
- [ ] `003_read_property.phpt` — AnalogValue.PRESENT_VALUE → float
- [ ] `004_write_property.phpt` — write + read-back verify
- [ ] `005_complex_types.phpt` — BitString, Date, Time, ObjectIdentifier decode
- [ ] `006_server_poll.phpt` — Server + poll + callback roundtrip
- [ ] `007_exceptions.phpt` — TimeoutException + DeviceException thrown correctly
- [ ] Write `tests/README.md` — how to start `bacserv` simulator

### Valgrind
- [ ] CI: 100-cycle `create`/`destroy` Valgrind run — 0 definite leaks
- [ ] Nightly: 1000-cycle run on pre-release
- [ ] Fix all `definite lost`: `zend_string`, `zval_ptr_dtor`, `efree` vs `pefree` audit

### GitHub Actions (`.github/workflows/ci.yml`)
- [ ] Matrix: PHP 8.4 + 8.5, Ubuntu latest
- [ ] Steps: checkout (submodules recursive) → deps → phpize → build → PHPT → Valgrind
- [ ] Validate: bacserv runs headlessly on GH Actions Ubuntu runner (Open Question 1)
- [ ] On tagged release: build .so artifacts, push Docker image, publish GitHub Release

### IDE Stubs & Docs
- [ ] Create `stubs/bacnet.stub.php` — all classes, enums, methods, PHPDoc, typed
- [ ] Run PHPStan level 8 against stubs — 0 errors
- [ ] Write `README.md` — install (submodules + compile), minimal Who-Is + ReadProperty example
- [ ] Write `API.md` — all classes and methods documented
- [ ] Write `CHANGELOG.md` — v0.1.0 initial release notes
- [ ] Create `LICENSE` (MIT or BSD-3, consistent with bacnet-stack license)

### PECL / PIE
- [ ] Create `package.xml` — name=bacnet, version=0.1.0, lead, PHP 8.4+ dep
- [ ] Test `pecl install bacnet` from release artifact
- [ ] PIE: best-effort (pin PIE version in CI, not a release gate)
- [ ] Publish GitHub Releases with pre-built `.so` for PHP 8.4 + 8.5 on Ubuntu 22.04 + 24.04

**Acceptance:** `make test` passes. Valgrind: 0 leaks. `pecl install bacnet` works. PHPStan stubs: level 8 clean.

---

## Open Questions to Resolve

| # | Question | Status |
|---|----------|--------|
| 1 | GH Actions UDP socket — validate `bacserv` headless on Ubuntu runner | ❓ pending |
| 2 | PIE format — pin version, treat as best-effort | ✅ decided |
| 3 | PHP 8.5 Zend API delta — run `PHP_VERSION_ID` guards in Phase 1 | ❓ pending |
| 4 | bacnet-stack submodule commit hash — record after submodule add | ❓ pending |
| 5 | Docker Hub namespace `php-bacnet` — confirm availability | ❓ pending |
| 6 | fd() / COV readiness — socket stays persistent; document in `src/bacnet_client.h` | ✅ decided |

---

## Not in Scope for v0.1.0

- ZTS (thread-safe) support
- Windows / macOS builds
- COV (Change of Value) subscriptions — socket design is ready, API ships in v0.2.0
- Multiple simultaneous `Client` instances per process (singleton enforced)
- ReadRange service (TrendLog uses `readProperty(LOG_BUFFER)` for MVP)
- Async/non-blocking client reads
- PHP 8.3 or earlier
