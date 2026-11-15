#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in baseline|target) ;; *) exit 2;; esac
mkdir -p .task-build
classes=$(mktemp -d .task-build/classes.XXXXXX)
output_log=$(mktemp .task-build/output.XXXXXX)
trap 'rm -rf "$classes"; rm -f "$output_log"' EXIT HUP INT TERM
find jme3-core/src/main/java jme3-core/src/plugins/java jme3-core/src/tools/java -name '*.java' -type f | LC_ALL=C sort > "$classes/sources"
test -s "$classes/sources"
printf '%s\n' task_support/Baseline.java >> "$classes/sources"
if [ "$mode" = target ]; then
  test -s jme3-core/src/main/java/com/jme3/scene/control/PostLogicalControl.java
  test -s task_tests/SurfaceAttachmentTests.java
  test -s task_examples/SurfaceAttachmentExample.java
  find task_tests task_examples -name '*.java' -type f | LC_ALL=C sort >> "$classes/sources"
  printf '%s\n' task_support/TaskChecks.java task_support/TargetMain.java >> "$classes/sources"
fi
javac --release 17 -d "$classes" @"$classes/sources"
if [ "$mode" = target ]; then
  java -Djava.awt.headless=true -cp "$classes" SurfaceAttachmentExample
  set +e
  java -Djava.awt.headless=true -cp "$classes" TargetMain > "$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [ "$status" -eq 0 ] || exit "$status"
  for group in deformation frames lifecycle; do
    grep -Eq "^GROUP $group checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -qx 'TARGET_COMPLETE groups=3' "$output_log"
fi
java -Djava.awt.headless=true -cp "$classes" Baseline
