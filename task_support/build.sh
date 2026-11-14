#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
sources=(cb_search exc_10_32_table exc_8_128_table filters gain_table hexc_table
 high_lsp_tables lsp ltp speex stereo vbr vq bits exc_10_16_table
 exc_20_32_table exc_5_256_table exc_5_64_table gain_table_lbr hexc_10_32_table
 lpc lsp_tables_nb modes modes_wb nb_celp quant_lsp sb_celp
 speex_callbacks speex_header window)
flags=(-std=c99 -O1 -fno-fast-math -ffp-contract=off -DFLOATING_POINT -DVAR_ARRAYS -DEXPORT=
 -Iinclude -Ilibspeex -Itask_support/include/speex -I.)
objects=()
for source in "${sources[@]}"; do
 "${CC:-cc}" "${flags[@]}" -c "libspeex/$source.c" -o ".task-build/$source.o"
 objects+=(".task-build/$source.o")
done
ar rcs .task-build/libspeex.a "${objects[@]}"
"${CC:-cc}" "${flags[@]}" task_support/baseline.c .task-build/libspeex.a -lm -o .task-build/baseline
