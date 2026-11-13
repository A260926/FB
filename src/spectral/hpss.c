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

/** @file
 *
 * Harmonic / percussive source separation with bounded sliding medians.
 *
 * The time median of each frequency bin, and the frequency median of every
 * emitted frame, are maintained by an AVL order-statistics tree used as a
 * sliding window:
 *
 *  - a new frame inserts one value per bin; the frame leaving the window
 *    removes exactly its own element, found by its (value, identity) key,
 *    so equal and zero values are always treated as distinct samples;
 *  - every insertion, removal and median query is
 *    O(log(2*radius+1)) key comparisons, including the shrinking windows
 *    at the end of the recording;
 *  - even member counts average the two middle values.
 *
 * Analysis and synthesis reuse the exact buffering of aubio_pvoc_t
 * (HanningZ window, half-window swap, 75% overlap-add with the 2/3 scaling)
 * through the existing aubio_fft_t backend. No external audio library is
 * used and no heap memory is touched outside of new_aubio_hpss() and
 * del_aubio_hpss().
 *
 */

#include "aubio_priv.h"
#include "fvec.h"
#include "fmat.h"
#include "cvec.h"
#include "mathutils.h"
#include "spectral/fft.h"
#include "spectral/hpss.h"

/* ------------------------------------------------------------------ */
/* AVL order-statistics tree                                           */
/* ------------------------------------------------------------------ */

/**
 * Tree node. Keys are (value, identity) pairs: values that compare equal,
 * in particular zero values, are ordered by their unique identity so that
 * every inserted sample can be removed individually.
 */
typedef struct {
  smpl_t value;
  uint_t id;
  sint_t left, right, parent;
  uint_t size;     /* number of nodes in this subtree */
  uint_t height;   /* AVL height, leaves are 1 */
} med_node_t;

typedef struct {
  med_node_t *nodes;   /* fixed-capacity pool, free list threaded via parent */
  uint_t capacity;
  sint_t root;
  sint_t free_head;
  uint_t count;
  uint_t next_id;
  uint_t inserts;
  uint_t removes;
  uint_t comparisons; /* key comparisons, the observable O(log n) cost */
} med_engine_t;

static uint_t med_node_height(const med_engine_t *e, sint_t n)
{
  return n < 0 ? 0 : e->nodes[n].height;
}

static uint_t med_node_size(const med_engine_t *e, sint_t n)
{
  return n < 0 ? 0 : e->nodes[n].size;
}

static void med_update(med_engine_t *e, sint_t n)
{
  med_node_t *nd = &e->nodes[n];
  uint_t hl = med_node_height(e, nd->left);
  uint_t hr = med_node_height(e, nd->right);
  nd->height = 1 + (hl > hr ? hl : hr);
  nd->size = 1 + med_node_size(e, nd->left)
      + med_node_size(e, nd->right);
}

static void med_engine_init(med_engine_t *e, med_node_t *storage,
    uint_t capacity)
{
  uint_t i;
  e->nodes = storage;
  e->capacity = capacity;
  e->root = -1;
  e->count = 0;
  e->next_id = 0;
  e->inserts = 0;
  e->removes = 0;
  e->comparisons = 0;
  for (i = 0; i < capacity; i++) {
    e->nodes[i].left = -1;
    e->nodes[i].right = -1;
    e->nodes[i].parent = (i + 1 < capacity) ? (sint_t)(i + 1) : -1;
    e->nodes[i].size = 0;
    e->nodes[i].height = 0;
    e->nodes[i].value = 0.;
    e->nodes[i].id = 0;
  }
  e->free_head = capacity ? 0 : -1;
}

static void med_engine_reset(med_engine_t *e)
{
  med_engine_init(e, e->nodes, e->capacity);
}

/** ordering of a loose key against a node, counted for the tests */
static uint_t med_key_less(med_engine_t *e, sint_t n, smpl_t value,
    uint_t id)
{
  const med_node_t *nd = &e->nodes[n];
  e->comparisons++;
  if (value < nd->value) return 1;
  if (value > nd->value) return 0;
  return id < nd->id;
}

static void med_replace_child(med_engine_t *e, sint_t parent,
    sint_t old_child, sint_t new_child)
{
  if (parent < 0) {
    e->root = new_child;
  } else if (e->nodes[parent].left == old_child) {
    e->nodes[parent].left = new_child;
  } else {
    e->nodes[parent].right = new_child;
  }
}

static sint_t med_rotate_left(med_engine_t *e, sint_t y)
{
  sint_t x = e->nodes[y].right;
  sint_t b = e->nodes[x].left;
  sint_t parent = e->nodes[y].parent;
  e->nodes[x].left = y;
  e->nodes[y].parent = x;
  e->nodes[y].right = b;
  if (b >= 0) e->nodes[b].parent = y;
  e->nodes[x].parent = parent;
  med_replace_child(e, parent, y, x);
  med_update(e, y);
  med_update(e, x);
  return x;
}

static sint_t med_rotate_right(med_engine_t *e, sint_t y)
{
  sint_t x = e->nodes[y].left;
  sint_t b = e->nodes[x].right;
  sint_t parent = e->nodes[y].parent;
  e->nodes[x].right = y;
  e->nodes[y].parent = x;
  e->nodes[y].left = b;
  if (b >= 0) e->nodes[b].parent = y;
  e->nodes[x].parent = parent;
  med_replace_child(e, parent, y, x);
  med_update(e, y);
  med_update(e, x);
  return x;
}

