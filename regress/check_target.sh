#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .build

FONT=resources/FiraGO-Regular.ttf

# Plain build: functional contract checks.
cc -std=c99 -I. regress/target_contract.c schrift.c regress/realalloc.c -lm -o .build/target_contract
.build/target_contract "$FONT"

# Instrumented build: the library's allocator calls are routed through the
# counters defined in target_contract.c, which lets the test prove memory
# bounds and allocation-failure atomicity without looking at process RSS.
# realalloc.c must be built without the remapping macros.
cc -std=c99 -I. -c regress/realalloc.c -o .build/realalloc.o
cc -std=c99 -DSFT_TEST_TRACK_ALLOCATIONS \
   -Dmalloc=test_malloc -Dcalloc=test_calloc -Drealloc=test_realloc -Dfree=test_free \
   -I. regress/target_contract.c schrift.c .build/realalloc.o -lm \
   -o .build/target_contract_mem
.build/target_contract_mem "$FONT"
