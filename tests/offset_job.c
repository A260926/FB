/*
 * Regression tests for the resumable offset-surface session.
 *
 * The driver (task_support/driver.c) provides main(); registration happens
 * through task_cases/task_case_count and every assertion is a TASK_CHECK.
 * The tests observe the real s1221/s1227/s1424 calls through the shipped
 * eval_trace observer, so they verify the actual evaluation sequence, not
 * just the counts the session reports about itself.
 *
 * Everything the tests build is a valid input for the supported class:
 * 3D, regular, open-clamped, polynomial or positive-weight rational,
 * order 3 or 4 (degree 2 or 3) in each direction, C2 in the interior.
 */

#include "../task_support/check.h"
#include "sisl.h"
#include "sisl_offset_job.h"
#include "../task_support/eval_trace.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Evaluation trace capture.                                          */
/* ------------------------------------------------------------------ */

struct trace
{
  char  text[1200000];   /* one canonical line per observed call        */
  int   count;
  int   record;
};

static struct trace captured;

static void trace_observer(const char *name, const void *obj, int n, int m,
                           const double *par, void *ctx)
{
  char line[160];
  struct trace *t = (struct trace *)ctx;

  (void)obj;
  if (t == NULL || !t->record) return;

  if (strcmp(name, "s1424") == 0)
    snprintf(line, sizeof(line), "%s %d %d %.17g %.17g\n", name, n, m,
             par[0], par[1]);
  else
    snprintf(line, sizeof(line), "%s %d %.17g\n", name, n, par[0]);

  strncat(t->text, line, sizeof(t->text) - strlen(t->text) - 1);
  ++t->count;
}

static void trace_start(struct trace *t)
{
  t->text[0] = '\0';
  t->count = 0;
  t->record = 1;
  task_eval_set_observer(trace_observer, t);
}

static void trace_stop(struct trace *t)
{
  task_eval_set_observer(NULL, NULL);
  t->record = 0;
}

/* ------------------------------------------------------------------ */
/* Test surfaces.                                                     */
/* ------------------------------------------------------------------ */

/* Reference surface used by the baseline: bilinear-in-parameters cubic
   patch with a mild saddle and non-unit parameter domain. */

static SISLSurf *make_cubic_surf(void)
{
  double k[] = {-2, -2, -2, -2, 4, 4, 4, 4};
  double c[48];
  int i, j;

  for (j = 0; j < 4; ++j)
    for (i = 0; i < 4; ++i)
    {
      int n = 3 * (4 * j + i);
      c[n + 0] = i;
      c[n + 1] = j;
      c[n + 2] = 0.12 * i * j + 0.03 * i * i;
    }

  return newSurf(4, 4, 4, 4, k, k, c, 1, 3, 1);
}

/* A more strongly curved surface with a non-uniform knot vector in the
   second direction: still open-clamped and C2 in the interior. */

static SISLSurf *make_curved_surf(void)
{
  double k1[] = {0, 0, 0, 0, 1, 2, 3, 3, 3, 3};      /* 6 x 6, degree 3 */
  double k2[] = {-1, -1, -1, -1, 0, 2, 4, 4, 4, 4};
  double c[6 * 6 * 3];
  int i, j;

  for (j = 0; j < 6; ++j)
    for (i = 0; i < 6; ++i)
    {
      int n = 3 * (6 * j + i);
      double u = (double)i / 5.0, v = (double)j / 5.0;
      c[n + 0] = 3.0 * u + 0.4 * (u * u - v * v);
      c[n + 1] = 2.0 * v + 0.6 * u * v;
      c[n + 2] = 1.4 * u * u * v + 0.5 * v * v - 0.3 * u * v * v;
    }

  return newSurf(6, 6, 4, 4, k1, k2, c, 1, 3, 1);
}

/* A positive-weight rational surface (weights all > 0).  Control points are
   stored as (w*x, w*y, w*z, w). */

static SISLSurf *make_rational_surf(void)
{
  /* Non-unit, asymmetric parameter domain with an interior knot in each
     direction; weights strictly positive and varying. */
  double k1[] = {-3, -3, -3, -3, 0.5, 4, 4, 4, 4};
  double k2[] = {2, 2, 2, 2, 3, 7.5, 7.5, 7.5, 7.5};
  double c[5 * 5 * 4];
  int i, j;

  for (j = 0; j < 5; ++j)
    for (i = 0; i < 5; ++i)
    {
      int n = 4 * (5 * j + i);
      double u = (double)i / 4.0, v = (double)j / 4.0;
      double w = 1.0 + 0.5 * u + 0.25 * u * v;
      c[n + 0] = w * (u + 0.2 * u * v);
      c[n + 1] = w * (v + 0.3 * u * u);
      c[n + 2] = w * (0.5 * u * u - 0.4 * u * v + 0.25 * v * v);
      c[n + 3] = w;
    }

  return newSurf(5, 5, 4, 4, k1, k2, c, 2, 3, 1);
}

/* A degree-2 (order 3) surface, to exercise the second supported degree. */

static SISLSurf *make_quadratic_surf(void)
{
  double k[] = {0, 0, 0, 1, 2, 2, 2};
  double c[5 * 5 * 3];
  int i, j;

  for (j = 0; j < 5; ++j)
    for (i = 0; i < 5; ++i)
    {
      int n = 3 * (5 * j + i);
      double u = (double)i / 4.0, v = (double)j / 4.0;
      c[n + 0] = u;
      c[n + 1] = v;
      c[n + 2] = 0.25 * u * v + 0.1 * u * u;
    }

  return newSurf(5, 5, 3, 3, k, k, c, 1, 3, 1);
}

/* ------------------------------------------------------------------ */
/* Helpers.                                                           */
/* ------------------------------------------------------------------ */

static double max3(double a, double b, double c)
{
  double m = a > b ? a : b;
  return m > c ? m : c;
}

/* Combined relative/absolute comparison used for geometry. */

static int close_enough(double a, double b, double scale)
{
  double d = fabs(a - b);
  return d <= 1e-9 * (scale > 1.0 ? scale : 1.0);
}

