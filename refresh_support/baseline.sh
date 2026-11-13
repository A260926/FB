#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d .refresh-baseline.XXXXXX)
trap 'rm -rf "$b"' EXIT
source refresh_support/build.sh
build_case refresh_support/baseline.c
"$b/case"
