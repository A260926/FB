#!/usr/bin/env bash
#
# Regression driver for the offline rate-distortion joint-stereo encoder.
# It first re-runs the legacy baseline (proving old streams still build and
# decode), then builds and runs the independent RD reference suite.
#
# Any missing file, empty test set or failed assertion makes this script
# exit non-zero.
#
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/sbc-rd.XXXXXX")
trap 'rm -rf "$build"' EXIT

# --- Every required source must be present ---------------------------------

for f in \
    include/sbc.h \
    src/sbc.c src/bits.c src/sbc_rd.c \
    src/sbc_internal.h src/bits.h \
    repro/baseline.sh repro/baseline.c \
    tests/rd_reference.c
do
    if [[ ! -f "$root/$f" ]]; then
        echo "missing required file: $f" >&2
        exit 1
    fi
done

# --- 1. Legacy baseline -----------------------------------------------------

(
    cd "$root"
    bash repro/baseline.sh
)

# --- 2. Build the library and the independent RD reference -----------------

make -C "$root" BUILD_DIR="$build/obj" BIN_DIR="$build/bin" libsbc -j4 >/dev/null

cc -std=c11 -Wall -Wextra -UNDEBUG -I"$root/include" -I"$root/src" \
    "$root/tests/rd_reference.c" "$build/bin/libsbc.a" -lm \
    -o "$build/rd_reference"

# --- 3. Run and require the exact success token with at least six cases ----

output=$("$build/rd_reference")
printf '%s\n' "$output"

n_cases=$(printf '%s\n' "$output" | grep -c '^case ')
if [[ "$n_cases" -lt 6 ]]; then
    echo "expected at least 6 named cases, found $n_cases" >&2
    exit 1
fi

if ! printf '%s\n' "$output" | grep -qx 'SBC_RD_OK cases=6'; then
    echo "RD regression did not report SBC_RD_OK cases=6" >&2
    exit 1
fi
