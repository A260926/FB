/* l3bitstrea.c */

#include "l3bitstream.h" /* the public interface */
#include "bitstream.h"
#include "huffman.h"
#include "l3loop.h"
#include "l3mdct.h"
#include "layer3.h"
#include "tables.h"
#include "types.h"

static void shine_HuffmanCode(bitstream_t *bs, int table_select, int x, int y);
static void shine_huffman_coder_count1(bitstream_t *bs,
                                       const struct huffcodetab *h, int v,
                                       int w, int x, int y);

static void encodeSideInfo(shine_global_config *config, bitstream_t *out,
                           unsigned int *crc);
static void encodeProtectedFrame(shine_global_config *config);
static void encodeMainData(shine_global_config *config);
static void Huffmancodebits(shine_global_config *config, int *ix, gr_info *gi);

/*
  shine_format_bitstream()

  This is called after a frame of audio has been quantized and coded.
  It will write the encoded audio to the bitstream. Note that
  from a layer3 encoder's perspective the bit stream is primarily
  a series of main_data() blocks, with header and side information
  inserted at the proper locations to maintain framing. (See Figure A.7
  in the IS).
*/

void shine_format_bitstream(shine_global_config *config) {
  int gr, ch, i;

  for (ch = 0; ch < config->wave.channels; ch++)
    for (gr = 0; gr < config->mpeg.granules_per_frame; gr++) {
      int *pi = &config->l3_enc[ch][gr][0];
      int32_t *pr = &config->mdct_freq[ch][gr][0];
      for (i = 0; i < GRANULE_SIZE; i++) {
        if ((pr[i] < 0) && (pi[i] > 0))
          pi[i] *= -1;
      }
    }

  if (config->mpeg.crc)
    encodeProtectedFrame(config);
  else {
    encodeSideInfo(config, &config->bs, NULL);
    encodeMainData(config);
  }
}

/*
 * Emit one field into out and, when crc is not NULL, fold the same bits
 * into the Layer III checksum in their transmitted (MSB-first) order.
 */
static void putCrcField(bitstream_t *out, unsigned int *crc, unsigned int val,
                        unsigned int N, int covered) {
  shine_putbits(out, val, N);
  if (covered && crc != NULL)
    *crc = shine_crc16(*crc, val, N);
}

/*
 * Write the 32-bit frame header followed by the side information. When crc
 * is NULL the fields go straight to the output stream, as in the
 * unprotected path. Otherwise they are assembled in the encoder's scratch
 * stream and every covered field (the header's last 16 bits and all side
 * information) is simultaneously fed to an independent CRC accumulator.
 * The 11 sync bits, version, layer and protection bit are never covered.
 */
static void encodeSideInfo(shine_global_config *config, bitstream_t *out,
                           unsigned int *crc) {
  int gr, ch, scfsi_band, region;
  shine_side_info_t si = config->side_info;

  putCrcField(out, crc, 0x7ff, 11, 0);
  putCrcField(out, crc, config->mpeg.version, 2, 0);
  putCrcField(out, crc, config->mpeg.layer, 2, 0);
  putCrcField(out, crc, !config->mpeg.crc, 1, 0);
  /* Everything from here on is covered by the Layer III CRC. */
  putCrcField(out, crc, config->mpeg.bitrate_index, 4, 1);
  putCrcField(out, crc, config->mpeg.samplerate_index % 3, 2, 1);
  putCrcField(out, crc, config->mpeg.padding, 1, 1);
  putCrcField(out, crc, config->mpeg.ext, 1, 1);
  putCrcField(out, crc, config->mpeg.mode, 2, 1);
  putCrcField(out, crc, config->mpeg.mode_ext, 2, 1);
  putCrcField(out, crc, config->mpeg.copyright, 1, 1);
  putCrcField(out, crc, config->mpeg.original, 1, 1);
  putCrcField(out, crc, config->mpeg.emph, 2, 1);

  if (config->mpeg.version == MPEG_I) {
    putCrcField(out, crc, 0, 9, 1);
    if (config->wave.channels == 2)
      putCrcField(out, crc, si.private_bits, 3, 1);
    else
      putCrcField(out, crc, si.private_bits, 5, 1);
  } else {
    putCrcField(out, crc, 0, 8, 1);
    if (config->wave.channels == 2)
      putCrcField(out, crc, si.private_bits, 2, 1);
    else
      putCrcField(out, crc, si.private_bits, 1, 1);
  }

  if (config->mpeg.version == MPEG_I)
    for (ch = 0; ch < config->wave.channels; ch++) {
      for (scfsi_band = 0; scfsi_band < 4; scfsi_band++)
        putCrcField(out, crc, si.scfsi[ch][scfsi_band], 1, 1);
    }

  for (gr = 0; gr < config->mpeg.granules_per_frame; gr++)
    for (ch = 0; ch < config->wave.channels; ch++) {
      gr_info *gi = &(si.gr[gr].ch[ch].tt);

      putCrcField(out, crc, gi->part2_3_length, 12, 1);
      putCrcField(out, crc, gi->big_values, 9, 1);
      putCrcField(out, crc, gi->global_gain, 8, 1);
      if (config->mpeg.version == MPEG_I)
        putCrcField(out, crc, gi->scalefac_compress, 4, 1);
      else
        putCrcField(out, crc, gi->scalefac_compress, 9, 1);
      putCrcField(out, crc, 0, 1, 1);

      for (region = 0; region < 3; region++)
        putCrcField(out, crc, gi->table_select[region], 5, 1);

      putCrcField(out, crc, gi->region0_count, 4, 1);
      putCrcField(out, crc, gi->region1_count, 3, 1);

      if (config->mpeg.version == MPEG_I)
        putCrcField(out, crc, gi->preflag, 1, 1);
      putCrcField(out, crc, gi->scalefac_scale, 1, 1);
      putCrcField(out, crc, gi->count1table_select, 1, 1);
    }
}

