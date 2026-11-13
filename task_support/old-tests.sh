#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
.push-build/wvtest --default --short --no-extras --no-floats --no-lossy --no-speeds > .push-build/wvtest.stdout 2>&1
cat .push-build/wvtest.stdout
grep -q '^all tests pass$' .push-build/wvtest.stdout
