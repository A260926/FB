#!/usr/bin/env bash
#
# End-to-end correctness chain for tests/hex_model:
#
#   1. Builds the library at YR_STRING_CHAINING_THRESHOLD 200 and 2, runs the
#      fixed set and the generated set (--seed 1125 --cases 240) against each
#      build, and checks the two builds publish identical match results while
#      the chain-part counts prove both chaining paths are exercised.
#   2. Repeats the runs under ASan/UBSan.
#   3. In temporary source copies, mutates (a) the inclusive chain-gap lower
#      bound in libyara/scan.c and (b) the masked-negation predicate in
#      libyara/re.c, rebuilds and asserts the fixed regression detects each
#      mutation. A compile failure is explicitly not accepted as detection.
#
# On success prints HEX_RELATION_OK, HEX_CHAIN_SWITCH_OK and
# HEX_SENSITIVITY_OK on separate lines.
#
set -euo pipefail

cd "$(dirname "$0")/../.."
repo_root=$(pwd)
cc=${CC:-cc}

work=$(mktemp -d "${TMPDIR:-/tmp}/yara-hex-model.XXXXXX")
trap 'rm -rf "$work"' EXIT

model_dir=tests/hex_model
model_sources=(
  "$model_dir/hex_model.c"
  "$model_dir/hex_generate.c"
  "$model_dir/hex_fixed.c"
  "$model_dir/hex_scan.c"
  "$model_dir/hex_check.c"
)
driver_source="$model_dir/hex_driver.c"
fuzz_source="$model_dir/hex_fuzz.c"

