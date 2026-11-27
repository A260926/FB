/* libSoX internal: exact joint planner for splice --cut-plan.
 *
 * This translation unit is deliberately free of libSoX dependencies: it is
 * compiled as part of the library and the regression tests include this same
 * source file so that they exercise the production planner directly.
 *
 * All comparisons are exact integer comparisons using __int128; no floating
 * point value ever takes part in choosing an offset.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "splice_cut_plan.h"

#define LSX_CUT_INF ((((unsigned __int128)-1) >> 2))

/* Saturating add so that absurd (test-supplied) costs cannot wrap. */
static unsigned __int128 lsx_cut_add(unsigned __int128 a,
    unsigned __int128 b)
{
  if (a >= LSX_CUT_INF || b >= LSX_CUT_INF || a > LSX_CUT_INF - b)
    return LSX_CUT_INF;
  return a + b;
}

/*
 * Individually valid offsets for one cut: the left window
 * [a+d-W, a+d) and the right window [b+d, b+d+W) must both lie inside
 * [0,frames).  Returns 0 when no offset in [-search,search] satisfies this.
 */
int lsx_cutplan_offset_range(const lsx_cut_plan_interval_t * cut,
    uint64_t frames, unsigned overlap, unsigned search, int * lo, int * hi)
{
  __int128 l = -(int)search, h = (int)search;
  __int128 w = overlap;

  if (!cut || cut->a >= cut->b || cut->b > frames || frames < overlap)
    return 0;

  /* Left window start a+d-W >= 0  =>  d >= W-a. */
  if (w - (__int128)cut->a > l)
    l = w - (__int128)cut->a;

  /* Right window end b+d+W <= frames  =>  d <= frames-W-b. */
  {
    __int128 lim = (__int128)frames - w - (__int128)cut->b;
    if (lim < h)
      h = lim;
  }

  if (l > h)
    return 0;
  *lo = (int)l;
  *hi = (int)h;
  return 1;
}

/* Pair constraint for adjacent cuts: d_{i-1} - d_i <= island - 2W.  The
 * island size is computed in 128 bits so that huge frame coordinates cannot
 * overflow a signed 64-bit value. */
static __int128 lsx_cut_pair_slack(const lsx_cut_plan_interval_t * prev,
    const lsx_cut_plan_interval_t * cur, unsigned overlap)
{
  return (__int128)(cur->a - prev->b) - 2 * (__int128)overlap;
}

/* Clamp a lower bound into [lo, hi+1] (hi+1 means "empty").  The values
 * involved are bounded by +/-S or by a 128-bit slack, so the comparison is
 * exact and cannot overflow. */
static int lsx_cut_clamp_lower(__int128 v, int lo, int hi)
{
  if (v <= (__int128)lo)
    return lo;
  if (v > (__int128)hi)
    return hi + 1;
  return (int)v;
}

int lsx_cutplan_feasible(const lsx_cut_plan_interval_t * cuts,
    unsigned ncuts, uint64_t frames, unsigned overlap, unsigned search)
{
  int lo[LSX_CUT_PLAN_MAX_CUTS], hi[LSX_CUT_PLAN_MAX_CUTS];
  unsigned char * reach, next[2 * LSX_CUT_PLAN_MAX_S + 1];
  unsigned char prefix[2 * LSX_CUT_PLAN_MAX_S + 2];
  size_t k = 2 * (size_t)search + 1;
  unsigned i;
  int d, any = 0;

  if (ncuts > LSX_CUT_PLAN_MAX_CUTS || overlap < 1 ||
      overlap > LSX_CUT_PLAN_MAX_W || search > LSX_CUT_PLAN_MAX_S)
    return 0;
  if (!ncuts)
    return 1;

  for (i = 0; i < ncuts; ++i)
    if (!lsx_cutplan_offset_range(&cuts[i], frames, overlap, search,
                                  &lo[i], &hi[i]))
      return 0;

  reach = (unsigned char *)calloc(k, 1);
  if (!reach)
    return 0;
  for (d = lo[0]; d <= hi[0]; ++d)
    reach[d + (int)search] = 1, any = 1;
  if (!any) {
    free(reach);
    return 0;
  }

  for (i = 1; i < ncuts; ++i) {
    __int128 slack = lsx_cut_pair_slack(&cuts[i - 1], &cuts[i], overlap);
    unsigned base = (unsigned)(lo[i - 1] + (int)search);
    int p;

    memset(next, 0, k);
    prefix[0] = 0;
    for (d = 0; d < (int)k; ++d) {
      int idx = d - (int)search;
      unsigned char ok = (unsigned char)(idx >= lo[i - 1] &&
          idx <= hi[i - 1] && reach[d]);
      prefix[d + 1] = (unsigned char)(prefix[d] | ok);
    }
    any = 0;
    for (d = lo[i]; d <= hi[i]; ++d) {
      __int128 lim = (__int128)d + slack;   /* prev <= lim */
      int up;
      if (lim < (__int128)(lo[i - 1] - 1))
        up = lo[i - 1] - 1;
      else if (lim >= (__int128)hi[i - 1])
        up = hi[i - 1];
      else
        up = (int)lim;
      p = up + (int)search;
      if (p >= (int)base && (prefix[p + 1] - prefix[base]) != 0) {
        next[d + (int)search] = 1;
        any = 1;
      }
    }
    memcpy(reach, next, k);
    if (!any) {
      free(reach);
      return 0;
    }
  }
  free(reach);
  return 1;
}

