/* libSoX effect: splice audio   Copyright (c) 2008-9 robs@users.sourceforge.net
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "sox_i.h"
#include "cutplan.h"
#include <string.h>
#include <inttypes.h>

#define CP_MAX_SEGS     64
#define CP_MAX_CHANNELS 8
#define CP_MAX_W        1024
#define CP_MAX_S        64
#define CP_IO_FRAMES    8192

/*============================================================================
 * Legacy splice effect (unchanged behaviour)
 *==========================================================================*/

static double difference(
    const sox_sample_t * a, const sox_sample_t * b, size_t length)
{
  double diff = 0;
  size_t i = 0;

  #define _ diff += sqr((double)a[i] - b[i]), ++i; /* Loop optimisation */
  do {_ _ _ _ _ _ _ _} while (i < length); /* N.B. length ≡ 0 (mod 8) */
  #undef _
  return diff;
}

/* Find where the two segments are most alike over the overlap period. */
static size_t best_overlap_position(sox_sample_t const * f1,
    sox_sample_t const * f2, uint64_t overlap, uint64_t search, size_t channels)
{
  size_t i, best_pos = 0;
  double diff, least_diff = difference(f2, f1, (size_t) (channels * overlap));

  for (i = 1; i < search; ++i) { /* linear search */
    diff = difference(f2 + channels * i, f1, (size_t) (channels * overlap));
    if (diff < least_diff)
      least_diff = diff, best_pos = i;
  }
  return best_pos;
}


typedef struct {
  enum {Cosine_2, Cosine_4, Triangular} fade_type;
  unsigned nsplices;     /* Number of splices requested */
  struct {
    char * str;          /* Command-line argument to parse for this splice */
    uint64_t overlap;    /* Number of samples to overlap */
    uint64_t search;     /* Number of samples to search */
    uint64_t start;      /* Start splicing when in_pos equals this */
  } * splices;

  uint64_t in_pos;       /* Number of samples read from the input stream */
  unsigned splices_pos;  /* Number of splices completed so far */
  size_t buffer_pos;     /* Number of samples through the current splice */
  size_t max_buffer_size;
  sox_sample_t * buffer;
  unsigned state;

  /*----- cut-plan mode (--cut-plan) -----*/
  int cut_mode;                 /* non-zero when running the new mode */
  char * cut_plan_path;
  uint64_t cp_W, cp_S;
  unsigned cp_m;                /* number of deletion segments */
  int cp_offsets[CP_MAX_SEGS]; /* solver output offsets */
  struct cp_seg * cp_segs;      /* deletion table with chosen offsets */
  struct cp_span * cp_spans;    /* render spans */
  unsigned cp_nspans;
  FILE * cp_tmp;                /* offline PCM storage */
  uint64_t cp_nframes;          /* N: input length in frames */
  size_t cp_channels;
  sox_sample_t * cp_lwin, * cp_rwin, * cp_iobuf;
  size_t cp_pcm_alloc;          /* total PCM samples currently allocated */
  uint64_t cp_recv;             /* samples received in flow() */
  int cp_render_started, cp_render_done, cp_render_failed;
  int cp_io_error;
  unsigned cp_span_i;
  uint64_t cp_span_off;
  int cp_xfade_loaded;
  size_t cp_xfade_pos;
} priv_t;

static void splice(sox_effect_t * effp, const sox_sample_t * in1, const
    sox_sample_t * in2, sox_sample_t * output, uint64_t overlap, size_t channels)
{
  priv_t * p = (priv_t *)effp->priv;
  size_t i, j, k = 0;

  if (p->fade_type == Cosine_4) {
    double fade_step = M_PI_2 / overlap;
    for (i = 0; i < overlap; ++i) {
      double fade_in  = sin(i * fade_step);
      double fade_out = cos(i * fade_step); /* constant RMS level (`power') */
      for (j = 0; j < channels; ++j, ++k) {
        double d = in1[k] * fade_out + in2[k] * fade_in;
        output[k] = SOX_ROUND_CLIP_COUNT(d, effp->clips); /* Might clip */
      }
    }
  }
  else if (p->fade_type == Cosine_2) {
    double fade_step = M_PI / overlap;
    for (i = 0; i < overlap; ++i) {
      double fade_in  = .5 - .5 * cos(i * fade_step);
      double fade_out = 1 - fade_in;    /* constant peak level (`gain') */
      for (j = 0; j < channels; ++j, ++k) {
        double d = in1[k] * fade_out + in2[k] * fade_in;
        output[k] = SOX_ROUND_CLIP_COUNT(d, effp->clips); /* Should not clip */
      }
    }
  }
  else /* Triangular */ {
    double fade_step = 1. / overlap;
    for (i = 0; i < overlap; ++i) {
      double fade_in  = fade_step * i;
      double fade_out = 1 - fade_in;    /* constant peak level (`gain') */
      for (j = 0; j < channels; ++j, ++k) {
        double d = in1[k] * fade_out + in2[k] * fade_in;
        output[k] = SOX_ROUND_CLIP_COUNT(d, effp->clips); /* Should not clip */
      }
    }
  }
}

static uint64_t do_splice(sox_effect_t * effp,
    sox_sample_t * f, uint64_t overlap, uint64_t search, size_t channels)
{
  uint64_t offset = search? best_overlap_position(
      f, f + overlap * channels, overlap, search, channels) : 0;
  splice(effp, f, f + (overlap + offset) * channels,
      f + (overlap + offset) * channels, overlap, channels);
  return overlap + offset;
}

