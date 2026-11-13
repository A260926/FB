#!/bin/sh
set -eu
node support/baseline.js
test -s exact-mesh.js
test -s test/exact-ao.js
node support/run-target.js
