#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
task_cache=$(mktemp -d)
trap 'rm -rf "$task_cache"' EXIT
export CARGO_HOME="$task_cache/cargo" CARGO_TARGET_DIR="$task_cache/target" CARGO_NET_OFFLINE=true
mkdir -p "$CARGO_HOME" "$CARGO_TARGET_DIR"
if [[ $mode == target ]]; then
 test -s tests/symbol_indexes.rs
 output_log="$task_cache/tests.log"
 set +e
 cargo test --offline --locked --test symbol_indexes -- --nocapture --test-threads=1 > "$output_log" 2>&1
 run_rc=$?
 set -e
 cat "$output_log"
 [[ $run_rc == 0 ]] || exit "$run_rc"
 for group in buckets names public_addresses compatibility; do
  grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
 done
 grep -Eq 'test result: ok\. [1-9][0-9]* passed; 0 failed; 0 ignored;' "$output_log"
 test -s examples/index_lookup.rs
 cargo run --offline --locked --example index_lookup
fi
cargo run --offline --locked --example baseline
