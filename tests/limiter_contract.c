/*
** tests/limiter_contract.c -- contract tests for the cmixer lookahead
** limiter. Real event sources and in-memory WAVs are rendered through
** cm_process(); the output is checked against an independent naive
** offline model (O(L) sliding window plus the scalar gain recursion).
*/

#include "task_support/check.h"
#include "task_support/alloc_probe.h"
#include "cmixer.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

#define TSR   48000
#define FXB   12
#define FXU   (1 << FXB)

/* ------------------------------------------------------------------ */
/* deterministic generator source (real cmixer source)                 */
/* ------------------------------------------------------------------ */
typedef struct {
  const cm_Int16 *data;   /* stereo interleaved */
  long frames;
  long pos;
  int loop;
} Gen;

static void gen_handler(cm_Event *e) {
  Gen *g = (Gen*) e->udata;
  int i;
  switch (e->type) {
    case CM_EVENT_SAMPLES:
      for (i = 0; i < e->length; i += 2) {
        long f = g->pos++;
        if (g->loop && g->frames > 0) {
          f %= g->frames;
        }
        e->buffer[i]     = (f < g->frames) ? g->data[2*f]     : 0;
        e->buffer[i + 1] = (f < g->frames) ? g->data[2*f + 1] : 0;
      }
      break;
    case CM_EVENT_REWIND:
      g->pos = 0;
      break;
  }
}

/* ------------------------------------------------------------------ */
/* model source: mirrors cmixer's playhead / EOF / interpolation       */
/* ------------------------------------------------------------------ */
typedef struct {
  const cm_Int16 *data;
  long frames;
  int lgain, rgain, rate;
  cm_Int64 position;      /* Q12 frame playhead */
  int end;                /* end frame of the play-through */
  int nextfill;           /* mirrors cmixer's prefetch bookkeeping */
  int state;              /* CM_STATE_* */
  int rewind;
} MSrc;

static void msrc_emit(MSrc *m, cm_Int32 *ol, cm_Int32 *or_) {
  long fi = (long) (m->position >> FXB);
  if (m->rate == FXU) {
    cm_Int32 a = fi < m->frames ? m->data[2*fi]     : 0;
    cm_Int32 b = fi < m->frames ? m->data[2*fi + 1] : 0;
    *ol = (a * m->lgain) >> FXB;
    *or_ = (b * m->rgain) >> FXB;
  } else {
    int p = (int) (m->position & (FXU - 1));
    cm_Int32 a0 = fi < m->frames     ? m->data[2*fi]         : 0;
    cm_Int32 b0 = fi + 1 < m->frames ? m->data[2*(fi+1)]     : 0;
    cm_Int32 a1 = fi < m->frames     ? m->data[2*fi + 1]     : 0;
    cm_Int32 b1 = fi + 1 < m->frames ? m->data[2*(fi+1) + 1] : 0;
    *ol = ((a0 + (((b0 - a0) * p) >> FXB)) * m->lgain) >> FXB;
    *or_ = ((a1 + (((b1 - a1) * p) >> FXB)) * m->rgain) >> FXB;
  }
  m->position += m->rate;
}

/* Mirrors process_source(): emits `frames` per-frame contributions. */
static void msrc_process(MSrc *m, cm_Int32 *ol, cm_Int32 *or_, int frames) {
  int rem = frames, i;
  if (m->rewind) {
    m->position = 0;
    m->end = (int) m->frames;
    m->nextfill = 0;
    m->rewind = 0;
  }
  if (m->state != CM_STATE_PLAYING) {
    memset(ol, 0, frames * sizeof(*ol));
    memset(or_, 0, frames * sizeof(*or_));
    return;
  }
  while (rem > 0) {
    int frame = (int) (m->position >> FXB);
    int n, count;
    if (frame + 3 >= m->nextfill) {
      m->nextfill += 128;
    }
    if (frame >= m->end) {
      m->end = frame + (int) m->frames;
      m->state = CM_STATE_STOPPED;
      break;
    }
    n = (m->nextfill - 2 < m->end ? m->nextfill - 2 : m->end) - frame;
    count = (n << FXB) / m->rate;
    if (count < 1) {
      count = 1;
    }
    if (count > rem) {
      count = rem;
    }
    for (i = 0; i < count; i++) {
      msrc_emit(m, ol++, or_++);
    }
    rem -= count;
  }
  for (i = 0; i < rem; i++) {
    ol[i] = 0;
    or_[i] = 0;
  }
}

/* ------------------------------------------------------------------ */
/* rig: real sources + naive limiter model in lockstep                 */
/* ------------------------------------------------------------------ */
#define RIG_MAX_SRC 40

typedef struct {
  long cap;
  long t;                 /* frames fed so far */
  int L, ceiling, release;
  int mgain;              /* Q12 */
  int disabled;           /* model the classic path instead */
  double g;
  cm_Int32 *S;            /* 2*cap: pre-gain sums */
  cm_Int32 *mg;           /* cap: master gain per frame */
  cm_Int16 *exp;          /* 2*cap: expected output */
  int nsrc;
  struct {
    cm_Source *src;
    Gen gen;
    MSrc m;
    cm_Int32 *cl, *cr;    /* per-frame contribution history */
    int alive;
  } s[RIG_MAX_SRC];
} Rig;

static void rig_init(Rig *r, long cap, int L, int ceiling, int release) {
  memset(r, 0, sizeof(*r));
  r->cap = cap;
  r->L = L;
  r->ceiling = ceiling;
  r->release = release;
  r->mgain = FXU;
  r->g = 1.0;
  r->S   = (cm_Int32*) malloc(sizeof(cm_Int32) * 2 * cap);
  r->mg  = (cm_Int32*) malloc(sizeof(cm_Int32) * cap);
  r->exp = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * cap);
}

static void rig_free(Rig *r) {
  int i;
  free(r->S);
  free(r->mg);
  free(r->exp);
  for (i = 0; i < r->nsrc; i++) {
    free(r->s[i].cl);
    free(r->s[i].cr);
  }
}

static void rig_teardown(Rig *r) {
  int i;
  for (i = 0; i < r->nsrc; i++) {
    if (r->s[i].alive) {
      cm_destroy_source(r->s[i].src);
    }
  }
  cm_disable_limiter();
  rig_free(r);
}

