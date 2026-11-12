#!/bin/bash
set -euo pipefail
test -s v3/sparse_ranges_test.go
out=$(bash devcheck/go.sh go test -count=1 -timeout=40s -v -run '^(TestSparseRangeCapture|TestSparseRangeConsistency)$' .)
printf '%s\n' "$out"
for name in TestSparseRangeCapture TestSparseRangeConsistency; do grep -q -- "--- PASS: $name " <<<"$out"; done
bash devcheck/go.sh go test -count=1 -v -run '^(TestBaseline|TestCopyFileReportsCloseError|TestCloseWriterIgnoresNonClosers)$' .
