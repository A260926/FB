#ifndef TABLES_H
#define TABLES_H

#include "types.h"

extern const int shine_slen1_tab[16];
extern const int shine_slen2_tab[16];

extern const int samplerates[9];
extern const int bitrates[16][4];

extern const int shine_scale_fact_band_index[9][23];

/* MPEG-1 short-window scalefactor band boundaries (frequency positions on a
 * 192-line scale, i.e. one window's worth of spectral lines). Indexed like
 * shine_scale_fact_band_index for the three MPEG-1 samplerates. */
extern const int shine_short_band_index[3][14];

extern const int32_t shine_enwindow[];

#endif