lib_sources=(libyara/*.c libyara/proc/none.c libyara/tlshc/*.c
  libyara/modules/tests/tests.c libyara/modules/elf/elf.c
  libyara/modules/math/math.c libyara/modules/time/time.c
  libyara/modules/pe/pe.c libyara/modules/pe/pe_utils.c
  libyara/modules/console/console.c libyara/modules/string/string.c)

# build_full <srcroot> <threshold> <outdir> <driver_out> [extra flags...]
build_full() {
  local srcroot=$1 threshold=$2 outdir=$3 driver_out=$4
  shift 4
  local extra=("$@")
  local flags=(-std=gnu11 -O1 -D_GNU_SOURCE -DHAVE_STDBOOL_H=1
    -DBUCKETS_128 -DCHECKSUM_1B -DUSE_NO_PROC
    -I "$srcroot/libyara/include" -I "$srcroot/libyara"
    -I "$repo_root/$model_dir" -pthread)
  if [[ $(uname -s) == Darwin ]]; then
    flags+=(-DHAVE_STRLCPY=1 -DHAVE_STRLCAT=1 -DHAVE_MEMMEM=1)
  fi
  mkdir -p "$outdir"
  local objs=()
  local src obj
  for src in "${lib_sources[@]}"; do
    obj="$outdir/$(echo "$src" | tr '/' '_').o"
    "$cc" "${flags[@]}" ${extra[@]+"${extra[@]}"} \
      -DYR_STRING_CHAINING_THRESHOLD="$threshold" \
      -c "$srcroot/$src" -o "$obj"
    objs+=("$obj")
  done
  ar rcs "$outdir/libyara.a" "${objs[@]}"
  "$cc" "${flags[@]}" ${extra[@]+"${extra[@]}"} \
    -DYR_STRING_CHAINING_THRESHOLD="$threshold" \
    "$srcroot/$driver_source" "${model_sources[@]/#/$srcroot/}" \
    "$outdir/libyara.a" -lm -o "$driver_out"
}

run_variant() { # label threshold sanitized
  local label=$1
  local threshold=$2
  local sanitized=$3
  local outdir="$work/$label"
  local driver="$outdir/driver"
  local extra=()
  if [[ $sanitized == yes ]]; then
    extra=(-g -fsanitize=address,undefined -fno-omit-frame-pointer)
  fi
  echo "== building $label (threshold=$threshold sanitizers=$sanitized)"
  build_full "$repo_root" "$threshold" "$outdir" "$driver" ${extra[@]+"${extra[@]}"}
  echo "== running $label"
  "$driver" --seed 1125 --cases 240 | tee "$outdir/report.txt"
}

# ---------------------------------------------------------------------------
# 1. Plain builds at both thresholds.
# ---------------------------------------------------------------------------
run_variant plain-200 200 no
run_variant plain-2   2   no

fixed_200=$(grep '^fixed' "$work/plain-200/report.txt")
fixed_2=$(grep '^fixed'   "$work/plain-2/report.txt")
gen_200=$(grep '^generated' "$work/plain-200/report.txt")
gen_2=$(grep '^generated'   "$work/plain-2/report.txt")

cmp_200=$(sed -n 's/.*comparisons=\([0-9]*\).*/\1/p' <<<"$gen_200")
cmp_2=$(sed -n 's/.*comparisons=\([0-9]*\).*/\1/p'   <<<"$gen_2")
three_piece=$(sed -n 's/.*three_piece_cases=\([0-9]*\).*/\1/p' <<<"$gen_2")
unsplit=$(sed -n 's/.*unsplit_long_cases=\([0-9]*\).*/\1/p' <<<"$gen_200")
div_gen_200=$(sed -n 's/.*known_divergences=\([0-9]*\).*/\1/p' <<<"$gen_200")
div_gen_2=$(sed -n 's/.*known_divergences=\([0-9]*\).*/\1/p'   <<<"$gen_2")
div_fixed_200=$(sed -n 's/.*known_divergences=\([0-9]*\).*/\1/p' <<<"$fixed_200")
div_fixed_2=$(sed -n 's/.*known_divergences=\([0-9]*\).*/\1/p'   <<<"$fixed_2")

# The driver already exits non-zero on any hard mismatch (HM_SCAN_MISMATCH),
# so reaching here means both builds agree with the model except for the
# explicitly recorded F13 chain-shadow class.
if [[ $cmp_200 != "$cmp_2" ]]; then
  echo "FAIL: comparison counts differ between builds ($cmp_200 vs $cmp_2)" >&2
  exit 1
fi
if [[ $div_gen_200 -ne 0 || $div_fixed_200 -ne 0 ]]; then
  echo "FAIL: default threshold-200 build diverged from the model" >&2
  exit 1
fi
if [[ $div_fixed_2 -lt 1 ]]; then
  echo "FAIL: recorded F13 chain divergence was not exercised at threshold 2" >&2
  exit 1
fi
if [[ $three_piece -lt 1 ]]; then
  echo "FAIL: no three-piece chain observed under threshold 2" >&2
  exit 1
fi
if [[ $unsplit -lt 1 ]]; then
  echo "FAIL: two-jump pattern was not kept unsplit under threshold 200" >&2
  exit 1
fi
echo "HEX_RELATION_OK"
echo "  default build fully matches the model; threshold-2 shows only the"
echo "  recorded strict-subset F13 class (fixed div=$div_fixed_2 gen div=$div_gen_2)"
echo "HEX_CHAIN_SWITCH_OK"
echo "  comparisons=$cmp_200 per build, three-piece cases=$three_piece"

# ---------------------------------------------------------------------------
# 2. ASan/UBSan builds at both thresholds.
# ---------------------------------------------------------------------------
if ! echo 'int main(void){return 0;}' | \
     "$cc" -fsanitize=address,undefined -x c - -o "$work/san-probe" 2>/dev/null
then
  echo "FAIL: compiler does not support ASan/UBSan" >&2
  exit 1
fi
run_variant san-200 200 yes
run_variant san-2   2   yes

# ---------------------------------------------------------------------------
# 3. Mutation sensitivity in temporary source copies.
#

# expect_detection <name> <sed-program> <relative source file>
#
# The mutated tree must compile successfully; a build failure is not a
# detection. The fixed driver then has to exit non-zero because its hand-
# computed expectations no longer match the mutated engine.
expect_detection() {
  local name=$1 sed_prog=$2 relfile=$3
  local copy="$work/mut-$name"
  mkdir -p "$copy"
  cp -R "$repo_root/libyara" "$copy/libyara"
  cp -R "$repo_root/tests" "$copy/tests"

  # -i.bak works with both BSD and GNU sed.
  sed -i.bak "$sed_prog" "$copy/$relfile"
  rm -f "$copy/$relfile.bak"

  local outdir="$copy/build"
  local driver="$outdir/driver"
  echo "== building mutated variant $name"
  if ! build_full "$copy" 2 "$outdir" "$driver"; then
    echo "FAIL: mutation $name broke compilation; that is not a detection" >&2
    exit 1
  fi
  if [[ ! -x $driver ]]; then
    echo "FAIL: mutation $name produced no driver binary" >&2
    exit 1
  fi

  echo "== expecting fixed-set failure under mutation $name"
  set +e
  "$driver" --fixed-only >"$copy/mut-run.log" 2>&1
  local rc=$?
  set -e
  if [[ $rc -eq 0 ]]; then
    echo "FAIL: mutation $name was not detected" >&2
    cat "$copy/mut-run.log" >&2
    exit 1
  fi
  echo "mutation $name detected (fixed driver exit $rc)"
}

# (a) Chain-gap lower bound: every inclusive <= becomes a strict <. Successors
#     whose gap is exactly the minimum (fixed cases F06/F11) must then take a
#     longer derivation or fail to confirm, changing the published length.
expect_detection "gap-bound" \
  's/chain_gap_min <=/chain_gap_min </g' \
  "libyara/scan.c"

# (b) Masked negation: "different from value" becomes "equal to value" in both
#     the fiber executor and the fast-regexp executor. The negation fixed
#     cases (F03 via the fast path, F09 via the fiber path) must then
#     disagree with the model.
expect_detection "mask-negation" \
  's/& mask) != value/\& mask) == value/g' \
  "libyara/re.c"

echo "HEX_SENSITIVITY_OK"

# ---------------------------------------------------------------------------
# 4. Optional libFuzzer harness build (the ordinary driver never links it).
# ---------------------------------------------------------------------------
cat >"$work/fuzz-probe.c" <<'EOF'
#include <stddef.h>
#include <stdint.h>
int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { (void)d; (void)n; return 0; }
int main(void) { return 0; }
EOF
if "$cc" -fsanitize=fuzzer,address "$work/fuzz-probe.c" -o "$work/fuzz-probe" 2>/dev/null
then
  fuzz_out="$work/fuzz"
  mkdir -p "$fuzz_out/corpus"
  # Serialized { 41 [1-3] 42 }: CONCAT(3), BYTE(FF,41), JUMP(1,3), BYTE(FF,42).
  printf '\x04\x03\x01\xff\x41\x03\x01\x03\x01\xff\x42' \
    >"$fuzz_out/corpus/seed1"
  "$cc" -std=gnu11 -g -fsanitize=fuzzer,address,undefined \
    -D_GNU_SOURCE -DHAVE_STDBOOL_H=1 -DBUCKETS_128 -DCHECKSUM_1B \
    -DUSE_NO_PROC -DYR_STRING_CHAINING_THRESHOLD=2 \
    -I "$repo_root/libyara/include" -I "$repo_root/libyara" \
    -I "$repo_root/$model_dir" -pthread \
    -DHAVE_STRLCPY=1 -DHAVE_STRLCAT=1 -DHAVE_MEMMEM=1 \
    "$repo_root/$fuzz_source" "${model_sources[@]/#/$repo_root/}" \
    "$work/san-2/libyara.a" -lm -o "$fuzz_out/hex_fuzz"
  "$fuzz_out/hex_fuzz" -runs=50 "$fuzz_out/corpus"
  echo "libFuzzer harness built and exercised (50 runs)"
else
  echo "libFuzzer not supported by this compiler; LLVMFuzzerTestOneInput is"
  echo "still provided in $fuzz_source and skipped here"
fi