static cm_Int16 clamp16(long v) {
  return (cm_Int16) (v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

static cm_Int32 rig_x(const Rig *r, long n, int ch) {
  return (cm_Int32) (((cm_Int64) r->S[2*n + ch] * r->mg[n]) >> FXB);
}

/* Naive model: O(L) window scan + scalar gain recursion, one frame. */
static void rig_feed(Rig *r, cm_Int32 sl, cm_Int32 sr) {
  r->S[2*r->t]     = sl;
  r->S[2*r->t + 1] = sr;
  r->mg[r->t]      = r->mgain;
  if (r->disabled) {
    r->exp[2*r->t]     = clamp16((cm_Int32) (sl * r->mgain) >> FXB);
    r->exp[2*r->t + 1] = clamp16((cm_Int32) (sr * r->mgain) >> FXB);
  } else if (r->t >= r->L) {
    long n = r->t - r->L, k;
    cm_Int32 p = 0;
    double a, g;
    for (k = n; k <= n + r->L; k++) {
      cm_Int32 xl = rig_x(r, k, 0), xr = rig_x(r, k, 1);
      cm_Int32 al = xl < 0 ? -xl : xl;
      cm_Int32 ar = xr < 0 ? -xr : xr;
      cm_Int32 m = al > ar ? al : ar;
      if (m > p) {
        p = m;
      }
    }
    a = p == 0 ? 1.0 : (double) r->ceiling / (double) p;
    if (a > 1.0) {
      a = 1.0;
    }
    g = r->g + 1.0 / (double) r->release;
    if (a < g) {
      g = a;
    }
    r->g = g;
    r->exp[2*r->t]     = clamp16(lround(rig_x(r, n, 0) * g));
    r->exp[2*r->t + 1] = clamp16(lround(rig_x(r, n, 1) * g));
  } else {
    r->exp[2*r->t]     = 0;
    r->exp[2*r->t + 1] = 0;
  }
  r->t++;
}

/* Drives the model sources through one sub-chunk (matching cm_process's
** internal 512-sample split) and feeds the naive limiter model. */
static void rig_model_subchunk(Rig *r, int frames) {
  int i, f;
  long base = r->t;
  for (i = 0; i < r->nsrc; i++) {
    if (r->s[i].alive) {
      msrc_process(&r->s[i].m, r->s[i].cl + base, r->s[i].cr + base, frames);
    } else {
      memset(r->s[i].cl + base, 0, frames * sizeof(cm_Int32));
      memset(r->s[i].cr + base, 0, frames * sizeof(cm_Int32));
    }
  }
  for (f = 0; f < frames; f++) {
    cm_Int32 sl = 0, sr = 0;
    for (i = 0; i < r->nsrc; i++) {
      sl += r->s[i].cl[base + f];
      sr += r->s[i].cr[base + f];
    }
    rig_feed(r, sl, sr);
  }
}

/* Renders `frames` frames through the real mixer and returns the number
** of samples that differ from the naive model. */
static int rig_process(Rig *r, cm_Int16 *dst, int frames) {
  long base = r->t;
  int rem = frames, f, bad = 0;
  cm_process(dst, frames * 2);
  while (rem > 0) {
    int sub = rem > 256 ? 256 : rem;
    rig_model_subchunk(r, sub);
    rem -= sub;
  }
  for (f = 0; f < frames * 2; f++) {
    if (dst[f] != r->exp[2*base + f]) {
      if (bad == 0) {
        fprintf(stderr, "  first mismatch at frame %ld ch %ld: got %d want %d\n",
                base + f/2, (long) (f & 1), dst[f], r->exp[2*base + f]);
      }
      bad++;
    }
  }
  return bad;
}

static void rig_retract(Rig *r, int i) {
  long lo = r->t - r->L, k;
  if (lo < 0) {
    lo = 0;
  }
  for (k = lo; k < r->t; k++) {
    r->S[2*k]     -= r->s[i].cl[k];
    r->S[2*k + 1] -= r->s[i].cr[k];
    r->s[i].cl[k] = 0;
    r->s[i].cr[k] = 0;
  }
}

static void rig_play(Rig *r, int i) {
  cm_play(r->s[i].src);
  r->s[i].m.state = CM_STATE_PLAYING;
}

static void rig_pause(Rig *r, int i) {
  cm_pause(r->s[i].src);
  r->s[i].m.state = CM_STATE_PAUSED;
}

static void rig_stop(Rig *r, int i) {
  cm_stop(r->s[i].src);
  rig_retract(r, i);
  r->s[i].m.state = CM_STATE_STOPPED;
  r->s[i].m.rewind = 1;
}

static void rig_destroy(Rig *r, int i) {
  cm_destroy_source(r->s[i].src);
  rig_retract(r, i);
  r->s[i].alive = 0;
}

static void rig_master_gain(Rig *r, double gain) {
  cm_set_master_gain(gain);
  r->mgain = (int) (gain * FXU);
}

/* Model state after a successful (re)configure or reset: queued PCM and
** envelope dropped, sources keep their positions. */
static void rig_model_reset(Rig *r, int L, int ceiling, int release) {
  int i;
  r->t = 0;
  r->g = 1.0;
  r->L = L;
  r->ceiling = ceiling;
  r->release = release;
  for (i = 0; i < r->nsrc; i++) {
    memset(r->s[i].cl, 0, r->cap * sizeof(cm_Int32));
    memset(r->s[i].cr, 0, r->cap * sizeof(cm_Int32));
  }
}

static void rig_source_model(Rig *r, int i, const cm_Int16 *data, long frames,
                             double gain, double pan, double pitch) {
  double l, rr;
  l  = gain * (pan <= 0. ? 1. : 1. - pan);
  rr = gain * (pan >= 0. ? 1. : 1. + pan);
  r->s[i].m.lgain = (int) (l * FXU);
  r->s[i].m.rgain = (int) (rr * FXU);
  r->s[i].m.rate  = (int) ((TSR / (double) TSR) * pitch * FXU);
  r->s[i].m.data  = data;
  r->s[i].m.frames = frames;
  r->s[i].m.position = 0;
  r->s[i].m.end   = (int) frames;
  r->s[i].m.nextfill = 0;
  r->s[i].m.state = CM_STATE_STOPPED;
  r->s[i].m.rewind = 1;
}

static int rig_add_source(Rig *r, const cm_Int16 *data, long frames,
                          double gain, double pan, double pitch) {
  cm_SourceInfo info;
  int i = r->nsrc++;
  memset(&info, 0, sizeof(info));
  r->s[i].gen.data = data;
  r->s[i].gen.frames = frames;
  r->s[i].gen.pos = 0;
  r->s[i].gen.loop = 0;
  info.handler = gen_handler;
  info.udata = &r->s[i].gen;
  info.samplerate = TSR;
  info.length = (int) frames;
  r->s[i].src = cm_new_source(&info);
  if (!r->s[i].src) {
    r->nsrc--;
    return -1;
  }
  cm_set_gain(r->s[i].src, gain);
  cm_set_pan(r->s[i].src, pan);
  cm_set_pitch(r->s[i].src, pitch);
  rig_source_model(r, i, data, frames, gain, pan, pitch);
  r->s[i].cl = (cm_Int32*) calloc(r->cap, sizeof(cm_Int32));
  r->s[i].cr = (cm_Int32*) calloc(r->cap, sizeof(cm_Int32));
  r->s[i].alive = 1;
  return i;
}

static int rig_add_wav_source(Rig *r, void *wav, int size,
                              const cm_Int16 *data, long frames,
                              double gain, double pan, double pitch) {
  int i = r->nsrc++;
  r->s[i].gen.data = NULL;
  r->s[i].gen.frames = 0;
  r->s[i].gen.pos = 0;
  r->s[i].gen.loop = 0;
  r->s[i].src = cm_new_source_from_mem(wav, size);
  if (!r->s[i].src) {
    r->nsrc--;
    return -1;
  }
  cm_set_gain(r->s[i].src, gain);
  cm_set_pan(r->s[i].src, pan);
  cm_set_pitch(r->s[i].src, pitch);
  rig_source_model(r, i, data, frames, gain, pan, pitch);
  r->s[i].cl = (cm_Int32*) calloc(r->cap, sizeof(cm_Int32));
  r->s[i].cr = (cm_Int32*) calloc(r->cap, sizeof(cm_Int32));
  r->s[i].alive = 1;
  return i;
}

/* ------------------------------------------------------------------ */
/* data helpers                                                        */
/* ------------------------------------------------------------------ */
static cm_Int16 *data_const(long frames, cm_Int16 l, cm_Int16 r) {
  cm_Int16 *d = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * frames);
  long i;
  for (i = 0; i < frames; i++) {
    d[2*i]     = l;
    d[2*i + 1] = r;
  }
  return d;
}

static cm_Int16 *data_lcg(long frames, unsigned seed, int amp) {
  cm_Int16 *d = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * frames);
  long i;
  unsigned s = seed;
  for (i = 0; i < frames; i++) {
    s = s * 1664525u + 1013904223u;
    d[2*i] = (cm_Int16) (((s >> 16) % (unsigned) (2*amp)) - amp);
    s = s * 1664525u + 1013904223u;
    d[2*i + 1] = (cm_Int16) (((s >> 16) % (unsigned) (2*amp)) - amp);
  }
  return d;
}