static void med_rebalance(med_engine_t *e, sint_t n)
{
  while (n >= 0) {
    sint_t parent = e->nodes[n].parent;
    sint_t l, r;
    sint_t bf;
    med_update(e, n);
    l = e->nodes[n].left;
    r = e->nodes[n].right;
    bf = (sint_t)med_node_height(e, l)
       - (sint_t)med_node_height(e, r);
    if (bf > 1) {
      if (med_node_height(e, e->nodes[l].left)
          < med_node_height(e, e->nodes[l].right)) {
        sint_t nl = med_rotate_left(e, l);
        e->nodes[n].left = nl;
        e->nodes[nl].parent = n;
      }
      n = med_rotate_right(e, n);
    } else if (bf < -1) {
      if (med_node_height(e, e->nodes[r].right)
          < med_node_height(e, e->nodes[r].left)) {
        sint_t nr = med_rotate_right(e, r);
        e->nodes[n].right = nr;
        e->nodes[nr].parent = n;
      }
      n = med_rotate_left(e, n);
    }
    n = parent;
  }
}

/** insert a (value, identity) element; the identity is assigned here */
static void med_engine_insert(med_engine_t *e, smpl_t value)
{
  sint_t slot, cur, parent;
  uint_t id;
  if (e->count >= e->capacity) return;
  slot = e->free_head;
  e->free_head = e->nodes[slot].parent;
  id = e->next_id++;
  e->nodes[slot].value = value;
  e->nodes[slot].id = id;
  e->nodes[slot].left = -1;
  e->nodes[slot].right = -1;
  e->nodes[slot].parent = -1;
  e->nodes[slot].size = 1;
  e->nodes[slot].height = 1;
  e->inserts++;
  if (e->root < 0) {
    e->root = slot;
    e->count = 1;
    return;
  }
  cur = e->root;
  for (;;) {
    parent = cur;
    if (med_key_less(e, cur, value, id)) {
      cur = e->nodes[cur].left;
      if (cur < 0) {
        e->nodes[parent].left = slot;
        break;
      }
    } else {
      cur = e->nodes[cur].right;
      if (cur < 0) {
        e->nodes[parent].right = slot;
        break;
      }
    }
  }
  e->nodes[slot].parent = parent;
  e->count++;
  med_rebalance(e, parent);
}

/** find the slot holding the (value, id) key, or -1 */
static sint_t med_engine_find(med_engine_t *e, smpl_t value, uint_t id)
{
  sint_t cur = e->root;
  while (cur >= 0) {
    if (med_key_less(e, cur, value, id)) {
      cur = e->nodes[cur].left;
    } else if (e->nodes[cur].value != value
        || e->nodes[cur].id != id) {
      cur = e->nodes[cur].right;
    } else {
      return cur;
    }
  }
  return -1;
}

static void med_engine_erase_slot(med_engine_t *e, sint_t n)
{
  sint_t parent, child, successor, rebalance_from;
  e->removes++;
  if (e->nodes[n].left >= 0 && e->nodes[n].right >= 0) {
    /* move the in-order successor's key into n, then remove successor */
    successor = e->nodes[n].right;
    while (e->nodes[successor].left >= 0)
      successor = e->nodes[successor].left;
    e->nodes[n].value = e->nodes[successor].value;
    e->nodes[n].id = e->nodes[successor].id;
    n = successor;
  }
  parent = e->nodes[n].parent;
  child = (e->nodes[n].left >= 0) ?
    e->nodes[n].left : e->nodes[n].right;
  if (child >= 0) e->nodes[child].parent = parent;
  med_replace_child(e, parent, n, child);
  if (e->root == n) e->root = child;
  e->count--;
  rebalance_from = parent;
  /* recycle the node through the free list */
  e->nodes[n].left = -1;
  e->nodes[n].right = -1;
  e->nodes[n].size = 0;
  e->nodes[n].height = 0;
  e->nodes[n].parent = e->free_head;
  e->free_head = n;
  if (rebalance_from >= 0) med_rebalance(e, rebalance_from);
}

/** remove the element identified by (value, id); O(log capacity) */
static void med_engine_remove(med_engine_t *e, smpl_t value, uint_t id)
{
  sint_t slot = med_engine_find(e, value, id);
  if (slot >= 0) med_engine_erase_slot(e, slot);
}

/** value of the k-th smallest element, k in [0, count) */
static smpl_t med_kth(const med_engine_t *e, uint_t k)
{
  sint_t cur = e->root;
  while (cur >= 0) {
    uint_t left = med_node_size(e, e->nodes[cur].left);
    if (k == left) return e->nodes[cur].value;
    if (k < left) cur = e->nodes[cur].left;
    else {
      k -= left + 1;
      cur = e->nodes[cur].right;
    }
  }
  return 0.;
}

/** median of the current window; even counts average the two middle values */
static smpl_t med_engine_median(const med_engine_t *e)
{
  uint_t n = e->count;
  if (n == 0) return 0.;
  if (n & 1u) return med_kth(e, n / 2);
  return (smpl_t)(0.5 * ((double)med_kth(e, (n - 1) / 2)
        + (double)med_kth(e, n / 2)));
}

