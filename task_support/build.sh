#!/bin/bash
set -euo pipefail
mkdir -p .task-build
cflags=(-std=c11 -O1 -g -ffunction-sections -fdata-sections -include task_support/platform.h -Ipjlib/include -Ipjmedia/include -I.)
case $(uname -s) in
 Darwin) ldflags=(-Wl,-dead_strip) ;;
 Linux) ldflags=(-Wl,--gc-sections) ;;
 *) echo 'POSIX C toolchain required' >&2; exit 1 ;;
esac
objects=()
for source in pjmedia/src/pjmedia/*.c pjlib/src/pj/*.c; do
 obj=.task-build/$(echo "$source" | tr / _).o
 "${CC:-cc}" "${cflags[@]}" -c "$source" -o "$obj"
 objects+=("$obj")
done
"${CC:-cc}" "${cflags[@]}" task_support/baseline.c "${objects[@]}" "${ldflags[@]}" -o .task-build/baseline
if [[ ${1:-baseline} == target ]]; then
 "${CC:-cc}" "${cflags[@]}" task_support/driver.c tests/splice.c "${objects[@]}" "${ldflags[@]}" -o .task-build/target
fi