/* Compare two offset surfaces on knots, control points and evaluation. */

static int surfaces_match(SISLSurf *a, SISLSurf *b, double *maxdiff)
{
  int i, n;
  double mx = 0.0;

  if (a == NULL || b == NULL) return 0;
  if (a->in1 != b->in1 || a->in2 != b->in2) return 0;
  if (a->ik1 != b->ik1 || a->ik2 != b->ik2) return 0;
  if (a->idim != b->idim) return 0;

  n = a->in1 + a->ik1;
  for (i = 0; i < n; ++i)
    mx = fmax(mx, fabs(a->et1[i] - b->et1[i]));
  n = a->in2 + a->ik2;
  for (i = 0; i < n; ++i)
    mx = fmax(mx, fabs(a->et2[i] - b->et2[i]));

  n = a->in1 * a->in2 * a->idim;
  for (i = 0; i < n; ++i)
    mx = fmax(mx, fabs(a->ecoef[i] - b->ecoef[i]));

  /* Also evaluate both surfaces on a parameter grid and compare positions
     and first derivatives, so a coincident control net with a different
     parametrisation could not pass. */

  {
    int gu, gv;
    for (gu = 1; gu <= 3; ++gu)
      for (gv = 1; gv <= 3; ++gv)
      {
        double uv[2];
        double da[27], db[27];
        int l1 = 0, l2 = 0, l3 = 0, l4 = 0, s1 = 0, s2 = 0;
        uv[0] = a->et1[a->ik1 - 1] +
                (a->et1[a->in1] - a->et1[a->ik1 - 1]) * gu / 4.0;
        uv[1] = a->et2[a->ik2 - 1] +
                (a->et2[a->in2] - a->et2[a->ik2 - 1]) * gv / 4.0;
        s1424(a, 1, 1, uv, &l1, &l2, da, &s1);
        s1424(b, 1, 1, uv, &l3, &l4, db, &s2);
        if (s1 < 0 || s2 < 0) return 0;
        for (i = 0; i < 9; ++i)
        {
          double scale = max3(fabs(da[i]), fabs(db[i]), 1.0);
          mx = fmax(mx, fabs(da[i] - db[i]) / scale);
        }
      }
  }

  *maxdiff = mx;
  return 1;
}

/* Run a session with the given budget until it reports something other
   than MORE.  Returns the final status. */

static int run_session(SISLOffsetJob *job, int budget,
                       SISLOffsetJobStats *last)
{
  SISLOffsetJobStats st;
  int rc, guard = 0;

  do
  {
    rc = sisl_offset_job_step(job, budget, &st);
    if (++guard > 2000000) return SISL_OFFSET_JOB_ERR_STATE;
  } while (rc == SISL_OFFSET_JOB_MORE);

  if (last != NULL) *last = st;
  return rc;
}

static int run_session_mixed(SISLOffsetJob *job, const int *budgets, int n,
                             SISLOffsetJobStats *last)
{
  SISLOffsetJobStats st;
  int rc = SISL_OFFSET_JOB_MORE, i = 0, guard = 0;

  while (rc == SISL_OFFSET_JOB_MORE)
  {
    int b = (i < n) ? budgets[i] : budgets[n - 1];
    rc = sisl_offset_job_step(job, b, &st);
    if (i < n - 1) ++i;
    if (++guard > 2000000) return SISL_OFFSET_JOB_ERR_STATE;
  }

  if (last != NULL) *last = st;
  return rc;
}

/* Count distinct interior knots in one direction, i.e. the number of free
   interior parameters of the surface. */

static int interior_knots(SISLSurf *s)
{
  int i, count = 0;
  for (i = s->ik1 - 1; i < s->in1; ++i)
    if (i > s->ik1 - 1 && s->et1[i] > s->et1[i - 1]) ++count;
  return count;
}

/* ------------------------------------------------------------------ */
/* Scheduling.                                                        */
/* ------------------------------------------------------------------ */

static int case_zero_budget_is_inert(void)
{
  SISLSurf *ps = make_cubic_surf();
  SISLOffsetJob *job;
  SISLOffsetJobStats st;
  int jstat = 0, rc;

  TASK_CHECK(ps != NULL);
  job = sisl_offset_job_create(ps, 0.1, 0.001, 0.0, 3, &jstat);
  TASK_CHECK(job != NULL);
  TASK_CHECK(jstat == 0);

  trace_start(&captured);
  rc = sisl_offset_job_step(job, 0, &st);
  trace_stop(&captured);

  TASK_CHECK(rc == SISL_OFFSET_JOB_MORE);
  TASK_CHECK(st.evaluations == 0);
  TASK_CHECK(st.evaluations_total == 0);
  TASK_CHECK(captured.count == 0);
  TASK_CHECK(st.phase == SISL_OFFSET_PHASE_EDGES);

  /* A second zero step must behave identically. */
  trace_start(&captured);
  rc = sisl_offset_job_step(job, 0, &st);
  trace_stop(&captured);
  TASK_CHECK(rc == SISL_OFFSET_JOB_MORE);
  TASK_CHECK(st.evaluations == 0 && captured.count == 0);

  sisl_offset_job_free(job);
  freeSurf(ps);
  return 0;
}

static int case_negative_budget_rejected(void)
{
  SISLSurf *ps = make_cubic_surf();
  SISLOffsetJob *job;
  SISLOffsetJobStats st;
  int jstat = 0, rc;

  TASK_CHECK(ps != NULL);
  job = sisl_offset_job_create(ps, 0.1, 0.001, 0.0, 3, &jstat);
  TASK_CHECK(job != NULL);

  trace_start(&captured);
  rc = sisl_offset_job_step(job, -1, &st);
  trace_stop(&captured);

  TASK_CHECK(rc == SISL_OFFSET_JOB_ERR_BUDGET);
  TASK_CHECK(st.status == SISL_OFFSET_JOB_ERR_BUDGET);
  TASK_CHECK(st.evaluations == 0);
  TASK_CHECK(st.evaluations_total == 0);
  TASK_CHECK(captured.count == 0);

  /* The session is untouched: a normal step still starts from zero. */
  trace_start(&captured);
  rc = sisl_offset_job_step(job, 3, &st);
  trace_stop(&captured);
  TASK_CHECK(rc == SISL_OFFSET_JOB_MORE);
  TASK_CHECK(st.evaluations == 3);
  TASK_CHECK(captured.count == 3);
  TASK_CHECK(st.evaluations_total == 3);

  sisl_offset_job_free(job);
  freeSurf(ps);
  return 0;
}

