#!/bin/bash
set -euo pipefail
build=$(mktemp -d "${TMPDIR:-/tmp}/gifplan-baseline.XXXXXX")
trap 'rm -rf "$build"' EXIT
bash support/build.sh "$build"
"$build/run"
