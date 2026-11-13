#!/bin/bash
set -euo pipefail
build_case() {
 cc -std=c99 -O2 -Wno-pointer-to-int-cast -D_POSIX_C_SOURCE=200112L -DH264E_MAX_THREADS=4 -I. -Irefresh_support -Irefresh_support/vendor/h264bsd refresh_support/encoder.c refresh_support/vendor/h264bsd/*.c "$@" -lm -o "$b/case"
}
