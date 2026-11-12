#!/bin/sh
set -eu
if ! test -s tests/row_scheduler.c; then
    echo 'missing or empty tests/row_scheduler.c' >&2
    exit 1
fi
sh checks/build.sh
sh checks/baseline.sh
${CC:-cc} -std=c99 -O2 -Wall -Wextra -I. -Ichecks tests/row_scheduler.c checks/contract-main.c -o .build/contracts
.build/contracts > .build/contracts.log
cat .build/contracts.log
for name in flow delayed-voice row-delay export-boundary; do
    test "$(grep -c "^CASE $name PASS$" .build/contracts.log)" = 1
done
test "$(grep -c '^POCKETMOD_ROW_SCHEDULER_OK cases=4$' .build/contracts.log)" = 1
