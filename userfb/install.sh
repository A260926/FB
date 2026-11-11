#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source userfb/test-env.sh
go version
go mod download
go mod verify
docker pull --platform linux/arm64 "$USERFB_IMAGE"
docker run --rm --platform linux/arm64 --entrypoint /headless-shell/headless-shell "$USERFB_IMAGE" --version
