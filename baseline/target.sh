#!/usr/bin/env bash
set -euo pipefail
root=$(pwd)
test -s baseline/harness/tests/line_contract.rs || { echo 'missing line_contract.rs' >&2; exit 1; }
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
export CARGO_TARGET_DIR="$scratch/build" CARGO_HOME="$scratch/cargo" CARGO_NET_OFFLINE=true
manifest=baseline/harness/Cargo.toml
cargo test --locked --manifest-path "$manifest" --test line_contract -- --exact line_contract --nocapture 2>&1 | tee "$scratch/original.log"
grep -q 'test result: ok. 1 passed; 0 failed;' "$scratch/original.log"
grep -q 'LINE_CONTRACT_CASES=24' "$scratch/original.log"
if grep -q '1 ignored' "$scratch/original.log"; then exit 1; fi
for name in carry fixed transient; do
  copy="$scratch/$name"
  mkdir "$copy"
  tar --exclude=.git --exclude=target -cf - . | (cd "$copy"; tar -xf -)
  (
    cd "$copy"
    git init -q
    git apply "$root/baseline/mutations/$name.patch"
    cargo test --locked --manifest-path "$manifest" --test line_contract --no-run > "$scratch/$name-build.log" 2>&1
    set +e
    cargo test --locked --manifest-path "$manifest" --test line_contract -- --exact line_contract --nocapture > "$scratch/$name-result.log" 2>&1
    rc=$?
    set -e
    if [[ "$rc" == 0 ]]; then echo "undetected mutation $name" >&2; exit 1; fi
    grep -q 'LINE_MODEL_MISMATCH' "$scratch/$name-result.log"
    echo "MUTATION_DETECTED $name"
    grep -m1 'LINE_MODEL_MISMATCH' "$scratch/$name-result.log"
  )
done
bash baseline/run.sh
echo GIMLI_LINE_CONTRACT_OK cases=24 mutants=3
