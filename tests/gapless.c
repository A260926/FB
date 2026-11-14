/* tests/gapless.c
 *
 * Tests for the gapless streaming API.  Registered with the task driver in
 * three groups - layout, audio and streaming - through task_cases /
 * task_case_count / TASK_CHECK (see task_support/check.h).
 */

#include "task_support/check.h"
#include "gapless.h"
#include "bitstream.h"
#include "layer3.h"
#include "types.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "devcheck/vendor/minimp3.h"
#include "devcheck/vendor/minimp3_ex.h"

/* ------------------------------------------------------------------ */
/* Byte sink with the exact write/patch callback signatures.          */
/* ------------------------------------------------------------------ */

struct sink {
  unsigned char *buf;
  size_t cap;
  size_t len;
  int writes;
  int patches;
  int fail_from_write; /* write call index (1 based) that fails, 0 = never */
  int fail_patch;      /* when nonzero, every patch fails */
};

static int sink_write(void *user, const unsigned char *data, size_t len) {
  struct sink *s = (struct sink *)user;

  if (s->fail_from_write && s->writes + 1 >= s->fail_from_write)
    return -1;

  if (s->len + len > s->cap) {
    size_t ncap = s->cap ? s->cap : 1024;
    while (ncap < s->len + len)
      ncap *= 2;
    unsigned char *nb = realloc(s->buf, ncap);
    if (!nb)
      return -1;
    s->buf = nb;
    s->cap = ncap;
  }
  memcpy(s->buf + s->len, data, len);
  s->len += len;
  s->writes++;
  return 0;
}

static int sink_patch(void *user, uint64_t offset,
                      const unsigned char *data, size_t len) {
  struct sink *s = (struct sink *)user;
  if (s->fail_patch)
    return -1;
  if ((uint64_t)offset + len > s->len)
    return -1;
  memcpy(s->buf + offset, data, len);
  s->patches++;
  return 0;
}

static void sink_init(struct sink *s) { memset(s, 0, sizeof(*s)); }
static void sink_free(struct sink *s) {
  free(s->buf);
  memset(s, 0, sizeof(*s));
}

/* ------------------------------------------------------------------ */
/* Deterministic PCM generation.                                      */
/* ------------------------------------------------------------------ */

static int16_t next_rand(uint32_t *rng) {
  *rng = 1664525u * *rng + 1013904223u;
  return (int16_t)(*rng >> 18); /* roughly -8192..8191 */
}

static void make_pcm(int16_t *pcm, size_t n, int channels, uint32_t seed) {
  uint32_t rng = seed;
  size_t i;
  int c;

  for (i = 0; i < n; i++) {
    int16_t v = next_rand(&rng);
    for (c = 0; c < channels; c++)
      pcm[i * channels + c] = v;
  }

  /* Strong head and tail impulses so their positions are unambiguous. */
  if (n >= 1)
    for (c = 0; c < channels; c++)
      pcm[c] = (c ? 24000 : 26000);
  if (n >= 2)
    for (c = 0; c < channels; c++)
      pcm[channels + c] = (c ? -24000 : -26000);
  if (n >= 2)
    for (c = 0; c < channels; c++)
      pcm[(n - 2) * channels + c] = (c ? -23000 : -25000);
  if (n >= 1)
    for (c = 0; c < channels; c++)
      pcm[(n - 1) * channels + c] = (c ? 23000 : 25000);
}

static void base_config(shine_config_t *cfg, int rate, int channels) {
  memset(cfg, 0, sizeof(*cfg));
  shine_set_config_mpeg_defaults(&cfg->mpeg);
  cfg->wave.samplerate = rate;
  cfg->mpeg.bitr = 128;
  if (channels == 1) {
    cfg->wave.channels = PCM_MONO;
    cfg->mpeg.mode = MONO;
  } else {
    cfg->wave.channels = PCM_STEREO;
    cfg->mpeg.mode = STEREO;
  }
}

/* ------------------------------------------------------------------ */
/* Independent MP3 frame parser (no use of minimp3 here).             */
/* ------------------------------------------------------------------ */

