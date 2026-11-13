#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
build=.build
mkdir -p "$build"
cc=${CC:-cc}
cxx=${CXX:-c++}
if [[ "$mode" == target ]]; then
 test -s tests/reverse_contract.cpp || { echo 'missing or empty tests/reverse_contract.cpp' >&2;exit 2; }
 "$cc" -std=c99 -O2 -I. -c support/plm.c -o "$build/plm.o"
 "$cxx" -std=c++17 -O2 -I. tests/reverse_contract.cpp support/driver.cpp "$build/plm.o" -lm -o "$build/target"
 status=0
 "$build/target" > "$build/target.stdout" 2> "$build/target.stderr" || status=$?
 printf '%s\n' "$status" > "$build/target.exit"
 cat "$build/target.stdout"
 cat "$build/target.stderr" >&2
 if [[ "$status" != 0 ]]; then exit "$status"; fi
 awk -f support/protocol.awk "$build/target.stdout"
elif [[ "$mode" != baseline ]]; then
 echo 'usage: run.sh baseline|target' >&2;exit 2
fi
"$cc" -std=c99 -O2 support/baseline.c -lm -o "$build/baseline"
"$build/baseline" support/media/open.m1v 38
"$build/baseline" support/media/closed.m1v 36
"$build/baseline" support/media/ip.m1v 38
printf 'PLM_FORWARD_BASELINE_OK clips=3\n'
