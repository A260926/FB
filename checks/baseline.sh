#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .build
cc -std=c99 -DTIGR_HEADLESS -I. checks/probe.c tigr.c -lm -o .build/probe
.build/probe
cc -std=c99 -DTIGR_HEADLESS -I. -include tigr.h checks/probe.c src/tigr_bitmaps.c -lm -o .build/probe-modular
.build/probe-modular
