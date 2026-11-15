#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/setup_binding_target.cpp; fi
mkdir -p .task-build
test -s DragonBones/src/dragonBones/armature/Armature.cpp
find DragonBones/src -name '*.cpp' -type f -print | LC_ALL=C sort > .task-build/sources
test -s .task-build/sources
object_files=()
while IFS= read -r source_file; do
  object_path=".task-build/$(echo "$source_file" | tr / _).o"
  c++ -std=c++17 -O1 -Wno-deprecated-declarations -IDragonBones/src -I3rdParty -I. -Itask_support -c "$source_file" -o "$object_path"
  object_files+=("$object_path")
done < .task-build/sources
[[ ${#object_files[@]} -gt 0 ]]
rm -f .task-build/libdragonbones.a
ar rcs .task-build/libdragonbones.a "${object_files[@]}"
c++ -std=c++17 -O1 -Wno-deprecated-declarations -IDragonBones/src -I3rdParty -I. -Itask_support task_support/baseline.cpp .task-build/libdragonbones.a -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s DragonBones/src/dragonBones/armature/SetupPoseBinding.h
test -s DragonBones/src/dragonBones/armature/SetupPoseBinding.cpp
test -s examples/setup_binding.cpp
c++ -std=c++17 -O1 -Wno-deprecated-declarations -IDragonBones/src -I3rdParty -I. -Itask_support task_support/driver.c tests/setup_binding_target.cpp .task-build/DragonBones_src_dragonBones_armature_SetupPoseBinding.cpp.o .task-build/libdragonbones.a -o .task-build/target
c++ -std=c++17 -O1 -Wno-deprecated-declarations -IDragonBones/src -I3rdParty -I. -Itask_support examples/setup_binding.cpp .task-build/DragonBones_src_dragonBones_armature_SetupPoseBinding.cpp.o .task-build/libdragonbones.a -o .task-build/example
.task-build/example
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in basis deform cache; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
