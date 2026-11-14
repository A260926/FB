#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build/classes .task-build/fixtures
find Procyon.Core/src/main/java Procyon.CompilerTools/src/main/java -name '*.java' -print | LC_ALL=C sort > .task-build/sources.list
javac --release 8 -Xlint:-options -encoding UTF-8 -d .task-build/classes @.task-build/sources.list
javac --release 17 -proc:none -encoding UTF-8 -d .task-build/fixtures task_support/Fixture.java
javac --release 17 -proc:none -encoding UTF-8 -cp .task-build/classes -d .task-build/classes task_support/PrepareOld.java
java -cp .task-build/classes PrepareOld
javac --release 17 -proc:none -encoding UTF-8 -cp .task-build/classes -d .task-build/classes task_support/OldAnnotationHarness.java .task-build/old-src/com/strobel/decompiler/AnnotationTests.java
case "${1:-}" in
  baseline)
    javac --release 17 -proc:none -encoding UTF-8 -cp .task-build/classes -d .task-build/classes task_support/Baseline.java
    java -cp .task-build/classes:.task-build/fixtures Baseline
    java -cp .task-build/classes com.strobel.decompiler.OldAnnotationHarness
    ;;
  target)
    test -s task_tests/TaskTypeAnnotations.java || { echo 'missing or empty task_tests/TaskTypeAnnotations.java' >&2; exit 1; }
    # A clean directory prevents an old target class from making a new empty file pass.
    rm -rf .task-build/target-classes
    mkdir -p .task-build/target-classes
    find task_tests -name '*.java' -print | LC_ALL=C sort > .task-build/target.list
    javac --release 17 -proc:none -encoding UTF-8 -cp .task-build/classes -d .task-build/target-classes task_support/TaskCheck.java @.task-build/target.list
    set +e
    java -cp .task-build/target-classes:.task-build/classes TaskCheck > .task-build/target.stdout 2> .task-build/target.stderr
    status=$?
    set -e
    cat .task-build/target.stdout
    cat .task-build/target.stderr >&2
    test "$status" -eq 0 || exit "$status"
    for group in Metadata Paths Rewrite; do
      grep -Eq "^TASK_GROUP_DONE $group cases=[1-9][0-9]* checks=[1-9][0-9]*$" .task-build/target.stdout || { echo "incomplete group $group" >&2; exit 1; }
    done
    grep -Eq '^TASK_TARGET_DONE cases=[1-9][0-9]* checks=[1-9][0-9]*$' .task-build/target.stdout || { echo 'incomplete target' >&2; exit 1; }
    java -cp .task-build/classes com.strobel.decompiler.OldAnnotationHarness
    echo TYPE_ANNOTATIONS_TARGET_OK
    ;;
  *) echo 'usage: bash task_support/run.sh baseline|target' >&2; exit 2;;
esac
