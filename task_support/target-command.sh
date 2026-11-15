#!/bin/bash
set -euo pipefail
go test -v -count=1 -timeout=120s -run '^(TestTerrainSelection|TestTerrainSeams|TestTerrainUpdates)$' .
test -s examples/terrain/main.go
if [[ -s examples/terrain/main.go ]]; then
 go run examples/terrain/main.go .task-build/terrain.png
fi
