#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
case "${1:-baseline}" in
 baseline) exec node task_support/baseline.cjs ;;
 target) exec node task_support/target.cjs ;;
 *) exit 2 ;;
esac
