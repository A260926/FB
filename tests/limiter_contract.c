/*
** Contract tests for the cmixer lookahead limiter.
**
** Every scenario drives real event sources (and on-the-fly generated WAVs)
** through cm_process() and checks the PCM against an independent reference:
** a scalar re-implementation of the mixer feeding a naive O(L) offline
** sliding-window limiter. Registered as task_cases in the groups
** "peak", "clock" and "lifecycle" (see task_support/driver.c).
*/

#include "cmixer.h"
#include "task_support/check.h"
#include "task_support/alloc_probe.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define FXU       (1 << 12)
#define MAXSRC    32
#define CAP       8192      /* frame capacity of the reference model */
#define MAXCHUNK  2048      /* largest single cm_process() call, in samples */

/*============================================================================
** Reference model
**============================================================================*/

typedef struct { short l, r; } SF;

typedef struct {
  const SF *data;
  int length, loop, lgain, rgain, rate;
  long long pos;
  int end, playing, rewindp;
} SSrc;

typedef struct {
  int enabled, L, ceiling, release;
  double invrel, g, last_gain;
  long long T;              /* frames pushed/emitted so far */
  int gainq;                /* current master gain, Q12 */
  int nsrc;
  SSrc src[MAXSRC];
  int *sumpre;              /* 2*CAP: pre-master-gain sums per frame */
  int *xg;                  /* 2*CAP: post-gain unclipped signal x per frame */
  int *fgain;               /* CAP: master gain Q12 latched per frame */
  short *out;               /* 2*CAP: expected output */
  int *contrib[MAXSRC];     /* 2*CAP each: per-source contributions */
} Sim;

static short cl16(long long v) {
  if (v < -32768) return -32768;
  if (v > 32767) return 32767;
  return (short) v;
}

static int sim_alloc(Sim *s) {
  int i;
  s->sumpre = calloc(2 * CAP, sizeof(int));
  s->xg     = calloc(2 * CAP, sizeof(int));
  s->fgain  = calloc(CAP, sizeof(int));
  s->out    = calloc(2 * CAP, sizeof(short));
  if (!s->sumpre || !s->xg || !s->fgain || !s->out) return 0;
  for (i = 0; i < MAXSRC; i++) {
    s->contrib[i] = calloc(2 * CAP, sizeof(int));
    if (!s->contrib[i]) return 0;
  }
  return 1;
}

static void sim_free(Sim *s) {
  int i;
  free(s->sumpre);
  free(s->xg);
  free(s->fgain);
  free(s->out);
  for (i = 0; i < MAXSRC; i++) free(s->contrib[i]);
}

static void sim_clear_history(Sim *s) {
  int i;
  s->T = 0;
  s->g = 1.0;
  s->last_gain = 1.0;
  memset(s->sumpre, 0, 2 * CAP * sizeof(int));
  memset(s->xg, 0, 2 * CAP * sizeof(int));
  memset(s->fgain, 0, CAP * sizeof(int));
  for (i = 0; i < MAXSRC; i++) memset(s->contrib[i], 0, 2 * CAP * sizeof(int));
}

static void sim_apply_config(Sim *s, int L, int ceiling, int release) {
  s->enabled = 1;
  s->L = L;
  s->ceiling = ceiling;
  s->release = release;
  s->invrel = 1.0 / release;
  sim_clear_history(s);
}

/* One frame of one source, mirroring process_source()'s fixed-point math */
static void sim_src_frame(SSrc *s, int *cl, int *cr) {
  int l = 0, r = 0;
  if (s->rewindp) {
    s->pos = 0;
    s->end = s->length;
    s->rewindp = 0;
  }
  if (s->playing) {
    int frame = (int) (s->pos >> 12);
    if (frame >= s->end) {
      if (s->loop) s->end = frame + s->length;
      else s->playing = 0;
    }
  }
  if (s->playing) {
    int frame = (int) (s->pos >> 12);
    if (s->rate == FXU) {
      l = s->data[frame % s->length].l;
      r = s->data[frame % s->length].r;
    } else {
      int p = (int) (s->pos & (FXU - 1));
      SF a = s->data[frame % s->length];
      SF b = s->data[(frame + 1) % s->length];
      l = a.l + (((b.l - a.l) * p) >> 12);
      r = a.r + (((b.r - a.r) * p) >> 12);
    }
    l = (l * s->lgain) >> 12;
    r = (r * s->rgain) >> 12;
    s->pos += s->rate;
  }
  *cl = l;
  *cr = r;
}

static void sim_mix_chunk(Sim *s, int frames) {
  long long t0 = s->T;
  int si, i;
  for (si = 0; si < s->nsrc; si++) {
    int *c = s->contrib[si] + 2 * t0;
    int *sum = s->sumpre + 2 * t0;
    for (i = 0; i < frames; i++) {
      int cl, cr;
      sim_src_frame(&s->src[si], &cl, &cr);
      c[2*i    ] = cl;
      c[2*i + 1] = cr;
      sum[2*i    ] += cl;
      sum[2*i + 1] += cr;
    }
  }
}

/* Naive offline limiter: rescans the whole window for every output frame */
static void sim_emit_chunk(Sim *s, int frames) {
  int i;
  for (i = 0; i < frames; i++) {
    long long t = s->T, k;
    int xl = (int) (((long long) s->sumpre[2*t    ] * s->gainq) >> 12);
    int xr = (int) (((long long) s->sumpre[2*t + 1] * s->gainq) >> 12);
    short yl = 0, yr = 0;
    s->xg[2*t    ] = xl;
    s->xg[2*t + 1] = xr;
    s->fgain[t] = s->gainq;
    if (t >= s->L) {
      long long n = t - s->L;
      int p = 0;
      double a, g;
      for (k = n; k <= n + s->L; k++) {
        int al = s->xg[2*k    ];
        int ar = s->xg[2*k + 1];
        int m;
        if (al < 0) al = -al;
        if (ar < 0) ar = -ar;
        m = al > ar ? al : ar;
        if (m > p) p = m;
      }
      a = p ? s->ceiling / (double) p : 1.0;
      if (a > 1.0) a = 1.0;
      g = s->g + s->invrel;
      if (a < g) g = a;
      s->g = g;
      s->last_gain = g;
      yl = cl16(llround(s->xg[2*n    ] * g));
      yr = cl16(llround(s->xg[2*n + 1] * g));
    }
    s->out[2*t    ] = yl;
    s->out[2*t + 1] = yr;
    s->T = t + 1;
  }
}

