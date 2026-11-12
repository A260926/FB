#!/bin/sh
set -eu
test -s sanity/composed-contract.test.cjs || { echo 'missing target: sanity/composed-contract.test.cjs' >&2; exit 1; }
sh sanity/build.sh
log=$(mktemp)
trap 'rm "$log"' EXIT
node --test --test-reporter=tap sanity/composed-contract.test.cjs >"$log" 2>&1 || { cat "$log"; exit 1; }
cat "$log"
grep -Eq '^# pass [1-9][0-9]*$' "$log"
if grep -Eq '^# (skipped|todo) [1-9]' "$log"; then echo "target tests contain skips or todo" >&2; exit 1; fi
grep -Eq '^ok [0-9]+ - composed-structure$' "$log"
grep -Eq '^ok [0-9]+ - composed-requery$' "$log"
NODE_PATH="$PWD/sanity/node_modules" sanity/node_modules/.bin/mocha --no-config --no-package --require should --require ./sanity/upstream-preload.cjs test/tests/textcontent-encode.js