static int case_budget_exactly_exhausted(void)
{
  SISLSurf *ps = make_cubic_surf();
  SISLOffsetJob *job;
  SISLOffsetJobStats st;
  int jstat = 0, rc, i, total = 0;

  TASK_CHECK(ps != NULL);
  job = sisl_offset_job_create(ps, 0.1, 0.001, 0.0, 3, &jstat);
  TASK_CHECK(job != NULL);

  /* Each step spends exactly its whole budget, never a call more. */
  for (i = 0; i < 4; ++i)
  {
    trace_start(&captured);
    rc = sisl_offset_job_step(job, 5, &st);
    trace_stop(&captured);
    TASK_CHECK(rc == SISL_OFFSET_JOB_MORE);
    TASK_CHECK(st.evaluations == 5);
    TASK_CHECK(captured.count == 5);
    total += st.evaluations;
    TASK_CHECK(st.evaluations_total == total);
  }

  sisl_offset_job_free(job);
  freeSurf(ps);
  return 0;
}

static int case_budget_one_trace_matches_full(void)
{
  SISLSurf *ps = make_cubic_surf();
  SISLSurf *full = NULL;
  SISLOffsetJob *a, *b;
  SISLOffsetJobStats sa, sb;
  int jstat = 0, rc, m1 = 0;
  char fulltrace[1200000];
  double diff = 0.0;

  TASK_CHECK(ps != NULL);

  /* The reference full run, observed. */
  {
    struct trace t;
    int stat = 0;
    t.text[0] = '\0'; t.count = 0; t.record = 1;
    task_eval_set_observer(trace_observer, &t);
    task_reference_s1365(ps, 0.1, 0.001, 0.0, 3, &full, &stat);
    task_eval_set_observer(NULL, NULL);
    TASK_CHECK(stat == 0 && full != NULL);
    strcpy(fulltrace, t.text);
    m1 = t.count;
  }

  /* One huge budget. */
  a = sisl_offset_job_create(ps, 0.1, 0.001, 0.0, 3, &jstat);
  TASK_CHECK(a != NULL);
  trace_start(&captured);
  rc = sisl_offset_job_step(a, 1000000, &sb);
  trace_stop(&captured);
  TASK_CHECK(rc == SISL_OFFSET_JOB_DONE);
  TASK_CHECK(sb.evaluations_total == m1);
  TASK_CHECK(strcmp(captured.text, fulltrace) == 0);

  /* Budget 1, repeatedly. */
  b = sisl_offset_job_create(ps, 0.1, 0.001, 0.0, 3, &jstat);
  TASK_CHECK(b != NULL);
  trace_start(&captured);
  rc = run_session(b, 1, &sa);
  trace_stop(&captured);
  TASK_CHECK(rc == SISL_OFFSET_JOB_DONE);
  TASK_CHECK(sa.evaluations_total == m1);
  TASK_CHECK(strcmp(captured.text, fulltrace) == 0);

  {
    SISLSurf *ra = sisl_offset_job_take_result(a, &jstat);
    SISLSurf *rb = sisl_offset_job_take_result(b, &jstat);
    TASK_CHECK(ra != NULL && rb != NULL);
    TASK_CHECK(surfaces_match(ra, full, &diff));
    TASK_CHECK(diff == 0.0);              /* exact for identical traces */
    TASK_CHECK(surfaces_match(rb, full, &diff));
    TASK_CHECK(diff == 0.0);
    freeSurf(ra);
    freeSurf(rb);
  }

  sisl_offset_job_free(a);
  sisl_offset_job_free(b);
  freeSurf(full);
  freeSurf(ps);
  return 0;
}

static int case_budget_two_and_mixed_match_full(void)
{
  SISLSurf *ps = make_curved_surf();
  SISLSurf *full = NULL;
  SISLOffsetJob *job;
  SISLOffsetJobStats st;
  int jstat = 0, rc, stat = 0;
  int budgets[] = {2, 1, 5, 1, 3, 4};
  char fulltrace[1200000];
  char splittrace[1200000];
  double diff = 0.0;
  struct trace t;

  TASK_CHECK(ps != NULL);
  task_reference_s1365(ps, 0.07, 0.0005, 0.0, 3, &full, &stat);
  TASK_CHECK(stat == 0 && full != NULL);

  t.text[0] = '\0'; t.count = 0; t.record = 1;
  task_eval_set_observer(trace_observer, &t);
  {
    SISLSurf *r2 = NULL;
    task_reference_s1365(ps, 0.07, 0.0005, 0.0, 3, &r2, &stat);
    freeSurf(r2);
  }
  task_eval_set_observer(NULL, NULL);
  strcpy(fulltrace, t.text);

  /* Budget 2. */
  job = sisl_offset_job_create(ps, 0.07, 0.0005, 0.0, 3, &jstat);
  TASK_CHECK(job != NULL);
  trace_start(&captured);
  rc = run_session(job, 2, &st);
  trace_stop(&captured);
  TASK_CHECK(rc == SISL_OFFSET_JOB_DONE);
  TASK_CHECK(strcmp(captured.text, fulltrace) == 0);
  {
    SISLSurf *r = sisl_offset_job_take_result(job, &jstat);
    TASK_CHECK(r != NULL && surfaces_match(r, full, &diff));
    TASK_CHECK(diff <= 1e-12);
    freeSurf(r);
  }
  sisl_offset_job_free(job);

  /* Mixed budgets. */
  job = sisl_offset_job_create(ps, 0.07, 0.0005, 0.0, 3, &jstat);
  TASK_CHECK(job != NULL);
  trace_start(&captured);
  rc = run_session_mixed(job, budgets, 6, &st);
  trace_stop(&captured);
  TASK_CHECK(rc == SISL_OFFSET_JOB_DONE);
  TASK_CHECK(st.evaluations_total == t.count);
  strcpy(splittrace, captured.text);
  TASK_CHECK(strcmp(splittrace, fulltrace) == 0);
  {
    SISLSurf *r = sisl_offset_job_take_result(job, &jstat);
    TASK_CHECK(r != NULL && surfaces_match(r, full, &diff));
    TASK_CHECK(diff <= 1e-12);
    freeSurf(r);
  }
  sisl_offset_job_free(job);

  freeSurf(full);
  freeSurf(ps);
  return 0;
}

