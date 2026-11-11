#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
node node_modules/typescript/bin/tsc --project author_support/tsconfig.json --noEmit
node author_support/run-tests.mjs test-page-pipeline.ts \
  'page pipeline overlap' 'page pipeline continuation' \
  'page pipeline stop' 'page pipeline errors'
sh author_support/old-tests.sh
