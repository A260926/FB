#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -f tests/live_bitrate.c
sh devcheck/build.sh tests/live_bitrate.c devcheck/build/live_bitrate
devcheck/build/live_bitrate
