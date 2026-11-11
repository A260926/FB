#!/usr/bin/env bash
set -euo pipefail
bash -e -o pipefail -c 'test -f tests/regress/domxss-flow.test.cjs; test -f tools/check-domxss-mutants.cjs; t=$(mktemp); node --test --test-reporter=tap tests/regress/domxss-flow.test.cjs | tee "$t"; for n in binding-writes expression-graph sink-config; do grep -Eq "^ok [0-9]+ - $n" "$t"; done; grep -q "# fail 0" "$t"; grep -q "# skipped 0" "$t"; node tools/check-domxss-mutants.cjs; baseline/node_modules/.bin/mocha tests/rules/property.js tests/rules/method.js --reporter dot'
