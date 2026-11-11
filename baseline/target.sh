#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
export CARGO_HOME="$root/.cargo-home"
test -s tests/debug_names_contract.rs
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
export CARGO_TARGET_DIR="$build"
cargo test --locked -p gimli --no-default-features --features read,std --test debug_names_contract debug_names_contract -- --nocapture | tee "$build/result"
grep -E 'test debug_names_contract \.\.\. ok' "$build/result" >/dev/null
for case in le_be dwarf64 hash_collision multi_entry units_parent malformed; do grep -Fx "PASS $case" "$build/result" >/dev/null; done
printf 'DEBUG_NAMES_CONTRACT_OK\n'
