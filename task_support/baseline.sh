#!/bin/bash
set -euo pipefail
mkdir -p .task-build/legacy .task-build/modern .task-build/recompiled
java -cp .task-build/classes:.task-build/analyzer task_support.Analyze .task-build/fixtures LegacySwitch > .task-build/legacy/LegacySwitch.java
javac --release 17 -proc:none -d .task-build/recompiled .task-build/legacy/LegacySwitch.java
printf '%s\n' 'BASELINE_LEGACY_OK decompile_and_recompile'
set +e
java -cp .task-build/classes:.task-build/analyzer task_support.Analyze .task-build/fixtures ModernSwitch > .task-build/modern/ModernSwitch.java 2> .task-build/modern-decompile.log
decompile_exit=$?
javac --release 17 -proc:none -d .task-build/recompiled .task-build/modern/ModernSwitch.java > .task-build/modern-recompile.log 2>&1
recompile_exit=$?
set -e
printf 'MODERN_DECOMPILE_EXIT=%s\nMODERN_RECOMPILE_EXIT=%s\n' "$decompile_exit" "$recompile_exit"
printf '%s\n' 'MODERN_CURRENT_OUTPUT_BEGIN'
cat .task-build/modern/ModernSwitch.java
cat .task-build/modern-decompile.log
cat .task-build/modern-recompile.log
printf '%s\n' 'MODERN_CURRENT_OUTPUT_END'
