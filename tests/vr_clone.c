/* SoX Resampler Library      Copyright (c) 2007-18 robs@users.sourceforge.net
 * Licence for this file: LGPL v2.1                  See LICENCE for details. */

/* Tests for soxr_clone() of variable-rate (SOXR_VR) resamplers.
 *
 * Groups (registered with task_support/check.h):
 *
 *   history  - a clone given the same subsequent input and call sequence as
 *              the source must consume, produce and emit byte-identical PCM,
 *              including audio already buffered at the fork point; across
 *              mono/stereo/6-channel, float32/int16 and interleaved/split.
 *   branch   - fork at a live point (unread input + unfinished slew + rate
 *              stage cross-fade), then let each side follow a different
 *              subsequent programme; each branch is checked byte for byte and
 *              count for count against an independent from-scratch replay.
 *              Also releases the source, grows one side's FIFOs, clones
 *              again, and shows none of that disturbs the other side.
 *   callback - an app input function is neither transferred to nor invoked
 *              by cloning; the source keeps working; a clone can be given its
 *              own callback and then matches the source exactly.
 */

#include "../task_support/check.h"
#include "soxr.h"
#include "vr-diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>



#define MAX_CH        6
#define MAX_IN     4096
#define MAX_OUT     256
#define LOG_CAP  (4u << 20)
#define WARM_CAP   6000



typedef struct { unsigned ch; int it16; int split; } cfg_t;

/* Six layouts for the three channel counts; two datatypes; both ends. */
#define CFG_LIST \
  X(0, 1, 0, 0) X(1, 2, 0, 0) X(2, 6, 0, 0) \
  X(3, 1, 0, 1) X(4, 2, 0, 1) X(5, 6, 0, 1) \
  X(6, 1, 1, 0) X(7, 2, 1, 0) X(8, 6, 1, 0) \
  X(9, 1, 1, 1) X(10,2, 1, 1) X(11,6, 1, 1)

static const cfg_t cfgs[] = {
#define X(i,ch,t,s) {(ch),(t),(s)},
  CFG_LIST
#undef X
};



/* Deterministic, index-addressed test audio: replay generators rather than
 * stored signals. */

static unsigned long mix32(unsigned long x)
{
  x &= 0xffffffffu;
  x ^= x >> 13, x = (x * 0x5bd1e995u) & 0xffffffffu, x ^= x >> 15;
  return x;
}

static float sr_f(long i, unsigned ch, int sel)
{
  unsigned long x = mix32((unsigned long)i * 2654435761u +
      (unsigned long)ch * 2246822519u + (unsigned long)sel * 3266489917u);
  return (float)((double)(int)x * (1.5 / 2147483648.0));
}

static short sr_s(long i, unsigned ch, int sel)
{
  unsigned long x = mix32((unsigned long)i * 2654435761u +
      (unsigned long)ch * 2246822519u + (unsigned long)sel * 3266489917u);
  return (short)(((int)x >> 8) * 15 / 16);
}



enum { ACT_RATIO, ACT_PROCESS, ACT_INSERT, ACT_FLUSH, ACT_CLEAR };

typedef struct {
  int kind;
  size_t ilen, olen;
  double ratio;
  size_t slew;
} act_t;

typedef struct {
  long idone;                  /* -1 when the action is not a process/flush. */
  long odone;
  int err;
} rec_t;

typedef struct {
  act_t * a;
  size_t n, cap;
} prog_t;

typedef struct {
  cfg_t c;
  soxr_t r;
  long inpos;                  /* Absolute frame position of next input. */
  int sel;                     /* Selects a distinct subsequent sound. */
  int log_on;
  unsigned char * log;
  size_t loglen;
  rec_t recs[MAX_OUT * 64];
  size_t nrec;

  float in_f[MAX_IN * MAX_CH], out_f[MAX_OUT * MAX_CH];
  short in_s[MAX_IN * MAX_CH], out_s[MAX_OUT * MAX_CH];
  float * in_fp[MAX_CH], * out_fp[MAX_CH];
  short * in_sp[MAX_CH], * out_sp[MAX_CH];
} stream_t;



static void stream_setup(stream_t * s, cfg_t c, soxr_t r, long inpos,
    int sel, int log_on)
{
  unsigned u;
  memset(s, 0, sizeof(*s));
  s->c = c, s->r = r, s->inpos = inpos, s->sel = sel, s->log_on = log_on;
  s->log = (unsigned char *)malloc(LOG_CAP);
  for (u = 0; u < c.ch; ++u) {
    s->in_fp[u] = s->in_f + (size_t)u * MAX_IN;
    s->out_fp[u] = s->out_f + (size_t)u * MAX_OUT;
    s->in_sp[u] = s->in_s + (size_t)u * MAX_IN;
    s->out_sp[u] = s->out_s + (size_t)u * MAX_OUT;
  }
}

static void stream_end(stream_t * s)
{
  soxr_delete(s->r);
  free(s->log);
  memset(s, 0, sizeof(*s));
}

/* `nodither' is needed when comparing against an independently created
 * resampler: int16 dithering is seeded per object at creation time, so two
 * separate objects cannot agree unless dither is off.  Clone-vs-original
 * comparisons (history group) keep dither on, to exercise seed inheritance. */