static void sim_emit_disabled_chunk(Sim *s, int frames) {
  int i;
  for (i = 0; i < frames; i++) {
    long long t = s->T;
    int xl = (int) (((long long) s->sumpre[2*t    ] * s->gainq) >> 12);
    int xr = (int) (((long long) s->sumpre[2*t + 1] * s->gainq) >> 12);
    s->out[2*t    ] = cl16(xl);
    s->out[2*t + 1] = cl16(xr);
    s->T = t + 1;
  }
}

/* Drop the source's queued contributions and re-derive the pending x frames;
** the envelope (g) is intentionally left alone */
static void sim_revoke(Sim *s, int si) {
  long long lo = s->T - s->L, k;
  if (lo < 0) lo = 0;
  for (k = lo; k < s->T; k++) {
    s->sumpre[2*k    ] -= s->contrib[si][2*k    ];
    s->sumpre[2*k + 1] -= s->contrib[si][2*k + 1];
    s->contrib[si][2*k    ] = 0;
    s->contrib[si][2*k + 1] = 0;
    s->xg[2*k    ] = (int) (((long long) s->sumpre[2*k    ] * s->fgain[k]) >> 12);
    s->xg[2*k + 1] = (int) (((long long) s->sumpre[2*k + 1] * s->fgain[k]) >> 12);
  }
}

/*============================================================================
** World: real mixer + reference model in lockstep
**============================================================================*/

typedef struct {
  const SF *data;
  int length;
  int idx;
} Tone;

static void tone_handler(cm_Event *e) {
  Tone *t = e->udata;
  if (e->type == CM_EVENT_SAMPLES) {
    int i, frames = e->length / 2;
    for (i = 0; i < frames; i++) {
      e->buffer[2*i    ] = t->data[t->idx].l;
      e->buffer[2*i + 1] = t->data[t->idx].r;
      t->idx++;
      if (t->idx >= t->length) t->idx = 0;
    }
  } else if (e->type == CM_EVENT_REWIND) {
    t->idx = 0;
  }
}

typedef struct {
  Sim sim;
  cm_Source *cs[MAXSRC];
  Tone tone[MAXSRC];
  unsigned char *wav[MAXSRC];
  int nsrc;
  cm_Int16 pcm[MAXCHUNK];
  short *real;              /* 2*CAP: recorded actual output */
} W;

static int lock_depth, lock_bad;
static void guard_lock(cm_Event *e) {
  if (e->type == CM_EVENT_LOCK) { if (lock_depth++) lock_bad = 1; }
  else if (e->type == CM_EVENT_UNLOCK) { if (--lock_depth) lock_bad = 1; }
}

static int world_init(W *w) {
  memset(w, 0, sizeof(*w));
  cm_init(48000);
  lock_depth = 0;
  lock_bad = 0;
  cm_set_lock(guard_lock);
  task_alloc_fail_after(-1);
  w->real = calloc(2 * CAP, sizeof(short));
  if (!w->real) return 0;
  if (!sim_alloc(&w->sim)) return 0;
  w->sim.gainq = FXU;
  w->sim.g = 1.0;
  w->sim.last_gain = 1.0;
  return 1;
}

static int world_fini(W *w) {
  int i, bad = 0;
  for (i = 0; i < w->nsrc; i++) {
    if (w->cs[i]) cm_destroy_source(w->cs[i]);
    free(w->wav[i]);
  }
  cm_disable_limiter();
  if (lock_depth != 0 || lock_bad) bad = 1;
  sim_free(&w->sim);
  free(w->real);
  return bad;
}

static void sim_set_gain_pan(SSrc *s, double gain, double pan) {
  double l, r, p = pan;
  if (p < -1.0) p = -1.0;
  if (p > 1.0) p = 1.0;
  l = gain * (p <= 0. ? 1. : 1. - p);
  r = gain * (p >= 0. ? 1. : 1. + p);
  s->lgain = (int) (l * FXU);
  s->rgain = (int) (r * FXU);
}

static void sim_init_src(SSrc *s, const SF *data, int frames, int loop,
                         double gain, double pan, double pitch) {
  s->data = data;
  s->length = frames;
  s->loop = loop;
  s->pos = 0;
  s->end = frames;
  s->playing = 0;
  s->rewindp = 1;
  sim_set_gain_pan(s, gain, pan);
  s->rate = (int) (48000 / (double) 48000 * pitch * FXU);
}

static int world_add_source(W *w, const SF *data, int frames, int loop,
                            double gain, double pan, double pitch) {
  int i;
  cm_SourceInfo info;
  if (w->nsrc >= MAXSRC) return -1;
  i = w->nsrc;
  w->tone[i].data = data;
  w->tone[i].length = frames;
  w->tone[i].idx = 0;
  info.handler = tone_handler;
  info.udata = &w->tone[i];
  info.samplerate = 48000;
  info.length = frames;
  w->cs[i] = cm_new_source(&info);
  if (!w->cs[i]) return -1;
  cm_set_gain(w->cs[i], gain);
  cm_set_pan(w->cs[i], pan);
  cm_set_pitch(w->cs[i], pitch);
  cm_set_loop(w->cs[i], loop);
  sim_init_src(&w->sim.src[i], data, frames, loop, gain, pan, pitch);
  w->nsrc++;
  w->sim.nsrc++;
  return i;
}

static void le16(unsigned char *b, unsigned x) {
  b[0] = (unsigned char) x;
  b[1] = (unsigned char) (x >> 8);
}
static void le32(unsigned char *b, unsigned x) {
  int i;
  for (i = 0; i < 4; i++) b[i] = (unsigned char) (x >> (8 * i));
}

