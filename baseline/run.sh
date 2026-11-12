#!/usr/bin/env bash
#
# Starting point for the line number contract: build the harness offline and
# run only the ordinary single-slot scenario (two sequences, six rows, one
# resume per sequence). The VLIW op_index coverage lives in target.sh.
set -euo pipefail

BASELINE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$BASELINE_DIR/.." && pwd)"
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/line-contract-baseline.XXXXXX")"
trap 'rm -rf "$WORK_DIR"' EXIT

# Layout the offline path-dependency build in a throwaway directory.
cp -R "$REPO_ROOT" "$WORK_DIR/gimli"
rm -rf "$WORK_DIR/gimli/target" "$WORK_DIR/gimli/baseline/harness/target"
mkdir -p "$WORK_DIR/harness"
cp -R "$BASELINE_DIR/harness/." "$WORK_DIR/harness/"
rm -rf "$WORK_DIR/harness/target"

export CARGO_HOME="$WORK_DIR/cargo-home"
mkdir -p "$CARGO_HOME"

cargo test --offline --manifest-path "$WORK_DIR/harness/Cargo.toml" --test baseline_start
