#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash support/build.sh "$build"
"$build/run"
