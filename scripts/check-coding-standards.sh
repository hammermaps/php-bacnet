#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

if ! command -v clang-format >/dev/null 2>&1; then
    echo "clang-format is required" >&2
    exit 1
fi

mapfile -t sources < <(
    find . \
        -path './deps' -prune -o \
        -name config.h -prune -o \
        -type f \( -name '*.c' -o -name '*.h' \) -print | sort
)

if ((${#sources[@]} == 0)); then
    exit 0
fi

clang-format --dry-run --Werror "${sources[@]}"

if rg -n 'strncat[[:space:]]*\(' "${sources[@]}"; then
    echo "strncat() is forbidden; use an explicitly bounded alternative." >&2
    exit 1
fi

if rg -n 'sizeof[[:space:]]*\([[:space:]]*"([^"\\]|\\.)*"[[:space:]]*\)[[:space:]]*-[[:space:]]*1' "${sources[@]}"; then
    echo 'sizeof("literal") - 1 is forbidden; use strlen("literal").' >&2
    exit 1
fi
