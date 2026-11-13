#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export GOENV=off GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off
go version
go test -mod=vendor -run '^$' .
