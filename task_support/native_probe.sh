#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p '.task-build/native probe'
printf 'LOG END\n' >'.task-build/native probe/normal log.log'
cat >'.task-build/native probe/manifest.txt' <<'DATA'
CASE normal 1 2
RUN normal 1 0 worker 12 COMPLETE
LOG normal log.log
ENDRUN
RUN normal 1 1 worker 12 COMPLETE
LOG normal log.log
ENDRUN
DATA
rm -f '.task-build/native probe/report.txt'
perl drmemory/postprocess.pl -regression_manifest '.task-build/native probe/manifest.txt' -regression_engine '.task-build/regression-engine' -regression_report '.task-build/native probe/report.txt'
printf 'CASE normal 1 CLEAN\n' >'.task-build/native probe/expected.txt'
cmp '.task-build/native probe/expected.txt' '.task-build/native probe/report.txt'
# A real native error block and shared stack must pass through original parse_error.
cat >'.task-build/native probe/stack log.log' <<'DATA'
CALLSTACK 7
# 0 benign!? (0x1004 <benign+0x4>)
# 1 benign!? (0x2000 <benign+0x1000>)
  error end
DATA
cat >'.task-build/native probe/error log.log' <<'DATA'
Error #1: UNADDRESSABLE ACCESS: reading ordinary simulated metadata
  info: @0:00:00.001 in thread 1
  callstack=7
  error end
LOG END
DATA
cat >'.task-build/native probe/manifest-error.txt' <<'DATA'
CASE mapped 1 1
IMAGE worker token 1 artifact 4096 4096 10 20
RUN mapped 1 0 worker 12 COMPLETE
STACK shared stack log.log
LOG error log.log
ENDRUN
DATA
rm -f '.task-build/native probe/mapped.txt'
perl drmemory/postprocess.pl -regression_manifest '.task-build/native probe/manifest-error.txt' -regression_engine '.task-build/regression-engine' -regression_report '.task-build/native probe/mapped.txt'
printf 'CASE mapped 1 STABLE\nGROUP UNADDR|artifact@4,artifact@fff mapped\n' >'.task-build/native probe/expected-mapped.txt'
cmp '.task-build/native probe/expected-mapped.txt' '.task-build/native probe/mapped.txt'
printf '%s\n' 'NATIVE_PROBE empty-and-shared-parser COMPLETE'
