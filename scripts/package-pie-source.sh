#!/usr/bin/env bash
set -euo pipefail

if (($# != 1)); then
    echo "Usage: $0 <version-or-tag>" >&2
    exit 64
fi

VERSION="${1#v}"
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT_DIR="${PIE_DIST_DIR:-$ROOT_DIR/dist}"
PACKAGE_ROOT="php-bacnet-${VERSION}"
ARCHIVE="$OUTPUT_DIR/php_bacnet-${VERSION}-src.tgz"

if [[ ! -f "$ROOT_DIR/deps/bacnet-stack/CMakeLists.txt" ]]; then
    echo "bacnet-stack source is missing; checkout recursively with submodules." >&2
    exit 1
fi

mkdir -p "$OUTPUT_DIR"
tar \
    --create \
    --gzip \
    --file "$ARCHIVE" \
    --transform "s,^,$PACKAGE_ROOT/," \
    --exclude-vcs \
    --exclude='./autom4te.cache' \
    --exclude='./build' \
    --exclude='./Makefile' \
    --exclude='./Makefile.fragments' \
    --exclude='./Makefile.objects' \
    --exclude='./acinclude.m4' \
    --exclude='./config.h' \
    --exclude='./config.h.in~' \
    --exclude='./config.log' \
    --exclude='./config.nice' \
    --exclude='./config.status' \
    --exclude='./configure' \
    --exclude='./configure~' \
    --exclude='./libtool' \
    --exclude='./*.dep' \
    --exclude='./*.la' \
    --exclude='./*.lo' \
    --exclude='./modules' \
    --exclude='./.libs' \
    --exclude='./deps/bacnet-stack/build' \
    --exclude='./deps/bacnet-stack/.cache' \
    --exclude='./deps/bacnet-stack/CMakeFiles' \
    --exclude='./deps/bacnet-stack/CMakeCache.txt' \
    --exclude='./php_test_results_*.txt' \
    --exclude='./dist' \
    -C "$ROOT_DIR" .

echo "$ARCHIVE"
