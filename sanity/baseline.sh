#!/usr/bin/env bash
set -euo pipefail
node sanity/baseline.mjs
node --test test/test.js
echo 'BASELINE upstream JS suite PASS'