static soxr_t stream_create(cfg_t c, int nodither, soxr_error_t * error)
{
  soxr_datatype_t dt = c.it16? SOXR_INT16_I : SOXR_FLOAT32_I;
  soxr_io_spec_t io;
  soxr_quality_spec_t q;
  if (c.split)
    dt = (soxr_datatype_t)(dt | SOXR_SPLIT);
  io = soxr_io_spec(dt, dt);
  if (nodither)
    io.flags = SOXR_NO_DITHER;
  q = soxr_quality_spec(SOXR_HQ, SOXR_VR);
  return soxr_create(32, 1, c.ch, error, &io, &q, NULL);
}

static soxr_in_t stream_in(stream_t * s, size_t n)
{
  size_t j;
  unsigned u;
  if (s->c.it16) {
    if (s->c.split) {
      for (u = 0; u < s->c.ch; ++u)
        for (j = 0; j < n; ++j)
          s->in_sp[u][j] = sr_s(s->inpos + (long)j, u, s->sel);
      return (soxr_in_t)(void *)s->in_sp;
    }
    for (j = 0; j < n; ++j)
      for (u = 0; u < s->c.ch; ++u)
        s->in_s[j * s->c.ch + u] = sr_s(s->inpos + (long)j, u, s->sel);
    return (soxr_in_t)(void *)s->in_s;
  }
  if (s->c.split) {
    for (u = 0; u < s->c.ch; ++u)
      for (j = 0; j < n; ++j)
        s->in_fp[u][j] = sr_f(s->inpos + (long)j, u, s->sel);
    return (soxr_in_t)(void *)s->in_fp;
  }
  for (j = 0; j < n; ++j)
    for (u = 0; u < s->c.ch; ++u)
      s->in_f[j * s->c.ch + u] = sr_f(s->inpos + (long)j, u, s->sel);
  return (soxr_in_t)(void *)s->in_f;
}

static soxr_out_t stream_out(stream_t * s)
{
  if (s->c.split)
    return (soxr_out_t)(void *)s->out_fp;
  return (soxr_out_t)(void *)(s->c.it16? (void *)s->out_s : (void *)s->out_f);
}

static void stream_append(stream_t * s, size_t odone)
{
  unsigned u;
  if (!s->log_on || !odone)
    return;
  if (s->c.split) {
    for (u = 0; u < s->c.ch; ++u) {
      size_t cb = odone * (s->c.it16? 2u : 4u);
      void const * p = s->c.it16? (void const *)s->out_sp[u] :
                                 (void const *)s->out_fp[u];
      /* The split write path leaves per-channel data in the planar buffers. */
      if (s->loglen + cb > LOG_CAP)
        return;
      memcpy(s->log + s->loglen, p, cb);
      s->loglen += cb;
    }
  }
  else {
    size_t bytes = odone * (s->c.it16? 2u : 4u) * s->c.ch;
    void const * p = s->c.it16? (void const *)s->out_s :
                               (void const *)s->out_f;
    if (s->loglen + bytes > LOG_CAP)
      return;
    memcpy(s->log + s->loglen, p, bytes);
    s->loglen += bytes;
  }
}



static prog_t * prog_new(void)
{
  prog_t * p = (prog_t *)calloc(1, sizeof(*p));
  p->cap = 20000;
  p->a = (act_t *)calloc(p->cap, sizeof(*p->a));
  return p;
}

static void prog_free(prog_t * p)
{
  if (p) {
    free(p->a);
    free(p);
  }
}

static void prog_add(prog_t * p, int kind, size_t ilen, size_t olen,
    double ratio, size_t slew)
{
  act_t * a;
  if (p->n >= p->cap)
    return;
  a = &p->a[p->n++];
  a->kind = kind, a->ilen = ilen, a->olen = olen;
  a->ratio = ratio, a->slew = slew;
}



/* Execute one action; appends a record and (optionally) the produced PCM. */

static int stream_exec(stream_t * s, act_t const * a)
{
  rec_t rec;
  soxr_error_t error = NULL;
  size_t idone = 0, odone = 0;

  memset(&rec, 0, sizeof(rec));
  rec.idone = rec.odone = -1;
  if (a->kind == ACT_RATIO)
    error = soxr_set_io_ratio(s->r, a->ratio, a->slew);
  else if (a->kind == ACT_CLEAR)
    error = soxr_clear(s->r);
  else if (a->kind == ACT_FLUSH) {
    error = soxr_process(s->r, NULL, 0, NULL, stream_out(s), a->olen, &odone);
    rec.odone = (long)odone;
    stream_append(s, odone);
  }
  else if (a->kind == ACT_INSERT) {
    /* Supply input with idone == NULL: soxr_process then takes every supplied
     * sample rather than only what the requested output needs, so the engine
     * FIFOs genuinely grow (and reallocate) rather than merely reuse. */
    soxr_in_t in = stream_in(s, a->ilen);
    error = soxr_process(s->r, in, a->ilen, NULL,
        stream_out(s), a->olen, &odone);
    rec.idone = (long)a->ilen, rec.odone = (long)odone;
    s->inpos += (long)a->ilen;
    stream_append(s, odone);
  }
  else {
    soxr_in_t in = stream_in(s, a->ilen);
    error = soxr_process(s->r, in, a->ilen, &idone,
        stream_out(s), a->olen, &odone);
    rec.idone = (long)idone, rec.odone = (long)odone;
    s->inpos += (long)idone;
    stream_append(s, odone);
  }
  rec.err = !!error;
  if (s->nrec < sizeof(s->recs)/sizeof(*s->recs))
    s->recs[s->nrec++] = rec;
  return !!error;
}

