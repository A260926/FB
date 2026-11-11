#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
export GOCACHE="$root/.go-cache"
log=$(mktemp)
trap 'rm -f "$log"' EXIT
go test -vet=off -count=1 -v . -run '^(TestContinue_Fuzz_WithReflectValue|TestFuzz_Maxdepth|TestNewFromGoFuzz)$' | tee "$log"
for name in TestContinue_Fuzz_WithReflectValue TestFuzz_Maxdepth TestNewFromGoFuzz; do grep -F -- "--- PASS: $name " "$log" >/dev/null; done
go run ./baseline
