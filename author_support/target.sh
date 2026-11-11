#!/usr/bin/env bash
set -euo pipefail
export GOTOOLCHAIN=local GOWORK=off GOFLAGS=-mod=readonly
export GOMODCACHE="$PWD/.build/gomod" GOCACHE="$PWD/.build/gocache"
mkdir -p .build
for file in browser/navigation_replay_test.go jar/history_replay_test.go; do
  test -s "$file" || { echo "missing target test: $file" >&2; exit 1; }
done
# Compile and enumerate first: missing, renamed and zero-match tests must fail.
go test ./browser ./jar -list '^TestNavigationReplay$|^TestHistoryReplayStorage$' > .build/replay-list.txt
grep -Fx 'TestNavigationReplay' .build/replay-list.txt >/dev/null
grep -Fx 'TestHistoryReplayStorage' .build/replay-list.txt >/dev/null
go test ./browser ./jar -run '^TestNavigationReplay$|^TestHistoryReplayStorage$' -count=1 -timeout=90s -json > .build/replay-target.jsonl
go run ./author_support/check_target.go .build/replay-target.jsonl
go test ./browser ./jar -run '^(TestRedirect|TestCookieHeader|TestTabInheritance|TestLinksWithBaseTag|TestBrowserFormDefaultNotSelected|TestBrowserFormSelected|TestBrowserFormDefaultsSelected|TestBrowserFormSelectMultiple|TestBrowserFormClickByValue|TestSubmitMultipart|TestMemoryHistory|TestMemoryHistoryWithMax|TestMemoryHistoryClear)$' -count=1 -timeout=90s -v
