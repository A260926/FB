#!/bin/sh
set -eu
. ./userfb_baseline/env.sh
go version | grep -F 'go1.26.4 '
go run ./sys/syz-sysgen
go list -deps -test ./prog ./pkg/fuzzer ./pkg/image >/dev/null
go mod verify
