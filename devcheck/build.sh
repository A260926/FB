#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p devcheck/build
${CC:-cc} -std=gnu99 -fgnu89-inline -O2 -D__unused= -I common/include -I aacenc/inc -I aacenc/basic_op common/cmnMemory.c aacenc/basic_op/*.c aacenc/src/*.c "$1" -lm -o "$2"
