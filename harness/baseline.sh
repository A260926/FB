#!/bin/bash
set -euo pipefail
harness/node_modules/.bin/tsc -p harness/tsconfig.json
node harness/baseline.mjs
node harness/probe.mjs
