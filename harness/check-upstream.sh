#!/bin/bash
set -euo pipefail

make_tmpdir() {
	local root="${TMPDIR:-/tmp}"
	local d
	if d=$(mktemp -d "$root/qh-upstream.XXXXXX" 2>/dev/null); then
		printf '%s\n' "$d"
		return
	fi
	d="$root/qh-upstream.$$.$RANDOM"
	mkdir -p "$d"
	printf '%s\n' "$d"
}

b=$(make_tmpdir)
trap 'rm -rf "$b"' EXIT
c++ -std=c++11 -O1 -I. QuickHull.cpp Tests/main.cpp Tests/QuickHullTests.cpp -o "$b/tests"
"$b/tests" >"$b/result" 2>&1
cat "$b/result"
! grep -qi 'test failed' "$b/result"
grep -Fx 'QuickHull tests succesfully passed.' "$b/result"
echo QUICKHULL_UPSTREAM_OK
