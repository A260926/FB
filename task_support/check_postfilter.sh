#!/usr/bin/env bash
# Content regression entry point for the CELT pitch post-filter.
#
# Builds a float, no-intrinsics library plus a second copy with the read-only
# observation hook (OPUS_POSTFILTER_TRACE) enabled, then runs:
#
#   1. tests/test_postfilter_unit    - comb_filter content checks against an
#                                      independent per-sample reference,
#                                      in-place and out-of-place separately.
#   2. tests/test_postfilter_decode  - reference replay over real CELT packets
#                                      at 2.5, 5 and 20 ms.
#
# Finally it applies two still-compiling degeneracies in temporary copies of
# the tree and requires the same content assertions to reject each one:
#
#   A. square transition -> linear transition
#   B. short-packet two-generation parameters merged early
#
# A degeneracy that fails to compile or crashes does not count as detected.
#
# Only sources, a C compiler, CMake and the system math library are used.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)
BUILD_ROOT="${POSTFILTER_BUILD_DIR:-$ROOT/.postfilter-build}"
CC=${CC:-cc}
WORK="$BUILD_ROOT/work"
rm -rf "$WORK"
mkdir -p "$WORK"

CMAKE_COMMON=(
  -DCMAKE_BUILD_TYPE=Release
  -DOPUS_BUILD_TESTING=OFF
  -DOPUS_BUILD_PROGRAMS=OFF
  -DOPUS_DISABLE_INTRINSICS=ON
  -DOPUS_DEEP_PLC=OFF
  -DOPUS_DRED=OFF
  -DOPUS_OSCE=OFF
)