static int parse(sox_effect_t * effp, char * * argv, sox_rate_t rate)
{
  priv_t * p = (priv_t *)effp->priv;
  char const * next;
  size_t i, buffer_size;
  uint64_t last_seen = 0;
  const uint64_t in_length = argv ? 0 :
    (effp->in_signal.length != SOX_UNKNOWN_LEN ?
     effp->in_signal.length / effp->in_signal.channels : SOX_UNKNOWN_LEN);

  p->max_buffer_size = 0;
  for (i = 0; i < p->nsplices; ++i) {
    if (argv) /* 1st parse only */
      p->splices[i].str = lsx_strdup(argv[i]);

    p->splices[i].overlap = rate * 0.01 + .5;
    p->splices[i].search = p->fade_type == Cosine_4? 0 : p->splices[i].overlap;

    next = lsx_parseposition(rate, p->splices[i].str,
             argv ? NULL : &p->splices[i].start, last_seen, in_length, '=');
    if (next == NULL) break;
    last_seen = p->splices[i].start;

    if (*next == ',') {
      next = lsx_parsesamples(rate, next + 1, &p->splices[i].overlap, 't');
      if (next == NULL) break;
      p->splices[i].overlap *= 2;
      if (*next == ',') {
        next = lsx_parsesamples(rate, next + 1, &p->splices[i].search, 't');
        if (next == NULL) break;
        p->splices[i].search *= 2;
      }
    }
    if (*next != '\0') break;
    p->splices[i].overlap = max(p->splices[i].overlap + 4, 16);
    p->splices[i].overlap &= ~7; /* Make divisible by 8 for loop optimisation */

    if (!argv) {
      if (i > 0 && p->splices[i].start <= p->splices[i-1].start) break;
      if (p->splices[i].start < p->splices[i].overlap) break;
      p->splices[i].start -= p->splices[i].overlap;
      buffer_size = 2 * p->splices[i].overlap + p->splices[i].search;
      p->max_buffer_size = max(p->max_buffer_size, buffer_size);
    }
  }
  if (i < p->nsplices)
    return lsx_usage(effp);
  return SOX_SUCCESS;
}

/*============================================================================
 * Cut-plan mode: joint window selection over a whole deletion table
 *==========================================================================*/

typedef unsigned __int128 cp_cost_t;
typedef signed   __int128 cp_i128_t;

typedef struct cp_seg {
  uint64_t a, b;             /* original-input half-open frame range [a,b) */
  int      d;                /* chosen offset in [-S,S] */
} cp_seg;

typedef struct cp_span {
  int      is_xfade;         /* 0: verbatim copy, 1: triangular cross-fade */
  uint64_t src;              /* first source frame (copy spans) */
  uint64_t len;              /* span length in frames */
  uint64_t lsrc, rsrc;       /* first frame of each window (xfade spans) */
} cp_span;

/* Parse one unsigned decimal token of exactly `digits` characters; every
 * check goes through 128-bit intermediates so nothing can overflow. */
static int cp_parse_u64(const char * s, size_t digits, uint64_t * out)
{
  cp_i128_t v = 0;
  size_t i;
  if (digits == 0)
    return 0;
  for (i = 0; i < digits; ++i) {
    char c = s[i];
    if (c < '0' || c > '9')
      return 0;
    v = v * 10 + (c - '0');
    if (v > (cp_i128_t)UINT64_MAX)
      return 0;
  }
  *out = (uint64_t)v;
  return 1;
}

