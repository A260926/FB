#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
export CARGO_HOME="$root/.cargo-home"
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
export CARGO_TARGET_DIR="$build"
cargo run --locked -p gimli --no-default-features --features read,std --example baseline
cargo test --locked -p gimli --no-default-features --features read,std --lib read::index::tests -- --nocapture | tee "$build/old"
grep -E 'test result: ok\. [1-9][0-9]* passed' "$build/old" >/dev/null
printf 'GIMLI_OLD_INDEX_OK\n'
