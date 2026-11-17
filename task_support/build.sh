#!/bin/bash
set -euo pipefail
mkdir -p .task-build/classes .task-build/analyzer .task-build/fixtures
find src/main/java -name '*.java' | sort > .task-build/production.list
input_hash=$({ while IFS= read -r file; do shasum -a 256 "$file"; done < .task-build/production.list; } | shasum -a 256)
if [[ ! -f .task-build/production.hash ]] || [[ $(cat .task-build/production.hash) != "$input_hash" ]]; then
  rm -rf .task-build/classes
  mkdir -p .task-build/classes
  javac --release 8 -encoding UTF-8 -d .task-build/classes @.task-build/production.list
  echo "$input_hash" > .task-build/production.hash
fi
find task_support/java -name '*.java' | sort > .task-build/support.list
javac --release 17 -proc:none -encoding UTF-8 -cp .task-build/classes -d .task-build/analyzer src/test/java/org/jd/core/v1/printer/PlainTextPrinter.java @.task-build/support.list
javac --release 17 -proc:none -g:none -encoding UTF-8 -d .task-build/fixtures task_support/fixtures/*.java