static int run_prog(stream_t * s, prog_t const * p)
{
  size_t i;
  int bad = 0;
  for (i = 0; i < p->n; ++i)
    bad |= stream_exec(s, &p->a[i]);
  return bad;
}

/* Drop the warm-up history so only post-fork records/log are compared. */
static void stream_mark(stream_t * s)
{
  s->nrec = 0;
  s->loglen = 0;
}



/* Warm-up takes a fresh resampler to a live fork point: unread input still
 * buffered, an unfinished ratio slew and a rate-stage change under way.
 * Mode 1 instead stops during a FIR interpolation x-fade.  Every action is
 * recorded so an independent replay can recreate the state from scratch. */

static int warmup(stream_t * s, prog_t * p, int mode, _soxr_vr_diag_t * ev)
{
  static const double xf_ratios[] = {6., 6., 3., 3., 1.6, 1.6, .9, .9,
                                      1.4, 1.6, 3., 6.};
  unsigned ph;
  int k, bad = 0;

  memset(ev, 0, sizeof(*ev));
  if (mode == 0) {
    prog_add(p, ACT_RATIO, 0, 0, 1.5, 0);
    bad |= stream_exec(s, &p->a[p->n - 1]);
    prog_add(p, ACT_RATIO, 0, 0, 3.0, 2000);
    bad |= stream_exec(s, &p->a[p->n - 1]);
    for (k = 0; k < WARM_CAP && !bad; ++k) {
      prog_add(p, ACT_PROCESS, 24, 8, 0, 0);
      bad |= stream_exec(s, &p->a[p->n - 1]);
      _soxr_vr_diag(s->r, ev);
      if (ev->input_buffered && ev->slew_remaining &&
          ev->fade_remaining && ev->stage_switches)
        return 0;
    }
  }
  else {
    for (ph = 0; ph < sizeof(xf_ratios)/sizeof(*xf_ratios) && !bad; ++ph) {
      prog_add(p, ACT_RATIO, 0, 0, xf_ratios[ph], 0);
      bad |= stream_exec(s, &p->a[p->n - 1]);
      for (k = 0; k < 600 && !bad; ++k) {
        prog_add(p, ACT_PROCESS, 24, 4, 0, 0);
        bad |= stream_exec(s, &p->a[p->n - 1]);
        _soxr_vr_diag(s->r, ev);
        if (ev->xfade_remaining && ev->input_buffered &&
            ev->stage_switches)
          return 0;

      }
    }
  }
  return 1;
}

/* Assert that this is a genuinely live fork point, not a fresh object. */
static int check_live(_soxr_vr_diag_t const * ev)
{
  TASK_CHECK(ev->input_buffered > 0);
  TASK_CHECK(ev->slew_remaining > 0);
  TASK_CHECK(ev->fade_remaining > 0);
  TASK_CHECK(ev->stage_switches > 0);
  return 0;
}



static int same_streams(stream_t const * a, stream_t const * b,
    char const * what)
{
  TASK_CHECK(a->nrec == b->nrec);
  TASK_CHECK(a->loglen == b->loglen);
  if (a->nrec != b->nrec || a->loglen != b->loglen) {
    fprintf(stderr, "FAIL %s: records %lu/%lu bytes %lu/%lu\n", what,
        (unsigned long)a->nrec, (unsigned long)b->nrec,
        (unsigned long)a->loglen, (unsigned long)b->loglen);
    return 1;
  }
  if (a->loglen && memcmp(a->log, b->log, a->loglen)) {
    size_t q = 0;
    while (q < a->loglen && a->log[q] == b->log[q]) ++q;
    fprintf(stderr, "FAIL %s: PCM differs at byte %lu of %lu\n",
        what, (unsigned long)q, (unsigned long)a->loglen);
    TASK_CHECK(0);
  }
  if (memcmp(a->recs, b->recs, a->nrec * sizeof(*a->recs))) {
    size_t q;
    for (q = 0; q < a->nrec; ++q)
      if (memcmp(&a->recs[q], &b->recs[q], sizeof(a->recs[q]))) {
        fprintf(stderr, "FAIL %s: action %lu counts %ld/%ld in, "
            "%ld/%ld out, errors %d/%d\n", what, (unsigned long)q,
            a->recs[q].idone, b->recs[q].idone, a->recs[q].odone,
            b->recs[q].odone, a->recs[q].err, b->recs[q].err);
        break;
      }
    TASK_CHECK(0);
  }
  return 0;
}



/* Post-fork programme: drain buffered audio with no new input first, then
 * vary the ratio (leaving slews unfinished), process more audio, flush. */

