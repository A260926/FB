#!/bin/bash
set -euo pipefail
cache=$(mktemp -d)
trap 'chmod -R u+w "$cache"; rm -rf "$cache"' EXIT
export GOCACHE="$cache/build" GOMODCACHE="$cache/mod" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
export GO111MODULE=off
log="$cache/target.log"
go test -count=1 -v -run '^(TestVHDXChainRouting|TestVHDXChainIdentity|TestVHDXChainBoundaries|TestVHDXChainIsolation)$' ./parser | tee "$log"
grep -Eq '^--- PASS: TestVHDXChainRouting \(' "$log"
grep -Eq '^--- PASS: TestVHDXChainIdentity \(' "$log"
grep -Eq '^--- PASS: TestVHDXChainBoundaries \(' "$log"
grep -Eq '^--- PASS: TestVHDXChainIsolation \(' "$log"
go test -count=1 -v ./parser -run '^TestBATBaseline$'
