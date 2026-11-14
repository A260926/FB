/* Cut-plan regression tests for the splice effect.
 *
 * Three groups:
 *   planner   - joint offset solver vs. independent brute-force enumeration
 *   pcm       - sample-exact rendering through the real SoX effect/CLI
 *   lifecycle - flow/drain behaviour, buffering, cancellation, temp storage
 *
 * Only C and the public libSoX effect API are used.
 */

#include "check.h"
#include "cutplan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>

/*----------------------------- generic helpers -----------------------------*/

static int count_dir_entries(const char * path);
static int wait_child_ok(pid_t pid, const char * err_path);

#define MAX_M 64
#define MAX_CH 8
#define MAX_W 1024

static const char * tmp_dir(void)
{
  const char * t = getenv("TMPDIR");
  return (t && *t) ? t : "/tmp";
}

static void write_plan_file(const char * path,
    const uint64_t * a, const uint64_t * b, unsigned m, int comments)
{
  FILE * f = fopen(path, "w");
  unsigned i;
  if (f == NULL) { perror(path); exit(1); }
  if (comments)
    fprintf(f, "# cut plan written by target_cases\n\n");
  for (i = 0; i < m; ++i)
    fprintf(f, "%llu %llu\n", (unsigned long long)a[i],
        (unsigned long long)b[i]);
  fclose(f);
}

static sox_sample_t marker_val(uint64_t frame, unsigned ch)
{
  /* Distinct, bounded, deterministic marker carrying the original position. */
  return (sox_sample_t)((int64_t)((frame * 13u + ch * 7u + 3u) % 100000u)
      - 50000);
}

static void fill_markers(sox_sample_t * pcm, uint64_t frames, unsigned ch)
{
  uint64_t f;
  unsigned c;
  for (f = 0; f < frames; ++f)
    for (c = 0; c < ch; ++c)
      pcm[f * ch + c] = marker_val(f, c);
}

/*----------------------- brute-force independent oracle --------------------*/

typedef struct {
  const sox_sample_t * pcm;
  unsigned ch;
} mem_ctx_t;

static int mem_reader(void * vctx, uint64_t frame, size_t frames,
    sox_sample_t * dst)
{
  mem_ctx_t * ctx = (mem_ctx_t *)vctx;
  memcpy(dst, ctx->pcm + frame * ctx->ch,
      frames * ctx->ch * sizeof(sox_sample_t));
  return 0;
}

static unsigned __int128 exact_cost(const sox_sample_t * pcm, unsigned ch,
    uint64_t a, uint64_t b, int d, unsigned W)
{
  unsigned __int128 sum = 0;
  uint64_t l0 = a + d - W, r0 = b + d;
  unsigned j;
  for (j = 0; j < W * ch; ++j) {
    int64_t diff = (int64_t)pcm[l0 * ch + j] - (int64_t)pcm[r0 * ch + j];
    unsigned __int128 ad = diff < 0 ? (unsigned __int128)(-diff)
                                     : (unsigned __int128)diff;
    sum += ad * ad;
  }
  return sum;
}

typedef struct {
  int found;
  int vec[MAX_M];
  unsigned __int128 cost;
  unsigned ties;                /* number of vectors sharing the best cost */
} brute_result_t;

static const uint64_t * g_a, * g_b;
static unsigned g_m, g_W, g_S, g_ch;
static uint64_t g_N;
static const sox_sample_t * g_pcm;
static brute_result_t g_br;
static int g_cur[MAX_M];

static void brute_enum(unsigned i, unsigned __int128 acc)
{
  int d;
  if (i == g_m) {
    if (!g_br.found || acc < g_br.cost) {
      g_br.found = 1;
      g_br.cost = acc;
      g_br.ties = 1;
      memcpy(g_br.vec, g_cur, sizeof(g_cur));
    } else if (acc == g_br.cost) {
      int lex = 0, k;
      ++g_br.ties;
      for (k = 0; k < (int)g_m; ++k) {       /* lexicographic minimum */
        if (g_cur[k] < g_br.vec[k]) { lex = 1; break; }
        if (g_cur[k] > g_br.vec[k]) break;
      }
      if (lex)
        memcpy(g_br.vec, g_cur, sizeof(g_cur));
    }
    return;
  }
  for (d = -(int)g_S; d <= (int)g_S; ++d) {
    if (g_a[i] + d < g_W)                     /* left window inside input */
      continue;
    if (g_b[i] + d + g_W > g_N)               /* right window inside input */
      continue;
    if (i > 0 && g_b[i - 1] + g_cur[i - 1] + g_W >
        g_a[i] + d - g_W)
      continue;                               /* adjacent windows collide */
    g_cur[i] = d;
    brute_enum(i + 1, acc + exact_cost(g_pcm, g_ch, g_a[i], g_b[i], d, g_W));
  }
}

static brute_result_t brute_solve(const sox_sample_t * pcm, unsigned ch,
    uint64_t N, const uint64_t * a, const uint64_t * b, unsigned m,
    unsigned W, unsigned S)
{
  g_pcm = pcm; g_ch = ch; g_N = N; g_a = a; g_b = b;
  g_m = m; g_W = W; g_S = S;
  memset(&g_br, 0, sizeof(g_br));
  brute_enum(0, 0);
  return g_br;
}

/* Independent reference renderer using the documented span rules. */
static size_t reference_render(const sox_sample_t * in, unsigned ch,
    uint64_t N, const uint64_t * a, const uint64_t * b, unsigned m,
    const int * d, unsigned W, sox_sample_t * out, size_t out_cap)
{
  size_t op = 0;
  uint64_t cur = 0;
  unsigned i, j, c;
  size_t clips = 0;
  for (i = 0; i < m; ++i) {
    uint64_t l0 = a[i] + d[i] - W;
    uint64_t r0 = b[i] + d[i];
    uint64_t x;
    for (x = cur; x < l0; ++x)
      for (c = 0; c < ch; ++c)
        out[op++] = in[x * ch + c];
    for (j = 0; j < W; ++j) {
      double fi = (double)j / (double)W, fo = 1.0 - fi;
      for (c = 0; c < ch; ++c) {
        double v = (double)in[(l0 + j) * ch + c] * fo +
                   (double)in[(r0 + j) * ch + c] * fi;
        TASK_CHECK(op < out_cap);
        out[op++] = SOX_ROUND_CLIP_COUNT(v, clips);
      }
    }
    cur = b[i] + d[i] + W;
  }
  for (; cur < N; ++cur)
    for (c = 0; c < ch; ++c)
      out[op++] = in[cur * ch + c];
  return op;
}

static uint64_t expected_out_frames(uint64_t N,
    const uint64_t * a, const uint64_t * b, unsigned m, unsigned W)
{
  unsigned __int128 del = 0;
  unsigned i;
  for (i = 0; i < m; ++i)
    del += (unsigned __int128)(b[i] - a[i]);
  return (uint64_t)((unsigned __int128)N - del -
      (unsigned __int128)m * W);
}

/* Output frame index of a source frame that lies in a verbatim (retained)
 * region; returns -1 if the frame is deleted or inside a cross-fade. */
