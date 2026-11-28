/*
** cm_limiter.c -- stereo-linked lookahead limiter for cmixer.
**
** Signal chain: the mixer hands over the wide (unclipped) stereo sum of
** all source contributions, the Q12 master gain is applied in 64 bit, and
** the result x[t] enters a delay line of L frames. For output frame n the
** peak p[n] is the maximum absolute sample over the window n..n+L, the
** target a[n] = min(1, ceiling/p[n]) and the gain follows
** g[n] = min(a[n], g[n-1] + 1/release_frames) with g[-1] = 1. Both
** channels share the same g[n]; rounding is to nearest, midpoints away
** from zero, then the result is clipped to int16.
**
** The peak window is a monotonic deque: bounded storage, amortized O(1)
** per frame, and equal peaks expiring from the window stay correct. Each
** source owns a ring of its queued contributions so that cm_stop() and
** cm_destroy_source() can withdraw everything the source has not yet
** delivered; the affected peak windows are then recomputed from the
** remaining sum while already emitted gain is kept.
*/

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "cm_limiter.h"

#define CM_LIM_FX_BITS  12   /* must match FX_BITS in src/cmixer.c */

struct cm_LimSlot {
  cm_LimSlot *next;         /* live registry list */
  cm_LimSlot *pnext;        /* pending list (during configuration) */
  cm_LimSlot **owner;       /* where the owner keeps this slot */
  cm_LimSlot *replaces;     /* slot superseded on commit, or NULL */
  cm_Int64 last_write;      /* last input frame recorded, -1 when none */
  cm_Int32 ring[];          /* 2 * L interleaved pre-gain contributions */
};

typedef struct {
  void *block;              /* single allocation backing the arrays below */
  cm_Int64 *dqidx;          /* monotonic deque: frame indices */
  cm_Int32 *sum;            /* 2*(L+1) interleaved pre-gain mix */
  cm_Int32 *mgain;          /* (L+1) Q12 master gain per frame */
  cm_Int32 *dqval;          /* monotonic deque: peak values */
} cm_LimRings;

static struct {
  int enabled;
  int L, ceiling, release;
  cm_Int64 t;               /* next input frame index */
  double g;                 /* gain of the last emitted frame (g[n-1]) */
  double last_gain;         /* reported by cm_lim_last_gain() */
  cm_LimRings rings;
  cm_Int64 dq_head, dq_tail;
  cm_LimSlot *slots;
  int pending;              /* a configuration is being prepared */
  int p_L, p_ceiling, p_release;
  cm_LimRings p_rings;
  cm_LimSlot *p_slots;
} lim = { .release = 1, .g = 1.0, .last_gain = 1.0 };


static int rings_alloc(cm_LimRings *r, int L) {
  size_t n = (size_t) L + 1;
  size_t b_idx = n * sizeof(cm_Int64);
  size_t b_sum = 2 * n * sizeof(cm_Int32);
  size_t b_mg  = n * sizeof(cm_Int32);
  size_t b_val = n * sizeof(cm_Int32);
  char *p = (char*) calloc(1, b_idx + b_sum + b_mg + b_val);
  if (!p) {
    return 0;
  }
  r->block = p;
  r->dqidx = (cm_Int64*) p; p += b_idx;   /* keep 8-byte alignment first */
  r->sum   = (cm_Int32*) p; p += b_sum;
  r->mgain = (cm_Int32*) p; p += b_mg;
  r->dqval = (cm_Int32*) p;
  return 1;
}


static void rings_free(cm_LimRings *r) {
  if (r->block) {
    free(r->block);
  }
  r->block = NULL;
  r->dqidx = NULL;
  r->sum = NULL;
  r->mgain = NULL;
  r->dqval = NULL;
}


static cm_LimSlot* slot_alloc(cm_LimSlot **owner, int L) {
  cm_LimSlot *s = (cm_LimSlot*) calloc(1, sizeof(*s) +
                                        (size_t) 2 * L * sizeof(cm_Int32));
  if (!s) {
    return NULL;
  }
  s->owner = owner;
  s->last_write = -1;
  return s;
}


static void slot_unlink(cm_LimSlot *slot) {
  cm_LimSlot **p = &lim.slots;
  while (*p) {
    if (*p == slot) {
      *p = slot->next;
      return;
    }
    p = &(*p)->next;
  }
}


int cm_lim_enabled(void) {
  return lim.enabled;
}


int cm_lim_latency(void) {
  return lim.enabled ? lim.L : 0;
}


double cm_lim_last_gain(void) {
  return lim.last_gain;
}


