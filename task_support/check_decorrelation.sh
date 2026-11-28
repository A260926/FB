#!/usr/bin/env bash
# tests/decorr_model.h, tests/decorr_reference.c, tests/decorr_blocks.c
# Run: bash task_support/check_decorrelation.sh
#
# Builds an observation-enabled static libwavpack (WAVPACK_DECORR_OBSERVE) at
# -O0 and -O2, runs the model unit test and the planned-block integration test
# against both, and then repeats the block test against two deliberately broken
# copies of the decoder in a temporary source tree to confirm the numerical
# assertions reject them (a compile failure, crash, or bare CRC error does not
# count).
set -euo pipefail

cd "$(dirname "$0")/.."
REPO="$PWD"
WORK="${TMPDIR:-/tmp}/decorr-check-$$"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK"

CC="${CC:-cc}"
CFLAGS_BASE="-std=c99 -Wall -Wextra -Wno-unused-function -Iinclude -Itests"
LDLIBS="-lm"

fail() { echo "CHECK_DECORRELATION FAIL: $*" >&2; exit 1; }

# build_observe_lib <srcdir> <builddir> <optlevel>
build_observe_lib() {
    local srcdir="$1" builddir="$2" opt="$3"
    cmake -S "$srcdir" -B "$builddir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="$opt" \
        -DBUILD_SHARED_LIBS=OFF \
        -DWAVPACK_BUILD_PROGRAMS=OFF \
        -DBUILD_TESTING=OFF \
        -DWAVPACK_ENABLE_ASM=OFF \
        -DWAVPACK_ENABLE_THREADS=OFF \
        -DWAVPACK_ENABLE_DSD=OFF \
        -DCMAKE_C_FLAGS="$opt -DWAVPACK_DECORR_OBSERVE" >/dev/null
    cmake --build "$builddir" --target wavpack -j 4 >/dev/null
}

# run_suite <libdir> <optlevel> <tag>
run_suite() {
    local libdir="$1" opt="$2" tag="$3"
    local lib="$libdir/libwavpack.a"
    [ -f "$lib" ] || fail "missing $lib"

    $CC $CFLAGS_BASE "$opt" tests/decorr_reference.c -o "$WORK/ref_$tag" $LDLIBS \
        || fail "compile decorr_reference ($tag)"

    $CC $CFLAGS_BASE "$opt" -DWAVPACK_DECORR_OBSERVE \
        tests/decorr_blocks.c tests/decorr_blocks_shim.c "$lib" \
        -o "$WORK/blk_$tag" $LDLIBS \
        || fail "compile decorr_blocks ($tag)"

    "$WORK/ref_$tag" || fail "decorr_reference failed ($tag)"
    "$WORK/blk_$tag" || fail "decorr_blocks failed ($tag)"
}

# ---------------------------------------------------------------- main build
echo "== building observation library (-O0) =="
build_observe_lib "$REPO" "$WORK/build-O0" "-O0"
echo "== building observation library (-O2) =="
build_observe_lib "$REPO" "$WORK/build-O2" "-O2"

echo "== running model unit test and planned-block test (-O0) =="
run_suite "$WORK/build-O0" "-O0" O0

echo "== running model unit test and planned-block test (-O2) =="
run_suite "$WORK/build-O2" "-O2" O2

# ------------------------------------------------ mutation checks (temp copy)
# Two specific defects must be rejected by the same numerical assertions:
#   1) term -3 uses this tick's opposite channel instead of the saved previous
#      tick's opposite channel,
#   2) the negative-term weight clamp is removed.
# Each is applied to a fresh copy of the source tree. We require the block test
# to fail with a NUMERIC assertion (a value mismatch), not merely a CRC error,
# and not a compile failure or crash.

