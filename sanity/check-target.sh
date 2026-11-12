#!/usr/bin/env bash
# Offline sanity check for the go-audio/wav playlist work.
#
# All dependencies are vendored locally, so the build and tests run with
# the module cache and checksum database disabled.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

export GOFLAGS=-mod=vendor
export GOPROXY=off
export GOSUMDB=off

# The fixed module sources and licenses must be vendored.
test -s vendor/modules.txt

echo "== go vet =="
go vet ./...

echo "== go test =="
go test ./...

echo "sanity check passed"
