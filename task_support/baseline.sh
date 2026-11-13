#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash task_support/build.sh baseline "$build"
"$build/baseline" "$@"