/*
 * Assemble a CRC-protected frame into the output bit stream.
 *
 * The full header and the side information are first built in a private
 * scratch stream, which lets the 16-bit checksum be computed before anything
 * is committed. The on-wire layout is:
 *
 *   4-byte header | CRC (16 bits, big endian) | side information | main data
 *
 * The CRC covers only the header's last 16 bits (scratch bytes 2-3) and the
 * side information. The checksum word itself, the sync/version/layer bits and
 * the main data are excluded. Main data and ancillary data are written
 * afterwards by the shared, unchanged encoder.
 */
static void encodeProtectedFrame(shine_global_config *config) {
  bitstream_t *scratch = config->crc_bs;
  bitstream_t *out = &config->bs;
  unsigned int crc = 0xffff;
  int total_bytes, i;

  encodeSideInfo(config, scratch, &crc);

  /* Header (4 bytes) plus side information is byte aligned. */
  shine_drain_bit_stream(scratch);
  total_bytes = scratch->data_position;

  /* The complete, contiguous four-byte frame header goes first. */
  for (i = 0; i < 4; i++)
    shine_putbits(out, scratch->data[i], 8);
  /* Checksum immediately follows the four-byte header. */
  shine_putbits(out, crc, 16);
  /* Then the side information, unchanged from the scratch assembly. */
  for (i = 4; i < total_bytes; i++)
    shine_putbits(out, scratch->data[i], 8);

  /* Rewind the scratch stream for the next frame. */
  scratch->data_position = 0;
  scratch->cache = 0;
  scratch->cache_bits = 32;

  encodeMainData(config);
}

static void encodeMainData(shine_global_config *config) {
  int gr, ch, sfb;
  shine_side_info_t si = config->side_info;

  for (gr = 0; gr < config->mpeg.granules_per_frame; gr++) {
    for (ch = 0; ch < config->wave.channels; ch++) {
      gr_info *gi = &(si.gr[gr].ch[ch].tt);
      unsigned slen1 = shine_slen1_tab[gi->scalefac_compress];
      unsigned slen2 = shine_slen2_tab[gi->scalefac_compress];
      int *ix = &config->l3_enc[ch][gr][0];

      if (gr == 0 || si.scfsi[ch][0] == 0)
        for (sfb = 0; sfb < 6; sfb++)
          shine_putbits(&config->bs, config->scalefactor.l[gr][ch][sfb], slen1);
      if (gr == 0 || si.scfsi[ch][1] == 0)
        for (sfb = 6; sfb < 11; sfb++)
          shine_putbits(&config->bs, config->scalefactor.l[gr][ch][sfb], slen1);
      if (gr == 0 || si.scfsi[ch][2] == 0)
        for (sfb = 11; sfb < 16; sfb++)
          shine_putbits(&config->bs, config->scalefactor.l[gr][ch][sfb], slen2);
      if (gr == 0 || si.scfsi[ch][3] == 0)
        for (sfb = 16; sfb < 21; sfb++)
          shine_putbits(&config->bs, config->scalefactor.l[gr][ch][sfb], slen2);

      Huffmancodebits(config, ix, gi);
    }
  }
}

/* Note the discussion of huffmancodebits() on pages 28 and 29 of the IS, as
  well as the definitions of the side information on pages 26 and 27. */