static int cp_is_space(char c)
{
  return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

/* Read and validate the deletion plan.  Returns 0 on success. */
static int cp_read_plan(sox_effect_t * effp, priv_t * p, uint64_t nframes)
{
  FILE * f;
  char line[1024];
  unsigned lineno = 0, m = 0;
  uint64_t prev_a = 0, prev_b = 0;
  int have_prev = 0;
  cp_seg * segs;

  f = fopen(p->cut_plan_path, "r");
  if (f == NULL) {
    lsx_fail("cut-plan: cannot open `%s': %s", p->cut_plan_path, strerror(errno));
    return -1;
  }
  segs = lsx_calloc(CP_MAX_SEGS, sizeof(*segs));
  while (fgets(line, sizeof(line), f) != NULL) {
    const char * s = line;
    const char * n1s, * n2s, * tail;
    size_t n1len, n2len;
    uint64_t a, b;
    ++lineno;
    if (strlen(line) == sizeof(line) - 1 && line[sizeof(line) - 2] != '\n') {
      lsx_fail("cut-plan: line %u too long", lineno);
      goto fail;
    }
    while (cp_is_space(*s)) ++s;
    if (*s == '\0' || *s == '\n' || *s == '#')
      continue;                          /* blank line or whole-line comment */
    n1s = s;
    while (*s >= '0' && *s <= '9') ++s;
    n1len = (size_t)(s - n1s);
    if (n1len == 0 || !cp_is_space(*s)) {
      lsx_fail("cut-plan: malformed line %u", lineno);
      goto fail;
    }
    while (cp_is_space(*s)) ++s;
    n2s = s;
    while (*s >= '0' && *s <= '9') ++s;
    n2len = (size_t)(s - n2s);
    if (n2len == 0) {
      lsx_fail("cut-plan: malformed line %u", lineno);
      goto fail;
    }
    tail = s;
    while (cp_is_space(*tail)) ++tail;
    if (*tail != '\0' && *tail != '\n') {   /* rejects extra fields */
      lsx_fail("cut-plan: unexpected extra field on line %u", lineno);
      goto fail;
    }
    if (!cp_parse_u64(n1s, n1len, &a) || !cp_parse_u64(n2s, n2len, &b)) {
      lsx_fail("cut-plan: number out of range on line %u", lineno);
      goto fail;
    }
    if (a >= b) {
      lsx_fail("cut-plan: line %u: require a < b", lineno);
      goto fail;
    }
    if (have_prev && a <= prev_a) {
      lsx_fail("cut-plan: line %u: starts not strictly increasing", lineno);
      goto fail;
    }
    if (b > nframes) {
      lsx_fail("cut-plan: line %u: segment outside input", lineno);
      goto fail;
    }
    if (have_prev && prev_b > a) {
      lsx_fail("cut-plan: line %u: segments overlap", lineno);
      goto fail;
    }
    if (m == CP_MAX_SEGS) {
      lsx_fail("cut-plan: more than %d deletion segments", CP_MAX_SEGS);
      goto fail;
    }
    segs[m].a = a;
    segs[m].b = b;
    ++m;
    prev_a = a;
    prev_b = b;
    have_prev = 1;
  }
  if (ferror(f)) {
    lsx_fail("cut-plan: error reading `%s': %s", p->cut_plan_path, strerror(errno));
    goto fail;
  }
  fclose(f);
  if (m == 0) {
    lsx_fail("cut-plan: plan contains no deletion segments");
    free(segs);
    return -1;
  }
  p->cp_segs = segs;
  p->cp_m = m;
  return 0;
fail:
  fclose(f);
  free(segs);
  return -1;
}

/* Structural feasibility (independent of audio): build the legal offset set
 * per segment and check that an admissible sequence exists.  All coordinate
 * arithmetic uses signed 128-bit values. */
int lsx_cutplan_feasible(uint64_t nframes,
    const uint64_t * a, const uint64_t * b, unsigned m,
    unsigned W, unsigned S)
{
  unsigned i;
  int k;
  const int64_t S64 = (int64_t)S, W64 = (int64_t)W;
  const cp_i128_t N = (cp_i128_t)nframes;
  unsigned char * reachable_prev = lsx_calloc((size_t)(2 * S64 + 1), 1);
  unsigned char * reachable_cur  = lsx_calloc((size_t)(2 * S64 + 1), 1);
  int any = 0;

  for (i = 0; i < m; ++i) {
    cp_i128_t ai = (cp_i128_t)a[i];
    cp_i128_t bi = (cp_i128_t)b[i];
    cp_i128_t lo = W64 - ai;             /* a + d - W >= 0 */
    cp_i128_t hi = N - W64 - bi;         /* b + d + W <= N */
    int row_any = 0;
    if (lo < -S64) lo = -S64;
    if (hi >  S64) hi =  S64;
    for (k = 0; k <= 2 * S64; ++k) {
      cp_i128_t d = k - S64;
      int ok = d >= lo && d <= hi;
      if (ok && i > 0) {
        int kp;
        cp_i128_t prev_b = (cp_i128_t)b[i - 1];
        for (kp = 0, ok = 0; kp <= 2 * S64; ++kp) {
          /* b_(i-1) + d_(i-1) + W <= a_i + d_i - W */
          if (reachable_prev[kp] &&
              prev_b + (cp_i128_t)(kp - S64) + W64 <= ai + d - W64) {
            ok = 1;
            break;
          }
        }
      }
      reachable_cur[k] = (unsigned char)ok;
      row_any |= ok;
    }
    any = row_any;
    memcpy(reachable_prev, reachable_cur, (size_t)(2 * S64 + 1));
    memset(reachable_cur, 0, (size_t)(2 * S64 + 1));
  }
  free(reachable_prev);
  free(reachable_cur);
  return any;
}

/* Exact integer SSE between two equal-length interleaved windows. */
static cp_cost_t cp_window_cost(const sox_sample_t * l,
    const sox_sample_t * r, size_t total)
{
  cp_cost_t sum = 0;
  size_t i;
  for (i = 0; i < total; ++i) {
    int64_t diff = (int64_t)l[i] - (int64_t)r[i];   /* exact, range +/- 2^32 */
    cp_cost_t sq = (cp_cost_t)(diff >= 0 ? diff : -diff);
    sum += sq * sq;
  }
  return sum;
}

static int cp_read_frames(priv_t * p, uint64_t frame, size_t frames,
    sox_sample_t * dst)
{
  size_t total = frames * p->cp_channels;
  cp_i128_t bytepos = (cp_i128_t)frame * (cp_i128_t)p->cp_channels *
      (cp_i128_t)sizeof(sox_sample_t);
  if (bytepos > (cp_i128_t)INT64_MAX)
    return -1;
  if (fseeko(p->cp_tmp, (off_t)bytepos, SEEK_SET) != 0)
    return -1;
  if (fread(dst, sizeof(sox_sample_t), total, p->cp_tmp) != total)
    return -1;
  return 0;
}

/* Joint dynamic program.  Space O(m*(2S+1)); tie-break is the lexicographic
 * order of the full signed offset vector, encoded via dense lex ranks. */
int lsx_cutplan_solve(lsx_cutplan_reader_t reader, void * ctx,
    unsigned channels, uint64_t nframes,
    const uint64_t * a, const uint64_t * b, unsigned m,
    unsigned W, unsigned S, sox_sample_t * scratch,
    int * offsets, unsigned __int128 * total_cost)
{
  const int K = 2 * (int)S + 1;
  const size_t ch = (size_t)channels;
  sox_sample_t * lwin = scratch;
  sox_sample_t * rwin = scratch + (size_t)W * ch;
  unsigned i;
  int k;
  cp_cost_t * dp_prev = lsx_calloc((size_t)K, sizeof(*dp_prev));
  cp_cost_t * dp_cur  = lsx_calloc((size_t)K, sizeof(*dp_cur));
  int * rank_prev = lsx_calloc((size_t)K, sizeof(*rank_prev));
  int * rank_cur  = lsx_calloc((size_t)K, sizeof(*rank_cur));
  /* back table: one row per segment */
  int * back = lsx_calloc((size_t)m * (size_t)K, sizeof(*back));
  unsigned char * feas = lsx_calloc((size_t)K, 1);
  int rc = -1;

  for (i = 0; i < m; ++i) {
    cp_i128_t ai = (cp_i128_t)a[i];
    cp_i128_t bi = (cp_i128_t)b[i];
    cp_i128_t lo = (cp_i128_t)W - ai,
              hi = (cp_i128_t)nframes - (cp_i128_t)W - bi;
    if (lo < -(cp_i128_t)S) lo = -(cp_i128_t)S;
    if (hi >  (cp_i128_t)S) hi =  (cp_i128_t)S;
    for (k = 0; k < K; ++k) {
      cp_i128_t d = k - (int)S;
      dp_cur[k] = 0;
      rank_cur[k] = -1;
      back[(size_t)i * K + k] = -1;
      feas[k] = (d >= lo && d <= hi);
      if (!feas[k])
        continue;
      if (reader(ctx, (uint64_t)(ai + d - (cp_i128_t)W), (size_t)W, lwin) ||
          reader(ctx, (uint64_t)(bi + d),               (size_t)W, rwin))
        goto done;
      {
        cp_cost_t cost = cp_window_cost(lwin, rwin, (size_t)W * ch);
        if (i == 0) {
          dp_cur[k] = cost;
        } else {
          int kp, best = -1;
          cp_cost_t best_cost = 0;
          int best_rank = 0;
          for (kp = 0; kp < K; ++kp) {
            cp_i128_t dpv = kp - (int)S;
            if (rank_prev[kp] < 0)
              continue;
            if ((cp_i128_t)b[i - 1] + dpv + (cp_i128_t)W >
                ai + d - (cp_i128_t)W)
              continue;
            {
              cp_cost_t cand = dp_prev[kp] + cost;
              if (best < 0 || cand < best_cost ||
                  (cand == best_cost && rank_prev[kp] < best_rank)) {
                best = kp;
                best_cost = cand;
                best_rank = rank_prev[kp];
              }
            }
          }
          if (best < 0) {
            feas[k] = 0;
            continue;
          }
          dp_cur[k] = best_cost;
          back[(size_t)i * K + k] = best;
        }
      }
    }
    /* Assign dense lex ranks for this stage's surviving vectors. */
    {
      int alive = 0, r;
      for (k = 0; k < K; ++k)
        alive += feas[k] != 0;
      if (alive == 0)
        goto done;
      for (r = 0; r < alive; ++r) {
        int pick = -1, k2;
        for (k2 = 0; k2 < K; ++k2) {
          int key_rank, key_tail, pick_rank;
          if (!feas[k2])
            continue;
          key_rank = i == 0 ? 0 : rank_prev[back[(size_t)i * K + k2]];
          key_tail = k2;
          if (pick < 0) { pick = k2; continue; }
          pick_rank = i == 0 ? 0 :
              rank_prev[back[(size_t)i * K + pick]];
          if (key_rank < pick_rank ||
              (key_rank == pick_rank && key_tail < pick))
            pick = k2;
        }
        rank_cur[pick] = r;
        feas[pick] = 0;   /* consume */
      }
    }
    memcpy(dp_prev, dp_cur, (size_t)K * sizeof(*dp_prev));
    memcpy(rank_prev, rank_cur, (size_t)K * sizeof(*rank_prev));
    memset(rank_cur, 0, (size_t)K * sizeof(*rank_cur));
  }
  /* Choose the final state: minimum cost, then smallest lex rank. */
  {
    int best = -1, k2;
    cp_cost_t best_cost = 0;
    int best_rank = 0;
    for (k2 = 0; k2 < K; ++k2) {
      if (rank_prev[k2] < 0)
        continue;
      if (best < 0 || dp_prev[k2] < best_cost ||
          (dp_prev[k2] == best_cost && rank_prev[k2] < best_rank)) {
        best = k2;
        best_cost = dp_prev[k2];
        best_rank = rank_prev[k2];
      }
    }
    if (best < 0)
      goto done;
    if (total_cost != NULL)
      *total_cost = best_cost;
    for (i = m - 1;;) {
      offsets[i] = best - (int)S;
      if (i == 0)
        break;
      best = back[(size_t)i * K + best];
      --i;
    }
  }
  rc = 0;
done:
  free(dp_prev); free(dp_cur); free(rank_prev); free(rank_cur);
  free(back); free(feas);
  return rc;
}

static int cp_tmp_reader(void * ctx, uint64_t frame, size_t frames,
    sox_sample_t * dst)
{
  priv_t * p = (priv_t *)ctx;
  if (cp_read_frames(p, frame, frames, dst) != 0) {
    p->cp_io_error = 1;
    return -1;
  }
  return 0;
}

/* Build the linear render span list from the chosen offsets. */
static int cp_build_spans(priv_t * p)
{
  unsigned i;
  uint64_t cur = 0;
  unsigned si = 0;
  cp_span * spans = lsx_calloc((size_t)(2 * CP_MAX_SEGS + 1), sizeof(*spans));

  for (i = 0; i < p->cp_m; ++i) {
    cp_i128_t d = p->cp_segs[i].d;
    uint64_t left0  = (uint64_t)((cp_i128_t)p->cp_segs[i].a + d - (cp_i128_t)p->cp_W);
    uint64_t right0 = (uint64_t)((cp_i128_t)p->cp_segs[i].b + d);
    if (left0 > cur)
      spans[si++] = (cp_span){0, cur, left0 - cur, 0, 0};
    spans[si++] = (cp_span){1, 0, p->cp_W, left0, right0};
    cur = (uint64_t)((cp_i128_t)p->cp_segs[i].b + d + (cp_i128_t)p->cp_W);
  }
  if (cur < p->cp_nframes)
    spans[si++] = (cp_span){0, cur, p->cp_nframes - cur, 0, 0};
  p->cp_spans = spans;
  p->cp_nspans = si;
  p->cp_span_i = 0;
  p->cp_span_off = 0;
  p->cp_xfade_loaded = 0;
  p->cp_xfade_pos = 0;
  return 0;
}

/* Triangular cross-fade with SoX rounding/saturation conventions. */
static void cp_render_xfade(sox_effect_t * effp, priv_t * p)
{
  size_t ch = p->cp_channels, W = (size_t)p->cp_W, j, c;
  double fade_step = 1.0 / (double)W;
  size_t k = 0;
  for (j = 0; j < W; ++j) {
    double fade_in = fade_step * (double)j;
    double fade_out = 1.0 - fade_in;
    for (c = 0; c < ch; ++c, ++k) {
      double v = (double)p->cp_lwin[k] * fade_out +
                 (double)p->cp_rwin[k] * fade_in;
      p->cp_iobuf[k] = SOX_ROUND_CLIP_COUNT(v, effp->clips);
    }
  }
}

static int cp_start_render(sox_effect_t * effp, priv_t * p)
{
  cp_i128_t total_del = 0, out_frames;
  unsigned i;
  size_t ch;

  if (p->cp_recv != (uint64_t)effp->in_signal.length) {
    lsx_fail("cut-plan: short read: got %" PRIu64 " of %" PRIu64 " samples",
        p->cp_recv, (uint64_t)effp->in_signal.length);
    return -1;
  }
  fflush(p->cp_tmp);
  {
    uint64_t aa[CP_MAX_SEGS], bb[CP_MAX_SEGS];
    unsigned i2;
    for (i2 = 0; i2 < p->cp_m; ++i2) {
      aa[i2] = p->cp_segs[i2].a;
      bb[i2] = p->cp_segs[i2].b;
    }
    if (lsx_cutplan_solve(cp_tmp_reader, p, (unsigned)p->cp_channels,
          p->cp_nframes, aa, bb, p->cp_m, (unsigned)p->cp_W,
          (unsigned)p->cp_S, p->cp_lwin, p->cp_offsets, NULL) != 0) {
      if (p->cp_io_error)
        lsx_fail("cut-plan: temporary file read failure: %s",
            strerror(errno));
      else
        lsx_fail("cut-plan: no feasible joint offset sequence");
      return -1;
    }
    for (i2 = 0; i2 < p->cp_m; ++i2)
      p->cp_segs[i2].d = p->cp_offsets[i2];
  }
  cp_build_spans(p);
  for (i = 0; i < p->cp_m; ++i)
    total_del += (cp_i128_t)p->cp_segs[i].b - (cp_i128_t)p->cp_segs[i].a;
  out_frames = (cp_i128_t)p->cp_nframes - total_del -
      (cp_i128_t)p->cp_m * (cp_i128_t)p->cp_W;
  if (out_frames < 0) {
    lsx_fail("cut-plan: internal output length error");
    return -1;
  }
  ch = p->cp_channels;
  {
    cp_i128_t out_samples = out_frames * (cp_i128_t)ch;
    if (out_samples > (cp_i128_t)UINT64_MAX) {
      lsx_fail("cut-plan: output length overflow");
      return -1;
    }
    effp->out_signal.length = (uint64_t)out_samples;
  }
  p->cp_render_started = 1;
  return 0;
}

static int cp_flow(sox_effect_t * effp, const sox_sample_t * ibuf,
    sox_sample_t * obuf, size_t * isamp, size_t * osamp)
{
  priv_t * p = (priv_t *)effp->priv;
  (void)obuf;
  if (*isamp) {
    cp_i128_t sum = (cp_i128_t)p->cp_recv + (cp_i128_t)*isamp;
    if (sum > (cp_i128_t)UINT64_MAX) {
      lsx_fail("cut-plan: input length overflow");
      return SOX_EOF;
    }
    if (fwrite(ibuf, sizeof(*ibuf), *isamp, p->cp_tmp) != *isamp) {
      lsx_fail("cut-plan: error writing temporary file: %s", strerror(errno));
      return SOX_EOF;
    }
    p->cp_recv += *isamp;
  }
  *osamp = 0;   /* offline effect: nothing is emitted until drain() */
  return SOX_SUCCESS;
}

static int cp_drain(sox_effect_t * effp, sox_sample_t * obuf, size_t * osamp)
{
  priv_t * p = (priv_t *)effp->priv;
  size_t ch = p->cp_channels;
  size_t cap_frames = *osamp / ch;
  size_t produced = 0;

  if (p->cp_render_done || p->cp_render_failed) {
    *osamp = 0;
    return SOX_EOF;
  }
  if (!p->cp_render_started && cp_start_render(effp, p) != 0) {
    *osamp = 0;
    p->cp_render_failed = 1;
    return SOX_EOF;
  }

  while (produced < cap_frames) {
    cp_span * span;
    size_t take, n_samples;
    if (p->cp_span_i >= p->cp_nspans) {
      p->cp_render_done = 1;
      break;
    }
    span = &p->cp_spans[p->cp_span_i];
    if (p->cp_span_off >= span->len) {
      ++p->cp_span_i;
      p->cp_span_off = 0;
      p->cp_xfade_loaded = 0;
      p->cp_xfade_pos = 0;
      continue;
    }
    take = (size_t)min((uint64_t)(cap_frames - produced),
        span->len - p->cp_span_off);

    if (!span->is_xfade) {
      size_t io_take = min(take, (size_t)CP_IO_FRAMES);
      if (cp_read_frames(p, span->src + p->cp_span_off, io_take,
          p->cp_iobuf) != 0) {
        lsx_fail("cut-plan: temporary file read failure: %s", strerror(errno));
        *osamp = produced * ch;
        p->cp_render_failed = 1;
        return SOX_EOF;
      }
      memcpy(obuf + produced * ch, p->cp_iobuf,
          io_take * ch * sizeof(sox_sample_t));
      p->cp_span_off += io_take;
      produced += io_take;
      continue;
    }

    if (!p->cp_xfade_loaded) {
      if (cp_read_frames(p, span->lsrc, (size_t)p->cp_W, p->cp_lwin) ||
          cp_read_frames(p, span->rsrc, (size_t)p->cp_W, p->cp_rwin)) {
        lsx_fail("cut-plan: temporary file read failure: %s", strerror(errno));
        *osamp = produced * ch;
        p->cp_render_failed = 1;
        return SOX_EOF;
      }
      cp_render_xfade(effp, p);
      p->cp_xfade_loaded = 1;
      p->cp_xfade_pos = 0;
    }
    n_samples = take * ch;
    memcpy(obuf + produced * ch, p->cp_iobuf + p->cp_xfade_pos,
        n_samples * sizeof(sox_sample_t));
    p->cp_xfade_pos += n_samples;
    p->cp_span_off += take;
    produced += take;
  }

  *osamp = produced * ch;
  if (p->cp_render_done)
    return SOX_EOF;
  return SOX_SUCCESS;
}

static int cp_create(sox_effect_t * effp, int argc, char * * argv)
{
  priv_t * p = (priv_t *)effp->priv;
  int have_plan = 0, have_W = 0, have_S = 0;
  int i;
  p->cut_mode = 1;
  for (i = 0; i < argc; ++i) {
    if (!strcmp(argv[i], "--cut-plan")) {
      if (++i >= argc || have_plan)
        return lsx_usage(effp);
      p->cut_plan_path = lsx_strdup(argv[i]);
      have_plan = 1;
    }
    else if (!strcmp(argv[i], "--overlap")) {
      uint64_t v;
      if (++i >= argc || have_W)
        return lsx_usage(effp);
      if (!cp_parse_u64(argv[i], strlen(argv[i]), &v) ||
          v < 1 || v > CP_MAX_W) {
        lsx_fail("cut-plan: --overlap must be an integer in [1,%d]", CP_MAX_W);
        return lsx_usage(effp);
      }
      p->cp_W = v;
      have_W = 1;
    }
    else if (!strcmp(argv[i], "--search")) {
      uint64_t v;
      if (++i >= argc || have_S)
        return lsx_usage(effp);
      if (!cp_parse_u64(argv[i], strlen(argv[i]), &v) || v > CP_MAX_S) {
        lsx_fail("cut-plan: --search must be an integer in [0,%d]", CP_MAX_S);
        return lsx_usage(effp);
      }
      p->cp_S = v;
      have_S = 1;
    }
    else
      return lsx_usage(effp);
  }
  if (!have_plan || !have_W || !have_S)
    return lsx_usage(effp);
  return SOX_SUCCESS;
}

static int cp_start(sox_effect_t * effp)
{
  priv_t * p = (priv_t *)effp->priv;
  size_t ch;
  cp_i128_t bound;

  if (effp->in_signal.length == SOX_UNKNOWN_LEN ||
      effp->in_signal.length == 0) {
    lsx_fail("cut-plan: input must have a known finite length");
    return SOX_EOF;
  }
  ch = (size_t)effp->in_signal.channels;
  if (ch < 1 || ch > CP_MAX_CHANNELS) {
    lsx_fail("cut-plan: channel count must be in [1,%d]", CP_MAX_CHANNELS);
    return SOX_EOF;
  }
  if (effp->in_signal.length % ch != 0) {
    lsx_fail("cut-plan: input length not divisible by channel count");
    return SOX_EOF;
  }
  p->cp_channels = ch;
  p->cp_nframes = effp->in_signal.length / ch;

  if (cp_read_plan(effp, p, p->cp_nframes) != 0)
    return SOX_EOF;
  {
    uint64_t aa[CP_MAX_SEGS], bb[CP_MAX_SEGS];
    unsigned i2;
    for (i2 = 0; i2 < p->cp_m; ++i2) {
      aa[i2] = p->cp_segs[i2].a;
      bb[i2] = p->cp_segs[i2].b;
    }
    if (!lsx_cutplan_feasible(p->cp_nframes, aa, bb, p->cp_m,
          (unsigned)p->cp_W, (unsigned)p->cp_S)) {
      lsx_fail("cut-plan: no feasible joint offset sequence");
      return SOX_EOF;
    }
  }

  p->cp_tmp = lsx_tmpfile();
  if (p->cp_tmp == NULL) {
    lsx_fail("cut-plan: can't create temporary file: %s", strerror(errno));
    return SOX_EOF;
  }
  /* PCM buffers: two contiguous W-frame windows plus an 8192-frame I/O
   * buffer; rwin aliases the second half of lwin (the solver's scratch
   * convention).  Total PCM storage is fixed-size and therefore never grows
   * with the input length: channels*(2W + 8192) <= channels*(4W+2S+8192). */
  p->cp_lwin  = lsx_malloc(2 * (size_t)p->cp_W * ch * sizeof(*p->cp_lwin));
  p->cp_rwin  = p->cp_lwin + (size_t)p->cp_W * ch;
  p->cp_iobuf = lsx_malloc((size_t)CP_IO_FRAMES * ch * sizeof(*p->cp_iobuf));
  p->cp_pcm_alloc = (2 * (size_t)p->cp_W + (size_t)CP_IO_FRAMES) * ch;
  bound = (cp_i128_t)ch * (4 * (cp_i128_t)p->cp_W +
      2 * (cp_i128_t)p->cp_S + CP_IO_FRAMES);
  if ((cp_i128_t)p->cp_pcm_alloc > bound) {
    lsx_fail("cut-plan: internal buffer budget error");
    fclose(p->cp_tmp);
    p->cp_tmp = NULL;
    return SOX_EOF;
  }
  effp->out_signal.length = SOX_UNKNOWN_LEN; /* set once rendering starts */
  return SOX_SUCCESS;
}

static int cp_stop(sox_effect_t * effp)
{
  priv_t * p = (priv_t *)effp->priv;
  if (p->cp_tmp != NULL) {
    const char * obs = getenv("SOX_CUT_PLAN_OBS");
    if (obs != NULL && p->cp_render_done) {
      FILE * o = fopen(obs, "w");
      if (o != NULL) {
        off_t bytes = ftello(p->cp_tmp);
        fprintf(o, "n=%" PRIu64 " m=%u W=%" PRIu64 " S=%" PRIu64
            " pcmsamples=%zu tmpbytes=%lld out=%" PRIu64 "\n",
            p->cp_nframes, p->cp_m, p->cp_W, p->cp_S, p->cp_pcm_alloc,
            (long long)bytes, effp->out_signal.length);
        fclose(o);
      }
    }
    fclose(p->cp_tmp); /* auto-deleted by lsx_tmpfile */
    p->cp_tmp = NULL;
  }
  free(p->cp_lwin); p->cp_lwin = NULL; p->cp_rwin = NULL;
  free(p->cp_iobuf); p->cp_iobuf = NULL;
  free(p->cp_spans); p->cp_spans = NULL;
  return SOX_SUCCESS;
}

static int cp_kill(sox_effect_t * effp)
{
  priv_t * p = (priv_t *)effp->priv;
  free(p->cut_plan_path);
  free(p->cp_segs);
  return SOX_SUCCESS;
}

/*============================================================================
 * Shared effect entry points (legacy behaviour is byte-for-byte unchanged)
 *==========================================================================*/

static int create(sox_effect_t * effp, int argc, char * * argv)
{
  priv_t * p = (priv_t *)effp->priv;
  --argc, ++argv;
  if (argc >= 1 && (!strcmp(argv[0], "--cut-plan") ||
                    !strcmp(argv[0], "--overlap") ||
                    !strcmp(argv[0], "--search")))
    return cp_create(effp, argc, argv);
  if (argc) {
    if      (!strcmp(*argv, "-t")) p->fade_type = Triangular, --argc, ++argv;
    else if (!strcmp(*argv, "-q")) p->fade_type = Cosine_4  , --argc, ++argv;
    else if (!strcmp(*argv, "-h")) p->fade_type = Cosine_2  , --argc, ++argv;
  }
  p->nsplices = argc;
  p->splices = lsx_calloc(p->nsplices, sizeof(*p->splices));
  return parse(effp, argv, 1e5); /* No rate yet; parse with dummy */
}

static int start(sox_effect_t * effp)
{
  priv_t * p = (priv_t *)effp->priv;
  unsigned i;

  if (p->cut_mode)
    return cp_start(effp);

  parse(effp, 0, effp->in_signal.rate); /* Re-parse now rate is known */
  p->buffer = lsx_calloc(p->max_buffer_size * effp->in_signal.channels, sizeof(*p->buffer));
  p->in_pos = p->buffer_pos = p->splices_pos = 0;
  p->state = p->splices_pos != p->nsplices && p->in_pos == p->splices[p->splices_pos].start;
  effp->out_signal.length = SOX_UNKNOWN_LEN; /* depends on input data */
  for (i = 0; i < p->nsplices; ++i)
    if (p->splices[i].overlap) {
      if (p->fade_type == Cosine_4 && effp->in_signal.mult)
        *effp->in_signal.mult *= pow(.5, .5);
      return SOX_SUCCESS;
    }
  return SOX_EFF_NULL;
}

static int flow(sox_effect_t * effp, const sox_sample_t * ibuf,
    sox_sample_t * obuf, size_t * isamp, size_t * osamp)
{
  priv_t * p = (priv_t *)effp->priv;
  size_t c, idone = 0, odone = 0;

  if (p->cut_mode)
    return cp_flow(effp, ibuf, obuf, isamp, osamp);

  *isamp /= effp->in_signal.channels;
  *osamp /= effp->in_signal.channels;

  while (sox_true) {
copying:
    if (p->state == 0) {
      for (; idone < *isamp && odone < *osamp; ++idone, ++odone, ++p->in_pos) {
        if (p->splices_pos != p->nsplices && p->in_pos == p->splices[p->splices_pos].start) {
          p->state = 1;
          goto buffering;
        }
        for (c = 0; c < effp->in_signal.channels; ++c)
          *obuf++ = *ibuf++;
      }
      break;
    }

buffering:
    if (p->state == 1) {
      size_t buffer_size = (2 * p->splices[p->splices_pos].overlap + p->splices[p->splices_pos].search) * effp->in_signal.channels;
      for (; idone < *isamp; ++idone, ++p->in_pos) {
        if (p->buffer_pos == buffer_size) {
          p->buffer_pos = do_splice(effp, p->buffer,
              p->splices[p->splices_pos].overlap,
              p->splices[p->splices_pos].search,
              (size_t)effp->in_signal.channels) * effp->in_signal.channels;
          p->state = 2;
          goto flushing;
          break;
        }
        for (c = 0; c < effp->in_signal.channels; ++c)
          p->buffer[p->buffer_pos++] = *ibuf++;
      }
      break;
    }

flushing:
    if (p->state == 2) {
      size_t buffer_size = (2 * p->splices[p->splices_pos].overlap + p->splices[p->splices_pos].search) * effp->in_signal.channels;
      for (; odone < *osamp; ++odone) {
        if (p->buffer_pos == buffer_size) {
          p->buffer_pos = 0;
          ++p->splices_pos;
          p->state = p->splices_pos != p->nsplices && p->in_pos == p->splices[p->splices_pos].start;
          goto copying;
        }
        for (c = 0; c < effp->in_signal.channels; ++c)
          *obuf++ = p->buffer[p->buffer_pos++];
      }
      break;
    }
  }

  *isamp = idone * effp->in_signal.channels;
  *osamp = odone * effp->in_signal.channels;
  return SOX_SUCCESS;
}

static int drain(sox_effect_t * effp, sox_sample_t * obuf, size_t * osamp)
{
  priv_t * p = (priv_t *)effp->priv;
  if (p->cut_mode)
    return cp_drain(effp, obuf, osamp);
  {
    size_t isamp = 0;
    return flow(effp, 0, obuf, &isamp, osamp);
  }
}

static int stop(sox_effect_t * effp)
{
  priv_t * p = (priv_t *)effp->priv;
  if (p->cut_mode)
    return cp_stop(effp);
  if (p->splices_pos != p->nsplices)
    lsx_warn("Input audio too short; splices not made: %u", p->nsplices - p->splices_pos);
  free(p->buffer);
  return SOX_SUCCESS;
}

static int lsx_kill(sox_effect_t * effp)
{
  priv_t * p = (priv_t *)effp->priv;
  unsigned i;
  if (p->cut_mode)
    return cp_kill(effp);
  for (i = 0; i < p->nsplices; ++i)
    free(p->splices[i].str);
  free(p->splices);
  return SOX_SUCCESS;
}

sox_effect_handler_t const * lsx_splice_effect_fn(void)
{
  static sox_effect_handler_t handler = {
    "splice", "[-h|-t|-q] {position[,excess[,leeway]]}\n"
    "  cut-plan mode: --cut-plan FILE --overlap W --search S"
    "\n  -h        Half sine fade (default); constant gain (for correlated audio)"
    "\n  -t        Triangular (linear) fade; constant gain (for correlated audio)"
    "\n  -q        Quarter sine fade; constant power (for correlated audio e.g. x-fade)"
    "\n  position  The length of part 1 (including the excess)"
    "\n  excess    At the end of part 1 & the start of part2 (default 0.005)"
    "\n  leeway    Before part2 (default 0.005; set to 0 for cross-fade)"
    "\n  --cut-plan FILE  deletion table; each line: a b (half-open input frames)"
    "\n  --overlap W      cross-fade window in frames, 1..1024 (cut-plan mode)"
    "\n  --search S       joint offset search radius in frames, 0..64 (cut-plan mode)",
    SOX_EFF_MCHAN | SOX_EFF_LENGTH,
    create, start, flow, drain, stop, lsx_kill, sizeof(priv_t)
  };
  return &handler;
}
