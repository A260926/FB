#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -s tests/push_decoder_test.c || { echo 'missing tests/push_decoder_test.c' >&2; exit 2; }
bash task_support/build.sh
cc -std=c99 -Wall -Wextra -Iinclude -Itask_support task_support/driver.c tests/push_decoder_test.c .push-build/libwavpack.a -lm -o .push-build/push-target
set +e
.push-build/push-target > .push-build/push-target.stdout 2>&1
result=$?
set -e
cat .push-build/push-target.stdout
test "$result" -eq 0 || exit "$result"
awk '
BEGIN {n=split("fragment correction flow terminal",g," "); stage=0; done=0}
$1=="WV_PUSH_BEGIN" {if(stage%2 || stage>=8 || $2!=g[stage/2+1] || NF!=2) exit 1;stage++;next}
$1=="WV_PUSH_PASS" {if(stage%2!=1 || $2!=g[(stage+1)/2] || NF!=3 || $3 !~ /^checks=[1-9][0-9]*$/) exit 1;stage++;next}
$1=="WV_PUSH_COMPLETE" {if(stage!=8 || NF!=2 || $2!="groups=4" || done) exit 1;done=1;next}
END {if(stage!=8 || done!=1) exit 1}
' .push-build/push-target.stdout
bash task_support/baseline.sh
bash task_support/old-tests.sh