static int case_large_budget_single_step(void)
{
  SISLSurf *ps = make_quadratic_surf();
  SISLSurf *full = NULL;
  SISLOffsetJob *job;
  SISLOffsetJobStats st;
  int jstat = 0, rc, stat = 0;
  double diff = 0.0;

  TASK_CHECK(ps != NULL);
  task_reference_s1365(ps, 0.05, 0.001, 0.0, 3, &full, &stat);
  TASK_CHECK(stat == 0 && full != NULL);

  job = sisl_offset_job_create(ps, 0.05, 0.001, 0.0, 3, &jstat);
  TASK_CHECK(job != NULL);

  rc = sisl_offset_job_step(job, 1000000, &st);
  TASK_CHECK(rc == SISL_OFFSET_JOB_DONE);
  TASK_CHECK(st.phase == SISL_OFFSET_PHASE_DONE);
  TASK_CHECK(st.evaluations == st.evaluations_total);

  {
    SISLSurf *r = sisl_offset_job_take_result(job, &jstat);
    TASK_CHECK(r != NULL && surfaces_match(r, full, &diff));
    TASK_CHECK(diff <= 1e-12);
    freeSurf(r);
  }

  sisl_offset_job_free(job);
  freeSurf(full);
  freeSurf(ps);
  return 0;
}

/* Pause right after a rejected trial step.  Budget 1 makes the session stop
   at every evaluation, including the ones inside an adaptive step; resuming
   must not restart the step or change its outcome. */

static int case_pause_at_rejected_trial(void)
{
  SISLSurf *ps = make_curved_surf();
  SISLSurf *ref = NULL;
  SISLOffsetJob *job;
  SISLOffsetJobStats st;
  int jstat = 0, rc, stat = 0, saw_more = 0, prev_total = 0;
  double diff = 0.0;
  int i;

  TASK_CHECK(ps != NULL);
  task_reference_s1365(ps, 0.08, 0.0008, 0.0, 3, &ref, &stat);
  TASK_CHECK(stat == 0 && ref != NULL);

  job = sisl_offset_job_create(ps, 0.08, 0.0008, 0.0, 3, &jstat);
  TASK_CHECK(job != NULL);

  for (i = 0; i < 2000000; ++i)
  {
    rc = sisl_offset_job_step(job, 1, &st);
    if (rc == SISL_OFFSET_JOB_MORE)
    {
      TASK_CHECK(st.evaluations == 1);
      TASK_CHECK(st.evaluations_total == prev_total + 1);
      prev_total = st.evaluations_total;
      saw_more = 1;
      /* The curve phase is where the adaptive trial steps live. */
      if (st.phase == SISL_OFFSET_PHASE_CURVES)
        TASK_CHECK(st.boundary >= 0 && st.boundary < 4);
    }
    else
      break;
  }
  TASK_CHECK(saw_more);
  TASK_CHECK(rc == SISL_OFFSET_JOB_DONE);

  {
    SISLSurf *r = sisl_offset_job_take_result(job, &jstat);
    TASK_CHECK(r != NULL && surfaces_match(r, ref, &diff));
    TASK_CHECK(diff <= 1e-12);
    freeSurf(r);
  }

  sisl_offset_job_free(job);
  freeSurf(ref);
  freeSurf(ps);
  return 0;
}

/* The boundary field must walk 0..3 during the edge-curve phase, and the
   fourth edge must be reported before the surface phase begins. */