say() { printf '%s\n' "$*"; }
fail() { printf 'CHECK_POSTFILTER_FAIL %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Library build helper.  $1 = source root, $2 = build dir, $3 = trace ON/OFF.
# ---------------------------------------------------------------------------
build_lib() {
  local srcroot=$1 dir=$2 trace=$3
  rm -rf "$dir"
  cmake -S "$srcroot" -B "$dir" "${CMAKE_COMMON[@]}" \
        -DOPUS_POSTFILTER_TRACE="$trace" >/dev/null
  cmake --build "$dir" -j 4 --target opus >/dev/null
  [ -f "$dir/libopus.a" ] || fail "library build produced no archive in $dir"
}

compile_test() {
  local root=$1 src=$2 out=$3 lib=$4
  "$CC" -O2 -std=gnu99 -I"$root/include" -I"$root/celt" -I"$root" -I"$root/tests" \
        -DOPUS_BUILD -DVAR_ARRAYS "$src" "$lib" -lm -o "$out" \
    || fail "compile $src"
}

# ---------------------------------------------------------------------------
# 1. The real tree.  Float, SIMD disabled.  The decode test needs the
#    read-only observation hook, which lives in a separate build so the plain
#    build stays free of the callback.
# ---------------------------------------------------------------------------
say "CHECK_POSTFILTER build: float, no intrinsics"
build_lib "$ROOT" "$WORK/build-plain" OFF
build_lib "$ROOT" "$WORK/build-trace" ON

# ABI guard: the plain build must not reference the test-only callback at all,
# and the callback must never be defined (exported) by the library.
if nm "$WORK/build-plain/libopus.a" 2>/dev/null | grep -q celt_postfilter_trace_frame; then
  fail "plain build references the test-only trace callback"
fi
if nm "$WORK/build-trace/libopus.a" 2>/dev/null | grep -q ' T _celt_postfilter_trace_frame$'; then
  fail "trace build exports the test-only callback"
fi

compile_test "$ROOT" "$ROOT/tests/test_postfilter_unit.c" \
  "$WORK/real-unit" "$WORK/build-plain/libopus.a"
compile_test "$ROOT" "$ROOT/tests/test_postfilter_decode.c" \
  "$WORK/real-decode" "$WORK/build-trace/libopus.a"

say "CHECK_POSTFILTER run: unit"
"$WORK/real-unit" | tee "$WORK/real-unit.log"
grep -q '^POSTFILTER_UNIT_PASS$' "$WORK/real-unit.log" || fail "unit test did not pass"

say "CHECK_POSTFILTER run: decode"
"$WORK/real-decode" | tee "$WORK/real-decode.log"
grep -q '^POSTFILTER_DEC_PASS$' "$WORK/real-decode.log" || fail "decode test did not pass"
grep '^POSTFILTER_DEC_LENGTH ' "$WORK/real-decode.log" | sed 's/^/  /'

# ---------------------------------------------------------------------------
# 2. Degeneracy checks.  Each runs against a temporary copy of the sources
#    with one minimal, still-compiling change.  Detection means a content
#    assertion failed: a POSTFILTER_*_FAIL marker in the log AND a non-zero
#    exit status.  A crash (signal exit) or a compile failure is not a valid
#    detection.
# ---------------------------------------------------------------------------
PRISTINE="$WORK/tree"
rm -rf "$PRISTINE"
mkdir -p "$PRISTINE"
# Copy only what CMake needs; this also avoids touching VCS metadata files.
for item in CMakeLists.txt cmake celt silk src include dnn tests \
            celt_sources.mk celt_headers.mk silk_sources.mk silk_headers.mk \
            opus_sources.mk opus_headers.mk lpcnet_sources.mk lpcnet_headers.mk \
            meson.build meson_options.txt meson Makefile.am configure.ac \
            m4 scripts .gitattributes AUTHORS COPYING version.sh \
            opus.pc.in opus-uninstalled.pc.in update_version; do
  [ -e "$ROOT/$item" ] || continue
  cp -R "$ROOT/$item" "$PRISTINE/"
done
[ -f "$PRISTINE/CMakeLists.txt" ] || fail "could not stage a source copy"

make_copy() {
  local dst=$1
  rm -rf "$dst"
  cp -R "$PRISTINE" "$dst"
}

# Apply a single literal replacement in a file.  $1 = file, $2 = old, $3 = new.
# Fails loudly (rather than silently missing) if the expected text is absent.
apply_edit() {
  local file=$1 old=$2 new=$3
  [ -f "$file" ] || fail "degeneracy source missing: $file"
  awk -v old="$old" -v new="$new" '
    BEGIN { done = 0 }
    {
      i = index($0, old)
      if (!done && i > 0) {
        $0 = substr($0, 1, i - 1) new substr($0, i + length(old))
        done = 1
      }
      print
    }
    END { if (!done) exit 3 }
  ' "$file" > "$file.tmp" || fail "degeneracy edit did not apply in $file"
  mv "$file.tmp" "$file"
}

detect_degeneracy() {
  local name=$1 dir=$2
  say "CHECK_POSTFILTER degeneracy: $name"
  build_lib "$dir" "$dir/build-plain" OFF
  build_lib "$dir" "$dir/build-trace" ON
  compile_test "$dir" "$dir/tests/test_postfilter_unit.c" \
    "$dir/unit" "$dir/build-plain/libopus.a"
  compile_test "$dir" "$dir/tests/test_postfilter_decode.c" \
    "$dir/decode" "$dir/build-trace/libopus.a"

  local urc drc
  set +e
  "$dir/unit" >"$dir/unit.log" 2>"$dir/unit.err"; urc=$?
  "$dir/decode" >"$dir/decode.log" 2>"$dir/decode.err"; drc=$?
  set -e

  # Signal termination means a crash, not a detected content regression.
  if [ $urc -gt 128 ] || [ $drc -gt 128 ]; then
    fail "degeneracy '$name' crashed (unit=$urc decode=$drc) instead of failing a content assertion"
  fi

  local unit_detected=0 dec_detected=0
  if [ $urc -ne 0 ] && grep -q '^POSTFILTER_UNIT_FAIL' "$dir/unit.log" "$dir/unit.err" 2>/dev/null; then
    unit_detected=1
  fi
  if [ $drc -ne 0 ] && grep -q '^POSTFILTER_DEC_FAIL' "$dir/decode.log" "$dir/decode.err" 2>/dev/null; then
    dec_detected=1
  fi

  if [ $unit_detected -eq 0 ] && [ $dec_detected -eq 0 ]; then
    fail "degeneracy '$name' was not detected by any content assertion"
  fi
  say "  detected: unit=${unit_detected} decode=${dec_detected}"
  # Report the first explicit content failure with its packet/channel/sample.
  grep -h -m1 '^POSTFILTER_DEC_FAIL ' "$dir/decode.log" "$dir/decode.err" 2>/dev/null \
    | sed 's/^/  /' || true

  # The temporary build is only needed while the assertion runs.
  rm -rf "$dir/build-plain" "$dir/build-trace"
}

# --- Degeneracy A: square transition replaced with a linear transition. -----
DIR_A="$WORK/degeneracy-square"
make_copy "$DIR_A"
apply_edit "$DIR_A/celt/celt.c" \
  "f = MULT16_16_Q15(window[i],window[i]);" \
  "f = window[i];"
detect_degeneracy "square-transition-to-linear" "$DIR_A"

# --- Degeneracy B: short packets merge the two generations early. ----------
# celt_decoder.c normally passes (old, previous) to the first segment.  Using
# (current, current) makes a 2.5 ms frame take the freshly decoded parameters
# immediately instead of crossing from the previous generation.
DIR_B="$WORK/degeneracy-merge"
make_copy "$DIR_B"
apply_edit "$DIR_B/celt/celt_decoder.c" \
  "st->postfilter_period_old, st->postfilter_period, mode->shortMdctSize," \
  "postfilter_pitch, postfilter_pitch, mode->shortMdctSize,"
apply_edit "$DIR_B/celt/celt_decoder.c" \
  "st->postfilter_gain_old, st->postfilter_gain, st->postfilter_tapset_old, st->postfilter_tapset," \
  "postfilter_gain, postfilter_gain, postfilter_tapset, postfilter_tapset,"
detect_degeneracy "short-packet-generation-merge" "$DIR_B"

say "CHECK_POSTFILTER_PASS"
