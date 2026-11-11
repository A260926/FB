#!/usr/bin/env bash
set -euo pipefail
test -s regress/seam-cut.cpp
test -s regress/check-seams.sh
test -s src/OpenMesh/Tools/Utils/SeamCutT.hh
out=$(mktemp)
trap 'rm -f "$out"' EXIT
bash regress/check-seams.sh | tee "$out"
for name in boundary-chain sphere-loop torus-loop empty reject-branch; do
 grep -q "^PASS $name$" "$out"
done
grep -q '^OPENMESH_SEAM_TARGET_OK$' "$out"
bash regress/build-run.sh regress/baseline.cpp
