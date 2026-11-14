#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash baseline/build.sh
.task-build/baseline
