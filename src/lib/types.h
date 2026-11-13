#ifndef PRIV_TYPES_H
#define PRIV_TYPES_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <math.h>

/* Internal test-only observation switch (never installed). */
#include "l3obs.h"

#define GRANULE_SIZE 576

#include "bitstream.h"

/* Include arch-specific instructions,
 * when defined. */
#if defined(__mips__) && (__mips == 32)
#include "mult_mips_gcc.h"
#elif defined(__arm__) && !defined(__thumb__)
#include "mult_sarm_gcc.h"
#endif

/* Include and define generic instructions,
 * when not already defined above. */
#include "mult_noarch_gcc.h"

#ifndef SWAB32
#if defined(__GNUC__) &&                                                       \
    (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2))
#define SWAB32(x) __builtin_bswap32(x)
#else
#define SWAB32(x)                                                              \
  (((unsigned int)(x) >> 24) | (((unsigned int)(x) >> 8) & 0xff00) |           \
   (((unsigned int)(x)&0xff00) << 8) | ((unsigned int)(x) << 24))
#endif
#endif

/* #define DEBUG if you want the library to dump info to stdout */

#define PI 3.14159265358979
#define PI4 0.78539816339745
#define PI12 0.26179938779915
#define PI36 0.087266462599717
#define PI64 0.049087385212
#define SQRT2 1.41421356237
#define LN2 0.69314718
#define LN_TO_LOG10 0.2302585093
#define BLKSIZE 1024
#define HAN_SIZE 512 /* for loop unrolling, require that HAN_SIZE%8==0 */
#define SCALE_BLOCK 12
#define SCALE_RANGE 64
#define SCALE 32768
#define SBLIMIT 32

#ifndef MAX_CHANNELS
#define MAX_CHANNELS 2
#endif

#ifndef MAX_GRANULES
#define MAX_GRANULES 2
#endif

/* Short block geometry: three short windows, six coefficients each. */
#ifndef SHINE_SHORT_WINDOWS
#define SHINE_SHORT_WINDOWS 3
#endif
#define SHINE_SHORT_BANDS 13

typedef struct {
  int channels;
  int samplerate;
} priv_shine_wave_t;

typedef struct {
  int version;
  int layer;
  int granules_per_frame;
  int mode; /* + */ /* Stereo mode */
  int bitr; /* + */ /* Must conform to known bitrate - see Main.c */
  int emph; /* + */ /* De-emphasis */
  int padding;
  int bits_per_frame;
  int bits_per_slot;
  double frac_slots_per_frame;
  double slot_lag;
  int whole_slots_per_frame;
  int bitrate_index; /* + */    /* See Main.c and Layer3.c */
  int samplerate_index; /* + */ /* See Main.c and Layer3.c */
  int crc;
  int ext;
  int mode_ext;
  int copyright; /* + */
  int original;  /* + */
} priv_shine_mpeg_t;

typedef struct {
  int32_t *xr;                  /* magnitudes of the spectral values */
  int32_t xrsq[GRANULE_SIZE];   /* xr squared */
  int32_t xrabs[GRANULE_SIZE];  /* xr absolute */
  int32_t xrmax;                /* maximum of xrabs array */
  int32_t en_tot[MAX_GRANULES]; /* gr */
  int32_t en[MAX_GRANULES][21];
  int32_t xm[MAX_GRANULES][21];
  int32_t xrmaxl[MAX_GRANULES];
  double steptab[128];   /* 2**(-x/4)  for x = -127..0 */
  int32_t steptabi[128]; /* 2**(-x/4)  for x = -127..0 */
  int int2idx[10000];    /* x**(3/4)   for x = 0..9999 */
} l3loop_t;

typedef struct {
  int32_t cos_l[18][36];
  /* Start/stop transition windows folded into the 36-point long MDCT, and
   * the three 12-point short-window MDCT coefficients. */
  int32_t cos_start[18][36];
  int32_t cos_stop[18][36];
  int32_t cos_s[6][12];
} mdct_t;

