#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
case "${1:-target}" in
baseline) exec node task_support/baseline.mjs;;
target)
 test -s src/main/js/svg.js
 test -s tests/svg.test.mjs
 mkdir -p .task-build
 node --test --test-reporter=tap tests/svg.test.mjs > .task-build/target.tap 2>&1 || { cat .task-build/target.tap; exit 1; }
 cat .task-build/target.tap
 node --input-type=module - <<'JS'
import fs from 'node:fs';
import assert from 'node:assert/strict';
const log=fs.readFileSync('.task-build/target.tap','utf8');
for(const name of ['inline-wrap','ruby-layout','region-output'])
 assert.match(log,new RegExp('TASK_DONE '+name+' assertions [1-9][0-9]*'));
for(const name of ['fail','cancelled','skipped','todo']) assert.match(log,new RegExp('^# '+name+' 0$', 'm'));
assert.match(log,/^# tests [1-9][0-9]*$/m);
const api=await import('./src/main/js/main.js'); assert.equal(typeof api.renderSVG,'function');
JS
 ;;
*) exit 2;;
esac