static long long retained_output_frame(uint64_t N,
    const uint64_t * a, const uint64_t * b, unsigned m, unsigned W,
    const int * d, uint64_t x)
{
  unsigned i;
  uint64_t cur = 0, out_frame = 0;
  for (i = 0; i < m; ++i) {
    uint64_t l0 = a[i] + d[i] - W;
    uint64_t r1 = b[i] + d[i] + W;
    if (x >= a[i] && x < b[i])
      return -1;                                  /* deleted */
    if (x >= l0 && x < r1)
      return -1;                                  /* inside a window */
    if (x < l0)
      return (long long)(out_frame + (x - cur)); /* in preceding keep */
    out_frame += (l0 - cur) + W;
    cur = r1;
  }
  if (x < cur || x >= N)
    return -1;
  return (long long)(out_frame + (x - cur));
}

/*================================ planner ==================================*/

static void test_planner(void)
{
  /* P1: individual optima (+1,-1) collide on a short retention island; the
   * joint optimum must be a different, feasible vector. */
  {
    const unsigned N = 30, ch = 1, W = 4, S = 1, m = 2;
    uint64_t a[2] = {10, 20}, b[2] = {12, 22};
    sox_sample_t * pcm = calloc(N * ch, sizeof(*pcm));
    int off[2], k, j;
    unsigned __int128 total;
    mem_ctx_t ctx = {pcm, ch};
    sox_sample_t scratch[2 * (size_t)MAX_W * MAX_CH];
    brute_result_t br;
    for (k = 0; k < (int)N; ++k)
      pcm[k] = (sox_sample_t)((k * 1103515245u + 12345u) % 2000001) - 1000000;
    /* Force seg0 d=+1 cost 0: right window [13,17) copied to left [7,11). */
    for (j = 0; j < (int)W; ++j) pcm[7 + j] = pcm[13 + j];
    /* Force seg1 d=-1 cost 0: right [21,25) copied to left [15,19). */
    for (j = 0; j < (int)W; ++j) pcm[15 + j] = pcm[21 + j];

    TASK_CHECK(lsx_cutplan_feasible(N, a, b, m, W, S) == 1);
    br = brute_solve(pcm, ch, N, a, b, m, W, S);
    TASK_CHECK(br.found == 1);
    TASK_CHECK(lsx_cutplan_solve(mem_reader, &ctx, ch, N, a, b, m, W, S,
        scratch, off, &total) == 0);
    TASK_CHECK(off[0] == br.vec[0] && off[1] == br.vec[1]);
    TASK_CHECK(total == br.cost);
    TASK_CHECK(!(off[0] == 1 && off[1] == -1));   /* greedy pair impossible */
    free(pcm);
  }

  /* P2: silent input, every vector ties -> most negative feasible vector. */
  {
    const unsigned N = 60, ch = 2, W = 4, S = 2, m = 2;
    uint64_t a[2] = {10, 40}, b[2] = {12, 42};
    sox_sample_t * pcm = calloc((size_t)N * ch, sizeof(*pcm));
    int off[2];
    mem_ctx_t ctx = {pcm, ch};
    sox_sample_t scratch[2 * (size_t)MAX_W * MAX_CH];
    brute_result_t br = brute_solve(pcm, ch, N, a, b, m, W, S);
    TASK_CHECK(lsx_cutplan_solve(mem_reader, &ctx, ch, N, a, b, m, W, S,
        scratch, off, NULL) == 0);
    TASK_CHECK(off[0] == -2 && off[1] == -2);
    TASK_CHECK(br.vec[0] == -2 && br.vec[1] == -2 && br.ties > 1);
    TASK_CHECK(br.cost == 0);
    free(pcm);
  }

  /* P3: period-2 signal gives genuine cross-vector total-cost ties that can
   * only be settled by global lexicographic order. */
  {
    const unsigned N = 80, ch = 1, W = 3, S = 2, m = 2;
    uint64_t a[2] = {12, 48}, b[2] = {14, 50};
    sox_sample_t * pcm = calloc(N, sizeof(*pcm));
    int off[2], k;
    mem_ctx_t ctx = {pcm, ch};
    sox_sample_t scratch[2 * (size_t)MAX_W * MAX_CH];
    brute_result_t br;
    for (k = 0; k < (int)N; ++k) pcm[k] = (sox_sample_t)(k % 2 ? 999 : -999);
    br = brute_solve(pcm, ch, N, a, b, m, W, S);
    TASK_CHECK(br.ties >= 2);                          /* a real global tie */
    TASK_CHECK(lsx_cutplan_solve(mem_reader, &ctx, ch, N, a, b, m, W, S,
        scratch, off, NULL) == 0);
    TASK_CHECK(off[0] == br.vec[0] && off[1] == br.vec[1]);
    TASK_CHECK(off[0] == -2 && off[1] == -2);          /* global lex-min */
    free(pcm);
  }

  /* P4: S=0 leaves exactly the zero offset, when structurally possible. */
  {
    const unsigned N = 100, ch = 2, W = 8, S = 0, m = 2;
    uint64_t a[2] = {20, 60}, b[2] = {24, 64};
    sox_sample_t * pcm = calloc((size_t)N * ch, sizeof(*pcm));
    int off[2];
    mem_ctx_t ctx = {pcm, ch};
    sox_sample_t scratch[2 * (size_t)MAX_W * MAX_CH];
    fill_markers(pcm, N, ch);
    TASK_CHECK(lsx_cutplan_feasible(N, a, b, m, W, S) == 1);
    TASK_CHECK(lsx_cutplan_solve(mem_reader, &ctx, ch, N, a, b, m, W, S,
        scratch, off, NULL) == 0);
    TASK_CHECK(off[0] == 0 && off[1] == 0);
    free(pcm);
  }

  /* P5: no feasible path (tight island and edge-too-close). */
  {
    const unsigned W = 8, S = 1;
    uint64_t a1[2] = {10, 25}, b1[2] = {12, 27};      /* gap 13 < 2W-2 */
    uint64_t a2[1] = {5}, b2[1] = {6};                /* needs d >= 3 */
    sox_sample_t pcm[64] = {0};
    int off[2];
    mem_ctx_t ctx = {pcm, 1};
    sox_sample_t scratch[2 * (size_t)MAX_W * MAX_CH];
    TASK_CHECK(lsx_cutplan_feasible(40, a1, b1, 2, W, S) == 0);
    TASK_CHECK(lsx_cutplan_solve(mem_reader, &ctx, 1, 40, a1, b1, 2, W, S,
        scratch, off, NULL) != 0);
    TASK_CHECK(lsx_cutplan_feasible(40, a2, b2, 1, W, S) == 0);
  }

  /* P6: near full-scale costs exceed 64 bits; 128-bit comparison required.
   * b-a is odd so the two windows land on opposite parities of the
   * full-scale alternating signal: every difference is MAX-MIN. */
  {
    const unsigned N = 4096, ch = 1, W = 1024, S = 0, m = 1;
    uint64_t a[1] = {1100}, b[1] = {1103};
    sox_sample_t * pcm = calloc((size_t)N * ch, sizeof(*pcm));
    int off[1];
    mem_ctx_t ctx = {pcm, ch};
    sox_sample_t * scratch = calloc(2 * W * ch, sizeof(*scratch));
    unsigned __int128 total;
    unsigned __int128 diff = (unsigned __int128)SOX_SAMPLE_MAX -
                             (unsigned __int128)SOX_SAMPLE_MIN;
    unsigned __int128 expect = diff * diff * (unsigned __int128)W;
    unsigned k;
    for (k = 0; k < N; ++k)
      pcm[k] = (k & 1) ? SOX_SAMPLE_MAX : SOX_SAMPLE_MIN;
    TASK_CHECK(expect > (unsigned __int128)UINT64_MAX);
    TASK_CHECK(lsx_cutplan_solve(mem_reader, &ctx, ch, N, a, b, m, W, S,
        scratch, off, &total) == 0);
    TASK_CHECK(total == expect);
    TASK_CHECK(off[0] == 0);
    free(pcm);
    free(scratch);
  }

  /* P7: maximum search radius S=64; the left edge forces the most-negative
   * feasible offset even though the audio would prefer the other side. */
  {
    const unsigned N = 1000, ch = 1, W = 8, S = 64, m = 1;
    uint64_t a[1] = {70}, b[1] = {74};        /* left edge needs d >= -62 */
    sox_sample_t * pcm = calloc((size_t)N * ch, sizeof(*pcm));
    int off[1], k;
    mem_ctx_t ctx = {pcm, ch};
    sox_sample_t scratch[2 * (size_t)MAX_W * MAX_CH];
    brute_result_t br;
    for (k = 0; k < (int)N; ++k)
      pcm[k] = (sox_sample_t)((k * 48271u) % 2000001) - 1000000;
    br = brute_solve(pcm, ch, N, a, b, m, W, S);
    TASK_CHECK(br.found);
    TASK_CHECK(lsx_cutplan_solve(mem_reader, &ctx, ch, N, a, b, m, W, S,
        scratch, off, NULL) == 0);
    TASK_CHECK(off[0] == br.vec[0]);
    TASK_CHECK(off[0] >= -62);
    free(pcm);
  }

  /* P8: 64-segment plan at the table limit, W=1, S=0, three channels. */
  {
    const unsigned N = 8000, ch = 3, W = 1, S = 0, m = 64;
    uint64_t a[64], b[64];
    sox_sample_t * pcm = calloc((size_t)N * ch, sizeof(*pcm));
    int off[64], i;
    mem_ctx_t ctx = {pcm, ch};
    sox_sample_t scratch[2 * (size_t)MAX_W * MAX_CH];
    brute_result_t br;
    fill_markers(pcm, N, ch);
    for (i = 0; i < 64; ++i) { a[i] = 100 + (uint64_t)i * 100; b[i] = a[i] + 4; }
    TASK_CHECK(lsx_cutplan_feasible(N, a, b, m, W, S) == 1);
    br = brute_solve(pcm, ch, N, a, b, m, W, S);
    TASK_CHECK(br.found);
    TASK_CHECK(lsx_cutplan_solve(mem_reader, &ctx, ch, N, a, b, m, W, S,
        scratch, off, NULL) == 0);
    for (i = 0; i < 64; ++i)
      TASK_CHECK(off[i] == 0);
    free(pcm);
  }
}

