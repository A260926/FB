#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/quarantine_transfer_contract.c; fi
task_work=$(mktemp -d "${TMPDIR:-/tmp}/isoalloc-run.XXXXXX")
printf 'TASK_BUILD_DIR=%s\n' "$task_work"
case $(uname -s) in Darwin) task_lib="$task_work/libisoalloc.dylib" ;; *) task_lib="$task_work/libisoalloc.so" ;; esac
task_sources=(src/iso_alloc.c)
for task_src in src/*.c; do
  case "$task_src" in src/malloc_hook.c|src/iso_alloc.c) ;; *) task_sources+=("$task_src") ;; esac
done
clang -Wall -Iinclude/ -DTHREAD_SUPPORT=1 -pthread -DPRE_POPULATE_PAGES=0 -DSMALL_MEM_STARTUP=0 -DDISABLE_CANARY=0 -Wno-pointer-arith -Wno-gnu-zero-variadic-macro-arguments -Wno-format-pedantic -Werror -pedantic -DMALLOC_HOOK=0  -fvisibility=hidden -std=c11     -DALLOC_SANITY=0 -DMEMCPY_SANITY=0 -DUNINIT_READ_SANITY=0 -DCPU_PIN=0  -DEXPERIMENTAL=0 -DUAF_PTR_PAGE=0 -DVERIFY_FREE_BIT_SLOTS=0 -DNAMED_MAPPINGS=0 -DABORT_ON_NULL=0 -DABORT_ON_UNOWNED_PTR=1 -DNO_ZERO_ALLOCATIONS=1 -DABORT_NO_ENTROPY=1 -DISO_DTOR_CLEANUP=0 -DRANDOMIZE_FREELIST=1 -DUSE_SPINLOCK=0 -DHUGE_PAGES=1 -DTHP_PAGES=1 -DUSE_MLOCK=1 -DMEMORY_TAGGING=0 -DSTRONG_SIZE_ISOLATION=0 -DMEMSET_SANITY=0 -DAUTO_CTOR_DTOR=0 -DSIGNAL_HANDLER=0 -DBIG_ZONE_META_DATA_GUARD=0 -DBIG_ZONE_GUARD=0  -DMASK_PTRS=1 -DSANITIZE_CHUNKS=0 -DFUZZ_MODE=0 -DPERM_FREE_REALLOC=0  -DDONT_USE_NEON=0 -fPIC -shared   -g -ggdb3 -fno-omit-frame-pointer -DLEAK_DETECTOR=1 -DDEBUG=1 "${task_sources[@]}" -o "$task_lib"
clang -std=gnu11 -pthread -DLEAK_DETECTOR=1 -Iinclude task_support/baseline.c "$task_lib" -Wl,-rpath,"$task_work" -o "$task_work/baseline"
if [[ $mode == target ]]; then
  test -s include/iso_alloc.h
test -s examples/quarantine_transfer.c
clang -std=gnu11 -pthread -Werror=implicit-function-declaration -Iinclude task_support/api_consumer.c "$task_lib" -Wl,-rpath,"$task_work" -o "$task_work/api-consumer"
"$task_work/api-consumer"
clang -std=gnu11 -pthread -Iinclude examples/quarantine_transfer.c "$task_lib" -Wl,-rpath,"$task_work" -o "$task_work/example"
"$task_work/example" > "$task_work/example.log"
cat "$task_work/example.log"
grep -qx QUARANTINE_TRANSFER_EXAMPLE_OK "$task_work/example.log"
clang -std=gnu11 -pthread -I. -Iinclude task_support/driver.c tests/quarantine_transfer_contract.c "$task_lib" -Wl,-rpath,"$task_work" -o "$task_work/target"
  output_log="$task_work/target.log"
  set +e
  "$task_work/target" >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in budget handoff lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
"$task_work/baseline"
