#!/bin/sh
set -eu
export GOWORK=off GOPROXY=off GOSUMDB=off GOFLAGS=-mod=vendor
export GOCACHE="$PWD/.build/go-cache" GOMODCACHE="$PWD/.build/go-mod" GOPATH="$PWD/.build/gopath"
exec "$@"