static void le16(unsigned char *b, unsigned x) {
  b[0] = (unsigned char) x;
  b[1] = (unsigned char) (x >> 8);
}

static void le32(unsigned char *b, unsigned x) {
  int i;
  for (i = 0; i < 4; i++) {
    b[i] = (unsigned char) (x >> (8*i));
  }
}

static unsigned char *make_wav(const cm_Int16 *frames, int nframes, int *size) {
  int datasz = nframes * 4;
  int total = 44 + datasz;
  int i;
  unsigned char *w = (unsigned char*) calloc((size_t) total, 1);
  memcpy(w, "RIFF", 4);
  le32(w + 4, (unsigned) (total - 8));
  memcpy(w + 8, "WAVE", 4);
  memcpy(w + 12, "fmt ", 4);
  le32(w + 16, 16);
  le16(w + 20, 1);
  le16(w + 22, 2);
  le32(w + 24, TSR);
  le32(w + 28, TSR * 4);
  le16(w + 32, 4);
  le16(w + 34, 16);
  memcpy(w + 36, "data", 4);
  le32(w + 40, (unsigned) datasz);
  for (i = 0; i < nframes * 2; i++) {
    le16(w + 44 + 2*i, (unsigned short) frames[i]);
  }
  *size = total;
  return w;
}

/* ================================================================== */
/* peak group                                                          */
/* ================================================================== */

/* A future impulse must pull the gain down exactly L frames early. */
static int case_future_impulse(void) {
  enum { FRAMES = 400, L = 40, CEIL = 10000, REL = 8 };
  Rig r;
  cm_Int16 *out, *data;
  int i, bad;
  cm_init(TSR);
  data = data_const(FRAMES, 3000, -2000);
  data[2*200]     = 30000;  /* left-only impulse at frame 200 */
  data[2*200 + 1] = 0;
  rig_init(&r, FRAMES, L, CEIL, REL);
  /* source created before the limiter is configured (attach on enable) */
  i = rig_add_source(&r, data, FRAMES, 1.0, 0.0, 1.0);
  TASK_CHECK(i >= 0);
  TASK_CHECK(cm_configure_limiter(L, CEIL, REL) == 1);
  TASK_CHECK(cm_get_limiter_latency() == L);
  TASK_CHECK(cm_get_limiter_gain() == 1.0);
  rig_play(&r, i);
  out = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * FRAMES);
  bad = rig_process(&r, out, FRAMES);
  TASK_CHECK(bad == 0);
  /* explicit spot checks (dst index = output frame + L) */
  TASK_CHECK(out[2*199] == 3000);       /* n=159: window not yet reached */
  TASK_CHECK(out[2*199 + 1] == -2000);
  /* n=160: window [160,200] first sees the impulse, gain = 10000/30000 */
  TASK_CHECK(out[2*200] == 1000);
  TASK_CHECK(out[2*200 + 1] == -667);
  /* n=200: the impulse itself lands exactly on the ceiling */
  TASK_CHECK(out[2*240] == 10000);
  TASK_CHECK(out[2*240 + 1] == 0);
  TASK_CHECK(cm_get_limiter_gain() == r.g);
  rig_teardown(&r);
  free(data);
  free(out);
  return 0;
}

/* Two equal peaks: the window max must survive the first one's expiry. */
static int case_equal_peak_expiry(void) {
  enum { FRAMES = 300, L = 40, CEIL = 10000, REL = 4 };
  Rig r;
  cm_Int16 *out, *data;
  int i, bad;
  cm_init(TSR);
  TASK_CHECK(cm_configure_limiter(L, CEIL, REL) == 1);
  data = data_const(FRAMES, 1000, 1000);
  data[2*100] = 20000;
  data[2*130] = 20000;    /* equal second peak, still inside the window */
  rig_init(&r, FRAMES, L, CEIL, REL);
  i = rig_add_source(&r, data, FRAMES, 1.0, 0.0, 1.0);
  TASK_CHECK(i >= 0);
  rig_play(&r, i);
  out = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * FRAMES);
  bad = rig_process(&r, out, FRAMES);
  TASK_CHECK(bad == 0);
  /* n=101: first peak expired, equal second peak still covers it */
  TASK_CHECK(out[2*141] == 500);
  /* n=131: window is clean again, gain releasing by 1/4 */
  TASK_CHECK(out[2*171] == 750);
  TASK_CHECK(out[2*172] == 1000);
  rig_teardown(&r);
  free(data);
  free(out);
  return 0;
}

/* Signals that exceed full scale individually but cancel in the sum. */
static int case_overrange_cancel(void) {
  enum { FRAMES = 500, L = 32, CEIL = 10000, REL = 8 };
  Rig r;
  cm_Int16 *out, *da, *db;
  int a, b, bad;
  cm_init(TSR);
  TASK_CHECK(cm_configure_limiter(L, CEIL, REL) == 1);
  da = data_const(FRAMES, 30000, 12000);
  db = data_const(FRAMES, 0, 0);
  for (a = 100; a < 400; a++) {
    db[2*a] = -30000;
    db[2*a + 1] = -12000;
  }
  rig_init(&r, FRAMES, L, CEIL, REL);
  a = rig_add_source(&r, da, FRAMES, 1.0, 0.0, 1.0);
  b = rig_add_source(&r, db, FRAMES, 1.0, 0.0, 1.0);
  TASK_CHECK(a >= 0 && b >= 0);
  rig_play(&r, a);
  rig_play(&r, b);
  out = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * FRAMES);
  bad = rig_process(&r, out, 250);
  TASK_CHECK(bad == 0);
  /* deep inside the cancelling region the output is exactly zero and
  ** the gain has fully recovered */
  TASK_CHECK(out[2*200] == 0);
  TASK_CHECK(out[2*200 + 1] == 0);
  TASK_CHECK(cm_get_limiter_gain() == 1.0);
  bad = rig_process(&r, out, 250);
  TASK_CHECK(bad == 0);
  rig_teardown(&r);
  free(da);
  free(db);
  free(out);
  return 0;
}

