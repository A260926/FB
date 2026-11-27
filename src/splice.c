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
#include "splice_cut_plan.h"
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <time.h>

/*
 * Joint cut-plan mode
 * -------------------
 *
 * `splice --cut-plan FILE --overlap W --search S' removes the given,
 * non-overlapping frame intervals from the input, replacing each one with a
 * W-frame triangular cross-fade between the audio immediately before and
 * immediately after the cut.  Unlike the legacy splice mode, the integer
 * window shift of every cut is chosen jointly so that the optimum is the
 * minimum of the *sum* of all per-cut costs (not a per-cut greedy choice).
 *
 * The arithmetic that compares costs is exact: the per-sample squared
 * difference and the accumulated totals are evaluated as mathematical
 * integers using __int128, never as floating point.
 */

#define CUT_PLAN_MAX_CUTS    LSX_CUT_PLAN_MAX_CUTS
#define CUT_PLAN_MAX_CHANS   LSX_CUT_PLAN_MAX_CHANS
#define CUT_PLAN_MAX_W       LSX_CUT_PLAN_MAX_W
#define CUT_PLAN_MAX_S       LSX_CUT_PLAN_MAX_S
#define CUT_PLAN_COPY_FRAMES 8192

typedef lsx_cut_plan_interval_t cut_interval_t;

/* Exact-integer helper: render an unsigned 128-bit value in decimal. */
static void cut_u128_to_str(char * out, size_t out_size, unsigned __int128 v)
{
  char buf[48];
  size_t n = 0, i;
  if (!out_size)
    return;
  if (!v)
    buf[n++] = '0';
  while (v) {
    buf[n++] = (char)('0' + (int)(v % 10));
    v /= 10;
  }
  for (i = 0; i < n && i + 1 < out_size; ++i)
    out[i] = buf[n - 1 - i];
  out[i] = '\0';
}

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

  /* Joint cut-plan mode (see the comment at the top of this file). */
  int cut_plan;                       /* --cut-plan was given */
  char * cut_plan_path;
  unsigned cut_overlap, cut_search;   /* W and S */
  unsigned ncuts;
  cut_interval_t * cuts;

  FILE * tmp;
  uint64_t input_frames;              /* declared frame count of the input */
  uint64_t frames_seen;               /* frames actually received */

  uint64_t out_frames;                /* declared output length in frames */
  uint64_t frames_out;                /* output frames produced so far */
  uint64_t render_gap;                /* next input frame to copy verbatim */
  unsigned render_cut;                /* next cut to be cross-faded */
  unsigned fade_j;                    /* frame index within current fade */
  sox_bool in_fade;                   /* a cross-fade is being emitted */
  sox_bool planned;                   /* planner has run */
  sox_bool drained;                   /* all output has been produced */
  sox_bool failed;                    /* fatal error already reported */
  sox_bool window_accounted;          /* PCM stats include the windows */

  /* Test-only drain pacing, enabled by an environment variable.  Limiting
   * the output capacity is always legal for an effect; it lets the suite
   * check that different drain sizes produce identical PCM. */
  unsigned drain_chunks[8], ndrain_chunks, drain_chunk_i;
  unsigned long delay_ms;

  unsigned channels;
  int * pick;                         /* planner: chosen offsets */
  unsigned __int128 * cost_scratch;   /* planner: cost matrix */
  sox_sample_t * left_buf, * right_buf; /* W frames each, interleaved */
} priv_t;

/* Process-global PCM/temp-file accounting used by the regression tests; it is
 * never read by the effect itself. */
static lsx_splice_cut_plan_stats_t cut_stats;

void lsx_cutplan_stats_reset(void)
{
  memset(&cut_stats, 0, sizeof(cut_stats));
}

void lsx_cutplan_stats_get(lsx_splice_cut_plan_stats_t * stats)
{
  if (stats)
    *stats = cut_stats;
}

void lsx_cutplan_stats_pcm_alloc(size_t samples)
{
  cut_stats.pcm_live_samples += samples;
  cut_stats.pcm_capacity_samples += samples;
  if (cut_stats.pcm_live_samples > cut_stats.pcm_peak_samples)
    cut_stats.pcm_peak_samples = cut_stats.pcm_live_samples;
}

void lsx_cutplan_stats_pcm_free(size_t samples)
{
  cut_stats.pcm_live_samples -= samples;
}

void lsx_cutplan_stats_temp_open(void) { ++cut_stats.temp_files_opened; }
void lsx_cutplan_stats_temp_close(void) { ++cut_stats.temp_files_closed; }
void lsx_cutplan_stats_temp_written(uint64_t bytes)
{ cut_stats.temp_bytes_written += bytes; }
void lsx_cutplan_stats_temp_error(void) { ++cut_stats.temp_io_errors; }

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

