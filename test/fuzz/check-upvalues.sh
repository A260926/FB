#!/usr/bin/env bash
#
# check-upvalues.sh
#
# Drives the structured open-upvalue fuzz chain (test/fuzz/upvalue_*.c).
#
# It does four things:
#   1. Builds the chain twice against the real public API: a normal build and
#      an ASan/UBSan build, and runs the fixed corpus plus >=300 deterministic
#      generated plans, each replayed twice in a fresh VM.
#   2. Asserts that the lockstep reference model and Wren agree on every event
#      (UPVALUE_MODEL_OK).
#   3. Asserts that Value-stack relocation really happened while open upvalues
#      were live and that the moving allocator really moved memory
#      (UPVALUE_MOVE_OK).
#   4. Asserts the replay/fresh-VM discipline (UPVALUE_REPLAY_OK).
#
# Then, in throwaway copies of the source, it independently breaks (a) the open
# upvalue stack-relocation correction and (b) same-slot upvalue capture reuse,
# and verifies the *same* checks now report a value/identity divergence or a
# memory diagnostic. A mutated copy that merely fails to compile is treated as
# a harness failure, never as a detection.
#
# Pure C99 plus the standard library; this file is the only scripting layer.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FUZZ="$ROOT/test/fuzz"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/upvalue-check.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

CC="${CC:-cc}"
COMMON_FLAGS=(-std=c99 -g -O1 -Wall -Wextra
  -I"$ROOT/src/include" -I"$ROOT/src/vm" -I"$ROOT/src/optional"
  -I"$FUZZ" -DWREN_FUZZ_TEST_HOOKS)
