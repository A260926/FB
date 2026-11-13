#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in
baseline) ;;
target) test -s test/radio_contract.cpp || { echo 'missing or empty test/radio_contract.cpp' >&2; exit 1; } ;;
*) exit 2 ;;
esac
build=$(mktemp -d "${TMPDIR:-/tmp}/lexbor-build.XXXXXX")
trap 'rm -rf "$build"' EXIT
cmake -S . -B "$build" -DLEXBOR_BUILD_SHARED=OFF -DLEXBOR_BUILD_STATIC=ON -DLEXBOR_BUILD_TESTS=ON -DLEXBOR_ENABLE_TEST_HTML_WPT=OFF -DLEXBOR_BUILD_EXAMPLES=OFF -DLEXBOR_BUILD_BENCHMARKS=OFF -DLEXBOR_BUILD_UTILS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" --target lexbor_static lexbor_html_test_lexbor_html_attributes lexbor_html_test_lexbor_html_clone -j 4
if [[ "$mode" == target ]]; then
 mkdir -p support/results
 rm -f support/results/target.stdout support/results/target.exit
 ${CXX:-c++} -std=c++17 -DLEXBOR_STATIC -Isource -Isupport support/driver.cpp test/radio_contract.cpp "$build/liblexbor_static.a" -lm -o "$build/target"
 set +e
 "$build/target" | tee support/results/target.stdout
 status=("${PIPESTATUS[@]}")
 set -e
 printf '%s\n' "${status[0]}" > support/results/target.exit
 printf 'TARGET_RAW_EXIT=%s\n' "${status[0]}"
 if (( status[0] != 0 )); then exit "${status[0]}"; fi
 if (( status[1] != 0 )); then exit "${status[1]}"; fi
 awk -f support/verify-output.awk support/results/target.stdout
fi
${CC:-cc} -std=c99 -DLEXBOR_STATIC -Isource support/baseline.c "$build/liblexbor_static.a" -lm -o "$build/baseline"
"$build/baseline"
ctest --test-dir "$build" -R '^lexbor_html_(attributes|clone)$' --output-on-failure | tee "$build/old.log"
grep -q '100% tests passed, 0 tests failed out of 2' "$build/old.log"
echo UPSTREAM_DOM_MUTATIONS_OK
