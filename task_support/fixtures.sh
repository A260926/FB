#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
for pair in linux/amd64 windows/amd64 darwin/arm64; do
  os=${pair%/*}
  arch=${pair#*/}
  output=".task-build/${os}-${arch}.bin"
  GOOS="$os" GOARCH="$arch" CGO_ENABLED=0 go build -trimpath -ldflags='-s -w' -o "$output" ./task_support/fixture
  # The target binaries are data. Only the host GoReSym executable is run.
  .task-build/goresym "$output" >".task-build/${os}-${arch}.json"
done