static prog_t * post_prog(void)
{
  prog_t * p = prog_new();
  size_t i;
  static const size_t ilens[] = {0, 7, 31, 0, 53, 19, 0, 41, 11, 0};

  prog_add(p, ACT_PROCESS, 0, 32, 0, 0);    /* Cached sound, no new input. */
  prog_add(p, ACT_PROCESS, 0, 27, 0, 0);
  prog_add(p, ACT_RATIO, 0, 0, .9, 700);
  for (i = 0; i < 40; ++i) {
    prog_add(p, ACT_PROCESS, ilens[i % 10], 13, 0, 0);
    if (i == 12)
      prog_add(p, ACT_RATIO, 0, 0, 2.6, 400);
  }
  for (i = 0; i < 60; ++i)
    prog_add(p, ACT_FLUSH, 0, 11, 0, 0);
  return p;
}



static int history_case(unsigned idx)
{
  cfg_t c = cfgs[idx];
  stream_t src, cl;
  prog_t * warm = prog_new(), * post;
  _soxr_vr_diag_t ev;
  soxr_error_t error = NULL;
  soxr_t clone;
  long fork_pos;
  int bad = 0;

  src.r = stream_create(c, 0, &error);
  TASK_CHECK(!error && src.r);
  if (error || !src.r) {
    prog_free(warm);
    return 1;
  }
  stream_setup(&src, c, src.r, 0, 0, 0);
  bad |= warmup(&src, warm, 0, &ev);
  TASK_CHECK(!bad);
  bad |= check_live(&ev);
  fork_pos = src.inpos;

  clone = soxr_clone(src.r, &error);
  TASK_CHECK(error == NULL);
  TASK_CHECK(clone != NULL);
  if (!clone) {
    stream_end(&src);
    prog_free(warm);
    return 1;
  }

  /* Inherited configuration and counters. */
  TASK_CHECK(!strcmp(soxr_engine(clone), soxr_engine(src.r)));
  TASK_CHECK(soxr_delay(clone) == soxr_delay(src.r));
  TASK_CHECK(*soxr_num_clips(clone) == *soxr_num_clips(src.r));

  stream_setup(&cl, c, clone, fork_pos, 0, 1);
  src.log_on = 1;
  stream_mark(&src);
  post = post_prog();
  bad |= run_prog(&cl, post);
  bad |= run_prog(&src, post);
  TASK_CHECK(!bad);
  TASK_CHECK(cl.recs[0].odone > 0);  /* Buffered audio, without new input. */
  bad |= same_streams(&cl, &src, "history");
  TASK_CHECK(*soxr_num_clips(clone) == *soxr_num_clips(src.r));

  stream_end(&cl);
  stream_end(&src);
  prog_free(warm);
  prog_free(post);
  return bad;
}



/* Each branch programme is a different subsequent sound (sel) and/or set of
 * ratio changes, so the branches genuinely diverge after the shared fork. */

static prog_t * branch_prog(int which)
{
  prog_t * p = prog_new();
  size_t i;

  if (which == 0) {                        /* Swerve to another ratio. */
    prog_add(p, ACT_RATIO, 0, 0, .75, 900);
    for (i = 0; i < 50; ++i) {
      size_t n = (i % 6 == 3)? 0 : (size_t)(7 + (i * 13) % 47);
      prog_add(p, ACT_PROCESS, n, 7, 0, 0);
      if (i == 22)
        prog_add(p, ACT_RATIO, 0, 0, 2.7, 500);
    }
    for (i = 0; i < 80; ++i)
      prog_add(p, ACT_FLUSH, 0, 9, 0, 0);
  }
  else if (which == 1) {                   /* clear() then start afresh. */
    prog_add(p, ACT_RATIO, 0, 0, 1.1, 300);
    for (i = 0; i < 20; ++i)
      prog_add(p, ACT_PROCESS, 17, 5, 0, 0);
    prog_add(p, ACT_CLEAR, 0, 0, 0, 0);
    prog_add(p, ACT_RATIO, 0, 0, .45, 0);
    for (i = 0; i < 70; ++i)
      prog_add(p, ACT_PROCESS, 29, 11, 0, 0);
    for (i = 0; i < 40; ++i)
      prog_add(p, ACT_FLUSH, 0, 13, 0, 0);
  }
  else {                                   /* Grow this side's FIFOs hard. */
    /* Insert far more input than the tiny output blocks can drain, forcing
     * the stage-0 FIFO past its initial allocation so its backing store is
     * really reallocated, not merely reused. */
    for (i = 0; i < 24; ++i)
      prog_add(p, ACT_INSERT, 4096, 2, 0, 0);
    for (i = 0; i < 40; ++i) {
      size_t n = (i % 4 == 1)? 0 : (size_t)(23 + (i * 17) % 61);
      prog_add(p, ACT_PROCESS, n, 5, 0, 0);
      if (i == 15)
        prog_add(p, ACT_RATIO, 0, 0, 1.9, 250);
    }
    for (i = 0; i < 60; ++i)
      prog_add(p, ACT_FLUSH, 0, 7, 0, 0);
  }
  return p;
}

/* Run a branch on the clone and, independently, replay the whole history and
 * the same branch from a fresh resampler; insist the audible results agree. */