typedef struct {
  int off[MAX_CHANNELS];
  int32_t fl[SBLIMIT][64];
  int32_t x[MAX_CHANNELS][HAN_SIZE];
} subband_t;

/* Side information */
typedef struct {
  unsigned part2_3_length;
  unsigned big_values;
  unsigned count1;
  unsigned global_gain;
  unsigned scalefac_compress;
  unsigned table_select[3];
  unsigned region0_count;
  unsigned region1_count;
  unsigned preflag;
  unsigned scalefac_scale;
  unsigned count1table_select;
  unsigned part2_length;
  unsigned sfb_lmax;
  unsigned address1;
  unsigned address2;
  unsigned address3;
  int quantizerStepSize;
  unsigned slen[4];
  /* Window switching (MPEG-1 block types 0..3). Only maintained on the
   * explicit block encoding path. */
  unsigned block_type;
  unsigned mixed_block_flag;
  int subblock_gain[SHINE_SHORT_WINDOWS];
} gr_info;

typedef struct {
  unsigned private_bits;
  int resvDrain;
  unsigned scfsi[MAX_CHANNELS][4];
  struct {
    struct {
      gr_info tt;
    } ch[MAX_CHANNELS];
  } gr[MAX_GRANULES];
} shine_side_info_t;

typedef struct {
  double l[MAX_GRANULES][MAX_CHANNELS][21];
} shine_psy_ratio_t;

typedef struct {
  double l[MAX_GRANULES][MAX_CHANNELS][21];
} shine_psy_xmin_t;

typedef struct {
  int32_t l[MAX_GRANULES][MAX_CHANNELS][22];    /* [cb] */
  int32_t s[MAX_GRANULES][MAX_CHANNELS][13][3]; /* [window][cb] */
} shine_scalefac_t;

typedef struct shine_global_flags {
  priv_shine_wave_t wave;
  priv_shine_mpeg_t mpeg;
  bitstream_t bs;
  shine_side_info_t side_info;
  int sideinfo_len;
  int mean_bits;
  shine_psy_ratio_t ratio;
  shine_scalefac_t scalefactor;
  int16_t *buffer[MAX_CHANNELS];
  double pe[MAX_CHANNELS][MAX_GRANULES];
  int l3_enc[MAX_CHANNELS][MAX_GRANULES][GRANULE_SIZE];
  int32_t l3_sb_sample[MAX_CHANNELS][MAX_GRANULES + 1][18][SBLIMIT];
  int32_t mdct_freq[MAX_CHANNELS][MAX_GRANULES][GRANULE_SIZE];
  int ResvSize;
  int ResvMax;
  l3loop_t l3loop;
  mdct_t mdct;
  subband_t subband;
  /* Explicit window switching state.
   * block_state: last committed block type per channel (starts at NORMAL).
   * frame_blocks: block plan accepted for the frame being encoded.
   * block_mode: 0 for the legacy long-only entries, 1 for the block entries.
   * l3_enc_natural: short-block quantized spectra in natural coordinates,
   * kept separately so the bitstream-ordered l3_enc only holds reordered
   * data. */
  int block_state[MAX_CHANNELS];
  int frame_blocks[MAX_GRANULES][MAX_CHANNELS];
  int block_mode;
  int l3_enc_natural[MAX_CHANNELS][MAX_GRANULES][GRANULE_SIZE];
#ifdef SHINE_TASK_OBSERVATIONS
  /* Read-only test observation points: the 36 subband samples feeding each
   * MDCT and the coefficients produced before aliasing reduction. They
   * exist only when the test build defines SHINE_TASK_OBSERVATIONS. */
  int32_t obs_mdct_in[MAX_CHANNELS][MAX_GRANULES][SBLIMIT][36];
  int32_t obs_mdct_pre_alias[MAX_CHANNELS][MAX_GRANULES][SBLIMIT][18];
  int obs_block_type[MAX_CHANNELS][MAX_GRANULES];
  int obs_valid;
  int obs_aliasing_applied[MAX_CHANNELS][MAX_GRANULES];
#endif
} shine_global_config;

#endif
