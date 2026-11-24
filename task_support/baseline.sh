#!/bin/bash
set -euo pipefail
go test ./go-fuzz -run '^(TestBenignBaseline|TestIncrementDecrement)$' -count=1 -v
go test ./go-fuzz/versifier -count=1
