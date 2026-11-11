#!/usr/bin/env bash
set -euo pipefail
npm --prefix author ci --omit=optional --ignore-scripts --no-audit --no-fund --cache "${TMPDIR:-/tmp}/userfb-z1165-npm-cache"
node -e "const assert=require('assert');assert.equal(require('./author/node_modules/av/package.json').version,'0.4.9');assert.equal(require('./author/node_modules/coffee-script/package.json').version,'1.12.7');console.log('DEPENDENCIES_LOCKED')"
test -s author/node_modules/av/src/decoder.js
test -s author/node_modules/av/src/core/bitstream.js
