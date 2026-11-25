#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
# Portability fallback: some macOS shells expose only the /usr/bin JDK stub.
if ! javac -version >/dev/null 2>&1; then
 for jdk in /opt/homebrew/opt/openjdk@17 /opt/homebrew/opt/openjdk@11 \
            /opt/homebrew/opt/openjdk /usr/local/opt/openjdk@17 \
            /usr/local/opt/openjdk; do
  if [ -x "$jdk/bin/javac" ]; then export PATH="$jdk/bin:$PATH"; break; fi
 done
fi
mode=${1:-baseline}
if [[ $mode == target ]]; then
 test -s tests/bounds/BoundsContract.java
 for production in BoundsChecker BoundsFinding; do
  test -s "src/cz/muni/stanse/boundschecker/$production.java"
 done
fi
mkdir -p .task-build
# Every invocation gets new class/output paths. No reused incremental classes.
build_dir=$(mktemp -d "$PWD/.task-build/run.XXXXXX")
mkdir "$build_dir/classes"
classpath="vendor/antlr-3.5.3-complete.jar:dist/lib/*"
find src vendor/tableformatter/src -name '*.java' > "$build_dir/production.list"
test -s "$build_dir/production.list"
javac --release 8 -cp "$classpath" -d "$build_dir/classes" @"$build_dir/production.list" > "$build_dir/compile.log" 2>&1 || { tail -30 "$build_dir/compile.log"; exit 1; }
# The fixed source consumer binds both new public classes and factory registration.
if [[ $mode == target ]]; then
 javac --release 8 -cp "$build_dir/classes:$classpath" -d "$build_dir/classes" task_support/Inputs.java task_support/BoundsApiProbe.java task_support/ContractSuite.java tests/bounds/BoundsContract.java
 STANSE_HOME="$PWD/dist" java -ea -Dtask.build="$build_dir" -Dlog4j.configuration="file:$PWD/task_support/log4j.properties" -cp "$build_dir/classes:$classpath" BoundsApiProbe
 set +e
 STANSE_HOME="$PWD/dist" java -ea -Dtask.build="$build_dir" -Dlog4j.configuration="file:$PWD/task_support/log4j.properties" -cp "$build_dir/classes:$classpath" task_support.ContractSuite > "$build_dir/target.log" 2>&1
 status=$?
 set -e
 cat "$build_dir/target.log"
 [[ $status == 0 ]] || exit "$status"
 for group in correlated loops witness; do
  grep -Eq "^GROUP $group cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$build_dir/target.log"
 done
 grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$build_dir/target.log"
fi
javac --release 8 -cp "$build_dir/classes:$classpath" -d "$build_dir/classes" task_support/Inputs.java task_support/Baseline.java
STANSE_HOME="$PWD/dist" java -ea -Dtask.build="$build_dir" -Dlog4j.configuration="file:$PWD/task_support/log4j.properties" -cp "$build_dir/classes:$classpath" Baseline
