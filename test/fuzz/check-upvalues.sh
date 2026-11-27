#!/usr/bin/env bash
#
# check-upvalues.sh
#
# End-to-end driver for the structured open-upvalue fuzzing chain.
#
#   * Builds the fuzzer against the unmodified VM twice: a plain build and an
#     ASan/UBSan build. Both must pass the fixed corpus and >=300 generated
#     plans, each replayed twice in a fresh VM.
#   * Then makes temporary source copies and independently (a) deletes the
#     open-upvalue stack-relocation fix and (b) breaks same-slot upvalue
#     reuse. Each mutated VM must still compile and must be caught by the same
#     checks, either as a value/identity deviation or as a memory diagnostic.
#     A compile failure is explicitly NOT counted as a detection.
#
# The moving allocator, reference model, decoder, renderer and execution chain
# live in test/fuzz/*.c; this file is only glue.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
WORK="${TMPDIR:-/tmp}/upvalue-fuzz.$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CC="${CC:-cc}"
STD="-std=c99"
OPT="-O1 -g"
INCLUDES="-I$ROOT/src/include -I$ROOT/src/vm -I$ROOT/src/optional"
VM_SOURCES="$ROOT/src/vm/wren_vm.c $ROOT/src/vm/wren_value.c \
$ROOT/src/vm/wren_utils.c $ROOT/src/vm/wren_core.c \
$ROOT/src/vm/wren_compiler.c $ROOT/src/vm/wren_debug.c \
$ROOT/src/vm/wren_primitive.c \
$ROOT/src/optional/wren_opt_meta.c $ROOT/src/optional/wren_opt_random.c"
FUZZ_SOURCES="$ROOT/test/fuzz/upvalue_plan.c $ROOT/test/fuzz/upvalue_model.c \
$ROOT/test/fuzz/upvalue_fuzz.c"

build() {
  local out="$1"; shift
  # shellcheck disable=SC2086
  "$CC" $STD $OPT $INCLUDES -DWREN_FUZZ_TESTING "$@" \
    $FUZZ_SOURCES $VM_SOURCES -lm -o "$out"
}

echo "== building plain fuzz binary =="
build "$WORK/upvalue_fuzz"

echo "== building ASan/UBSan fuzz binary =="
build "$WORK/upvalue_fuzz_asan" -fsanitize=address,undefined \
  -fno-sanitize-recover=all -fno-omit-frame-pointer

run_normal() {
  local bin="$1" label="$2"
  echo "== running $label =="
  "$bin"
}

# Both healthy builds must pass the full fixed + generated chain.
run_normal "$WORK/upvalue_fuzz" "plain build"
PLAIN_OUT="$WORK/plain.txt"
"$WORK/upvalue_fuzz" >"$PLAIN_OUT"
run_normal "$WORK/upvalue_fuzz_asan" "ASan/UBSan build"
ASAN_OUT="$WORK/asan.txt"
"$WORK/upvalue_fuzz_asan" >"$ASAN_OUT"

# The run only counts if relocations with open upvalues and real GCs happened.
grep -q "openUpvalueMoves=[1-9]" "$PLAIN_OUT"
grep -q "gcCount=[1-9]" "$PLAIN_OUT"
grep -q "openUpvalueMoves=[1-9]" "$ASAN_OUT"
grep -q "gcCount=[1-9]" "$ASAN_OUT"

# Replay stability is enforced inside the driver (every plan runs twice in a
# fresh VM and the counters must match exactly).
echo "UPVALUE_MODEL_OK"
echo "UPVALUE_MOVE_OK"
echo "UPVALUE_REPLAY_OK"

# ---------------------------------------------------------------------------
# Mutation experiments in temporary source copies.
#
# The two fix sites carry explicit marker comments; the mutations delete the
# lines between each marker pair. A mutated tree that fails to compile is a
# test-harness bug, not a detection, and aborts this script.
# ---------------------------------------------------------------------------

