#!/bin/sh
set -eu
export GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off
for name in TestRechunkLayout TestRechunkTables TestRechunkInvalid; do
 go test ./format/mp4 -list "^${name}$" | grep -qx "$name"
 go test ./format/mp4 -run "^${name}$" -count=1 -v
done
go run ./support/baseline.go
