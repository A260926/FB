#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
test -s fuzz_existing_test.go
export GOCACHE="$root/.go-cache"
log=$(mktemp)
trap 'rm -f "$log"' EXIT
go test -vet=off -count=1 -v . -run '^TestFuzzExistingContract$' | tee "$log"
grep -F -- '--- PASS: TestFuzzExistingContract ' "$log" >/dev/null
for name in topology interface_values overlapping_slices map_order policies errors old_stream; do grep -Fx "PASS $name" "$log" >/dev/null; done
printf 'FUZZ_EXISTING_CONTRACT_OK\n'
