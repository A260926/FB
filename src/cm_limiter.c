/*
** Lookahead limiter for cmixer.
**
** The mixed, master-gained stereo signal x[t] is delayed by L frames. Each
** output frame n is scaled by the shared envelope
**
**   p[n] = max |x[k]|               over k in [n, n+L], both channels
**   a[n] = p[n] ? min(1, ceiling/p[n]) : 1
**   g[n] = min(a[n], g[n-1] + 1/release_frames),   g[-1] = 1
**
** and both channels use the same g[n] (stereo-linked). The sliding peak is
** maintained with a monotone deque, giving amortized O(1) work per frame.
**
** Every source owns a ring with its queued (not yet delivered) contribution
** so that cm_stop()/cm_destroy_source() can revoke it: the frames are
** subtracted from the queued sums and the peak window is rebuilt from what
** remains. The master gain is baked into the queued sums (and remembered per
** frame) so later gain changes never re-amplify queued audio.
*/

#include "cm_limiter.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#define LIM_FX_BITS       12
#define LIM_MAX_LOOKAHEAD 65536
#define LIM_MAX_CEILING   32767

struct cm_LimSource {
  int lookahead;
  int nonempty;   /* whether the ring may hold non-zero data */
  int ring[];     /* 2 * lookahead: queued stereo contribution, frame % lookahead */
};

struct cm_LimPlan {
  int lookahead;
  int ceiling;
  int release;
  int *sum;         /* 2 * lookahead: queued pre-master-gain sums */
  int *gainq;       /* lookahead: master gain (Q12) per queued frame */
  long long *dqidx; /* lookahead + 1: monotone deque of frame indices */
  int *dqval;       /* lookahead + 1: deque peak values */
};

static struct {
  int enabled;
  int lookahead;
  int ceiling;
  int release;
  double inv_release;
  long long T;      /* absolute frame clock: frames in == frames out */
  double g;         /* envelope carried into the next emitted frame */
  double last_gain; /* envelope used by the last emitted frame */
  int *sum;
  int *gainq;
  long long *dqidx;
  int *dqval;
  int dqhead;
  int dqcount;
} lim = { .g = 1.0, .last_gain = 1.0 };


static short lim_clamp16(long long v) {
  if (v < -32768) return -32768;
  if (v > 32767) return 32767;
  return (short) v;
}


static int lim_peak(int l, int r) {
  if (l < 0) l = -l;
  if (r < 0) r = -r;
  return l > r ? l : r;
}


/* Pushes (idx, val) keeping values monotonically decreasing from head to
** tail; equal values evict the older entry, so an equal peak leaving the
** window is still accounted for by its newer sibling. */
static void dq_push(long long idx, int val) {
  int cap = lim.lookahead + 1;
  int pos;
  while (lim.dqcount > 0) {
    int back = lim.dqhead + lim.dqcount - 1;
    if (back >= cap) back -= cap;
    if (lim.dqval[back] > val) break;
    lim.dqcount--;
  }
  pos = lim.dqhead + lim.dqcount;
  if (pos >= cap) pos -= cap;
  lim.dqidx[pos] = idx;
  lim.dqval[pos] = val;
  lim.dqcount++;
}


static void dq_evict_before(long long lo) {
  int cap = lim.lookahead + 1;
  while (lim.dqcount > 0 && lim.dqidx[lim.dqhead] < lo) {
    lim.dqhead++;
    if (lim.dqhead >= cap) lim.dqhead = 0;
    lim.dqcount--;
  }
}