static const int mpeg1_l3_bitrates[16] = {
    0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
static const int mpeg1_samplerates[3] = {44100, 48000, 32000};

struct parsed_hdr {
  int bitrate;
  int samplerate;
  int padding;
  int mode;
  int channels;
  size_t frame_bytes;
};

static int parse_hdr(const unsigned char *p, struct parsed_hdr *h) {
  int bri, sri;

  if ((p[0] & 0xFF) != 0xFF || (p[1] & 0xE0) != 0xE0)
    return -1;
  if (((p[1] >> 3) & 3) != 3) /* MPEG-I */
    return -1;
  if (((p[1] >> 1) & 3) != 1) /* Layer III */
    return -1;
  if ((p[1] & 1) != 1)        /* protection absent */
    return -1;

  bri = (p[2] >> 4) & 0x0F;
  sri = (p[2] >> 2) & 3;
  if (bri == 0 || bri == 15 || sri == 3)
    return -1;

  h->bitrate = mpeg1_l3_bitrates[bri];
  h->samplerate = mpeg1_samplerates[sri];
  h->padding = p[2] & 1;
  h->mode = (p[3] >> 6) & 3;
  h->channels = (h->mode == 3) ? 1 : 2;
  h->frame_bytes =
      (size_t)(144000 * h->bitrate / h->samplerate) + (size_t)h->padding;
  return 0;
}

/* Walk every frame, returning the number found and the audio frame
 * offsets (first offset is the tag frame). */
static int walk_frames(const unsigned char *buf, size_t len,
                       size_t *offsets, int max_frames) {
  size_t off = 0;
  int count = 0;

  while (off + 4 <= len) {
    struct parsed_hdr h;
    if (parse_hdr(buf + off, &h) != 0)
      return -1;
    if (off + h.frame_bytes > len)
      return -1;
    if (offsets && count < max_frames)
      offsets[count] = off;
    count++;
    off += h.frame_bytes;
  }
  if (off != len)
    return -1;
  return count;
}

static uint32_t read_be32(const unsigned char *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* ------------------------------------------------------------------ */
/* Encoding helpers.                                                  */
/* ------------------------------------------------------------------ */

/* Encode a whole buffer through the gapless API using one call. */
static int gapless_once(const shine_config_t *cfg, const int16_t *pcm,
                        size_t frames, struct sink *sink) {
  shine_gapless_t *g;
  int rc;

  sink_init(sink);
  g = shine_gapless_open(cfg, sink, sink_write, sink_patch);
  if (!g)
    return -100;
  rc = shine_gapless_write(g, pcm, frames);
  if (rc != SHINE_GAPLESS_OK) {
    shine_gapless_close(g);
    return rc;
  }
  rc = shine_gapless_finish(g);
  shine_gapless_close(g);
  return rc;
}

/* Old-core reference: original PCM, zero padding to a full block, plus
 * 'extra_drain' silent blocks, with the bitstream cache tail emitted. */
static size_t old_core_encode(const shine_config_t *cfg, const int16_t *pcm,
                              size_t n, int extra_drain,
                              unsigned char *out, size_t out_cap) {
  shine_t s = shine_initialise((shine_config_t *)cfg);
  shine_global_config *sc;
  int spp, channels, total_blocks, b;
  int16_t *block;
  size_t total = 0;

  if (!s)
    return 0;
  sc = (shine_global_config *)s;
  spp = shine_samples_per_pass(s);
  channels = cfg->wave.channels;
  total_blocks = (int)((n + spp - 1) / spp) + 1 /* required drain */ +
                 extra_drain;
  block = calloc((size_t)spp * (size_t)channels, sizeof(int16_t));

  for (b = 0; b < total_blocks; b++) {
    int written = 0, i, c;
    unsigned char *data;
    size_t base = (size_t)b * spp;

    for (i = 0; i < spp; i++) {
      if (base + i < n)
        for (c = 0; c < channels; c++)
          block[i * channels + c] = pcm[(base + i) * channels + c];
      else
        for (c = 0; c < channels; c++)
          block[i * channels + c] = 0;
    }

    data = shine_encode_buffer_interleaved(s, block, &written);
    if (total + written > out_cap)
      goto fail;
    memcpy(out + total, data, written);
    total += written;
  }

  {
    int written = 0;
    unsigned char *data = shine_flush(s, &written);
    if (total + written > out_cap)
      goto fail;
    memcpy(out + total, data, written);
    total += written;
  }

  /* The core stores its word cache big-endian; emit the valid bytes. */
  {
    int valid_bytes = (32 - sc->bs.cache_bits) / 8;
    int i;
    if (total + valid_bytes > out_cap)
      goto fail;
    for (i = 0; i < valid_bytes; i++)
      out[total + i] = (unsigned char)(sc->bs.cache >> (24 - 8 * i));
    total += valid_bytes;
  }

fail:
  free(block);
  shine_close(s);
  return total;
}

/* Decode a complete buffer, trimming via the Info/LAME tag when present. */
static int decode_trimmed(const unsigned char *buf, size_t len,
                          mp3d_sample_t **out, size_t *out_samples,
                          int *out_channels, int *out_hz) {
  mp3dec_file_info_t info;
  mp3dec_t dec;
  int rc;

  mp3dec_init(&dec);
  memset(&info, 0, sizeof(info));
  rc = mp3dec_load_buf(&dec, buf, len, &info, NULL, NULL);
  if (rc != 0) {
    free(info.buffer);
    return rc;
  }
  *out = info.buffer;
  *out_samples = info.samples;
  *out_channels = info.channels;
  *out_hz = info.hz;
  return 0;
}

static int loud_index(const mp3d_sample_t *pcm, size_t n, int channels,
                      int first) {
  size_t i;
  if (first) {
    for (i = 0; i < n; i++)
      if (abs(pcm[i * channels]) >= 12000)
        return (int)i;
  } else {
    for (i = n; i-- > 0;)
      if (abs(pcm[i * channels]) >= 12000)
        return (int)i;
  }
  return -1;
}

/* Strongest sample within a 64-sample window at the head or tail edge.
 * Lossy coding spreads an impulse over a few samples, so edge fidelity is
 * judged against the independently decoded old-core reference instead of an
 * absolute index. */
static size_t edge_peak(const mp3d_sample_t *pcm, size_t n, int channels,
                        int head) {
  size_t window = n < 64 ? n : 64;
  size_t base = head ? 0 : n - window;
  size_t i, best = base;
  int best_amp = abs(pcm[base * channels]);

  for (i = base; i < base + window; i++) {
    int a = abs(pcm[i * channels]);
    if (a > best_amp) {
      best_amp = a;
      best = i;
    }
  }
  return best;
}

/* ================================================================== */
/* layout group                                                       */
/* ================================================================== */

static int test_layout_counts(void) {
  shine_config_t cfg;
  struct sink sink;
  const size_t n = 4000;
  int channels = 2;
  int16_t *pcm = calloc(n * channels, sizeof(int16_t));
  size_t offsets[64];
  int frames, f;
  struct parsed_hdr tag_hdr, audio_hdr;
  int sideinfo, tag_off;
  uint32_t tag_frames, tag_bytes;

  make_pcm(pcm, n, channels, 11);
  base_config(&cfg, 48000, channels);

  TASK_CHECK(gapless_once(&cfg, pcm, n, &sink) == SHINE_GAPLESS_OK);

  frames = walk_frames(sink.buf, sink.len, offsets, 64);
  TASK_CHECK(frames >= 3);
  TASK_CHECK(offsets[0] == 0);

  /* First frame is the tag frame, excluded from the advertised count. */
  TASK_CHECK(parse_hdr(sink.buf + offsets[0], &tag_hdr) == 0);
  TASK_CHECK(tag_hdr.channels == channels);
  TASK_CHECK(tag_hdr.samplerate == 48000);
  TASK_CHECK(tag_hdr.bitrate == 128);
  TASK_CHECK(tag_hdr.padding == 0);

  sideinfo = channels == 2 ? 32 : 17;
  tag_off = 4 + sideinfo;
  TASK_CHECK(memcmp(sink.buf + tag_off, "Info", 4) == 0);
  TASK_CHECK(sink.buf[tag_off + 7] == 3); /* flags: frames + bytes */
  tag_frames = read_be32(sink.buf + tag_off + 8);
  tag_bytes = read_be32(sink.buf + tag_off + 12);

  TASK_CHECK((int)tag_frames == frames - 1);
  TASK_CHECK(tag_bytes == sink.len);

  /* Every audio frame follows the size formula and the stream ends
   * exactly on a frame boundary. */
  for (f = 1; f < frames; f++) {
    TASK_CHECK(parse_hdr(sink.buf + offsets[f], &audio_hdr) == 0);
    TASK_CHECK(audio_hdr.bitrate == 128);
    TASK_CHECK(audio_hdr.samplerate == 48000);
    TASK_CHECK(audio_hdr.channels == channels);
    if (f + 1 < frames)
      TASK_CHECK(offsets[f] + audio_hdr.frame_bytes == offsets[f + 1]);
  }
  TASK_CHECK(offsets[frames - 1] + audio_hdr.frame_bytes == sink.len);

  sink_free(&sink);
  free(pcm);
  return 0;
}

static int test_layout_fields(void) {
  shine_config_t cfg;
  struct sink sink;
  const size_t n = 1152 * 2 + 333;
  int channels = 1;
  int16_t *pcm = calloc(n * channels, sizeof(int16_t));
  int tag_off = 4 + 17;
  int lame_off;
  uint32_t raw_delay, raw_pad;
  uint32_t expected_frames;
  size_t i;
  int zero;

  make_pcm(pcm, n, channels, 23);
  base_config(&cfg, 44100, channels);

  TASK_CHECK(gapless_once(&cfg, pcm, n, &sink) == SHINE_GAPLESS_OK);

  TASK_CHECK(memcmp(sink.buf + tag_off, "Info", 4) == 0);
  TASK_CHECK(read_be32(sink.buf + tag_off + 4) == 3);

  lame_off = tag_off + 16;
  TASK_CHECK(memcmp(sink.buf + lame_off, "LAME3.100", 9) == 0);

  raw_delay = ((uint32_t)sink.buf[lame_off + 21] << 4) |
              (sink.buf[lame_off + 22] >> 4);
  raw_pad = ((uint32_t)(sink.buf[lame_off + 22] & 0x0F) << 8) |
            sink.buf[lame_off + 23];

  /* Encoder delay only: the decoder's own delay is not included. */
  TASK_CHECK(raw_delay == 528);

  expected_frames = (uint32_t)((n + 1151) / 1152 + 1);
  TASK_CHECK(read_be32(sink.buf + tag_off + 8) == expected_frames);
  TASK_CHECK(raw_pad == expected_frames * 1152 - (uint32_t)n - 528);

  /* Everything else in the LAME extension is zero: no CRC is declared. */
  zero = 1;
  for (i = 9; i < 21; i++)
    if (sink.buf[lame_off + i] != 0)
      zero = 0;
  for (i = 24; i < 36; i++)
    if (sink.buf[lame_off + i] != 0)
      zero = 0;
  TASK_CHECK(zero);

  /* The tag is one unpadded 44.1 kHz slot. */
  {
    struct parsed_hdr h;
    TASK_CHECK(parse_hdr(sink.buf, &h) == 0);
    TASK_CHECK(h.frame_bytes == 417);
    TASK_CHECK(h.padding == 0);
  }

  sink_free(&sink);
  free(pcm);
  return 0;
}

static int test_layout_reject(void) {
  shine_config_t cfg;
  struct sink sink;

  sink_init(&sink);

  base_config(&cfg, 44100, 2);
  cfg.mpeg.bitr = 64; /* wrong bitrate */
  TASK_CHECK(shine_gapless_open(&cfg, &sink, sink_write, sink_patch) == NULL);

  base_config(&cfg, 32000, 2); /* unsupported samplerate */
  cfg.mpeg.bitr = 128;
  TASK_CHECK(shine_gapless_open(&cfg, &sink, sink_write, sink_patch) == NULL);

  base_config(&cfg, 24000, 2); /* MPEG-II */
  cfg.mpeg.bitr = 64;
  TASK_CHECK(shine_gapless_open(&cfg, &sink, sink_write, sink_patch) == NULL);

  base_config(&cfg, 48000, 2);
  cfg.wave.channels = PCM_MONO; /* mismatched channel modes */
  TASK_CHECK(shine_gapless_open(&cfg, &sink, sink_write, sink_patch) == NULL);

  base_config(&cfg, 48000, 1);
  cfg.mpeg.mode = STEREO;
  TASK_CHECK(shine_gapless_open(&cfg, &sink, sink_write, sink_patch) == NULL);

  TASK_CHECK(shine_gapless_open(NULL, &sink, sink_write, sink_patch) == NULL);
  base_config(&cfg, 48000, 1);
  TASK_CHECK(shine_gapless_open(&cfg, &sink, NULL, sink_patch) == NULL);
  TASK_CHECK(shine_gapless_open(&cfg, &sink, sink_write, NULL) == NULL);

  /* A rejected open never emitted anything. */
  TASK_CHECK(sink.len == 0 && sink.writes == 0);

  /* NULL handle arguments are reported as parameter errors. */
  TASK_CHECK(shine_gapless_write(NULL, NULL, 0) == SHINE_GAPLESS_ERR_PARAM);
  TASK_CHECK(shine_gapless_finish(NULL) == SHINE_GAPLESS_ERR_PARAM);
  shine_gapless_close(NULL); /* must be a safe no-op */

  sink_free(&sink);
  return 0;
}

/* ================================================================== */
/* audio group                                                        */
/* ================================================================== */

/* Check one recording: gapless output trims to exactly N samples per
 * channel and matches the old-core long-tail reference, with head and
 * tail impulses landing at the very first and last sample.  Every
 * condition goes through task_check so the harness counts it. */
static int check_one_audio(shine_config_t *cfg, size_t n, uint32_t seed) {
  int channels = cfg->wave.channels;
  int16_t *pcm = calloc(n * channels, sizeof(int16_t));
  struct sink sink;
  mp3d_sample_t *got = NULL, *ref = NULL;
  size_t got_samples = 0, ref_samples = 0;
  int got_ch = 0, got_hz = 0, ref_ch = 0, ref_hz = 0;
  unsigned char *ref_buf = malloc(1u << 20);
  size_t ref_len;
  size_t i;
  size_t mism = 0;
  long long energy = 0;
  int ok = 1;

#define AUDIO_CHECK(x)                                                       \
  do {                                                                       \
    if (!task_check(!!(x), #x, __FILE__, __LINE__))                         \
      ok = 0;                                                                \
  } while (0)

  make_pcm(pcm, n, channels, seed);

  AUDIO_CHECK(gapless_once(cfg, pcm, n, &sink) == SHINE_GAPLESS_OK);
  AUDIO_CHECK(decode_trimmed(sink.buf, sink.len, &got, &got_samples, &got_ch,
                             &got_hz) == 0);
  AUDIO_CHECK(got_ch == channels && got_hz == cfg->wave.samplerate);
  AUDIO_CHECK(got_samples == n * (size_t)channels);

  /* Old-core reference with a long zero tail. */
  ref_len = old_core_encode(cfg, pcm, n, 3, ref_buf, 1u << 20);
  AUDIO_CHECK(ref_len > 0);
  AUDIO_CHECK(decode_trimmed(ref_buf, ref_len, &ref, &ref_samples, &ref_ch,
                             &ref_hz) == 0);
  AUDIO_CHECK(ref_samples >= got_samples + 1057 * channels);

  if (got && ref && ref_samples >= got_samples + 1057 * channels) {
    /* Gapless sample i equals long-tail reference sample i + 1057. */
    for (i = 0; i < got_samples; i++) {
      int d = (int)got[i] - (int)ref[(1057 * channels) + i];
      if (d < 0)
        d = -d;
      if (d > 32)
        mism++;
      energy += (long long)got[i] * got[i];
    }
    AUDIO_CHECK(mism * 1000 < got_samples);
    AUDIO_CHECK(energy > 1000000);

    /* Head and tail impulses: the gapless stream must place its edge peaks
     * exactly where the independently decoded old-core long-tail reference
     * does, and both edges must carry clearly audible, strong sound. */
    AUDIO_CHECK(edge_peak(got, n, channels, 1) ==
                edge_peak(ref + 1057 * channels, n, channels, 1));
    AUDIO_CHECK(edge_peak(got, n, channels, 0) ==
                edge_peak(ref + 1057 * channels, n, channels, 0));
    AUDIO_CHECK(loud_index(got, n, channels, 1) >= 0 &&
                loud_index(got, n, channels, 1) < 64);
    AUDIO_CHECK(loud_index(got, n, channels, 0) >= (int)n - 64);
    /* Amplitudes survive lossy coding within a generous envelope. */
    AUDIO_CHECK(
        abs(got[edge_peak(got, n, channels, 1) * channels]) > 12000 &&
        abs(got[edge_peak(got, n, channels, 0) * channels]) > 12000);
  }

#undef AUDIO_CHECK

  free(got);
  free(ref);
  free(ref_buf);
  sink_free(&sink);
  free(pcm);
  return ok ? 0 : 1;
}

static int test_audio_short(void) {
  shine_config_t cfg;

  base_config(&cfg, 48000, 1);
  TASK_CHECK(check_one_audio(&cfg, 1, 101) == 0);
  base_config(&cfg, 44100, 2);
  TASK_CHECK(check_one_audio(&cfg, 300, 102) == 0);
  return 0;
}

static int test_audio_exact_frame(void) {
  shine_config_t cfg;

  base_config(&cfg, 48000, 2);
  TASK_CHECK(check_one_audio(&cfg, 1152, 201) == 0);
  base_config(&cfg, 44100, 1);
  TASK_CHECK(check_one_audio(&cfg, 1152, 202) == 0);
  return 0;
}

static int test_audio_multi_frame(void) {
  shine_config_t cfg;

  base_config(&cfg, 48000, 1);
  TASK_CHECK(check_one_audio(&cfg, 1152 * 3 + 333, 301) == 0);
  base_config(&cfg, 44100, 2);
  TASK_CHECK(check_one_audio(&cfg, 1152 * 5 + 777, 302) == 0);
  return 0;
}

/* ================================================================== */
/* streaming group                                                    */
/* ================================================================== */

static int encode_chunked(const shine_config_t *cfg, const int16_t *pcm,
                          size_t frames, size_t chunk, struct sink *sink) {
  shine_gapless_t *g;
  size_t off = 0;
  int rc;

  sink_init(sink);
  g = shine_gapless_open(cfg, sink, sink_write, sink_patch);
  if (!g)
    return -100;

  while (off < frames) {
    size_t take = frames - off;
    if (take > chunk)
      take = chunk;
    rc = shine_gapless_write(g, pcm + off * cfg->wave.channels, take);
    if (rc != SHINE_GAPLESS_OK) {
      shine_gapless_close(g);
      return rc;
    }
    off += take;
  }
  rc = shine_gapless_finish(g);
  shine_gapless_close(g);
  return rc;
}

static int test_stream_fragments(void) {
  static const size_t chunks[] = {1, 7, 113};
  static const struct {
    int rate;
    int channels;
    size_t n;
  } cases[] = {
      {48000, 2, 1152 * 5 + 257},
      {44100, 1, 2000},
      {48000, 1, 333},
      {44100, 2, 1152 * 4 + 1},
  };
  size_t ci;

  for (ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
    shine_config_t cfg;
    int16_t *pcm;
    struct sink whole, part;
    size_t k;

    base_config(&cfg, cases[ci].rate, cases[ci].channels);
    pcm = calloc(cases[ci].n * cases[ci].channels, sizeof(int16_t));
    make_pcm(pcm, cases[ci].n, cases[ci].channels, 400 + (uint32_t)ci);

    TASK_CHECK(gapless_once(&cfg, pcm, cases[ci].n, &whole) ==
               SHINE_GAPLESS_OK);

    for (k = 0; k < sizeof(chunks) / sizeof(chunks[0]); k++) {
      TASK_CHECK(encode_chunked(&cfg, pcm, cases[ci].n, chunks[k], &part) ==
                 SHINE_GAPLESS_OK);
      TASK_CHECK(part.len == whole.len);
      TASK_CHECK(memcmp(part.buf, whole.buf, whole.len) == 0);
      sink_free(&part);
    }

    /* The result really contains decodable, non-silent sound. */
    {
      mp3d_sample_t *dec = NULL;
      size_t dec_n = 0;
      int dch = 0, dhz = 0;
      TASK_CHECK(decode_trimmed(whole.buf, whole.len, &dec, &dec_n, &dch,
                                &dhz) == 0);
      TASK_CHECK(dec_n == cases[ci].n * cases[ci].channels);
      TASK_CHECK(dch == cases[ci].channels && dhz == cases[ci].rate);
      free(dec);
    }

    sink_free(&whole);
    free(pcm);
  }
  return 0;
}

static int test_stream_byte_identity(void) {
  static const struct {
    int rate;
    int channels;
    size_t n;
  } cases[] = {
      {48000, 2, 3000},
      {44100, 2, 1152 * 3 + 17},
      {48000, 1, 1152},
      {44100, 1, 1},
  };
  size_t ci;

  for (ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
    shine_config_t cfg;
    int16_t *pcm;
    struct sink sink;
    unsigned char ref[1u << 18];
    size_t ref_len;
    struct parsed_hdr tag_hdr;
    size_t tag_size;

    base_config(&cfg, cases[ci].rate, cases[ci].channels);
    pcm = calloc(cases[ci].n * cases[ci].channels, sizeof(int16_t));
    make_pcm(pcm, cases[ci].n, cases[ci].channels, 500 + (uint32_t)ci);

    TASK_CHECK(gapless_once(&cfg, pcm, cases[ci].n, &sink) ==
               SHINE_GAPLESS_OK);
    TASK_CHECK(parse_hdr(sink.buf, &tag_hdr) == 0);
    tag_size = tag_hdr.frame_bytes;

    /* Old core: same PCM, zero padding and exactly the required drain. */
    ref_len = old_core_encode(&cfg, pcm, cases[ci].n, 0, ref, sizeof(ref));
    TASK_CHECK(ref_len > 0);
    TASK_CHECK(sink.len == tag_size + ref_len);
    TASK_CHECK(memcmp(sink.buf + tag_size, ref, ref_len) == 0);

    sink_free(&sink);
    free(pcm);
  }
  return 0;
}

static int test_stream_interleave(void) {
  shine_config_t cfg_a, cfg_b;
  shine_gapless_t *a, *b;
  struct sink sa, sb;
  struct sink ref_a, ref_b;
  int16_t *pa, *pb;
  size_t na = 1152 * 3 + 51, nb = 1152 * 2 + 900;
  int rc;

  base_config(&cfg_a, 48000, 2);
  base_config(&cfg_b, 44100, 1);
  pa = calloc(na * 2, sizeof(int16_t));
  pb = calloc(nb, sizeof(int16_t));
  make_pcm(pa, na, 2, 601);
  make_pcm(pb, nb, 1, 602);

  /* Single-shot references. */
  TASK_CHECK(gapless_once(&cfg_a, pa, na, &ref_a) == SHINE_GAPLESS_OK);
  TASK_CHECK(gapless_once(&cfg_b, pb, nb, &ref_b) == SHINE_GAPLESS_OK);

  sink_init(&sa);
  sink_init(&sb);
  a = shine_gapless_open(&cfg_a, &sa, sink_write, sink_patch);
  b = shine_gapless_open(&cfg_b, &sb, sink_write, sink_patch);
  TASK_CHECK(a != NULL && b != NULL);

  {
    size_t off_a = 0, off_b = 0;
    while (off_a < na || off_b < nb) {
      if (off_a < na) {
        size_t take = na - off_a;
        if (take > 13)
          take = 13;
        rc = shine_gapless_write(a, pa + off_a * 2, take);
        TASK_CHECK(rc == SHINE_GAPLESS_OK);
        off_a += take;
      }
      if (off_b < nb) {
        size_t take = nb - off_b;
        if (take > 29)
          take = 29;
        rc = shine_gapless_write(b, pb + off_b, take);
        TASK_CHECK(rc == SHINE_GAPLESS_OK);
        off_b += take;
      }
    }
  }

  TASK_CHECK(shine_gapless_finish(a) == SHINE_GAPLESS_OK);
  TASK_CHECK(shine_gapless_finish(b) == SHINE_GAPLESS_OK);
  shine_gapless_close(a);
  shine_gapless_close(b);

  TASK_CHECK(sa.len == ref_a.len && memcmp(sa.buf, ref_a.buf, sa.len) == 0);
  TASK_CHECK(sb.len == ref_b.len && memcmp(sb.buf, ref_b.buf, sb.len) == 0);

  sink_free(&sa);
  sink_free(&sb);
  sink_free(&ref_a);
  sink_free(&ref_b);
  free(pa);
  free(pb);
  return 0;
}

static int test_stream_finish_rules(void) {
  shine_config_t cfg;
  struct sink sink;
  shine_gapless_t *g;
  int16_t pcm[128];
  size_t before_len;
  int before_patches;

  base_config(&cfg, 48000, 1);
  make_pcm(pcm, 128, 1, 701);

  /* Empty recording: explicit error, only the tag frame went out. */
  sink_init(&sink);
  g = shine_gapless_open(&cfg, &sink, sink_write, sink_patch);
  TASK_CHECK(g != NULL);
  TASK_CHECK(shine_gapless_write(g, NULL, 0) == SHINE_GAPLESS_OK);
  TASK_CHECK(shine_gapless_finish(g) == SHINE_GAPLESS_ERR_EMPTY);
  {
    struct parsed_hdr h;
    TASK_CHECK(parse_hdr(sink.buf, &h) == 0);
    TASK_CHECK(sink.len == h.frame_bytes);
  }
  shine_gapless_close(g);
  sink_free(&sink);

  /* Repeated finish succeeds, writes nothing more, and no audio follows. */
  sink_init(&sink);
  g = shine_gapless_open(&cfg, &sink, sink_write, sink_patch);
  TASK_CHECK(g != NULL);
  TASK_CHECK(shine_gapless_write(g, pcm, 128) == SHINE_GAPLESS_OK);
  TASK_CHECK(shine_gapless_finish(g) == SHINE_GAPLESS_OK);
  before_len = sink.len;
  before_patches = sink.patches;
  TASK_CHECK(shine_gapless_finish(g) == SHINE_GAPLESS_OK);
  TASK_CHECK(sink.len == before_len && sink.patches == before_patches);
  TASK_CHECK(shine_gapless_write(g, pcm, 1) == SHINE_GAPLESS_ERR_STATE);
  TASK_CHECK(sink.len == before_len);
  shine_gapless_close(g);
  sink_free(&sink);

  /* Zero-length writes mixed with real ones do not change the output. */
  {
    struct sink ref;
    TASK_CHECK(gapless_once(&cfg, pcm, 128, &ref) == SHINE_GAPLESS_OK);
    sink_init(&sink);
    g = shine_gapless_open(&cfg, &sink, sink_write, sink_patch);
    TASK_CHECK(g != NULL);
    TASK_CHECK(shine_gapless_write(g, NULL, 0) == SHINE_GAPLESS_OK);
    TASK_CHECK(shine_gapless_write(g, pcm, 64) == SHINE_GAPLESS_OK);
    TASK_CHECK(shine_gapless_write(g, NULL, 0) == SHINE_GAPLESS_OK);
    TASK_CHECK(shine_gapless_write(g, pcm + 64, 64) == SHINE_GAPLESS_OK);
    TASK_CHECK(shine_gapless_write(g, NULL, 0) == SHINE_GAPLESS_OK);
    TASK_CHECK(shine_gapless_finish(g) == SHINE_GAPLESS_OK);
    TASK_CHECK(sink.len == ref.len);
    TASK_CHECK(memcmp(sink.buf, ref.buf, ref.len) == 0);
    shine_gapless_close(g);
    sink_free(&sink);
    sink_free(&ref);
  }
  return 0;
}

static int test_stream_callback_failure(void) {
  shine_config_t cfg;
  struct sink sink;
  shine_gapless_t *g;
  int16_t pcm[1152 * 4 * 2];
  size_t len_at_fail;

  base_config(&cfg, 48000, 2);
  make_pcm(pcm, 1152 * 4, 2, 801);

  sink_init(&sink);
  sink.fail_from_write = 3; /* tag is write 1, audio blocks follow */
  g = shine_gapless_open(&cfg, &sink, sink_write, sink_patch);
  TASK_CHECK(g != NULL);
  TASK_CHECK(shine_gapless_write(g, pcm, 1152 * 4) ==
             SHINE_GAPLESS_ERR_CALLBACK);
  len_at_fail = sink.len;
  /* Sticky error state: no further bytes, write and finish refuse. */
  TASK_CHECK(shine_gapless_write(g, pcm, 1) == SHINE_GAPLESS_ERR_CALLBACK);
  TASK_CHECK(shine_gapless_finish(g) == SHINE_GAPLESS_ERR_CALLBACK);
  TASK_CHECK(sink.len == len_at_fail);
  shine_gapless_close(g);
  sink_free(&sink);

  /* A failing patch at finish also poisons the instance. */
  sink_init(&sink);
  g = shine_gapless_open(&cfg, &sink, sink_write, sink_patch);
  TASK_CHECK(g != NULL);
  TASK_CHECK(shine_gapless_write(g, pcm, 100) == SHINE_GAPLESS_OK);
  sink.fail_patch = 1;
  TASK_CHECK(shine_gapless_finish(g) == SHINE_GAPLESS_ERR_CALLBACK);
  TASK_CHECK(shine_gapless_finish(g) == SHINE_GAPLESS_ERR_CALLBACK);
  shine_gapless_close(g);
  sink_free(&sink);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Registration.                                                      */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    {"layout", "counts", test_layout_counts},
    {"layout", "fields", test_layout_fields},
    {"layout", "reject", test_layout_reject},

    {"audio", "short", test_audio_short},
    {"audio", "exact_frame", test_audio_exact_frame},
    {"audio", "multi_frame", test_audio_multi_frame},

    {"streaming", "fragments", test_stream_fragments},
    {"streaming", "byte_identity", test_stream_byte_identity},
    {"streaming", "interleave", test_stream_interleave},
    {"streaming", "finish_rules", test_stream_finish_rules},
    {"streaming", "callback_failure", test_stream_callback_failure},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