/* A low-level signal must pass through with gain exactly 1. */
static int case_low_level(void) {
  enum { FRAMES = 200, L = 24, CEIL = 16000, REL = 8 };
  Rig r;
  cm_Int16 *out, *data;
  int i, bad;
  cm_init(TSR);
  TASK_CHECK(cm_configure_limiter(L, CEIL, REL) == 1);
  data = data_const(FRAMES, 8000, -5000);
  rig_init(&r, FRAMES, L, CEIL, REL);
  i = rig_add_source(&r, data, FRAMES, 1.0, 0.0, 1.0);
  TASK_CHECK(i >= 0);
  rig_play(&r, i);
  out = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * FRAMES);
  bad = rig_process(&r, out, FRAMES);
  TASK_CHECK(bad == 0);
  TASK_CHECK(out[2*100] == 8000);
  TASK_CHECK(out[2*100 + 1] == -5000);
  TASK_CHECK(cm_get_limiter_gain() == 1.0);
  rig_teardown(&r);
  free(data);
  free(out);
  return 0;
}

/* A half-speed source whose reads cross the ring prefetch boundaries. */
static int case_half_speed_ring_boundary(void) {
  enum { FRAMES = 1300, SRCLEN = 600, L = 50, CEIL = 12000, REL = 16 };
  Rig r;
  cm_Int16 *out, *data;
  int i, bad;
  cm_init(TSR);
  TASK_CHECK(cm_configure_limiter(L, CEIL, REL) == 1);
  data = data_lcg(SRCLEN, 12345u, 10000);
  data[2*300] = 25000;      /* spike that must be limited */
  data[2*301] = -24000;
  rig_init(&r, FRAMES, L, CEIL, REL);
  i = rig_add_source(&r, data, SRCLEN, 1.0, 0.3, 0.5);
  TASK_CHECK(i >= 0);
  rig_play(&r, i);
  out = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * FRAMES);
  bad = rig_process(&r, out, FRAMES);
  TASK_CHECK(bad == 0);
  TASK_CHECK(cm_get_state(r.s[i].src) == CM_STATE_STOPPED);
  rig_teardown(&r);
  free(data);
  free(out);
  return 0;
}

/* An in-memory WAV and an event source mixed through the limiter. */
static int case_wav_and_events(void) {
  enum { FRAMES = 300, WAVLEN = 180, L = 30, CEIL = 9000, REL = 8 };
  Rig r;
  cm_Int16 *out, *wd, *ed;
  unsigned char *wav;
  int size, a, b, bad;
  long k;
  cm_init(TSR);
  TASK_CHECK(cm_configure_limiter(L, CEIL, REL) == 1);
  wd = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * WAVLEN);
  for (k = 0; k < WAVLEN; k++) {
    wd[2*k]     = (cm_Int16) ((k % 40) * 500 - 9000);
    wd[2*k + 1] = (cm_Int16) (7000 - (k % 30) * 400);
  }
  wd[2*90] = 22000;
  wav = make_wav(wd, WAVLEN, &size);
  ed = data_const(FRAMES, -4000, 3000);
  rig_init(&r, FRAMES, L, CEIL, REL);
  a = rig_add_wav_source(&r, wav, size, wd, WAVLEN, 1.0, -0.2, 1.0);
  b = rig_add_source(&r, ed, FRAMES, 0.8, 0.4, 1.0);
  TASK_CHECK(a >= 0 && b >= 0);
  rig_play(&r, a);
  rig_play(&r, b);
  out = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * FRAMES);
  bad = rig_process(&r, out, FRAMES);
  TASK_CHECK(bad == 0);
  TASK_CHECK(cm_get_state(r.s[a].src) == CM_STATE_STOPPED);
  rig_teardown(&r);
  free(wd);
  free(wav);
  free(ed);
  free(out);
  return 0;
}

/* The classic path must stay bit-identical when the limiter is off. */
static int case_disabled_path_compare(void) {
  enum { FRAMES = 500 };
  Rig r;
  cm_Int16 *out, *da, *db;
  int a, b, bad;
  cm_init(TSR);
  da = data_const(FRAMES, 8000, -6000);
  db = data_lcg(FRAMES, 777u, 9000);
  rig_init(&r, FRAMES, 0, 0, 0);
  r.disabled = 1;
  a = rig_add_source(&r, da, FRAMES, 1.5, -0.4, 1.0);
  b = rig_add_source(&r, db, FRAMES, 0.5, 0.8, 0.75);
  TASK_CHECK(a >= 0 && b >= 0);
  rig_master_gain(&r, 0.8);
  rig_play(&r, a);
  rig_play(&r, b);
  out = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * FRAMES);
  bad = rig_process(&r, out, FRAMES);
  TASK_CHECK(bad == 0);
  TASK_CHECK(cm_get_limiter_latency() == 0);
  TASK_CHECK(cm_get_limiter_gain() == 1.0);
  rig_teardown(&r);
  free(da);
  free(db);
  free(out);
  return 0;
}

/* 32 sources at maximum gain/pan/pitch with master gain 8: the wide
** summation must not overflow and the limiter must still track the model. */
static int case_many_sources(void) {
  enum { FRAMES = 400, NSRC = 32, L = 48, CEIL = 20000, REL = 32 };
  Rig r;
  cm_Int16 *out, *data;
  int i, bad;
  cm_init(TSR);
  TASK_CHECK(cm_configure_limiter(L, CEIL, REL) == 1);
  data = data_lcg(FRAMES, 2024u, 12000);
  rig_init(&r, FRAMES, L, CEIL, REL);
  for (i = 0; i < NSRC; i++) {
    double pan = (i % 2) ? 0.9 : -0.9;
    double pitch = 0.5 + (double) (i % 4) * 0.5;   /* 0.5 .. 2.0 */
    int idx = rig_add_source(&r, data, FRAMES, 8.0, pan, pitch);
    TASK_CHECK(idx == i);
    rig_play(&r, idx);
  }
  rig_master_gain(&r, 8.0);
  out = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * FRAMES);
  bad = rig_process(&r, out, FRAMES);
  TASK_CHECK(bad == 0);
  TASK_CHECK(cm_get_limiter_gain() < 1.0);   /* clearly limiting */
  rig_teardown(&r);
  free(data);
  free(out);
  return 0;
}

/* ================================================================== */
/* clock group                                                         */
/* ================================================================== */

