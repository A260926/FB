#!/usr/bin/env bash
set -euo pipefail
test -x .baseline-build/meshoptdemo
test -x .baseline-build/motion-baseline
.baseline-build/meshoptdemo
echo BASELINE_UPSTREAM_ASSERTIONS_OK
.baseline-build/motion-baseline
