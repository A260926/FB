#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .build
cc -std=c99 -I. regress/probe.c schrift.c -lm -o .build/probe
.build/probe
