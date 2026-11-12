#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash support/build.sh "$build" target
"$build/run" | tee "$build/target.stdout"
# Only the fixed C driver emits completion lines, after each group returns.
# An exit(0) inside a test must not silently skip remaining groups.
awk '
/^GROUP matrix checks=[1-9][0-9]*$/ {m++; split($3,a,"="); sum+=a[2]; next}
/^GROUP rows checks=[1-9][0-9]*$/ {r++; split($3,a,"="); sum+=a[2]; next}
/^GROUP isolation checks=[1-9][0-9]*$/ {i++; split($3,a,"="); sum+=a[2]; next}
/^WEBP_COLOR_OK checks=[1-9][0-9]*$/ {f++; split($2,a,"="); total=a[2]; next}
END {if(m!=1 || r!=1 || i!=1 || f!=1 || total!=sum) {print "INCOMPLETE color groups" > "/dev/stderr"; exit 44}}
' "$build/target.stdout"
bash support/baseline.sh
