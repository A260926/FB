#!/bin/bash
set -euo pipefail
mkdir -p .task-build/include/ogg .task-build/include/oggz .task-build/objects
cat > .task-build/include/config.h <<'CONFIG'
#define HAVE_STDINT_H 1
#define HAVE_UNISTD_H 1
#define OGGZ_CONFIG_READ 1
#define OGGZ_CONFIG_WRITE 1
#define OGG_H_CONST_CORRECT 1
#define VERSION "1.1.1"
CONFIG
cat > .task-build/include/ogg/config_types.h <<'TYPES'
#include <stdint.h>
typedef int16_t ogg_int16_t;
typedef uint16_t ogg_uint16_t;
typedef int32_t ogg_int32_t;
typedef uint32_t ogg_uint32_t;
typedef int64_t ogg_int64_t;
typedef uint64_t ogg_uint64_t;
TYPES
cat > .task-build/include/oggz/oggz_off_t_generated.h <<'OFF'
#include <sys/types.h>
typedef off_t oggz_off_t;
#define PRI_OGGZ_OFF_T "lld"
OFF
objects=()
for source_file in src/liboggz/*.c vendor/ogg/src/bitwise.c vendor/ogg/src/framing.c; do
  object_file=".task-build/objects/$(basename "$source_file" .c).o"
  cc -std=gnu11 -O1 -include unistd.h -I.task-build/include -Iinclude -Isrc/liboggz -Ivendor/ogg/include -c "$source_file" -o "$object_file"
  objects+=("$object_file")
done
ar rcs .task-build/liboggz-task.a "${objects[@]}"
cc -std=gnu11 -O1 -include unistd.h -I.task-build/include -Iinclude -Ivendor/ogg/include task_support/baseline.c .task-build/liboggz-task.a -lm -o .task-build/baseline