/* ------------------------------------------------------------------ */
/* object definition                                                   */
/* ------------------------------------------------------------------ */

/** per-channel phase vocoder analysis/synthesis memory */
typedef struct {
  aubio_fft_t *fft;
  fvec_t *data;       /* analysis grain [win_s] */
  fvec_t *dataold;    /* past analysis samples [win_s-hop_s] */
  fvec_t *synth_h;    /* harmonic synthesis grain [win_s] */
  fvec_t *synth_p;    /* percussive synthesis grain [win_s] */
  fvec_t *synthold_h; /* harmonic overlap-add tail [win_s-hop_s] */
  fvec_t *synthold_p; /* percussive overlap-add tail [win_s-hop_s] */
  fvec_t *tmp;        /* windowed analysis grain [win_s] */
  cvec_t *grain;      /* analysis spectrum */
  cvec_t grain_h;     /* masked harmonic spectrum */
  cvec_t grain_p;     /* masked percussive spectrum */
} hpss_pvoc_t;

struct _aubio_hpss_t {
  uint_t win_s;
  uint_t hop_s;
  uint_t channels;
  uint_t bins;
  uint_t time_radius;
  uint_t freq_radius;
  uint_t ring_cap;          /* 2*time_radius+1 */
  uint_t freq_cap;          /* 2*freq_radius+1 */
  uint_t start, end;        /* overlap-add boundaries, as in pvoc */
  smpl_t scale;             /* overlap-add scaling, as in pvoc */

  hpss_pvoc_t *pv;
  fvec_t *window;           /* hanningz analysis window */

  cvec_t **ring;            /* ring_cap slots, each channels cvecs */
  smpl_t *mag_ring;         /* ring_cap * bins public magnitudes */
  uint_t *id_ring;          /* ring_cap * bins insertion identities */
  uint_t head;              /* slot of the oldest frame */
  uint_t filled;            /* valid slots, <= ring_cap */
  uint_t oldest_idx;        /* analysis frame index of the oldest slot */

  cvec_t **tail;            /* the three zero-input analysis frames */

  med_engine_t *time_med;   /* one engine per frequency bin */
  med_node_t *time_nodes;   /* ring_cap nodes per bin */
  med_engine_t freq_med;    /* single frequency engine, reused per frame */
  med_node_t *freq_nodes;
  uint_t *freq_id;          /* insertion identity of each bin, bins entries */

  smpl_t *mag;              /* public magnitude of the current frame */
  smpl_t *h_est;            /* harmonic magnitudes of the center frame */
  smpl_t *p_est;            /* percussive magnitudes of the center frame */
  smpl_t *mask;             /* harmonic soft mask */

  fmat_t *out_harm;         /* scratch output matrices, channels x hop */
  fmat_t *out_perc;

  uint_t do_count;          /* input hops consumed */
  uint_t lookahead;         /* zero output blocks still due */
  uint_t sealed;            /* aubio_hpss_finish was called once */
  uint_t tail_total;        /* time_radius + 3 */
  uint_t tail_delivered;    /* finish blocks already written */
  uint_t freq_compares;     /* accumulated frequency engine comparisons */
};

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static uint_t hpss_matrix_ok(const aubio_hpss_t *o, const fmat_t *m)
{
  return m != NULL && m->height == o->channels && m->length == o->hop_s;
}

static uint_t is_power_of_two(uint_t v)
{
  return v >= 2 && (v & (v - 1)) == 0;
}

/** shared magnitude a[k] = sqrt(mean over channels of norm^2) */
static void hpss_public_magnitude(aubio_hpss_t *o, const cvec_t *specs,
    smpl_t *mag)
{
  uint_t k, c;
  for (k = 0; k < o->bins; k++) {
    smpl_t acc = 0.;
    for (c = 0; c < o->channels; c++) {
      smpl_t n = specs[c].norm[k];
      acc += n * n;
    }
    mag[k] = SQRT(acc / (smpl_t)o->channels);
  }
}

/** the production soft mask formula, shared by the PCM path and the tests */
static void hpss_compute_mask(aubio_hpss_t *o, const smpl_t *h,
    const smpl_t *p)
{
  uint_t k;
  for (k = 0; k < o->bins; k++) {
    smpl_t h2 = h[k] * h[k];
    smpl_t p2 = p[k] * p[k];
    if (h2 == 0. && p2 == 0.) o->mask[k] = 0.5f;
    else o->mask[k] = h2 / (h2 + p2);
  }
}

/* ------------------------------------------------------------------ */
/* phase vocoder analysis/synthesis, identical buffering to pvoc       */
/* ------------------------------------------------------------------ */

static void hpss_analysis(aubio_hpss_t *o, hpss_pvoc_t *pv,
    const smpl_t *input)
{
  uint_t i;
  smpl_t *data = pv->data->data;
  smpl_t *dataold = pv->dataold->data;
  for (i = 0; i < o->end; i++) data[i] = dataold[i];
  for (i = 0; i < o->hop_s; i++) data[o->end + i] = input[i];
  for (i = 0; i < o->end; i++) dataold[i] = data[i + o->hop_s];
  for (i = 0; i < o->win_s; i++) pv->tmp->data[i] = data[i];
  fvec_weight(pv->tmp, o->window);
  fvec_shift(pv->tmp);
  aubio_fft_do(pv->fft, pv->tmp, pv->grain);
}

