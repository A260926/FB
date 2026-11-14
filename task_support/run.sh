#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOWORK=off GOFLAGS=""
export GOCACHE="$PWD/.task-build/gocache" GOMODCACHE="$PWD/.task-build/modcache" GOPATH="$PWD/.task-build/gopath"
mkdir -p "$GOCACHE" "$GOMODCACHE" "$GOPATH"
if [[ $mode == target ]]; then
  test -s clipboard_flow_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -mod=readonly -vet=off -race -count=1 -v -timeout=30s -run '^(TestClipboardNegotiation|TestClipboardOwnership|TestClipboardWire)$' . >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestClipboardNegotiation TestClipboardOwnership TestClipboardWire; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go test -mod=readonly -vet=off -count=1 -v -timeout=30s -run '^(TestDesktopBaseline|TestParseProtocolVersion|TestClient_LowMajorVersion|TestClient_LowMinorVersion|TestClientAuthNone_Impl|TestClientAuthPasswordSuccess_Impl|TestClientAuthPasswordReject_Impl)$' .