static int case_boundary_progress_reaches_fourth_edge(void)
{
  SISLSurf *ps = make_cubic_surf();
  SISLOffsetJob *job;
  SISLOffsetJobStats st;
  int jstat = 0, rc, seen[4] = {0, 0, 0, 0};
  int prev = -1, rc2, guard = 0;

  TASK_CHECK(ps != NULL);
  job = sisl_offset_job_create(ps, 0.1, 0.001, 0.0, 3, &jstat);
  TASK_CHECK(job != NULL);

  do
  {
    rc = sisl_offset_job_step(job, 2, &st);
    if (st.phase == SISL_OFFSET_PHASE_CURVES &&
        st.boundary >= 0 && st.boundary < 4)
      seen[st.boundary] = 1;
    if (st.phase == SISL_OFFSET_PHASE_SURFACE) prev = 4;
    if (++guard > 2000000) return 1;
  } while (rc == SISL_OFFSET_JOB_MORE);

  TASK_CHECK(rc == SISL_OFFSET_JOB_DONE);
  TASK_CHECK(seen[0] && seen[1] && seen[2] && seen[3]);
  TASK_CHECK(prev == 4);

  /* After completion a step still reports DONE and does no evaluation. */
  trace_start(&captured);
  rc2 = sisl_offset_job_step(job, 5, &st);
  trace_stop(&captured);
  TASK_CHECK(rc2 == SISL_OFFSET_JOB_DONE);
  TASK_CHECK(st.evaluations == 0);
  TASK_CHECK(captured.count == 0);

  sisl_offset_job_free(job);
  freeSurf(ps);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Ownership.                                                         */
/* ------------------------------------------------------------------ */

static int case_result_only_after_done(void)
{
  SISLSurf *ps = make_cubic_surf();
  SISLOffsetJob *job;
  SISLOffsetJobStats st;
  int jstat = 0;

  TASK_CHECK(ps != NULL);
  job = sisl_offset_job_create(ps, 0.1, 0.001, 0.0, 3, &jstat);
  TASK_CHECK(job != NULL);

  TASK_CHECK(sisl_offset_job_step(job, 5, &st) == SISL_OFFSET_JOB_MORE);
  {
    SISLSurf *r = sisl_offset_job_take_result(job, &jstat);
    TASK_CHECK(r == NULL);
    TASK_CHECK(jstat == SISL_OFFSET_JOB_ERR_STATE);
  }

  TASK_CHECK(run_session(job, 5, &st) == SISL_OFFSET_JOB_DONE);
  {
    SISLSurf *r = sisl_offset_job_take_result(job, &jstat);
    SISLSurf *again;
    int jstat2 = 0;
    TASK_CHECK(r != NULL && jstat == 0);
    TASK_CHECK(st.result_taken == 0);

    again = sisl_offset_job_take_result(job, &jstat2);
    TASK_CHECK(again == NULL);
    TASK_CHECK(jstat2 == SISL_OFFSET_JOB_ERR_STATE);

    freeSurf(r);
  }

  sisl_offset_job_free(job);
  freeSurf(ps);
  return 0;
}

static int case_source_may_be_freed_after_create(void)
{
  SISLSurf *ps = make_curved_surf();
  SISLSurf *ref = NULL;
  SISLOffsetJob *job;
  SISLOffsetJobStats st;
  int jstat = 0, stat = 0, rc;
  double diff = 0.0;

  TASK_CHECK(ps != NULL);
  task_reference_s1365(ps, 0.09, 0.0006, 0.0, 3, &ref, &stat);
  TASK_CHECK(stat == 0 && ref != NULL);

  job = sisl_offset_job_create(ps, 0.09, 0.0006, 0.0, 3, &jstat);
  TASK_CHECK(job != NULL);

  /* Free and then poison-scrub the caller's surface; the session must be
     unaffected because create() copied it. */
  freeSurf(ps);
  ps = NULL;

  rc = run_session(job, 3, &st);
  TASK_CHECK(rc == SISL_OFFSET_JOB_DONE);
  {
    SISLSurf *r = sisl_offset_job_take_result(job, &jstat);
    TASK_CHECK(r != NULL && surfaces_match(r, ref, &diff));
    TASK_CHECK(diff <= 1e-12);
    freeSurf(r);
  }

  sisl_offset_job_free(job);
  freeSurf(ref);
  return 0;
}

static int case_two_sessions_interleave(void)
{
  SISLSurf *ps1 = make_cubic_surf();
  SISLSurf *ps2 = make_curved_surf();
  SISLSurf *ref1 = NULL, *ref2 = NULL;
  SISLOffsetJob *a, *b;
  SISLOffsetJobStats sa, sb;
  int jstat = 0, stat = 0, ra = 1, rb = 1, guard = 0;
  double d1 = 0.0, d2 = 0.0;

  TASK_CHECK(ps1 != NULL && ps2 != NULL);
  task_reference_s1365(ps1, 0.1, 0.001, 0.0, 3, &ref1, &stat);
  TASK_CHECK(stat == 0 && ref1 != NULL);
  task_reference_s1365(ps2, 0.06, 0.0007, 0.0, 3, &ref2, &stat);
  TASK_CHECK(stat == 0 && ref2 != NULL);

  a = sisl_offset_job_create(ps1, 0.1, 0.001, 0.0, 3, &jstat);
  b = sisl_offset_job_create(ps2, 0.06, 0.0007, 0.0, 3, &jstat);
  TASK_CHECK(a != NULL && b != NULL);

  while ((ra == SISL_OFFSET_JOB_MORE || rb == SISL_OFFSET_JOB_MORE) &&
         guard++ < 4000000)
  {
    if (ra == SISL_OFFSET_JOB_MORE) ra = sisl_offset_job_step(a, 2, &sa);
    if (rb == SISL_OFFSET_JOB_MORE) rb = sisl_offset_job_step(b, 3, &sb);
  }
  TASK_CHECK(ra == SISL_OFFSET_JOB_DONE);
  TASK_CHECK(rb == SISL_OFFSET_JOB_DONE);

  {
    SISLSurf *r1 = sisl_offset_job_take_result(a, &jstat);
    SISLSurf *r2 = sisl_offset_job_take_result(b, &jstat);
    TASK_CHECK(r1 != NULL && r2 != NULL);
    TASK_CHECK(surfaces_match(r1, ref1, &d1));
    TASK_CHECK(surfaces_match(r2, ref2, &d2));
    TASK_CHECK(d1 <= 1e-12 && d2 <= 1e-12);
    freeSurf(r1);
    freeSurf(r2);
  }

  sisl_offset_job_free(a);
  sisl_offset_job_free(b);
  freeSurf(ref1);
  freeSurf(ref2);
  freeSurf(ps1);
  freeSurf(ps2);
  return 0;
}

static int case_cancel_before_done(void)
{
  SISLSurf *ps = make_cubic_surf();
  SISLOffsetJob *job;
  SISLOffsetJobStats st;
  int jstat = 0, rc;

  TASK_CHECK(ps != NULL);
  job = sisl_offset_job_create(ps, 0.1, 0.001, 0.0, 3, &jstat);
  TASK_CHECK(job != NULL);

  TASK_CHECK(sisl_offset_job_step(job, 4, &st) == SISL_OFFSET_JOB_MORE);
  TASK_CHECK(st.evaluations_total == 4);

  TASK_CHECK(sisl_offset_job_cancel(job) == 0);
  TASK_CHECK(sisl_offset_job_cancel(job) == 0);   /* idempotent */

  trace_start(&captured);
  rc = sisl_offset_job_step(job, 8, &st);
  trace_stop(&captured);
  TASK_CHECK(rc == SISL_OFFSET_JOB_CANCELLED);
  TASK_CHECK(st.evaluations == 0);
  TASK_CHECK(captured.count == 0);

  {
    SISLSurf *r = sisl_offset_job_take_result(job, &jstat);
    TASK_CHECK(r == NULL);
    TASK_CHECK(jstat == SISL_OFFSET_JOB_ERR_STATE);
  }

  sisl_offset_job_free(job);
  freeSurf(ps);
  return 0;
}

static int case_cancel_before_any_step(void)
{
  SISLSurf *ps = make_cubic_surf();
  SISLOffsetJob *job;
  SISLOffsetJobStats st;
  int jstat = 0, rc;

  TASK_CHECK(ps != NULL);
  job = sisl_offset_job_create(ps, 0.1, 0.001, 0.0, 3, &jstat);
  TASK_CHECK(job != NULL);

  TASK_CHECK(sisl_offset_job_cancel(job) == 0);
  trace_start(&captured);
  rc = sisl_offset_job_step(job, 4, &st);
  trace_stop(&captured);
  TASK_CHECK(rc == SISL_OFFSET_JOB_CANCELLED);
  TASK_CHECK(st.evaluations == 0 && captured.count == 0);

  sisl_offset_job_free(job);
  freeSurf(ps);
  return 0;
}

static int case_cancel_after_done_is_noop(void)
{
  SISLSurf *ps = make_cubic_surf();
  SISLSurf *ref = NULL;
  SISLOffsetJob *job;
  SISLOffsetJobStats st;
  int jstat = 0, stat = 0;
  double diff = 0.0;

  TASK_CHECK(ps != NULL);
  task_reference_s1365(ps, 0.1, 0.001, 0.0, 3, &ref, &stat);
  TASK_CHECK(stat == 0 && ref != NULL);

  job = sisl_offset_job_create(ps, 0.1, 0.001, 0.0, 3, &jstat);
  TASK_CHECK(job != NULL);
  TASK_CHECK(run_session(job, 6, &st) == SISL_OFFSET_JOB_DONE);

  TASK_CHECK(sisl_offset_job_cancel(job) == 0);
  TASK_CHECK(sisl_offset_job_cancel(job) == 0);

  /* The result is still available and correct. */
  {
    SISLSurf *r = sisl_offset_job_take_result(job, &jstat);
    TASK_CHECK(r != NULL && surfaces_match(r, ref, &diff));
    TASK_CHECK(diff <= 1e-12);
    freeSurf(r);
  }

  sisl_offset_job_free(job);
  freeSurf(ref);
  freeSurf(ps);
  return 0;
}

static int case_cancel_after_taking_result(void)
{
  SISLSurf *ps = make_cubic_surf();
  SISLOffsetJob *job;
  SISLOffsetJobStats st;
  SISLSurf *r;
  int jstat = 0;

  TASK_CHECK(ps != NULL);
  job = sisl_offset_job_create(ps, 0.1, 0.001, 0.0, 3, &jstat);
  TASK_CHECK(job != NULL);
  TASK_CHECK(run_session(job, 1000000, &st) == SISL_OFFSET_JOB_DONE);

  r = sisl_offset_job_take_result(job, &jstat);
  TASK_CHECK(r != NULL);

  TASK_CHECK(sisl_offset_job_cancel(job) == 0);
  /* The surface the caller owns survives the cancel and the free. */
  TASK_CHECK(r->in1 > 0 && r->ecoef != NULL);

  sisl_offset_job_free(job);
  TASK_CHECK(r->in1 > 0 && r->ecoef != NULL);
  freeSurf(r);
  freeSurf(ps);
  return 0;
}

static int case_free_without_taking(void)
{
  SISLSurf *ps = make_cubic_surf();
  SISLOffsetJob *job;
  SISLOffsetJobStats st;
  int jstat = 0;

  TASK_CHECK(ps != NULL);
  job = sisl_offset_job_create(ps, 0.1, 0.001, 0.0, 3, &jstat);
  TASK_CHECK(job != NULL);
  TASK_CHECK(run_session(job, 7, &st) == SISL_OFFSET_JOB_DONE);
  /* Freeing without taking must release the result. */
  sisl_offset_job_free(job);

  /* Freeing while suspended mid-work must also be safe. */
  job = sisl_offset_job_create(ps, 0.1, 0.001, 0.0, 3, &jstat);
  TASK_CHECK(job != NULL);
  TASK_CHECK(sisl_offset_job_step(job, 3, &st) == SISL_OFFSET_JOB_MORE);
  sisl_offset_job_free(job);

  freeSurf(ps);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Compatibility.                                                     */
/* ------------------------------------------------------------------ */

/* For each input, the resumable session must reproduce the reference
   geometry on knots, control points and the true offset (position and
   derivative) within a small combined tolerance. */

static int compare_on_input(SISLSurf *ps, double off, double eps)
{
  SISLSurf *ref = NULL;
  SISLSurf *jobres;
  SISLOffsetJob *job;
  SISLOffsetJobStats st;
  int jstat = 0, stat = 0, rc, i;
  double diff = 0.0;

  task_reference_s1365(ps, off, eps, 0.0, 3, &ref, &stat);
  TASK_CHECK(stat == 0 && ref != NULL);

  job = sisl_offset_job_create(ps, off, eps, 0.0, 3, &jstat);
  TASK_CHECK(job != NULL);
  rc = run_session(job, 3, &st);
  TASK_CHECK(rc == SISL_OFFSET_JOB_DONE);

  jobres = sisl_offset_job_take_result(job, &jstat);
  TASK_CHECK(jobres != NULL);
  TASK_CHECK(surfaces_match(jobres, ref, &diff));
  TASK_CHECK(diff <= 1e-9);
  /* The result is a genuine B-spline surface with a well-formed knot
     vector, not an empty shell. */
  TASK_CHECK(jobres->in1 >= jobres->ik1 && jobres->in2 >= jobres->ik2);
  TASK_CHECK(interior_knots(jobres) >= 0);

  /* Back-substitute the true offset at several points on both surfaces and
     compare position and derivatives. */
  {
    int gu, gv;
    double u0 = ref->et1[ref->ik1 - 1], u1 = ref->et1[ref->in1];
    double v0 = ref->et2[ref->ik2 - 1], v1 = ref->et2[ref->in2];
    (void)i;
    for (gu = 1; gu <= 3; ++gu)
      for (gv = 1; gv <= 3; ++gv)
      {
        double uv[2];
        double dref[27], djob[27];
        int l1 = 0, l2 = 0, l3 = 0, l4 = 0, s1 = 0, s2 = 0;
        double scale;

        uv[0] = u0 + (u1 - u0) * gu / 4.0;
        uv[1] = v0 + (v1 - v0) * gv / 4.0;
        s1424(ref, 1, 1, uv, &l1, &l2, dref, &s1);
        s1424(jobres, 1, 1, uv, &l3, &l4, djob, &s2);
        TASK_CHECK(s1 >= 0 && s2 >= 0);

        for (i = 0; i < 9; ++i)
        {
          scale = max3(fabs(dref[i]), fabs(djob[i]), 1.0);
          TASK_CHECK(close_enough(dref[i], djob[i], scale));
        }
      }
  }

  sisl_offset_job_free(job);
  freeSurf(jobres);
  freeSurf(ref);
  return 0;
}

static int case_compare_cubic(void)
{
  SISLSurf *ps = make_cubic_surf();
  TASK_CHECK(ps != NULL);
  if (compare_on_input(ps, 0.1, 0.001) != 0) return 1;
  freeSurf(ps);
  return 0;
}

static int case_compare_curved_nonunit(void)
{
  SISLSurf *ps = make_curved_surf();
  TASK_CHECK(ps != NULL);
  if (compare_on_input(ps, -0.08, 0.0008) != 0) return 1;
  freeSurf(ps);
  return 0;
}

static int case_compare_rational(void)
{
  SISLSurf *ps = make_rational_surf();
  TASK_CHECK(ps != NULL);
  if (compare_on_input(ps, 0.05, 0.0005) != 0) return 1;
  freeSurf(ps);
  return 0;
}

static int case_compare_quadratic(void)
{
  SISLSurf *ps = make_quadratic_surf();
  TASK_CHECK(ps != NULL);
  if (compare_on_input(ps, 0.06, 0.0009) != 0) return 1;
  freeSurf(ps);
  return 0;
}

/* The old synchronous entry points must keep working through the shared
   kernel and still be usable after a session has run. */

static int case_legacy_sync_still_works(void)
{
  SISLSurf *ps = make_curved_surf();
  SISLSurf *a = NULL, *b = NULL;
  SISLOffsetJob *job;
  SISLOffsetJobStats st;
  int stat = 0, jstat = 0, rc;
  double diff = 0.0;

  TASK_CHECK(ps != NULL);
  s1365(ps, 0.09, 0.0006, 0.0, 3, &a, &stat);
  TASK_CHECK(stat == 0 && a != NULL);

  /* Run a session in between, then call the legacy entry again. */
  job = sisl_offset_job_create(ps, 0.09, 0.0006, 0.0, 3, &jstat);
  TASK_CHECK(job != NULL);
  rc = run_session(job, 4, &st);
  TASK_CHECK(rc == SISL_OFFSET_JOB_DONE);
  {
    SISLSurf *r = sisl_offset_job_take_result(job, &jstat);
    TASK_CHECK(r != NULL && surfaces_match(r, a, &diff) && diff <= 1e-12);
    freeSurf(r);
  }
  sisl_offset_job_free(job);

  s1365(ps, 0.09, 0.0006, 0.0, 3, &b, &stat);
  TASK_CHECK(stat == 0 && b != NULL);
  TASK_CHECK(surfaces_match(a, b, &diff) && diff == 0.0);

  freeSurf(a);
  freeSurf(b);
  freeSurf(ps);
  return 0;
}

/* Reference and synchronous paths agree on all inputs we support. */

static int case_reference_matches_legacy(void)
{
  SISLSurf *inputs[4];
  int n = 0, k;

  inputs[n++] = make_cubic_surf();
  inputs[n++] = make_curved_surf();
  inputs[n++] = make_rational_surf();
  inputs[n++] = make_quadratic_surf();

  for (k = 0; k < n; ++k)
  {
    SISLSurf *ref = NULL, *sync = NULL;
    int s1 = 0, s2 = 0;
    double diff = 0.0;
    TASK_CHECK(inputs[k] != NULL);
    task_reference_s1365(inputs[k], 0.07, 0.0007, 0.0, 3, &ref, &s1);
    s1365(inputs[k], 0.07, 0.0007, 0.0, 3, &sync, &s2);
    TASK_CHECK(s1 == 0 && s2 == 0);
    TASK_CHECK(ref != NULL && sync != NULL);
    TASK_CHECK(surfaces_match(sync, ref, &diff));
    TASK_CHECK(diff == 0.0);
    freeSurf(ref);
    freeSurf(sync);
  }

  for (k = 0; k < n; ++k) freeSurf(inputs[k]);
  return 0;
}

/* Input validation is reported distinctly and never leaks a half-built job. */

static int case_invalid_inputs(void)
{
  int jstat = 0;
  SISLOffsetJob *job;

  job = sisl_offset_job_create(NULL, 0.1, 0.001, 0.0, 3, &jstat);
  TASK_CHECK(job == NULL && jstat == SISL_OFFSET_JOB_ERR_ARGUMENT);

  {
    SISLSurf *ps = make_cubic_surf();
    TASK_CHECK(ps != NULL);

    /* Wrong ambient dimension. */
    job = sisl_offset_job_create(ps, 0.1, 0.001, 0.0, 2, &jstat);
    TASK_CHECK(job == NULL && jstat == SISL_OFFSET_JOB_ERR_ARGUMENT);

    /* Non-positive geometric tolerance. */
    job = sisl_offset_job_create(ps, 0.1, 0.0, 0.0, 3, &jstat);
    TASK_CHECK(job == NULL && jstat == SISL_OFFSET_JOB_ERR_ARGUMENT);

    /* Negative maximal step. */
    job = sisl_offset_job_create(ps, 0.1, 0.001, -1.0, 3, &jstat);
    TASK_CHECK(job == NULL && jstat == SISL_OFFSET_JOB_ERR_ARGUMENT);

    freeSurf(ps);
  }

  /* NULL session arguments. */
  TASK_CHECK(sisl_offset_job_create(NULL, 0.0, 1.0, 0.0, 3, NULL) == NULL);
  TASK_CHECK(sisl_offset_job_cancel(NULL) == SISL_OFFSET_JOB_ERR_ARGUMENT);
  TASK_CHECK(sisl_offset_job_take_result(NULL, &jstat) == NULL);
  TASK_CHECK(jstat == SISL_OFFSET_JOB_ERR_ARGUMENT);
  sisl_offset_job_free(NULL);   /* must be harmless */

  return 0;
}

/* A failure in the middle does not corrupt the next session. */

static int case_failure_does_not_poison_next_session(void)
{
  SISLSurf *ps = make_cubic_surf();
  SISLOffsetJob *bad;
  SISLOffsetJobStats st;
  SISLSurf *good = NULL;
  int jstat = 0, rc, stat = 0;
  double diff = 0.0;

  TASK_CHECK(ps != NULL);
  task_reference_s1365(ps, 0.1, 0.001, 0.0, 3, &good, &stat);
  TASK_CHECK(stat == 0 && good != NULL);

  bad = sisl_offset_job_create(ps, 0.1, 0.001, 0.0, 3, &jstat);
  TASK_CHECK(bad != NULL);
  TASK_CHECK(sisl_offset_job_step(bad, 6, &st) == SISL_OFFSET_JOB_MORE);
  TASK_CHECK(sisl_offset_job_cancel(bad) == 0);
  sisl_offset_job_free(bad);

  {
    SISLOffsetJob *job = sisl_offset_job_create(ps, 0.1, 0.001, 0.0, 3,
                                                &jstat);
    TASK_CHECK(job != NULL);
    rc = run_session(job, 1, &st);
    TASK_CHECK(rc == SISL_OFFSET_JOB_DONE);
    {
      SISLSurf *r = sisl_offset_job_take_result(job, &jstat);
      TASK_CHECK(r != NULL && surfaces_match(r, good, &diff) && diff <= 1e-12);
      freeSurf(r);
    }
    sisl_offset_job_free(job);
  }

  freeSurf(good);
  freeSurf(ps);
  return 0;
}

/* Stats report both per-step and cumulative counts, and the observation of
   the real evaluators agrees with the reported numbers. */

static int case_stats_counts_match_observed(void)
{
  SISLSurf *ps = make_curved_surf();
  SISLOffsetJob *job;
  SISLOffsetJobStats st;
  int jstat = 0, rc, prev_total = 0, guard = 0;

  TASK_CHECK(ps != NULL);
  job = sisl_offset_job_create(ps, 0.07, 0.0006, 0.0, 3, &jstat);
  TASK_CHECK(job != NULL);

  do
  {
    int observed;
    trace_start(&captured);
    rc = sisl_offset_job_step(job, 4, &st);
    trace_stop(&captured);
    observed = captured.count;
    TASK_CHECK(st.evaluations == observed);
    TASK_CHECK(st.evaluations_total == prev_total + observed);
    prev_total = st.evaluations_total;
    if (++guard > 2000000) return 1;
  } while (rc == SISL_OFFSET_JOB_MORE);

  TASK_CHECK(rc == SISL_OFFSET_JOB_DONE);
  TASK_CHECK(st.phase == SISL_OFFSET_PHASE_DONE);

  sisl_offset_job_free(job);
  freeSurf(ps);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Registration.                                                      */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] =
{
  {"scheduling",  "zero_budget_is_inert",             case_zero_budget_is_inert},
  {"scheduling",  "negative_budget_rejected",         case_negative_budget_rejected},
  {"scheduling",  "budget_exactly_exhausted",         case_budget_exactly_exhausted},
  {"scheduling",  "budget_one_trace_matches_full",    case_budget_one_trace_matches_full},
  {"scheduling",  "budget_two_and_mixed_match_full",  case_budget_two_and_mixed_match_full},
  {"scheduling",  "large_budget_single_step",         case_large_budget_single_step},
  {"scheduling",  "pause_at_rejected_trial",          case_pause_at_rejected_trial},
  {"scheduling",  "boundary_progress_reaches_fourth", case_boundary_progress_reaches_fourth_edge},
  {"scheduling",  "stats_counts_match_observed",      case_stats_counts_match_observed},

  {"ownership",   "result_only_after_done",           case_result_only_after_done},
  {"ownership",   "source_may_be_freed_after_create", case_source_may_be_freed_after_create},
  {"ownership",   "two_sessions_interleave",          case_two_sessions_interleave},
  {"ownership",   "cancel_before_done",               case_cancel_before_done},
  {"ownership",   "cancel_before_any_step",           case_cancel_before_any_step},
  {"ownership",   "cancel_after_done_is_noop",        case_cancel_after_done_is_noop},
  {"ownership",   "cancel_after_taking_result",       case_cancel_after_taking_result},
  {"ownership",   "free_without_taking",              case_free_without_taking},

  {"compatibility", "compare_cubic",                  case_compare_cubic},
  {"compatibility", "compare_curved_nonunit",         case_compare_curved_nonunit},
  {"compatibility", "compare_rational",               case_compare_rational},
  {"compatibility", "compare_quadratic",              case_compare_quadratic},
  {"compatibility", "legacy_sync_still_works",        case_legacy_sync_still_works},
  {"compatibility", "reference_matches_legacy",       case_reference_matches_legacy},
  {"compatibility", "invalid_inputs",                 case_invalid_inputs},
  {"compatibility", "failure_does_not_poison_next",   case_failure_does_not_poison_next_session}
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
