#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -f tests/vfr_2pass.c
sh devcheck/build.sh tests/vfr_2pass.c devcheck/build/vfr_2pass
devcheck/build/vfr_2pass
