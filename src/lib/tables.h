#ifndef TABLES_H
#define TABLES_H

#include "types.h"

extern const int shine_slen1_tab[16];
extern const int shine_slen2_tab[16];

extern const int samplerates[9];
extern const int bitrates[16][4];

extern const int shine_scale_fact_band_index[9][23];

/* MPEG-1 short-block scalefactor band boundaries: 13 bands per window,
 * frequency positions on a 192-line grid. Only the first three rows
 * (MPEG-1) are meaningful. */
extern const int shine_scale_fact_band_index_short[9][15];

extern const int32_t shine_enwindow[];

#endif
