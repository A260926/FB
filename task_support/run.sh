#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
case "${1:-baseline}" in
 baseline) exec sh task_support/go.sh go run -mod=vendor ./task_support/baseline;;
 target) test -s rpcc/session_tunnel_test.go; mkdir -p .task-build
 sh task_support/go.sh sh -ec 'go test -mod=vendor -json -count=1 -timeout=45s -run "^(TestTunnelDualReply|TestTunnelIsolation|TestTunnelGenerated)$" ./rpcc > .task-build/target.json; go run -mod=vendor ./task_support/verify .task-build/target.json TestTunnelDualReply TestTunnelIsolation TestTunnelGenerated; go test -mod=vendor -count=1 -timeout=30s -run "^(TestConn_Invoke|TestConn_InvokeError|TestConn_InvokeRemoteDisconnected|TestConn_InvokeConnectionClosed|TestConn_InvokeDeadlineExceeded|TestConn_InvokeContextCanceled)$" ./rpcc' ;;
 *) exit 2;;
esac
