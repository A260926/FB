#!/bin/bash
set -euo pipefail
test -s sanity/topology.cpp
test -s sanity/check-topology.sh
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
bash sanity/build-run.sh
bash sanity/build-run.sh sanity/topology.cpp >"$b/direct" 2>&1 || { cat "$b/direct"; exit 1; }
cat "$b/direct"
for tag in closed-shells boundaries lossless-reuse rejected-inputs; do grep -Eq "^PASS $tag cases=[1-9][0-9]*$" "$b/direct"; done
grep -Fx FQMS_TOPOLOGY_OK "$b/direct"
bash sanity/check-topology.sh >"$b/result" 2>&1 || { cat "$b/result"; exit 1; }
cat "$b/result"
for tag in closed-shells boundaries lossless-reuse rejected-inputs; do grep -Eq "^PASS $tag cases=[1-9][0-9]*$" "$b/result"; done
grep -Fx FQMS_TOPOLOGY_OK "$b/result"