cm_LimPlan *cm_lim_plan_new(int lookahead_frames, int ceiling, int release_frames, const char **err) {
  cm_LimPlan *p;
  if (err) *err = NULL;
  if (lookahead_frames < 1 || lookahead_frames > LIM_MAX_LOOKAHEAD) {
    if (err) *err = "invalid lookahead_frames (expected 1..65536)";
    return NULL;
  }
  if (ceiling < 1 || ceiling > LIM_MAX_CEILING) {
    if (err) *err = "invalid ceiling (expected 1..32767)";
    return NULL;
  }
  if (release_frames < 1) {
    if (err) *err = "invalid release_frames (expected > 0)";
    return NULL;
  }
  p = calloc(1, sizeof(*p));
  if (!p) goto oom;
  p->sum   = calloc((size_t) 2 * lookahead_frames, sizeof(int));
  p->gainq = calloc((size_t) lookahead_frames, sizeof(int));
  p->dqidx = calloc((size_t) lookahead_frames + 1, sizeof(long long));
  p->dqval = calloc((size_t) lookahead_frames + 1, sizeof(int));
  if (!p->sum || !p->gainq || !p->dqidx || !p->dqval) goto oom;
  p->lookahead = lookahead_frames;
  p->ceiling = ceiling;
  p->release = release_frames;
  return p;
oom:
  if (p) {
    free(p->sum);
    free(p->gainq);
    free(p->dqidx);
    free(p->dqval);
    free(p);
  }
  if (err) *err = "allocation failed";
  return NULL;
}


void cm_lim_plan_abort(cm_LimPlan *p) {
  if (!p) return;
  free(p->sum);
  free(p->gainq);
  free(p->dqidx);
  free(p->dqval);
  free(p);
}


void cm_lim_plan_commit(cm_LimPlan *p) {
  if (!p) return;
  free(lim.sum);
  free(lim.gainq);
  free(lim.dqidx);
  free(lim.dqval);
  lim.sum = p->sum;
  lim.gainq = p->gainq;
  lim.dqidx = p->dqidx;
  lim.dqval = p->dqval;
  lim.lookahead = p->lookahead;
  lim.ceiling = p->ceiling;
  lim.release = p->release;
  lim.inv_release = 1.0 / (double) p->release;
  lim.T = 0;
  lim.g = 1.0;
  lim.last_gain = 1.0;
  lim.dqhead = 0;
  lim.dqcount = 0;
  lim.enabled = 1;
  free(p);
}


cm_LimSource *cm_lim_source_new(int lookahead_frames) {
  cm_LimSource *s = calloc(1, sizeof(*s) + (size_t) 2 * lookahead_frames * sizeof(int));
  if (!s) return NULL;
  s->lookahead = lookahead_frames;
  return s;
}


void cm_lim_source_free(cm_LimSource *s) {
  free(s);
}


void cm_lim_source_clear(cm_LimSource *s) {
  if (!s) return;
  memset(s->ring, 0, (size_t) 2 * s->lookahead * sizeof(int));
  s->nonempty = 0;
}


int cm_lim_enabled(void) {
  return lim.enabled;
}


int cm_lim_lookahead(void) {
  return lim.lookahead;
}


double cm_lim_last_gain(void) {
  return lim.last_gain;
}


void cm_lim_reset(void) {
  if (!lim.enabled) return;
  lim.T = 0;
  lim.g = 1.0;
  lim.last_gain = 1.0;
  lim.dqhead = 0;
  lim.dqcount = 0;
  /* The rings are left alone: slots are (re)written before they can be read
  ** again now that the clock restarted. Source rings are cleared by the
  ** caller, which owns the source list. */
}


void cm_lim_disable(void) {
  if (!lim.enabled) return;
  free(lim.sum);
  free(lim.gainq);
  free(lim.dqidx);
  free(lim.dqval);
  lim.sum = NULL;
  lim.gainq = NULL;
  lim.dqidx = NULL;
  lim.dqval = NULL;
  lim.enabled = 0;
  lim.lookahead = 0;
  lim.T = 0;
  lim.g = 1.0;
  lim.dqhead = 0;
  lim.dqcount = 0;
}


int cm_lim_set_parameters(int ceiling, int release_frames, const char **err) {
  if (err) *err = NULL;
  if (!lim.enabled) {
    if (err) *err = "limiter is not enabled";
    return 0;
  }
  if (ceiling < 1 || ceiling > LIM_MAX_CEILING) {
    if (err) *err = "invalid ceiling (expected 1..32767)";
    return 0;
  }
  if (release_frames < 1) {
    if (err) *err = "invalid release_frames (expected > 0)";
    return 0;
  }
  lim.ceiling = ceiling;
  lim.release = release_frames;
  lim.inv_release = 1.0 / (double) release_frames;
  return 1;
}


