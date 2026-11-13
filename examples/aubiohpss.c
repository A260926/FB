/*
  Copyright (C) 2026 Paul Brossier <piem@aubio.org>

  This file is part of aubio.

  aubio is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  aubio is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with aubio.  If not, see <http://www.gnu.org/licenses/>.

*/

/** \file

  Harmonic / percussive source separation example.

  The whole signal is synthesized in memory, so the example needs no audio
  file or external library. It shows the streaming contract of
  aubio_hpss_t: every aubio_hpss_do() consumes one hop and writes one hop
  of each stem, the first time_radius hops are silence, and
  aubio_hpss_finish() then delivers exactly time_radius + 3 tail blocks.

  \example spectral/test-hpss.c

*/

#include <aubio.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static smpl_t make_sample(uint_t n)
{
  /* a steady tone plus a few isolated impulses */
  smpl_t v = (smpl_t)(0.30 * sin(2. * M_PI * 8. * (double)n / 256.));
  if (n % 97 == 0) v += 0.6f;
  if (n % 97 == 1) v -= 0.3f;
  return v;
}

int main(int argc, char **argv)
{
  uint_t N = 256, time_radius = 3, freq_radius = 3, blocks = 20;
  uint_t H, channels = 1, b, c, n;
  aubio_hpss_t *hpss;
  fmat_t *in, *harm, *perc;
  smpl_t *source;
  uint_t tail_blocks = 0;

  if (argc > 1) N = (uint_t)atoi(argv[1]);
  if (argc > 2) time_radius = (uint_t)atoi(argv[2]);
  if (argc > 3) freq_radius = (uint_t)atoi(argv[3]);
  H = N / 4;

  /* invalid construction returns NULL, without touching the heap further */
  hpss = new_aubio_hpss(N, H, channels, time_radius, freq_radius);
  if (hpss == NULL) {
    fprintf(stderr, "could not create hpss: N must be a power of two >= 64,"
        " H = N/4, channels in {1,2}, radii in [1,31].\n");
    return 1;
  }

  in = new_fmat(channels, H);
  harm = new_fmat(channels, H);
  perc = new_fmat(channels, H);
  source = (smpl_t *)malloc(blocks * H * sizeof(smpl_t));
  if (in == NULL || harm == NULL || perc == NULL || source == NULL) {
    del_aubio_hpss(hpss);
    return 1;
  }
  for (n = 0; n < blocks * H; n++) source[n] = make_sample(n);

  printf("hpss: N=%d H=%d time_radius=%d freq_radius=%d latency=%d samples\n",
      N, H, time_radius, freq_radius, aubio_hpss_get_latency(hpss));

  /* streaming phase: one input hop, one hop of each stem per call */
  for (b = 0; b < blocks; b++) {
    for (n = 0; n < H; n++) in->data[0][n] = source[b * H + n];
    if (aubio_hpss_do(hpss, in, harm, perc) != 0) {
      fprintf(stderr, "aubio_hpss_do failed on block %d\n", b);
      return 1;
    }
  }

  /* tail phase: one block per 1 return, exactly time_radius + 3 of them */
  while (aubio_hpss_finish(hpss, harm, perc) == 1) {
    tail_blocks++;
    for (c = 0; c < channels; c++)
      for (n = 0; n < H; n++)
        if (fabs(harm->data[c][n]) + fabs(perc->data[c][n]) > 4.) {
          /* masks are complementary, so stems stay within the input range */
          fprintf(stderr, "unexpected stem amplitude\n");
          return 1;
        }
  }
  if (tail_blocks != time_radius + 3) {
    fprintf(stderr, "expected %d tail blocks, got %d\n",
        time_radius + 3, tail_blocks);
    return 1;
  }

  (void)c;
  /* once drained, finish keeps returning 0 and writes nothing */
  if (aubio_hpss_finish(hpss, harm, perc) != 0) {
    fprintf(stderr, "drained finish did not return 0\n");
    return 1;
  }

  /* reset clears every history while keeping the configuration */
  aubio_hpss_reset(hpss);
  if (aubio_hpss_get_latency(hpss)
      != (N - H) + time_radius * H) {
    fprintf(stderr, "latency changed after reset\n");
    return 1;
  }

  free(source);
  del_fmat(perc);
  del_fmat(harm);
  del_fmat(in);
  del_aubio_hpss(hpss);
  aubio_cleanup();
  printf("hpss: streamed %d input hops and %d tail blocks, ok\n",
      blocks, tail_blocks);
  return 0;
}
