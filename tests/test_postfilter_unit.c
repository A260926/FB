/* Unit-level content regression for the CELT pitch post-filter.
 *
 * Every expectation comes from tests/postfilter_ref.h, an independent
 * per-sample evaluation of the five-tap delay difference.  comb_filter() is
 * only the device under test.  The in-place and out-of-place entries are each
 * checked against their own reference evaluation; their outputs are never
 * compared with each other as a substitute for correctness.
 *
 * Coverage: shortest legal period, a period next to the block length and one
 * at the block length, near-maximum periods, handoffs between all three
 * tapsets, and positive, negative, and zero gains.  Signals are small
 * asymmetric impulses on a deterministic sequence; no production output is
 * stored as an expectation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "celt/celt.h"
#include "celt/modes.h"
#include "postfilter_ref.h"
#include "celt/postfilter_trace.h"

/* The trace build of the library references this symbol; the unit test does
 * not use the decoder observation point. */
void celt_postfilter_trace_frame(const celt_postfilter_frame *frame)
{
   (void)frame;
}

#define TOL(expected) (4e-5 * (1.0 + fabs((double)(expected))))
#define PREFIX PF_MAX_PERIOD
#define GUARD 64

static const opus_val16 *g_window;
static float g_window_f[PF_OVERLAP_MAX];
static int g_overlap;

static unsigned long g_rng;
static double frand(void)
{
   g_rng = g_rng * 1664525UL + 1013904223UL;
   return (double)((g_rng >> 8) & 0xffffffUL) / 16777216.0 - 0.5;
}

/* Small asymmetric impulses (of alternating sign) on a deterministic
 * sequence.  The DC value is kept near zero so the recursive filter stays
 * bounded for every gain in the sweep. */
static void make_signal(float *hist, float *x, int N, unsigned long seed)
{
   int i;
   double dc = 0.0;
   g_rng = seed;
   for (i = 0; i < PF_MAX_PERIOD; i++) {
      hist[i] = (float)(2.0e-3 * frand() + 3.0e-4 * sin(0.017 * i + 0.3));
      dc += hist[i];
   }
   for (i = 0; i < N; i++)
      x[i] = (float)(2.0e-3 * frand() + 2.0e-4 * sin(0.051 * i));
   /* Remove the residual mean so a near-unity feedback cannot integrate it. */
   for (i = 0; i < PF_MAX_PERIOD; i++)
      hist[i] = (float)(hist[i] - dc / PF_MAX_PERIOD);
   x[0] += 5.0e-3f;
   x[N / 7] -= 8.0e-3f;
   x[N / 3 + 1] += 4.0e-3f;
   x[N / 2 + 2] -= 6.0e-3f;
   if (N > 8) x[N - 3] += 3.0e-3f;
}

typedef struct {
   int N;
   int T0;
   float g0;
   int ts0;
   int T1;
   float g1;
   int ts1;
   int overlap;
   int in_place;
} unit_case;

static long g_compared;
static long g_cases;
static int g_failures;
static long g_divergent_io;
static long g_checked_io;

static void report_fail(const char *fmt, int case_id, int ch, int i,
      double expected, double observed)
{
   if (g_failures < 20)
      fprintf(stderr, "POSTFILTER_UNIT_FAIL case=%d %s channel=%d index=%d expected=%.9g observed=%.9g err=%.3g\n",
            case_id, fmt, ch, i, expected, observed,
            fabs(observed - (double)expected));
   g_failures++;
}

static int equal_region(const char *what, const float *got, const float *want,
      int len, int case_id)
{
   int i;
   for (i = 0; i < len; i++) {
      if (got[i] != want[i] || !isfinite(got[i])) {
         report_fail(what, case_id, 0, i, want[i], got[i]);
         return 1;
      }
   }
   return 0;
}

