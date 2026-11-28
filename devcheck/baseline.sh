#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
sh devcheck/build.sh devcheck/baseline.c devcheck/build/baseline
devcheck/build/baseline
