/*
 * crc_frames.c - independent verification of Shine's Layer III CRC frames.
 *
 * This test only uses the public encoder API (layer3.h). The MP3 frames are
 * located through their sync words and parsed with a private bit reader, and
 * the Layer III checksum is recomputed bit by bit from the actual output
 * bytes. The production CRC helper in bitstream.c is deliberately never
 * called here, so a shared implementation bug cannot validate itself.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "layer3.h"

/* ------------------------------------------------------------------ */
/* Independent bit reader over an assembled byte stream.              */
/* ------------------------------------------------------------------ */

typedef struct {
  const unsigned char *data;
  size_t pos; /* bit position, MSB of each byte first */
} bit_reader_t;

static unsigned br_read(bit_reader_t *br, unsigned n) {
  unsigned v = 0;
  while (n--) {
    unsigned bit = (br->data[br->pos >> 3] >> (7 - (br->pos & 7))) & 1u;
    br->pos++;
    v = (v << 1) | bit;
  }
  return v;
}

/*
 * Independent Layer III CRC-16: 0xffff initial value, MSB first, polynomial
 * 0x8005, no final exclusive-or. Recomputed from the raw bytes, one bit at a
 * time, exactly as ISO 11172-3 describes the decoder check. Pass the current
 * register value (start at 0xffff) so several regions can be covered in
 * order.
 */
static uint16_t independent_crc(uint16_t crc, const unsigned char *data,
                                size_t byte_start, size_t nbytes) {
  size_t b;
  for (b = 0; b < nbytes; b++) {
    unsigned val = data[byte_start + b];
    int k;
    for (k = 7; k >= 0; k--) {
      unsigned bit = (val >> k) & 1u;
      unsigned msb = (crc >> 15) & 1u;
      crc = (uint16_t)((crc << 1) & 0xffffu);
      if (msb ^ bit)
        crc ^= 0x8005u;
    }
  }
  return crc;
}

/* ------------------------------------------------------------------ */
/* Header tables, duplicated from the standard for test independence. */
/* ------------------------------------------------------------------ */

static const int k_samplerates[4][3] = {
    /* version 2.5 */ {11025, 12000, 8000},
    /* reserved */ {0, 0, 0},
    /* version II */ {22050, 24000, 16000},
    /* version I */ {44100, 48000, 32000},
};

