#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -s cavalier_contours/tests/offset_reference.rs || { echo 'missing offset_reference.rs' >&2; exit 1; }
test -s checks/offset-mutants.sh || { echo 'missing checks/offset-mutants.sh' >&2; exit 1; }
b=$(mktemp -d "${TMPDIR:-/tmp}/cavalier-reference.XXXXXX")
trap 'rm -rf "$b"' EXIT
export CARGO_HOME="$b/cargo-home"
export CARGO_TARGET_DIR="$b/target"
cargo test --frozen -p cavalier_contours --test offset_reference -- --show-output --test-threads=1 | tee "$b/out"
bash checks/check-test-results.sh "$b/out" 3 offset_metric_reference offset_topology_reference offset_transform_reference
bash checks/offset-mutants.sh | tee "$b/mutants"
for name in arc_metric hole_direction slice_filter; do
  grep -Fx "MUTANT_REJECTED $name assertion" "$b/mutants"
done
bash checks/run-baseline.sh
