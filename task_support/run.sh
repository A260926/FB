#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in baseline|target);; *) exit 64;; esac
task_cache=$(mktemp -d)
trap 'rm -rf "$task_cache"' EXIT
export CARGO_HOME="$task_cache/cargo" CARGO_TARGET_DIR="$task_cache/target" CARGO_NET_OFFLINE=true
mkdir -p "$CARGO_HOME" "$CARGO_TARGET_DIR"
if [[ $mode == target ]]; then
 test -s tests/procedure_types.rs
 output_log="$task_cache/tests.log"
 set +e
 cargo test --offline --locked --test procedure_types -- --nocapture --test-threads=1 > "$output_log" 2>&1
 run_rc=$?
 set -e
 cat "$output_log"
 [[ $run_rc == 0 ]] || exit "$run_rc"
 for group in origin forward recursive compatibility; do
  grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
 done
 grep -Eq 'test result: ok\. 4 passed; 0 failed; 0 ignored;' "$output_log"
fi
cargo run --offline --locked --example baseline
cargo test --offline --locked --lib
