--TEST--
Phase 4 — BitString, Date, Time, ObjectRef API (no network required)
--SKIPIF--
<?php
if (!extension_loaded('bacnet')) die('skip bacnet extension not loaded');
?>
--FILE--
<?php
// ── BitString ─────────────────────────────────────────────────────────
$bs = new Bacnet\BitString([true, false, true, true, false, false, true]);
echo 'BitString length: '  . $bs->getLength() . PHP_EOL;
echo 'BitString length prop: ' . $bs->length . PHP_EOL;
echo 'bit[0]=' . (int)$bs->getBit(0) . PHP_EOL;
echo 'bit[1]=' . (int)$bs->getBit(1) . PHP_EOL;
echo 'bit[2]=' . (int)$bs->getBit(2) . PHP_EOL;
echo 'bit[3]=' . (int)$bs->getBit(3) . PHP_EOL;
echo 'bit[6]=' . (int)$bs->getBit(6) . PHP_EOL;
echo 'toArray: ' . implode(',', array_map('intval', $bs->toArray())) . PHP_EOL;
// Out-of-range → false
var_dump($bs->getBit(100));
// Empty BitString
$empty = new Bacnet\BitString([]);
echo 'empty length: ' . $empty->getLength() . PHP_EOL;

// ── Date ──────────────────────────────────────────────────────────────
$d = new Bacnet\Date(2024, 6, 29, 6);
echo 'Date year='   . $d->year    . PHP_EOL;
echo 'Date month='  . $d->month   . PHP_EOL;
echo 'Date day='    . $d->day     . PHP_EOL;
echo 'Date weekday=' . $d->weekday . PHP_EOL;
// Wildcard (0xFF = 255)
$dw = new Bacnet\Date(255, 255, 255, 255);
echo 'Wildcard year=' . $dw->year . ' month=' . $dw->month . PHP_EOL;

// ── Time ──────────────────────────────────────────────────────────────
$t = new Bacnet\Time(14, 30, 59, 99);
echo 'Time hour='       . $t->hour       . PHP_EOL;
echo 'Time minute='     . $t->minute     . PHP_EOL;
echo 'Time second='     . $t->second     . PHP_EOL;
echo 'Time hundredths=' . $t->hundredths . PHP_EOL;

// ── Device::readProperty signature ───────────────────────────────────
$rm = new ReflectionMethod('Bacnet\Device', 'readProperty');
echo 'readProperty params: ' . $rm->getNumberOfParameters() . PHP_EOL;
echo 'readProperty required: ' . $rm->getNumberOfRequiredParameters() . PHP_EOL;

// ── ObjectRef ─────────────────────────────────────────────────────────
// class exists and has readProperty
var_dump(class_exists('Bacnet\ObjectRef'));
var_dump(method_exists('Bacnet\ObjectRef', 'readProperty'));
$rm2 = new ReflectionMethod('Bacnet\ObjectRef', 'readProperty');
echo 'ObjectRef::readProperty params: ' . $rm2->getNumberOfParameters() . PHP_EOL;

echo "done\n";
?>
--EXPECT--
BitString length: 7
BitString length prop: 7
bit[0]=1
bit[1]=0
bit[2]=1
bit[3]=1
bit[6]=1
toArray: 1,0,1,1,0,0,1
bool(false)
empty length: 0
Date year=2024
Date month=6
Date day=29
Date weekday=6
Wildcard year=255 month=255
Time hour=14
Time minute=30
Time second=59
Time hundredths=99
readProperty params: 4
readProperty required: 3
bool(true)
bool(true)
ObjectRef::readProperty params: 2
done
