#!/usr/bin/env bash
set -euo pipefail
b=.userfb-build/bin/cppcheck
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
count() { xmllint --xpath "count(/results/errors/error[@id='ctuArrayIndex'])" "$1"; }
for name in constant_bad dynamic_bad dynamic_safe paired_safe forward_bad; do
    "$b" --platform=unix64 --check-level=exhaustive --max-ctu-depth=5 --xml "userfb_baseline/$name.c" > "$work/$name.stdout" 2> "$work/$name.xml"
    xmllint --noout "$work/$name.xml"
done
test "$(count "$work/constant_bad.xml")" = 1
grep -Fq 'buffer size is 16 and it is accessed at offset 16.' "$work/constant_bad.xml"
for name in dynamic_bad dynamic_safe paired_safe forward_bad; do
    test "$(count "$work/$name.xml")" = 0
done
printf '%s\n' 'BASELINE_PARAMETER_INDEX constant_bad=1 dynamic_bad=0 dynamic_safe=0 paired_safe=0 forward_bad=0'
for mode in constant dynamic; do
    "$b" --platform=unix64 --check-level=exhaustive --max-ctu-depth=5 --xml "userfb_baseline/multitu_$mode" > "$work/multi-$mode.stdout" 2> "$work/multi-$mode.xml"
    mkdir "$work/cache-$mode"
    "$b" --platform=unix64 --check-level=exhaustive --max-ctu-depth=5 --cppcheck-build-dir="$work/cache-$mode" --xml "userfb_baseline/multitu_$mode" > "$work/disk-$mode.stdout" 2> "$work/disk-$mode.xml"
    test -s "$work/cache-$mode/files.txt"
done
test "$(count "$work/multi-constant.xml")" = 1
test "$(count "$work/multi-dynamic.xml")" = 0
test "$(count "$work/disk-constant.xml")" = 1
test "$(count "$work/disk-dynamic.xml")" = 0
for part in sink relay entry; do
    grep -Fq "multitu_constant/$part.c" "$work/multi-constant.xml"
done
printf '%s\n' 'BASELINE_MULTI_TU memory_constant=1 memory_dynamic=0 xml_constant=1 xml_dynamic=0'
bash userfb_baseline/run-old-tests.sh
