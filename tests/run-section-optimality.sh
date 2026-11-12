#!/bin/sh
# Convenience wrapper: the authoritative entry point is the fixed
# checks/run-section-target.sh, which compiles the real scalar library with
# tests/section_optimality.c and drives its registry.
set -eu
cd "$(dirname "$0")/.."
exec sh checks/run-section-target.sh "$@"
