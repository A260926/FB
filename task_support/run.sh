#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
exec node task_support/run.cjs "${1:-target}"
