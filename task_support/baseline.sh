#!/bin/bash
set -euo pipefail
mkdir -p .task-build/plain-output
rm -f .task-build/plain-output/Plain.java
bash task_support/decompile.sh .task-build/fixtures/Plain.class .task-build/plain-output
test -s .task-build/plain-output/Plain.java
grep -Eq 'return .*\+ 1;' .task-build/plain-output/Plain.java
printf 'BASELINE_OK decompiled=Plain.class arithmetic=preserved\n'
