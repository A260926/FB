#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -r "$build"' EXIT
export CARGO_HOME="$PWD/harness/.cargo"
export CARGO_TARGET_DIR="$build/target"
cargo test --locked --manifest-path harness/Cargo.toml --test sampling_pdf -- --nocapture | tee "$build/test.log"
grep -Fq 'SAMPLING_PDF_OK' "$build/test.log"
grep -Eq 'test result: ok\. [1-9][0-9]* passed; 0 failed' "$build/test.log"