/*--------------------- real effect: chain & manual driving -----------------*/

typedef struct {
  sox_sample_t * data;
  size_t cap, len;              /* samples */
} dynbuf_t;

static void db_push(dynbuf_t * db, const sox_sample_t * src, size_t n)
{
  if (db->len + n > db->cap) {
    db->cap = (db->len + n) * 2 + 64;
    db->data = realloc(db->data, db->cap * sizeof(sox_sample_t));
    if (!db->data) { perror("realloc"); exit(1); }
  }
  memcpy(db->data + db->len, src, n * sizeof(sox_sample_t));
  db->len += n;
}

/* In-memory source effect: emits frames following a 1/7/113 cycle. */
typedef struct {
  const sox_sample_t * pcm;
  size_t total, pos;
  unsigned ch;
  unsigned pat_i;
} src_ctx_t;

static int src_start(sox_effect_t * e)
{
  (void)e;
  return SOX_SUCCESS;
}

static int src_drain(sox_effect_t * e, sox_sample_t * obuf, size_t * osamp)
{
  static const unsigned pat[3] = {1, 7, 113};
  src_ctx_t * c = (src_ctx_t *)e->priv;
  size_t want = *osamp / c->ch;
  size_t chunk = pat[c->pat_i % 3];
  size_t take;
  if (want > chunk) want = chunk;
  take = c->total - c->pos;
  if (take > want) take = want;
  memcpy(obuf, c->pcm + c->pos * c->ch, take * c->ch * sizeof(sox_sample_t));
  c->pos += take;
  ++c->pat_i;
  *osamp = take * c->ch;
  return (c->pos == c->total) ? SOX_EOF : SOX_SUCCESS;
}

static sox_effect_handler_t const src_handler = {
  "mem_src_cutplan", NULL, SOX_EFF_MCHAN,
  NULL, src_start, NULL, src_drain, NULL, NULL, sizeof(src_ctx_t)
};

/* In-memory sink effect.  Like the built-in "output" effect it is
 * consume-only (osamp stays 0, data written straight to the caller), so the
 * chain engine does not grow a terminal output buffer. */
typedef struct { dynbuf_t * out; } sink_ctx_t;

static int sink_flow(sox_effect_t * e, const sox_sample_t * ibuf,
    sox_sample_t * obuf, size_t * isamp, size_t * osamp)
{
  sink_ctx_t * c = (sink_ctx_t *)e->priv;
  (void)obuf;
  if (*isamp)
    db_push(c->out, ibuf, *isamp);
  *osamp = 0;
  return SOX_SUCCESS;
}

static sox_effect_handler_t const sink_handler = {
  "mem_sink_cutplan", NULL, SOX_EFF_MCHAN,
  NULL, NULL, sink_flow, NULL, NULL, NULL, sizeof(sink_ctx_t)
};

static sox_signalinfo_t make_sig(unsigned ch, uint64_t frames)
{
  sox_signalinfo_t s;
  memset(&s, 0, sizeof(s));
  s.rate = 48000;
  s.channels = ch;
  s.precision = 32;
  s.length = frames * ch;
  return s;
}

/* Drive the real splice effect through a complete public effects chain. */
static int cancel_calls;

static int cancel_callback(sox_bool all_done, void * client_data)
{
  int * n = (int *)client_data;
  (void)all_done;
  ++*n;
  return *n >= 3 ? SOX_EOF : SOX_SUCCESS;
}

static int run_chain(const sox_sample_t * in, uint64_t frames, unsigned ch,
    const char * plan, unsigned W, unsigned S, size_t bufsiz_samples,
    dynbuf_t * out, int cancel_after, int * cancelled)
{
  sox_encodinginfo_t enc;
  sox_signalinfo_t sig = make_sig(ch, frames);
  sox_effects_chain_t * chain;
  sox_effect_t * e;
  src_ctx_t * src;
  sink_ctx_t * sink;
  size_t saved_bufsiz = sox_globals.bufsiz;
  char wstr[16], sstr[16];
  char * args[6];
  int rc;
  sox_init_encodinginfo(&enc);
  enc.encoding = SOX_ENCODING_SIGN2;
  enc.bits_per_sample = 32;
  sox_globals.bufsiz = bufsiz_samples;
  chain = sox_create_effects_chain(&enc, &enc);

  e = sox_create_effect(&src_handler);
  src = (src_ctx_t *)e->priv;
  src->pcm = in; src->total = frames; src->ch = ch;
  TASK_CHECK(sox_add_effect(chain, e, &sig, &sig) == SOX_SUCCESS);
  free(e);

  e = sox_create_effect(sox_find_effect("splice"));
  snprintf(wstr, sizeof(wstr), "%u", W);
  snprintf(sstr, sizeof(sstr), "%u", S);
  args[0] = (char *)"--cut-plan"; args[1] = (char *)plan;
  args[2] = (char *)"--overlap"; args[3] = wstr;
  args[4] = (char *)"--search";  args[5] = sstr;
  TASK_CHECK(sox_effect_options(e, 6, args) == SOX_SUCCESS);
  TASK_CHECK(sox_add_effect(chain, e, &sig, &sig) == SOX_SUCCESS);
  free(e);

  e = sox_create_effect(&sink_handler);
  sink = (sink_ctx_t *)e->priv;
  sink->out = out;
  TASK_CHECK(sox_add_effect(chain, e, &sig, &sig) == SOX_SUCCESS);
  free(e);

  cancel_calls = 0;
  *cancelled = 0;
  if (cancel_after > 0) {
    rc = sox_flow_effects(chain, cancel_callback, &cancel_calls);
    if (rc == SOX_EOF)
      *cancelled = 1;
  } else {
    rc = sox_flow_effects(chain, NULL, NULL);
  }
  TASK_CHECK(cancel_after > 0 || rc == SOX_SUCCESS);
  sox_delete_effects_chain(chain);
  sox_globals.bufsiz = saved_bufsiz;
  return SOX_SUCCESS;
}

