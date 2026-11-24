#!/bin/bash
set -euo pipefail
for production in internal/fuzz/replay.go internal/fuzz/replay_codec.go; do
  test -s "$production"
  go list -f '{{range .GoFiles}}{{println .}}{{end}}' ./internal/fuzz | grep -qx "${production##*/}"
done
cp task_support/api_consumer_test.go.in internal/fuzz/task_api_consumer_test.go
go test ./internal/fuzz -run '^TestTaskAPIConsumer$' -count=1 -v
test -s examples/replay/main.go
go build -o .task-build/replay-example ./examples/replay
output=$(mktemp -d .task-build/replay-output.XXXXXX)
.task-build/replay-example "$output/transcript.txt"
test -s "$output/transcript.txt"
grep -q 'replay_restart_ok' "$output/transcript.txt"