static const int k_bitrates[4][16] = {
    /* 2.5 */ {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
    /* reserved */ {0},
    /* II */ {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
    /* I */ {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},
};

typedef struct {
  int frames;
  int pad0;
  int pad1;
  int channels;
  int spf;
} scenario_result_t;

/* Side information length in bytes for a given MPEG version/channel count. */
static int sideinfo_bytes(int version, int channels) {
  if (version == MPEG_I)
    return channels == 1 ? 17 : 32;
  return channels == 1 ? 9 : 17;
}

/* Parse and independently validate every frame of one protected stream. */
static void verify_stream(const unsigned char *stream, size_t length,
                          int version_expect, scenario_result_t *res) {
  size_t off = 0;
  int frame = 0;

  memset(res, 0, sizeof(*res));

  while (off + 4 <= length) {
    /* Locate the frame through its header sync word. */
    assert(stream[off] == 0xff);
    assert((stream[off + 1] & 0xe0) == 0xe0);

    unsigned version = (stream[off + 1] >> 3) & 3;
    unsigned layer = (stream[off + 1] >> 1) & 3;
    unsigned protection = stream[off + 1] & 1;
    assert(version == (unsigned)version_expect);
    assert(layer == LAYER_III);
    assert(protection == 0); /* every protected frame clears this bit */

    /* The contiguous four-byte header is followed by the two CRC bytes;
     * side information starts at +6. */
    unsigned h2 = stream[off + 2];
    unsigned h3 = stream[off + 3];
    uint16_t stored_crc =
        (uint16_t)((stream[off + 4] << 8) | stream[off + 5]);
    unsigned br_idx = h2 >> 4;
    unsigned sr_idx = (h2 >> 2) & 3;
    unsigned padding = (h2 >> 1) & 1;
    unsigned mode = (h3 >> 6) & 3;
    int channels = (mode == MONO) ? 1 : 2;

    int samplerate = k_samplerates[version][sr_idx];
    int bitrate = k_bitrates[version][br_idx];
    assert(samplerate > 0 && bitrate > 0);

    int flen = (version == MPEG_I ? 144 : 72) * bitrate * 1000 / samplerate +
               (int)padding;
    assert(off + (size_t)flen <= length);

    int sib = sideinfo_bytes(version, channels);
    size_t side_start = off + 6;            /* after 4 header + 2 CRC bytes */
    size_t main_start = side_start + sib;
    assert(main_start <= off + (size_t)flen);

    /* Independently recompute the CRC over the header's last 16 bits
     * (bytes +2/+3) and the whole side information; the sync/version bits,
     * the CRC word itself and the main data are excluded. */
    uint16_t crc = independent_crc(0xffff, stream, off + 2, 2);
    crc = independent_crc(crc, stream, side_start, sib);
    assert(crc == stored_crc);

    /* Parse the side information fields the task requires. */
    bit_reader_t br = {stream, side_start * 8};
    unsigned main_data_begin = br_read(&br, version == MPEG_I ? 9 : 8);
    assert(main_data_begin == 0); /* no reservoir back-references */
    if (version == MPEG_I)
      br_read(&br, channels == 2 ? 3 : 5); /* private bits */
    else
      br_read(&br, channels == 2 ? 2 : 1);
    if (version == MPEG_I)
      br_read(&br, 4u * channels); /* scfsi bands */

    int granules = (version == MPEG_I) ? 2 : 1;
    unsigned p23_sum = 0;
    int gr, ch;
    for (gr = 0; gr < granules; gr++)
      for (ch = 0; ch < channels; ch++) {
        unsigned part2_3_length = br_read(&br, 12);
        assert(part2_3_length <= 4095);
        p23_sum += part2_3_length;
        /* Consume the remaining granule/channel side-info fields. */
        br_read(&br, 9);  /* big_values */
        br_read(&br, 8);  /* global_gain */
        br_read(&br, version == MPEG_I ? 4 : 9); /* scalefac_compress */
        br_read(&br, 1);  /* window switching */
        br_read(&br, 15); /* three table_select fields */
        br_read(&br, 4);  /* region0_count */
        br_read(&br, 3);  /* region1_count */
        if (version == MPEG_I)
          br_read(&br, 1); /* preflag */
        br_read(&br, 1);    /* scalefac_scale */
        br_read(&br, 1);    /* count1table_select */
      }

    /* The Huffman/scalefactor output must fit the remaining main-data
     * capacity; it cannot be made consistent by lying in the header. */
    size_t main_data_bytes = (size_t)flen - 6 - (size_t)sib;
    assert(p23_sum <= (unsigned)(main_data_bytes * 8));

    /* Side-info parsing must consume exactly the declared side-info width. */
    assert(br.pos == main_start * 8);

    if (padding)
      res->pad1++;
    else
      res->pad0++;
    res->channels = channels;
    res->spf = (version == MPEG_I) ? 1152 : 576;

    off += (size_t)flen;
    frame++;
  }

  /* Every byte belongs to a frame, the final one included. */
  assert(off == length);
  res->frames = frame;
  assert(frame > 0);
}

/* ------------------------------------------------------------------ */
/* Encoding helpers.                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
  unsigned char *data;
  size_t len;
  size_t cap;
} assembled_t;

static void append(assembled_t *a, const unsigned char *p, int n) {
  if (a->len + (size_t)n > a->cap) {
    a->cap = (a->len + n) * 2;
    a->data = realloc(a->data, a->cap);
    assert(a->data);
  }
  memcpy(a->data + a->len, p, n);
  a->len += n;
}

static void fill_pcm(int16_t *l, int16_t *r, int spp, int frame) {
  int i;
  for (i = 0; i < spp; i++) {
    /* Deterministic, non-zero, and deliberately different per channel. */
    l[i] = (int16_t)(((i * 37 + frame * 101 + 7) % 4001) - 2000);
    r[i] = (int16_t)(((i * 53 + frame * 71 + 13) % 3001) - 1500);
    if (l[i] == 0)
      l[i] = 123;
    if (r[i] == 0)
      r[i] = -321;
  }
}

