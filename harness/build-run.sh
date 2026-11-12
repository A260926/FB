#!/bin/bash
set -euo pipefail
src=${1:-harness/baseline.cpp}
if [ "$#" -gt 0 ]; then shift; fi

# Create a writable temp directory. Prefer mktemp under $TMPDIR; fall back to
# an explicitly created directory if the default mktemp location is unusable.
make_tmpdir() {
	local root="${TMPDIR:-/tmp}"
	local d
	if d=$(mktemp -d "$root/qh-build.XXXXXX" 2>/dev/null); then
		printf '%s\n' "$d"
		return
	fi
	d="$root/qh-build.$$.$RANDOM"
	mkdir -p "$d"
	printf '%s\n' "$d"
}

b=$(make_tmpdir)
trap 'rm -rf "$b"' EXIT
c++ -std=c++11 -O1 -I. QuickHull.cpp "$src" -o "$b/check"
"$b/check" "$@"
