#!/bin/bash
set -euo pipefail
mkdir -p .task-build
# The original source and licenses remain intact. These are exact function
# ranges of pe.c at the seed commit; the optional hashing/unload paths are
# not compiled. No crypto/import/resource API is replaced with a stub.
{
  sed -n '1,20p' lib/libpe/pe.c
  printf '#include <libpe/pe.h>\n#include <stdlib.h>\n#include <string.h>\n#include <assert.h>\n'
  sed -n '37,41p;171,333p;401,437p;480,486p;523,587p' lib/libpe/pe.c
} > .task-build/pe_core.c
cc -std=c11 -D_DEFAULT_SOURCE -Ilib/libpe/include -c .task-build/pe_core.c -o .task-build/pe_core.o
