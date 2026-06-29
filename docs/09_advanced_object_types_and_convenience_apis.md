# Step 9: Erweiterte Objekttypen & Convenience-APIs

**Ziel:** Unterstützung für komplexe Objekte aus der Anforderung (Schedule, TrendLog, EventEnrollment, NotificationClass). Da diese komplexe Properties besitzen, werden spezialisierte PHP-Hilfsklassen eingeführt, die das Lesen/Schreiben dieser Strukturen vereinfachen.

## Aktionen

1.  **Enum-Erweiterung (falls nicht bereits in Step 4 geschehen):**
    Stelle sicher, dass `ObjectType` vollständig ist:
    - `NOTIFICATION_CLASS = 15`
    - `SCHEDULE = 17`
    - `TREND_LOG = 20`
    - `EVENT_ENROLLMENT = 9`

2.  **Klasse `Bacnet\ScheduleEntry`:**
    ```php
    namespace Bacnet;
    class ScheduleEntry {
        public function __construct(Time $startTime, mixed $value);
        public function getStartTime(): Time;
        public function getValue(): mixed;
    }
    ```
    - Intern: Speichert `BACNET_DAILY_SCHEDULE`-ähnliche Struktur.
    - Mapping zu BACnet: Jeder Entry ist ein `TimeValue` (BACnetSequenceOf TimeValue).

3.  **Klasse `Bacnet\WeeklySchedule`:**
    ```php
    namespace Bacnet;
    class WeeklySchedule {
        /** @param ScheduleEntry[] $monday ... $sunday */
        public function __construct(array $monday = [], ..., array $sunday = []);
        public function getDay(int $weekday /* 1=Mon..7=Sun */): array;
    }
    ```
    - Convenience auf `Schedule.weekly_schedule` (Property 123 / `WEEKLY_SCHEDULE`).
    - `Device`-/`ObjectRef`-Methode:
      ```php
      public function readWeeklySchedule(): WeeklySchedule;
      public function writeWeeklySchedule(WeeklySchedule $schedule): void;
      ```
    - Implementierung: Liest `WEEKLY_SCHEDULE` (Array-of-Sequence). Jeder Tag ist eine Sequenz von `ScheduleEntry`. Wir mappen dies auf `WeeklySchedule`.

4.  **Klasse `Bacnet\TrendLogRecord`:**
    ```php
    namespace Bacnet;
    class TrendLogRecord {
        public function __construct(
            public readonly DateTime $timestamp, // oder Bacnet\Date + Bacnet\Time
            public readonly mixed $value,
            public readonly int $statusFlags
        );
    }
    ```
    - Convenience: `TrendLog::readLog(?int $start = null, ?int $end = null): array`.
    - Intern: Nutzt `ReadRange`-Service (optional, falls zu komplex: erstmal nur `readProperty(Property::LOG_BUFFER)` als Roh-Array).
    - Für MVP reicht: `ObjectRef::readTrendLog(): TrendLogRecord[]`, das intern `LOG_BUFFER` als Array decodiert.

5.  **EventEnrollment & NotificationClass (Basic):**
    - Für diese reichen die generischen `readProperty`/`writeProperty` mit korrekten Properties.
    - Ergänze wichtige `Property`-Konstanten:
      - `EVENT_TYPE = 37`
      - `NOTIFY_TYPE = 72`
      - `EVENT_PARAMETERS = 83`
      - `OBJECT_PROPERTY_REFERENCE = 78` (für EventEnrollment)
      - `RECIPIENT_LIST = 102` (für NotificationClass)
    - Keine speziellen Convenience-Klassen für MVP nötig, aber dokumentiert.

6.  **Special: Binary/MS Value Convenience:**
    - `BinaryValue` Present-Value schreiben:
      ```php
      $bv->writeActive();   // == writeProperty(PRESENT_VALUE, Value::enumerated(1))
      $bv->writeInactive(); // == writeProperty(PRESENT_VALUE, Value::enumerated(0))
      ```
    - Optional, falls Zeit: `ObjectRef::writePresentValue(mixed $value)` Shortcut.

7.  **Zusätzliche Properties im Enum:**
    Ergänze weitere allgemeine Properties, die für die neuen Objekte relevant sind, z. B.:
    - `RELIABILITY = 103`
    - `NOTIFICATION_CLASS` (bereits vorhanden)
    - `ACK_REQUIRED = 1`
    - `STATE_TEXT = 110`
    - `NUMBER_OF_STATES = 74`

## Akzeptanzkriterien

- [ ] Ein `Schedule`-Objekt erlaubt `readWeeklySchedule()` und liefert `WeeklySchedule`.
- [ ] `writeWeeklySchedule()` encodiert korrekt in BACnet `SequenceOf DailySchedule`.
- [ ] `TrendLog` kann gelesen werden (zumindest Roh-Property, idealerweise als `TrendLogRecord[]`).
- [ ] `EventEnrollment` und `NotificationClass` sind als `ObjectType` verwendbar und generisches Read/Write funktioniert.
- [ ] `BitString` aus Step 5 wird korrekt für `statusFlags` in TrendLogRecords verwendet.
