#!/usr/bin/env bash
# Regression entry point for the Theora loop filter boundary-order
# checks.  Runs the existing baseline, builds the library with the
# test-only observation point, builds and runs the two new C tests,
# and then proves the suite has teeth by building two isolated,
# degraded copies of the sources (mutants) and verifying that the
# tests reject them with numeric evidence.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/.." && pwd)
cd "$src"

fail() {
  echo "THEORA_LOOPFILTER_FAIL: $*" >&2
  exit 1
}

# --- 0. Required inputs -------------------------------------------------
for f in repro/baseline.sh \
         tests/loopfilter_reference.c \
         tests/loopfilter_stream.c \
         tests/loopfilter_ref.h \
         tests/loopfilter_mutants/001-trunc-toward-zero.patch \
         tests/loopfilter_mutants/002-drop-bottom-uncoded-edge.patch; do
  [ -f "$f" ] || fail "missing required file: $f"
done

if [ ! -x "$src/configure" ]; then
  echo "THEORA_LOOPFILTER_INFO configure missing, running autogen.sh"
  (cd "$src" && ./autogen.sh >/dev/null) || fail "autogen.sh failed"
fi

# --- 1. Existing baseline must pass -------------------------------------
bash repro/baseline.sh

# --- 2. Build the library (test build with the observation point) -------
work=$(mktemp -d "${TMPDIR:-/tmp}/theora-loopfilter.XXXXXX")
trap 'rm -rf "$work"' EXIT
evidence="$src/tests/loopfilter_evidence"
mkdir -p "$evidence"

OGG_CFLAGS=$(pkg-config --cflags ogg)
OGG_LIBS=$(pkg-config --libs ogg)
JOBS=4

build_tree() { # $1=build dir, $2=source dir
  mkdir -p "$1"
  (cd "$1" && "$2/configure" --disable-shared --enable-static \
    --disable-examples --disable-doc --disable-spec \
    CFLAGS="-g -O2 -DOC_LOOPFILTER_OBSERVER" >configure.log 2>&1) \
    || { tail -20 "$1/configure.log"; return 1; }
  make -C "$1" -j"$JOBS" >/dev/null || return 1
}

compile_tests() { # $1=build dir, $2=source dir, $3=output prefix
  cc -std=c99 -UNDEBUG -DHAVE_CONFIG_H \
    -I"$1" -I"$2/include" -I"$2/lib" $OGG_CFLAGS \
    "$2/tests/loopfilter_reference.c" "$1/lib/.libs/libtheoradec.a" \
    $OGG_LIBS -lm -o "$3_reference" || return 1
  cc -std=c99 -UNDEBUG -DHAVE_CONFIG_H \
    -I"$1" -I"$2/include" -I"$2/lib" $OGG_CFLAGS \
    "$2/tests/loopfilter_stream.c" "$1/lib/.libs/libtheoraenc.a" \
    "$1/lib/.libs/libtheoradec.a" $OGG_LIBS -lm -o "$3_stream" || return 1
}

echo "THEORA_LOOPFILTER_INFO building main library (asm defaults, observer hook)"
build_tree "$work/main" "$src" || fail "main library build failed"
compile_tests "$work/main" "$src" "$work/lf" || fail "test compile failed"

# --- 3. Run the two new tests -------------------------------------------
"$work/lf_reference" | tee "$evidence/main-reference.log"
"$work/lf_stream" | tee "$evidence/main-stream.log"

cases=$(cat "$evidence/main-reference.log" "$evidence/main-stream.log" \
  | grep -c '^THEORA_LOOPFILTER_CASE .* PASS' || true)
fails=$(cat "$evidence/main-reference.log" "$evidence/main-stream.log" \
  | grep -c '^THEORA_LOOPFILTER_CASE .* FAIL' || true)
[ "$fails" -eq 0 ] || fail "$fails named case(s) failed"
[ "$cases" -gt 0 ] || fail "zero named cases executed"
[ "$cases" -eq 6 ] || fail "expected 6 named cases, got $cases"

# --- 4. Mutant copies: the suite must reject degraded sources -----------
mutants="001-trunc-toward-zero 002-drop-bottom-uncoded-edge"
for m in $mutants; do
  patch_file="tests/loopfilter_mutants/$m.patch"
  msrc="$work/mut-$m"
  mbuild="$work/build-$m"
  echo "THEORA_LOOPFILTER_INFO mutant=$m: copying sources"
  cp -R "$src" "$msrc"
  rm -rf "$msrc/tests/loopfilter_evidence"
  if command -v patch >/dev/null 2>&1; then
    (cd "$msrc" && patch -p1 \
      < "$src/$patch_file" >patch.log 2>&1) \
      || { cat "$msrc/patch.log"; fail "mutant $m: patch does not apply"; }
  else
    (cd "$msrc" && git apply "$src/$patch_file") \
      || fail "mutant $m: patch does not apply"
  fi
  # Preserve the difference and the build/run logs as evidence.
  cp "$patch_file" "$evidence/$m.patch"
  diff -u --label a/lib/state.c --label b/lib/state.c \
    "$src/lib/state.c" "$msrc/lib/state.c" > "$evidence/$m.diff" || true
  build_tree "$mbuild" "$msrc" >"$evidence/$m-build.log" 2>&1 \
    || fail "mutant $m: library build failed (compile failure is not a catch)"
  compile_tests "$mbuild" "$msrc" "$work/mut-$m" \
    >"$evidence/$m-testbuild.log" 2>&1 \
    || fail "mutant $m: test compile failed (compile failure is not a catch)"
  set +e
  "$work/mut-${m}_reference" >"$evidence/$m-reference.log" 2>&1
  rc_ref=$?
  "$work/mut-${m}_stream" >"$evidence/$m-stream.log" 2>&1
  rc_stream=$?
  set -e
  # The mutant is caught only if a test actually ran, reached a numeric
  # comparison, and was rejected with a printed deviation.
  n_dev=$(cat "$evidence/$m-reference.log" "$evidence/$m-stream.log" \
    | grep -c '^THEORA_LOOPFILTER_CASE .* FAIL' || true)
  if [ "$n_dev" -eq 0 ] || { [ "$rc_ref" -eq 0 ] && [ "$rc_stream" -eq 0 ]; }; then
    cat "$evidence/$m-reference.log" "$evidence/$m-stream.log"
    fail "mutant $m: tests did not reject it with numeric evidence (rc_reference=$rc_ref rc_stream=$rc_stream)"
  fi
  grep -hm1 '^THEORA_LOOPFILTER_CASE .* FAIL' \
    "$evidence/$m-reference.log" "$evidence/$m-stream.log" \
    | head -1 | sed "s|^|THEORA_LOOPFILTER_MUTANT $m first_deviation: |"
  grep -m1 -E 'got=|want=|early=|final=' \
    "$evidence/$m-reference.log" "$evidence/$m-stream.log" \
    >/dev/null || fail "mutant $m: rejection without numeric evidence"
  echo "THEORA_LOOPFILTER_MUTANT $m rejected rc_reference=$rc_ref rc_stream=$rc_stream"
done

echo "THEORA_LOOPFILTER_OK cases=6"
