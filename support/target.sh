#!/bin/sh
set -eu
test -s test/id3-emsg-node.test.js
node --test test/id3-emsg-node.test.js
node support/baseline.js
