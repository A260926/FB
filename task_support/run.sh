#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .task-build
export LC_ALL=C
# Additional library C translation units may be listed here, one relative path per line.
set -- "$@"
mode=${1:-baseline}
compile_library() {
    while IFS= read -r src; do
        case "$src" in micromod-c/*.c) ;; *) echo "invalid library source: $src" >&2; exit 2;; esac
        test -s "$src"
    done < task_support/sources.list
}
compile_library
# sources.list is deliberately an explicit library-only list, avoiding SDL and extra mains.
if [ "$mode" = baseline ]; then
    cc -std=c99 -O2 -Wall -Wextra -Imicromod-c -Itask_support $(cat task_support/sources.list) task_support/baseline.c -o .task-build/baseline
    .task-build/baseline .task-build/plain.mod
    cc -std=c99 -O2 -Imicromod-c $(cat task_support/sources.list) micromod-c/mod2wav.c -o .task-build/mod2wav
    .task-build/mod2wav .task-build/plain.mod .task-build/plain.wav -rate 8287 > .task-build/converter.stdout
    test "$(wc -c < .task-build/plain.wav | tr -d ' ')" = 42412
    echo 'LEGACY_CONVERTER_OK wav_bytes=42412'
elif [ "$mode" = target ]; then
    test -s micromod-c/tests/sample_contract.c || { echo 'missing target micromod-c/tests/sample_contract.c' >&2; exit 2; }
    cc -std=c99 -O2 -Wall -Wextra -Imicromod-c -Itask_support $(cat task_support/sources.list) task_support/driver.c micromod-c/tests/sample_contract.c -o .task-build/target
    # Preserve the real exit status and require completion of every group outside the process.
    .task-build/target > .task-build/target.stdout
    cat .task-build/target.stdout
    for group in dma funk instances compatibility; do
        awk -v group="$group" '$1=="GROUP" && $2==group && $3=="CHECKS" && $4 ~ /^[0-9]+$/ && $4+0>0 && $5=="COMPLETE" && NF==5 {n++} END {exit(n!=1)}' .task-build/target.stdout
    done
    test "$(grep -cx 'MICROMOD_TARGET_COMPLETE groups=4' .task-build/target.stdout)" = 1
    sh task_support/run.sh baseline
else
    echo 'usage: sh task_support/run.sh baseline|target' >&2; exit 2
fi
