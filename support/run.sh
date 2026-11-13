#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:?use build, baseline or target}
b=.textpath-build
cmake -S . -B "$b" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DLUNASVG_BUILD_EXAMPLES=OFF -DLUNASVG_DISABLE_LOAD_SYSTEM_FONTS=ON -DLUNASVG_DISABLE_EXTERNAL_RESOURCES=ON -DPLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD=ON
cmake --build "$b" --parallel 2
[[ "$mode" != build ]] || exit 0
t=$(mktemp -d);trap 'rm -rf "$t"' EXIT
compile() {
 c++ -std=c++17 -DLUNASVG_BUILD_STATIC -DPLUTOVG_BUILD_STATIC -Iinclude -Isource -Iplutovg/include -Isupport "$@" "$b/liblunasvg.a" "$b/plutovg/libplutovg.a" -pthread -lm
}
baseline() {
 compile support/baseline.cpp -o "$t/baseline";"$t/baseline"
 compile examples/svg2png.cpp -o "$t/svg2png"
 (root="$PWD"; cd "$t"; ./svg2png "$root/support/vector.svg" 64x64)
 test -s "$t/vector.svg.png"
 [[ "$(od -An -tx1 -N8 "$t/vector.svg.png" | tr -d ' \n')" == 89504e470d0a1a0a ]]
 echo 'UPSTREAM_SVG2PNG_OK size=64x64'
}
if [[ "$mode" == baseline ]];then baseline;exit;fi
[[ "$mode" == target ]]
test -s tests/text_path.cpp
compile tests/text_path.cpp support/driver.cpp -o "$t/target"
status=0
"$t/target" >"$t/stdout" 2>"$t/stderr" || status=$?
cat "$t/stdout";cat "$t/stderr" >&2
printf 'TARGET_PROCESS_EXIT %s\n' "$status"
[[ "$status" == 0 ]] || exit "$status"
for g in normal boundary integration; do
 grep -Eq "^GROUP $g [2-9][0-9]* [1-9][0-9]* DONE$|^GROUP $g [1-9][0-9]+ [1-9][0-9]* DONE$" "$t/stdout"
done
grep -Eq '^TARGET_DONE [1-9][0-9]*$' "$t/stdout"
baseline
