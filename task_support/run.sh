#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
case "${1:-baseline}" in
 baseline) exec sh task_support/go.sh go run ./task_support/baseline;;
 target) test -s transfer_lease_test.go
 mkdir -p .task-build
 # Upstream TestMain requires external pure-ftpd/proftpd. Compile every production
 # Go file plus the new self-contained local-protocol regression file instead.
 set --
 for f in ./*.go; do case "$f" in *_test.go) ;; *) set -- "$@" "$f";; esac; done
 sh task_support/go.sh go test -json -count=1 -timeout=45s "$@" transfer_lease_test.go > .task-build/target.json
 sh task_support/go.sh go run ./task_support/verify .task-build/target.json TestPreparedPassive TestPreparedActive TestPreparedPool;;
 *) exit 2;;
esac
