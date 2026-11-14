#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode="${1:-baseline}"
case "$mode" in baseline|target) ;; *) exit 2;; esac
task_work="$(mktemp -d "${TMPDIR:-/tmp}/task.XXXXXX")"
trap 'rm -rf "$task_work"' EXIT
export CARGO_HOME="$task_work/cargo"
export CARGO_TARGET_DIR="$task_work/target"
export CARGO_NET_OFFLINE=true
mkdir -p "$CARGO_HOME"
if [[ "$mode" == baseline ]]; then
    cargo run --offline --locked --example baseline
    exit
fi
test -s tests/symbol_sections.rs
test -s examples/symbol_sections.rs
for task_case in attachments identities relocations compatibility; do
    if cargo test --offline --locked --test symbol_sections "$task_case" -- --exact --nocapture --test-threads=1 > "$task_work/test.log" 2>&1; then :
    else task_status=$?; cat "$task_work/test.log"; exit "$task_status"; fi
    cat "$task_work/test.log"
    grep -Eq "TASK_CASE ${task_case} assertions=[1-9][0-9]*" "$task_work/test.log"
    grep -q 'test result: ok. 1 passed; 0 failed; 0 ignored;' "$task_work/test.log"
done
cargo run --offline --locked --example symbol_sections
cargo run --offline --locked --example baseline -- --normal
printf '%s\n' SYMBOL_SECTIONS_OK
