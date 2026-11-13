#!/bin/bash
set -euo pipefail
test -s contract/StableSelectionTest.java
bash task_support/build.sh
javac -encoding UTF-8 --release 11 -cp .build/classes -d .build/classes contract/StableSelectionTest.java
java -ea -cp .build/classes StableSelectionTest > .build/stable-target.log 2>&1
cat .build/stable-target.log
for name in STABLE_MATCHES_OK PARENT_FRONTIER_OK STABLE_REUSE_OK; do
 grep -qx "$name" .build/stable-target.log
done
bash task_support/baseline.sh