static int world_add_wav(W *w, const SF *data, int frames, int loop,
                         double gain, double pan, double pitch) {
  int i, j;
  unsigned char *wav;
  cm_Source *cs;
  if (w->nsrc >= MAXSRC) return -1;
  wav = malloc(44 + (size_t) frames * 4);
  if (!wav) return -1;
  memcpy(wav, "RIFF", 4);
  le32(wav + 4, 36 + (unsigned) frames * 4);
  memcpy(wav + 8, "WAVEfmt ", 8);
  le32(wav + 16, 16);
  le16(wav + 20, 1);
  le16(wav + 22, 2);
  le32(wav + 24, 48000);
  le32(wav + 28, 192000);
  le16(wav + 32, 4);
  le16(wav + 34, 16);
  memcpy(wav + 36, "data", 4);
  le32(wav + 40, (unsigned) frames * 4);
  for (j = 0; j < frames; j++) {
    le16(wav + 44 + 4*j    , (unsigned short) data[j].l);
    le16(wav + 44 + 4*j + 2, (unsigned short) data[j].r);
  }
  cs = cm_new_source_from_mem(wav, 44 + frames * 4);
  if (!cs) {
    free(wav);
    return -1;
  }
  i = w->nsrc;
  w->wav[i] = wav;
  cm_set_gain(cs, gain);
  cm_set_pan(cs, pan);
  cm_set_pitch(cs, pitch);
  cm_set_loop(cs, loop);
  w->cs[i] = cs;
  sim_init_src(&w->sim.src[i], data, frames, loop, gain, pan, pitch);
  w->nsrc++;
  w->sim.nsrc++;
  return i;
}

static void world_play(W *w, int i)  { cm_play(w->cs[i]);  w->sim.src[i].playing = 1; }
static void world_pause(W *w, int i) { cm_pause(w->cs[i]); w->sim.src[i].playing = 0; }
static void world_stop(W *w, int i) {
  cm_stop(w->cs[i]);
  w->sim.src[i].playing = 0;
  w->sim.src[i].rewindp = 1;
  sim_revoke(&w->sim, i);
}
static void world_destroy(W *w, int i) {
  if (w->cs[i]) cm_destroy_source(w->cs[i]);
  w->cs[i] = NULL;
  sim_revoke(&w->sim, i);
  w->sim.src[i].playing = 0;
}
static void world_set_master_gain(W *w, double g) {
  cm_set_master_gain(g);
  w->sim.gainq = (int) (g * FXU);
}
static int world_configure(W *w, int L, int ceiling, int release) {
  int ok = cm_configure_limiter(L, ceiling, release);
  if (ok) sim_apply_config(&w->sim, L, ceiling, release);
  return ok;
}
static void world_reset(W *w)   { cm_reset_limiter();  sim_clear_history(&w->sim); }
static void world_disable(W *w) { cm_disable_limiter(); w->sim.enabled = 0; }
static int world_set_params(W *w, int ceiling, int release) {
  int ok = cm_set_limiter_parameters(ceiling, release);
  if (ok) {
    w->sim.ceiling = ceiling;
    w->sim.release = release;
    w->sim.invrel = 1.0 / release;
  }
  return ok;
}

static int world_run(W *w, int len, int tol) {
  int frames = len / 2, i;
  long long t0 = w->sim.T;
  if (t0 + frames > CAP) {
    fprintf(stderr, "reference model capacity exceeded\n");
    return 1;
  }
  sim_mix_chunk(&w->sim, frames);
  cm_process(w->pcm, len);
  if (w->sim.enabled) sim_emit_chunk(&w->sim, frames);
  else sim_emit_disabled_chunk(&w->sim, frames);
  for (i = 0; i < len; i++) {
    int got = w->pcm[i];
    int want = w->sim.out[2*t0 + i];
    w->real[2*t0 + i] = (short) got;
    if (got - want > tol || want - got > tol) {
      fprintf(stderr, "pcm mismatch at t=%lld ch=%d: got %d want %d\n",
              t0 + i / 2, i & 1, got, want);
      return 1;
    }
  }
  if (w->sim.enabled) {
    double dg = cm_get_limiter_gain() - w->sim.last_gain;
    if (dg < 0) dg = -dg;
    if (dg > 1e-9) {
      fprintf(stderr, "gain mismatch: got %f want %f\n",
              cm_get_limiter_gain(), w->sim.last_gain);
      return 1;
    }
  }
  return 0;
}

/*============================================================================
** Test data
**============================================================================*/

static SF *mkconst(int frames, int l, int r) {
  SF *d = malloc((size_t) frames * sizeof(SF));
  int i;
  if (!d) return NULL;
  for (i = 0; i < frames; i++) {
    d[i].l = (short) l;
    d[i].r = (short) r;
  }
  return d;
}

static unsigned rng_s;
static unsigned rng_next(void) {
  rng_s = rng_s * 1664525u + 1013904223u;
  return rng_s >> 8;
}

static SF *mkdata(int frames, unsigned seed, int ampl, int ampr) {
  SF *d = malloc((size_t) frames * sizeof(SF));
  int i;
  if (!d) return NULL;
  rng_s = seed;
  for (i = 0; i < frames; i++) {
    d[i].l = (short) ((int) (rng_next() % (unsigned) (2*ampl + 1)) - ampl);
    d[i].r = (short) ((int) (rng_next() % (unsigned) (2*ampr + 1)) - ampr);
  }
  return d;
}

/*============================================================================
** peak group
**============================================================================*/

/* Limiter disabled: the original per-sample path must be bit-exact */
static int pk_disabled(void) {
  W w;
  SF *d0, *d1;
  int a, b;
  TASK_CHECK(world_init(&w));
  TASK_CHECK(cm_get_limiter_latency() == 0);
  d0 = mkconst(64, 24000, -12000);
  d1 = mkdata(100, 7, 8000, 6000);
  a = world_add_source(&w, d0, 64, 1, 1.0, -0.25, 1.0);
  b = world_add_source(&w, d1, 100, 0, 0.5, 0.5, 1.0);
  TASK_CHECK(a >= 0 && b >= 0);
  world_play(&w, a);
  world_play(&w, b);
  TASK_CHECK(!world_run(&w, 512, 0));
  TASK_CHECK(!world_run(&w, 258, 0));
  world_set_master_gain(&w, 0.5);
  TASK_CHECK(!world_run(&w, 1000, 0));
  world_set_master_gain(&w, 2.0);
  TASK_CHECK(!world_run(&w, 640, 0));
  world_stop(&w, a);
  TASK_CHECK(!world_run(&w, 512, 0));
  world_destroy(&w, b);
  TASK_CHECK(!world_run(&w, 256, 0));
  /* the unmodified render path must not allocate either */
  task_alloc_counters_reset();
  TASK_CHECK(!world_run(&w, 512, 0));
  TASK_CHECK(task_alloc_calls() == 0 && task_free_calls() == 0);
  TASK_CHECK(!world_fini(&w));
  free(d0);
  free(d1);
  return 0;
}

