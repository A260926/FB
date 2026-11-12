#!/usr/bin/env bash
# Builds and runs the DTVCC (native CEA-708) decoder unit tests in a temporary
# directory. Prints DTVCC_WINDOWS_PASS on success. Missing files, zero executed
# cases and any failure exit non zero.
set -euo pipefail
cd "$(dirname "$0")/.."

expected_cases=16

for f in caption/dtvcc.h src/dtvcc.c unit_tests/test_dtvcc.c; do
    if [ ! -f "$f" ]; then
        echo "DTVCC CHECK FAILED: missing $f" >&2
        exit 1
    fi
done

b=$(mktemp -d "${TMPDIR:-/tmp}/check_dtvcc.XXXXXX")
trap 'rm -rf "$b"' EXIT

cp src/eia608_from_utf8.c.cached "$b/eia608_from_utf8.c"
cc -std=c99 -I. -Icaption \
    unit_tests/test_dtvcc.c \
    src/dtvcc.c src/cea708.c src/caption.c src/eia608.c src/eia608_charmap.c src/utf8.c src/xds.c \
    "$b/eia608_from_utf8.c" \
    -o "$b/test_dtvcc"

out=$("$b/test_dtvcc")
printf '%s\n' "$out"

run=$(printf '%s\n' "$out" | sed -n 's/^DTVCC_CASES_RUN \([0-9][0-9]*\)$/\1/p' | head -1)
passed=$(printf '%s\n' "$out" | grep -c '^DTVCC_CASE .* PASS$' || true)

if [ -z "$run" ]; then
    echo "DTVCC CHECK FAILED: no case count reported" >&2
    exit 1
fi

if [ "$run" -eq 0 ] || [ "$passed" -eq 0 ]; then
    echo "DTVCC CHECK FAILED: zero cases executed" >&2
    exit 1
fi

if [ "$run" != "$passed" ]; then
    echo "DTVCC CHECK FAILED: $run cases ran but $passed passed" >&2
    exit 1
fi

if [ "$run" != "$expected_cases" ]; then
    echo "DTVCC CHECK FAILED: expected $expected_cases cases, got $run" >&2
    exit 1
fi

echo "DTVCC_WINDOWS_PASS"