/* Whole-block and odd-sized chunk feeds must give identical PCM. */
static int case_chunk_invariance(void) {
  enum { FRAMES = 3000, SRCLEN = 3600, L = 33, CEIL = 12000, REL = 24 };
  static const int chunks[] = { 2, 6, 100, 512, 1000, 254, 2, 766, 130, 512, 8, 2000 };
  Rig ra, rb;
  cm_Int16 *outa, *outb, *da, *db;
  int i, rem, c;
  cm_Int16 *dst;
  cm_init(TSR);
  da = data_lcg(SRCLEN, 42u, 9000);
  db = data_lcg(SRCLEN, 99u, 7000);
  /* run A: one big block (crosses the internal 512-sample split) */
  TASK_CHECK(cm_configure_limiter(L, CEIL, REL) == 1);
  rig_init(&ra, FRAMES, L, CEIL, REL);
  i = rig_add_source(&ra, da, SRCLEN, 1.2, -0.3, 1.0);
  TASK_CHECK(i >= 0);
  i = rig_add_source(&ra, db, SRCLEN, 0.9, 0.5, 0.75);
  TASK_CHECK(i >= 0);
  rig_play(&ra, 0);
  rig_play(&ra, 1);
  outa = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * FRAMES);
  cm_process(outa, 2 * FRAMES);
  rig_teardown(&ra);
  /* run B: odd-sized chunks */
  cm_init(TSR);
  TASK_CHECK(cm_configure_limiter(L, CEIL, REL) == 1);
  rig_init(&rb, FRAMES, L, CEIL, REL);
  i = rig_add_source(&rb, da, SRCLEN, 1.2, -0.3, 1.0);
  TASK_CHECK(i >= 0);
  i = rig_add_source(&rb, db, SRCLEN, 0.9, 0.5, 0.75);
  TASK_CHECK(i >= 0);
  rig_play(&rb, 0);
  rig_play(&rb, 1);
  outb = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * FRAMES);
  rem = 2 * FRAMES;
  dst = outb;
  c = 0;
  while (rem > 0) {
    int n = chunks[c % (int) (sizeof(chunks) / sizeof(chunks[0]))];
    if (n > rem) {
      n = rem;
    }
    cm_process(dst, n);
    dst += n;
    rem -= n;
    c++;
  }
  TASK_CHECK(memcmp(outa, outb, 2 * FRAMES * sizeof(cm_Int16)) == 0);
  rig_teardown(&rb);
  free(da);
  free(db);
  free(outa);
  free(outb);
  return 0;
}

/* Fixed L, long stream: total work must grow linearly with input. */
static int case_linear_scaling(void) {
  enum { L = 128, CEIL = 20000, REL = 50, DATALEN = 2048 };
  const long N1 = 1l << 20;
  const long N2 = 4l << 20;
  cm_Int16 *data, *out;
  Gen g;
  cm_SourceInfo info;
  cm_Source *src;
  clock_t t0, t1, t2;
  double a, b;
  long i, nz;
  cm_init(TSR);
  TASK_CHECK(cm_configure_limiter(L, CEIL, REL) == 1);
  data = data_lcg(DATALEN, 7u, 9500);
  g.data = data;
  g.frames = DATALEN;
  g.pos = 0;
  g.loop = 1;
  memset(&info, 0, sizeof(info));
  info.handler = gen_handler;
  info.udata = &g;
  info.samplerate = TSR;
  info.length = DATALEN;
  src = cm_new_source(&info);
  TASK_CHECK(src != NULL);
  cm_set_loop(src, 1);
  cm_play(src);
  out = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * (size_t) N2);
  TASK_CHECK(out != NULL);
  cm_process(out, 8192);            /* warmup: past the latency */
  t0 = clock();
  cm_process(out, (int) (2 * N1));
  t1 = clock();
  cm_process(out, (int) (2 * N2));
  t2 = clock();
  a = (double) (t1 - t0) / (double) N1;
  b = (double) (t2 - t1) / (double) N2;
  /* per-frame cost must not grow with stream length */
  TASK_CHECK(b <= a * 4.0 + 1e-6);
  nz = 0;
  for (i = 0; i < 64; i++) {
    nz |= out[2*i];
  }
  TASK_CHECK(nz != 0);
  cm_destroy_source(src);
  cm_disable_limiter();
  free(data);
  free(out);
  return 0;
}

/* cm_process must not allocate or free while rendering. */
static int case_no_render_allocs(void) {
  enum { FRAMES = 512, SRCLEN = 200, L = 64, CEIL = 10000, REL = 16 };
  Rig r;
  cm_Int16 *out, *data;
  int i, bad;
  cm_init(TSR);
  TASK_CHECK(cm_configure_limiter(L, CEIL, REL) == 1);
  data = data_lcg(SRCLEN, 55u, 12000);
  rig_init(&r, FRAMES, L, CEIL, REL);
  i = rig_add_source(&r, data, SRCLEN, 1.0, 0.2, 1.0);
  TASK_CHECK(i >= 0);
  rig_play(&r, i);
  out = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * FRAMES);
  bad = rig_process(&r, out, 100);
  TASK_CHECK(bad == 0);
  task_alloc_counters_reset();
  bad = rig_process(&r, out, 150);    /* crosses the source's EOF */
  TASK_CHECK(bad == 0);
  bad = rig_process(&r, out, 100);    /* silence after EOF */
  TASK_CHECK(bad == 0);
  cm_process(NULL, 0);
  TASK_CHECK(task_alloc_calls() == 0);
  TASK_CHECK(task_free_calls() == 0);
  rig_teardown(&r);
  free(data);
  free(out);
  return 0;
}

/* ================================================================== */
/* lifecycle group                                                     */
/* ================================================================== */

/* Master gain changes must only affect input entering the delay line. */
static int case_master_gain_queued(void) {
  enum { FRAMES = 300, L = 64, CEIL = 30000, REL = 16 };
  Rig r;
  cm_Int16 *out, *data;
  int i, bad;
  cm_init(TSR);
  TASK_CHECK(cm_configure_limiter(L, CEIL, REL) == 1);
  data = data_const(FRAMES, 10000, 10000);
  rig_init(&r, FRAMES, L, CEIL, REL);
  i = rig_add_source(&r, data, FRAMES, 1.0, 0.0, 1.0);
  TASK_CHECK(i >= 0);
  rig_play(&r, i);
  out = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * FRAMES);
  bad = rig_process(&r, out, 100);
  TASK_CHECK(bad == 0);
  rig_master_gain(&r, 0.5);           /* 100 frames are already queued */
  bad = rig_process(&r, out, 200);    /* out[] now holds stream frames 100..299 */
  TASK_CHECK(bad == 0);
  TASK_CHECK(out[2*63] == 10000);     /* stream frame 163, n=99: queued, old gain */
  TASK_CHECK(out[2*64] == 5000);      /* stream frame 164, n=100: new gain */
  TASK_CHECK(out[2*100] == 5000);
  rig_teardown(&r);
  free(data);
  free(out);
  return 0;
}

