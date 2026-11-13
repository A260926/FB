#!/bin/sh
set -eu
mkdir -p .build
# Start from a clean archive: a stale object in .build would otherwise
# end up in libltc.a and break linking.
rm -f .build/*.o .build/libltc.a
for name in ltc encoder decoder timecode; do
 cc -std=c99 -O2 -Isrc -c "src/$name.c" -o ".build/$name.o"
done
# Include new production translation units as they are added, not only legacy files.
for path in src/*.c; do
 case "$path" in src/ltc.c|src/encoder.c|src/decoder.c|src/timecode.c) continue;; esac
 name=$(basename "$path" .c)
 cc -std=c99 -O2 -Isrc -c "$path" -o ".build/$name.o"
done
ar rcs .build/libltc.a .build/*.o
