# Changelog

All notable changes to php-bacnet are documented here.

## Unreleased

### Added

- POSIX-Shared-Memory-L1 für Client und MixedServer sowie LMDB als persistentes
  Standard-L2 ergänzt.
- `CacheBackendInterface`, Cacheoptionen, Statistiken, Refresh-Parameter und
  getrennte State-, Objekt-, Objektlisten-, Device-, IP- und Negative-Caches
  ergänzt; beliebige PHP-Backends können LMDB pro Instanz ersetzen.

- `Bacnet\Server` und `Bacnet\MixedServer` schützen eingehende Pakete durch
  IPv4-CIDR-ACLs, globale und quellbezogene Token-Buckets, dienstspezifische
  Who-Is-/Write-Limits, temporäre Quellsperren und Write-Deduplizierung.
- `setSecurityOptions()`, `getSecurityOptions()` und `getSecurityStats()` sowie
  `bacnet.server_*`-INI-Direktiven konfigurieren und beobachten den Schutz.
- MixedServer prüft Pakete vor seiner 32-PDU-Queue; Sicherheitsdemo sowie API-,
  INI- und Netzwerk-Integrationstests dokumentieren den Betrieb.

- `Bacnet\MixedServer` kombiniert die bestehende Server-API mit `whoIs()` und
  verwendet für gefundene Geräte denselben BACnet/IP-Socket. Eingehende
  Server-PDUs während synchroner Client-Aufrufe werden bis zum nächsten
  `poll()` gepuffert; `getPendingPduCount()` meldet den Queue-Stand.
- Mixed-Modus-Leitfaden, ausführbare Daemon-/Read-Demos sowie Struktur-,
  Lifecycle- und Netzwerk-Integrationstests.

### Changed

- `Client::whoIs()` retries one empty discovery after 250 ms to handle an
  occasionally missed initial Who-Is broadcast.

## [0.1.0] — 2026-06-29

### Added

**Core infrastructure**
- `config.m4` + `scripts/build-deps.sh`: builds `bacnet-stack` as a static `-fPIC` library via CMake
- `bacnet.c` / `php_bacnet.h`: module lifecycle (MINIT/MSHUTDOWN/RINIT/RSHUTDOWN/MINFO), INI settings (`bacnet.default_port`, `bacnet.default_timeout_ms`, `bacnet.default_interface`)

**Client mode**
- `Bacnet\Client` — opens one BACnet/IP UDP socket per PHP process; singleton guard prevents double-init
- `Client::whoIs(?int, ?int, ?int): Device[]` — Who-Is broadcast + I-Am collector (up to 64 devices)
- `Bacnet\Device` — wraps a discovered device's BACNET_ADDRESS and device ID
- `Device::readProperty(ObjectType, int, Property, ?int): mixed`
- `Device::writeProperty(ObjectType, int, Property, Value, int, ?int): void`
- `Bacnet\ObjectRef` — convenience handle combining Device + (ObjectType, instance)
- `ObjectRef::readProperty(Property, ?int): mixed`
- `ObjectRef::writeProperty(Property, Value, int, ?int): void`
- `ObjectRef::writeActive(): void` / `writeInactive(): void` — PRESENT_VALUE shortcuts
- `ObjectRef::writePresentValue(mixed): void` — auto-tags native PHP types
- `ObjectRef::readTrendLog(): TrendLogRecord[]` — reads LOG_BUFFER property
- `ObjectRef::readWeeklySchedule(): WeeklySchedule` — reads WEEKLY_SCHEDULE property
- `ObjectRef::writeWeeklySchedule(WeeklySchedule): void` — stub (sequence encoding TODO)

**Type system**
- `Bacnet\Value` — factory class: `boolean`, `unsignedInt`, `signedInt`, `real`, `enumerated`, `characterString`, `bitString`, `date`, `time`, `objectIdentifier`
- `Bacnet\BitString` — wraps BACnet BIT STRING (up to 120 bits); `getBit`, `getLength`, `toArray`
- `Bacnet\Date` — BACnet DATE with year/month/day/weekday (0xFF = wildcard)
- `Bacnet\Time` — BACnet TIME with hour/minute/second/hundredths
- `Bacnet\ObjectIdentifier` — immutable (ObjectType enum, instance int) pair

**Enums**
- `Bacnet\ObjectType: int` — 14 cases: ANALOG_INPUT, ANALOG_OUTPUT, ANALOG_VALUE, BINARY_INPUT, BINARY_OUTPUT, BINARY_VALUE, DEVICE, EVENT_ENROLLMENT, MULTI_STATE_INPUT, MULTI_STATE_OUTPUT, NOTIFICATION_CLASS, SCHEDULE, MULTI_STATE_VALUE, TREND_LOG
- `Bacnet\Property: int` — 30 cases covering all standard properties needed for the supported object types

**Exceptions**
- `Bacnet\Exception` → `TimeoutException`, `DeviceException` (adds `errorClass` / `errorCode`)

**Server mode**
- `Bacnet\Server` — non-blocking BACnet/IP server with PHP callbacks
- `Server::addLocalObject(ObjectIdentifier): void` / `removeLocalObject`
- `Server::onReadProperty(callable): void` — called on incoming ReadProperty requests
- `Server::onWriteProperty(callable): void` — called on incoming WriteProperty requests
- `Server::setAutoIAm(bool): void` — automatic I-Am response to Who-Is
- `Server::poll(int $timeoutMs = 0): void` — processes one PDU per call

**Schedule / TrendLog helpers**
- `Bacnet\ScheduleEntry` — (Time, mixed) value pair; `getStartTime()`, `getValue()`
- `Bacnet\WeeklySchedule` — 7-day array container; `getDay(1..7)`
- `Bacnet\TrendLogRecord` — (timestamp, value, statusFlags) record from LOG_BUFFER

**Release artifacts**
- `stubs/bacnet.stub.php` — complete IDE/PHPStan stubs
- `.github/workflows/ci.yml` — GitHub Actions CI with PHP 8.4, CMake build, PHPT suite
- `tests/README.md` — test setup guide including BACnet simulator instructions
- `package.xml` — PECL package descriptor

### Notes

- NTS only (no ZTS/thread-safety required)
- Requires PHP 8.4+ dev headers, `build-essential`, `cmake`
- Linux only (GCC/Autotools); BACnet/IP over IPv4
- bacnet-stack pinned at `5afc5c9a` (1.5.0-109)
- `writeWeeklySchedule` encoding is not yet implemented (throws `Bacnet\Exception`)
- `readWeeklySchedule` / `readTrendLog` return raw/empty data for complex BACnet sequences (full ReadRange decoding is planned for 0.2.0)
