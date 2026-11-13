#!/bin/sh
set -eu
test -f stream_reader_test.go
out=$(mktemp "${TMPDIR:-/tmp}/robotstxt-target.XXXXXX")
trap 'rm -f "$out"' EXIT
bash task_support/env.sh go test -race -count=1 -v -run '^TestReaderStream' . >"$out" 2>&1 || { cat "$out"; exit 1; }
cat "$out"
for name in Equivalence ReaderError Bounded Response; do
  grep -q -- "--- PASS: TestReaderStream${name}" "$out"
done
bash task_support/env.sh go test -race -count=1 ./...
