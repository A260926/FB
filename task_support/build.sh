#!/bin/bash
set -euo pipefail
mkdir -p .task-build
rm -rf .task-build/classes
mkdir -p .task-build/classes
find src/main/java -name '*.java' -type f | LC_ALL=C sort > .task-build/production.list
javac --release 8 -encoding UTF-8 -d .task-build/classes @.task-build/production.list
javac --release 8 -encoding UTF-8 -cp .task-build/classes -d .task-build/classes task_support/Baseline.java task_support/Check.java task_support/Driver.java
