#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source userfb/test-env.sh
CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -mod=readonly -o .userfb-build/witness ./userfb/witness
docker run --rm --platform linux/arm64 --shm-size=256m --entrypoint /work/.userfb-build/witness --mount "type=bind,source=$PWD,target=/work" --workdir /work --env CHROMEDP_TEST_RUNNER=/headless-shell/headless-shell "$USERFB_IMAGE"
