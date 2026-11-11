#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .build
export GOCACHE="$PWD/.build/go-build"
export GOPATH="$PWD/.build/go"
go test ./masktest -run "^TestBaseline$" -count=1 -v
