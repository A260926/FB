#!/usr/bin/env bash
#
# End-to-end driver for the hex-string relation regression.
#
#  1. Builds the library at YR_STRING_CHAINING_THRESHOLD 200 and 2, both as a
#     normal build and under ASan/UBSan, and runs the same generated/fixed
#     content through the model-vs-scanner comparison in every build.
#  2. Proves the chain representation switches: two short jumps stay one
#     string at 200 but become three fragments at 2, while two long jumps are
#     three fragments in both builds.
#  3. In temporary source copies, independently breaks (a) the chain-gap
#     lower-bound inclusion check and (b) the masked-negation check, rebuilds
#     and confirms the regression detects each change behaviourally. A mere
#     compile failure never counts as detection.
#
# On success prints:
#   HEX_RELATION_OK HEX_CHAIN_SWITCH_OK HEX_SENSITIVITY_OK
set -euo pipefail

cd "$(dirname "$0")/../.."
repo_root=$(pwd)
cc=${CC:-cc}

work=$(mktemp -d "${TMPDIR:-/tmp}/yara-hex-model.XXXXXX")
trap 'rm -rf "$work"' EXIT

HM_DIR=tests/hex_model
HM_CORE=(
  "$HM_DIR/hex_model.c"
  "$HM_DIR/hex_scan.c"
  "$HM_DIR/hex_check.c"
  "$HM_DIR/hex_cases.c"
  "$HM_DIR/hex_runner.c"
)
DRIVER_SRC="$HM_DIR/hex_driver.c"
PROBE_SRC="$HM_DIR/hex_chain_probe.c"

base_flags=(-std=gnu11 -O1 -g -DHAVE_STDBOOL_H=1 -DBUCKETS_128 -DCHECKSUM_1B
  -DUSE_NO_PROC -I libyara/include -I libyara -I "$HM_DIR" -Wall -Wextra)

# Builds one library + driver + chain probe in $1 for threshold $2 with the
# extra (sanitizer) flags in $3.
build_variant() {
  local out=$1 threshold=$2 extra=$3
  mkdir -p "$out/lib"
  if [[ -n $extra ]]; then
    EXTRA_CFLAGS="$extra" bash task_support/build-library.sh "$out/lib" "$threshold"
  else
    bash task_support/build-library.sh "$out/lib" "$threshold"
  fi
  # shellcheck disable=SC2086
  "$cc" "${base_flags[@]}" -DYR_STRING_CHAINING_THRESHOLD="$threshold" $extra \
    "${HM_CORE[@]}" "$DRIVER_SRC" "$out/lib/libyara.a" -pthread -lm \
    -o "$out/driver"
  # shellcheck disable=SC2086
  "$cc" "${base_flags[@]}" -DYR_STRING_CHAINING_THRESHOLD="$threshold" $extra \
    "$PROBE_SRC" "$out/lib/libyara.a" -pthread -lm -o "$out/probe"
}

relation_ok=1
declare -a comp_lines

run_variant() {
  local out=$1 label=$2
  echo "---- relation run: $label ----"
  "$out/probe"
  local summary
  summary=$("$out/driver" --seed 1125 --cases 240)
  echo "$summary" | grep -E '^PINNED' || true
  echo "$summary" | grep '^SUMMARY'
  comp_lines+=("$(echo "$summary" | grep '^SUMMARY' | sed 's/.*comparisons=//;s/ .*//')")
  echo "$summary" | grep -q 'HEX_MODEL_PASS' || relation_ok=0
}

echo "################ normal + sanitizer relation runs ################"
for threshold in 200 2; do
  normal="$work/normal-$threshold"
  build_variant "$normal" "$threshold" ""
  run_variant "$normal" "normal threshold=$threshold"

  san="$work/san-$threshold"
  build_variant "$san" "$threshold" "-fsanitize=address,undefined -fno-omit-frame-pointer"
  ASAN_OPTIONS="detect_leaks=0:halt_on_error=1" UBSAN_OPTIONS="halt_on_error=1" \
    run_variant "$san" "asan/ubsan threshold=$threshold"
