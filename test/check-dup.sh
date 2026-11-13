#!/usr/bin/env bash
#
#  check-dup - open identity / dup / dup2 contract tests
#
#  Builds test/dup_contract.c against the original lib-fd.c wrappers
#  through the task_support link adapter (real libc I/O, no preload)
#  and exercises the three contract groups. Any failed assertion makes
#  the program exit non-zero; this script exits non-zero in that case.
#
#  Copyright © 2002—2015 Sam Hocevar <sam@hocevar.net>
#
#  This program is free software. It comes without any warranty, to
#  the extent permitted by applicable law. You can redistribute it
#  and/or modify it under the terms of the Do What the Fuck You Want
#  to Public License, Version 2, as published by the WTFPL Task Force.
#  See http://www.wtfpl.net/ for more details.

set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/zzuf-dup.XXXXXX")
trap 'rm -rf "$work"' EXIT

: "${CC:=cc}"

"$CC" -std=c99 -O0 -Wall -Wextra \
      -I"$root/task_support" -I"$root/src" -I"$root/src/common" \
      -I"$root/src/libzzuf" \
      "$root/test/dup_contract.c" \
      "$root/src/common/fd.c" "$root/src/common/fuzz.c" \
      "$root/src/common/random.c" "$root/src/common/ranges.c" \
      -lm -o "$work/dup_contract"

status=0

run_mode() {
    mode=$1
    marker=$2

    echo "== dup contract: $mode =="
    if out=$("$work/dup_contract" "$mode"); then
        echo "$out"
        case "$out" in
            *"$marker"*) ;;
            *)
                echo "missing marker $marker" >&2
                status=1
                ;;
        esac
    else
        rc=$?
        echo "dup_contract $mode failed with exit status $rc" >&2
        status=1
    fi
}

run_mode identity DUP_IDENTITY_OK
run_mode position DUP_POSITION_OK
run_mode lifetime DUP_LIFETIME_OK

if [ "$status" -eq 0 ]; then
    echo "check-dup: all tests passed"
fi

exit "$status"