static int run_branch(cfg_t c, soxr_t clone_r, long fork_pos,
    prog_t const * warm, prog_t const * bp, int sel, char const * what)
{
  stream_t cl, oracle;
  soxr_error_t error = NULL;
  int bad = 0;

  memset(&cl, 0, sizeof(cl));
  cl.c = c, cl.r = clone_r, cl.inpos = fork_pos, cl.sel = sel;
  cl.log_on = 1;
  { unsigned u; for (u = 0; u < c.ch; ++u) {
      cl.in_fp[u] = cl.in_f + (size_t)u * MAX_IN;
      cl.out_fp[u] = cl.out_f + (size_t)u * MAX_OUT;
      cl.in_sp[u] = cl.in_s + (size_t)u * MAX_IN;
      cl.out_sp[u] = cl.out_s + (size_t)u * MAX_OUT; } }
  cl.log = (unsigned char *)malloc(LOG_CAP);
  bad |= run_prog(&cl, bp);

  oracle.r = stream_create(c, 1, &error);
  TASK_CHECK(!error && oracle.r);
  if (error || !oracle.r) {
    free(cl.log);
    return 1;
  }
  stream_setup(&oracle, c, oracle.r, 0, 0, 0);
  bad |= run_prog(&oracle, warm);          /* Replay history, no cloning. */
  oracle.sel = sel;
  oracle.log_on = 1;
  stream_mark(&oracle);
  bad |= run_prog(&oracle, bp);            /* Then the branch's own sound. */

  TASK_CHECK(!bad);
  bad |= same_streams(&cl, &oracle, what);
  TASK_CHECK(*soxr_num_clips(clone_r) == *soxr_num_clips(oracle.r));

  free(cl.log);                     /* The clone is freed by the caller. */
  stream_end(&oracle);
  return bad;
}



static int branch_case(unsigned idx)
{
  cfg_t c = cfgs[idx];
  stream_t src;
  prog_t * warm = prog_new(), * bp;
  _soxr_vr_diag_t ev;
  soxr_error_t error = NULL;
  soxr_t c1, c2, c3;
  long fork_pos;
  int bad = 0;

  /* No dither: the oracle is a separately-seeded object.  (The clone itself
   * inherits this flag, so both branches compare like for like.) */
  src.r = stream_create(c, 1, &error);
  TASK_CHECK(!error && src.r);
  if (error || !src.r) {
    prog_free(warm);
    return 1;
  }
  stream_setup(&src, c, src.r, 0, 0, 0);
  bad |= warmup(&src, warm, 0, &ev);
  TASK_CHECK(!bad);
  bad |= check_live(&ev);
  fork_pos = src.inpos;

  c1 = soxr_clone(src.r, &error);
  TASK_CHECK(!error && c1);
  c2 = soxr_clone(src.r, &error);
  TASK_CHECK(!error && c2);
  if (!c1 || !c2) {
    soxr_delete(c1), soxr_delete(c2);
    stream_end(&src);
    prog_free(warm);
    return 1;
  }

  /* The original is released before either branch does anything. */
  soxr_delete(src.r);
  src.r = NULL;

  bp = branch_prog(0);
  bad |= run_branch(c, c1, fork_pos, warm, bp, 7, "branch0");
  prog_free(bp);

  /* Clone one mid-life clone again, then grow its FIFOs hard; verify that the
   * re-clone's heavy input growth leaves its twin (c2), cloned from the same
   * point, untouched: the twins must still agree byte for byte. */
  c3 = soxr_clone(c2, &error);
  TASK_CHECK(!error && c3);
  if (c3) {
    prog_t * bp2 = branch_prog(2);       /* Includes a 2048-sample insert. */
    bad |= run_branch(c, c3, fork_pos, warm, bp2, 5, "branch2-regrow");
    prog_free(bp2);
  }

  bp = branch_prog(1);
  bad |= run_branch(c, c2, fork_pos, warm, bp, 3, "branch1-clear");
  prog_free(bp);
  soxr_delete(c3);

  soxr_delete(c1);
  soxr_delete(c2);
  prog_free(warm);
  return bad;
}



/* Dedicated evidence cases: record the stage-crossing / x-fade fork points
 * separately, then show cached audio still plays from a clone taken there. */

static int evidence_case(int mode, char const * label)
{
  cfg_t c = cfgs[0];
  stream_t src;
  prog_t * warm = prog_new();
  _soxr_vr_diag_t ev;
  soxr_error_t error = NULL;
  int bad = 0;

  src.r = stream_create(c, 0, &error);
  TASK_CHECK(!error && src.r);
  if (error || !src.r) {
    prog_free(warm);
    return 1;
  }
  stream_setup(&src, c, src.r, 0, 0, 0);
  bad |= warmup(&src, warm, mode, &ev);
  TASK_CHECK(!bad);

  if (mode == 0) {
    TASK_CHECK(ev.stage_switches >= 1);
    TASK_CHECK(ev.fade_remaining > 0);
    TASK_CHECK(ev.slew_remaining > 0);
    TASK_CHECK(ev.input_buffered > 0);
  }
  else {
    TASK_CHECK(ev.xfade_starts >= 1);
    TASK_CHECK(ev.xfade_remaining > 0);
    TASK_CHECK(ev.input_buffered > 0);
  }
  fprintf(stderr, "EVIDENCE %s: switches=%lu xfade_starts=%lu "
      "fade=%lu slew=%lu xfade_remaining=%lu buf=%lu\n", label,
      ev.stage_switches, ev.xfade_starts, ev.fade_remaining,
      ev.slew_remaining, ev.xfade_remaining, ev.input_buffered);

  /* Take the clone here, in the middle of a live fade / x-fade, and run the
   * same post-fork programme on both the clone and the original.  Not only
   * must cached audio play from the clone without new input, the whole
   * continuation must be byte-identical (this exercises the clone + source
   * sharing a stage while its cross-fade is still in flight). */
  {
    soxr_t cl = soxr_clone(src.r, &error);
    stream_t cstr;
    prog_t * pp;
    TASK_CHECK(!error && cl);
    if (!cl) {
      soxr_delete(src.r);
      prog_free(warm);
      return 1;
    }
    stream_setup(&cstr, c, cl, src.inpos, 0, 1);
    pp = post_prog();
    bad |= run_prog(&cstr, pp);
    src.log_on = 1;
    stream_mark(&src);
    bad |= run_prog(&src, pp);
    TASK_CHECK(!bad);
    TASK_CHECK(cstr.recs[0].odone > 0);    /* Cached audio, no new input. */
    bad |= same_streams(&cstr, &src, label);
    TASK_CHECK(*soxr_num_clips(cl) == *soxr_num_clips(src.r));
    prog_free(pp);
    stream_end(&cstr);                     /* Frees cl. */
  }
  soxr_delete(src.r);
  prog_free(warm);
  return bad;
}

