#!/bin/sh
set -eu
node support/baseline.js
test -s terrain-renderer.js
test -s test/perspective.js
node support/run-target.js
