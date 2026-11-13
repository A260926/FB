#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
sh devcheck/build.sh
.build/baseline
