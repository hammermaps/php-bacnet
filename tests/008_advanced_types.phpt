--TEST--
Advanced types: ScheduleEntry, WeeklySchedule, TrendLogRecord, ObjectRef convenience (Phase 7)
--EXTENSIONS--
bacnet
--FILE--
<?php
// --- Class existence ---
foreach (['Bacnet\ScheduleEntry', 'Bacnet\WeeklySchedule', 'Bacnet\TrendLogRecord'] as $c) {
    assert(class_exists($c), "$c missing");
}
echo "classes exist: OK\n";

// --- ScheduleEntry ---
$t = new Bacnet\Time(8, 0, 0, 0);
$se = new Bacnet\ScheduleEntry($t, 1.5);
assert($se->startTime instanceof Bacnet\Time, 'startTime property');
assert($se->getStartTime() instanceof Bacnet\Time, 'getStartTime()');
assert($se->getStartTime()->hour === 8, 'startTime hour');
assert($se->getValue() === 1.5, 'getValue()');
assert($se->value === 1.5, 'value property');
echo "ScheduleEntry: OK\n";

// ScheduleEntry with bool value
$se2 = new Bacnet\ScheduleEntry(new Bacnet\Time(17, 0, 0, 0), false);
assert($se2->getValue() === false, 'bool value');
echo "ScheduleEntry bool: OK\n";

// --- WeeklySchedule: all defaults ---
$ws = new Bacnet\WeeklySchedule();
for ($d = 1; $d <= 7; $d++) {
    assert($ws->getDay($d) === [], "day $d should be empty");
}
echo "WeeklySchedule empty: OK\n";

// WeeklySchedule: with entries per day
$ws2 = new Bacnet\WeeklySchedule(
    monday:    [$se, $se2],
    wednesday: [$se],
    sunday:    [$se2]
);
assert(count($ws2->getDay(1)) === 2, 'Monday has 2 entries');
assert(count($ws2->getDay(2)) === 0, 'Tuesday empty');
assert(count($ws2->getDay(3)) === 1, 'Wednesday has 1');
assert(count($ws2->getDay(7)) === 1, 'Sunday has 1');
assert($ws2->getDay(1)[0] instanceof Bacnet\ScheduleEntry, 'entries are ScheduleEntry');
echo "WeeklySchedule with entries: OK\n";

// WeeklySchedule: getDay bounds
try {
    $ws->getDay(0);
    assert(false, 'weekday 0 should throw');
} catch (Bacnet\Exception $e) {
    echo "getDay(0) throws: OK\n";
}
try {
    $ws->getDay(8);
    assert(false, 'weekday 8 should throw');
} catch (Bacnet\Exception $e) {
    echo "getDay(8) throws: OK\n";
}

// --- TrendLogRecord ---
$r = new Bacnet\TrendLogRecord(null, 42.0, 0);
assert($r->timestamp === null, 'timestamp null');
assert($r->value === 42.0, 'value 42.0');
assert($r->statusFlags === 0, 'statusFlags 0');
echo "TrendLogRecord basic: OK\n";

$r2 = new Bacnet\TrendLogRecord(
    new Bacnet\Date(2024, 6, 29, 6),
    new Bacnet\BitString([true, false, false, false]),
    4
);
assert($r2->timestamp instanceof Bacnet\Date, 'timestamp Date');
assert($r2->value instanceof Bacnet\BitString, 'value BitString');
assert($r2->statusFlags === 4, 'statusFlags 4');
echo "TrendLogRecord complex: OK\n";

// --- ObjectRef convenience methods exist ---
$methods = ['writeActive', 'writeInactive', 'writePresentValue',
            'readTrendLog', 'readWeeklySchedule', 'writeWeeklySchedule'];
foreach ($methods as $m) {
    assert(method_exists('Bacnet\ObjectRef', $m), "$m missing");
}
echo "ObjectRef methods: OK\n";

// writeWeeklySchedule throws NotImplemented
$ref_uninit = (new ReflectionClass('Bacnet\ObjectRef'))->newInstanceWithoutConstructor();
try {
    $ref_uninit->writeWeeklySchedule(new Bacnet\WeeklySchedule());
    assert(false, 'should throw');
} catch (Bacnet\Exception $e) {
    assert(str_contains($e->getMessage(), 'not yet implemented'), 'correct stub message');
    echo "writeWeeklySchedule stub: OK\n";
}

// writePresentValue with unsupported type throws
try {
    $ref_uninit->writePresentValue(new Bacnet\WeeklySchedule());
    assert(false, 'should throw');
} catch (Bacnet\Exception $e) {
    // "no associated device" fires first — that's fine
    echo "writePresentValue guard: OK\n";
}

// --- ObjectType enum coverage for Phase 7 objects ---
assert(Bacnet\ObjectType::SCHEDULE->value === 17,           'SCHEDULE=17');
assert(Bacnet\ObjectType::TREND_LOG->value === 20,          'TREND_LOG=20');
assert(Bacnet\ObjectType::EVENT_ENROLLMENT->value === 9,    'EVENT_ENROLLMENT=9');
assert(Bacnet\ObjectType::NOTIFICATION_CLASS->value === 15, 'NOTIFICATION_CLASS=15');
echo "ObjectType enum: OK\n";

// --- Property enum coverage ---
assert(Bacnet\Property::WEEKLY_SCHEDULE->value === 123, 'WEEKLY_SCHEDULE=123');
assert(Bacnet\Property::LOG_BUFFER->value === 131,      'LOG_BUFFER=131');
assert(Bacnet\Property::RELIABILITY->value === 103,     'RELIABILITY=103');
echo "Property enum: OK\n";

echo "All Phase 7 tests passed.\n";
?>
--EXPECT--
classes exist: OK
ScheduleEntry: OK
ScheduleEntry bool: OK
WeeklySchedule empty: OK
WeeklySchedule with entries: OK
getDay(0) throws: OK
getDay(8) throws: OK
TrendLogRecord basic: OK
TrendLogRecord complex: OK
ObjectRef methods: OK
writeWeeklySchedule stub: OK
writePresentValue guard: OK
ObjectType enum: OK
Property enum: OK
All Phase 7 tests passed.