static void hpss_synth_one(aubio_hpss_t *o, hpss_pvoc_t *pv,
    fvec_t *synth, fvec_t *synthold, cvec_t *grain, smpl_t *out)
{
  uint_t i;
  smpl_t *s;
  aubio_fft_rdo(pv->fft, grain, synth);
  fvec_ishift(synth);
  /* 75% overlap: the hanningz window is re-applied on synthesis, exactly
   * as aubio_pvoc_rdo does, giving perfect reconstruction with the 2/3
   * overlap-add scaling below */
  fvec_weight(synth, o->window);
  s = synth->data;
  for (i = 0; i < o->hop_s; i++) out[i] = s[i] * o->scale;
  if (o->end == 0) return;
  for (i = 0; i < o->hop_s; i++) out[i] += synthold->data[i];
  for (i = 0; i < o->start; i++)
    synthold->data[i] = synthold->data[i + o->hop_s];
  for (i = o->start; i < o->end; i++) synthold->data[i] = 0.;
  for (i = 0; i < o->end; i++)
    synthold->data[i] += s[i + o->hop_s] * o->scale;
}

/* ------------------------------------------------------------------ */
/* ring of frames and time-median maintenance                          */
/* ------------------------------------------------------------------ */

/** slot where the next pushed frame will be stored */
static uint_t hpss_next_write_slot(const aubio_hpss_t *o)
{
  return (o->filled < o->ring_cap)
    ? (o->head + o->filled) % o->ring_cap : o->head;
}

/** push one magnitude frame into every per-bin sliding time window */
static void hpss_push_magnitudes(aubio_hpss_t *o, const smpl_t *mag)
{
  uint_t k;
  uint_t slot;
  if (o->filled == o->ring_cap) {
    /* the oldest frame leaves every window, removed by sample identity */
    slot = o->head;
    for (k = 0; k < o->bins; k++) {
      med_engine_remove(&o->time_med[k],
          o->mag_ring[(size_t)slot * o->bins + k],
          o->id_ring[(size_t)slot * o->bins + k]);
    }
    o->oldest_idx++;
  } else {
    slot = (o->head + o->filled) % o->ring_cap;
  }
  for (k = 0; k < o->bins; k++) {
    med_engine_t *e = &o->time_med[k];
    o->mag_ring[(size_t)slot * o->bins + k] = mag[k];
    o->id_ring[(size_t)slot * o->bins + k] = e->next_id;
    med_engine_insert(e, mag[k]);
  }
  if (o->filled < o->ring_cap) o->filled++;
  else o->head = (o->head + 1) % o->ring_cap;
}

/** drop the oldest frame: the shrinking tail windows, identity exact */
static void hpss_pop_oldest(aubio_hpss_t *o)
{
  uint_t k;
  uint_t slot = o->head;
  if (o->filled == 0) return;
  for (k = 0; k < o->bins; k++) {
    med_engine_remove(&o->time_med[k],
        o->mag_ring[(size_t)slot * o->bins + k],
        o->id_ring[(size_t)slot * o->bins + k]);
  }
  o->head = (o->head + 1) % o->ring_cap;
  o->filled--;
  o->oldest_idx++;
}

/**
 * Frequency medians of one magnitude frame with one sliding tree window.
 * The engine always holds exactly the clipped centered neighbourhood
 * [max(0,k-fr), min(bins-1,k+fr)] of the current bin: each bin entering
 * the right edge is inserted once and each bin leaving the left edge is
 * removed once, so the whole frame costs
 * O(bins * log(2*freq_radius+1)) rather than one sort per neighbourhood.
 */
static void hpss_freq_medians(aubio_hpss_t *o, const smpl_t *mag,
    smpl_t *out)
{
  uint_t k, lo, hi;
  med_engine_t *e = &o->freq_med;
  med_engine_reset(e);
  /* window of the first bin: [0, min(fr, bins-1)], no left padding */
  hi = o->freq_radius < o->bins - 1 ? o->freq_radius : o->bins - 1;
  for (k = 0; k <= hi; k++) {
    o->freq_id[k] = e->next_id;
    med_engine_insert(e, mag[k]);
  }
  lo = 0;
  for (k = 0; k < o->bins; k++) {
    out[k] = med_engine_median(e);
    if (k + 1 < o->bins) {
      uint_t new_lo = (k + 1 > o->freq_radius)
        ? k + 1 - o->freq_radius : 0;
      uint_t new_hi = k + 1 + o->freq_radius;
      if (new_hi > o->bins - 1) new_hi = o->bins - 1;
      /* shrink the left edge before growing the right edge so the fixed
       * freq_cap node pool never needs more than 2*freq_radius+1 nodes */
      while (lo < new_lo) {
        med_engine_remove(e, mag[lo], o->freq_id[lo]);
        lo++;
      }
      while (hi < new_hi) {
        hi++;
        o->freq_id[hi] = e->next_id;
        med_engine_insert(e, mag[hi]);
      }
    }
  }
  o->freq_compares += e->comparisons;
}

/* ------------------------------------------------------------------ */
/* center frame processing                                             */
/* ------------------------------------------------------------------ */

/**
 * Build the masks of center frame m and synthesize both stems. The stored
 * complex spectra are only read here: the magnitudes feeding the medians
 * live in separate scratch storage and never mutate the ring.
 */
