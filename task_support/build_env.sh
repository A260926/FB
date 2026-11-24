# Sourced by the task runner. No solution code.
set -euo pipefail
mode=${1:-baseline}
if [[ $mode == target ]]; then
 test -s go-fuzz/ownership_test.go
 for src in ownership.go ownership_worker.go ownership_example.go; do test -s "go-fuzz/$src"; done
fi
TASK_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/gofuzz-build.XXXXXX")
TASK_CACHE=${TASK_GUARD_CACHE:-$TASK_BUILD/cache}
mkdir -p "$TASK_CACHE/gocache" "$TASK_CACHE/modcache"
export GOCACHE="$TASK_CACHE/gocache" GOMODCACHE="$TASK_CACHE/modcache"
export GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off GOFLAGS=-mod=vendor
export GOFUZZ_BENIGN_BIN="$TASK_BUILD/benign"
go build -tags gofuzz -o "$GOFUZZ_BENIGN_BIN" ./task_support/benign
if [[ $mode == target ]]; then
 go list -f '{{range .GoFiles}}{{println .}}{{end}}' ./go-fuzz > "$TASK_BUILD/gofiles.txt"
 for src in ownership.go ownership_worker.go ownership_example.go; do grep -qx "$src" "$TASK_BUILD/gofiles.txt"; done
 cp task_support/api_probe.go.txt go-fuzz/zz_author_ownership_probe_test.go
fi
