<?php

/**
 * php-bacnet — IDE/PHPStan stubs for the bacnet extension.
 * Generated for version 0.1.0.  Do not execute directly.
 *
 * @phpstan-type BacnetValue scalar|bool|null|Bacnet\BitString|Bacnet\Date|Bacnet\Time|Bacnet\ObjectIdentifier
 */

namespace Bacnet {

    /* ── Enums ─────────────────────────────────────────────────────────── */

    enum ObjectType: int
    {
        case ANALOG_INPUT       = 0;
        case ANALOG_OUTPUT      = 1;
        case ANALOG_VALUE       = 2;
        case BINARY_INPUT       = 3;
        case BINARY_OUTPUT      = 4;
        case BINARY_VALUE       = 5;
        case DEVICE             = 8;
        case EVENT_ENROLLMENT   = 9;
        case MULTI_STATE_INPUT  = 13;
        case MULTI_STATE_OUTPUT = 14;
        case NOTIFICATION_CLASS = 15;
        case SCHEDULE           = 17;
        case MULTI_STATE_VALUE  = 19;
        case TREND_LOG          = 20;
    }

    enum Property: int
    {
        case ACK_REQUIRED               = 1;
        case DESCRIPTION                = 28;
        case EFFECTIVE_PERIOD           = 32;
        case EVENT_STATE                = 36;
        case EVENT_TYPE                 = 37;
        case EXCEPTION_SCHEDULE         = 38;
        case NOTIFY_TYPE                = 72;
        case NUMBER_OF_STATES           = 74;
        case OBJECT_IDENTIFIER          = 75;
        case OBJECT_LIST                = 76;
        case OBJECT_NAME                = 77;
        case OBJECT_PROPERTY_REFERENCE  = 78;
        case OBJECT_TYPE                = 79;
        case OUT_OF_SERVICE             = 81;
        case EVENT_PARAMETERS           = 83;
        case PRESENT_VALUE              = 85;
        case PRIORITY_ARRAY             = 87;
        case RECIPIENT_LIST             = 102;
        case RELIABILITY                = 103;
        case RELINQUISH_DEFAULT         = 104;
        case STATE_TEXT                 = 110;
        case STATUS_FLAGS               = 111;
        case UNITS                      = 117;
        case WEEKLY_SCHEDULE            = 123;
        case LOG_BUFFER                 = 131;
        case LOG_DEVICE_OBJECT_PROPERTY = 132;
        case RECORD_COUNT               = 141;
        case TOTAL_RECORD_COUNT         = 145;
        case NOTIFICATION_CLASS         = 17;
        case SCHEDULE_DEFAULT           = 174;
    }

    /* ── Exceptions ────────────────────────────────────────────────────── */

    class Exception extends \Exception {}

    class TimeoutException extends Exception {}

    class DeviceException extends Exception
    {
        public int $errorClass = 0;
        public int $errorCode  = 0;
    }

    /* ── Value types ────────────────────────────────────────────────────── */

    /**
     * Immutable typed BACnet application-data value used for WriteProperty.
     * All factory methods return a new Value instance.
     */
    final class Value
    {
        public static function boolean(bool $value): self {}
        public static function unsignedInt(int $value): self {}
        public static function signedInt(int $value): self {}
        public static function real(float $value): self {}
        public static function enumerated(int $value): self {}
        public static function characterString(string $value): self {}
        public static function bitString(BitString $value): self {}
        public static function date(Date $value): self {}
        public static function time(Time $value): self {}
        public static function objectIdentifier(ObjectIdentifier $value): self {}
    }

    /**
     * BACnet BIT STRING — up to 120 bits, MSB-first per byte.
     */
    final class BitString
    {
        public int $length = 0;

        /** @param bool[] $bits */
        public function __construct(array $bits) {}

        public function getBit(int $index): bool {}
        public function getLength(): int {}

        /** @return bool[] */
        public function toArray(): array {}
    }

    /**
     * BACnet DATE — year/month/day/weekday.
     * Use 0xFF for wildcard fields.
     */
    final class Date
    {
        public int $year;
        public int $month;
        public int $day;
        public int $weekday;

        public function __construct(int $year, int $month, int $day, int $weekday) {}
    }