/* Drive flow()/drain() on the effect directly, controlling both input
 * chunks and output drain capacity. */
typedef struct {
  int eof;
  size_t out_samples;
} manual_result_t;

static manual_result_t run_manual(const sox_sample_t * in, uint64_t frames,
    unsigned ch, uint64_t declared_frames, const char * plan_path,
    unsigned W, unsigned S, size_t out_cap_frames, dynbuf_t * out,
    unsigned pattern_mode)
{
  static const unsigned pat[3] = {1, 7, 113};
  sox_signalinfo_t sig = make_sig(ch, declared_frames);
  sox_effect_t * e;
  char wstr[16], sstr[16];
  char * args[6];
  size_t in_pos = 0, pi = 0;
  manual_result_t res = {0, 0};

  e = sox_create_effect(sox_find_effect("splice"));
  snprintf(wstr, sizeof(wstr), "%u", W);
  snprintf(sstr, sizeof(sstr), "%u", S);
  args[0] = (char *)"--cut-plan"; args[1] = (char *)plan_path;
  args[2] = (char *)"--overlap"; args[3] = wstr;
  args[4] = (char *)"--search";  args[5] = sstr;
  TASK_CHECK(sox_effect_options(e, 6, args) == SOX_SUCCESS);
  e->in_signal = sig;
  e->out_signal = sig;
  TASK_CHECK(e->handler.start(e) == SOX_SUCCESS);

  while (in_pos < frames) {
    size_t chunk = pattern_mode ? pat[pi % 3] : 8192;
    size_t isamp, osamp = 0;
    sox_sample_t scratch[1];
    if (chunk > frames - in_pos) chunk = frames - in_pos;
    isamp = chunk * ch;
    TASK_CHECK(e->handler.flow(e, in + in_pos * ch, scratch,
        &isamp, &osamp) == SOX_SUCCESS);
    TASK_CHECK(osamp == 0);
    in_pos += isamp / ch;
    ++pi;
  }

  for (;;) {
    size_t osamp = out_cap_frames * ch;
    sox_sample_t * obuf = malloc(osamp * sizeof(sox_sample_t));
    int rc = e->handler.drain(e, obuf, &osamp);
    if (osamp)
      db_push(out, obuf, osamp);
    res.out_samples += osamp;
    free(obuf);
    if (rc == SOX_EOF) { res.eof = 1; break; }
    TASK_CHECK(rc == SOX_SUCCESS);
  }
  /* A drain after completion must emit nothing and keep returning EOF. */
  {
    size_t osamp = out_cap_frames * ch;
    sox_sample_t * obuf = calloc(osamp, sizeof(sox_sample_t));
    int rc = e->handler.drain(e, obuf, &osamp);
    TASK_CHECK(rc == SOX_EOF && osamp == 0);
    free(obuf);
  }
  TASK_CHECK(e->handler.stop(e) == SOX_SUCCESS);
  e->handler.kill(e);
  free(e->priv);
  free(e);
  return res;
}

/*================================== pcm ====================================*/

static void check_render_case(const char * tag, uint64_t N, unsigned ch,
    const uint64_t * a, const uint64_t * b, unsigned m, unsigned W,
    unsigned S, const sox_sample_t * pcm)
{
  char plan[256];
  brute_result_t br;
  size_t expect_frames = expected_out_frames(N, a, b, m, W);
  sox_sample_t * ref = malloc((size_t)(expect_frames + 1) * ch * sizeof(*ref));
  size_t ref_samples;
  dynbuf_t out1 = {0}, out2 = {0}, out3 = {0};
  unsigned i;
  int cancelled;

  snprintf(plan, sizeof(plan), "%s/sox_cp_%s_%ld.txt",
      tmp_dir(), tag, (long)getpid());
  write_plan_file(plan, a, b, m, 1);

  br = brute_solve(pcm, ch, N, a, b, m, W, S);
  TASK_CHECK(br.found);
  ref_samples = reference_render(pcm, ch, N, a, b, m, br.vec, W,
      ref, (expect_frames + 1) * ch);
  TASK_CHECK(ref_samples == expect_frames * ch);

  /* Different chain buffer sizes and the 1/7/113 input pattern. */
  TASK_CHECK(run_chain(pcm, N, ch, plan, W, S, (size_t)64 * ch,
      &out1, 0, &cancelled) == SOX_SUCCESS);
  TASK_CHECK(run_chain(pcm, N, ch, plan, W, S, (size_t)8192 * ch,
      &out2, 0, &cancelled) == SOX_SUCCESS);
  TASK_CHECK(out1.len == ref_samples && out2.len == ref_samples);
  TASK_CHECK(memcmp(out1.data, ref, ref_samples * sizeof(*ref)) == 0);
  TASK_CHECK(memcmp(out2.data, ref, ref_samples * sizeof(*ref)) == 0);

  /* Tiny odd output drain capacities via direct flow/drain driving. */
  TASK_CHECK(run_manual(pcm, N, ch, N, plan, W, S, 1, &out3, 1).eof == 1);
  TASK_CHECK(out3.len == ref_samples);
  TASK_CHECK(memcmp(out3.data, ref, ref_samples * sizeof(*ref)) == 0);

  /* Retained regions are copied sample-for-sample from the original; head
   * and tail frames match whenever they fall outside a cross-fade window. */
  {
    long long hf = retained_output_frame(N, a, b, m, W, br.vec, 0);
    long long tf = retained_output_frame(N, a, b, m, W, br.vec, N - 1);
    if (hf >= 0)
      for (i = 0; i < ch; ++i)
        TASK_CHECK(out1.data[(size_t)hf * ch + i] == pcm[i]);
    if (tf >= 0)
      for (i = 0; i < ch; ++i)
        TASK_CHECK(out1.data[(size_t)tf * ch + i] ==
            pcm[(N - 1) * ch + i]);
  }
  {
    uint64_t x;
    unsigned checks = 0;
    for (x = 0; x < N && checks < 16; ++x) {
      long long of = retained_output_frame(N, a, b, m, W, br.vec, x);
      if (of < 0)
        continue;
      for (i = 0; i < ch; ++i)
        TASK_CHECK(out1.data[(size_t)of * ch + i] == pcm[x * ch + i]);
      ++checks;
    }
    TASK_CHECK(checks > 0);
  }

  free(ref); free(out1.data); free(out2.data); free(out3.data);
  unlink(plan);
}

