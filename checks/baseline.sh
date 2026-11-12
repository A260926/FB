#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c99 -O2 -I include -I checks -DZEPHYR_CONFIG_OVERRIDE=ffconf_override.h ff.c option/ffunicode.c checks/memdisk.c checks/baseline.c -o "$build/baseline"
"$build/baseline"
