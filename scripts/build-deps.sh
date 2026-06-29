#!/usr/bin/env bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BACNET_DIR="$SCRIPT_DIR/../deps/bacnet-stack"

cd "$BACNET_DIR"
mkdir -p build
cd build
cmake .. \
  -DBUILD_SHARED_LIBS=OFF \
  -DBACNET_STACK_BUILD_APPS=OFF \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  -DBACNET_STACK_BUILD_TESTS=OFF \
  -DCMAKE_C_FLAGS="-fPIC"
cmake --build . --parallel
echo "Built: $BACNET_DIR/build/libbacnet-stack.a"
