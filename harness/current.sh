#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -r "$build"' EXIT
export CARGO_HOME="$PWD/harness/.cargo"
export CARGO_TARGET_DIR="$build/target"
cargo build --quiet --locked --manifest-path harness/Cargo.toml --bin current
"$build/target/debug/current" "$@"