    /**
     * BACnet TIME — hour/minute/second/hundredths.
     */
    final class Time
    {
        public int $hour;
        public int $minute;
        public int $second;
        public int $hundredths;

        public function __construct(int $hour, int $minute, int $second, int $hundredths) {}
    }

    /**
     * BACnet OBJECT IDENTIFIER — (ObjectType, instance) pair.
     */
    final class ObjectIdentifier
    {
        public ?ObjectType $type;
        public int $instance;

        public function __construct(ObjectType $type, int $instance) {}
    }

    /* ── Schedule helpers ─────────────────────────────────────────────── */

    /**
     * One TimeValue entry in a BACnet DailySchedule.
     */
    class ScheduleEntry
    {
        public Time $startTime;
        public mixed $value;

        public function __construct(Time $startTime, mixed $value) {}
        public function getStartTime(): Time {}
        public function getValue(): mixed {}
    }

    /**
     * BACnet WeeklySchedule — seven days of ScheduleEntry arrays.
     */
    class WeeklySchedule
    {
        /** @var ScheduleEntry[] */
        public ?array $monday    = [];
        /** @var ScheduleEntry[] */
        public ?array $tuesday   = [];
        /** @var ScheduleEntry[] */
        public ?array $wednesday = [];
        /** @var ScheduleEntry[] */
        public ?array $thursday  = [];
        /** @var ScheduleEntry[] */
        public ?array $friday    = [];
        /** @var ScheduleEntry[] */
        public ?array $saturday  = [];
        /** @var ScheduleEntry[] */
        public ?array $sunday    = [];

        /**
         * @param ScheduleEntry[] $monday
         * @param ScheduleEntry[] $tuesday
         * @param ScheduleEntry[] $wednesday
         * @param ScheduleEntry[] $thursday
         * @param ScheduleEntry[] $friday
         * @param ScheduleEntry[] $saturday
         * @param ScheduleEntry[] $sunday
         */
        public function __construct(
            array $monday    = [],
            array $tuesday   = [],
            array $wednesday = [],
            array $thursday  = [],
            array $friday    = [],
            array $saturday  = [],
            array $sunday    = [],
        ) {}

        /**
         * @param int $weekday 1=Mon .. 7=Sun
         * @return ScheduleEntry[]
         */
        public function getDay(int $weekday): array {}
    }

    /**
     * One record decoded from a TrendLog's LOG_BUFFER property.
     */
    class TrendLogRecord
    {
        public mixed $timestamp   = null;
        public mixed $value       = null;
        public int   $statusFlags = 0;

        public function __construct(mixed $timestamp, mixed $value, int $statusFlags) {}
    }

    /* ── Core classes ─────────────────────────────────────────────────── */

    /**
     * BACnet/IP client — manages one UDP socket per PHP process.
     * Only one Client (or Server) may exist at a time.
     */
    class Client
    {
        /**
         * @param string|null $interface  Network interface name (e.g. "eth0") or null for auto-detect.
         * @param int|null    $port       UDP port (default: bacnet.default_port INI, typically 47808).
         * @param int|null    $timeoutMs  Request timeout in ms (default: bacnet.default_timeout_ms INI).
         * @throws \Error if a Client/Server already exists in this process.
         */
        public function __construct(
            ?string $interface = null,
            ?int    $port      = null,
            ?int    $timeoutMs = null,
        ) {}

        /**
         * Broadcast Who-Is and collect I-Am responses.
         *
         * @param int|null $lowLimit   Device instance lower bound (null = 0).
         * @param int|null $highLimit  Device instance upper bound (null = 4194303).
         * @param int|null $timeoutMs  Override the default timeout.
         * @return Device[]
         * @throws TimeoutException
         */
        public function whoIs(
            ?int $lowLimit  = null,
            ?int $highLimit = null,
            ?int $timeoutMs = null,
        ): array {}
    }

    /**
     * A discovered BACnet device.  Obtained from Client::whoIs().
     * Retains a reference to the originating Client.
     */
    class Device
    {
        /**
         * Read a single property from any object on this device.
         *
         * @throws TimeoutException
         * @throws DeviceException
         */
        public function readProperty(
            ObjectType $objectType,
            int        $instance,
            Property   $property,
            ?int       $arrayIndex = null,
        ): mixed {}

