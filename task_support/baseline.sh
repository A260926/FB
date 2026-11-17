#!/bin/bash
set -euo pipefail
rm -rf .task-build/input .task-build/normal .task-build/shared .task-build/recompiled
mkdir -p .task-build/input .task-build/normal .task-build/shared .task-build/recompiled
javac -proc:none --release 8 -g:none -d .task-build/input task_support/fixtures/Ordinary.java task_support/fixtures/SharedEntry.java
java -cp .task-build/cfr org.benf.cfr.reader.Main .task-build/input/Ordinary.class --outputdir .task-build/normal > .task-build/normal-tool.log 2>&1
test -s .task-build/normal/Ordinary.java
javac -proc:none --release 8 -d .task-build/recompiled .task-build/normal/Ordinary.java
printf 'BASELINE_NORMAL_OK\n'
java -cp .task-build/cfr org.benf.cfr.reader.Main .task-build/input/SharedEntry.class --outputdir .task-build/shared > .task-build/shared-tool.log 2>&1
test -s .task-build/shared/SharedEntry.java
set +e
javac -proc:none --release 8 -d .task-build/recompiled .task-build/shared/SharedEntry.java > .task-build/shared-recompile.log 2>&1
compile_rc=$?
set -e
if grep -Eq '\*\* GOTO|Unable to fully structure code' .task-build/shared/SharedEntry.java; then
  test "$compile_rc" -ne 0
  printf 'SHARED_ENTRY_STATE=unstructured recompile_exit=%s\n' "$compile_rc"
else
  test "$compile_rc" -eq 0
  printf 'SHARED_ENTRY_STATE=structured recompile_exit=0\n'
fi

java -cp .task-build/cfr ApiDump .task-build/input/SharedEntry.class .task-build/shared/SharedEntry-api.java
test -s .task-build/shared/SharedEntry-api.java
if grep -Eq "\*\* GOTO|Unable to fully structure code" .task-build/shared/SharedEntry-api.java; then
  printf "API_SHARED_ENTRY_STATE=unstructured\n"
else
  printf "API_SHARED_ENTRY_STATE=structured\n"
fi
