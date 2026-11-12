#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
CARGO_HOME="$build/cargo" CARGO_NET_OFFLINE=true CARGO_TARGET_DIR="$build" cargo run --locked --manifest-path baseline/harness/Cargo.toml