/* Plain decimal digits only: no sign, no whitespace, no suffixes. */
static int cut_parse_u64(const char * s, size_t len, uint64_t * value)
{
  uint64_t v = 0;
  size_t i;
  if (!len)
    return 0;
  for (i = 0; i < len; ++i) {
    unsigned digit;
    if (s[i] < '0' || s[i] > '9')
      return 0;
    digit = (unsigned)(s[i] - '0');
    if (v > (UINT64_MAX - digit) / 10)
      return 0;                 /* Would overflow a uint64_t. */
    v = v * 10 + digit;
  }
  *value = v;
  return 1;
}

/* Read one line into a growing buffer; returns 0 at EOF, -1 on error. */
static int cut_read_line(FILE * f, char * * buf, size_t * cap, size_t * len)
{
  size_t n = 0;
  int c;
  while ((c = getc(f)) != EOF) {
    if (c == '\n')
      break;
    if (n + 2 > *cap) {
      size_t ncap = *cap? *cap * 2 : 128;
      char * nb = (char *)realloc(*buf, ncap);
      if (!nb)
        return -1;
      *buf = nb;
      *cap = ncap;
    }
    (*buf)[n++] = (char)c;
  }
  if (ferror(f))
    return -1;
  if (c == EOF && n == 0)
    return 0;
  if (n + 1 > *cap) {
    char * nb = (char *)realloc(*buf, n + 1);
    if (!nb)
      return -1;
    *buf = nb;
    *cap = n + 1;
  }
  (*buf)[n] = '\0';
  *len = n;
  return 1;
}

/*
 * Parse the cut table.  Each non-blank, non-comment line must consist of
 * exactly two decimal unsigned integers: the half-open interval [a,b).  The
 * intervals must be non-empty, strictly increasing by start, and
 * non-overlapping.
 */
