#!/usr/bin/env bash
set -euo pipefail
test -s devcheck/cdt-mesh.cpp
test -s devcheck/check-cdt.sh
out=$(mktemp "${TMPDIR:-/tmp}/p2t-target-out.XXXXXX")
artifacts=$(mktemp -d "${TMPDIR:-/tmp}/p2t-target-art.XXXXXX")
trap 'rm -f "$out"; rm -rf "$artifacts"' EXIT
bash devcheck/check-cdt.sh "$artifacts" | tee "$out"
for name in coverage topology delaunay identity; do grep -q "^PASS $name$" "$out"; done
for name in skip-steiner open-hole skip-legalize; do grep -q "^DETECTED $name$" "$out"; done
grep -q '^POLY2TRI_MESH_TARGET_OK$' "$out"
find "$artifacts" -name '*.svg' -type f -size +100c | grep -q .
bash devcheck/build-run.sh devcheck/baseline.cpp
