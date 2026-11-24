#!/bin/bash
set -euo pipefail
unit=$1
test -s "$unit"
name=${unit//\//_}
if [[ $unit == *.c ]]; then
 cc -O0 -DNO_OPENSSL=1 -DSOCKLEN_T=socklen_t -I. -IliveMedia/include -Igroupsock/include -IUsageEnvironment/include -IBasicUsageEnvironment/include -c "$unit" -o ".task-build/unit-$name.o"
else
 c++ -std=c++20 -O0 -pthread -DNO_OPENSSL=1 -DSOCKLEN_T=socklen_t -I. -IliveMedia/include -Igroupsock/include -IUsageEnvironment/include -IBasicUsageEnvironment/include -c "$unit" -o ".task-build/unit-$name.o"
fi
