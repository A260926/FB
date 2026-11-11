#!/bin/sh
set -eu
export GOTOOLCHAIN=local GOWORK=off GOFLAGS=-mod=readonly
export GOPROXY=https://proxy.golang.org GOSUMDB=sum.golang.org
export GOCACHE="$PWD/.cache/build" GOMODCACHE="$PWD/.cache/mod"
go test -count=1 -timeout=45s -v -run '^TestAuthorBaselineReceiveEpoch$' .