/* Encode a full stream with a freshly initialised encoder. */
static assembled_t encode_stream(const shine_config_t *cfg, int crc_enabled,
                                 int use_interleaved, int nframes) {
  shine_t s = crc_enabled < 0 ? shine_initialise((shine_config_t *)cfg)
                              : shine_initialise_crc((shine_config_t *)cfg,
                                                     crc_enabled);
  assert(s);
  int spp = shine_samples_per_pass(s);
  int channels = cfg->wave.channels;
  int16_t l[1152], r[1152], inter[2304];
  int16_t *planes[2] = {l, r};
  assembled_t out = {0};
  int f, written;
  unsigned char *p;

  for (f = 0; f < nframes; f++) {
    fill_pcm(l, r, spp, f);
    if (use_interleaved) {
      int i;
      for (i = 0; i < spp; i++)
        if (channels == 2) {
          inter[2 * i] = l[i];
          inter[2 * i + 1] = r[i];
        } else {
          inter[i] = l[i];
        }
      p = shine_encode_buffer_interleaved(s, inter, &written);
    } else {
      p = shine_encode_buffer(s, planes, &written);
    }
    append(&out, p, written);
  }
  p = shine_flush(s, &written);
  append(&out, p, written);
  shine_close(s);
  return out;
}

static int expect_version(int samplerate) {
  if (samplerate >= 32000)
    return MPEG_I;
  if (samplerate >= 16000)
    return MPEG_II;
  return MPEG_25;
}

static shine_config_t make_config(int samplerate, int bitrate, int channels) {
  shine_config_t c;
  memset(&c, 0, sizeof(c));
  c.wave.channels = channels;
  c.wave.samplerate = samplerate;
  shine_set_config_mpeg_defaults(&c.mpeg);
  c.mpeg.bitr = bitrate;
  c.mpeg.mode = (channels == 1) ? MONO : STEREO;
  return c;
}

