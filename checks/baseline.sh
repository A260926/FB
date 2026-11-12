#!/bin/bash
set -euo pipefail
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
build=$(mktemp -d)
trap 'chmod -R u+w "$build"; rm -rf "$build"' EXIT
export GOCACHE="$build/cache" GOMODCACHE="$build/mod"
go test -count=1 -v -run '^(TestIRI|TestLiteral|TestNT|TestNTSerialization|TestTTL|TestEncodingTTL|TestNQ|TestEncodeNQuads|TestBaseline)$' .