/* Withdrawing a strong source must free the others from its peaks. */
static int case_retract_strong_source(void) {
  enum { FRAMES = 400, L = 64, CEIL = 8000, REL = 16 };
  Rig r;
  cm_Int16 *out, *da, *db;
  int a, b, bad;
  cm_init(TSR);
  TASK_CHECK(cm_configure_limiter(L, CEIL, REL) == 1);
  da = data_const(FRAMES, 20000, 10000);
  db = data_const(FRAMES, 1500, -1500);
  rig_init(&r, FRAMES, L, CEIL, REL);
  a = rig_add_source(&r, da, FRAMES, 1.0, 0.0, 1.0);
  b = rig_add_source(&r, db, FRAMES, 1.0, 0.0, 1.0);
  TASK_CHECK(a >= 0 && b >= 0);
  rig_play(&r, a);
  rig_play(&r, b);
  out = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * FRAMES);
  bad = rig_process(&r, out, 200);
  TASK_CHECK(bad == 0);
  rig_stop(&r, a);                    /* withdraw A's queued contributions */
  bad = rig_process(&r, out, 200);    /* out[] now holds stream frames 200..399 */
  TASK_CHECK(bad == 0);
  /* B's content emerges undistorted and the gain releases to 1 */
  TASK_CHECK(out[2*50] == 1500);      /* stream frame 250, n=186 */
  TASK_CHECK(out[2*50 + 1] == -1500);
  TASK_CHECK(cm_get_limiter_gain() == 1.0);
  rig_teardown(&r);
  free(da);
  free(db);
  free(out);
  return 0;
}

/* Ceiling/release can be redirected at runtime, invalid ones rejected. */
static int case_set_parameters(void) {
  enum { FRAMES = 300, L = 32, CEIL = 10000, REL = 16 };
  Rig r;
  cm_Int16 *out, *data;
  int i, bad;
  cm_init(TSR);
  TASK_CHECK(cm_configure_limiter(L, CEIL, REL) == 1);
  data = data_const(FRAMES, 20000, 20000);
  rig_init(&r, FRAMES, L, CEIL, REL);
  i = rig_add_source(&r, data, FRAMES, 1.0, 0.0, 1.0);
  TASK_CHECK(i >= 0);
  rig_play(&r, i);
  out = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * FRAMES);
  bad = rig_process(&r, out, 100);
  TASK_CHECK(bad == 0);
  TASK_CHECK(out[2*50] == 10000);     /* old ceiling */
  TASK_CHECK(cm_set_limiter_parameters(5000, 4) == 1);
  r.ceiling = 5000;
  r.release = 4;
  bad = rig_process(&r, out, 100);
  TASK_CHECK(bad == 0);
  TASK_CHECK(out[2*50] == 5000);      /* new ceiling, from the next frame */
  /* invalid parameters keep the current state */
  TASK_CHECK(cm_set_limiter_parameters(0, 4) == 0);
  TASK_CHECK(cm_set_limiter_parameters(5000, 0) == 0);
  TASK_CHECK(cm_set_limiter_parameters(40000, 4) == 0);
  TASK_CHECK(cm_get_error() != NULL);
  bad = rig_process(&r, out, 100);
  TASK_CHECK(bad == 0);
  TASK_CHECK(out[2*50] == 5000);
  cm_disable_limiter();
  TASK_CHECK(cm_set_limiter_parameters(5000, 4) == 0);
  TASK_CHECK(cm_get_error() != NULL);
  rig_teardown(&r);
  free(data);
  free(out);
  return 0;
}

/* A source shorter than the latency must still deliver its full tail. */
static int case_short_source_tail(void) {
  enum { FRAMES = 200, SRCLEN = 30, L = 100, CEIL = 8000, REL = 8 };
  Rig r;
  cm_Int16 *out, *data;
  int i, bad;
  cm_init(TSR);
  TASK_CHECK(cm_configure_limiter(L, CEIL, REL) == 1);
  data = data_const(SRCLEN, 20000, 12000);
  rig_init(&r, FRAMES, L, CEIL, REL);
  i = rig_add_source(&r, data, SRCLEN, 1.0, 0.0, 1.0);
  TASK_CHECK(i >= 0);
  rig_play(&r, i);
  out = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * FRAMES);
  bad = rig_process(&r, out, FRAMES);
  TASK_CHECK(bad == 0);
  TASK_CHECK(out[2*50] == 0);          /* still in the latency */
  TASK_CHECK(out[2*100] == 8000);      /* first tail frame, at ceiling */
  TASK_CHECK(out[2*100 + 1] == 4800);
  TASK_CHECK(out[2*129] != 0);         /* last frame of the source */
  TASK_CHECK(out[2*130] == 0);         /* then silence again */
  TASK_CHECK(cm_get_state(r.s[i].src) == CM_STATE_STOPPED);
  rig_teardown(&r);
  free(data);
  free(out);
  return 0;
}

/* After natural EOF a source leaves the active list but its queued
** contributions must still be withdrawable by cm_destroy_source(). */
static int case_eof_selective_destroy(void) {
  enum { FRAMES = 200, LEN_A = 50, LEN_B = 90, L = 64, CEIL = 8000, REL = 16 };
  Rig r;
  cm_Int16 *out, *da, *db;
  int a, b, bad;
  cm_init(TSR);
  TASK_CHECK(cm_configure_limiter(L, CEIL, REL) == 1);
  da = data_const(LEN_A, 20000, 20000);
  db = data_const(LEN_B, 6000, -3000);
  rig_init(&r, FRAMES, L, CEIL, REL);
  a = rig_add_source(&r, da, LEN_A, 1.0, 0.0, 1.0);
  b = rig_add_source(&r, db, LEN_B, 1.0, 0.0, 1.0);
  TASK_CHECK(a >= 0 && b >= 0);
  rig_play(&r, a);
  rig_play(&r, b);
  out = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * FRAMES);
  bad = rig_process(&r, out, 100);    /* both sources EOF and delist */
  TASK_CHECK(bad == 0);
  TASK_CHECK(cm_get_state(r.s[a].src) == CM_STATE_STOPPED);
  TASK_CHECK(cm_get_state(r.s[b].src) == CM_STATE_STOPPED);
  rig_destroy(&r, a);                 /* withdraw A's queued tail */
  bad = rig_process(&r, out, 100);    /* out[] now holds stream frames 100..199 */
  TASK_CHECK(bad == 0);
  TASK_CHECK(out[2*40] == 6000);      /* stream frame 140, n=76: only B, released */
  TASK_CHECK(out[2*40 + 1] == -3000);
  TASK_CHECK(out[2*99] == 0);         /* stream frame 199, n=135: both done */
  TASK_CHECK(cm_get_limiter_gain() == 1.0);
  rig_teardown(&r);
  free(da);
  free(db);
  free(out);
  return 0;
}