static void hpss_emit_center(aubio_hpss_t *o, uint_t center_idx)
{
  uint_t k, c;
  uint_t offset = center_idx - o->oldest_idx;
  uint_t slot = (o->head + offset) % o->ring_cap;
  cvec_t **center = o->ring;

  for (k = 0; k < o->bins; k++)
    o->h_est[k] = med_engine_median(&o->time_med[k]);
  hpss_public_magnitude(o, center[slot], o->mag);
  hpss_freq_medians(o, o->mag, o->p_est);
  hpss_compute_mask(o, o->h_est, o->p_est);
  for (c = 0; c < o->channels; c++) {
    cvec_t *src = &center[slot][c];
    cvec_t *gh = &o->pv[c].grain_h;
    cvec_t *gp = &o->pv[c].grain_p;
    for (k = 0; k < o->bins; k++) {
      smpl_t m = o->mask[k];
      gh->norm[k] = src->norm[k] * m;
      gp->norm[k] = src->norm[k] * (1.f - m);
      gh->phas[k] = src->phas[k];
      gp->phas[k] = src->phas[k];
    }
    hpss_synth_one(o, &o->pv[c], o->pv[c].synth_h,
        o->pv[c].synthold_h, gh, o->out_harm->data[c]);
    hpss_synth_one(o, &o->pv[c], o->pv[c].synth_p,
        o->pv[c].synthold_p, gp, o->out_perc->data[c]);
  }
}

static void hpss_write_zeros(aubio_hpss_t *o, fmat_t *harmonic,
    fmat_t *percussive)
{
  uint_t c, k;
  for (c = 0; c < o->channels; c++) {
    for (k = 0; k < o->hop_s; k++) {
      harmonic->data[c][k] = 0.;
      percussive->data[c][k] = 0.;
    }
  }
}

static void hpss_copy_outputs(aubio_hpss_t *o, fmat_t *harmonic,
    fmat_t *percussive)
{
  uint_t c, k;
  for (c = 0; c < o->channels; c++) {
    for (k = 0; k < o->hop_s; k++) {
      harmonic->data[c][k] = o->out_harm->data[c][k];
      percussive->data[c][k] = o->out_perc->data[c][k];
    }
  }
}