/* A future impulse must pull the gain down before it arrives (lookahead) */
static int pk_future_impulse(void) {
  W w;
  SF *d;
  int a;
  TASK_CHECK(world_init(&w));
  /* invalid configurations are rejected without touching the state */
  TASK_CHECK(cm_configure_limiter(0, 16000, 64) == 0);
  TASK_CHECK(cm_get_error() != NULL);
  TASK_CHECK(cm_configure_limiter(70000, 16000, 64) == 0);
  TASK_CHECK(cm_configure_limiter(37, 0, 64) == 0);
  TASK_CHECK(cm_configure_limiter(37, 32768, 64) == 0);
  TASK_CHECK(cm_configure_limiter(37, 16000, 0) == 0);
  TASK_CHECK(cm_configure_limiter(37, 16000, -5) == 0);
  TASK_CHECK(cm_get_limiter_latency() == 0);
  TASK_CHECK(world_configure(&w, 37, 16000, 64));
  TASK_CHECK(cm_get_limiter_gain() == 1.0);
  d = mkconst(400, 3000, -2000);
  d[100].l = 30000;
  a = world_add_source(&w, d, 400, 0, 1.0, 0.0, 1.0);
  TASK_CHECK(a >= 0);
  world_play(&w, a);
  TASK_CHECK(cm_get_limiter_latency() == 37);
  TASK_CHECK(!world_run(&w, 512, 1));
  TASK_CHECK(!world_run(&w, 512, 1));
  TASK_CHECK(!world_run(&w, 512, 1));
  /* stream position 99 = frame 62: still outside the impulse window */
  TASK_CHECK(w.sim.out[2*99] == 3000);
  /* stream position 100 = frame 63: the gain is already pulled down by the
  ** future impulse, 37 frames before it shows up (lookahead) */
  TASK_CHECK(w.sim.out[2*100] < 3000 && w.sim.out[2*100] > 0);
  /* stream position 137 = the impulse itself, limited to the ceiling */
  TASK_CHECK(w.sim.out[2*137] <= 16000 && w.sim.out[2*137] > 15000);
  /* the right channel follows the same envelope (stereo-linked) */
  TASK_CHECK(w.sim.out[2*137+1] > -3000 && w.sim.out[2*137+1] < 0);
  TASK_CHECK(!world_fini(&w));
  free(d);
  return 0;
}

/* Equal peaks: when one leaves the window the other must still count */
static int pk_equal_peaks(void) {
  W w;
  SF *d;
  int a;
  TASK_CHECK(world_init(&w));
  TASK_CHECK(world_configure(&w, 30, 10000, 1000));
  d = mkconst(300, 500, -400);
  d[50].l = 20000;
  d[90].l = 20000;
  d[200].l = 20000;
  d[201].l = 20000;
  a = world_add_source(&w, d, 300, 0, 1.0, 0.0, 1.0);
  TASK_CHECK(a >= 0);
  world_play(&w, a);
  TASK_CHECK(!world_run(&w, 512, 1));
  TASK_CHECK(!world_run(&w, 512, 1));
  TASK_CHECK(w.sim.out[2*49] == 500);    /* frame 19: no window holds a peak yet */
  TASK_CHECK(w.sim.out[2*50] == 250);    /* frame 20: first peak enters the window */
  TASK_CHECK(w.sim.out[2*85] >= 251 && w.sim.out[2*85] <= 254); /* gap: releasing */
  TASK_CHECK(w.sim.out[2*90] == 250);    /* frame 60: second equal peak takes over */
  TASK_CHECK(w.sim.out[2*230] == 10000); /* adjacent equal pair, first peak */
  TASK_CHECK(w.sim.out[2*231] == 10000); /* adjacent equal pair, second peak */
  TASK_CHECK(!world_fini(&w));
  free(d);
  return 0;
}

/* Sum exceeds full scale, then the sources cancel each other */
static int pk_over_cancel(void) {
  W w;
  SF *d0, *d1;
  int a, b, i;
  TASK_CHECK(world_init(&w));
  TASK_CHECK(world_configure(&w, 40, 30000, 32));
  d0 = mkconst(400, 24000, 6000);
  d1 = mkconst(400, 24000, -6000);
  for (i = 200; i < 400; i++) {
    d1[i].l = -24000;
    d1[i].r = 6000;
  }
  a = world_add_source(&w, d0, 400, 0, 1.0, 0.0, 1.0);
  b = world_add_source(&w, d1, 400, 0, 1.0, 0.0, 1.0);
  TASK_CHECK(a >= 0 && b >= 0);
  world_play(&w, a);
  world_play(&w, b);
  TASK_CHECK(!world_run(&w, 512, 1));
  TASK_CHECK(!world_run(&w, 512, 1));
  TASK_CHECK(w.sim.out[2*140] == 30000);     /* 48000 * (30000/48000) */
  TASK_CHECK(w.sim.out[2*140+1] == 0);       /* cancelled right channel */
  TASK_CHECK(w.sim.out[2*290] == 0);         /* cancelled left channel */
  TASK_CHECK(w.sim.out[2*290+1] == 12000);   /* released again */
  TASK_CHECK(!world_fini(&w));
  free(d0);
  free(d1);
  return 0;
}

