#!/usr/bin/env bash
set -euo pipefail
export GOENV=off GOTOOLCHAIN=go1.26.4
export GOPATH="$PWD/.build/go" GOMODCACHE="$PWD/.build/mod" GOCACHE="$PWD/.build/cache"
go run -mod=readonly author_support/baseline/main.go
