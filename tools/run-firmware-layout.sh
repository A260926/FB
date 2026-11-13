#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export GOMODCACHE="$work/mod" GOCACHE="$work/cache" GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off GOFLAGS='-modfile=tools/offline.mod -mod=vendor'
[ -s pkg/uefi/layout.go ] && [ -s pkg/uefi/layout_test.go ]
go test -list '^TestFirmwareLayout$' ./pkg/uefi | grep -qx TestFirmwareLayout
go test -count=1 -v -run '^TestFirmwareLayout$' ./pkg/uefi | tee "$work/test.log"
! grep -q -- '--- SKIP:' "$work/test.log"
awk '/--- PASS: TestFirmwareLayout\// {n++} END {exit(n<6)}' "$work/test.log"
go run ./tools/baseline
