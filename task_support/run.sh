#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_support/edited_reader_cases.cpp; fi
mkdir -p .task-build
cmake -S . -B .task-build/lib -DBUILD_APPS=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" > .task-build/configure.log 2>&1 || { tail -n 30 .task-build/configure.log; exit 1; }
if [[ $mode == target ]]; then fresh_build=(--clean-first); else fresh_build=(--verbose); fi
cmake --build .task-build/lib --target ap4 "${fresh_build[@]}" -j 4 > .task-build/build.log 2>&1 || { tail -n 30 .task-build/build.log; exit 1; }
includes=(-ISource/C++/Core -ISource/C++/Codecs -ISource/C++/Crypto -ISource/C++/MetaData -I.)
c++ -std=c++11 "${includes[@]}" task_support/baseline.cpp .task-build/lib/libap4.a -o .task-build/baseline
if [[ $mode == target ]]; then
  for src in Source/C++/Core/Ap4EditedSampleReader.h Source/C++/Core/Ap4EditedSampleReader.cpp Source/C++/Core/Ap4SampleSpool.h Source/C++/Core/Ap4SampleSpool.cpp Source/C++/Apps/EditedPreview/EditedPreview.cpp task_support/edited_preview_verify.cpp; do test -s "$src"; done
for src in Ap4EditedSampleReader.cpp Ap4SampleSpool.cpp; do grep -F "/Core/$src" .task-build/lib/compile_commands.json >/dev/null; test -s ".task-build/lib/CMakeFiles/ap4.dir/Source/C++/Core/$src.o"; done
forced=(-include Source/C++/Core/Ap4EditedSampleReader.h -include Source/C++/Core/Ap4SampleSpool.h)
c++ -std=c++11 "${includes[@]}" "${forced[@]}" -c task_support/header_consumer.cpp -o .task-build/header_consumer.o
if [[ $(uname -s) == Darwin ]]; then library=(-Wl,-force_load,.task-build/lib/libap4.a); else library=(-Wl,--whole-archive .task-build/lib/libap4.a -Wl,--no-whole-archive); fi
c++ -std=c++11 "${includes[@]}" "${forced[@]}" -x c++ task_support/driver.c task_support/edited_reader_cases.cpp -x none .task-build/header_consumer.o "${library[@]}" -o .task-build/target
c++ -std=c++11 "${includes[@]}" "${forced[@]}" Source/C++/Apps/EditedPreview/EditedPreview.cpp "${library[@]}" -o .task-build/editedpreview
c++ -std=c++11 "${includes[@]}" "${forced[@]}" task_support/edited_preview_verify.cpp "${library[@]}" -o .task-build/preview_verify
rm -f .task-build/preview-input.mp4 .task-build/preview.bin .task-build/preview.scratch
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in forward budget timeline errors; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == target ]]; then
  test -s .task-build/preview-input.mp4
  .task-build/editedpreview .task-build/preview-input.mp4 .task-build/preview.scratch .task-build/preview.bin 1048576
  test -s .task-build/preview.bin
  .task-build/preview_verify .task-build/preview-input.mp4 .task-build/preview.bin
  .task-build/baseline --normal-only
else
  .task-build/baseline
fi
