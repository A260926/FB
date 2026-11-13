#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export GOENV=off GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off
go test -mod=vendor -run '^TestRPABaseline$' -count=1 -v .
