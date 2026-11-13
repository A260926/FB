#!/bin/bash
set -euo pipefail
# Used by baseline and target; output directory is always supplied by the caller.
b=$1
shift
src=(mdct smallft block envelope window lsp lpc analysis synthesis psy info floor1 floor0 res0 mapping0 registry codebook sharedbook lookup bitrate vorbisenc)
objects=()
flags=(-O2)
if [[ ${TRACE:-0} == 1 ]]; then flags+=(-DVORBIS_COUPLING_TRACE); fi
for f in "${src[@]}"; do
 source="lib/$f.c"
 if [[ "$f" == mapping0 && -n "${MAPPING_SOURCE:-}" ]]; then source=$MAPPING_SOURCE; fi
 cc -std=c99 -O2 "${flags[@]}" -DHAVE_ALLOCA_H -Iinclude -Ilib -Itask_support/vendor/ogg/include -c "${source}" -o "$b/$f.o"
 objects+=("$b/$f.o")
done
cc -std=c99 -O2 -Itask_support/vendor/ogg/include -c task_support/vendor/ogg/src/bitwise.c -o "$b/bitwise.o"
cc -std=c99 -O2 -Itask_support/vendor/ogg/include -c task_support/vendor/ogg/src/framing.c -o "$b/framing.o"
ar rcs "$b/libcodec.a" "${objects[@]}" "$b/bitwise.o" "$b/framing.o"