/* Content below the ceiling: gain stays 1, output is the delayed input */
static int pk_low(void) {
  W w;
  SF *d0, *d1;
  int a, b;
  TASK_CHECK(world_init(&w));
  TASK_CHECK(world_configure(&w, 50, 16000, 64));
  d0 = mkdata(300, 11, 4000, 3000);
  d1 = mkdata(200, 12, 2000, 2500);
  a = world_add_source(&w, d0, 300, 1, 1.0, 0.2, 1.0);
  b = world_add_source(&w, d1, 200, 1, 0.5, -0.3, 1.0);
  TASK_CHECK(a >= 0 && b >= 0);
  world_play(&w, a);
  world_play(&w, b);
  TASK_CHECK(!world_run(&w, 512, 0));
  TASK_CHECK(!world_run(&w, 512, 0));
  TASK_CHECK(!world_run(&w, 512, 0));
  TASK_CHECK(cm_get_limiter_gain() == 1.0);
  TASK_CHECK(!world_fini(&w));
  free(d0);
  free(d1);
  return 0;
}

/* 32 sources at gain 8 with a hot master gain: the enabled path's wide
** intermediate math must not overflow or truncate early */
static int pk_many_sources(void) {
  W w;
  SF *d;
  int i;
  TASK_CHECK(world_init(&w));
  TASK_CHECK(world_configure(&w, 33, 32767, 16));
  d = mkdata(260, 77, 32000, 28000);
  for (i = 0; i < 32; i++) {
    int s = world_add_source(&w, d, 260, 1, 8.0, (i % 2) ? 0.5 : -0.5, 1.0);
    TASK_CHECK(s >= 0);
    world_play(&w, s);
  }
  world_set_master_gain(&w, 8.0);
  TASK_CHECK(!world_run(&w, 512, 1));
  TASK_CHECK(!world_run(&w, 512, 1));
  TASK_CHECK(cm_get_limiter_gain() < 1.0);   /* heavily limited */
  TASK_CHECK(!world_fini(&w));
  free(d);
  return 0;
}

/* On-the-fly generated WAV through cm_new_source_from_mem() */
static int pk_wav(void) {
  W w;
  SF *d0, *d1;
  int a, b;
  TASK_CHECK(world_init(&w));
  TASK_CHECK(world_configure(&w, 45, 15000, 72));
  d0 = mkdata(500, 33, 25000, 18000);
  d1 = mkconst(500, 1000, 800);
  a = world_add_wav(&w, d0, 500, 1, 1.1, -0.2, 1.0);
  b = world_add_source(&w, d1, 500, 1, 1.0, 0.0, 1.0);
  TASK_CHECK(a >= 0 && b >= 0);
  world_play(&w, a);
  world_play(&w, b);
  TASK_CHECK(!world_run(&w, 512, 1));
  TASK_CHECK(!world_run(&w, 300, 1));
  TASK_CHECK(!world_run(&w, 1000, 1));
  TASK_CHECK(!world_run(&w, 512, 1));
  TASK_CHECK(!world_fini(&w));
  free(d0);
  free(d1);
  return 0;
}

/*============================================================================
** clock group
**============================================================================*/

/* Whole blocks and uneven chunks must produce identical PCM */
static int ck_chunk_invariance(void) {
  W w;
  SF *d0, *d1;
  short *ref;
  int i, a, b;
  static const int chunks[] = { 2, 62, 258, 1000, 512, 6, 1024, 700, 1300, 1280 };
  d0 = mkdata(180, 61, 20000, 14000);
  d1 = mkdata(220, 62, 15000, 9000);
  /* pass 1: uniform 512-sample chunks */
  TASK_CHECK(world_init(&w));
  TASK_CHECK(world_configure(&w, 37, 12000, 48));
  a = world_add_source(&w, d0, 180, 1, 1.2, 0.3, 1.5);
  b = world_add_source(&w, d1, 220, 1, 0.8, -0.4, 1.0);
  TASK_CHECK(a >= 0 && b >= 0);
  world_play(&w, 0);
  world_play(&w, 1);
  for (i = 0; i < 12; i++) TASK_CHECK(!world_run(&w, 512, 1));
  ref = malloc(6144 * sizeof(short));
  TASK_CHECK(ref != NULL);
  memcpy(ref, w.real, 6144 * sizeof(short));
  TASK_CHECK(!world_fini(&w));
  /* pass 2: irregular chunks (sums to the same 6144 samples) */
  TASK_CHECK(world_init(&w));
  TASK_CHECK(world_configure(&w, 37, 12000, 48));
  a = world_add_source(&w, d0, 180, 1, 1.2, 0.3, 1.5);
  b = world_add_source(&w, d1, 220, 1, 0.8, -0.4, 1.0);
  TASK_CHECK(a >= 0 && b >= 0);
  world_play(&w, 0);
  world_play(&w, 1);
  for (i = 0; i < 10; i++) TASK_CHECK(!world_run(&w, chunks[i], 1));
  TASK_CHECK(memcmp(ref, w.real, 6144 * sizeof(short)) == 0);
  TASK_CHECK(!world_fini(&w));
  free(ref);
  free(d0);
  free(d1);
  return 0;
}

/* Latency, the zero-length call, and rendering the tail after EOF */
static int ck_latency_tail(void) {
  W w;
  SF *d;
  int a;
  TASK_CHECK(world_init(&w));
  TASK_CHECK(world_configure(&w, 64, 16000, 64));
  d = mkconst(300, 8000, 4000);
  a = world_add_source(&w, d, 300, 0, 1.0, 0.0, 1.0);
  TASK_CHECK(a >= 0);
  world_play(&w, a);
  TASK_CHECK(cm_get_limiter_latency() == 64);
  TASK_CHECK(!world_run(&w, 512, 1));
  TASK_CHECK(w.real[0] == 0 && w.real[1] == 0);
  TASK_CHECK(w.real[2*63] == 0 && w.real[2*63+1] == 0);
  /* zero-length calls with empty dst are allowed and must not advance */
  cm_process(NULL, 0);
  cm_process(w.pcm, 0);
  TASK_CHECK(!world_run(&w, 512, 1));
  TASK_CHECK(!world_run(&w, 512, 1));
  /* the source ended at frame 300: its tail is delivered L frames later */
  TASK_CHECK(w.real[2*350] != 0);
  TASK_CHECK(w.real[2*400] == 0);
  TASK_CHECK(cm_get_limiter_gain() == 1.0);
  TASK_CHECK(!world_fini(&w));
  free(d);
  return 0;
}

