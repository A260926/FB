/*
** Limiter demo: mixes two oscillators hot enough to clip, enables the
** lookahead limiter (L=37, ceiling=16000, release=64 frames) and writes the
** limited s16le stereo stream — including the latency tail — to the file
** named by the sole argument. Prints LIMITER_DEMO_OK on success.
*/

#include "cmixer.h"

#include <stdio.h>
#include <math.h>

#define PI    3.14159265358979323846
#define RATE  44100

typedef struct {
  double phase;
  double step;
  int ampl;
  int ampr;
} Osc;

static void osc_handler(cm_Event *e) {
  Osc *o = e->udata;
  if (e->type == CM_EVENT_SAMPLES) {
    int i, frames = e->length / 2;
    for (i = 0; i < frames; i++) {
      double s = sin(o->phase);
      o->phase += o->step;
      if (o->phase >= 2.0 * PI) o->phase -= 2.0 * PI;
      e->buffer[2*i    ] = (cm_Int16) (s * o->ampl);
      e->buffer[2*i + 1] = (cm_Int16) (s * o->ampr);
    }
  } else if (e->type == CM_EVENT_REWIND) {
    o->phase = 0.0;
  }
}

int main(int argc, char **argv) {
  static Osc osc_a, osc_b;
  cm_SourceInfo ia, ib;
  cm_Source *sa, *sb;
  FILE *fp;
  cm_Int16 buf[512];
  long frames, total;
  int i, peak = 0, nonzero = 0;

  if (argc != 2) {
    fprintf(stderr, "usage: %s <output.pcm>\n", argv[0]);
    return 1;
  }

  cm_init(RATE);
  if (!cm_configure_limiter(37, 16000, 64)) {
    fprintf(stderr, "cm_configure_limiter failed: %s\n", cm_get_error());
    return 1;
  }

  osc_a.phase = 0.0;
  osc_a.step = 2.0 * PI * 220.0 / RATE;
  osc_a.ampl = 24000;
  osc_a.ampr = 14000;
  osc_b.phase = 0.0;
  osc_b.step = 2.0 * PI * 331.0 / RATE;
  osc_b.ampl = 15000;
  osc_b.ampr = 22000;

  ia.handler = osc_handler;
  ia.udata = &osc_a;
  ia.samplerate = RATE;
  ia.length = RATE;
  ib.handler = osc_handler;
  ib.udata = &osc_b;
  ib.samplerate = RATE;
  ib.length = RATE;

  sa = cm_new_source(&ia);
  sb = cm_new_source(&ib);
  if (!sa || !sb) {
    fprintf(stderr, "cm_new_source failed: %s\n", cm_get_error());
    return 1;
  }
  cm_set_pan(sa, -0.3);
  cm_set_pan(sb, 0.3);
  cm_play(sa);
  cm_play(sb);

  fp = fopen(argv[1], "wb");
  if (!fp) {
    fprintf(stderr, "could not open %s\n", argv[1]);
    return 1;
  }

  /* Render the whole mix plus the limiter's latency tail */
  total = RATE + cm_get_limiter_latency() + 256;
  for (frames = 0; frames < total; frames += 256) {
    cm_process(buf, 512);
    for (i = 0; i < 512; i++) {
      int v = buf[i];
      int a = v < 0 ? -v : v;
      if (a > peak) peak = a;
      if (v) nonzero = 1;
      fputc(v & 0xff, fp);
      fputc((v >> 8) & 0xff, fp);
    }
  }
  if (fclose(fp)) {
    fprintf(stderr, "could not write %s\n", argv[1]);
    return 1;
  }

  cm_destroy_source(sa);
  cm_destroy_source(sb);
  cm_disable_limiter();

  if (!nonzero || peak > 16000) {
    fprintf(stderr, "bad limiter output: peak=%d nonzero=%d\n", peak, nonzero);
    return 1;
  }
  printf("LIMITER_DEMO_OK\n");
  return 0;
}
