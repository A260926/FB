#!/bin/bash
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor
set -euo pipefail
export GOMODCACHE="$PWD/.cache/mod"
export GOCACHE="$PWD/.cache/go"
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
go build -o "$b/baseline" ./checks/baseline
"$b/baseline"
go test -v ./model3d -run '^(TestEmptyColliders|TestMeshRayCollisionsConsistency|TestMeshSDFVertices)$' -count=1 | tee "$b/old"
for name in TestEmptyColliders TestMeshRayCollisionsConsistency TestMeshSDFVertices; do
 grep -Eq "^--- PASS: $name \\(" "$b/old"
done
echo MODEL3D_OLD_COLLIDER_SDF_OK
