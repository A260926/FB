#!/bin/sh
set -eu
b=$(mktemp -d 2>/dev/null || mktemp -d "${TMPDIR:-/tmp}/pt.XXXXXX")
trap 'rm -rf "$b"' EXIT HUP INT TERM
GO111MODULE=off go test ./pt -run '^TestAbsorptionBaseline$' -count=1 -v
mkdir -p "$b/src/github.com/fogleman"
ln -s "$PWD" "$b/src/github.com/fogleman/pt"
GO111MODULE=off GOPATH="$b" go build -o "$b/hello" examples/hello.go
printf '%s\n' 'PT_EXAMPLE_BUILD_OK'
