/*
** examples/limiter.c -- renders a loud stereo mix through the lookahead
** limiter (L=37, ceiling=16000, release_frames=64) and writes raw s16le
** interleaved PCM to the file given as the only argument. The limiter's
** latency tail is rendered as well. Prints LIMITER_DEMO_OK on success.
*/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "cmixer.h"

#define SRATE  48000
#define FRAMES 4800
#define CHUNK  512
#define PI     3.14159265358979323846

typedef struct {
  long pos;
  long frames;
  const cm_Int16 *data;
} Gen;

static void gen_handler(cm_Event *e) {
  Gen *g = (Gen*) e->udata;
  int i;
  switch (e->type) {
    case CM_EVENT_SAMPLES:
      for (i = 0; i < e->length; i += 2) {
        long f = g->pos++;
        e->buffer[i]     = f < g->frames ? g->data[2*f]     : 0;
        e->buffer[i + 1] = f < g->frames ? g->data[2*f + 1] : 0;
      }
      break;
    case CM_EVENT_REWIND:
      g->pos = 0;
      break;
  }
}

int main(int argc, char **argv) {
  static cm_Int16 data_a[2 * FRAMES], data_b[2 * FRAMES];
  static cm_Int16 out[CHUNK];
  Gen ga, gb;
  cm_SourceInfo ia, ib;
  cm_Source *sa, *sb;
  FILE *fp;
  long f, i, total;
  int peak = 0, nonzero = 0;

  if (argc != 2) {
    fprintf(stderr, "usage: %s <output.pcm>\n", argv[0]);
    return 1;
  }

  /* two loud detuned voices; the channels differ so the mix is stereo */
  for (f = 0; f < FRAMES; f++) {
    double t = (double) f / SRATE;
    data_a[2*f]     = (cm_Int16) (15000.0 * sin(2.0 * PI * 330.0 * t) +
                                   5000.0 * sin(2.0 * PI * 990.0 * t));
    data_a[2*f + 1] = (cm_Int16) (12000.0 * sin(2.0 * PI * 332.0 * t + 0.5));
    data_b[2*f]     = (cm_Int16)  (9000.0 * sin(2.0 * PI * 660.0 * t));
    data_b[2*f + 1] = (cm_Int16) (14000.0 * sin(2.0 * PI * 657.0 * t + 1.0) +
                                   3000.0 * sin(2.0 * PI * 1980.0 * t));
  }

  cm_init(SRATE);
  ga.pos = 0; ga.frames = FRAMES; ga.data = data_a;
  gb.pos = 0; gb.frames = FRAMES; gb.data = data_b;
  ia.handler = gen_handler; ia.udata = &ga; ia.samplerate = SRATE; ia.length = FRAMES;
  ib.handler = gen_handler; ib.udata = &gb; ib.samplerate = SRATE; ib.length = FRAMES;
  sa = cm_new_source(&ia);
  sb = cm_new_source(&ib);
  if (!sa || !sb) {
    fprintf(stderr, "source creation failed: %s\n", cm_get_error());
    return 1;
  }
  cm_set_gain(sa, 1.4);
  cm_set_gain(sb, 1.3);
  cm_set_pan(sa, -0.35);
  cm_set_pan(sb, 0.45);

  if (!cm_configure_limiter(37, 16000, 64)) {
    fprintf(stderr, "cm_configure_limiter failed: %s\n", cm_get_error());
    return 1;
  }
  cm_play(sa);
  cm_play(sb);

  fp = fopen(argv[1], "wb");
  if (!fp) {
    fprintf(stderr, "cannot open %s\n", argv[1]);
    return 1;
  }

  /* render the programme plus the limiter's latency tail */
  total = FRAMES + cm_get_limiter_latency();
  f = 0;
  while (f < total) {
    int frames = (int) ((total - f) > (CHUNK / 2) ? (CHUNK / 2) : (total - f));
    cm_process(out, frames * 2);
    for (i = 0; i < frames * 2; i++) {
      int v = out[i];
      int a = v < 0 ? -v : v;
      if (a > peak) {
        peak = a;
      }
      if (v) {
        nonzero = 1;
      }
    }
    if (fwrite(out, sizeof(cm_Int16), (size_t) (frames * 2), fp) !=
        (size_t) (frames * 2)) {
      fclose(fp);
      fprintf(stderr, "write failed\n");
      return 1;
    }
    f += frames;
  }
  if (fclose(fp)) {
    fprintf(stderr, "close failed\n");
    return 1;
  }

  cm_destroy_source(sa);
  cm_destroy_source(sb);
  cm_disable_limiter();

  if (!nonzero) {
    fprintf(stderr, "output is silent\n");
    return 1;
  }
  if (peak > 16000) {
    fprintf(stderr, "peak %d exceeds ceiling\n", peak);
    return 1;
  }
  printf("LIMITER_DEMO_OK\n");
  return 0;
}
