#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode="${1:-baseline}"
case "$mode" in baseline|target) ;; *) echo 'usage: run.sh baseline|target' >&2; exit 2;; esac
# All caches may be empty. The fixed toolchain must be installed beforehand.
export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-$PWD/.task-build}"
export CARGO_HOME="${CARGO_HOME:-$PWD/.task-cache}"
mkdir -p "$CARGO_HOME" "$CARGO_TARGET_DIR"
log="$CARGO_TARGET_DIR/task-$mode.log"
if [[ "$mode" == baseline ]]; then
    cargo +1.94.0 test --manifest-path Cargo.toml -p fidget --no-default-features --locked --offline --lib mesh:: -- --nocapture >"$log" 2>&1 || { tail -30 "$log"; exit 1; }
    cat "$log"
    grep -q '^CPU_BASELINE_OK ' "$log"
    grep -q '^PARENT_TABLE_OK mask=0x81 pieces=2 ' "$log"
    grep -Eq '^test result: ok\. [1-9][0-9]* passed; 0 failed; 0 ignored;' "$log"
    echo 'BASELINE_OK cpu=true default_features=false'
else
    test -s task_support/multipatch_tests.rs || { echo 'missing nonempty multipatch_tests.rs' >&2; exit 3; }
    cargo +1.94.0 test --manifest-path Cargo.toml -p fidget --no-default-features --features task-target --locked --offline --lib 'mesh::octree::task_target::registered_multipatch_suite' -- --exact --nocapture >"$log" 2>&1 || { tail -30 "$log"; exit 1; }
    cat "$log"
    for group in topology qef seams compat; do
        grep -Eq "^TASK_CASE_OK group=$group name=[^ ]+ checks=[1-9][0-9]*$" "$log"
    done
    grep -Eq '^TASK_TARGET_OK cases=[1-9][0-9]* checks=[1-9][0-9]* groups=4$' "$log"
    grep -q '^test result: ok. 1 passed; 0 failed; 0 ignored;' "$log"
    bash task_support/run.sh baseline
fi
