#!/bin/sh
set -eux
cd "$(dirname "$0")/.."
mkdir -p .build/obj
flags="-std=c99 -O2 -Idevcheck/include -Ilibtwolame "
for file in ath.c availbits.c bitbuffer.c crc.c dab.c encode.c energy.c fft.c get_set.c mem.c psycho_0.c psycho_1.c psycho_2.c psycho_3.c psycho_4.c psycho_n1.c subband.c twolame.c util.c; do
  "${CC:-cc}" $flags -c "libtwolame/$file" -o ".build/obj/${file%.c}.o"
done
ar rcs .build/libcodec.a .build/obj/*.o
"${CC:-cc}" $flags devcheck/baseline.c .build/libcodec.a -lm -o .build/baseline
