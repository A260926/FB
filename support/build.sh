#!/bin/bash
set -euo pipefail
build=$1
mkdir -p "$build"
src=(lib/dgif_lib.c lib/egif_lib.c lib/gifalloc.c lib/gif_hash.c lib/gif_err.c lib/openbsd-reallocarray.c)
if [[ ${2:-baseline} == target ]]; then [[ ! -f lib/gif_plan.c ]] || src+=(lib/gif_plan.c); src+=(support/target_main.c tests/plan_contract.c); else src+=(support/baseline.c); fi
cc -O2 -std=c99 -Ilib -Isupport "${src[@]}" -o "$build/run"