/* Master gain changes must not re-amplify already queued PCM */
static int ck_master_gain_queued(void) {
  W w;
  SF *d;
  int a;
  TASK_CHECK(world_init(&w));
  TASK_CHECK(world_configure(&w, 100, 16000, 64));
  d = mkconst(600, 20000, 10000);
  a = world_add_source(&w, d, 600, 1, 1.0, 0.0, 1.0);
  TASK_CHECK(a >= 0);
  world_play(&w, a);
  TASK_CHECK(!world_run(&w, 300, 1));
  world_set_master_gain(&w, 2.0);
  TASK_CHECK(!world_run(&w, 300, 1));
  TASK_CHECK(!world_run(&w, 300, 1));
  TASK_CHECK(w.sim.fgain[75] == 4096);    /* queued before the change */
  TASK_CHECK(w.sim.fgain[150] == 8192);   /* entered after the change */
  /* frame 75 keeps its old gain: 20000 * (16000/40000) == 8000, not 16000 */
  TASK_CHECK(w.sim.out[2*175] == 8000);
  TASK_CHECK(w.sim.out[2*250] == 16000);
  TASK_CHECK(!world_fini(&w));
  free(d);
  return 0;
}

/* Half-speed source crossing the ring preread boundary, plus a 2x source */
static int ck_half_speed(void) {
  W w;
  SF *d0, *d1;
  int a, b, i;
  TASK_CHECK(world_init(&w));
  TASK_CHECK(world_configure(&w, 37, 14000, 96));
  d0 = mkdata(300, 21, 20000, 15000);
  d1 = mkdata(130, 22, 8000, 9000);
  a = world_add_source(&w, d0, 300, 1, 1.2, 0.15, 0.5);
  b = world_add_source(&w, d1, 130, 0, 0.7, -0.6, 2.0);
  TASK_CHECK(a >= 0 && b >= 0);
  world_play(&w, a);
  world_play(&w, b);
  for (i = 0; i < 6; i++) TASK_CHECK(!world_run(&w, 512, 1));
  TASK_CHECK(!world_fini(&w));
  free(d0);
  free(d1);
  return 0;
}

/* Fixed maximal L and a long stream: work must grow linearly with input
** (a quadratic implementation could not finish this in reasonable time) */
static int ck_linear(void) {
  enum { FRAMES = 2000000, L = 65536 };
  W w;
  SF *d;
  int a;
  long long t = 0;
  TASK_CHECK(world_init(&w));
  TASK_CHECK(world_configure(&w, L, 32767, 1000));
  d = mkdata(8192, 31, 1000, 700);
  a = world_add_source(&w, d, 8192, 1, 1.0, 0.0, 1.0);
  TASK_CHECK(a >= 0);
  world_play(&w, a);
  TASK_CHECK(cm_get_limiter_latency() == L);
  task_alloc_counters_reset();
  while (t < FRAMES) {
    int i;
    cm_process(w.pcm, 512);
    for (i = 0; i < 256; i++, t++) {
      short wl = 0, wr = 0;
      if (t >= L) {
        wl = d[(t - L) % 8192].l;
        wr = d[(t - L) % 8192].r;
      }
      if (w.pcm[2*i] != wl || w.pcm[2*i+1] != wr) {
        fprintf(stderr, "linear mismatch at t=%lld\n", t);
        TASK_CHECK(0);
      }
    }
  }
  TASK_CHECK(task_alloc_calls() == 0 && task_free_calls() == 0);
  TASK_CHECK(cm_get_limiter_gain() == 1.0);
  TASK_CHECK(!world_fini(&w));
  free(d);
  return 0;
}

/*============================================================================
** lifecycle group
**============================================================================*/

/* Stopping a strong source revokes its queued audio; the rest keeps playing
** and the envelope releases from its current value */
static int lc_revoke_strong(void) {
  W w;
  SF *d0, *d1;
  int a, b, t, nz = 0;
  TASK_CHECK(world_init(&w));
  TASK_CHECK(world_configure(&w, 50, 16000, 40));
  d0 = mkconst(500, 28000, -14000);
  d1 = mkdata(500, 41, 2000, 1000);
  a = world_add_source(&w, d0, 500, 1, 1.0, 0.0, 1.0);
  b = world_add_source(&w, d1, 500, 1, 1.0, 0.0, 1.0);
  TASK_CHECK(a >= 0 && b >= 0);
  world_play(&w, a);
  world_play(&w, b);
  TASK_CHECK(!world_run(&w, 400, 1));
  world_stop(&w, a);
  TASK_CHECK(!world_run(&w, 400, 1));
  /* the weak source survives and the gain releases upward */
  for (t = 200; t < 300; t++) if (w.sim.out[2*t]) nz = 1;
  TASK_CHECK(nz);
  TASK_CHECK(cm_get_limiter_gain() > 0.9);
  /* replaying restarts the source from the beginning */
  world_play(&w, a);
  TASK_CHECK(!world_run(&w, 400, 1));
  TASK_CHECK(!world_fini(&w));
  free(d0);
  free(d1);
  return 0;
}

/* Runtime parameter changes apply to the next emitted frame; invalid ones
** are rejected without touching the state */
static int lc_param_redirect(void) {
  W w;
  SF *d;
  int a;
  TASK_CHECK(world_init(&w));
  TASK_CHECK(world_configure(&w, 40, 16000, 64));
  d = mkconst(500, 30000, 15000);
  a = world_add_source(&w, d, 500, 1, 1.0, 0.0, 1.0);
  TASK_CHECK(a >= 0);
  world_play(&w, a);
  TASK_CHECK(!world_run(&w, 200, 1));
  TASK_CHECK(world_set_params(&w, 8000, 16) == 1);
  TASK_CHECK(!world_run(&w, 200, 1));
  TASK_CHECK(w.sim.out[2*(w.sim.T-1)] == 8000);
  TASK_CHECK(cm_set_limiter_parameters(0, 16) == 0);
  TASK_CHECK(cm_set_limiter_parameters(100, 0) == 0);
  TASK_CHECK(cm_set_limiter_parameters(32768, 1) == 0);
  TASK_CHECK(!world_run(&w, 100, 1));
  TASK_CHECK(!world_fini(&w));
  /* rejected while disabled */
  TASK_CHECK(world_init(&w));
  TASK_CHECK(cm_set_limiter_parameters(8000, 16) == 0);
  TASK_CHECK(!world_fini(&w));
  free(d);
  return 0;
}

