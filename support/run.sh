#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in baseline|target) ;; *) echo "expected baseline or target" >&2; exit 2;; esac
build=${CHECK_BUILD_DIR:-.build}
mkdir -p "$build"
cxx=${CXX:-c++}
flags=(-std=c++17 -O1 -g -DP2T_STATIC_EXPORTS -I. -Isupport)
sources=(poly2tri/common/*.cc poly2tri/sweep/*.cc)
run_checked() {
 local tag=$1 required=$2 minimum=$3 per_group=$4; shift 4
 set +e
 "$@" >"$build/$tag.stdout" 2>"$build/$tag.stderr"
 local raw=$?
 set -e
 printf '%s\n' "$raw" >"$build/$tag.exit"
 cat "$build/$tag.stdout"
 cat "$build/$tag.stderr" >&2
 if ((raw!=0)); then echo "RAW_EXIT $raw" >&2; return "$raw"; fi
 awk -v required="$required" -v minimum="$minimum" -v per_group="$per_group" -f support/verify.awk "$build/$tag.stdout"
}
if [[ "$mode" == target ]]; then
 test -s tests/constraint_edit.cpp || { echo "MISSING_OR_EMPTY tests/constraint_edit.cpp" >&2; exit 2; }
 "$cxx" "${flags[@]}" "${sources[@]}" support/driver.cpp tests/constraint_edit.cpp -o "$build/target"
 run_checked target geometry,ownership,session 6 2 "$build/target"
fi
"$cxx" "${flags[@]}" "${sources[@]}" support/baseline.cpp -o "$build/baseline"
set +e
"$build/baseline" >"$build/baseline.stdout" 2>"$build/baseline.stderr"
raw=$?
set -e
printf '%s\n' "$raw" >"$build/baseline.exit"
cat "$build/baseline.stdout"; cat "$build/baseline.stderr" >&2
((raw==0)) || exit "$raw"
grep -q '^POLY2TRI_STATIC_SURFACE_OK triangles=20 area=116 boundary=8$' "$build/baseline.stdout"
"$cxx" "${flags[@]}" -Isupport/compat -DP2T_BASE_DIR=\".\" "${sources[@]}" support/driver.cpp unittest/main.cpp unittest/TriangleTest.cpp -o "$build/upstream"
run_checked upstream upstream 11 11 "$build/upstream" upstream
if [[ "$mode" == target ]]; then echo POLY2TRI_CONSTRAINT_EDIT_COMPLETE; else echo UPSTREAM_POLY2TRI_SUITE_OK; fi