VM_SOURCES=("$ROOT"/src/vm/*.c "$ROOT"/src/optional/*.c)
FUZZ_SOURCES=("$FUZZ/upvalue_plan.c" "$FUZZ/upvalue_model.c"
              "$FUZZ/upvalue_fuzz_main.c")

GENERATED_MIN=300

build_and_run() {
  local binary="$1"; shift
  local label="$1"; shift
  "$CC" "${COMMON_FLAGS[@]}" "$@" \
    "${FUZZ_SOURCES[@]}" ${VM_SOURCES[@]} -lm -o "$binary"
  "$binary" | tee "$WORK/$label.log"
}

# Extracts a numeric counter ("key=N") from a run log.
counter() {
  local key="$1" file="$2"
  tr ' ' '\n' < "$file" | grep -E "^${key}=" | head -1 |
    sed "s/^${key}=//"
}

fail() { echo "CHECK FAILED: $*" >&2; exit 1; }

echo "== production build (no test hooks) =="
PROD_BIN="$WORK/upfuzz_prod"
"$CC" -std=c99 -g -O1 -Wall -Wextra \
  -I"$ROOT/src/include" -I"$ROOT/src/vm" -I"$ROOT/src/optional" -I"$FUZZ" \
  "${FUZZ_SOURCES[@]}" ${VM_SOURCES[@]} -lm -o "$PROD_BIN"
"$PROD_BIN" | tee "$WORK/prod.log"
# The hook accessors must not exist and the hook counters must stay zero.
if nm "$PROD_BIN" | grep -q "wrenFuzzTest"; then
  fail "test-hook symbols leaked into the production build"
fi
[ "$(counter hookMovesWithOpenUpvalue "$WORK/prod.log")" -eq 0 ] ||
  fail "production build reported hook counters"

echo "== normal build (test hooks enabled) =="
NORMAL_BIN="$WORK/upfuzz"
build_and_run "$NORMAL_BIN" normal

echo "== ASan/UBSan build =="
ASAN_BIN="$WORK/upfuzz_asan"
build_and_run "$ASAN_BIN" asan -fsanitize=address,undefined \
  -fno-omit-frame-pointer

NORMAL_LOG="$WORK/normal.log"
ASAN_LOG="$WORK/asan.log"

# --- UPVALUE_MODEL_OK: every item-wise comparison matched in both builds ----
grep -q "comparisons=" "$NORMAL_LOG" || fail "normal run produced no counters"
[ "$(counter comparisons "$NORMAL_LOG")" -gt 0 ] ||
  fail "normal build performed zero comparisons"
[ "$(counter comparisons "$ASAN_LOG")" -gt 0 ] ||
  fail "ASan build performed zero comparisons"
# Sanitizers abort noisily on a memory error; reaching the summary means clean.
grep -q "plans:" "$ASAN_LOG" || fail "ASan/UBSan run did not finish cleanly"
if grep -Eq "runtime error|AddressSanitizer|UndefinedBehaviorSanitizer" \
     "$ASAN_LOG"; then
  fail "ASan/UBSan reported a diagnostic"
fi
echo "UPVALUE_MODEL_OK"

# --- UPVALUE_MOVE_OK: relocation with open upvalues really occurred ---------
MOVES="$(counter movingAllocatorRelocations "$NORMAL_LOG")"
OPEN_MOVES="$(counter hookMovesWithOpenUpvalue "$NORMAL_LOG")"
GCS="$(counter hookCollections "$NORMAL_LOG")"
[ "${MOVES:-0}" -gt 0 ] || fail "moving allocator never relocated memory"
[ "${OPEN_MOVES:-0}" -gt 0 ] ||
  fail "no Value-stack move happened while open upvalues were live"
[ "${GCS:-0}" -gt 0 ] || fail "no garbage collection was observed"
echo "UPVALUE_MOVE_OK (moves=$MOVES movesWithOpenUpvalue=$OPEN_MOVES gcs=$GCS)"

# --- UPVALUE_REPLAY_OK: distinct plans, each replayed twice in a fresh VM ---
RUN_PLANS="$(counter run "$NORMAL_LOG")"
REPLAYS="$(counter replays "$NORMAL_LOG")"
[ "$RUN_PLANS" -ge $((GENERATED_MIN + 3)) ] ||
  fail "expected at least $((GENERATED_MIN + 3)) distinct plans, got $RUN_PLANS"
[ "$REPLAYS" -eq $((RUN_PLANS * 2)) ] ||
  fail "expected $((RUN_PLANS * 2)) fresh-VM replays, got $REPLAYS"
echo "UPVALUE_REPLAY_OK (plans=$RUN_PLANS replays=$REPLAYS)"

# ---------------------------------------------------------------------------
# Mutation tests. Each mutation is applied to a private source copy with an
# explicit anchor assertion. The mutated VM must still compile; only then does
# a runtime disagreement count as a detection.
# ---------------------------------------------------------------------------
copy_sources() {
  local dst="$1"
  mkdir -p "$dst"
  cp -R "$ROOT/src" "$dst/src"
  mkdir -p "$dst/test"
  cp -R "$FUZZ" "$dst/test/fuzz"
}

mutated_build_run() {
  local name="$1"; shift
  local dir="$WORK/mut-$name"
  copy_sources "$dir"
  python3 - "$dir" "$@" <<'PY'
import sys
root, mutation = sys.argv[1], sys.argv[2]
path = root + "/src/vm/" + (
    "wren_value.c" if mutation == "open-upvalue" else "wren_vm.c")
with open(path, "r") as f:
    text = f.read()
if mutation == "open-upvalue":
    anchor = """    // Open upvalues.
    for (ObjUpvalue* upvalue = fiber->openUpvalues;
         upvalue != NULL;
         upvalue = upvalue->next)
    {
      upvalue->value = fiber->stack + (upvalue->value - oldStack);
    }
"""
    assert text.count(anchor) == 1, "open-upvalue anchor not unique"
    text = text.replace(anchor,
        "    // MUTATION: open-upvalue relocation intentionally removed.\n")
else:
    anchor = """  // Found an existing upvalue for this local.
  if (upvalue != NULL && upvalue->value == local) return upvalue;
"""
    assert text.count(anchor) == 1, "capture-reuse anchor not unique"
    text = text.replace(anchor,
        "  // MUTATION: same-slot upvalue reuse intentionally disabled.\n")
with open(path, "w") as f:
    f.write(text)
sys.stderr.write("mutation " + mutation + " applied to " + path + "\n")
PY
  local bin="$dir/upfuzz-mut"
  if ! "$CC" -std=c99 -g -O1 -Wall -Wextra \
       -I"$dir/src/include" -I"$dir/src/vm" -I"$dir/src/optional" \
       -I"$dir/test/fuzz" -DWREN_FUZZ_TEST_HOOKS \
       "$dir"/test/fuzz/upvalue_*.c "$dir"/src/vm/*.c "$dir"/src/optional/*.c \
       -lm -o "$bin" 2> "$dir/build.err"; then
    echo "mutated '$name' failed to compile (not a valid detection):" >&2
    cat "$dir/build.err" >&2
    exit 1
  fi
  echo "$bin"
}

expect_detection() {
  local name="$1" bin="$2"
  local out="$WORK/mut-$name/run.log"
  set +e
  "$bin" > "$out" 2>&1
  local rc=$?
  set -e
  if [ "$rc" -eq 0 ]; then
    fail "mutation '$name' was not detected (checks still passed)"
  fi
  # A detected defect surfaces as an item-wise model/Wren divergence, a Wren
  # runtime/assert error from a corrupted upvalue, or a sanitizer diagnostic.
  local pattern='differs|mismatch|decl=|Wren error|Assert failed|runtime error|AddressSanitizer|UndefinedBehaviorSanitizer|heap-use-after-free|SEGV'
  if ! grep -Eq "$pattern" "$out"; then
    fail "mutation '$name' failed but produced no value/identity or memory diagnostic"
  fi
  echo "mutation '$name' detected as required (exit $rc):"
  grep -E "$pattern" "$out" | head -2
}

MUT_BIN="$(mutated_build_run open-upvalue open-upvalue)"
expect_detection open-upvalue "$MUT_BIN"

MUT_BIN="$(mutated_build_run capture-reuse capture-reuse)"
expect_detection capture-reuse "$MUT_BIN"

echo
echo "ALL UPVALUE FUZZ CHECKS PASSED"
