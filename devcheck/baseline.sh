#!/bin/sh
set -eu
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT HUP INT TERM
CARGO_HOME="$PWD/.userfb-cargo" CARGO_TARGET_DIR="$b/build" cargo run --manifest-path devcheck/Cargo.toml --locked --bin baseline --quiet -- "$b/texture.png"