static void hpss_store_grain(aubio_hpss_t *o, cvec_t *dst)
{
  uint_t c, k;
  for (c = 0; c < o->channels; c++) {
    for (k = 0; k < o->bins; k++) {
      dst[c].norm[k] = o->pv[c].grain->norm[k];
      dst[c].phas[k] = o->pv[c].grain->phas[k];
    }
  }
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

uint_t aubio_hpss_do(aubio_hpss_t *o, const fmat_t *in,
    fmat_t *harmonic, fmat_t *percussive)
{
  uint_t c;
  if (o == NULL || in == NULL || harmonic == NULL || percussive == NULL)
    return AUBIO_FAIL;
  if (!hpss_matrix_ok(o, in) || !hpss_matrix_ok(o, harmonic)
      || !hpss_matrix_ok(o, percussive))
    return AUBIO_FAIL;
  if (o->sealed) return AUBIO_FAIL;

  /* analyze one hop per channel into the next ring slot */
  for (c = 0; c < o->channels; c++)
    hpss_analysis(o, &o->pv[c], in->data[c]);
  {
    uint_t slot = hpss_next_write_slot(o);
    hpss_store_grain(o, o->ring[slot]);
    /* recompute magnitudes over the slot just written, then slide windows */
    hpss_public_magnitude(o, o->ring[slot], o->mag);
    hpss_push_magnitudes(o, o->mag);
  }
  o->do_count++;

  if (o->lookahead > 0) {
    /* first time_radius blocks: fill the output timeline with silence */
    hpss_write_zeros(o, harmonic, percussive);
    o->lookahead--;
  } else {
    /* center m = block_index - time_radius, block_index = do_count - 1 */
    hpss_emit_center(o, o->do_count - 1 - o->time_radius);
    hpss_copy_outputs(o, harmonic, percussive);
  }
  return AUBIO_OK;
}

uint_t aubio_hpss_finish(aubio_hpss_t *o,
    fmat_t *harmonic, fmat_t *percussive)
{
  uint_t c, k;
  uint_t block_index;
  if (o == NULL || harmonic == NULL || percussive == NULL) return 2;
  if (!hpss_matrix_ok(o, harmonic) || !hpss_matrix_ok(o, percussive))
    return 2;

  if (!o->sealed) {
    smpl_t *zeros = o->h_est;
    /* seal first: analyze exactly three zero-input windows up front */
    for (k = 0; k < o->hop_s; k++) zeros[k] = 0.;
    o->sealed = 1;
    for (k = 0; k < 3; k++) {
      for (c = 0; c < o->channels; c++)
        hpss_analysis(o, &o->pv[c], zeros);
      hpss_store_grain(o, o->tail[k]);
    }
  }

  if (o->tail_delivered >= o->tail_total) return 0;

  block_index = o->do_count + o->tail_delivered;

  /* zero analysis frame "block_index" enters the windows once its turn comes */
  if (block_index >= o->do_count && block_index < o->do_count + 3) {
    uint_t z = block_index - o->do_count;
    uint_t slot = hpss_next_write_slot(o);
    for (c = 0; c < o->channels; c++) {
      for (k = 0; k < o->bins; k++) {
        o->ring[slot][c].norm[k] = o->tail[z][c].norm[k];
        o->ring[slot][c].phas[k] = o->tail[z][c].phas[k];
      }
    }
    hpss_public_magnitude(o, o->ring[slot], o->mag);
    hpss_push_magnitudes(o, o->mag);
  }

  {
    sint_t center = (sint_t)block_index - (sint_t)o->time_radius;
    if (center >= 0) {
      /* clip the window to frames that actually exist: no padding */
      uint_t target_oldest = (uint_t)center - o->time_radius;
      if ((sint_t)target_oldest < 0) target_oldest = 0;
      while (o->filled > 0 && o->oldest_idx < target_oldest)
        hpss_pop_oldest(o);
      hpss_emit_center(o, (uint_t)center);
      hpss_copy_outputs(o, harmonic, percussive);
    } else {
      /* short recording: leading look-ahead zeros on the finish timeline */
      hpss_write_zeros(o, harmonic, percussive);
    }
  }
  o->tail_delivered++;
  return 1;
}

void aubio_hpss_reset(aubio_hpss_t *o)
{
  uint_t c, s, k;
  if (o == NULL) return;
  for (c = 0; c < o->channels; c++) {
    fvec_zeros(o->pv[c].data);
    fvec_zeros(o->pv[c].dataold);
    fvec_zeros(o->pv[c].synth_h);
    fvec_zeros(o->pv[c].synth_p);
    fvec_zeros(o->pv[c].synthold_h);
    fvec_zeros(o->pv[c].synthold_p);
    fvec_zeros(o->pv[c].tmp);
    for (k = 0; k < o->bins; k++) {
      o->pv[c].grain->norm[k] = 0.;
      o->pv[c].grain->phas[k] = 0.;
      o->pv[c].grain_h.norm[k] = 0.;
      o->pv[c].grain_h.phas[k] = 0.;
      o->pv[c].grain_p.norm[k] = 0.;
      o->pv[c].grain_p.phas[k] = 0.;
    }
  }
  for (s = 0; s < o->ring_cap; s++) {
    for (c = 0; c < o->channels; c++) {
      for (k = 0; k < o->bins; k++) {
        o->ring[s][c].norm[k] = 0.;
        o->ring[s][c].phas[k] = 0.;
      }
    }
  }
  for (s = 0; s < 3; s++) {
    for (c = 0; c < o->channels; c++) {
      for (k = 0; k < o->bins; k++) {
        o->tail[s][c].norm[k] = 0.;
        o->tail[s][c].phas[k] = 0.;
      }
    }
  }
  for (k = 0; k < (uint_t)((size_t)o->ring_cap * o->bins); k++) {
    o->mag_ring[k] = 0.;
    o->id_ring[k] = 0;
  }
  for (k = 0; k < o->bins; k++) o->freq_id[k] = 0;
  for (k = 0; k < o->bins; k++) med_engine_reset(&o->time_med[k]);
  med_engine_reset(&o->freq_med);
  for (k = 0; k < o->bins; k++) {
    o->mag[k] = 0.;
    o->h_est[k] = 0.;
    o->p_est[k] = 0.;
    o->mask[k] = 0.;
  }
  fmat_zeros(o->out_harm);
  fmat_zeros(o->out_perc);
  o->head = 0;
  o->filled = 0;
  o->oldest_idx = 0;
  o->do_count = 0;
  o->lookahead = o->time_radius;
  o->sealed = 0;
  o->tail_delivered = 0;
  o->freq_compares = 0;
}

uint_t aubio_hpss_get_latency(const aubio_hpss_t *o)
{
  if (o == NULL) return 0;
  return (o->win_s - o->hop_s) + o->time_radius * o->hop_s;
}

/* ------------------------------------------------------------------ */
/* construction / destruction                                          */
/* ------------------------------------------------------------------ */

aubio_hpss_t * new_aubio_hpss(uint_t N, uint_t H, uint_t channels,
    uint_t time_radius, uint_t freq_radius)
{
  aubio_hpss_t *o;
  uint_t c, s, k;
  uint_t failed = 0;
  if (N < 64 || !is_power_of_two(N)) {
    AUBIO_ERR("hpss: N must be a power of two not smaller than 64,"
        " got %d\n", N);
    return NULL;
  }
  if (H != N / 4) {
    AUBIO_ERR("hpss: H must equal N/4 (%d), got %d\n", N / 4, H);
    return NULL;
  }
  if (channels != 1 && channels != 2) {
    AUBIO_ERR("hpss: channels must be 1 or 2, got %d\n", channels);
    return NULL;
  }
  if (time_radius < 1 || time_radius > 31
      || freq_radius < 1 || freq_radius > 31) {
    AUBIO_ERR("hpss: radii must lie in [1, 31], got time %d freq %d\n",
        time_radius, freq_radius);
    return NULL;
  }

  o = AUBIO_NEW(aubio_hpss_t);
  if (o == NULL) return NULL;
  o->win_s = N;
  o->hop_s = H;
  o->channels = channels;
  o->bins = N / 2 + 1;
  o->time_radius = time_radius;
  o->freq_radius = freq_radius;
  o->ring_cap = 2 * time_radius + 1;
  o->freq_cap = 2 * freq_radius + 1;
  o->end = N - H;
  o->start = N - 2 * H;
  o->scale = 2. / 3.; /* win_s == 4 * hop_s, same scaling as aubio_pvoc_t */
  o->tail_total = time_radius + 3;
  o->lookahead = time_radius;

  o->pv = AUBIO_ARRAY(hpss_pvoc_t, channels);
  o->window = new_aubio_window("hanningz", N);
  o->ring = AUBIO_ARRAY(cvec_t *, o->ring_cap);
  o->tail = AUBIO_ARRAY(cvec_t *, 3);
  o->mag_ring = AUBIO_ARRAY(smpl_t, (size_t)o->ring_cap * o->bins);
  o->id_ring = AUBIO_ARRAY(uint_t, (size_t)o->ring_cap * o->bins);
  o->time_med = AUBIO_ARRAY(med_engine_t, o->bins);
  o->time_nodes = AUBIO_ARRAY(med_node_t, (size_t)o->bins * o->ring_cap);
  o->freq_nodes = AUBIO_ARRAY(med_node_t, o->freq_cap);
  o->freq_id = AUBIO_ARRAY(uint_t, o->bins);
  o->mag = AUBIO_ARRAY(smpl_t, o->bins);
  o->h_est = AUBIO_ARRAY(smpl_t, o->bins);
  o->p_est = AUBIO_ARRAY(smpl_t, o->bins);
  o->mask = AUBIO_ARRAY(smpl_t, o->bins);
  o->out_harm = new_fmat(channels, H);
  o->out_perc = new_fmat(channels, H);

  for (c = 0; c < channels; c++) {
    hpss_pvoc_t *pv = &o->pv[c];
    pv->fft = new_aubio_fft(N);
    pv->data = new_fvec(N);
    pv->dataold = new_fvec(o->end);
    pv->synth_h = new_fvec(N);
    pv->synth_p = new_fvec(N);
    pv->synthold_h = new_fvec(o->end);
    pv->synthold_p = new_fvec(o->end);
    pv->tmp = new_fvec(N);
    pv->grain = new_cvec(N);
    pv->grain_h.norm = AUBIO_ARRAY(smpl_t, o->bins);
    pv->grain_h.phas = AUBIO_ARRAY(smpl_t, o->bins);
    pv->grain_h.length = o->bins;
    pv->grain_p.norm = AUBIO_ARRAY(smpl_t, o->bins);
    pv->grain_p.phas = AUBIO_ARRAY(smpl_t, o->bins);
    pv->grain_p.length = o->bins;
    failed |= (pv->fft == NULL || pv->data == NULL || pv->dataold == NULL
        || pv->synth_h == NULL || pv->synth_p == NULL
        || pv->synthold_h == NULL || pv->synthold_p == NULL
        || pv->tmp == NULL || pv->grain == NULL
        || pv->grain_h.norm == NULL || pv->grain_h.phas == NULL
        || pv->grain_p.norm == NULL || pv->grain_p.phas == NULL);
  }
  for (s = 0; s < o->ring_cap; s++) {
    o->ring[s] = AUBIO_ARRAY(cvec_t, channels);
    failed |= (o->ring[s] == NULL);
    if (o->ring[s] != NULL) {
      for (c = 0; c < channels; c++) {
        o->ring[s][c].norm = AUBIO_ARRAY(smpl_t, o->bins);
        o->ring[s][c].phas = AUBIO_ARRAY(smpl_t, o->bins);
        o->ring[s][c].length = o->bins;
        failed |= (o->ring[s][c].norm == NULL || o->ring[s][c].phas == NULL);
      }
    }
  }
  for (s = 0; s < 3; s++) {
    o->tail[s] = AUBIO_ARRAY(cvec_t, channels);
    failed |= (o->tail[s] == NULL);
    if (o->tail[s] != NULL) {
      for (c = 0; c < channels; c++) {
        o->tail[s][c].norm = AUBIO_ARRAY(smpl_t, o->bins);
        o->tail[s][c].phas = AUBIO_ARRAY(smpl_t, o->bins);
        o->tail[s][c].length = o->bins;
        failed |= (o->tail[s][c].norm == NULL || o->tail[s][c].phas == NULL);
      }
    }
  }
  for (k = 0; k < o->bins; k++) {
    med_engine_init(&o->time_med[k],
        &o->time_nodes[(size_t)k * o->ring_cap], o->ring_cap);
  }
  med_engine_init(&o->freq_med, o->freq_nodes, o->freq_cap);
  failed |= (o->window == NULL || o->out_harm == NULL || o->out_perc == NULL
      || o->mag_ring == NULL || o->id_ring == NULL || o->time_med == NULL
      || o->time_nodes == NULL || o->freq_nodes == NULL
      || o->mag == NULL || o->h_est == NULL || o->p_est == NULL
      || o->mask == NULL);

  if (failed) {
    AUBIO_ERR("%s", "hpss: failed to allocate object\n");
    del_aubio_hpss(o);
    return NULL;
  }
  aubio_hpss_reset(o);
  return o;
}

void del_aubio_hpss(aubio_hpss_t *o)
{
  uint_t c, s;
  if (o == NULL) return;
  if (o->pv != NULL) {
    for (c = 0; c < o->channels; c++) {
      hpss_pvoc_t *pv = &o->pv[c];
      if (pv->fft != NULL) del_aubio_fft(pv->fft);
      if (pv->data != NULL) del_fvec(pv->data);
      if (pv->dataold != NULL) del_fvec(pv->dataold);
      if (pv->synth_h != NULL) del_fvec(pv->synth_h);
      if (pv->synth_p != NULL) del_fvec(pv->synth_p);
      if (pv->synthold_h != NULL) del_fvec(pv->synthold_h);
      if (pv->synthold_p != NULL) del_fvec(pv->synthold_p);
      if (pv->tmp != NULL) del_fvec(pv->tmp);
      if (pv->grain != NULL) del_cvec(pv->grain);
      AUBIO_FREE(pv->grain_h.norm);
      AUBIO_FREE(pv->grain_h.phas);
      AUBIO_FREE(pv->grain_p.norm);
      AUBIO_FREE(pv->grain_p.phas);
    }
    AUBIO_FREE(o->pv);
  }
  if (o->ring != NULL) {
    for (s = 0; s < o->ring_cap; s++) {
      if (o->ring[s] != NULL) {
        for (c = 0; c < o->channels; c++) {
          AUBIO_FREE(o->ring[s][c].norm);
          AUBIO_FREE(o->ring[s][c].phas);
        }
        AUBIO_FREE(o->ring[s]);
      }
    }
    AUBIO_FREE(o->ring);
  }
  if (o->tail != NULL) {
    for (s = 0; s < 3; s++) {
      if (o->tail[s] != NULL) {
        for (c = 0; c < o->channels; c++) {
          AUBIO_FREE(o->tail[s][c].norm);
          AUBIO_FREE(o->tail[s][c].phas);
        }
        AUBIO_FREE(o->tail[s]);
      }
    }
    AUBIO_FREE(o->tail);
  }
  if (o->window != NULL) del_fvec(o->window);
  AUBIO_FREE(o->time_med);
  AUBIO_FREE(o->time_nodes);
  AUBIO_FREE(o->freq_nodes);
  AUBIO_FREE(o->freq_id);
  AUBIO_FREE(o->mag_ring);
  AUBIO_FREE(o->id_ring);
  AUBIO_FREE(o->mag);
  AUBIO_FREE(o->h_est);
  AUBIO_FREE(o->p_est);
  AUBIO_FREE(o->mask);
  if (o->out_harm != NULL) del_fmat(o->out_harm);
  if (o->out_perc != NULL) del_fmat(o->out_perc);
  AUBIO_FREE(o);
}

/* ------------------------------------------------------------------ */
/* test-only entry points, always compiled; prototypes in hpss.h are   */
/* guarded by AUBIO_HPSS_TEST_HOOKS                                    */
/* ------------------------------------------------------------------ */

void aubio_hpss_test_push_magnitude(aubio_hpss_t *o, const smpl_t *mag)
{
  uint_t k, c;
  uint_t slot = hpss_next_write_slot(o);
  for (k = 0; k < o->bins; k++) {
    o->ring[slot][0].norm[k] = mag[k];
    o->ring[slot][0].phas[k] = 0.;
  }
  for (c = 1; c < o->channels; c++) {
    for (k = 0; k < o->bins; k++) {
      o->ring[slot][c].norm[k] = 0.;
      o->ring[slot][c].phas[k] = 0.;
    }
  }
  /* the exact ring/median push used by the PCM path */
  hpss_push_magnitudes(o, mag);
}

smpl_t aubio_hpss_test_harm_median(const aubio_hpss_t *o, uint_t bin)
{
  return med_engine_median(&o->time_med[bin]);
}

void aubio_hpss_test_freq_medians(const aubio_hpss_t *o, const smpl_t *mag,
    smpl_t *out)
{
  /* the production frequency engine, run on a supplied frame */
  hpss_freq_medians((aubio_hpss_t *)o, mag, out);
}

void aubio_hpss_test_compute_mask(aubio_hpss_t *o, const smpl_t *h,
    const smpl_t *p, smpl_t *harm, smpl_t *perc)
{
  uint_t k;
  hpss_compute_mask(o, h, p);
  for (k = 0; k < o->bins; k++) {
    harm[k] = o->mask[k];
    perc[k] = 1.f - o->mask[k];
  }
}

void aubio_hpss_test_shrink(aubio_hpss_t *o)
{
  hpss_pop_oldest(o);
}

uint_t aubio_hpss_test_time_inserts(const aubio_hpss_t *o)
{
  uint_t k, total = 0;
  for (k = 0; k < o->bins; k++) total += o->time_med[k].inserts;
  return total;
}

uint_t aubio_hpss_test_time_removes(const aubio_hpss_t *o)
{
  uint_t k, total = 0;
  for (k = 0; k < o->bins; k++) total += o->time_med[k].removes;
  return total;
}

uint_t aubio_hpss_test_time_comparisons(const aubio_hpss_t *o)
{
  uint_t k, total = 0;
  for (k = 0; k < o->bins; k++) total += o->time_med[k].comparisons;
  return total;
}

uint_t aubio_hpss_test_freq_comparisons(const aubio_hpss_t *o)
{
  return o->freq_compares;
}

uint_t aubio_hpss_test_ring_filled(const aubio_hpss_t *o)
{
  return o->filled;
}

uint_t aubio_hpss_test_ring_capacity(const aubio_hpss_t *o)
{
  return o->ring_cap;
}
