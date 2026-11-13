#!/bin/sh
set -eu
export GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off
for name in TestContentEncodingStack TestContentEncodingLaces TestContentEncodingErrors; do
 go test ./mkvcore -list "^${name}$" | grep -qx "$name"
 go test ./mkvcore -run "^${name}$" -count=1 -v
done
go test ./mkvcore -run '^TestBlockReader$' -count=1
go run ./support/baseline.go
