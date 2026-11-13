#!/bin/sh
set -eu
export GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off
for name in TestProgramReplacement TestPMTSections TestProgramCompatibility; do
 go test ./go-mpeg2 -list "^${name}$" | grep -qx "$name"
 go test ./go-mpeg2 -run "^${name}$" -count=1 -v
done
go run ./support/baseline.go
