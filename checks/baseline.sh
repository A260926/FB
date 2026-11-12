#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/rn-base.XXXXXX")
trap 'rm -rf "$b"' EXIT
source checks/build.sh
cc -std=c99 -O1 -Iinclude checks/baseline.c "$b"/*.o -lm -o "$b/base"
"$b/base"