/* A paused source must be included when the limiter is reconfigured. */
static int case_paused_reconfigure(void) {
  enum { FRAMES = 500, L1 = 32, L2 = 80, CEIL = 12000, REL = 16 };
  Rig r;
  cm_Int16 *out, *da, *db;
  int a, b, bad;
  cm_init(TSR);
  TASK_CHECK(cm_configure_limiter(L1, CEIL, REL) == 1);
  da = data_const(FRAMES, 11000, 7000);
  db = data_const(FRAMES, 9000, -9000);
  rig_init(&r, FRAMES, L1, CEIL, REL);
  a = rig_add_source(&r, da, FRAMES, 1.0, 0.0, 1.0);
  b = rig_add_source(&r, db, FRAMES, 1.0, 0.0, 1.0);
  TASK_CHECK(a >= 0 && b >= 0);
  rig_play(&r, a);
  rig_play(&r, b);
  out = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * FRAMES);
  bad = rig_process(&r, out, 50);
  TASK_CHECK(bad == 0);
  rig_pause(&r, b);
  bad = rig_process(&r, out, 50);
  TASK_CHECK(bad == 0);
  /* reconfigure while B is paused: queued PCM and envelope are dropped */
  TASK_CHECK(cm_configure_limiter(L2, CEIL, REL) == 1);
  rig_model_reset(&r, L2, CEIL, REL);
  TASK_CHECK(cm_get_limiter_latency() == L2);
  bad = rig_process(&r, out, 100);
  TASK_CHECK(bad == 0);
  TASK_CHECK(out[2*0] == 0);          /* latency restarts from zero */
  TASK_CHECK(out[2*79] == 0);
  TASK_CHECK(out[2*80] != 0 || out[2*80 + 1] != 0);
  rig_play(&r, b);                    /* paused source resumes cleanly */
  bad = rig_process(&r, out, 150);
  TASK_CHECK(bad == 0);
  rig_teardown(&r);
  free(da);
  free(db);
  free(out);
  return 0;
}

/* Stop/destroy sources that are not the head of the active list. */
static int case_non_head_stop_destroy(void) {
  enum { FRAMES = 300, L = 40, CEIL = 10000, REL = 16 };
  Rig r;
  cm_Int16 *out, *da, *db, *dc;
  int a, b, c, bad;
  cm_init(TSR);
  TASK_CHECK(cm_configure_limiter(L, CEIL, REL) == 1);
  da = data_const(FRAMES, 5000, 5000);
  db = data_const(FRAMES, 6000, -6000);
  dc = data_const(FRAMES, 7000, 3000);
  rig_init(&r, FRAMES, L, CEIL, REL);
  a = rig_add_source(&r, da, FRAMES, 1.0, 0.0, 1.0);
  b = rig_add_source(&r, db, FRAMES, 1.0, 0.0, 1.0);
  c = rig_add_source(&r, dc, FRAMES, 1.0, 0.0, 1.0);
  TASK_CHECK(a >= 0 && b >= 0 && c >= 0);
  rig_play(&r, a);
  rig_play(&r, b);
  rig_play(&r, c);                    /* active list: C -> B -> A */
  out = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * FRAMES);
  bad = rig_process(&r, out, 80);
  TASK_CHECK(bad == 0);
  rig_stop(&r, b);                    /* middle of the list */
  bad = rig_process(&r, out, 100);
  TASK_CHECK(bad == 0);
  rig_destroy(&r, a);                 /* tail of the list */
  bad = rig_process(&r, out, 60);
  TASK_CHECK(bad == 0);
  rig_destroy(&r, c);                 /* head of the list */
  bad = rig_process(&r, out, 60);
  TASK_CHECK(bad == 0);
  TASK_CHECK(out[2*59] == 0);         /* nothing left playing */
  rig_teardown(&r);
  free(da);
  free(db);
  free(dc);
  free(out);
  return 0;
}

/* Replay after stop follows the rewind rules; old contributions must
** not come back through a new source reusing the freed memory. */
static int case_replay_after_stop(void) {
  enum { FRAMES = 700, L = 48, CEIL = 10000, REL = 16 };
  Rig r;
  cm_Int16 *out, *da, *db;
  int a, b, bad;
  cm_init(TSR);
  TASK_CHECK(cm_configure_limiter(L, CEIL, REL) == 1);
  da = data_const(400, 15000, 9000);
  rig_init(&r, FRAMES, L, CEIL, REL);
  a = rig_add_source(&r, da, 400, 1.0, 0.0, 1.0);
  TASK_CHECK(a >= 0);
  rig_play(&r, a);
  out = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * FRAMES);
  bad = rig_process(&r, out, 100);
  TASK_CHECK(bad == 0);
  rig_stop(&r, a);
  bad = rig_process(&r, out, 60);
  TASK_CHECK(bad == 0);
  rig_play(&r, a);                    /* rewinds to the beginning */
  bad = rig_process(&r, out, 200);
  TASK_CHECK(bad == 0);
  rig_destroy(&r, a);
  /* a fresh source (likely reusing the freed block) must start clean */
  db = data_const(300, 4000, -4000);
  b = rig_add_source(&r, db, 300, 1.0, 0.0, 1.0);
  TASK_CHECK(b >= 0);
  rig_play(&r, b);
  bad = rig_process(&r, out, 300);
  TASK_CHECK(bad == 0);
  TASK_CHECK(out[2*100] == 4000);
  TASK_CHECK(out[2*100 + 1] == -4000);
  rig_teardown(&r);
  free(da);
  free(db);
  free(out);
  return 0;
}

/* Reset clears the limiter history; disable restores the classic path. */
static int case_reset_disable(void) {
  enum { FRAMES = 500, L = 40, CEIL = 8000, REL = 16 };
  Rig r;
  cm_Int16 *out, *data;
  int i, bad;
  cm_init(TSR);
  TASK_CHECK(cm_configure_limiter(L, CEIL, REL) == 1);
  data = data_const(FRAMES, 20000, 10000);
  rig_init(&r, FRAMES, L, CEIL, REL);
  i = rig_add_source(&r, data, FRAMES, 1.0, 0.0, 1.0);
  TASK_CHECK(i >= 0);
  rig_master_gain(&r, 2.0);
  rig_play(&r, i);
  out = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * FRAMES);
  bad = rig_process(&r, out, 100);
  TASK_CHECK(bad == 0);
  TASK_CHECK(out[2*50] == 8000);      /* limited to the ceiling */
  cm_reset_limiter();
  rig_model_reset(&r, L, CEIL, REL);
  TASK_CHECK(cm_get_limiter_latency() == L);
  bad = rig_process(&r, out, 100);
  TASK_CHECK(bad == 0);
  TASK_CHECK(out[2*0] == 0);          /* history cleared: latency again */
  TASK_CHECK(out[2*39] == 0);
  TASK_CHECK(out[2*40] == 8000);
  cm_disable_limiter();
  r.disabled = 1;
  TASK_CHECK(cm_get_limiter_latency() == 0);
  TASK_CHECK(cm_get_limiter_gain() == 1.0);
  bad = rig_process(&r, out, 100);
  TASK_CHECK(bad == 0);
  TASK_CHECK(out[2*0] == 32767);      /* classic path: immediate, clipped */
  TASK_CHECK(out[2*0 + 1] == 20000);
  rig_teardown(&r);
  free(data);
  free(out);
  return 0;
}

