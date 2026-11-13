#!/usr/bin/env bash
#
# check_decorrelation.sh
#
# End-to-end driver for the independent decorrelation certification.
#
# For each optimization level (-O0 and -O2 by default) it:
#   1. builds the library with the read-only DECORR_TEST_OBSERVE test hooks,
#   2. builds and runs tests/decorr_reference and tests/decorr_blocks against
#      the public WavpackOpenFileInputEx64 / WavpackUnpackSamples chain,
#   3. runs the mutation suite to prove the numeric assertions actually
#      reject a faulty -3 dependency and a missing negative-term clip,
# then it rebuilds the untouched shipping library and runs baseline.sh.
#
# Per-term and multi-level chain comparison counts are printed by the two
# test binaries.  No network access, no downloads, C/Shell/CMake only.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SRCS=(
    src/common_utils.c src/decorr_utils.c src/entropy_utils.c src/extra1.c
    src/extra2.c src/open_utils.c src/open_filename.c src/open_raw.c
    src/pack.c src/pack_dns.c src/pack_floats.c src/pack_utils.c
    src/read_words.c src/tags.c src/tag_utils.c src/unpack.c
    src/unpack_floats.c src/unpack_seek.c src/unpack_utils.c
    src/write_words.c tests/decorr_observe.c
)

if [ "$#" -eq 0 ]; then
    LEVELS=(-O0 -O2)
else
    LEVELS=("$@")
fi

for OPT in "${LEVELS[@]}"; do
    echo "=================== decorrelation checks at $OPT ==================="
    BUILD="$ROOT/.task-dt-$OPT"
    rm -rf "$BUILD"
    mkdir -p "$BUILD"

    # instrumented library; the hooks compile to nothing in the regular build
    for f in "${SRCS[@]}"; do
        o="$BUILD/$(basename "$f" .c).o"
        cc -std=c99 "$OPT" -g -Wall -Wextra \
           -DDECORR_TEST_OBSERVE -DHAVE___BUILTIN_CLZ -DHAVE_FSEEKO \
           -Iinclude -Isrc -Itests -c "$f" -o "$o"
    done
    ar rcs "$BUILD/libdt.a" "$BUILD"/*.o

    CFLAGS="-std=c99 $OPT -g -Wall -Wextra -DDECORR_TEST_OBSERVE \
            -DHAVE___BUILTIN_CLZ -DHAVE_FSEEKO -Iinclude -Isrc -Itests"

    cc $CFLAGS tests/decorr_reference.c "$BUILD/libdt.a" -lm -o "$BUILD/decorr_reference"
    cc $CFLAGS tests/decorr_blocks.c    "$BUILD/libdt.a" -lm -o "$BUILD/decorr_blocks"

    echo "--- decorr_reference ($OPT) ---"
    "$BUILD/decorr_reference"

    echo "--- decorr_blocks ($OPT) ---"
    "$BUILD/decorr_blocks"

    echo "--- mutation suite ($OPT) ---"
    # the mutate script needs to know where this level's library lives
    DT_LIBA="$BUILD/libdt.a" bash task_support/mutate_decorr.sh "$OPT"
done

echo "=================== shipping library + baseline ==================="
bash task_support/build.sh
bash task_support/baseline.sh

echo "ALL DECORRELATION CHECKS PASSED"
