#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
cc -std=c99 -I tinyshader tinyshader/tinyshader_unity.c checks/prior_control.c -lm -o .task-build/prior_control
.task-build/prior_control
cc -std=c99 -I tinyshader tinyshader/tinyshader_unity.c checks/sampling_baseline.c -lm -o .task-build/sampling_baseline
.task-build/sampling_baseline
echo BASELINE_OK