mutate_and_expect_reject() {
    local name="$1" apply="$2"
    local mdir="$WORK/src-$name"
    local lib="$WORK/libmut-$name.a"

    rm -rf "$mdir"
    mkdir -p "$mdir"
    cp -R "$REPO/src" "$REPO/include" "$mdir/"

    ( cd "$mdir" && eval "$apply" ) || fail "mutation '$name' did not apply"

    # Compile the library sources directly into a static archive: no CMake, so
    # the mutation tree only needs src/ and include/.
    $CC -std=c99 -O2 -I"$mdir/include" -I"$mdir/src" \
        -DWAVPACK_DECORR_OBSERVE -c "$mdir"/src/*.c 2>"$WORK/$name.build.log" \
        || fail "mutation '$name': library failed to compile"

    ar rcs "$lib" ./*.o 2>/dev/null || ar rcs "$lib" $WORK/*.o
    rm -f ./*.o

    $CC $CFLAGS_BASE "-O2" -DWAVPACK_DECORR_OBSERVE \
        tests/decorr_blocks.c tests/decorr_blocks_shim.c "$lib" \
        -o "$WORK/blk_$name" $LDLIBS \
        || fail "mutation '$name': block test failed to compile (not a numerical rejection)"

    set +e
    "$WORK/blk_$name" >"$WORK/$name.out" 2>"$WORK/$name.err"
    local rc=$?
    set -e

    if [ $rc -eq 0 ]; then
        fail "mutation '$name' was NOT rejected"
    fi

    # Require a genuine numerical state/field assertion (weight, history or
    # residual), not merely a PCM divergence caused by CRC muting. The observer
    # state comparison runs independently of the CRC.
    if grep -q "first deviation:.*field=\(weight_A\|weight_B\|history_A\|history_B\|residual\)" "$WORK/$name.err"; then
        echo "mutation '$name' rejected by numerical state assertion:"
        grep -m1 "first deviation:.*field=\(weight_A\|weight_B\|history_A\|history_B\|residual\)" "$WORK/$name.err"
    else
        echo "--- mutation '$name' stderr ---" >&2
        cat "$WORK/$name.err" >&2
        fail "mutation '$name' was not rejected by a state/field numerical assertion"
    fi
}

# Defect 1: term -3's left channel uses *this* tick's already-reconstructed
# right channel as its prediction source, instead of the saved previous tick's
# right channel (samples_A[0]). Production reads both saved values before
# reconstructing either channel; the mutant reconstructs the right first and
# feeds it into the left. The storage crossing at the end is left untouched.
apply_minus3_defect() {
    python3 - "$1" <<'PY'
import sys
path = sys.argv[1] + "/src/unpack.c"
src = open(path).read()
old = """        case -3:
            for (bptr = buffer; bptr < eptr; bptr += 2) {
                int32_t sam_A, sam_B;

                sam_A = bptr [0] + apply_weight (dpp->weight_A, dpp->samples_A [0]);
                update_weight_clip (dpp->weight_A, dpp->delta, dpp->samples_A [0], bptr [0]);
                sam_B = bptr [1] + apply_weight (dpp->weight_B, dpp->samples_B [0]);
                update_weight_clip (dpp->weight_B, dpp->delta, dpp->samples_B [0], bptr [1]);
                bptr [0] = dpp->samples_B [0] = sam_A;
                bptr [1] = dpp->samples_A [0] = sam_B;
            }

            break;"""
new = """        case -3:
            for (bptr = buffer; bptr < eptr; bptr += 2) {
                int32_t sam_A, sam_B;

                sam_B = bptr [1] + apply_weight (dpp->weight_B, dpp->samples_B [0]);
                update_weight_clip (dpp->weight_B, dpp->delta, dpp->samples_B [0], bptr [1]);
                sam_A = bptr [0] + apply_weight (dpp->weight_A, sam_B);
                update_weight_clip (dpp->weight_A, dpp->delta, sam_B, bptr [0]);
                bptr [0] = dpp->samples_B [0] = sam_A;
                bptr [1] = dpp->samples_A [0] = sam_B;
            }

            break;"""
assert src.count(old) == 1, "term -3 anchor not found exactly once"
open(path, "w").write(src.replace(old, new))
PY
}

# Defect 2: negative-term weight clamp removed (plain +/- without the +/-1024
# bound). update_weight_clip is only used by negative terms, so removing the
# bounds only affects them.
apply_no_clamp_defect() {
    python3 - "$1" <<'PY'
import sys
path = sys.argv[1] + "/src/wavpack_local.h"
src = open(path).read()
old = """#define update_weight_clip(weight, delta, source, result) \\
    if (source && result) { \\
        const int32_t s = (source ^ result) >> 31; \\
        if ((weight = (weight ^ s) + (delta - s)) > 1024) weight = 1024; \\
        weight = (weight ^ s) - s; \\
    }"""
new = """#define update_weight_clip(weight, delta, source, result) \\
    if (source && result) { \\
        const int32_t s = (source ^ result) >> 31; \\
        weight = (weight ^ s) + (delta - s); \\
        weight = (weight ^ s) - s; \\
    }"""
assert src.count(old) == 1, "update_weight_clip anchor not found exactly once"
open(path, "w").write(src.replace(old, new))
PY
}

echo "== mutation check: term -3 uses current-tick opposite channel =="
mutate_and_expect_reject "minus3" 'apply_minus3_defect "$mdir"'

echo "== mutation check: negative-term weight clamp removed =="
mutate_and_expect_reject "noclamp" 'apply_no_clamp_defect "$mdir"'

echo "CHECK_DECORRELATION OK"