static int evidence_stage_case(void)
{
  return evidence_case(0, "stage_crossing");
}

static int evidence_xfade_case(void)
{
  return evidence_case(1, "xfade");
}



/* Error behaviour: other engines, unconfigured and failed sources. */

static int error_case(void)
{
  soxr_error_t error = "sentinel", e2 = "sentinel";
  soxr_quality_spec_t cr_q = soxr_quality_spec(SOXR_HQ, 0);
  soxr_quality_spec_t vr_q = soxr_quality_spec(SOXR_HQ, SOXR_VR);
  soxr_t cr, un, vr, cl;

  TASK_CHECK(soxr_clone(NULL, &error) == NULL);
  TASK_CHECK(error != NULL && error[0]);
  TASK_CHECK(soxr_clone(NULL, NULL) == NULL);

  cr = soxr_create(48000, 44100, 2, &error, NULL, &cr_q, NULL);
  TASK_CHECK(!error && cr);
  if (cr) {
    cl = soxr_clone(cr, &error);
    TASK_CHECK(cl == NULL);
    TASK_CHECK(error != NULL && error[0]);
    TASK_CHECK(soxr_error(cr) == NULL);          /* Source unchanged. */
    soxr_delete(cr);
  }

  un = soxr_create(0, 0, 0, &error, NULL, &vr_q, NULL);
  TASK_CHECK(!error && un);
  if (un) {
    cl = soxr_clone(un, &error);
    TASK_CHECK(cl == NULL);
    TASK_CHECK(error != NULL && error[0]);
    soxr_delete(un);
  }

  vr = soxr_create(48000, 44100, 1, &error, NULL, &vr_q, NULL);
  TASK_CHECK(!error && vr);
  if (vr) {
    size_t odone = 0;
    float out[16];
    soxr_error_t e = soxr_process(vr, out, 0, NULL, NULL, 16, &odone);
    TASK_CHECK(e != NULL && soxr_error(vr) == e);  /* Now in error state. */
    cl = soxr_clone(vr, &e2);
    TASK_CHECK(cl == NULL);
    TASK_CHECK(e2 != NULL && e2[0]);
    TASK_CHECK(soxr_error(vr) == e);                /* Untouched. */
    soxr_delete(vr);
  }

  /* Success clears the caller's error slot. */
  {
    stream_t s;
    soxr_t src = stream_create(cfgs[0], 0, &error);
    TASK_CHECK(!error && src);
    if (src) {
      stream_setup(&s, cfgs[0], src, 0, 0, 0);
      cl = soxr_clone(src, &error);
      TASK_CHECK(error == NULL && cl);
      soxr_delete(cl);
      soxr_delete(src);
    }
  }
  return 0;
}



/* ----------------------------- callback group ----------------------------- */

#define CB_CHUNK 64
#define CB_MAX   (CB_CHUNK * MAX_CH)

typedef struct {
  long pos;
  int sel;
  unsigned ch;
  int it16;
  int split;
  int calls;
  float f[CB_MAX], fp_data[MAX_CH][CB_CHUNK], * fp[MAX_CH];
  short s[CB_MAX], sp_data[MAX_CH][CB_CHUNK], * sp[MAX_CH];
} cb_state_t;

static size_t cb_input(void * state, soxr_in_t * data0, size_t requested)
{
  cb_state_t * st = (cb_state_t *)state;
  size_t n = requested, j;
  unsigned u;
  ++st->calls;
  if (n > CB_CHUNK)
    n = CB_CHUNK;

  if (st->it16) {
    for (j = 0; j < n; ++j)
      for (u = 0; u < st->ch; ++u) {
        short v = sr_s(st->pos + (long)j, u, st->sel);
        if (st->split) st->sp_data[u][j] = v;
        else st->s[j * st->ch + u] = v;
      }
    *(data0) = st->split? (soxr_in_t)(void *)st->sp : (soxr_in_t)(void *)st->s;
  }
  else {
    for (j = 0; j < n; ++j)
      for (u = 0; u < st->ch; ++u) {
        float v = sr_f(st->pos + (long)j, u, st->sel);
        if (st->split) st->fp_data[u][j] = v;
        else st->f[j * st->ch + u] = v;
      }
    *(data0) = st->split? (soxr_in_t)(void *)st->fp : (soxr_in_t)(void *)st->f;
  }

  st->pos += (long)n;
  return n;
}

