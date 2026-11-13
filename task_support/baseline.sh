#!/bin/sh
set -eu
mkdir -p .task-build
extra=''
if [ -f adpcm-search-cache.c ]; then extra=adpcm-search-cache.c; fi
cc -std=c99 -O2 -I. adpcm-lib.c adpcm-dns.c $extra task_support/baseline.c -lm -o .task-build/baseline
.task-build/baseline
cc -std=c99 -O2 -I. adpcm-lib.c adpcm-dns.c $extra adpcm-xq.c -lm -o .task-build/adpcm-xq