int main(int argc, char **argv) {
  const char *outdir = (argc > 1) ? argv[1] : ".";
  /* At 44.1 kHz the frame length averages 417.96 bytes, so roughly one frame
   * in 24 is unpadded; encode enough frames to observe both padding values. */
  int nframes = 30;

  struct {
    const char *name;
    int samplerate, bitrate, channels;
  } cases[] = {
      {"mpeg1_44100_stereo", 44100, 128, 2},
      {"mpeg1_48000_stereo", 48000, 128, 2},
      {"mpeg2_22050_stereo", 22050, 64, 2},
      {"mpeg25_8000_mono", 8000, 32, 1},
      {"mpeg1_44100_mono", 44100, 32, 1},
  };
  int nc = (int)(sizeof(cases) / sizeof(cases[0]));
  int i;

  for (i = 0; i < nc; i++) {
    shine_config_t cfg =
        make_config(cases[i].samplerate, cases[i].bitrate, cases[i].channels);
    int version = expect_version(cases[i].samplerate);

    assembled_t planar = encode_stream(&cfg, 1, 0, nframes);
    assembled_t interleaved = encode_stream(&cfg, 1, 1, nframes);

    /* Dual entry point: identical PCM and initial state must match. */
    assert(planar.len == interleaved.len);
    assert(!memcmp(planar.data, interleaved.data, planar.len));
    printf("DUAL_ENTRY %s planar=interleaved bytes=%zu OK\n", cases[i].name,
           planar.len);

    scenario_result_t res;
    verify_stream(planar.data, planar.len, version, &res);
    printf("SCENARIO %s frames=%d padding0=%d padding1=%d channels=%d\n",
           cases[i].name, res.frames, res.pad0, res.pad1, res.channels);
    assert(res.frames == nframes);
    if (cases[i].samplerate == 44100) {
      /* Every 44.1 kHz stream here must really exhibit both padding slots.
       * The 128 kbps stereo stream first goes unpadded around frame 23;
       * the 32 kbps mono cadence alternates almost every frame. */
      assert(res.pad0 > 0 && res.pad1 > 0);
    }

    free(interleaved.data);

    /* Persist a couple of protected streams for the external FFmpeg check. */
    if (strcmp(cases[i].name, "mpeg1_44100_stereo") == 0 ||
        strcmp(cases[i].name, "mpeg25_8000_mono") == 0) {
      char path[512];
      snprintf(path, sizeof(path), "%s/%s_protected.mp3", outdir,
               cases[i].name);
      FILE *fp = fopen(path, "wb");
      assert(fp);
      fwrite(planar.data, 1, planar.len, fp);
      fclose(fp);
      printf("FFCASE %s %d %d %d\n", path, res.frames, res.channels, res.spf);
    }
    free(planar.data);
  }

  /* Byte-for-byte comparison: default entry vs new entry with CRC disabled. */
  {
    shine_config_t cfg = make_config(44100, 128, 2);
    assembled_t legacy = encode_stream(&cfg, -1, 0, nframes);
    assembled_t disabled = encode_stream(&cfg, 0, 0, nframes);
    assert(legacy.len == disabled.len);
    assert(!memcmp(legacy.data, disabled.data, legacy.len));
    printf("BYTE_COMPARE default_vs_crc0 bytes=%zu identical OK\n",
           legacy.len);
    free(legacy.data);
    free(disabled.data);
  }

  /* Every legal samplerate/bitrate combination must still encode protected
   * frames for both mono and stereo, and each frame must parse cleanly.
   *
   * The MPEG spec hard-caps part2_3_length at 4095 bits per granule and
   * channel, so a handful of extreme high-bitrate MONO configurations demand
   * more main-data bits than any compliant frame can hold (the original
   * encoder underfills them too). Those are an MPEG limitation, not an
   * encoder regression, and are skipped explicitly below. */
  {
    static const int rates[] = {44100, 48000, 32000, 22050, 24000, 16000,
                                11025, 12000, 8000};
    static const int all_bitrates[] = {8,  16, 24, 32, 40, 48,  56,  64, 80,
                                       96, 112, 128, 144, 160, 192, 224, 256,
                                       320};
    int combos = 0, spec_capped = 0, ri, bi, ch;
    for (ri = 0; ri < (int)(sizeof(rates) / sizeof(rates[0])); ri++)
      for (bi = 0; bi < (int)(sizeof(all_bitrates) / sizeof(all_bitrates[0]));
           bi++)
        for (ch = 1; ch <= 2; ch++) {
          int rate = rates[ri], bitr = all_bitrates[bi];
          if (shine_check_config(rate, bitr) < 0)
            continue; /* unsupported pair for this version: skip it */

          int version = expect_version(rate);
          int granules = (version == MPEG_I) ? 2 : 1;
          int sideinfo_bits =
              (version == MPEG_I) ? (ch == 1 ? 8 * 21 : 8 * 36)
                                  : (ch == 1 ? 8 * 13 : 8 * 21);
          int whole_slots =
              (version == MPEG_I ? 144 : 72) * bitr * 1000 / rate;
          int main_capacity_bits =
              whole_slots * 8 - sideinfo_bits - 16 /* CRC */;
          int part23_cap = granules * ch * 4095;

          if (main_capacity_bits > part23_cap) {
            /* Only mono can exceed the per-granule 4095-bit ceiling. */
            assert(ch == 1);
            spec_capped++;
            continue;
          }

          shine_config_t cfg = make_config(rate, bitr, ch);
          assembled_t st = encode_stream(&cfg, 1, 0, 4);
          scenario_result_t r;
          verify_stream(st.data, st.len, version, &r);
          assert(r.frames == 4);
          free(st.data);
          combos++;
        }
    printf("MATRIX legal_samplerate_bitrate combos=%d spec_capped_mono=%d OK\n",
           combos, spec_capped);
    assert(combos > 50);
    assert(spec_capped == 6); /* exactly the extreme high-bitrate mono cases */
  }

  /* The originally supported stereo modes keep working under protection. */
  {
    static const int modes[] = {STEREO, JOINT_STEREO, DUAL_CHANNEL};
    int mi;
    for (mi = 0; mi < (int)(sizeof(modes) / sizeof(modes[0])); mi++) {
      shine_config_t cfg = make_config(44100, 128, 2);
      cfg.mpeg.mode = modes[mi];
      assembled_t st = encode_stream(&cfg, 1, 0, 6);
      scenario_result_t r;
      verify_stream(st.data, st.len, MPEG_I, &r);
      assert(r.frames == 6 && r.channels == 2);
      free(st.data);
    }
    printf("MODES stereo/joint/dual OK\n");
  }

  /* Two live protected encoders must not share CRC or bit-cache state. */
  {
    shine_config_t ca = make_config(44100, 128, 2);
    shine_config_t cb = make_config(48000, 96, 2);
    shine_t a = shine_initialise_crc(&ca, 1);
    shine_t b = shine_initialise_crc(&cb, 1);
    assert(a && b);
    assembled_t ref_a = encode_stream(&ca, 1, 0, 8);
    int spp_a = shine_samples_per_pass(a);
    int spp_b = shine_samples_per_pass(b);
    int16_t la[1152], ra[1152], lb[1152], rb[1152], *pa[2] = {la, ra},
                                          *pb[2] = {lb, rb};
    assembled_t got_a = {0}, got_b = {0};
    int f;
    for (f = 0; f < 8; f++) {
      fill_pcm(la, ra, spp_a, f);
      fill_pcm(lb, rb, spp_b, f + 50);
      int wa, wb;
      unsigned char *xa = shine_encode_buffer(a, pa, &wa);
      unsigned char *xb = shine_encode_buffer(b, pb, &wb);
      append(&got_a, xa, wa);
      append(&got_b, xb, wb);
    }
    int wa, wb;
    append(&got_a, shine_flush(a, &wa), wa);
    append(&got_b, shine_flush(b, &wb), wb);
    /* Interleaving calls to the two instances must not perturb either one. */
    assert(got_a.len == ref_a.len);
    assert(!memcmp(got_a.data, ref_a.data, got_a.len));
    scenario_result_t rb2;
    verify_stream(got_b.data, got_b.len, MPEG_I, &rb2);
    assert(rb2.frames == 8);
    printf("PARALLEL independent_instances OK\n");
    free(ref_a.data);
    free(got_a.data);
    free(got_b.data);
    shine_close(a);
    shine_close(b);
  }

  /* Mutation tests on one protected stream. */
  {
    shine_config_t cfg = make_config(44100, 128, 2);
    assembled_t good = encode_stream(&cfg, 1, 0, nframes);
    int version = MPEG_I, sib = sideinfo_bytes(version, 2);

    /* Locate frame 0 through its header. */
    size_t f0 = 0;
    assert(good.data[f0] == 0xff && (good.data[f0 + 1] & 0xe0) == 0xe0);
    unsigned sr_idx = (good.data[f0 + 2] >> 2) & 3;
    unsigned br_idx = good.data[f0 + 2] >> 4;
    unsigned pad = (good.data[f0 + 2] >> 1) & 1;
    int flen = 144 * k_bitrates[MPEG_I][br_idx] * 1000 /
                   k_samplerates[MPEG_I][sr_idx] +
               pad;
    uint16_t stored =
        (uint16_t)((good.data[f0 + 4] << 8) | good.data[f0 + 5]);
    size_t side = f0 + 6;
    size_t main = side + sib;
    assert(main + 4 < f0 + (size_t)flen);

    /* Flip a protected side-info bit: the independent check must fail. */
    unsigned char *mut = malloc(good.len);
    assert(mut);
    memcpy(mut, good.data, good.len);
    mut[side] ^= 0x01;
    uint16_t crc_side = independent_crc(0xffff, mut, f0 + 2, 2);
    crc_side = independent_crc(crc_side, mut, side, sib);
    assert(crc_side != stored);
    printf("MUTATION side_info_bit_flip crc_detects=1 OK\n");

    /* Flip an unprotected main-data bit: the check must still match. */
    memcpy(mut, good.data, good.len);
    mut[main + 3] ^= 0x01;
    uint16_t crc_main = independent_crc(0xffff, mut, f0 + 2, 2);
    crc_main = independent_crc(crc_main, mut, side, sib);
    assert(crc_main == stored);
    printf("MUTATION main_data_bit_flip crc_unchanged=1 OK\n");
    free(mut);
    free(good.data);
  }

  /* Invalid crc_enabled arguments must be rejected. */
  {
    shine_config_t cfg = make_config(44100, 128, 2);
    assert(shine_initialise_crc(&cfg, 2) == NULL);
    assert(shine_initialise_crc(&cfg, -1) == NULL);
    assert(shine_initialise_crc(&cfg, 0) != NULL);
    printf("ARGUMENT validation OK\n");
  }

  /* Protected flush: the final frame's cached bytes are delivered once, no
   * extra frame is synthesised, and a repeated flush yields zero bytes. */
  {
    shine_config_t cfg = make_config(44100, 128, 2);
    shine_t s = shine_initialise_crc(&cfg, 1);
    assert(s);
    int spp = shine_samples_per_pass(s);
    int16_t l[1152], r[1152], *planes[2] = {l, r};
    assembled_t one = {0};
    for (int f = 0; f < 3; f++) {
      fill_pcm(l, r, spp, f);
      int w;
      unsigned char *p = shine_encode_buffer(s, planes, &w);
      append(&one, p, w);
    }
    int w;
    unsigned char *p = shine_flush(s, &w);
    append(&one, p, w);
    int w2;
    unsigned char *p2 = shine_flush(s, &w2);
    (void)p2;
    assert(w2 == 0); /* repeated flush delivers nothing */

    /* Exactly three frames, with no trailing extra frame. */
    scenario_result_t res;
    verify_stream(one.data, one.len, MPEG_I, &res);
    assert(res.frames == 3);
    printf("FLUSH single_delivery frames=%d repeat_flush_bytes=0 OK\n",
           res.frames);
    free(one.data);
    shine_close(s);
  }

  puts("CRC_FRAMES_OK");
  return 0;
}
