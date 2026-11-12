#!/bin/bash
#
# Build and run the scale/translation regression suite.
#
# The C++ file harness/scale-regression.cpp has its own main() and is self
# checking: this script compiles it together with the production QuickHull
# sources, runs it with no arguments, and validates the printed markers.
set -euo pipefail

# Resolve a writable temporary directory. Prefer mktemp under $TMPDIR; fall
# back to an explicitly created directory if mktemp cannot use the default
# location (some sandboxes disallow the system temp path).
make_tmpdir() {
	local root="${TMPDIR:-/tmp}"
	local d
	if d=$(mktemp -d "$root/qh-scale.XXXXXX" 2>/dev/null); then
		printf '%s\n' "$d"
		return
	fi
	d="$root/qh-scale.$$.$RANDOM"
	mkdir -p "$d"
	printf '%s\n' "$d"
}

b=$(make_tmpdir)
trap 'rm -rf "$b"' EXIT

# build-run.sh compiles QuickHull.cpp together with the given source and runs
# the resulting binary with no arguments.
bash harness/build-run.sh harness/scale-regression.cpp >"$b/result" 2>&1
cat "$b/result"

for tag in similarity entrypoints reuse-degenerate; do
	grep -Eq "^PASS $tag cases=[1-9][0-9]*$" "$b/result"
done
grep -Fx QUICKHULL_SCALE_OK "$b/result"
