#!/bin/bash
set -euo pipefail
javac -encoding UTF-8 --release 11 -cp .build/classes -d .build/classes task_support/Baseline.java
java -ea -cp .build/classes Baseline
