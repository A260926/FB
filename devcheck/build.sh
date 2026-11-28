#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p devcheck/build
sources=$(awk '/^SRC_GENERIC/{on=1;next}/^SRC_IA32/{on=0}on' xvidcore/build/generic/sources.inc | sed 's/\\//g' | awk 'NF {print "xvidcore/src/" $1}')
${CC:-cc} -std=gnu99 -O2 -DARCH_IS_GENERIC -DARCH_IS_64BIT -DHAVE_PTHREAD -I xvidcore/src $sources "$1" -lm -pthread -o "$2"
