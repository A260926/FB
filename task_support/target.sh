#!/bin/sh
set -eu
for f in adpcm-search-cache.c adpcm-search-cache.h tests/search_cache_test.c; do [ -s "$f" ] || { echo "missing $f" >&2; exit 2; }; done
bash task_support/baseline.sh
cc -std=c99 -O2 -Wall -Wextra -I. adpcm-lib.c adpcm-dns.c adpcm-search-cache.c tests/search_cache_test.c -lm -o .task-build/cache-test
.task-build/cache-test > .task-build/cache.log
cat .task-build/cache.log
grep -Eq '^ADPCM_SEARCH_CACHE_OK cases=[1-9][0-9]*$' .task-build/cache.log
