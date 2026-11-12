#!/bin/bash
set -euo pipefail
npm ci --omit=dev --ignore-scripts --userconfig=/dev/null --globalconfig="$PWD/harness/empty-npmrc" --cache "$PWD/.build/npm-cache" --no-audit --no-fund
npm ci --prefix harness --ignore-scripts --userconfig=/dev/null --globalconfig="$PWD/harness/empty-npmrc" --cache "$PWD/.build/npm-cache" --no-audit --no-fund
