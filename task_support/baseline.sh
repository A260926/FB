#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
bash task_support/build.sh "$work/baseline" task_support/baseline.c
"$work/baseline"
bash task_support/build.sh "$work/libcheck" tests/libcheck.c
"$work/libcheck"
echo UDIS_LEGACY_LIBCHECK_OK