done

if [[ $relation_ok -ne 1 ]]; then
  echo "relation check failed" >&2
  exit 1
fi

# The same seed/cases must yield the identical number of field comparisons in
# every build, demonstrating the two thresholds (and sanitizer builds) agree.
first=${comp_lines[0]}
for v in "${comp_lines[@]}"; do
  [[ $v == "$first" ]] || { echo "comparison counts differ: ${comp_lines[*]}" >&2; exit 1; }
done
echo "HEX_RELATION_OK (comparisons per build=$first)"

echo "################ chain representation switch ################"
"$work/normal-200/probe" | tee "$work/probe200.txt"
"$work/normal-2/probe"   | tee "$work/probe2.txt"
grep -q 'two_short_jumps threshold=200 parts=0' "$work/probe200.txt"
grep -q 'two_long_jumps threshold=200 parts=3'  "$work/probe200.txt"
grep -q 'two_short_jumps threshold=2 parts=3'   "$work/probe2.txt"
grep -q 'two_long_jumps threshold=2 parts=3'    "$work/probe2.txt"
echo "HEX_CHAIN_SWITCH_OK"

echo "################ source mutation sensitivity ################"

# Copies the pristine tree, applies one mutation, rebuilds and confirms the
# regression fails behaviourally. Compile success is asserted first so a
# broken build can never masquerade as a detected mutation.
run_mutant() {
  local name=$1
  local mutant="$work/mutant-$name"
  cp -R "$repo_root" "$mutant"

  case $name in
    gap_lower_bound)
      # Inclusion check: a gap equal to the minimum must be accepted. Drop
      # equality so an exact-minimum gap is wrongly rejected.
      sed -i '' 's/chain_gap_min <=/chain_gap_min </g' "$mutant/libyara/scan.c"
      if grep -q 'chain_gap_min <=' "$mutant/libyara/scan.c"; then
        echo "gap mutation did not apply" >&2; exit 1
      fi
      ;;
    masked_negation)
      # MASKED_NOT_LITERAL must test (!=); flip both RE engines to (==).
      sed -i '' 's/(\*input & mask) != value)/(*input \& mask) == value)/' \
        "$mutant/libyara/re.c"
      sed -i '' 's/(\*current->input & mask) != value)/(*current->input \& mask) == value)/' \
        "$mutant/libyara/re.c"
      if [[ $(grep -c 'mask) == value' "$mutant/libyara/re.c") -lt 2 ]]; then
        echo "masked-negation mutation did not apply" >&2; exit 1
      fi
      ;;
    *)
      echo "unknown mutant $name" >&2; exit 1;;
  esac

  local mout="$mutant/build"
  mkdir -p "$mout"
  ( cd "$mutant" && bash task_support/build-library.sh "$mout" 200 )
  [[ -f "$mout/libyara.a" ]] || { echo "mutant $name did not compile" >&2; exit 1; }

  ( cd "$mutant" && "$cc" "${base_flags[@]}" -DYR_STRING_CHAINING_THRESHOLD=200 \
      "${HM_CORE[@]}" "$DRIVER_SRC" "$mout/libyara.a" -pthread -lm \
      -o "$mout/driver" )

  set +e
  ASAN_OPTIONS="detect_leaks=0" "$mout/driver" --fixed-only >"$mutant/out.txt" 2>&1
  local rc=$?
  set -e
  if [[ $rc -eq 0 ]]; then
    echo "mutant $name was NOT detected (driver unexpectedly passed)" >&2
    cat "$mutant/out.txt" >&2
    exit 1
  fi
  grep -q 'MISMATCH\|chain fragment mismatch' "$mutant/out.txt" || {
    echo "mutant $name failed for the wrong reason" >&2
    cat "$mutant/out.txt" >&2
    exit 1
  }
  echo "mutant $name detected behaviourally (driver rc=$rc)"
}

run_mutant gap_lower_bound
run_mutant masked_negation
echo "HEX_SENSITIVITY_OK"