static void run_case(const unit_case *tc, int case_id)
{
   int total = PREFIX + tc->N + GUARD;
   float *xbuf = (float *)calloc((size_t)total, sizeof(float));
   float *ybuf = (float *)calloc((size_t)total, sizeof(float));
   float *refbuf = (float *)calloc((size_t)total, sizeof(float));
   float *hist = (float *)calloc((size_t)PF_MAX_PERIOD, sizeof(float));
   float *input = (float *)calloc((size_t)tc->N, sizeof(float));
   float *guard = (float *)calloc((size_t)GUARD, sizeof(float));
   float *snap = (float *)calloc((size_t)total, sizeof(float));
   float *scratch = (float *)calloc((size_t)total, sizeof(float));
   pf_hist hstore;
   pf_gen ga, gb;
   int fade, i, fail = 0;

   if (!xbuf || !ybuf || !refbuf || !hist || !input || !guard || !snap || !scratch) {
      fprintf(stderr, "POSTFILTER_UNIT_FAIL allocation\n");
      g_failures++;
      goto done;
   }

   make_signal(hist, input, tc->N, 0x9e3779b9UL ^ (unsigned long)case_id);
   for (i = 0; i < GUARD; i++)
      guard[i] = (float)(0.005 + 0.0001 * i + 1.0e-6 * case_id);

   /* Input array with a history prefix (so the out-of-place delay taps are
    * defined) and a guard tail. */
   memcpy(xbuf, hist, (size_t)PF_MAX_PERIOD * sizeof(float));
   memcpy(xbuf + PREFIX, input, (size_t)tc->N * sizeof(float));
   for (i = 0; i < GUARD; i++) xbuf[PREFIX + tc->N + i] = guard[i];
   memcpy(snap, xbuf, (size_t)total * sizeof(float));

   /* Output array starts as a copy of the input; for the in-place entry the
    * device under test reads and writes this same array. */
   memcpy(ybuf, xbuf, (size_t)total * sizeof(float));

   /* Reference generations.  Production clamps the period before use. */
   ga.period = tc->T0 < PF_MIN_PERIOD ? PF_MIN_PERIOD : tc->T0;
   ga.gain = tc->g0;
   ga.tapset = tc->ts0;
   gb.period = tc->T1 < PF_MIN_PERIOD ? PF_MIN_PERIOD : tc->T1;
   gb.gain = tc->g1;
   gb.tapset = tc->ts1;

   /* Independent reference: yref begins as a copy of the pristine input, then
    * the recurrence is evaluated one sample at a time. */
   memcpy(refbuf, xbuf, (size_t)total * sizeof(float));
   hstore.data = hist;
   hstore.len = PF_MAX_PERIOD;
   fade = pf_fade_len(&ga, &gb, tc->overlap, tc->N);
   pf_segment(&hstore, xbuf + PREFIX, refbuf + PREFIX, tc->in_place, 0, tc->N,
         &ga, &gb, g_window_f, fade);

   /* Device under test on the float arrays. */
   if (tc->in_place)
      comb_filter((opus_val32 *)(ybuf + PREFIX), (opus_val32 *)(ybuf + PREFIX),
            tc->T0, tc->T1, tc->N, (opus_val16)tc->g0, (opus_val16)tc->g1,
            tc->ts0, tc->ts1, g_window, tc->overlap, 0);
   else
      comb_filter((opus_val32 *)(ybuf + PREFIX), (opus_val32 *)(xbuf + PREFIX),
            tc->T0, tc->T1, tc->N, (opus_val16)tc->g0, (opus_val16)tc->g1,
            tc->ts0, tc->ts1, g_window, tc->overlap, 0);

   /* Element-by-element comparison at the stated tolerance, plus finiteness. */
   for (i = 0; i < tc->N; i++) {
      float expected = refbuf[PREFIX + i];
      float observed = ybuf[PREFIX + i];
      g_compared++;
      if (!isfinite(observed)) {
         report_fail("non-finite output", case_id, 0, i, expected, observed);
         fail = 1;
         break;
      }
      if (fabs((double)observed - (double)expected) > TOL(expected)) {
         report_fail("value", case_id, 0, i, expected, observed);
         fail = 1;
         break;
      }
   }

   if (!fail) {
      /* Read-only protection.  The history prefix of the output must never be
       * written, and the guard tail must be untouched.  For the out-of-place
       * entry the input array (prefix, frame and tail) must be untouched. */
      if (equal_region("output history prefix", ybuf, hist, PREFIX, case_id))
         fail = 1;
      if (equal_region("output guard tail", ybuf + PREFIX + tc->N, guard, GUARD, case_id))
         fail = 1;
      if (!tc->in_place && equal_region("input unchanged", xbuf, snap, total, case_id))
         fail = 1;
   }

   /* Sanity: the two entries are not interchangeable.  Run the in-place form
    * on a copy and record whether it differed from the out-of-place result.
    * This is reported only; it never substitutes for the reference check. */
   if (!tc->in_place) {
      memcpy(scratch, xbuf, (size_t)total * sizeof(float));
      comb_filter((opus_val32 *)(scratch + PREFIX), (opus_val32 *)(scratch + PREFIX),
            tc->T0, tc->T1, tc->N, (opus_val16)tc->g0, (opus_val16)tc->g1,
            tc->ts0, tc->ts1, g_window, tc->overlap, 0);
      {
         int differs = 0;
         for (i = 0; i < tc->N; i++) {
            if (fabs((double)scratch[PREFIX + i] - (double)ybuf[PREFIX + i]) > 1e-9) {
               differs = 1;
               break;
            }
         }
         g_checked_io++;
         g_divergent_io += differs;
      }
   }

   g_cases++;
   if (g_cases <= 6 || (case_id % 1500) == 0) {
      printf("POSTFILTER_UNIT_CASE case=%d N=%d T=%d,%d g=%.4f,%.4f ts=%d,%d ov=%d inplace=%d fade=%d\n",
            case_id, tc->N, tc->T0, tc->T1, tc->g0, tc->g1, tc->ts0, tc->ts1,
            tc->overlap, tc->in_place, fade);
   }

done:
   free(xbuf); free(ybuf); free(refbuf); free(hist); free(input);
   free(guard); free(snap); free(scratch);
}

