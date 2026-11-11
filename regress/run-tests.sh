#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source regress/test-env.sh
mode="${1:-target}"
old='TestRunResponse|TestRunResponse_noResponse|TestQueryIframe|TestNavigateCancelled|TestNavigateWithoutWaitingForLoad'
case "$mode" in
 target)
  test -f frame_response_test.go || { echo 'missing frame_response_test.go' >&2; exit 1; }
  names="TestFrameResponseForm|TestFrameResponseIsolation|TestFrameResponseExitPaths|$old"
  ;;
 old) names="$old" ;;
 *) echo 'usage: bash regress/run-tests.sh [target|old]' >&2; exit 2 ;;
esac
CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go test -mod=readonly -c -o .userfb-build/chromedp.test .
docker run --rm --platform linux/arm64 --shm-size=256m --entrypoint /work/.userfb-build/chromedp.test --mount "type=bind,source=$PWD,target=/work" --workdir /work --env CHROMEDP_TEST_RUNNER=/headless-shell/headless-shell "$USERFB_IMAGE" -test.v -test.count=1 -test.parallel=1 -test.timeout=75s -test.run="^($names)$" | go tool test2json -t -p github.com/chromedp/chromedp | tee ".userfb-build/$mode-tests.jsonl"
go run -mod=readonly ./regress/testcheck "$mode" ".userfb-build/$mode-tests.jsonl"