/* Allocation failures during reconfigure and source construction must
** leave the old configuration and the old output untouched. */
static int case_alloc_failure(void) {
  enum { FRAMES = 400, L = 32, CEIL = 10000, REL = 16 };
  Rig r;
  cm_Int16 *out, *data;
  Gen g2;
  cm_SourceInfo info;
  cm_Source *tmp;
  int i, bad;
  cm_init(TSR);
  TASK_CHECK(cm_configure_limiter(L, CEIL, REL) == 1);
  data = data_const(FRAMES, 12000, 9000);
  rig_init(&r, FRAMES, L, CEIL, REL);
  i = rig_add_source(&r, data, FRAMES, 1.0, 0.0, 1.0);
  TASK_CHECK(i >= 0);
  rig_play(&r, i);
  out = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * FRAMES);
  bad = rig_process(&r, out, 50);
  TASK_CHECK(bad == 0);
  /* reconfigure failing on the first allocation */
  task_alloc_fail_after(0);
  TASK_CHECK(cm_configure_limiter(64, 5000, 8) == 0);
  TASK_CHECK(cm_get_error() != NULL);
  task_alloc_fail_after(-1);
  bad = rig_process(&r, out, 40);
  TASK_CHECK(bad == 0);
  /* reconfigure failing on a per-source delay line */
  task_alloc_fail_after(1);
  TASK_CHECK(cm_configure_limiter(64, 5000, 8) == 0);
  TASK_CHECK(cm_get_error() != NULL);
  task_alloc_fail_after(-1);
  bad = rig_process(&r, out, 40);
  TASK_CHECK(bad == 0);
  /* source construction failing on the source itself */
  memset(&info, 0, sizeof(info));
  g2.data = data;
  g2.frames = FRAMES;
  g2.pos = 0;
  g2.loop = 0;
  info.handler = gen_handler;
  info.udata = &g2;
  info.samplerate = TSR;
  info.length = FRAMES;
  task_alloc_fail_after(0);
  tmp = cm_new_source(&info);
  TASK_CHECK(tmp == NULL);
  TASK_CHECK(cm_get_error() != NULL);
  /* and on its limiter delay line */
  task_alloc_fail_after(1);
  tmp = cm_new_source(&info);
  TASK_CHECK(tmp == NULL);
  TASK_CHECK(cm_get_error() != NULL);
  task_alloc_fail_after(-1);
  bad = rig_process(&r, out, 40);
  TASK_CHECK(bad == 0);
  /* a working reconfigure still takes effect */
  TASK_CHECK(cm_configure_limiter(64, 5000, 8) == 1);
  rig_model_reset(&r, 64, 5000, 8);
  bad = rig_process(&r, out, 120);
  TASK_CHECK(bad == 0);
  rig_teardown(&r);
  free(data);
  free(out);
  return 0;
}

/* Parameter validation and the query functions. */
static int case_configure_validation(void) {
  cm_init(TSR);
  TASK_CHECK(cm_get_limiter_latency() == 0);
  TASK_CHECK(cm_get_limiter_gain() == 1.0);   /* initial value */
  TASK_CHECK(cm_configure_limiter(0, 1000, 10) == 0);
  TASK_CHECK(cm_get_error() != NULL);
  TASK_CHECK(cm_configure_limiter(65537, 1000, 10) == 0);
  TASK_CHECK(cm_get_error() != NULL);
  TASK_CHECK(cm_configure_limiter(-3, 1000, 10) == 0);
  TASK_CHECK(cm_configure_limiter(1, 0, 10) == 0);
  TASK_CHECK(cm_configure_limiter(1, 32768, 10) == 0);
  TASK_CHECK(cm_configure_limiter(1, 1000, 0) == 0);
  TASK_CHECK(cm_configure_limiter(1, 1000, -5) == 0);
  TASK_CHECK(cm_get_error() != NULL);
  TASK_CHECK(cm_get_limiter_latency() == 0);
  TASK_CHECK(cm_configure_limiter(1, 32767, 1) == 1);
  TASK_CHECK(cm_get_limiter_latency() == 1);
  TASK_CHECK(cm_get_limiter_gain() == 1.0);
  TASK_CHECK(cm_configure_limiter(65536, 16000, 64) == 1);
  TASK_CHECK(cm_get_limiter_latency() == 65536);
  cm_disable_limiter();
  TASK_CHECK(cm_get_limiter_latency() == 0);
  TASK_CHECK(cm_set_limiter_parameters(1000, 10) == 0);
  TASK_CHECK(cm_get_error() != NULL);
  return 0;
}

/* Zero-length processing with a NULL destination must be a no-op. */
static int case_zero_length(void) {
  enum { FRAMES = 300, L = 40, CEIL = 10000, REL = 16 };
  Rig r;
  cm_Int16 *out, *data;
  int i, bad;
  cm_init(TSR);
  TASK_CHECK(cm_configure_limiter(L, CEIL, REL) == 1);
  data = data_const(FRAMES, 12000, 8000);
  rig_init(&r, FRAMES, L, CEIL, REL);
  i = rig_add_source(&r, data, FRAMES, 1.0, 0.0, 1.0);
  TASK_CHECK(i >= 0);
  rig_play(&r, i);
  out = (cm_Int16*) malloc(sizeof(cm_Int16) * 2 * FRAMES);
  bad = rig_process(&r, out, 50);
  TASK_CHECK(bad == 0);
  cm_process(NULL, 0);                /* must not advance or crash */
  cm_process(out, 0);
  bad = rig_process(&r, out, 100);
  TASK_CHECK(bad == 0);
  rig_teardown(&r);
  free(data);
  free(out);
  return 0;
}

/* ================================================================== */

const struct task_case task_cases[] = {
  { "peak",      "future_impulse",           case_future_impulse },
  { "peak",      "equal_peak_expiry",        case_equal_peak_expiry },
  { "peak",      "overrange_cancel",         case_overrange_cancel },
  { "peak",      "low_level",                case_low_level },
  { "peak",      "half_speed_ring_boundary", case_half_speed_ring_boundary },
  { "peak",      "wav_and_events",           case_wav_and_events },
  { "peak",      "disabled_path_compare",    case_disabled_path_compare },
  { "peak",      "many_sources",             case_many_sources },
  { "clock",     "chunk_invariance",         case_chunk_invariance },
  { "clock",     "linear_scaling",           case_linear_scaling },
  { "clock",     "no_render_allocs",         case_no_render_allocs },
  { "lifecycle", "master_gain_queued",       case_master_gain_queued },
  { "lifecycle", "retract_strong_source",    case_retract_strong_source },
  { "lifecycle", "set_parameters",           case_set_parameters },
  { "lifecycle", "short_source_tail",        case_short_source_tail },
  { "lifecycle", "eof_selective_destroy",    case_eof_selective_destroy },
  { "lifecycle", "paused_reconfigure",       case_paused_reconfigure },
  { "lifecycle", "non_head_stop_destroy",    case_non_head_stop_destroy },
  { "lifecycle", "replay_after_stop",        case_replay_after_stop },
  { "lifecycle", "reset_disable",            case_reset_disable },
  { "lifecycle", "alloc_failure",            case_alloc_failure },
  { "lifecycle", "configure_validation",     case_configure_validation },
  { "lifecycle", "zero_length",              case_zero_length },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