int main(void)
{
   static const float gains[] = {0.0f, 0.25f, -0.25f, 0.65f, -0.65f};
   static const int n_list[] = {120, 240, 960};
   CELTMode *mode;
   int err, i;
   int ni, gi, gj, ti, tj, pi, pj, ii, oi, case_id = 1;
   int periods[4];

   mode = opus_custom_mode_create(48000, 960, &err);
   if (mode == NULL || err != OPUS_OK) {
      fprintf(stderr, "POSTFILTER_UNIT_FAIL mode create\n");
      return 1;
   }
   g_window = mode->window;
   g_overlap = mode->overlap;
   if (g_overlap != PF_OVERLAP_MAX) {
      fprintf(stderr, "POSTFILTER_UNIT_FAIL unexpected overlap %d\n", g_overlap);
      return 1;
   }
   for (i = 0; i < g_overlap; i++) g_window_f[i] = (float)g_window[i];

   printf("POSTFILTER_UNIT_START overlap=%d min_period=%d max_period=%d\n",
         g_overlap, PF_MIN_PERIOD, PF_MAX_PERIOD);

   /* Full sweep: lengths, generation handoffs across all tapsets, gains of
    * both signs and zero, periods at the legal minimum, adjacent to and at
    * the block length, and near the maximum, in both entry modes. */
   for (ni = 0; ni < 3; ni++) {
      int N = n_list[ni];
      periods[0] = PF_MIN_PERIOD;
      periods[1] = N - 1;
      periods[2] = N;
      periods[3] = 1022;
      for (gi = 0; gi < 5; gi++) {
         for (gj = 0; gj < 5; gj++) {
            for (ti = 0; ti < 3; ti++) {
               for (tj = 0; tj < 3; tj++) {
                  for (pi = 0; pi < 4; pi++) {
                     for (pj = 0; pj < 4; pj++) {
                        for (ii = 0; ii < 2; ii++) {
                           unit_case tc;
                           tc.N = N;
                           tc.T0 = periods[pi];
                           tc.T1 = periods[pj];
                           tc.g0 = gains[gi];
                           tc.g1 = gains[gj];
                           tc.ts0 = ti;
                           tc.ts1 = tj;
                           tc.overlap = g_overlap;
                           tc.in_place = ii;
                           run_case(&tc, case_id++);
                        }
                     }
                  }
               }
            }
         }
      }
   }

   /* Unchanged parameters: production collapses the fade and applies one
    * constant filter from the first sample. */
   for (ni = 0; ni < 3; ni++) {
      int N = n_list[ni];
      for (gi = 1; gi < 5; gi++) {
         for (oi = 0; oi < 2; oi++) {
            unit_case tc;
            tc.N = N;
            tc.T0 = 37; tc.T1 = 37;
            tc.g0 = gains[gi]; tc.g1 = gains[gi];
            tc.ts0 = gi % 3; tc.ts1 = gi % 3;
            tc.overlap = g_overlap;
            tc.in_place = oi;
            run_case(&tc, case_id++);
         }
      }
   }

   /* Zero-gain paths: both zero (copy), old only, new only. */
   for (ni = 0; ni < 3; ni++) {
      int N = n_list[ni];
      for (oi = 0; oi < 2; oi++) {
         unit_case tc;
         tc.N = N; tc.T0 = PF_MIN_PERIOD; tc.T1 = 41;
         tc.ts0 = 0; tc.ts1 = 2; tc.overlap = g_overlap; tc.in_place = oi;
         tc.g0 = 0.0f; tc.g1 = 0.0f; run_case(&tc, case_id++);
         tc.g0 = 0.0f; tc.g1 = 0.45f; run_case(&tc, case_id++);
         tc.g0 = -0.45f; tc.g1 = 0.0f; run_case(&tc, case_id++);
      }
   }

   /* Zero and partial overlap. */
   for (ni = 0; ni < 3; ni++) {
      int N = n_list[ni];
      for (oi = 0; oi < 2; oi++) {
         unit_case tc;
         tc.N = N; tc.T0 = 61; tc.T1 = 223;
         tc.g0 = 0.3f; tc.g1 = 0.65f; tc.ts0 = 1; tc.ts1 = 2;
         tc.in_place = oi;
         tc.overlap = 0; run_case(&tc, case_id++);
         tc.overlap = g_overlap / 3; run_case(&tc, case_id++);
      }
   }

   printf("POSTFILTER_UNIT_SUMMARY cases=%ld compared=%ld inplace_outofplace_divergent=%ld/%ld failures=%d\n",
         g_cases, g_compared, g_divergent_io, g_checked_io, g_failures);
   if (g_failures != 0) {
      printf("POSTFILTER_UNIT_FAIL failures=%d\n", g_failures);
      return 1;
   }
   if (g_divergent_io == 0) {
      printf("POSTFILTER_UNIT_FAIL no in-place/out-of-place divergence observed\n");
      return 1;
   }
   printf("POSTFILTER_UNIT_PASS\n");
   return 0;
}
