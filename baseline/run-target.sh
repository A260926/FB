#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -s analysis/reachability_contract_test.go
build=$(mktemp -d)
export GOCACHE="$build/cache" GOMODCACHE="$build/mod"
trap 'chmod -R u+w "$build"; rm -rf "$build"' EXIT
go mod download
go test ./analysis -run '^TestReachabilityContract$' -count=1 -v | tee "$build/target.log"
grep -q -- '--- PASS: TestReachabilityContract ' "$build/target.log"
for name in initialization fixedpoint copy signature external malformed; do
  grep -q -- "--- PASS: TestReachabilityContract/$name " "$build/target.log"
done
echo WASM_REACHABILITY_CONTRACT_OK
bash baseline/run.sh