/* A short source's tail is delivered after its natural EOF */
static int lc_short_tail(void) {
  W w;
  SF *d;
  int a;
  TASK_CHECK(world_init(&w));
  TASK_CHECK(world_configure(&w, 60, 16000, 50));
  d = mkconst(120, 20000, 8000);
  a = world_add_source(&w, d, 120, 0, 1.0, 0.0, 1.0);
  TASK_CHECK(a >= 0);
  world_play(&w, a);
  TASK_CHECK(!world_run(&w, 512, 1));
  TASK_CHECK(cm_get_state(w.cs[a]) == CM_STATE_STOPPED);
  TASK_CHECK(w.real[2*150] != 0);   /* frame 90, still inside the source */
  TASK_CHECK(w.real[2*200] == 0);   /* frame 140, past its end */
  TASK_CHECK(!world_run(&w, 512, 1));
  TASK_CHECK(w.real[2*300] == 0);
  TASK_CHECK(!world_fini(&w));
  free(d);
  return 0;
}

/* Destroying a source after its natural EOF still revokes its queued audio */
static int lc_eof_selective_destroy(void) {
  W w;
  SF *d0, *d1;
  int a, b;
  TASK_CHECK(world_init(&w));
  TASK_CHECK(world_configure(&w, 60, 16000, 50));
  d0 = mkconst(200, 25000, 12000);
  d1 = mkconst(600, 3000, 1500);
  a = world_add_source(&w, d0, 200, 0, 1.0, 0.0, 1.0);
  b = world_add_source(&w, d1, 600, 1, 1.0, 0.0, 1.0);
  TASK_CHECK(a >= 0 && b >= 0);
  world_play(&w, a);
  world_play(&w, b);
  TASK_CHECK(!world_run(&w, 460, 1));
  TASK_CHECK(cm_get_state(w.cs[a]) == CM_STATE_STOPPED);
  world_destroy(&w, a);
  TASK_CHECK(!world_run(&w, 200, 1));
  /* frame 180 would have been dominated by the destroyed source */
  TASK_CHECK(w.real[2*240] < 4000 && w.real[2*240] > -4000);
  TASK_CHECK(!world_fini(&w));
  free(d0);
  free(d1);
  return 0;
}

/* Reconfiguring while a source is paused must keep that source functional */
static int lc_paused_reconfig(void) {
  W w;
  SF *d0, *d1;
  int a, b;
  TASK_CHECK(world_init(&w));
  TASK_CHECK(world_configure(&w, 40, 16000, 64));
  d0 = mkconst(500, 20000, 10000);
  d1 = mkdata(500, 51, 4000, 3000);
  a = world_add_source(&w, d0, 500, 1, 1.0, 0.0, 1.0);
  b = world_add_source(&w, d1, 500, 1, 1.0, 0.0, 1.0);
  TASK_CHECK(a >= 0 && b >= 0);
  world_play(&w, a);
  world_play(&w, b);
  TASK_CHECK(!world_run(&w, 200, 1));
  world_pause(&w, a);
  TASK_CHECK(world_configure(&w, 80, 12000, 32) == 1);
  TASK_CHECK(cm_get_limiter_latency() == 80);
  TASK_CHECK(!world_run(&w, 200, 1));
  TASK_CHECK(w.real[0] == 0 && w.real[2*79] == 0);  /* fresh delay */
  world_play(&w, a);
  TASK_CHECK(!world_run(&w, 400, 1));
  TASK_CHECK(!world_fini(&w));
  free(d0);
  free(d1);
  return 0;
}

/* Stopping and destroying sources that are not at the head of the list */
static int lc_non_head(void) {
  W w;
  SF *d0, *d1, *d2;
  int a, b, c;
  TASK_CHECK(world_init(&w));
  TASK_CHECK(world_configure(&w, 50, 16000, 40));
  d0 = mkconst(400, 10000, 5000);
  d1 = mkconst(400, 8000, 4000);
  d2 = mkconst(400, 6000, 3000);
  a = world_add_source(&w, d0, 400, 1, 1.0, 0.0, 1.0);
  b = world_add_source(&w, d1, 400, 1, 1.0, 0.0, 1.0);
  c = world_add_source(&w, d2, 400, 1, 1.0, 0.0, 1.0);
  TASK_CHECK(a >= 0 && b >= 0 && c >= 0);
  world_play(&w, a);
  world_play(&w, b);
  world_play(&w, c);
  TASK_CHECK(!world_run(&w, 200, 1));
  world_stop(&w, b);        /* middle of the active list */
  TASK_CHECK(!world_run(&w, 200, 1));
  world_destroy(&w, a);     /* tail of the active list */
  TASK_CHECK(!world_run(&w, 200, 1));
  world_destroy(&w, c);     /* head of the active list */
  TASK_CHECK(!world_run(&w, 200, 1));
  world_destroy(&w, b);     /* already stopped */
  TASK_CHECK(!world_run(&w, 100, 1));
  TASK_CHECK(!world_fini(&w));
  free(d0);
  free(d1);
  free(d2);
  return 0;
}

/* Stop/replay and address reuse: old contributions must not come back */
static int lc_stop_replay_reuse(void) {
  W w;
  SF *d0, *d1, *d2;
  int a, b, c;
  TASK_CHECK(world_init(&w));
  TASK_CHECK(world_configure(&w, 50, 16000, 40));
  d0 = mkconst(300, 26000, 13000);
  d1 = mkconst(600, 2000, 1000);
  a = world_add_source(&w, d0, 300, 1, 1.0, 0.0, 1.0);
  b = world_add_source(&w, d1, 600, 1, 1.0, 0.0, 1.0);
  TASK_CHECK(a >= 0 && b >= 0);
  world_play(&w, a);
  world_play(&w, b);
  TASK_CHECK(!world_run(&w, 300, 1));
  world_stop(&w, a);
  world_play(&w, a);        /* replays from the beginning */
  TASK_CHECK(!world_run(&w, 300, 1));
  world_destroy(&w, a);
  d2 = mkconst(300, 5000, 2500);
  c = world_add_source(&w, d2, 300, 1, 1.0, 0.0, 1.0);
  TASK_CHECK(c >= 0);
  world_play(&w, c);
  TASK_CHECK(!world_run(&w, 300, 1));
  TASK_CHECK(!world_fini(&w));
  free(d0);
  free(d1);
  free(d2);
  return 0;
}

