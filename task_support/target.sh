#!/bin/sh
set -eu
test -f task_support/auth_router_test.go
out=$(mktemp)
trap 'rm -f "$out"' EXIT
bash task_support/env.sh go test -race -count=1 -v ./task_support >"$out" 2>&1 || { cat "$out"; exit 1; }
cat "$out"
for name in Config Paused Challenge Sessions Failure; do
  grep -q -- "--- PASS: TestRouterAuth${name}" "$out"
done
bash task_support/env.sh go test -count=1 ./lib/proto ./lib/input