static int cut_plan_load(sox_effect_t * effp, const char * path,
    cut_interval_t * * out_cuts, unsigned * out_ncuts)
{
  FILE * f;
  char * line = NULL;
  size_t cap = 0, len;
  cut_interval_t cuts[CUT_PLAN_MAX_CUTS];
  unsigned n = 0;
  unsigned lineno = 0;
  int status;
  int err = SOX_SUCCESS;

  f = fopen(path, "rb");
  if (!f) {
    lsx_fail("can't open cut plan `%s': %s", path, strerror(errno));
    return SOX_EOF;
  }
  status = 0;
  while ((status = cut_read_line(f, &line, &cap, &len)) > 0) {
    char * tok[2];
    size_t toklen[2];
    size_t i = 0, ntok = 0;
    ++lineno;
    /* Split on ASCII white space. */
    for (i = 0; i < len && isspace((unsigned char)line[i]); ++i)
      ;                               /* Leading white space. */
    if (i == len)
      continue;                       /* Blank line. */
    if (line[i] == '#')
      continue;                       /* Whole-line comment. */
    ntok = 0;
    while (i < len) {
      size_t start;
      while (i < len && isspace((unsigned char)line[i]))
        ++i;
      if (i >= len)
        break;
      start = i;
      while (i < len && !isspace((unsigned char)line[i]))
        ++i;
      if (ntok == 2) {
        ntok = 3;                     /* Too many fields. */
        break;
      }
      tok[ntok] = line + start;
      toklen[ntok] = i - start;
      ++ntok;
    }
    if (ntok != 2) {
      lsx_fail("cut plan `%s' line %u: expected exactly two fields",
          path, lineno);
      err = SOX_EOF;
      break;
    }
    if (n == CUT_PLAN_MAX_CUTS) {
      lsx_fail("cut plan `%s' line %u: more than %u intervals",
          path, lineno, CUT_PLAN_MAX_CUTS);
      err = SOX_EOF;
      break;
    }
    if (!cut_parse_u64(tok[0], toklen[0], &cuts[n].a) ||
        !cut_parse_u64(tok[1], toklen[1], &cuts[n].b)) {
      lsx_fail("cut plan `%s' line %u: fields must be decimal unsigned integers",
          path, lineno);
      err = SOX_EOF;
      break;
    }
    if (cuts[n].a >= cuts[n].b) {
      lsx_fail("cut plan `%s' line %u: empty interval [%" PRIu64 ",%" PRIu64 ")",
          path, lineno, cuts[n].a, cuts[n].b);
      err = SOX_EOF;
      break;
    }
    if (n && cuts[n].a <= cuts[n - 1].a) {
      lsx_fail("cut plan `%s' line %u: start is not strictly increasing",
          path, lineno);
      err = SOX_EOF;
      break;
    }
    if (n && cuts[n].a < cuts[n - 1].b) {
      lsx_fail("cut plan `%s' line %u: intervals overlap", path, lineno);
      err = SOX_EOF;
      break;
    }
    ++n;
  }
  if (err == SOX_SUCCESS && status < 0) {
    lsx_fail("cut plan `%s': error reading file: %s", path, strerror(errno));
    err = SOX_EOF;
  }
  free(line);
  if (fclose(f) != 0 && err == SOX_SUCCESS) {
    lsx_fail("cut plan `%s': error closing file: %s", path, strerror(errno));
    err = SOX_EOF;
  }
  if (err != SOX_SUCCESS)
    return err;

  *out_cuts = lsx_calloc(n, sizeof(**out_cuts));
  if (n)
    memcpy(*out_cuts, cuts, n * sizeof(*cuts));
  *out_ncuts = n;
  return SOX_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Cut-plan option parsing                                             */
/* ------------------------------------------------------------------ */

static int cut_parse_uint_opt(const char * s, unsigned lo, unsigned hi,
    unsigned * out, const char * what)
{
  uint64_t v;
  size_t len = strlen(s);
  if (!cut_parse_u64(s, len, &v) || v < lo || v > hi) {
    lsx_fail("%s must be an integer in %u..%u (got `%s')", what, lo, hi, s);
    return SOX_EOF;
  }
  *out = (unsigned)v;
  return SOX_SUCCESS;
}

/* Parse `--cut-plan FILE --overlap W --search S', in any order. */
static int cut_plan_getopts(sox_effect_t * effp, int argc, char * * argv)
{
  priv_t * p = (priv_t *)effp->priv;
  int i;
  int have_plan = 0, have_w = 0, have_s = 0;

  for (i = 0; i < argc; ++i) {
    if (!strcmp(argv[i], "--cut-plan")) {
      if (have_plan || i + 1 >= argc)
        return lsx_usage(effp);
      p->cut_plan_path = lsx_strdup(argv[++i]);
      have_plan = 1;
    }
    else if (!strcmp(argv[i], "--overlap")) {
      if (have_w || i + 1 >= argc ||
          cut_parse_uint_opt(argv[++i], 1, CUT_PLAN_MAX_W, &p->cut_overlap,
              "--overlap") != SOX_SUCCESS)
        return SOX_EOF;
      have_w = 1;
    }
    else if (!strcmp(argv[i], "--search")) {
      if (have_s || i + 1 >= argc ||
          cut_parse_uint_opt(argv[++i], 0, CUT_PLAN_MAX_S, &p->cut_search,
              "--search") != SOX_SUCCESS)
        return SOX_EOF;
      have_s = 1;
    }
    else
      return lsx_usage(effp);
  }
  if (!have_plan || !have_w || !have_s)
    return lsx_usage(effp);
  p->cut_plan = 1;
  return SOX_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Cut-plan temporary PCM store                                        */
/* ------------------------------------------------------------------ */

static int cut_tmp_write(sox_effect_t * effp, const sox_sample_t * buf,
    size_t samples)
{
  priv_t * p = (priv_t *)effp->priv;
  if (!samples)
    return SOX_SUCCESS;
  if (fwrite(buf, sizeof(*buf), samples, p->tmp) != samples) {
    lsx_fail("error writing temporary file: %s", strerror(errno));
    lsx_cutplan_stats_temp_error();
    return SOX_EOF;
  }
  lsx_cutplan_stats_temp_written((uint64_t)samples * sizeof(*buf));
  return SOX_SUCCESS;
}

/* Read exactly `samples' interleaved samples at input frame `frame'. */
static int cut_tmp_read(sox_effect_t * effp, uint64_t frame, size_t samples,
    sox_sample_t * buf)
{
  priv_t * p = (priv_t *)effp->priv;
  off_t off;
  uint64_t max_frame = (uint64_t)INT64_MAX /
      ((uint64_t)p->channels * sizeof(sox_sample_t));
  if (frame > max_frame) {
    lsx_fail("temporary file offset for frame %" PRIu64 " is out of range",
        frame);
    lsx_cutplan_stats_temp_error();
    return SOX_EOF;
  }
  off = (off_t)(frame * p->channels * sizeof(sox_sample_t));
  if (fseeko(p->tmp, off, SEEK_SET) != 0) {
    lsx_fail("error seeking temporary file: %s", strerror(errno));
    lsx_cutplan_stats_temp_error();
    return SOX_EOF;
  }
  if (!samples)
    return SOX_SUCCESS;
  if (fread(buf, sizeof(*buf), samples, p->tmp) != samples) {
    lsx_fail("short read from temporary file at frame %" PRIu64 ": %s",
        frame, strerror(errno));
    lsx_cutplan_stats_temp_error();
    return SOX_EOF;
  }
  return SOX_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Cut-plan lifecycle                                                  */
/* ------------------------------------------------------------------ */

/* Free everything the cut-plan mode allocated, including the temporary
 * file.  Safe to call more than once, and from error paths. */
static void cut_plan_release(priv_t * p)
{
  free(p->left_buf);
  free(p->right_buf);
  free(p->pick);
  free(p->cost_scratch);
  p->left_buf = p->right_buf = NULL;
  p->pick = NULL;
  p->cost_scratch = NULL;
  if (p->tmp) {
    fclose(p->tmp);
    p->tmp = NULL;
    lsx_cutplan_stats_temp_close();
  }
  if (p->window_accounted) {
    lsx_cutplan_stats_pcm_free(2 * (size_t)p->cut_overlap * p->channels);
    p->window_accounted = sox_false;
  }
}

static unsigned cut_parse_chunk_list(const char * text, unsigned * out,
    unsigned cap)
{
  unsigned n = 0;
  while (text && *text && n < cap) {
    char * end;
    unsigned long v = strtoul(text, &end, 10);
    if (end == text)
      break;
    out[n++] = (unsigned)min(v, 0x7ffffffful);
    text = *end == ','? end + 1 : end;
    if (!*text)
      break;
  }
  return n;
}

static int cut_plan_start(sox_effect_t * effp)
{
  priv_t * p = (priv_t *)effp->priv;
  unsigned i;
  uint64_t removed = 0;
  uint64_t chans = effp->in_signal.channels;
  size_t window_samples;

  if (!p->cut_plan_path) {
    lsx_fail("no cut plan file");
    return SOX_EOF;
  }
  if (effp->in_signal.channels < 1 ||
      effp->in_signal.channels > CUT_PLAN_MAX_CHANS) {
    lsx_fail("cut plan mode supports 1 to %u channels", CUT_PLAN_MAX_CHANS);
    return SOX_EOF;
  }
  if (effp->in_signal.length == SOX_UNKNOWN_LEN ||
      effp->in_signal.length == SOX_UNSPEC ||
      effp->in_signal.length == SOX_IGNORE_LENGTH ||
      effp->in_signal.length % chans != 0) {
    lsx_fail("cut plan mode needs a known input length divisible by the "
        "channel count");
    return SOX_EOF;
  }
  lsx_cutplan_stats_reset();
  p->channels = effp->in_signal.channels;
  p->input_frames = effp->in_signal.length / chans;

  if (cut_plan_load(effp, p->cut_plan_path, &p->cuts, &p->ncuts) != SOX_SUCCESS)
    return SOX_EOF;
  if (p->ncuts == 0) {
    /* Nothing to cut: behave as a transparent pass-through.  (A NULL effect
     * is removed from the chain, so no buffers or temp file are needed.) */
    effp->out_signal.length = effp->in_signal.length;
    return SOX_EFF_NULL;
  }

  /* Validate every interval against the real input; all sums stay within
   * uint64_t because b <= frames. */
  for (i = 0; i < p->ncuts; ++i) {
    if (p->cuts[i].b > p->input_frames) {
      lsx_fail("cut %u [%" PRIu64 ",%" PRIu64 ") is outside the %" PRIu64
          "-frame input", i + 1, p->cuts[i].a, p->cuts[i].b,
          p->input_frames);
      return SOX_EOF;
    }
    removed += p->cuts[i].b - p->cuts[i].a;
    if (removed > p->input_frames) {
      lsx_fail("cut plan removes more frames than the input contains");
      return SOX_EOF;
    }
  }

  /* Each cut needs two non-overlapping W-frame windows.  When adjacent cuts
   * leave an island smaller than 2W, no offset assignment can possibly work,
   * regardless of S; rejecting here keeps the diagnostic precise and the
   * window bound honest. */
  for (i = 1; i < p->ncuts; ++i) {
    uint64_t island = p->cuts[i].a - p->cuts[i - 1].b;
    if ((__int128)island < 2 * (__int128)p->cut_overlap) {
      lsx_fail("cut %u and cut %u leave only %" PRIu64 " frames between "
          "them, fewer than the %u needed by two overlap-%u windows",
          i, i + 1, island, 2 * p->cut_overlap, p->cut_overlap);
      return SOX_EOF;
    }
  }

  /* Refuse the plan before consuming any input if no joint placement is
   * legal: no dropping of a cut, shortening of W or merging is allowed. */
  if (!lsx_cutplan_feasible(p->cuts, p->ncuts, p->input_frames,
                            p->cut_overlap, p->cut_search)) {
    lsx_fail("no legal window placement for the cut plan (overlap %u, "
        "search %u); the plan is rejected rather than dropping or merging "
        "cuts", p->cut_overlap, p->cut_search);
    return SOX_EOF;
  }

  {
    /* The output length is N - removed - ncuts*W.  Compute it in 128 bits
     * so the subtraction can never wrap; a plan that would go negative was
     * already impossible for a feasible window placement. */
    __int128 need = (__int128)removed +
        (__int128)p->ncuts * (__int128)p->cut_overlap;
    if (need > (__int128)p->input_frames) {
      lsx_fail("cut plan removes %" PRIu64 " frames plus %u overlap windows "
          "but the input is only %" PRIu64 " frames", removed, p->ncuts,
          p->input_frames);
      return SOX_EOF;
    }
    p->out_frames = p->input_frames - (uint64_t)need;
  }
  effp->out_signal.length = p->out_frames * chans;

  p->pick = lsx_calloc(p->ncuts? p->ncuts : 1, sizeof(*p->pick));
  p->cost_scratch = lsx_calloc(
      (size_t)(p->ncuts? p->ncuts : 1) * (2 * (size_t)p->cut_search + 1),
      sizeof(*p->cost_scratch));
  window_samples = (size_t)p->cut_overlap * p->channels;
  p->left_buf = lsx_calloc(window_samples, sizeof(*p->left_buf));
  p->right_buf = lsx_calloc(window_samples, sizeof(*p->right_buf));
  p->window_accounted = sox_true;
  lsx_cutplan_stats_pcm_alloc(2 * window_samples);

  {
    const char * dc = getenv("SOX_CUT_PLAN_DRAIN_CHUNK");
    const char * dl = getenv("SOX_CUT_PLAN_TEST_DELAY_MS");
    p->ndrain_chunks = cut_parse_chunk_list(dc, p->drain_chunks, 8);
    p->drain_chunk_i = 0;
    p->delay_ms = dl? (unsigned long)strtoul(dl, NULL, 10) : 0;
  }

  p->tmp = lsx_tmpfile();
  if (!p->tmp) {
    lsx_fail("can't create temporary file: %s", strerror(errno));
    cut_plan_release(p);
    return SOX_EOF;
  }
  lsx_cutplan_stats_temp_open();

  p->frames_seen = 0;
  p->frames_out = 0;
  p->render_gap = 0;
  p->render_cut = 0;
  p->fade_j = 0;
  p->in_fade = sox_false;
  p->planned = sox_false;
  p->drained = sox_false;
  p->failed = sox_false;
  return SOX_SUCCESS;
}

/* Append received PCM to the temporary store.  All input is spooled before
 * any output is produced, so candidate windows can be read at random. */
static int cut_plan_flow(sox_effect_t * effp, const sox_sample_t * ibuf,
    sox_sample_t * obuf, size_t * isamp, size_t * osamp)
{
  priv_t * p = (priv_t *)effp->priv;
  size_t in_samples = *isamp;

  (void)obuf;
  *isamp = *osamp = 0;
  if (p->failed)
    return SOX_EOF;
  if (p->planned) {
    lsx_fail("internal error: input after the cut plan was fixed");
    p->failed = sox_true;
    return SOX_EOF;
  }
  if (in_samples % p->channels != 0) {
    lsx_fail("input did not arrive in whole frames");
    p->failed = sox_true;
    return SOX_EOF;
  }
  if (p->frames_seen + in_samples / p->channels > p->input_frames) {
    lsx_fail("input has more frames than the declared length");
    p->failed = sox_true;
    return SOX_EOF;
  }
  if (p->delay_ms) {
    struct timespec ts;
    ts.tv_sec = p->delay_ms / 1000;
    ts.tv_nsec = (long)(p->delay_ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
  }
  if (cut_tmp_write(effp, ibuf, in_samples) != SOX_SUCCESS) {
    p->failed = sox_true;
    return SOX_EOF;
  }
  p->frames_seen += in_samples / p->channels;
  *isamp = in_samples;
  *osamp = 0;
  return SOX_SUCCESS;
}

/* Read exactly W frames starting at `frame' from the temp store. */
static int cut_prefetch(sox_effect_t * effp, uint64_t frame,
    sox_sample_t * dst)
{
  priv_t * p = (priv_t *)effp->priv;
  size_t samples = (size_t)p->cut_overlap * p->channels;
  if (cut_tmp_read(effp, frame, samples, dst) != SOX_SUCCESS) {
    p->failed = sox_true;
    return SOX_EOF;
  }
  return SOX_SUCCESS;
}

/* Exact cost of cut i shifted by d, summed over all channels and all W
 * positions; computed as a mathematical integer. */
static unsigned __int128 lsx_cut_add_u128(unsigned __int128 a, unsigned __int128 b)
{
  const unsigned __int128 inf = ((((unsigned __int128)-1) >> 2));
  if (a >= inf || b >= inf || a > inf - b)
    return inf;
  return a + b;
}

static unsigned __int128 cut_window_cost(sox_effect_t * effp, unsigned i,
    int d)
{
  priv_t * p = (priv_t *)effp->priv;
  cut_interval_t * cut = &p->cuts[i];
  unsigned __int128 total = 0;
  size_t j, c;

  if (cut_prefetch(effp,
          (uint64_t)((__int128)cut->a + d - (__int128)p->cut_overlap),
          p->left_buf) != SOX_SUCCESS ||
      cut_prefetch(effp, (uint64_t)((__int128)cut->b + d),
          p->right_buf) != SOX_SUCCESS) {
    return ((((unsigned __int128)-1) >> 2));
  }

  for (j = 0; j < p->cut_overlap; ++j)
    for (c = 0; c < p->channels; ++c) {
      int64_t diff = (int64_t)p->left_buf[j * p->channels + c] -
                     (int64_t)p->right_buf[j * p->channels + c];
      uint64_t mag = (uint64_t)(diff < 0? -diff : diff);
      total = lsx_cut_add_u128(total, (unsigned __int128)mag * mag);
    }
  return total;
}

/* Fill the cost matrix and run the joint planner. */
static int cut_plan_plan(sox_effect_t * effp)
{
  priv_t * p = (priv_t *)effp->priv;
  const unsigned __int128 inf = ((((unsigned __int128)-1) >> 2));
  unsigned i;
  int d;
  size_t K = 2 * (size_t)p->cut_search + 1;

  if (fflush(p->tmp) != 0 || ferror(p->tmp)) {
    lsx_fail("error flushing temporary file: %s", strerror(errno));
    lsx_cutplan_stats_temp_error();
    return SOX_EOF;
  }
  for (i = 0; i < p->ncuts; ++i) {
    int lo, hi;
    if (!lsx_cutplan_offset_range(&p->cuts[i], p->input_frames,
            p->cut_overlap, p->cut_search, &lo, &hi)) {
      lsx_fail("no legal window placement for cut %u", i + 1);
      return SOX_EOF;
    }
    for (d = -(int)p->cut_search; d <= (int)p->cut_search; ++d)
      p->cost_scratch[(size_t)i * K + (size_t)(d + (int)p->cut_search)] =
          (d < lo || d > hi)? inf : cut_window_cost(effp, i, d);
    if (p->failed)
      return SOX_EOF;
  }

  {
    unsigned __int128 total_cost;
    if (lsx_cutplan_solve(p->cuts, p->ncuts, p->input_frames,
            p->cut_overlap, p->cut_search, p->cost_scratch, p->pick,
            &total_cost) != 0) {
      lsx_fail("no feasible joint window placement for the cut plan");
      return SOX_EOF;
    }
  }
  p->planned = sox_true;
  return SOX_SUCCESS;
}

/* Cross-fade frame j: (1-j/W)*left + (j/W)*right, SoX rounding/clipping. */
static void cut_fade_frame(sox_effect_t * effp, const sox_sample_t * left,
    const sox_sample_t * right, unsigned j, sox_sample_t * out)
{
  priv_t * p = (priv_t *)effp->priv;
  size_t c;
  for (c = 0; c < p->channels; ++c) {
    double d = ((double)(p->cut_overlap - j) * left[c] +
                (double)j * right[c]) / (double)p->cut_overlap;
    out[c] = SOX_ROUND_CLIP_COUNT(d, effp->clips);
  }
}

/* Emit up to `want' frames.  Returns SOX_EOF on a hard error. */
static int cut_emit(sox_effect_t * effp, sox_sample_t * obuf, size_t want,
    size_t * done_out)
{
  priv_t * p = (priv_t *)effp->priv;
  size_t done = 0;

  while (done < want) {
    if (p->in_fade) {
      size_t n = (size_t)p->cut_overlap - p->fade_j;
      n = min(n, want - done);
      while (n--) {
        cut_fade_frame(effp, p->left_buf + p->fade_j * p->channels,
            p->right_buf + p->fade_j * p->channels, p->fade_j, obuf);
        obuf += p->channels;
        ++p->fade_j;
        ++done;
        if (p->fade_j == p->cut_overlap) {
          p->in_fade = sox_false;
          ++p->render_cut;
          break;
        }
      }
      continue;
    }

    if (p->render_cut >= p->ncuts) {
      /* Tail: copy what is left of the input verbatim. */
      uint64_t left = p->input_frames - p->render_gap;
      size_t n = (size_t)(left < (uint64_t)(want - done)?
          left : (uint64_t)(want - done));
      if (!n)
        break;
      if (cut_tmp_read(effp, p->render_gap, n * p->channels, obuf) !=
          SOX_SUCCESS) {
        p->failed = sox_true;
        return SOX_EOF;
      }
      p->render_gap += n;
      done += n;
      obuf += n * p->channels;
      continue;
    }

    {
      cut_interval_t * cut = &p->cuts[p->render_cut];
      int d = p->pick[p->render_cut];
      uint64_t left_start = (uint64_t)((__int128)cut->a + d -
          (__int128)p->cut_overlap);
      uint64_t right_start = (uint64_t)((__int128)cut->b + d);

      if (p->render_gap < left_start) {
        uint64_t left = left_start - p->render_gap;
        size_t n = (size_t)(left < (uint64_t)(want - done)?
            left : (uint64_t)(want - done));
        if (cut_tmp_read(effp, p->render_gap, n * p->channels, obuf) !=
            SOX_SUCCESS) {
          p->failed = sox_true;
          return SOX_EOF;
        }
        p->render_gap += n;
        done += n;
        obuf += n * p->channels;
        continue;
      }

      /* Load both windows and cross-fade them. */
      if (cut_prefetch(effp, left_start, p->left_buf) != SOX_SUCCESS ||
          cut_prefetch(effp, right_start, p->right_buf) != SOX_SUCCESS)
        return SOX_EOF;
      p->render_gap = right_start + p->cut_overlap;
      p->fade_j = 0;
      p->in_fade = sox_true;

      {
        size_t n = (size_t)p->cut_overlap;
        n = min(n, want - done);
        while (n--) {
          cut_fade_frame(effp, p->left_buf + p->fade_j * p->channels,
              p->right_buf + p->fade_j * p->channels, p->fade_j, obuf);
          obuf += p->channels;
          ++p->fade_j;
          ++done;
        }
        if (p->fade_j == p->cut_overlap) {
          p->in_fade = sox_false;
          ++p->render_cut;
        }
      }
    }
  }

  p->frames_out += done;
  *done_out = done;
  return SOX_SUCCESS;
}

static int cut_plan_drain(sox_effect_t * effp, sox_sample_t * obuf,
    size_t * osamp)
{
  priv_t * p = (priv_t *)effp->priv;
  size_t want = *osamp / p->channels;
  size_t done = 0;

  if (p->ndrain_chunks) {
    size_t cap = p->drain_chunks[p->drain_chunk_i];
    if (cap && want > cap)
      want = cap;
    p->drain_chunk_i = (p->drain_chunk_i + 1) % p->ndrain_chunks;
  }
  *osamp = 0;
  if (p->failed)
    return SOX_EOF;
  if (p->drained)
    return SOX_EOF;                 /* Repeated drains stay silent. */

  if (!p->planned) {
    if (p->frames_seen != p->input_frames) {
      lsx_fail("input ended after %" PRIu64 " frames but %" PRIu64
          " were declared", p->frames_seen, p->input_frames);
      p->failed = sox_true;
      return SOX_EOF;
    }
    if (cut_plan_plan(effp) != SOX_SUCCESS) {
      p->failed = sox_true;
      return SOX_EOF;
    }
  }

  if (p->frames_out < p->out_frames) {
    if (cut_emit(effp, obuf, want, &done) != SOX_SUCCESS)
      return SOX_EOF;
    *osamp = done * p->channels;
  }
  /* Never signal EOF in the same call that produced samples; the host may
   * otherwise drop the tail.  A later call returns the final EOF. */
  if (p->frames_out >= p->out_frames &&
      p->render_cut == p->ncuts && !p->in_fade) {
    p->drained = sox_true;
    if (!done)
      return SOX_EOF;
  }
  return SOX_SUCCESS;
}

static int cut_plan_stop(sox_effect_t * effp)
{
  priv_t * p = (priv_t *)effp->priv;
  int status = SOX_SUCCESS;

  if (p->tmp && fclose(p->tmp) != 0 && !p->failed) {
    lsx_fail("error closing temporary file: %s", strerror(errno));
    lsx_cutplan_stats_temp_error();
    status = SOX_EOF;
  }
  if (p->tmp) {
    p->tmp = NULL;
    lsx_cutplan_stats_temp_close();
  }
  if (p->window_accounted) {
    lsx_cutplan_stats_pcm_free(2 * (size_t)p->cut_overlap * p->channels);
    p->window_accounted = sox_false;
  }
  if (!p->failed && p->planned && p->frames_out != p->out_frames)
    lsx_warn("cut plan stopped before completion: produced %" PRIu64
        " of %" PRIu64 " output frames", p->frames_out, p->out_frames);
  else if (!p->failed && !p->planned)
    lsx_warn("cut plan cancelled: only %" PRIu64 " of %" PRIu64
        " input frames were read", p->frames_seen, p->input_frames);
  {
    /* Observation point used by the regression tests: append the resource
     * counters to the file named by SOX_CUT_PLAN_STATS.  It never affects
     * the produced PCM. */
    char const * stats_path = getenv("SOX_CUT_PLAN_STATS");
    if (stats_path && *stats_path) {
      FILE * sf = fopen(stats_path, "a");
      if (sf) {
        lsx_splice_cut_plan_stats_t st;
        lsx_cutplan_stats_get(&st);
        fprintf(sf, "pcm_peak=%zu pcm_capacity=%zu pcm_live=%zu "
            "temp_bytes=%" PRIu64 " temp_opened=%u temp_closed=%u "
            "temp_errors=%u failed=%d\n",
            st.pcm_peak_samples, st.pcm_capacity_samples,
            st.pcm_live_samples, st.temp_bytes_written,
            st.temp_files_opened, st.temp_files_closed,
            st.temp_io_errors, (int)p->failed);
        fclose(sf);
      }
    }
  }
  return status;
}


static int create(sox_effect_t * effp, int argc, char * * argv)
{
  priv_t * p = (priv_t *)effp->priv;
  int i;
  --argc, ++argv;

  /* The joint cut-plan form is unambiguous: it is the only one that uses
   * long options.  Old positional arguments and -h/-t/-q keep their exact
   * previous behaviour. */
  for (i = 0; i < argc; ++i)
    if (!strncmp(argv[i], "--", 2))
      return cut_plan_getopts(effp, argc, argv);

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

  if (p->cut_plan)
    return cut_plan_start(effp);

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

  if (p->cut_plan)
    return cut_plan_flow(effp, ibuf, obuf, isamp, osamp);

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
  size_t isamp = 0;

  if (p->cut_plan)
    return cut_plan_drain(effp, obuf, osamp);
  return flow(effp, 0, obuf, &isamp, osamp);
}

static int stop(sox_effect_t * effp)
{
  priv_t * p = (priv_t *)effp->priv;
  if (p->cut_plan)
    return cut_plan_stop(effp);
  if (p->splices_pos != p->nsplices)
    lsx_warn("Input audio too short; splices not made: %u", p->nsplices - p->splices_pos);
  free(p->buffer);
  return SOX_SUCCESS;
}

static int lsx_kill(sox_effect_t * effp)
{
  priv_t * p = (priv_t *)effp->priv;
  unsigned i;
  for (i = 0; i < p->nsplices; ++i)
    free(p->splices[i].str);
  free(p->splices);
  free(p->cut_plan_path);
  free(p->cuts);
  cut_plan_release(p);
  return SOX_SUCCESS;
}

sox_effect_handler_t const * lsx_splice_effect_fn(void)
{
  static sox_effect_handler_t handler = {
    "splice", "[-h|-t|-q] {position[,excess[,leeway]]}"
    "\n  -h        Half sine fade (default); constant gain (for correlated audio)"
    "\n  -t        Triangular (linear) fade; constant gain (for correlated audio)"
    "\n  -q        Quarter sine fade; constant power (for correlated audio e.g. x-fade)"
    "\n  position  The length of part 1 (including the excess)"
    "\n  excess    At the end of part 1 & the start of part2 (default 0.005)"
    "\n  leeway    Before part2 (default 0.005; set to 0 for cross-fade)"
    "\n"
    "\n  splice --cut-plan FILE --overlap W --search S"
    "\n            Remove the [a,b) frame intervals listed in FILE jointly:"
    "\n            each cut is replaced by a W-frame triangular cross-fade"
    "\n            whose placement (offset in [-S,S]) is chosen for the"
    "\n            smallest total exact squared-difference cost.",
    SOX_EFF_MCHAN | SOX_EFF_LENGTH,
    create, start, flow, drain, stop, lsx_kill, sizeof(priv_t)
  };
  return &handler;
}