/* Reset clears the history but keeps the configuration; disable returns to
** the immediate output path */
static int lc_reset_disable(void) {
  W w;
  SF *d;
  int a;
  TASK_CHECK(world_init(&w));
  TASK_CHECK(world_configure(&w, 40, 16000, 64));
  d = mkconst(600, 22000, 11000);
  a = world_add_source(&w, d, 600, 1, 1.0, 0.0, 1.0);
  TASK_CHECK(a >= 0);
  world_play(&w, a);
  TASK_CHECK(!world_run(&w, 200, 1));
  world_reset(&w);
  TASK_CHECK(cm_get_limiter_latency() == 40);
  TASK_CHECK(!world_run(&w, 200, 1));
  TASK_CHECK(w.real[0] == 0 && w.real[2*39] == 0);  /* delay restarts */
  TASK_CHECK(w.real[2*40] != 0);
  world_disable(&w);
  TASK_CHECK(cm_get_limiter_latency() == 0);
  TASK_CHECK(!world_run(&w, 200, 1));
  TASK_CHECK(w.real[2*100] != 0);                    /* immediate again */
  TASK_CHECK(world_configure(&w, 40, 16000, 64) == 1);
  TASK_CHECK(!world_run(&w, 200, 1));
  TASK_CHECK(w.real[0] == 0);                        /* delay once more */
  TASK_CHECK(!world_fini(&w));
  free(d);
  return 0;
}

/* Allocation failures: reconfiguration and source creation must fail
** atomically, and cm_process() must never allocate */
static int lc_alloc_failure(void) {
  W w;
  SF *d0, *d1;
  int a, b, c, k, ok = 0, failed = 0;
  TASK_CHECK(world_init(&w));
  TASK_CHECK(world_configure(&w, 40, 16000, 64));
  d0 = mkconst(400, 20000, 10000);
  d1 = mkconst(400, 5000, 2500);
  a = world_add_source(&w, d0, 400, 1, 1.0, 0.0, 1.0);
  b = world_add_source(&w, d1, 400, 1, 1.0, 0.0, 1.0);
  TASK_CHECK(a >= 0 && b >= 0);
  world_play(&w, a);
  world_play(&w, b);
  TASK_CHECK(!world_run(&w, 512, 1));
  /* every allocation point of a reconfigure: failure keeps the old output */
  for (k = 0; k < 64 && !ok; k++) {
    int r;
    task_alloc_fail_after(k);
    r = cm_configure_limiter(80, 12000, 32);
    task_alloc_fail_after(-1);
    if (r) {
      ok = 1;
      sim_apply_config(&w.sim, 80, 12000, 32);
    } else {
      failed++;
      TASK_CHECK(!world_run(&w, 64, 1));
    }
  }
  TASK_CHECK(ok);
  TASK_CHECK(failed > 0);   /* prove failures were actually injected */
  TASK_CHECK(!world_run(&w, 512, 1));
  /* source construction failure must not disturb the existing output */
  {
    Tone *t = &w.tone[w.nsrc];
    cm_SourceInfo info;
    t->data = d1;
    t->length = 400;
    t->idx = 0;
    info.handler = tone_handler;
    info.udata = t;
    info.samplerate = 48000;
    info.length = 400;
    task_alloc_fail_after(0);
    TASK_CHECK(cm_new_source(&info) == NULL);
    TASK_CHECK(cm_get_error() != NULL);
    task_alloc_fail_after(1);
    TASK_CHECK(cm_new_source(&info) == NULL);
    task_alloc_fail_after(-1);
  }
  TASK_CHECK(!world_run(&w, 512, 1));
  /* ... and a later successful creation works again */
  c = world_add_source(&w, d1, 400, 1, 1.0, 0.0, 1.0);
  TASK_CHECK(c >= 0);
  world_destroy(&w, c);
  TASK_CHECK(!world_run(&w, 128, 1));
  /* the render path itself never allocates or frees */
  task_alloc_counters_reset();
  TASK_CHECK(!world_run(&w, 512, 1));
  TASK_CHECK(task_alloc_calls() == 0 && task_free_calls() == 0);
  TASK_CHECK(!world_fini(&w));
  free(d0);
  free(d1);
  return 0;
}

/*============================================================================
** Registration
**============================================================================*/

const struct task_case task_cases[] = {
  { "peak", "disabled_passthrough", pk_disabled },
  { "peak", "future_impulse", pk_future_impulse },
  { "peak", "equal_peak_expiry", pk_equal_peaks },
  { "peak", "over_full_scale_cancel", pk_over_cancel },
  { "peak", "low_amplitude", pk_low },
  { "peak", "many_sources_full_scale", pk_many_sources },
  { "peak", "wav_stream", pk_wav },
  { "clock", "chunk_invariance", ck_chunk_invariance },
  { "clock", "latency_and_tail", ck_latency_tail },
  { "clock", "master_gain_queued", ck_master_gain_queued },
  { "clock", "half_speed_ring_boundary", ck_half_speed },
  { "clock", "linear_scaling", ck_linear },
  { "lifecycle", "revoke_strong_source", lc_revoke_strong },
  { "lifecycle", "param_redirect", lc_param_redirect },
  { "lifecycle", "short_source_tail", lc_short_tail },
  { "lifecycle", "eof_selective_destroy", lc_eof_selective_destroy },
  { "lifecycle", "paused_reconfig", lc_paused_reconfig },
  { "lifecycle", "non_head_stop_destroy", lc_non_head },
  { "lifecycle", "stop_replay_reuse", lc_stop_replay_reuse },
  { "lifecycle", "reset_disable", lc_reset_disable },
  { "lifecycle", "alloc_failure", lc_alloc_failure },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
