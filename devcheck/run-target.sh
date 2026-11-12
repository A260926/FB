#!/bin/bash
set -euo pipefail
test -s devcheck/fairing.cpp
test -s devcheck/check-fairing.sh
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
bash devcheck/build-run.sh
bash devcheck/build-run.sh devcheck/fairing.cpp >"$b/direct" 2>&1 || { cat "$b/direct"; exit 1; }
cat "$b/direct"
for tag in constrained-descent component-volumes geometry-state stopping-errors; do grep -Eq "^PASS $tag cases=[1-9][0-9]*$" "$b/direct"; done
grep -Fx CINOLIB_FAIRING_OK "$b/direct"
bash devcheck/check-fairing.sh >"$b/result" 2>&1 || { cat "$b/result"; exit 1; }
cat "$b/result"
for tag in constrained-descent component-volumes geometry-state stopping-errors; do grep -Eq "^PASS $tag cases=[1-9][0-9]*$" "$b/result"; done
grep -Fx CINOLIB_FAIRING_OK "$b/result"