static void Huffmancodebits(shine_global_config *config, int *ix, gr_info *gi) {
  const int *scalefac =
      &shine_scale_fact_band_index[config->mpeg.samplerate_index][0];
  unsigned scalefac_index;
  int region1Start, region2Start;
  int i, bigvalues, count1End;
  int v, w, x, y;
  const struct huffcodetab *h;
  int bits;

  bits = shine_get_bits_count(&config->bs);

  /* 1: Write the bigvalues */
  bigvalues = gi->big_values << 1;

  scalefac_index = gi->region0_count + 1;
  region1Start = scalefac[scalefac_index];
  scalefac_index += gi->region1_count + 1;
  region2Start = scalefac[scalefac_index];

  for (i = 0; i < bigvalues; i += 2) {
    /* get table pointer */
    int idx = (i >= region1Start) + (i >= region2Start);
    unsigned tableindex = gi->table_select[idx];
    /* get huffman code */
    if (tableindex) {
      x = ix[i];
      y = ix[i + 1];
      shine_HuffmanCode(&config->bs, tableindex, x, y);
    }
  }

  /* 2: Write count1 area */
  h = &shine_huffman_table[gi->count1table_select + 32];
  count1End = bigvalues + (gi->count1 << 2);
  for (i = bigvalues; i < count1End; i += 4) {
    v = ix[i];
    w = ix[i + 1];
    x = ix[i + 2];
    y = ix[i + 3];
    shine_huffman_coder_count1(&config->bs, h, v, w, x, y);
  }

  bits = shine_get_bits_count(&config->bs) - bits;
  bits = gi->part2_3_length - gi->part2_length - bits;
  if (bits) {
    int stuffingWords = bits / 32;
    int remainingBits = bits % 32;

    /* Due to the nature of the Huffman code tables, we will pad with ones */
    while (stuffingWords--)
      shine_putbits(&config->bs, ~0, 32);
    if (remainingBits)
      shine_putbits(&config->bs, (1UL << remainingBits) - 1, remainingBits);
  }
}

static inline int shine_abs_and_sign(int *x) {
  if (*x > 0)
    return 0;
  *x *= -1;
  return 1;
}

static void shine_huffman_coder_count1(bitstream_t *bs,
                                       const struct huffcodetab *h, int v,
                                       int w, int x, int y) {
  unsigned int signv, signw, signx, signy;
  unsigned int code = 0;
  int p, cbits = 0;

  signv = shine_abs_and_sign(&v);
  signw = shine_abs_and_sign(&w);
  signx = shine_abs_and_sign(&x);
  signy = shine_abs_and_sign(&y);

  p = v + (w << 1) + (x << 2) + (y << 3);
  shine_putbits(bs, h->table[p], h->hlen[p]);

  if (v) {
    code = signv;
    cbits = 1;
  }
  if (w) {
    code = (code << 1) | signw;
    cbits++;
  }
  if (x) {
    code = (code << 1) | signx;
    cbits++;
  }
  if (y) {
    code = (code << 1) | signy;
    cbits++;
  }
  shine_putbits(bs, code, cbits);
}

/* Implements the pseudocode of page 98 of the IS */
static void shine_HuffmanCode(bitstream_t *bs, int table_select, int x, int y) {
  int cbits = 0, xbits = 0;
  unsigned int code = 0, ext = 0;
  unsigned signx, signy, ylen, idx;
  const struct huffcodetab *h;

  signx = shine_abs_and_sign(&x);
  signy = shine_abs_and_sign(&y);

  h = &(shine_huffman_table[table_select]);
  ylen = h->ylen;

  if (table_select > 15) { /* ESC-table is used */
    unsigned linbitsx = 0, linbitsy = 0, linbits = h->linbits;

    if (x > 14) {
      linbitsx = x - 15;
      x = 15;
    }
    if (y > 14) {
      linbitsy = y - 15;
      y = 15;
    }

    idx = (x * ylen) + y;
    code = h->table[idx];
    cbits = h->hlen[idx];
    if (x > 14) {
      ext |= linbitsx;
      xbits += linbits;
    }
    if (x != 0) {
      ext <<= 1;
      ext |= signx;
      xbits += 1;
    }
    if (y > 14) {
      ext <<= linbits;
      ext |= linbitsy;
      xbits += linbits;
    }
    if (y != 0) {
      ext <<= 1;
      ext |= signy;
      xbits += 1;
    }

    shine_putbits(bs, code, cbits);
    shine_putbits(bs, ext, xbits);
  } else { /* No ESC-words */
    idx = (x * ylen) + y;
    code = h->table[idx];
    cbits = h->hlen[idx];
    if (x != 0) {
      code <<= 1;
      code |= signx;
      cbits += 1;
    }
    if (y != 0) {
      code <<= 1;
      code |= signy;
      cbits += 1;
    }

    shine_putbits(bs, code, cbits);
  }
}
