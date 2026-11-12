#!/bin/bash
set -euo pipefail
test -s harness/scale-regression.cpp
test -s harness/check-scale.sh

make_tmpdir() {
	local root="${TMPDIR:-/tmp}"
	local d
	if d=$(mktemp -d "$root/qh-target.XXXXXX" 2>/dev/null); then
		printf '%s\n' "$d"
		return
	fi
	d="$root/qh-target.$$.$RANDOM"
	mkdir -p "$d"
	printf '%s\n' "$d"
}

b=$(make_tmpdir)
trap 'rm -rf "$b"' EXIT
bash harness/build-run.sh harness/baseline.cpp --control-only
bash harness/check-upstream.sh
bash harness/build-run.sh harness/scale-regression.cpp >"$b/direct" 2>&1 || { cat "$b/direct"; exit 1; }
cat "$b/direct"
for tag in similarity entrypoints reuse-degenerate; do grep -Eq "^PASS $tag cases=[1-9][0-9]*$" "$b/direct"; done
grep -Fx QUICKHULL_SCALE_OK "$b/direct"
bash harness/check-scale.sh >"$b/result" 2>&1 || { cat "$b/result"; exit 1; }
cat "$b/result"
for tag in similarity entrypoints reuse-degenerate; do grep -Eq "^PASS $tag cases=[1-9][0-9]*$" "$b/result"; done
grep -Fx QUICKHULL_SCALE_OK "$b/result"