copy_sources() {
  local dst="$1"
  mkdir -p "$dst/src/vm" "$dst/src/optional" "$dst/src/include" \
           "$dst/test/fuzz"
  cp "$ROOT/src/vm"/*.c "$ROOT/src/vm"/*.h "$ROOT/src/vm"/*.inc \
     "$dst/src/vm/"
  cp "$ROOT/src/optional"/*.c "$ROOT/src/optional"/*.h \
     "$ROOT/src/optional"/*.inc "$dst/src/optional/"
  cp "$ROOT/src/include"/*.h "$dst/src/include/"
  cp "$ROOT/test/fuzz"/*.c "$ROOT/test/fuzz"/*.h "$dst/test/fuzz/"
}

mutated_build() {
  local dir="$1" out="$2"; shift 2
  # shellcheck disable=SC2086
  "$CC" $STD $OPT "-I$dir/src/include" "-I$dir/src/vm" "-I$dir/src/optional" \
    -DWREN_FUZZ_TESTING "$@" \
    "$dir/test/fuzz/upvalue_plan.c" "$dir/test/fuzz/upvalue_model.c" \
    "$dir/test/fuzz/upvalue_fuzz.c" \
    "$dir"/src/vm/*.c "$dir"/src/optional/*.c -lm -o "$out"
}

# Returns 0 when the mutated binary is detected (nonzero exit), 1 otherwise.
expect_detection() {
  local name="$1" bin="$2" asan="$3"
  local log="$WORK/$name.log"
  if "$bin" >"$log" 2>&1; then
    echo "FAIL: $name mutation was NOT detected by the plain build" >&2
    cat "$log" >&2
    return 1
  fi
  if grep -Eq "MISMATCH|REPLAY INSTABILITY|canary|leak" "$log"; then
    echo "  $name: caught as a value/identity/allocator deviation"
    return 0
  fi
  # If the plain build crashed without a structured message, require the
  # sanitizer build to produce an explicit memory diagnostic.
  local alog="$WORK/$name.asan.log"
  if "$asan" >"$alog" 2>&1; then
    echo "FAIL: $name mutation not detected even under ASan/UBSan" >&2
    cat "$alog" >&2
    return 1
  fi
  if grep -Eq "runtime error:|AddressSanitizer|heap-use-after-free|stack-buffer" \
       "$alog"; then
    echo "  $name: caught as a memory diagnostic"
    return 0
  fi
  echo "FAIL: $name mutation failed but with no recognized diagnostic" >&2
  cat "$alog" >&2
  return 1
}

echo "== mutation 1: remove open-upvalue stack relocation fix =="
MOVE_DIR="$WORK/mut-move"
copy_sources "$MOVE_DIR"
# Delete the marked relocation block (inclusive of the markers).
sed -i '' '/>FUZZ-MOVE-FIX-START/,/>FUZZ-MOVE-FIX-END/d' \
  "$MOVE_DIR/src/vm/wren_value.c"
mutated_build "$MOVE_DIR" "$WORK/mut_move"
mutated_build "$MOVE_DIR" "$WORK/mut_move_asan" \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -fno-omit-frame-pointer
expect_detection "move-fix" "$WORK/mut_move" "$WORK/mut_move_asan"

echo "== mutation 2: break same-slot capture reuse =="
REUSE_DIR="$WORK/mut-reuse"
copy_sources "$REUSE_DIR"
sed -i '' '/>FUZZ-REUSE-FIX-START/,/>FUZZ-REUSE-FIX-END/d' \
  "$REUSE_DIR/src/vm/wren_vm.c"
mutated_build "$REUSE_DIR" "$WORK/mut_reuse"
mutated_build "$REUSE_DIR" "$WORK/mut_reuse_asan" \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -fno-omit-frame-pointer
expect_detection "reuse-fix" "$WORK/mut_reuse" "$WORK/mut_reuse_asan"

# Leave a directly runnable rendered example next to the sources for inspection.
"$WORK/upvalue_fuzz" --print-source 0 > "$ROOT/test/fuzz/example.generated.wren"

echo "ALL UPVALUE FUZZ CHECKS PASSED"
