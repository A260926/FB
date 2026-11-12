#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/cavalier-baseline.XXXXXX")
trap 'rm -rf "$b"' EXIT
export CARGO_HOME="$b/cargo-home"
export CARGO_TARGET_DIR="$b/target"
cargo run --frozen --quiet -p cavalier_contours --example baseline
cargo test --frozen -p cavalier_contours --test test_shape_parallel_offset -- --show-output --test-threads=1 | tee "$b/old"
bash checks/check-test-results.sh "$b/old" 1
echo CAVALIER_OLD_SHAPE_OK
