#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
[[ $mode == baseline || $mode == target ]]
if [[ $mode == target ]]; then
    test -s source/integrator/path-tracer/wavefront.hpp
    test -s source/integrator/path-tracer/wavefront.cpp
    test -s checks/wavefront.cpp
    test -s examples/wavefront.cpp
fi

mkdir -p .task-build/objects
objects=()
while IFS= read -r -d '' source_file; do
    object=".task-build/objects/${#objects[@]}.o"
    c++ -std=c++20 -O1 -pthread -I. -Ilib/glm -Ilib/nlohmann -c "$source_file" -o "$object"
    objects+=("$object")
done < <(find source -type f -name '*.cpp' ! -path 'source/main.cpp' -print0)
test "${#objects[@]}" -gt 0
c++ -std=c++20 -O1 -pthread -I. -Ilib/glm -Ilib/nlohmann \
    task_support/baseline.cpp "${objects[@]}" -o .task-build/baseline

if [[ $mode == target ]]; then
    c++ -std=c++20 -O1 -pthread -I. -Ilib/glm -Ilib/nlohmann \
        task_support/driver.cpp checks/wavefront.cpp "${objects[@]}" -o .task-build/target
    .task-build/target
    c++ -std=c++20 -O1 -pthread -I. -Ilib/glm -Ilib/nlohmann \
        examples/wavefront.cpp "${objects[@]}" -o .task-build/wavefront-example
    output=$(mktemp -d "$PWD/.task-build/wavefront-output.XXXXXX")
    example="$PWD/.task-build/wavefront-example"
    (cd "$output" && "$example" "$output/wavefront.ppm")
    image_found=0
    while IFS= read -r -d '' image; do
        signature=$(head -c 2 "$image")
        if [[ -s $image && ( $signature == P3 || $signature == P6 ) ]]; then
            image_found=1
            printf 'WAVEFRONT_EXAMPLE_IMAGE %s\n' "$image"
        fi
    done < <(find "$output" -type f -name '*.ppm' -print0)
    test "$image_found" == 1
fi
.task-build/baseline
