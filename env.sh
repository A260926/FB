# Source this file from the repository root before Go commands.
export GOTOOLCHAIN=local
export GOPATH="$PWD/.cache/gopath"
export GOMODCACHE="$PWD/.cache/mod"
export GOCACHE="$PWD/.cache/build"
export GOFLAGS=-mod=readonly
