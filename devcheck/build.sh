#!/bin/sh
set -eux
cd "$(dirname "$0")/.."
mkdir -p .build/obj
flags="-std=c99 -O2 -Idevcheck/include -Iinclude -Ilibspeex -DHAVE_CONFIG_H"
for file in cb_search.c exc_10_32_table.c exc_8_128_table.c filters.c gain_table.c hexc_table.c high_lsp_tables.c lsp.c ltp.c speex.c stereo.c vbr.c vq.c bits.c exc_10_16_table.c exc_20_32_table.c exc_5_256_table.c exc_5_64_table.c gain_table_lbr.c hexc_10_32_table.c lpc.c lsp_tables_nb.c modes.c modes_wb.c nb_celp.c quant_lsp.c sb_celp.c speex_callbacks.c speex_header.c window.c; do
  "${CC:-cc}" $flags -c "libspeex/$file" -o ".build/obj/${file%.c}.o"
done
ar rcs .build/libcodec.a .build/obj/*.o
"${CC:-cc}" $flags devcheck/baseline.c .build/libcodec.a -lm -o .build/baseline