/* Pull olen-sample blocks for `iters' iterations, logging produced PCM. */

static long cb_run(soxr_t r, cfg_t c, int iters, void * out, size_t olen,
    unsigned char * log, size_t * loglen)
{
  long total = 0;
  int i;
  size_t bytes = olen * c.ch * (c.it16? 2u : 4u);
  for (i = 0; i < iters; ++i) {
    size_t odone = soxr_output(r, (soxr_out_t)out, olen);
    if (!odone)
      break;
    total += (long)odone;
    if (log && *loglen + bytes <= LOG_CAP) {
      memcpy(log + *loglen, out, odone * c.ch * (c.it16? 2u : 4u));
      *loglen += odone * c.ch * (c.it16? 2u : 4u);
    }
  }
  return total;
}



static int callback_case_cfg(cfg_t c)
{
  stream_t src;
  cb_state_t st, st1, st2;
  soxr_error_t error = NULL;
  soxr_t c1, c2;
  _soxr_vr_diag_t ev;
  unsigned char * l1, * l2;
  union { float f[MAX_OUT * MAX_CH]; short s[MAX_OUT * MAX_CH]; } out;
  void * outp = c.it16? (void *)out.s : (void *)out.f;
  int calls_before, i, bad = 0;

  TASK_CHECK(!c.split);

  src.r = stream_create(c, 0, &error);
  TASK_CHECK(!error && src.r);
  if (error || !src.r)
    return 1;
  stream_setup(&src, c, src.r, 0, 0, 0);

  memset(&st, 0, sizeof(st));
  st.ch = c.ch, st.it16 = c.it16, st.split = c.split, st.sel = 0;
  for (i = 0; i < (int)c.ch; ++i)
    st.fp[i] = st.fp_data[i], st.sp[i] = st.sp_data[i];
  TASK_CHECK(!soxr_set_input_fn(src.r, cb_input, &st, CB_CHUNK));

  /* Drive to the same live fork point, this time via soxr_output. */
  TASK_CHECK(!soxr_set_io_ratio(src.r, 1.5, 0));
  TASK_CHECK(!soxr_set_io_ratio(src.r, 3.0, 2000));
  for (i = 0; i < WARM_CAP; ++i) {
    size_t got = soxr_output(src.r, (soxr_out_t)outp, 8);
    if (!got) break;
    _soxr_vr_diag(src.r, &ev);
    if (ev.input_buffered && ev.slew_remaining && ev.fade_remaining &&
        ev.stage_switches)
      break;
  }
  bad |= check_live(&ev);
  TASK_CHECK(st.calls > 0);

  calls_before = st.calls;
  c1 = soxr_clone(src.r, &error);
  TASK_CHECK(!error && c1);
  TASK_CHECK(st.calls == calls_before);      /* Cloning called nothing. */
  c2 = soxr_clone(src.r, &error);
  TASK_CHECK(!error && c2);
  TASK_CHECK(st.calls == calls_before);      /* Still nothing. */
  if (!c1 || !c2) {
    soxr_delete(c1), soxr_delete(c2);
    soxr_delete(src.r);
    return 1;
  }

  l1 = (unsigned char *)malloc(LOG_CAP);
  l2 = (unsigned char *)malloc(LOG_CAP);

  /* Buffered audio plays from each clone with no new input and no callback
   * of its own (soxr_output would refuse to make progress without one). */
  {
    size_t n1 = 0, n2 = 0, odone;
    long t1 = 0, t2 = 0;
    int j1, j2;
    for (j1 = 0; j1 < 40; ++j1) {
      odone = 0;
      if (soxr_process(c1, NULL, 0, NULL, (soxr_out_t)outp, 8, &odone))
        break;
      if (!odone)
        break;
      t1 += (long)odone;
      memcpy(l1 + n1, outp, odone * c.ch * (c.it16? 2u : 4u));
      n1 += odone * c.ch * (c.it16? 2u : 4u);
    }
    for (j2 = 0; j2 < 40; ++j2) {
      odone = 0;
      if (soxr_process(c2, NULL, 0, NULL, (soxr_out_t)outp, 8, &odone))
        break;
      if (!odone)
        break;
      t2 += (long)odone;
      memcpy(l2 + n2, outp, odone * c.ch * (c.it16? 2u : 4u));
      n2 += odone * c.ch * (c.it16? 2u : 4u);
    }
    TASK_CHECK(t1 > 0 && t1 == t2);
    TASK_CHECK(n1 == n2 && !memcmp(l1, l2, n1));
    TASK_CHECK(st.calls == calls_before);   /* Neither used the source's cb. */
  }

  /* A fresh clone given its own callback at exactly the source's resume
   * position; driven identically, the two must agree byte for byte. */
  {
    soxr_t c3 = soxr_clone(src.r, &error);
    TASK_CHECK(!error && c3);
    if (c3) {
      size_t n1 = 0, n2 = 0;
      long t1, t2;
      st1 = st, st1.calls = 0;
      TASK_CHECK(!soxr_set_input_fn(c3, cb_input, &st1, CB_CHUNK));
      t2 = cb_run(src.r, c, 80, outp, 9, l2, &n2);
      t1 = cb_run(c3, c, 80, outp, 9, l1, &n1);
      TASK_CHECK(t1 > 0);
      TASK_CHECK(t1 == t2);
      TASK_CHECK(n1 == n2 && !memcmp(l1, l2, n1));
      TASK_CHECK(st.calls > calls_before);
      TASK_CHECK(st1.calls > 0);
      TASK_CHECK(!soxr_clear(c3));        /* Callback survives clear(). */
      st1.pos = 1000, st1.calls = 0;
      TASK_CHECK(soxr_output(c3, (soxr_out_t)outp, 16) > 0);
      TASK_CHECK(st1.calls > 0);
      st2 = st1, st2.calls = 0;
      TASK_CHECK(!soxr_set_input_fn(c3, cb_input, &st2, CB_CHUNK));
      soxr_delete(src.r);                  /* Source gone; clone unaffected. */
      src.r = NULL;
      TASK_CHECK(soxr_output(c3, (soxr_out_t)outp, 32) > 0);
      TASK_CHECK(st2.calls > 0);
      soxr_delete(c3);
    }
  }

  soxr_delete(src.r);                     /* c1/c2 already drained their cache. */

  free(l1);
  free(l2);
  soxr_delete(c1);
  soxr_delete(c2);
  return bad;
}

