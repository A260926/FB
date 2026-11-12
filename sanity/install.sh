#!/usr/bin/env bash
set -euo pipefail
cache=$(mktemp -d)
trap 'rm -rf "$cache"' EXIT
touch "$cache/user.npmrc" "$cache/global.npmrc"
npm ci --omit=dev --ignore-scripts --no-audit --no-fund --cache "$cache" --userconfig "$cache/user.npmrc" --globalconfig "$cache/global.npmrc"