static void test_pcm(void)
{
  /* C1: head and tail boundary splices, two joints, markers, W=4. */
  {
    const uint64_t N = 40;
    const unsigned ch = 2, W = 4, S = 2, m = 2;
    uint64_t a[2] = {6, 32}, b[2] = {8, 34};
    sox_sample_t * pcm = malloc((size_t)N * ch * sizeof(*pcm));
    fill_markers(pcm, N, ch);
    check_render_case("c1", N, ch, a, b, m, W, S, pcm);
    free(pcm);
  }

  /* C2: W=1, single splice; j=0 gives the pure left sample. */
  {
    const uint64_t N = 30;
    const unsigned ch = 2, W = 1, S = 2, m = 1;
    uint64_t a[1] = {10}, b[1] = {12};
    sox_sample_t * pcm = malloc((size_t)N * ch * sizeof(*pcm));
    fill_markers(pcm, N, ch);
    check_render_case("c2", N, ch, a, b, m, W, S, pcm);
    free(pcm);
  }

  /* C3: three splices, one deliberately tight retention island (gap 12,
   * 2W=10, S=3) so adjacency constraints bind. */
  {
    const uint64_t N = 120;
    const unsigned ch = 2, W = 5, S = 3, m = 3;
    uint64_t a[3] = {12, 26, 80}, b[3] = {14, 28, 84};
    sox_sample_t * pcm = malloc((size_t)N * ch * sizeof(*pcm));
    fill_markers(pcm, N, ch);
    pcm[60 * ch] = SOX_SAMPLE_MAX / 2;      /* perturb a retained frame */
    pcm[60 * ch + 1] = SOX_SAMPLE_MIN / 2;
    check_render_case("c3", N, ch, a, b, m, W, S, pcm);
    free(pcm);
  }

  /* C4: opposite channel polarity. */
  {
    const uint64_t N = 64;
    const unsigned ch = 2, W = 6, S = 2, m = 2;
    uint64_t a[2] = {10, 44}, b[2] = {12, 48};
    sox_sample_t * pcm = malloc((size_t)N * ch * sizeof(*pcm));
    uint64_t f;
    for (f = 0; f < N; ++f) {
      sox_sample_t v = marker_val(f, 0);
      pcm[f * 2] = v;
      pcm[f * 2 + 1] = (sox_sample_t)(-(int64_t)v);
    }
    check_render_case("c4", N, ch, a, b, m, W, S, pcm);
    free(pcm);
  }

  /* C5: channels disagree about offsets; one shared vector must win. */
  {
    const uint64_t N = 96;
    const unsigned ch = 2, W = 4, S = 3, m = 2;
    uint64_t a[2] = {14, 60}, b[2] = {18, 62};
    sox_sample_t * pcm = malloc((size_t)N * ch * sizeof(*pcm));
    uint64_t f;
    for (f = 0; f < N; ++f) {
      pcm[f * 2]     = (sox_sample_t)(f % 2 ? 70000 : -70000);
      pcm[f * 2 + 1] = marker_val(f, 1);
    }
    check_render_case("c5", N, ch, a, b, m, W, S, pcm);
    free(pcm);
  }

  /* C6: real CLI run, both buffer sizes, byte-exact against each other and
   * the chain reference. */
  {
    const char * bin = getenv("SOX_TASK_BIN");
    const uint64_t N = 50;
    const unsigned ch = 2, W = 4, S = 2, m = 1;
    uint64_t a[1] = {16}, b[1] = {20};
    char in[256], plan[256], o64[256], o8192[256], err[256];
    brute_result_t br;
    sox_sample_t * pcm = malloc((size_t)N * ch * sizeof(*pcm));
    size_t expect_frames = expected_out_frames(N, a, b, m, W);
    sox_sample_t * ref = malloc(expect_frames * ch * sizeof(*ref));
    FILE * f;
    size_t got64, got8192;
    sox_sample_t * buf64, * buf8192;
    if (bin == NULL) bin = ".build/src/sox";
    snprintf(in, sizeof(in), "%s/sox_cp_c6_%ld.s32", tmp_dir(), (long)getpid());
    snprintf(plan, sizeof(plan), "%s/sox_cp_c6_%ld.txt", tmp_dir(), (long)getpid());
    snprintf(o64, sizeof(o64), "%s/sox_cp_c6_%ld.64.s32", tmp_dir(), (long)getpid());
    snprintf(o8192, sizeof(o8192), "%s/sox_cp_c6_%ld.8k.s32", tmp_dir(), (long)getpid());
    snprintf(err, sizeof(err), "%s/sox_cp_c6_%ld.err", tmp_dir(), (long)getpid());
    fill_markers(pcm, N, ch);
    f = fopen(in, "wb");
    TASK_CHECK(f && fwrite(pcm, sizeof(*pcm), (size_t)N * ch, f) == N * ch);
    fclose(f);
    write_plan_file(plan, a, b, m, 0);
    br = brute_solve(pcm, ch, N, a, b, m, W, S);
    TASK_CHECK(br.found);
    TASK_CHECK(reference_render(pcm, ch, N, a, b, m, br.vec, W, ref,
        expect_frames * ch) == expect_frames * ch);

    {
      const char * args1[] = {bin, "-D", "--buffer", "64", "-t", "raw",
          "-e", "signed-integer", "-b", "32", "-r", "48000", "-c", "2",
          in, "-t", "raw", "-e", "signed-integer", "-b", "32", o64,
          "splice", "--cut-plan", plan, "--overlap", "4", "--search", "2",
          NULL};
      const char * args2[] = {bin, "-D", "--buffer", "8192", "-t", "raw",
          "-e", "signed-integer", "-b", "32", "-r", "48000", "-c", "2",
          in, "-t", "raw", "-e", "signed-integer", "-b", "32", o8192,
          "splice", "--cut-plan", plan, "--overlap", "4", "--search", "2",
          NULL};
      pid_t pid;
      pid = fork();
      TASK_CHECK(pid >= 0);
      if (pid == 0) {
        int efd = open(err, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (efd >= 0) { dup2(efd, 2); if (efd != 2) close(efd); }
        execv(bin, (char * const *)(uintptr_t)args1);
        {
          char m[256];
          int n = snprintf(m, sizeof(m), "execv(%s) failed: %s\n",
              bin, strerror(errno));
          write(2, m, (size_t)n);
        }
        _exit(127);
      }
      TASK_CHECK(wait_child_ok(pid, err));
      pid = fork();
      TASK_CHECK(pid >= 0);
      if (pid == 0) {
        int efd = open(err, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (efd >= 0) { dup2(efd, 2); if (efd != 2) close(efd); }
        execv(bin, (char * const *)(uintptr_t)args2);
        {
          char m[256];
          int n = snprintf(m, sizeof(m), "execv(%s) failed: %s\n",
              bin, strerror(errno));
          write(2, m, (size_t)n);
        }
        _exit(127);
      }
      TASK_CHECK(wait_child_ok(pid, err));
    }
    f = fopen(o64, "rb");
    fseek(f, 0, SEEK_END); got64 = (size_t)ftell(f);
    buf64 = malloc(got64); rewind(f);
    TASK_CHECK(fread(buf64, 1, got64, f) == got64);
    fclose(f);
    f = fopen(o8192, "rb");
    fseek(f, 0, SEEK_END); got8192 = (size_t)ftell(f);
    buf8192 = malloc(got8192); rewind(f);
    TASK_CHECK(fread(buf8192, 1, got8192, f) == got8192);
    fclose(f);
    TASK_CHECK(got64 == got8192);
    TASK_CHECK(got64 == expect_frames * ch * sizeof(sox_sample_t));
    TASK_CHECK(memcmp(buf64, buf8192, got64) == 0);
    TASK_CHECK(memcmp(buf64, ref, got64) == 0);

    free(pcm); free(ref); free(buf64); free(buf8192);
    unlink(in); unlink(plan); unlink(o64); unlink(o8192); unlink(err);
  }

  /* C7: maximum window W=1024 and the 8-channel limit, two splices. */
  {
    const uint64_t N = 4400;
    const unsigned ch = 8, W = 1024, S = 2, m = 2;
    uint64_t a[2] = {1100, 3200}, b[2] = {1104, 3206};
    sox_sample_t * pcm = malloc((size_t)N * ch * sizeof(*pcm));
    fill_markers(pcm, N, ch);
    check_render_case("c7", N, ch, a, b, m, W, S, pcm);
    free(pcm);
  }

  /* C8: exact equality adjacency - with equal offsets the two cross-fade
   * windows touch (b0+d0+W == a1+d1-W), leaving zero extra retained frames.
   * A silent plan ties everywhere so the lex-min vector is (-1,-1), which
   * is exactly the touching configuration. */
  {
    const uint64_t N = 40;
    const unsigned ch = 2, W = 4, S = 1, m = 2;
    uint64_t a[2] = {8, 20}, b[2] = {12, 24};   /* gap 8 == 2W when d0==d1 */
    sox_sample_t * pcm = calloc((size_t)N * ch, sizeof(*pcm));
    brute_result_t br = brute_solve(pcm, ch, N, a, b, m, W, S);
    TASK_CHECK(br.found && br.vec[0] == -1 && br.vec[1] == -1);
    TASK_CHECK(b[0] + br.vec[0] + W == a[1] + br.vec[1] - W);
    check_render_case("c8", N, ch, a, b, m, W, S, pcm);
    free(pcm);
  }
}

/*================================ lifecycle ================================*/


static int count_dir_entries(const char * path)
{
  DIR * d = opendir(path);
  struct dirent * e;
  int n = 0;
  if (!d) return -1;
  while ((e = readdir(d)) != NULL)
    if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
      ++n;
  closedir(d);
  return n;
}

static int wait_child_ok(pid_t pid, const char * err_path)
{
  int st;
  waitpid(pid, &st, 0);
  if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
    return 1;
  {
    char diag[512];
    FILE * ef = fopen(err_path, "r");
    size_t dn = 0;
    if (ef) { dn = fread(diag, 1, sizeof(diag) - 1, ef); fclose(ef); }
    diag[dn] = 0;
    fprintf(stderr, "child failed status=%d sig=%d stderr=<%s>\n",
        WIFEXITED(st) ? (int)WEXITSTATUS(st) : -1,
        WIFSIGNALED(st) ? (int)WTERMSIG(st) : -1, diag);
  }
  return 0;
}

/* Validate argument/coordinate rejection through the real effect entry
 * points (getopts / start), including overflow-sized values. */
static void test_validation(void)
{
  char plan[256];
  sox_signalinfo_t sig;
  sox_effect_t * e;
  unsigned ch = 2;
  uint64_t N = 60;
  sox_sample_t pcm[120];

  snprintf(plan, sizeof(plan), "%s/sox_cp_val_%ld.txt", tmp_dir(),
      (long)getpid());
  write_plan_file(plan, (uint64_t[]){16}, (uint64_t[]){20}, 1, 1);
  memset(&sig, 0, sizeof(sig));
  sig.rate = 48000; sig.channels = ch; sig.precision = 32;
  sig.length = N * ch;
  memset(pcm, 0, sizeof(pcm));

  /* W and S out of range, non-numeric, missing options, unknown flags. */
  {
    char * badW[6] = {"--cut-plan", plan, "--overlap", "0", "--search", "2"};
    char * bigW[6] = {"--cut-plan", plan, "--overlap", "1025",
                      "--search", "2"};
    char * badS[6] = {"--cut-plan", plan, "--overlap", "4", "--search", "65"};
    char * nonum[6] = {"--cut-plan", plan, "--overlap", "4x",
                       "--search", "2"};
    char * missing[4] = {"--cut-plan", plan, "--overlap", "4"};
    char * unknown[2] = {"--frobnicate", "x"};
    char * huge[6] = {"--cut-plan", plan, "--overlap",
                      "99999999999999999999999999", "--search", "2"};
    char * neg[6] = {"--cut-plan", plan, "--overlap", "-3", "--search", "2"};
    e = sox_create_effect(sox_find_effect("splice"));
    TASK_CHECK(sox_effect_options(e, 6, badW) == SOX_EOF);
    sox_delete_effect(e);
    e = sox_create_effect(sox_find_effect("splice"));
    TASK_CHECK(sox_effect_options(e, 6, bigW) == SOX_EOF);
    sox_delete_effect(e);
    e = sox_create_effect(sox_find_effect("splice"));
    TASK_CHECK(sox_effect_options(e, 6, badS) == SOX_EOF);
    sox_delete_effect(e);
    e = sox_create_effect(sox_find_effect("splice"));
    TASK_CHECK(sox_effect_options(e, 6, nonum) == SOX_EOF);
    sox_delete_effect(e);
    e = sox_create_effect(sox_find_effect("splice"));
    TASK_CHECK(sox_effect_options(e, 4, missing) == SOX_EOF);
    sox_delete_effect(e);
    e = sox_create_effect(sox_find_effect("splice"));
    TASK_CHECK(sox_effect_options(e, 2, unknown) == SOX_EOF);
    sox_delete_effect(e);
    e = sox_create_effect(sox_find_effect("splice"));
    TASK_CHECK(sox_effect_options(e, 6, huge) == SOX_EOF);
    sox_delete_effect(e);
    e = sox_create_effect(sox_find_effect("splice"));
    TASK_CHECK(sox_effect_options(e, 6, neg) == SOX_EOF);
    sox_delete_effect(e);
  }

  /* start() rejects unknown length, length not divisible by channels, too
   * many channels, an unreadable plan, and structurally infeasible plans. */
  {
    char * ok[6] = {"--cut-plan", plan, "--overlap", "4", "--search", "2"};
    e = sox_create_effect(sox_find_effect("splice"));
    TASK_CHECK(sox_effect_options(e, 6, ok) == SOX_SUCCESS);
    e->in_signal = sig; e->out_signal = sig;
    e->in_signal.length = SOX_UNKNOWN_LEN;
    TASK_CHECK(e->handler.start(e) != SOX_SUCCESS);
    e->handler.kill(e); free(e->priv); free(e);

    e = sox_create_effect(sox_find_effect("splice"));
    TASK_CHECK(sox_effect_options(e, 6, ok) == SOX_SUCCESS);
    e->in_signal = sig; e->out_signal = sig;
    e->in_signal.length = N * ch + 1;        /* not divisible */
    TASK_CHECK(e->handler.start(e) != SOX_SUCCESS);
    e->handler.kill(e); free(e->priv); free(e);

    e = sox_create_effect(sox_find_effect("splice"));
    TASK_CHECK(sox_effect_options(e, 6, ok) == SOX_SUCCESS);
    e->in_signal = sig; e->out_signal = sig;
    e->in_signal.channels = 9;               /* > 8 */
    TASK_CHECK(e->handler.start(e) != SOX_SUCCESS);
    e->handler.kill(e); free(e->priv); free(e);

    {
      char * noplan[6] = {"--cut-plan",
          "/no/such/dir/sox_cp_missing_plan.txt",
          "--overlap", "4", "--search", "2"};
      e = sox_create_effect(sox_find_effect("splice"));
      TASK_CHECK(sox_effect_options(e, 6, noplan) == SOX_SUCCESS);
      e->in_signal = sig; e->out_signal = sig;
      TASK_CHECK(e->handler.start(e) != SOX_SUCCESS);
      e->handler.kill(e); free(e->priv); free(e);
    }
    {
      char tight[256];
      char * args[6];
      uint64_t ta[2] = {10, 25}, tb[2] = {12, 27};   /* gap 13, W 8 */
      snprintf(tight, sizeof(tight), "%s/sox_cp_tight_%ld.txt",
          tmp_dir(), (long)getpid());
      write_plan_file(tight, ta, tb, 2, 0);
      args[0] = "--cut-plan"; args[1] = tight;
      args[2] = "--overlap"; args[3] = "8";
      args[4] = "--search";  args[5] = "1";
      e = sox_create_effect(sox_find_effect("splice"));
      TASK_CHECK(sox_effect_options(e, 6, args) == SOX_SUCCESS);
      e->in_signal = sig; e->out_signal = sig;
      TASK_CHECK(e->handler.start(e) != SOX_SUCCESS);  /* infeasible */
      e->handler.kill(e); free(e->priv); free(e);
      unlink(tight);
    }
  }

  /* Malformed plan files: extra field, a>=b, non-increasing start, overlap,
   * segment outside input, and an oversized unsigned coordinate. */
  {
    const char * cases[6] = {"16 20 x\n", "20 20\n", "20 24\n18 30\n",
        "16 24\n20 28\n", "55 70\n",
        "18446744073709551616 5\n"};
    unsigned t;
    for (t = 0; t < 6; ++t) {
      char mp[256];
      char * args[6];
      FILE * mf;
      snprintf(mp, sizeof(mp), "%s/sox_cp_mal_%u_%ld.txt", tmp_dir(),
          t, (long)getpid());
      mf = fopen(mp, "w");
      TASK_CHECK(mf != NULL);
      fputs(cases[t], mf);
      fclose(mf);
      args[0] = "--cut-plan"; args[1] = mp;
      args[2] = "--overlap"; args[3] = "4";
      args[4] = "--search";  args[5] = "2";
      e = sox_create_effect(sox_find_effect("splice"));
      TASK_CHECK(sox_effect_options(e, 6, args) == SOX_SUCCESS);
      e->in_signal = sig; e->out_signal = sig;
      TASK_CHECK(e->handler.start(e) != SOX_SUCCESS);
      e->handler.kill(e); free(e->priv); free(e);
      unlink(mp);
    }
    /* empty plan (only comments and blank lines) */
    {
      char mp[256];
      char * args[6];
      FILE * mf;
      snprintf(mp, sizeof(mp), "%s/sox_cp_empty_%ld.txt", tmp_dir(),
          (long)getpid());
      mf = fopen(mp, "w");
      TASK_CHECK(mf != NULL);
      fputs("# nothing here\n\n   \n", mf);
      fclose(mf);
      args[0] = "--cut-plan"; args[1] = mp;
      args[2] = "--overlap"; args[3] = "2";
      args[4] = "--search";  args[5] = "1";
      e = sox_create_effect(sox_find_effect("splice"));
      TASK_CHECK(sox_effect_options(e, 6, args) == SOX_SUCCESS);
      e->in_signal = sig; e->out_signal = sig;
      TASK_CHECK(e->handler.start(e) != SOX_SUCCESS);
      e->handler.kill(e); free(e->priv); free(e);
      unlink(mp);
    }
    /* more than 64 segments */
    {
      char mp[256];
      char * args[6];
      FILE * mf;
      unsigned t2;
      snprintf(mp, sizeof(mp), "%s/sox_cp_many_%ld.txt", tmp_dir(),
          (long)getpid());
      mf = fopen(mp, "w");
      TASK_CHECK(mf != NULL);
      for (t2 = 0; t2 < 65; ++t2)
        fprintf(mf, "%u %u\n", 2 + t2 * 2, 3 + t2 * 2);
      fclose(mf);
      args[0] = "--cut-plan"; args[1] = mp;
      args[2] = "--overlap"; args[3] = "1";
      args[4] = "--search";  args[5] = "0";
      e = sox_create_effect(sox_find_effect("splice"));
      TASK_CHECK(sox_effect_options(e, 6, args) == SOX_SUCCESS);
      e->in_signal = sig; e->out_signal = sig;
      TASK_CHECK(e->handler.start(e) != SOX_SUCCESS);
      e->handler.kill(e); free(e->priv); free(e);
      unlink(mp);
    }
    /* overlong line, even as a comment, must be rejected */
    {
      char mp[256];
      char * args[6];
      FILE * mf;
      unsigned t2;
      snprintf(mp, sizeof(mp), "%s/sox_cp_long_%ld.txt", tmp_dir(),
          (long)getpid());
      mf = fopen(mp, "w");
      TASK_CHECK(mf != NULL);
      fputc('#', mf);
      for (t2 = 0; t2 < 2000; ++t2)
        fputc('x', mf);
      fputc('\n', mf);
      fputs("16 20\n", mf);
      fclose(mf);
      args[0] = "--cut-plan"; args[1] = mp;
      args[2] = "--overlap"; args[3] = "2";
      args[4] = "--search";  args[5] = "1";
      e = sox_create_effect(sox_find_effect("splice"));
      TASK_CHECK(sox_effect_options(e, 6, args) == SOX_SUCCESS);
      e->in_signal = sig; e->out_signal = sig;
      TASK_CHECK(e->handler.start(e) != SOX_SUCCESS);
      e->handler.kill(e); free(e->priv); free(e);
      unlink(mp);
    }
    /* overlap of otherwise separate segments */
    {
      char mp[256];
      char * args[6];
      FILE * mf = NULL;
      uint64_t oa[2] = {10, 14}, ob[2] = {16, 20};
      snprintf(mp, sizeof(mp), "%s/sox_cp_ovl_%ld.txt", tmp_dir(),
          (long)getpid());
      mf = fopen(mp, "w");
      TASK_CHECK(mf != NULL);
      fprintf(mf, "%llu %llu\n%llu %llu\n",
          (unsigned long long)oa[0], (unsigned long long)ob[0],
          (unsigned long long)oa[1], (unsigned long long)ob[1]);
      fclose(mf);
      args[0] = "--cut-plan"; args[1] = mp;
      args[2] = "--overlap"; args[3] = "2";
      args[4] = "--search";  args[5] = "1";
      e = sox_create_effect(sox_find_effect("splice"));
      TASK_CHECK(sox_effect_options(e, 6, args) == SOX_SUCCESS);
      e->in_signal = sig; e->out_signal = sig;
      TASK_CHECK(e->handler.start(e) != SOX_SUCCESS);
      e->handler.kill(e); free(e->priv); free(e);
      unlink(mp);
    }
  }
  unlink(plan);
}

static void test_lifecycle(void)
{
  const unsigned ch = 2, W = 4, S = 2, m = 1;
  uint64_t a[1] = {16}, b[1] = {20};
  const uint64_t N = 60;
  sox_sample_t * pcm = malloc((size_t)N * ch * sizeof(*pcm));
  brute_result_t br;
  size_t expect_frames;
  sox_sample_t * ref;
  char plan[256];
  static const size_t caps[4] = {1, 7, 13, 113};
  dynbuf_t runs[4] = {{0}};
  size_t k;
  int cancelled;

  fill_markers(pcm, N, ch);
  snprintf(plan, sizeof(plan), "%s/sox_cp_lc_%ld.txt", tmp_dir(), (long)getpid());
  write_plan_file(plan, a, b, m, 1);
  br = brute_solve(pcm, ch, N, a, b, m, W, S);
  TASK_CHECK(br.found);
  expect_frames = expected_out_frames(N, a, b, m, W);
  ref = malloc(expect_frames * ch * sizeof(*ref));
  TASK_CHECK(reference_render(pcm, ch, N, a, b, m, br.vec, W, ref,
      expect_frames * ch) == expect_frames * ch);

  /* L1: 1/7/113 input alternation with several output drain capacities. */
  for (k = 0; k < sizeof(caps) / sizeof(caps[0]); ++k) {
    manual_result_t r = run_manual(pcm, N, ch, N, plan, W, S, caps[k],
        &runs[k], 1);
    TASK_CHECK(r.eof == 1);
    TASK_CHECK(runs[k].len == expect_frames * ch);
    TASK_CHECK(memcmp(runs[k].data, ref, runs[k].len * sizeof(*ref)) == 0);
  }
  /* All chunkings produce identical PCM. */
  for (k = 1; k < sizeof(caps) / sizeof(caps[0]); ++k)
    TASK_CHECK(runs[k].len == runs[0].len &&
        memcmp(runs[k].data, runs[0].data,
            runs[0].len * sizeof(*ref)) == 0);

  /* L2: chain flow at buffer 64 and 8192, full drain, identical PCM. */
  {
    dynbuf_t o64 = {0}, o8k = {0};
    TASK_CHECK(run_chain(pcm, N, ch, plan, W, S, 64 * ch, &o64, 0,
        &cancelled) == SOX_SUCCESS);
    TASK_CHECK(run_chain(pcm, N, ch, plan, W, S, 8192 * ch, &o8k, 0,
        &cancelled) == SOX_SUCCESS);
    TASK_CHECK(o64.len == expect_frames * ch && o8k.len == o64.len);
    TASK_CHECK(memcmp(o64.data, ref, o64.len * sizeof(*ref)) == 0);
    TASK_CHECK(memcmp(o8k.data, ref, o8k.len * sizeof(*ref)) == 0);
    free(o64.data); free(o8k.data);
  }

  /* L3a: cancellation through the flow callback; temp resources cleaned.
   * A dedicated temp directory isolates this run from other processes. */
  {
    char tdir[256];
    char * saved_tmp;
    dynbuf_t dropped = {0};
    int was_cancelled = 0;
    snprintf(tdir, sizeof(tdir), "%s/sox_cp_tmp_%ld", tmp_dir(),
        (long)getpid());
    TASK_CHECK(mkdir(tdir, 0700) == 0);
    saved_tmp = sox_globals.tmp_path;
    sox_globals.tmp_path = tdir;
    TASK_CHECK(run_chain(pcm, N, ch, plan, W, S, 64 * ch, &dropped, 1,
        &was_cancelled) == SOX_SUCCESS);
    sox_globals.tmp_path = saved_tmp;
    TASK_CHECK(was_cancelled == 1);
    TASK_CHECK(count_dir_entries(tdir) == 0);
    rmdir(tdir);
    free(dropped.data);
  }

  /* L3b: short read (declared length exceeds actual input) must fail drain
   * with a diagnostic and emit no rendered PCM. */
  {
    dynbuf_t short_out = {0};
    manual_result_t r = run_manual(pcm, N, ch, N + 10, plan, W, S, 7,
        &short_out, 0);
    TASK_CHECK(r.eof == 1 && r.out_samples == 0);
    free(short_out.data);
  }

  for (k = 0; k < sizeof(caps) / sizeof(caps[0]); ++k)
    free(runs[k].data);
  free(ref);
  unlink(plan);
  free(pcm);

  /* L4: input far larger than the PCM memory budget; disk-backed processing
   * keeps bounded buffers and exact output. */
  {
    const uint64_t bigN = 200000;
    const unsigned bigW = 16, bigS = 4, bigm = 2, bigch = 2;
    uint64_t ba[2] = {50000, 150000}, bb[2] = {50010, 150008};
    char bplan[256], obs[256];
    sox_sample_t * big = malloc((size_t)bigN * bigch * sizeof(*big));
    dynbuf_t bout = {0};
    brute_result_t bbr;
    size_t bexp;
    sox_sample_t * bref;
    FILE * mf;
    unsigned long long n = 0, mcnt = 0, pcm_s = 0, tmp_b = 0, outs = 0;
    unsigned WW = 0, SS = 0;
    fill_markers(big, bigN, bigch);
    snprintf(bplan, sizeof(bplan), "%s/sox_cp_big_%ld.txt",
        tmp_dir(), (long)getpid());
    snprintf(obs, sizeof(obs), "%s/sox_cp_big_%ld.obs",
        tmp_dir(), (long)getpid());
    write_plan_file(bplan, ba, bb, bigm, 1);
    unlink(obs);
    setenv("SOX_CUT_PLAN_OBS", obs, 1);
    TASK_CHECK(run_chain(big, bigN, bigch, bplan, bigW, bigS, 8192 * bigch,
        &bout, 0, &cancelled) == SOX_SUCCESS);
    unsetenv("SOX_CUT_PLAN_OBS");

    bbr = brute_solve(big, bigch, bigN, ba, bb, bigm, bigW, bigS);
    TASK_CHECK(bbr.found);
    bexp = expected_out_frames(bigN, ba, bb, bigm, bigW);
    TASK_CHECK(bout.len == bexp * bigch);
    bref = malloc(bout.len * sizeof(*bref));
    TASK_CHECK(reference_render(big, bigch, bigN, ba, bb, bigm, bbr.vec,
        bigW, bref, bout.len) == bout.len);
    TASK_CHECK(memcmp(bout.data, bref, bout.len * sizeof(*bref)) == 0);

    mf = fopen(obs, "r");
    TASK_CHECK(mf != NULL);
    TASK_CHECK(fscanf(mf,
        "n=%llu m=%llu W=%u S=%u pcmsamples=%llu tmpbytes=%llu out=%llu",
        &n, &mcnt, &WW, &SS, &pcm_s, &tmp_b, &outs) == 7);
    fclose(mf);
    TASK_CHECK(n == bigN && mcnt == bigm && WW == bigW && SS == bigS);
    TASK_CHECK(tmp_b == bigN * bigch * sizeof(sox_sample_t));
    TASK_CHECK((uint64_t)pcm_s <=
        (uint64_t)bigch * (4u * bigW + 2u * bigS + 8192u));
    TASK_CHECK((uint64_t)outs == bout.len);
    TASK_CHECK((uint64_t)tmp_b >
        (uint64_t)bigch * (4u * bigW + 2u * bigS + 8192u) *
        sizeof(sox_sample_t));

    free(big); free(bref); free(bout.data);
    unlink(bplan); unlink(obs);
  }
}

int run_cut_plan_tests(const char * group)
{
  static int initialized;
  if (!initialized) {
    TASK_CHECK(sox_init() == SOX_SUCCESS);
    initialized = 1;
  }
  if (!strcmp(group, "planner"))
    test_planner();
  else if (!strcmp(group, "pcm"))
    test_pcm();
  else if (!strcmp(group, "lifecycle")) {
    test_validation();
    test_lifecycle();
  }
  else
    return 1;
  return 0;
}