void cm_lim_source_put(cm_LimSource *s, const int *stereo_frames, int nframes) {
  int L, i;
  long long t;
  if (!lim.enabled || !s) return;
  L = lim.lookahead;
  t = lim.T;
  for (i = 0; i < nframes; i++) {
    int slot = (int) ((t + i) % L);
    s->ring[2*slot    ] = stereo_frames[2*i    ];
    s->ring[2*slot + 1] = stereo_frames[2*i + 1];
  }
  if (nframes > 0) s->nonempty = 1;
}


void cm_lim_source_put_silence(cm_LimSource *s, int nframes) {
  int L, i;
  long long t;
  if (!lim.enabled || !s) return;
  L = lim.lookahead;
  t = lim.T;
  for (i = 0; i < nframes; i++) {
    int slot = (int) ((t + i) % L);
    s->ring[2*slot    ] = 0;
    s->ring[2*slot + 1] = 0;
  }
}


void cm_lim_revoke(cm_LimSource *s) {
  long long lo, k;
  int L;
  if (!lim.enabled || !s || !s->nonempty) return;
  L = lim.lookahead;
  lo = lim.T - L;
  if (lo < 0) lo = 0;
  /* Remove every queued (not yet delivered) contribution of the source */
  for (k = lo; k < lim.T; k++) {
    int slot = (int) (k % L);
    lim.sum[2*slot    ] -= s->ring[2*slot    ];
    lim.sum[2*slot + 1] -= s->ring[2*slot + 1];
    s->ring[2*slot    ] = 0;
    s->ring[2*slot + 1] = 0;
  }
  s->nonempty = 0;
  /* Rebuild the peak window over the remaining sums; the envelope itself is
  ** not rolled back and keeps releasing from its current value */
  lim.dqhead = 0;
  lim.dqcount = 0;
  for (k = lo; k < lim.T; k++) {
    int slot = (int) (k % L);
    int xl = (int) (((long long) lim.sum[2*slot    ] * lim.gainq[slot]) >> LIM_FX_BITS);
    int xr = (int) (((long long) lim.sum[2*slot + 1] * lim.gainq[slot]) >> LIM_FX_BITS);
    dq_push(k, lim_peak(xl, xr));
  }
}


void cm_lim_push(const int *stereo_sum, int nframes, int master_gain_q12, short *dst) {
  int L = lim.lookahead;
  int i;
  for (i = 0; i < nframes; i++) {
    long long t = lim.T;
    int slot = (int) (t % L);
    int xl = 0, xr = 0, nl, nr;
    /* Read the frame leaving the delay line before overwriting its slot */
    if (t >= L) {
      xl = (int) (((long long) lim.sum[2*slot    ] * lim.gainq[slot]) >> LIM_FX_BITS);
      xr = (int) (((long long) lim.sum[2*slot + 1] * lim.gainq[slot]) >> LIM_FX_BITS);
    }
    lim.sum[2*slot    ] = stereo_sum[2*i    ];
    lim.sum[2*slot + 1] = stereo_sum[2*i + 1];
    lim.gainq[slot]     = master_gain_q12;
    nl = (int) (((long long) stereo_sum[2*i    ] * master_gain_q12) >> LIM_FX_BITS);
    nr = (int) (((long long) stereo_sum[2*i + 1] * master_gain_q12) >> LIM_FX_BITS);
    if (t >= L) dq_evict_before(t - L);
    dq_push(t, lim_peak(nl, nr));
    if (t >= L) {
      int p = lim.dqval[lim.dqhead];
      double a = p ? lim.ceiling / (double) p : 1.0;
      double g;
      if (a > 1.0) a = 1.0;
      g = lim.g + lim.inv_release;
      if (a < g) g = a;
      lim.g = g;
      lim.last_gain = g;
      dst[2*i    ] = lim_clamp16(llround(xl * g));
      dst[2*i + 1] = lim_clamp16(llround(xr * g));
    } else {
      /* Initial latency: the envelope recursion does not advance yet */
      dst[2*i    ] = 0;
      dst[2*i + 1] = 0;
    }
    lim.T = t + 1;
  }
}
