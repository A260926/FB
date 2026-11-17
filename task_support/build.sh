#!/bin/bash
set -euo pipefail
unset CLASSPATH JAVA_TOOL_OPTIONS JDK_JAVA_OPTIONS
rm -rf .task-build/classes .task-build/fixture
mkdir -p .task-build/classes .task-build/fixture
find src -name '*.java' -type f | sort > .task-build/sources.list
javac --release 21 -proc:none -encoding UTF-8 -d .task-build/classes @.task-build/sources.list
while IFS= read -r path; do
  target=".task-build/classes/${path#src/}"
  mkdir -p "$(dirname "$target")"
  cp "$path" "$target"
done < <(find src -type f -name '*.properties')
cp build/productinfo.properties .task-build/classes/org/openjdk/asmtools/util/productinfo.properties
javac -proc:none -g --release 17 -d .task-build/fixture task_support/fixtures/*.java
javac -proc:none -cp .task-build/classes -d .task-build/classes task_support/Baseline.java
if [[ ${1:-baseline} == target ]] && [[ -d task_tests ]]; then
  find task_tests -name '*.java' -type f | sort > .task-build/test-sources.list
  if [[ -s .task-build/test-sources.list ]]; then
    javac -proc:none -cp .task-build/classes -d .task-build/classes @.task-build/test-sources.list
  fi
fi