int cm_lim_prepare(int lookahead_frames, int ceiling, int release_frames) {
  if (lim.pending) {
    cm_lim_rollback();
  }
  if (!rings_alloc(&lim.p_rings, lookahead_frames)) {
    return 0;
  }
  lim.p_L = lookahead_frames;
  lim.p_ceiling = ceiling;
  lim.p_release = release_frames;
  lim.p_slots = NULL;
  lim.pending = 1;
  return 1;
}


cm_LimSlot* cm_lim_slot_create(cm_LimSlot **owner) {
  cm_LimSlot *s;
  if (lim.pending) {
    /* Stage a fresh slot for the pending configuration; the owner's
    ** current slot (if any) is replaced only on commit. */
    s = slot_alloc(owner, lim.p_L);
    if (!s) {
      return NULL;
    }
    s->replaces = *owner;
    s->pnext = lim.p_slots;
    lim.p_slots = s;
    return s;
  }
  if (!lim.enabled) {
    return NULL;
  }
  s = slot_alloc(owner, lim.L);
  if (!s) {
    return NULL;
  }
  *owner = s;
  s->next = lim.slots;
  lim.slots = s;
  return s;
}


void cm_lim_commit(void) {
  cm_LimSlot *s;
  if (!lim.pending) {
    return;
  }
  rings_free(&lim.rings);
  lim.rings = lim.p_rings;
  memset(&lim.p_rings, 0, sizeof(lim.p_rings));
  lim.L = lim.p_L;
  lim.ceiling = lim.p_ceiling;
  lim.release = lim.p_release;
  while (lim.p_slots) {
    s = lim.p_slots;
    lim.p_slots = s->pnext;
    s->pnext = NULL;
    if (s->replaces) {
      slot_unlink(s->replaces);
      if (s->replaces->owner && *s->replaces->owner == s->replaces) {
        *s->replaces->owner = NULL;
      }
      free(s->replaces);
      s->replaces = NULL;
    }
    if (s->owner) {
      *s->owner = s;
    }
    s->next = lim.slots;
    lim.slots = s;
  }
  lim.t = 0;
  lim.g = 1.0;
  lim.last_gain = 1.0;
  lim.dq_head = lim.dq_tail = 0;
  lim.enabled = 1;
  lim.pending = 0;
}


void cm_lim_rollback(void) {
  if (!lim.pending) {
    return;
  }
  rings_free(&lim.p_rings);
  while (lim.p_slots) {
    cm_LimSlot *s = lim.p_slots;
    lim.p_slots = s->pnext;
    free(s);
  }
  lim.pending = 0;
}


void cm_lim_set_parameters(int ceiling, int release_frames) {
  if (!lim.enabled) {
    return;
  }
  lim.ceiling = ceiling;
  lim.release = release_frames;
}


void cm_lim_reset(void) {
  cm_LimSlot *s;
  if (!lim.enabled) {
    return;
  }
  lim.t = 0;
  lim.g = 1.0;
  lim.last_gain = 1.0;
  lim.dq_head = lim.dq_tail = 0;
  if (lim.rings.block) {
    size_t n = (size_t) lim.L + 1;
    memset(lim.rings.block, 0, n * (sizeof(cm_Int64) + 4 * sizeof(cm_Int32)));
  }
  for (s = lim.slots; s; s = s->next) {
    memset(s->ring, 0, (size_t) 2 * lim.L * sizeof(cm_Int32));
    s->last_write = -1;
  }
}


void cm_lim_disable(void) {
  if (lim.pending) {
    cm_lim_rollback();
  }
  rings_free(&lim.rings);
  while (lim.slots) {
    cm_LimSlot *s = lim.slots;
    lim.slots = s->next;
    if (s->owner && *s->owner == s) {
      *s->owner = NULL;
    }
    free(s);
  }
  lim.enabled = 0;
  lim.L = 0;
  lim.ceiling = 0;
  lim.release = 1;
  lim.t = 0;
  lim.g = 1.0;
  lim.last_gain = 1.0;
  lim.dq_head = lim.dq_tail = 0;
}


void cm_lim_slot_destroy(cm_LimSlot *slot) {
  if (!slot) {
    return;
  }
  slot_unlink(slot);
  if (slot->owner && *slot->owner == slot) {
    *slot->owner = NULL;
  }
  free(slot);
}


/* Peak of the (master-gain scaled) mix recorded at frame k. */
static cm_Int32 peak_of(cm_Int64 k) {
  size_t si = (size_t) (k % (lim.L + 1));
  cm_Int32 xl = (cm_Int32) (((cm_Int64) lim.rings.sum[2*si] *
                             lim.rings.mgain[si]) >> CM_LIM_FX_BITS);
  cm_Int32 xr = (cm_Int32) (((cm_Int64) lim.rings.sum[2*si + 1] *
                             lim.rings.mgain[si]) >> CM_LIM_FX_BITS);
  cm_Int32 al = xl < 0 ? -xl : xl;
  cm_Int32 ar = xr < 0 ? -xr : xr;
  return al > ar ? al : ar;
}


