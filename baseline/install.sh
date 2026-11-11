#!/bin/sh
set -eu
. ./env.sh
version=$(go env GOVERSION)
test "$version" = go1.26.4 || { echo "need go1.26.4; got $version" >&2; exit 1; }
go mod download all
go mod verify
go build -tags=gofuzz -o baseline/testee-bin ./baseline/testee
printf 'INSTALL_READY %s\n' "$version"