        /**
         * Write a property on any object on this device.
         *
         * @param int  $priority   Write priority 1 (highest) .. 16 (lowest, default).
         * @throws TimeoutException
         * @throws DeviceException
         */
        public function writeProperty(
            ObjectType $objectType,
            int        $instance,
            Property   $property,
            Value      $value,
            int        $priority   = 16,
            ?int       $arrayIndex = null,
        ): void {}
    }

    /**
     * A specific object on a BACnet device.
     * Wraps a Device + (ObjectType, instance) for convenient repeated access.
     */
    class ObjectRef
    {
        public function __construct(Device $device, ObjectType $type, int $instance) {}

        /**
         * @throws TimeoutException
         * @throws DeviceException
         */
        public function readProperty(Property $property, ?int $arrayIndex = null): mixed {}

        /**
         * @param int  $priority   1 (highest) .. 16 (lowest, default).
         * @throws TimeoutException
         * @throws DeviceException
         */
        public function writeProperty(
            Property $property,
            Value    $value,
            int      $priority   = 16,
            ?int     $arrayIndex = null,
        ): void {}

        /**
         * Shortcut: write PRESENT_VALUE = enumerated(1).
         * Suitable for BinaryValue/BinaryOutput objects.
         */
        public function writeActive(): void {}

        /**
         * Shortcut: write PRESENT_VALUE = enumerated(0).
         */
        public function writeInactive(): void {}

        /**
         * Write PRESENT_VALUE from a native PHP value or Bacnet\Value.
         * Accepted types: bool, int, float, string, null, Value, BitString, Date, Time, ObjectIdentifier.
         */
        public function writePresentValue(mixed $value): void {}

        /**
         * Read LOG_BUFFER and return decoded entries.
         *
         * @return TrendLogRecord[]
         */
        public function readTrendLog(): array {}

        /**
         * Read WEEKLY_SCHEDULE.
         * Full sequence decoding returns an empty WeeklySchedule in the current version.
         */
        public function readWeeklySchedule(): WeeklySchedule {}

        /**
         * Write WEEKLY_SCHEDULE.
         * @throws Exception Not yet implemented in this version.
         */
        public function writeWeeklySchedule(WeeklySchedule $schedule): void {}
    }

    /* ── Server ─────────────────────────────────────────────────────────── */

    /**
     * BACnet/IP server — responds to incoming requests via PHP callbacks.
     * Only one Client (or Server) may exist per process.
     */
    class Server
    {
        /**
         * @param int    $deviceId       Device instance ID (0 .. 4194302).
         * @param string $bindInterface  Interface name or "0.0.0.0" for auto-detect.
         * @param int    $port           UDP port (default 47808).
         * @throws Exception on init failure.
         * @throws \Error if a Client/Server already exists.
         */
        public function __construct(
            int    $deviceId,
            string $bindInterface = '0.0.0.0',
            int    $port          = 47808,
        ) {}

        /**
         * Register a local BACnet object this server owns.
         * ReadProperty/WriteProperty requests targeting this object are dispatched
         * to the installed callbacks.
         */
        public function addLocalObject(ObjectIdentifier $oid): void {}

        /** Remove a previously registered local object. */
        public function removeLocalObject(ObjectIdentifier $oid): void {}

        /**
         * Register the ReadProperty callback.
         *
         * Signature: function(ObjectIdentifier $oid, Property|int $property, ?int $arrayIndex): mixed
         * Return value is encoded as a BACnet application-data value in the ACK.
         * Accepted return types: bool, int, float, string, null, Value, BitString, Date, Time, ObjectIdentifier.
         */
        public function onReadProperty(callable $handler): void {}

        /**
         * Register the WriteProperty callback.
         *
         * Signature: function(ObjectIdentifier $oid, Property|int $property, mixed $value, ?int $arrayIndex): void
         */
        public function onWriteProperty(callable $handler): void {}

        /**
         * Enable or disable automatic I-Am responses to Who-Is broadcasts.
         * Enabled by default.
         */
        public function setAutoIAm(bool $enabled): void {}

        /**
         * Process at most one pending PDU from the socket (non-blocking by default).
         *
         * @param int $timeoutMs  Maximum time to block waiting for a PDU, in milliseconds.
         *                        Use 0 for non-blocking, or a positive value for a blocking poll.
         */
        public function poll(int $timeoutMs = 0): void {}
    }
}