static int callback_case(void)
{
  /* Mono and stereo, float32 and int16 (the dither random state must be
   * inherited).  Interleaved layouts only: soxr_output's callback retry loop
   * advances its output pointer as for interleaved data, which is the long-
   * standing behaviour this test must not be read as changing.  Split
   * layouts are exercised through soxr_process in the history/branch groups. */
  int bad = 0;
  bad |= callback_case_cfg(cfgs[0]);      /* mono f32 interleaved */
  bad |= callback_case_cfg(cfgs[1]);      /* stereo f32 interleaved */
  bad |= callback_case_cfg(cfgs[7]);      /* stereo int16 interleaved */
  return bad;
}



/* -------------------------------- registry -------------------------------- */

#define HISTORY_ENTRY(name, idx) \
  static int name(void) { return history_case(idx); }

HISTORY_ENTRY(h_mono_f32_i,   0)
HISTORY_ENTRY(h_stereo_f32_i, 1)
HISTORY_ENTRY(h_6ch_f32_i,    2)
HISTORY_ENTRY(h_mono_f32_s,   3)
HISTORY_ENTRY(h_stereo_f32_s, 4)
HISTORY_ENTRY(h_6ch_f32_s,    5)
HISTORY_ENTRY(h_mono_s16_i,   6)
HISTORY_ENTRY(h_stereo_s16_i, 7)
HISTORY_ENTRY(h_6ch_s16_i,    8)
HISTORY_ENTRY(h_mono_s16_s,   9)
HISTORY_ENTRY(h_stereo_s16_s, 10)
HISTORY_ENTRY(h_6ch_s16_s,    11)
static int h_errors(void) { return error_case(); }

#define BRANCH_ENTRY(name, idx) \
  static int name(void) { return branch_case(idx); }

BRANCH_ENTRY(b_mono_f32_i,   0)
BRANCH_ENTRY(b_stereo_f32_i, 1)
BRANCH_ENTRY(b_6ch_f32_i,    2)
BRANCH_ENTRY(b_mono_f32_s,   3)
BRANCH_ENTRY(b_stereo_s16_i, 7)
BRANCH_ENTRY(b_6ch_s16_s,    11)

const struct task_case task_cases[] = {
  {"history",  "mono_f32_i",       h_mono_f32_i},
  {"history",  "stereo_f32_i",     h_stereo_f32_i},
  {"history",  "6ch_f32_i",        h_6ch_f32_i},
  {"history",  "mono_f32_s",       h_mono_f32_s},
  {"history",  "stereo_f32_s",     h_stereo_f32_s},
  {"history",  "6ch_f32_s",        h_6ch_f32_s},
  {"history",  "mono_s16_i",       h_mono_s16_i},
  {"history",  "stereo_s16_i",     h_stereo_s16_i},
  {"history",  "6ch_s16_i",        h_6ch_s16_i},
  {"history",  "mono_s16_s",       h_mono_s16_s},
  {"history",  "stereo_s16_s",     h_stereo_s16_s},
  {"history",  "6ch_s16_s",        h_6ch_s16_s},
  {"history",  "error_handling",   h_errors},

  {"branch",   "mono_f32_i",       b_mono_f32_i},
  {"branch",   "stereo_f32_i",     b_stereo_f32_i},
  {"branch",   "6ch_f32_i",        b_6ch_f32_i},
  {"branch",   "mono_f32_s",       b_mono_f32_s},
  {"branch",   "stereo_s16_i",     b_stereo_s16_i},
  {"branch",   "6ch_s16_s",        b_6ch_s16_s},
  {"branch",   "stage_fork_evidence", evidence_stage_case},
  {"branch",   "xfade_fork_evidence", evidence_xfade_case},

  {"callback", "callback_isolation", callback_case},
};

const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
