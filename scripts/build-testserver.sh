#!/usr/bin/env bash
# Build the BACnet test server (bacnet-mini) from bacnet-stack server-mini example.
# Links directly against libbacnet-stack.a — no separate cmake step needed.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$SCRIPT_DIR/.."
BACNET_DIR="$REPO_DIR/deps/bacnet-stack"
LIB="$BACNET_DIR/build/libbacnet-stack.a"
OUT="$REPO_DIR/tests/bacnet-testserver"

if [ ! -f "$LIB" ]; then
  echo "ERROR: $LIB not found. Run: ./scripts/build-deps.sh"
  exit 1
fi

gcc \
  -I"$BACNET_DIR/src" \
  -DBACDL_BIP \
  -DBACNET_STACK_DEPRECATED_DISABLE \
  -DBACAPP_PRINT_ENABLED \
  -o "$OUT" \
  "$BACNET_DIR/apps/server-mini/main.c" \
  "$LIB" \
  -Wl,-rpath,"$BACNET_DIR/build" \
  -lm

echo "Built: $OUT"
echo ""
echo "Usage:"
echo "  $OUT [device_id] [device_name]"
echo "  $OUT 1234 TestDevice"
echo ""
echo "Objects exposed:"
echo "  AV:0  AnalogValue (read-only)  PRESENT_VALUE=22.5, UNITS=degrees-celsius"
echo "  AO:0  AnalogOutput (writable)  PRESENT_VALUE=50.0, UNITS=percent"
echo "  BO:0  BinaryOutput (writable)  PRESENT_VALUE=inactive"
echo "  BV:0  BinaryValue  (read-only) PRESENT_VALUE cycles active/inactive"
