#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash devcheck/build.sh
.build/baseline
