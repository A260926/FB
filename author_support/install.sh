#!/bin/sh
set -eu
export GOTOOLCHAIN=local GOWORK=off GOFLAGS=-mod=readonly
export GOPROXY=https://proxy.golang.org GOSUMDB=sum.golang.org
export GOCACHE="$PWD/.author-cache/build" GOMODCACHE="$PWD/.author-cache/mod"
case "$(go env GOVERSION)" in
  go1.26.4) ;;
  *) echo 'Go 1.26.4 is required by this reproducible snapshot' >&2; exit 1 ;;
esac
go mod download
go mod verify
go version
