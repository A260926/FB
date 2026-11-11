#!/bin/sh
set -eu
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT HUP INT TERM
CARGO_HOME="$PWD/.userfb-cargo" CARGO_TARGET_DIR="$b" cargo run --manifest-path userfb/Cargo.toml --locked --quiet
