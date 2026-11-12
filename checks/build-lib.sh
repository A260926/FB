#!/bin/bash
set -euo pipefail
build=$1
mkdir -p "$build/libexif"
cat > "$build/config.h" <<'CFG'
#define GETTEXT_PACKAGE "libexif"
#define LOCALEDIR ""
#define PACKAGE_VERSION "0.6.26.1"
#define HAVE_LOCALTIME_R 1
CFG
printf '#include <stdint.h>\n' > "$build/libexif/_stdint.h"
sources=(libexif/*.c libexif/canon/*.c libexif/fuji/*.c libexif/olympus/*.c libexif/pentax/*.c)
cc -std=c99 -D_DEFAULT_SOURCE -O1 -I"$build" -I. "${sources[@]}" "${@:2}" -lm -o "$build/program"