int lsx_cutplan_solve(const lsx_cut_plan_interval_t * cuts, unsigned ncuts,
    uint64_t frames, unsigned overlap, unsigned search,
    const unsigned __int128 * costs, int * offsets,
    unsigned __int128 * total_cost)
{
  int lo[LSX_CUT_PLAN_MAX_CUTS], hi[LSX_CUT_PLAN_MAX_CUTS];
  unsigned __int128 * suf;
  size_t k, cells;
  unsigned i;
  int d, K;

  if (ncuts > LSX_CUT_PLAN_MAX_CUTS || overlap < 1 ||
      overlap > LSX_CUT_PLAN_MAX_W || search > LSX_CUT_PLAN_MAX_S)
    return -1;
  if (ncuts && (!cuts || !costs || !offsets || !total_cost))
    return -1;
  if (!ncuts) {
    if (total_cost)
      *total_cost = 0;
    return 0;
  }

  for (i = 0; i < ncuts; ++i)
    if (!lsx_cutplan_offset_range(&cuts[i], frames, overlap, search,
                                  &lo[i], &hi[i]))
      return -1;

  K = (int)(2 * search + 1);
  k = (size_t)K;
  cells = (size_t)(ncuts + 1) * k;
  suf = (unsigned __int128 *)malloc(cells * sizeof(*suf));
  if (!suf)
    return -1;

  /* suf[i][d]: cheapest cost of cuts i..n-1 when cut i uses offset d
   * (the predecessor constraint only depends on d_i, so this is well
   * defined). */
  for (d = 0; d < K; ++d)
    suf[(size_t)ncuts * k + (size_t)d] = 0;

  for (i = ncuts; i-- > 0; ) {
    unsigned __int128 * row = suf + (size_t)i * k;
    unsigned __int128 * nxt = suf + (size_t)(i + 1) * k;
    __int128 slack = 0;
    if (i + 1 < ncuts)
      slack = lsx_cut_pair_slack(&cuts[i], &cuts[i + 1], overlap);

    for (d = 0; d < K; ++d) {
      int off = d - (int)search;
      unsigned __int128 c;
      if (off < lo[i] || off > hi[i]) {
        row[d] = LSX_CUT_INF;
        continue;
      }
      c = costs[(size_t)i * k + (size_t)d];
      if (c >= LSX_CUT_INF) {
        row[d] = LSX_CUT_INF;
        continue;
      }
      if (i + 1 == ncuts)
        row[d] = c;
      else {
        int lower = lsx_cut_clamp_lower((__int128)off - slack,
                                        lo[i + 1], hi[i + 1]);
        unsigned __int128 best = LSX_CUT_INF;
        int dn;
        for (dn = lower; dn <= hi[i + 1]; ++dn) {
          unsigned __int128 v = nxt[dn + (int)search];
          if (v < best)
            best = v;
        }
        row[d] = best >= LSX_CUT_INF? LSX_CUT_INF : lsx_cut_add(c, best);
      }
    }
  }

  {
    unsigned __int128 best = LSX_CUT_INF;
    for (d = lo[0]; d <= hi[0]; ++d) {
      unsigned __int128 v = suf[d + (int)search];
      if (v < best)
        best = v;
    }
    if (best >= LSX_CUT_INF) {
      free(suf);
      return -1;
    }
    *total_cost = best;
  }

  /* Backtrack, always taking the smallest signed offset that still achieves
   * the global optimum; that yields the lexicographic minimum vector. */
  {
    unsigned __int128 prefix_cost = 0;
    int prev = 0;
    for (i = 0; i < ncuts; ++i) {
      int lower = lo[i], chosen = 0, found = 0;
      unsigned __int128 * row = suf + (size_t)i * k;
      if (i > 0) {
        __int128 slack = lsx_cut_pair_slack(&cuts[i - 1], &cuts[i], overlap);
        lower = lsx_cut_clamp_lower((__int128)prev - slack, lo[i], hi[i]);
      }
      for (d = lower; d <= hi[i]; ++d) {
        unsigned __int128 v = row[d + (int)search];
        if (v < LSX_CUT_INF &&
            lsx_cut_add(prefix_cost, v) == *total_cost) {
          chosen = d;
          found = 1;
          break;
        }
      }
      if (!found) {
        free(suf);
        return -1;
      }
      offsets[i] = chosen;
      prefix_cost = lsx_cut_add(prefix_cost,
          costs[(size_t)i * k + (size_t)(chosen + (int)search)]);
      prev = chosen;
    }
  }

  free(suf);
  return 0;
}
