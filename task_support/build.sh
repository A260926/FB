#!/bin/bash
set -euo pipefail
mkdir -p .build/classes
find src/main/java task_support/vendor -name '*.java' -print | LC_ALL=C sort > .build/sources.list
javac -encoding UTF-8 --release 11 -d .build/classes @.build/sources.list
if test -d src/main/resources; then cp -R src/main/resources/. .build/classes/; fi
printf '%s\n' JSOUP_OFFLINE_COMPILED
