#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
export CARGO_TARGET_DIR="$b/target"
export CARGO_HOME="$PWD/.cargo-cache"
cargo +1.88.0 test --locked --lib nal::sps::test::test_it -- --exact --nocapture 2>&1 | tee "$b/sps.log"
grep -Fq 'test nal::sps::test::test_it ... ok' "$b/sps.log"
grep -Fq '1 passed; 0 failed' "$b/sps.log"
cargo +1.88.0 test --locked --lib nal::pps::test::test_it -- --exact --nocapture 2>&1 | tee "$b/pps.log"
grep -Fq 'test nal::pps::test::test_it ... ok' "$b/pps.log"
grep -Fq '1 passed; 0 failed' "$b/pps.log"
echo BASELINE_H264_SPS_PPS_PARSED_PASS
