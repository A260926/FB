#!/usr/bin/env bash
# Regression orchestration for the projection demixing-matrix PCM test.
#
# 1. Builds tests/test_projection_pcm.c against the normal source tree
#    (same plain float CMake configuration as verification/baseline.sh)
#    and runs it: NORMAL_PROJECTION_PCM_PASS.
# 2. Repeats in three isolated temporary copies of the source, each with
#    exactly one seeded defect:
#      SIGN      matrix loading drops the sign of negative coefficients
#      TRANSPOSE float demixing reads the matrix transposed
#      CLEAR     float output callback no longer clears per call
#    A copy counts as detected (DETECTED_<name>) only when it still
#    builds and the unmodified test fails with a numeric assertion
#    (exit code 1 with a MISMATCH report). Compile failures, crashes,
#    missing files, or zero compared samples never count as detections;
#    they abort this script instead.
set -euo pipefail

SRC_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Explicit template so TMPDIR is honored everywhere (plain `mktemp -d`
# ignores TMPDIR on some platforms, e.g. macOS).
WORK="$(mktemp -d "${TMPDIR:-/tmp}/check-projection-pcm.XXXXXXXX")"
trap 'rm -rf "$WORK"' EXIT

CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  -DBUILD_TESTING=ON
  -DOPUS_ENABLE_FLOAT_API=ON
  -DBUILD_SHARED_LIBS=OFF
)

# build_test <src_dir> <out_dir>: configure, build libopus, compile and
# link the test binary. Returns non-zero on any build failure.
build_test() {
  local src="$1" out="$2"
  mkdir -p "$out"
  cmake -S "$src" -B "$out/build" "${CMAKE_ARGS[@]}" \
      >"$out/config.log" 2>&1 || return 1
  cmake --build "$out/build" --target opus -j 4 \
      >"$out/build.log" 2>&1 || return 1
  cc -std=c99 -I"$src/include" "$src/tests/test_projection_pcm.c" \
      "$out/build/libopus.a" -lm -o "$out/test_projection_pcm" || return 1
}

# make_copy <dest>: copy the source tree (without .git) into <dest>.
make_copy() {
  local dest="$1"
  mkdir -p "$dest"
  (cd "$SRC_ROOT" && tar --exclude=.git -cf - .) | (cd "$dest" && tar -xf -)
}

# Seeded defects. Each returns non-zero if its source pattern is missing
# or the edit did not take, so silent no-op mutations cannot pass.

mutation_sign() {
  local f="$1/src/opus_projection_decoder.c"
  grep -qF 'buf[i] = (opus_int16)s;' "$f" || return 1
  perl -pi -e 's/buf\[i\] = \(opus_int16\)s;/buf[i] = (opus_int16)(s < 0 ? -s : s);/' "$f"
  grep -qF 'buf[i] = (opus_int16)(s < 0 ? -s : s);' "$f" || return 1
}

mutation_transpose() {
  local f="$1/src/mapping_matrix.c"
  grep -qF 'MATRIX_INDEX(matrix->rows, row, input_row)' "$f" || return 1
  perl -pi -e 's/MATRIX_INDEX\(matrix->rows, row, input_row\)/MATRIX_INDEX(matrix->cols, input_row, row)/' "$f"
  grep -qF 'MATRIX_INDEX(matrix->cols, input_row, row)' "$f" || return 1
}

mutation_clear() {
  local f="$1/src/opus_projection_decoder.c"
  grep -qF 'OPUS_CLEAR(float_dst, frame_size * dst_stride);' "$f" || return 1
  perl -0pi -e 's/  if \(dst_channel == 0\)\n    OPUS_CLEAR\(float_dst, frame_size \* dst_stride\);\n//' "$f"
  ! grep -qF 'OPUS_CLEAR(float_dst' "$f" || return 1
}

# run_mutation <name> <mutation_function> <token>
run_mutation() {
  local name="$1" fn="$2" token="$3"
  local copy="$WORK/copy_$name" out="$WORK/build_$name"
  local rc
  make_copy "$copy"
  if ! "$fn" "$copy"; then
    echo "ERROR: mutation $name could not be applied" >&2
    exit 1
  fi
  if ! build_test "$copy" "$out"; then
    echo "ERROR: mutated source ($name) failed to build" >&2
    exit 1
  fi
  set +e
  "$out/test_projection_pcm" >"$WORK/run_$name.log" 2>&1
  rc=$?
  set -e
  if [ "$rc" -eq 0 ]; then
    echo "ERROR: mutation $name was NOT detected (test passed)" >&2
    exit 1
  fi
  if [ "$rc" -ne 1 ]; then
    echo "ERROR: mutation $name exited with $rc, not a numeric assertion" >&2
    cat "$WORK/run_$name.log" >&2
    exit 1
  fi
  if ! grep -q '^MISMATCH group=float ' "$WORK/run_$name.log"; then
    echo "ERROR: mutation $name failed without the expected float mismatch" >&2
    cat "$WORK/run_$name.log" >&2
    exit 1
  fi
  grep '^MISMATCH' "$WORK/run_$name.log" | head -1
  echo "$token"
}

echo "== normal source =="
if ! build_test "$SRC_ROOT" "$WORK/normal"; then
  echo "ERROR: normal build failed" >&2
  cat "$WORK/normal/config.log" "$WORK/normal/build.log" >&2 2>/dev/null || true
  exit 1
fi
if ! "$WORK/normal/test_projection_pcm" >"$WORK/normal/run.log" 2>&1; then
  echo "ERROR: normal run failed" >&2
  cat "$WORK/normal/run.log" >&2
  exit 1
fi
cat "$WORK/normal/run.log"
grep -q '^PROJECTION_PCM_PASS$' "$WORK/normal/run.log" || {
  echo "ERROR: pass marker missing in normal run" >&2
  exit 1
}

# Every normal group must report a positive number of compared samples.
counts_ok=1
for g in float int16 reset; do
  n=$(sed -n "s/^GROUP_OK $g samples=\([0-9][0-9]*\).*/\1/p" \
      "$WORK/normal/run.log" | head -1)
  if [ -z "${n:-}" ] || [ "$n" -le 0 ]; then
    echo "ERROR: group $g compared ${n:-no} samples" >&2
    counts_ok=0
  else
    echo "GROUP $g compared $n samples"
  fi
done
[ "$counts_ok" -eq 1 ] || exit 1
echo NORMAL_PROJECTION_PCM_PASS

run_mutation sign mutation_sign DETECTED_SIGN
run_mutation transpose mutation_transpose DETECTED_TRANSPOSE
run_mutation clear mutation_clear DETECTED_CLEAR
