#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOWORK=off GOFLAGS=-mod=readonly
shasum -a 256 -c task_support/deps.sha256
# All root Go files, including the real Mesh type; no stubs or excluded production files.
go list -deps . > /dev/null
printf '%s\n' OFFLINE_SOURCES_OK
