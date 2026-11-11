#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -s pt/csg_target_test.go || { echo 'MISSING_TARGET pt/csg_target_test.go'; exit 1; }
test -s examples/csg/main.go || { echo 'MISSING_TARGET examples/csg/main.go'; exit 1; }
mkdir -p .cache
log=$(mktemp .cache/target.XXXXXX)
trap 'rm -f "$log"' EXIT HUP INT TERM
if ! sh support/go-env.sh go test -race -json ./pt -run '^(TestCSGIntervals|TestCSGSurfaceTransport|TestCSGRender|TestSeedPrimitivePaths|TestSeedSceneSampler)$' -count=1 >"$log" 2>&1; then
  cat "$log"
  exit 1
fi
cat "$log"
sh support/go-env.sh go run ./support/checkrun.go "$log" TestCSGIntervals TestCSGSurfaceTransport TestCSGRender TestSeedPrimitivePaths TestSeedSceneSampler
sh support/go-env.sh go build -o .cache/hello ./examples/hello.go
sh support/go-env.sh go run ./examples/csg
test -s .cache/csg.ppm
echo CSG_TARGET_OK