static void deque_push(cm_Int64 idx, cm_Int32 val) {
  int cap = lim.L + 1;
  while (lim.dq_tail > lim.dq_head &&
         lim.rings.dqval[(lim.dq_tail - 1) % cap] <= val) {
    lim.dq_tail--;
  }
  lim.rings.dqidx[lim.dq_tail % cap] = idx;
  lim.rings.dqval[lim.dq_tail % cap] = val;
  lim.dq_tail++;
}


static void deque_rebuild(void) {
  cm_Int64 lo, k;
  lim.dq_head = lim.dq_tail = 0;
  if (lim.t == 0) {
    return;
  }
  lo = lim.t - 1 - lim.L;
  if (lo < 0) {
    lo = 0;
  }
  for (k = lo; k < lim.t; k++) {
    deque_push(k, peak_of(k));
  }
}


void cm_lim_slot_retract(cm_LimSlot *slot) {
  cm_Int64 lo, hi, k;
  int changed = 0;
  if (!lim.enabled || !slot || lim.t == 0) {
    return;
  }
  /* Pending (not yet emitted) frames are t-L .. t-1; the slot only holds
  ** frames up to its last write. */
  lo = lim.t - lim.L;
  if (lo < 0) {
    lo = 0;
  }
  hi = lim.t - 1;
  if (hi > slot->last_write) {
    hi = slot->last_write;
  }
  for (k = lo; k <= hi; k++) {
    size_t si = (size_t) (k % (lim.L + 1));
    size_t ri = (size_t) (k % lim.L);
    lim.rings.sum[2*si]     -= slot->ring[2*ri];
    lim.rings.sum[2*si + 1] -= slot->ring[2*ri + 1];
    slot->ring[2*ri] = 0;
    slot->ring[2*ri + 1] = 0;
    changed = 1;
  }
  if (changed) {
    deque_rebuild();
  }
}


void cm_lim_slot_write(cm_LimSlot *slot, const cm_Int32 *frames, int count) {
  cm_Int64 base;
  int j;
  if (!lim.enabled || !slot || count <= 0) {
    return;
  }
  base = lim.t;
  for (j = 0; j < count; j++) {
    size_t idx = (size_t) ((base + j) % lim.L);
    slot->ring[2*idx]     = frames[2*j];
    slot->ring[2*idx + 1] = frames[2*j + 1];
  }
  slot->last_write = base + count - 1;
}


void cm_lim_process(const cm_Int32 *sum, cm_Int32 mgain, int frames,
                    cm_Int16 *dst) {
  int j, cap;
  if (!lim.enabled || frames <= 0) {
    return;
  }
  cap = lim.L + 1;
  for (j = 0; j < frames; j++) {
    cm_Int64 t = lim.t;
    size_t si = (size_t) (t % cap);
    lim.rings.sum[2*si]     = sum[2*j];
    lim.rings.sum[2*si + 1] = sum[2*j + 1];
    lim.rings.mgain[si]     = mgain;
    deque_push(t, peak_of(t));
    while (lim.dq_head < lim.dq_tail &&
           lim.rings.dqidx[lim.dq_head % cap] < t - lim.L) {
      lim.dq_head++;
    }
    if (t >= lim.L) {
      cm_Int64 n = t - lim.L;
      size_t ni = (size_t) (n % cap);
      cm_Int32 pn = lim.rings.dqval[lim.dq_head % cap];
      double a = pn == 0 ? 1.0 : (double) lim.ceiling / (double) pn;
      double g;
      cm_Int32 xnl, xnr;
      long yl, yr;
      if (a > 1.0) {
        a = 1.0;
      }
      g = lim.g + 1.0 / (double) lim.release;
      if (a < g) {
        g = a;
      }
      lim.g = g;
      lim.last_gain = g;
      xnl = (cm_Int32) (((cm_Int64) lim.rings.sum[2*ni] *
                         lim.rings.mgain[ni]) >> CM_LIM_FX_BITS);
      xnr = (cm_Int32) (((cm_Int64) lim.rings.sum[2*ni + 1] *
                         lim.rings.mgain[ni]) >> CM_LIM_FX_BITS);
      yl = lround((double) xnl * g);
      yr = lround((double) xnr * g);
      dst[2*j]     = (cm_Int16) (yl < -32768 ? -32768 : yl > 32767 ? 32767 : yl);
      dst[2*j + 1] = (cm_Int16) (yr < -32768 ? -32768 : yr > 32767 ? 32767 : yr);
    } else {
      dst[2*j] = 0;
      dst[2*j + 1] = 0;
    }
    lim.t = t + 1;
  }
}
