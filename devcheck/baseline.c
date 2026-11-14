/* Public-API witness for the unchanged, reservoir-free Shine encoder. */
#include "layer3.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "vendor/minimp3.h"

enum { FRAMES = 40, CAPACITY = 200000 };
static unsigned bits(const unsigned char *p, int pos, int n) {
  unsigned v = 0;
  while (n--) { v = (v << 1) | ((p[pos / 8] >> (7 - pos % 8)) & 1); ++pos; }
  return v;
}
static size_t encode(int rate, int bitrate, int channels, int planar,
                     unsigned char *out) {
  shine_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  shine_set_config_mpeg_defaults(&cfg.mpeg);
  cfg.wave.channels = channels == 1 ? PCM_MONO : PCM_STEREO;
  cfg.wave.samplerate = rate;
  cfg.mpeg.bitr = bitrate;
  cfg.mpeg.mode = channels == 1 ? MONO : STEREO;
  shine_t s = shine_initialise(&cfg);
  assert(s);
  int n = shine_samples_per_pass(s);
  int16_t inter[2 * SHINE_MAX_SAMPLES], l[SHINE_MAX_SAMPLES], r[SHINE_MAX_SAMPLES];
  int16_t *planes[2] = {l, r};
  uint32_t rng = 0x12345678u;
  size_t total = 0;
  for (int f = 0; f < FRAMES; ++f) {
    for (int i = 0; i < n; ++i) {
      rng = 1664525u * rng + 1013904223u;
      l[i] = f < 8 || f >= 32 ? 0 : (int16_t)((int)(rng >> 18) - 8192);
      rng = 1664525u * rng + 1013904223u;
      r[i] = f < 8 || f >= 32 ? 0 : (int16_t)((int)(rng >> 18) - 8192);
      inter[channels * i] = l[i];
      if (channels == 2) inter[2 * i + 1] = r[i];
    }
    int written = 0;
    unsigned char *p = planar ? shine_encode_buffer(s, planes, &written)
                              : shine_encode_buffer_interleaved(s, inter, &written);
    assert(written >= 0 && total + written <= CAPACITY);
    memcpy(out + total, p, (size_t)written); total += (size_t)written;
  }
  int written = 0;
  unsigned char *p = shine_flush(s, &written);
  assert(written >= 0 && total + written <= CAPACITY);
  memcpy(out + total, p, (size_t)written); total += (size_t)written;
  shine_close(s);
  return total;
}
static void run(int rate, int bitrate, int channels) {
  unsigned char a[CAPACITY], b[CAPACITY];
  size_t len = encode(rate, bitrate, channels, 0, a);
  assert(len == encode(rate, bitrate, channels, 1, b));
  assert(!memcmp(a, b, len));
  int version = rate >= 32000 ? MPEG_I : rate >= 16000 ? MPEG_II : MPEG_25;
  int samples = version == MPEG_I ? 1152 : 576;
  int frame_size = (version == MPEG_I ? 144000 : 72000) * bitrate / rate;
  assert((frame_size & 3) == 0 && len == (size_t)(FRAMES * frame_size));
  int si_size = version == MPEG_I ? (channels == 2 ? 32 : 17)
                                  : (channels == 2 ? 17 : 9);
  int granules = version == MPEG_I ? 2 : 1;
  int max_main_bits = 0, first_mdb = -1;
  for (int f = 0; f < FRAMES; ++f) {
    const unsigned char *h = a + f * frame_size;
    assert(bits(h, 0, 11) == 0x7ff && bits(h, 11, 2) == (unsigned)version);
    assert(bits(h, 15, 1) == 1 && bits(h, 22, 1) == 0);
    assert(bits(h, 24, 2) == (unsigned)(channels == 1 ? MONO : STEREO));
    unsigned mdb = bits(h + 4, 0, version == MPEG_I ? 9 : 8);
    if (!f) first_mdb = (int)mdb;
    assert(mdb == 0);
    int pos = version == MPEG_I ? 9 + (channels == 2 ? 3 : 5) + 4 * channels
                                : 8 + (channels == 2 ? 2 : 1);
    int sum = 0;
    for (int g = 0; g < granules; ++g)
      for (int c = 0; c < channels; ++c) {
        sum += (int)bits(h + 4, pos, 12);
        pos += version == MPEG_I ? 59 : 63;
      }
    assert(pos == si_size * 8);
    assert(sum <= (frame_size - 4 - si_size) * 8);
    if (sum > max_main_bits) max_main_bits = sum;
  }
  mp3dec_t dec; mp3dec_init(&dec);
  size_t offset = 0; int frames = 0, decoded = 0; uint64_t energy = 0;
  while (offset < len) {
    mp3dec_frame_info_t info;
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int n = mp3dec_decode_frame(&dec, a + offset, (int)(len - offset), pcm, &info);
    assert(info.frame_bytes == frame_size && n == samples);
    assert(info.channels == channels && info.hz == rate);
    for (int j = 0; j < n * channels; ++j)
      energy += (uint64_t)((int64_t)pcm[j] * pcm[j]);
    offset += (size_t)info.frame_bytes; ++frames; decoded += n;
  }
  assert(frames == FRAMES && decoded == FRAMES * samples && energy > 1000000);
  uint64_t hash = UINT64_C(14695981039346656037);
  for (size_t i = 0; i < len; ++i) { hash ^= a[i]; hash *= UINT64_C(1099511628211); }
  printf("SHINE_BASELINE rate=%d channels=%d frames=%d bytes=%zu mdb_first=%d mdb_nonzero=0 max_part23=%d decoded=%d energy=%llu fnv=%016llx\n",
         rate, channels, frames, len, first_mdb, max_main_bits, decoded,
         (unsigned long long)energy, (unsigned long long)hash);
}
int main(void) {
  run(48000, 128, 2); run(48000, 128, 1);
  run(24000, 64, 2); run(8000, 32, 1);
  puts("SHINE_BASELINE_OK cases=4");
  return 0;
}
