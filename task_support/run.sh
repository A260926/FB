#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in baseline|target|emit) ;; *) exit 2;; esac
work=$(mktemp -d "${TMPDIR:-/tmp}/check-rust.XXXXXX")
trap 'rm -rf "$work"' EXIT
# Fixed sources only: no Cargo, package cache, network or build scripts.
rustc --edition=2021 --crate-name memchr --crate-type rlib --cfg 'feature="std"' --cfg 'feature="alloc"' vendor/memchr/src/lib.rs -o "$work/libmemchr.rlib"
rustc --edition=2024 --crate-name object --crate-type rlib --cfg 'feature="std"' --cfg 'feature="read_core"' --cfg 'feature="macho"' --cfg 'feature="unaligned"' src/lib.rs --extern memchr="$work/libmemchr.rlib" -L dependency="$work" -o "$work/libobject.rlib"
rustc --edition=2024 task_support/baseline.rs --extern object="$work/libobject.rlib" -L dependency="$work" -o "$work/baseline"
if [[ "$mode" == emit ]];then "$work/baseline" --emit;exit;fi
if [[ "$mode" == baseline ]];then "$work/baseline";exit;fi
"$work/baseline" --normal
test -s tests/scattered_layout.rs
rustc --edition=2024 --test tests/scattered_layout.rs --extern object="$work/libobject.rlib" -L dependency="$work" -o "$work/regression"
"$work/regression" --list > "$work/list"
for group in scattered_records layout_values compatibility;do
    grep -Eq "^${group}: test$" "$work/list"
    "$work/regression" --exact "$group" --nocapture > "$work/$group.log" 2>&1 || { task_code=$?;cat "$work/$group.log";exit "$task_code"; }
    cat "$work/$group.log"
    grep -Eq 'test result: ok\. 1 passed; 0 failed; 0 ignored;' "$work/$group.log"
    grep -Eq "^TASK_GROUP_OK ${group} checks=[1-9][0-9]*$" "$work/$group.log"
done
test -s examples/scattered_layout.rs
rustc --edition=2024 examples/scattered_layout.rs --extern object="$work/libobject.rlib" -L dependency="$work" -o "$work/example"
"$work/example" task_support/basic.macho > "$work/example.log"
test -s "$work/example.log"
cat "$work/example.log"
