#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in build|baseline|target) ;; *) exit 64;; esac
mkdir -p .task-build
objects=()
for source in src/*.cpp; do
    [[ "$source" == src/MiniLight.cpp ]] && continue
    object=".task-build/$(basename "${source%.cpp}").o"
    "${CXX:-c++}" -std=c++17 -O2 -fno-fast-math -Isrc -Itask_support -c "$source" -o "$object"
    objects+=("$object")
done
"${CXX:-c++}" -std=c++17 -O2 -fno-fast-math -Isrc task_support/baseline.cpp "${objects[@]}" -o .task-build/baseline
"${CXX:-c++}" -std=c++17 -O2 -fno-fast-math -Isrc src/MiniLight.cpp "${objects[@]}" -o .task-build/minilight
[[ "$mode" == build ]] && exit 0
.task-build/baseline
.task-build/minilight .task-build/legacy.ml > .task-build/legacy-cli.stdout
cat .task-build/legacy-cli.stdout
grep -qx finished .task-build/legacy-cli.stdout
test -s .task-build/legacy.ml.ppm
echo LEGACY_CLI_OK
if [[ "$mode" == target ]]; then
    test -s task_support/guiding_test.cpp || { echo MISSING_TARGET_TEST >&2; exit 3; }
    "${CXX:-c++}" -std=c++17 -O2 -fno-fast-math -Isrc -Itask_support task_support/runner.cpp task_support/guiding_test.cpp "${objects[@]}" -o .task-build/target
    raw=0
    .task-build/target > .task-build/target.stdout 2> .task-build/target.stderr || raw=$?
    cat .task-build/target.stdout
    cat .task-build/target.stderr >&2
    printf 'TARGET_PROCESS_EXIT=%s\n' "$raw"
    [[ "$raw" == 0 ]] || exit "$raw"
    awk -f task_support/verify.awk .task-build/target.stdout
fi
