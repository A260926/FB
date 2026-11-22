/*
 * Copyright (C) 1998, 2000-2007, 2010, 2011, 2012, 2013 SINTEF Digital,
 * Applied Mathematics, Norway.
 *
 * Contact information: E-mail: tor.dokken@sintef.no
 * SINTEF Digital, Department of Mathematics and Cybernetics,
 * P.O. Box 124 Blindern,
 * 0314 Oslo, Norway.
 *
 * This file is part of SISL.
 *
 * SISL is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * SISL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public
 * License along with SISL. If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * In accordance with Section 7(b) of the GNU Affero General Public
 * License, a covered work must retain the producer line in every data
 * file that is created or manipulated using SISL.
 *
 * Other Usage
 * You can be released from the requirements of the license by purchasing
 * a commercial license. Buying such a license is mandatory as soon as you
 * develop commercial activities involving the SISL library without
 * disclosing the source code of your own applications.
 *
 * This file may be used in accordance with the terms contained in a
 * written agreement between you and SINTEF Digital.
 */

/*
 * Resumable offset-surface sessions.
 *
 * The public, synchronous s1365() entry point and this module run the
 * same kernel, sisl_offset_surface_run(), so the geometry cannot diverge.
 * The session executes the kernel on a small coroutine.  Every real curve
 * or surface evaluation on the offset path passes through
 * SISL_OFFSET_EVAL_GATE() (see sislP.h); when the budget of the current
 * step is spent the gate switches back to sisl_offset_job_step(), leaving
 * the kernel suspended exactly before the unpaid evaluation.  A later step
 * switches back into the kernel and the evaluation proceeds normally.
 * Because the kernel's own stack and locals survive suspension, pausing
 * never loses a partially accepted trial step and never restarts work.
 *
 * Cancellation unwinds the kernel by returning a distinguished status
 * (SISL_OFFSET_ABORT) from the gate, which propagates through the existing
 * error paths of s1360/s1367/... and frees their temporary storage before
 * control returns here.  Failures reported by the kernel propagate the
 * same way and are translated into the session's error codes.
 */

#define _XOPEN_SOURCE 600

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <ucontext.h>

#include "sisl-copyright.h"
#include "sislP.h"
#include "sisl_offset_job.h"

#if !defined(SISL_OFFSET_COROUTINE_STACK)
#define SISL_OFFSET_COROUTINE_STACK ((size_t)1048576)
#endif

/* The hooks the evaluators consult are also defined (weakly) alongside the
   shared kernel in src/s1365.c.  Defininig them here as well guarantees the
   job engine links whether or not that archive member is pulled in; the
   duplicate weak definitions merge.  They are all null outside a running
   session, which makes the gate macros no-ops for ordinary library callers. */

#if defined(__GNUC__) || defined(__clang__)
#define SISL_OFFSET_WEAK __attribute__((weak))
#else
#define SISL_OFFSET_WEAK
#endif

SISL_OFFSET_WEAK int (*sisl_offset_eval_wait_hook)(void) = SISL_NULL;
SISL_OFFSET_WEAK int (*sisl_offset_eval_enter_hook)(void) = SISL_NULL;
SISL_OFFSET_WEAK void (*sisl_offset_progress_hook)(int,int) = SISL_NULL;
SISL_OFFSET_WEAK int sisl_offset_quiet = 0;

struct SISLOffsetJob
{
  /* Inputs (a private copy: the caller may free the source surface). */
  SISLSurf *source;
  double offset;
  double epsge;
  double maxstep;
  int idim;

  /* Coroutine execution. */
  ucontext_t caller;
  ucontext_t worker;
  char *stack;
  int worker_live;      /* 1 while the kernel has not returned          */
  int suspended;        /* 1 while suspended at an evaluation gate      */

  /* Budget accounting for the current step. */
  int budget;
  int used;

  /* Progress reporting. */
  int phase;
  int boundary;

  /* Outcome. */
  int done;             /* kernel returned                             */
  int failed;
  int cancelled;        /* cancel() requested; unwind in progress      */
  int status;           /* last status (result or error)               */
  int result_taken;
  SISLSurf *result;

  int evaluations_total;
};

static SISLOffsetJob *current_job;   /* the one session currently running */

/* ------------------------------------------------------------------ */
/* Hooks seen by the evaluators.                                       */
/* ------------------------------------------------------------------ */

/* Called immediately before a real evaluation.  Returns non-zero when the
   caller must abandon the evaluation because the session is being
   cancelled (or the kernel already failed). */

static int offset_wait(void)
{
  SISLOffsetJob *job = current_job;

  if (job == SISL_NULL) return 0;

  if (job->cancelled || job->failed)
    return 1;

  if (job->used >= job->budget)
  {
    /* Budget exhausted: leave before starting the evaluation. */
    job->suspended = 1;
    swapcontext(&job->worker, &job->caller);
    job->suspended = 0;

    /* We only get here when a later step resumed us.  Re-check the
       cancellation state, which cancel() may have set meanwhile. */
    if (job->cancelled || job->failed)
      return 1;
  }

  return 0;
}

/* Called at the real evaluation entry point, after the wait hook let the
   evaluation proceed.  One unit of budget per evaluation. */

static int offset_enter(void)
{
  SISLOffsetJob *job = current_job;

  if (job == SISL_NULL) return 0;

  if (job->used >= job->budget)
  {
    /* Defensive: the gate should have suspended before we got here. */
    return 1;
  }

  ++job->used;
  ++job->evaluations_total;

  return 0;
}

static void offset_progress(int phase, int boundary)
{
  SISLOffsetJob *job = current_job;

  if (job == SISL_NULL) return;

  job->phase = phase;
  if (boundary >= 0) job->boundary = boundary;
}

/* ------------------------------------------------------------------ */
/* Coroutine body: run the shared kernel.                              */
/* ------------------------------------------------------------------ */

static void offset_job_body(void)
{
  SISLOffsetJob *job = current_job;
  SISLSurf *result = SISL_NULL;
  int stat = 0;

  sisl_offset_surface_run(job->source, job->offset, job->epsge, job->maxstep,
                          job->idim, &result, &stat);

  job->worker_live = 0;

  if (job->cancelled)
  {
    if (result != SISL_NULL) freeSurf(result);
    job->status = SISL_OFFSET_JOB_CANCELLED;
    return;
  }

  if (stat == SISL_OFFSET_ABORT)
  {
    /* The gate only aborts on cancellation or an earlier failure, both
       handled above, so reaching here means an inconsistent unwind. */
    if (result != SISL_NULL) freeSurf(result);
    job->failed = 1;
    job->status = SISL_OFFSET_JOB_ERR_NUMERIC;
    return;
  }

  if (stat < 0 || result == SISL_NULL)
  {
    if (result != SISL_NULL) freeSurf(result);
    job->failed = 1;
    /* Distinguish allocation failure from the geometry statuses, while
       still surfacing the precise library code for other failures. */
    if (stat == -101)
      job->status = SISL_OFFSET_JOB_ERR_MEMORY;
    else if (stat < 0)
      job->status = stat;
    else
      job->status = SISL_OFFSET_JOB_ERR_NUMERIC;
    return;
  }

  /* Guard against a numerically failed result before handing it out. */
  {
    int i, finite = 1;
    for (i = 0; i < result->in1 * result->in2 * result->idim; ++i)
      if (!isfinite(result->ecoef[i])) { finite = 0; break; }
    if (!finite)
    {
      freeSurf(result);
      job->failed = 1;
      job->status = SISL_OFFSET_JOB_ERR_NUMERIC;
      return;
    }
  }

  job->result = result;
  job->done = 1;
  job->phase = SISL_OFFSET_PHASE_DONE;
}

/* ------------------------------------------------------------------ */
/* Public API.                                                         */
/* ------------------------------------------------------------------ */

static int valid_surface(const SISLSurf *ps)
{
  if (ps == SISL_NULL) return 0;
  if (ps->idim != 3) return 0;
  if (ps->ikind != 1 && ps->ikind != 2 && ps->ikind != 3 && ps->ikind != 4)
    return 0;
  if (ps->in1 < ps->ik1 || ps->in2 < ps->ik2) return 0;
  /* Degree 2 or 3 in each direction, i.e. spline order 3 or 4. */
  if (ps->ik1 != 3 && ps->ik1 != 4) return 0;
  if (ps->ik2 != 3 && ps->ik2 != 4) return 0;
  if (ps->et1 == SISL_NULL || ps->et2 == SISL_NULL) return 0;
  if (ps->ecoef == SISL_NULL) return 0;
  if (ps->cuopen_1 == SISL_CRV_PERIODIC || ps->cuopen_2 == SISL_CRV_PERIODIC)
    return 0;
  return 1;
}

SISLOffsetJob *sisl_offset_job_create(const SISLSurf *ps, double aoffset,
                                       double aepsge, double amax, int idim,
                                       int *jstat)
{
  SISLOffsetJob *job;

  if (jstat != SISL_NULL) *jstat = 0;

  if (ps == SISL_NULL || idim != 3 || !valid_surface(ps))
  {
    if (jstat != SISL_NULL) *jstat = SISL_OFFSET_JOB_ERR_ARGUMENT;
    return SISL_NULL;
  }

  if (!(aepsge > 0.0) || !(amax >= 0.0) || !isfinite(aoffset))
  {
    if (jstat != SISL_NULL) *jstat = SISL_OFFSET_JOB_ERR_ARGUMENT;
    return SISL_NULL;
  }

  job = (SISLOffsetJob *)calloc(1, sizeof(*job));
  if (job == SISL_NULL)
  {
    if (jstat != SISL_NULL) *jstat = SISL_OFFSET_JOB_ERR_MEMORY;
    return SISL_NULL;
  }

  job->source = copySurface((SISLSurf *)ps);
  if (job->source == SISL_NULL)
  {
    free(job);
    if (jstat != SISL_NULL) *jstat = SISL_OFFSET_JOB_ERR_MEMORY;
    return SISL_NULL;
  }

  job->offset = aoffset;
  job->epsge = aepsge;
  job->maxstep = amax;
  job->idim = idim;
  job->phase = SISL_OFFSET_PHASE_EDGES;
  job->boundary = -1;
  job->status = SISL_OFFSET_JOB_MORE;

  /* Only now, once the state is complete, set up the coroutine.  Nothing
     is evaluated until the first step runs it. */

  job->stack = (char *)malloc(SISL_OFFSET_COROUTINE_STACK);
  if (job->stack == SISL_NULL)
  {
    freeSurf(job->source);
    free(job);
    if (jstat != SISL_NULL) *jstat = SISL_OFFSET_JOB_ERR_MEMORY;
    return SISL_NULL;
  }

  if (getcontext(&job->worker) != 0)
  {
    free(job->stack);
    freeSurf(job->source);
    free(job);
    if (jstat != SISL_NULL) *jstat = SISL_OFFSET_JOB_ERR_MEMORY;
    return SISL_NULL;
  }

  job->worker.uc_stack.ss_sp = job->stack;
  job->worker.uc_stack.ss_size = SISL_OFFSET_COROUTINE_STACK;
  job->worker.uc_link = &job->caller;
  makecontext(&job->worker, offset_job_body, 0);
  job->worker_live = 1;

  return job;
}

static void fill_stats(SISLOffsetJob *job, SISLOffsetJobStats *stats)
{
  if (stats == SISL_NULL) return;

  stats->status = job->status;
  stats->evaluations = job->used;
  stats->evaluations_total = job->evaluations_total;
  stats->phase = job->phase;
  stats->boundary = job->boundary;
  stats->result_taken = job->result_taken;
}

int sisl_offset_job_step(SISLOffsetJob *job, int budget,
                         SISLOffsetJobStats *stats)
{
  int saved_quiet;

  if (job == SISL_NULL)
  {
    if (stats != SISL_NULL)
    {
      memset(stats, 0, sizeof(*stats));
      stats->status = SISL_OFFSET_JOB_ERR_ARGUMENT;
      stats->boundary = -1;
    }
    return SISL_OFFSET_JOB_ERR_ARGUMENT;
  }

  if (budget < 0)
  {
    /* A negative budget is rejected without touching the session. */
    if (stats != SISL_NULL) fill_stats(job, stats);
    if (stats != SISL_NULL) stats->evaluations = 0;
    if (stats != SISL_NULL) stats->status = SISL_OFFSET_JOB_ERR_BUDGET;
    return SISL_OFFSET_JOB_ERR_BUDGET;
  }

  /* Report a terminal state without running anything.  The per-step count
     is zero because this call performed no evaluation. */
  if (job->cancelled)
  {
    job->status = SISL_OFFSET_JOB_CANCELLED;
    fill_stats(job, stats);
    if (stats != SISL_NULL) stats->evaluations = 0;
    return SISL_OFFSET_JOB_CANCELLED;
  }
  if (job->failed)
  {
    int st = job->status;
    fill_stats(job, stats);
    if (stats != SISL_NULL) stats->evaluations = 0;
    return st;
  }
  if (job->done)
  {
    job->status = SISL_OFFSET_JOB_DONE;
    fill_stats(job, stats);
    if (stats != SISL_NULL) stats->evaluations = 0;
    return SISL_OFFSET_JOB_DONE;
  }

  if (budget == 0)
  {
    /* Zero budget is free: no state change, no evaluation. */
    if (stats != SISL_NULL)
    {
      fill_stats(job, stats);
      stats->evaluations = 0;
      stats->status = SISL_OFFSET_JOB_MORE;
    }
    return SISL_OFFSET_JOB_MORE;
  }

  job->budget = budget;
  job->used = 0;

  current_job = job;
  sisl_offset_eval_wait_hook = offset_wait;
  sisl_offset_eval_enter_hook = offset_enter;
  sisl_offset_progress_hook = offset_progress;
  saved_quiet = sisl_offset_quiet;
  sisl_offset_quiet = 1;

  if (job->worker_live)
    swapcontext(&job->caller, &job->worker);

  sisl_offset_quiet = saved_quiet;
  sisl_offset_eval_wait_hook = SISL_NULL;
  sisl_offset_eval_enter_hook = SISL_NULL;
  sisl_offset_progress_hook = SISL_NULL;
  current_job = SISL_NULL;

  if (job->cancelled)
  {
    job->status = SISL_OFFSET_JOB_CANCELLED;
    fill_stats(job, stats);
    return SISL_OFFSET_JOB_CANCELLED;
  }

  if (job->failed)
  {
    fill_stats(job, stats);
    return job->status;
  }

  if (job->done)
  {
    job->status = SISL_OFFSET_JOB_DONE;
    fill_stats(job, stats);
    return SISL_OFFSET_JOB_DONE;
  }

  /* The kernel suspended because the budget was spent. */
  job->status = SISL_OFFSET_JOB_MORE;
  fill_stats(job, stats);
  if (stats != SISL_NULL) stats->status = SISL_OFFSET_JOB_MORE;
  return SISL_OFFSET_JOB_MORE;
}

SISLSurf *sisl_offset_job_take_result(SISLOffsetJob *job, int *jstat)
{
  if (jstat != SISL_NULL) *jstat = 0;

  if (job == SISL_NULL)
  {
    if (jstat != SISL_NULL) *jstat = SISL_OFFSET_JOB_ERR_ARGUMENT;
    return SISL_NULL;
  }

  if (!job->done || job->result == SISL_NULL)
  {
    if (jstat != SISL_NULL) *jstat = SISL_OFFSET_JOB_ERR_STATE;
    return SISL_NULL;
  }

  if (job->result_taken)
  {
    if (jstat != SISL_NULL) *jstat = SISL_OFFSET_JOB_ERR_STATE;
    return SISL_NULL;
  }

  job->result_taken = 1;
  return job->result;
}

int sisl_offset_job_cancel(SISLOffsetJob *job)
{
  if (job == SISL_NULL) return SISL_OFFSET_JOB_ERR_ARGUMENT;

  /* After DONE cancellation is a no-op and the result is untouched. */
  if (job->done) return 0;

  if (!job->cancelled)
  {
    job->cancelled = 1;
    job->status = SISL_OFFSET_JOB_CANCELLED;
  }

  /* If the kernel is suspended mid-evaluation, drive it once more so the
     gate unwinds it and releases the work it is holding.  The cancellation
     flag is checked before the budget on resume, so the parked gate returns
     immediately with SISL_OFFSET_ABORT and the kernel unwinds. */
  if (job->worker_live && job->suspended)
  {
    int saved_quiet;

    job->budget = 0;
    job->used = 0;

    current_job = job;
    sisl_offset_eval_wait_hook = offset_wait;
    sisl_offset_eval_enter_hook = offset_enter;
    sisl_offset_progress_hook = offset_progress;
    saved_quiet = sisl_offset_quiet;
    sisl_offset_quiet = 1;

    swapcontext(&job->caller, &job->worker);

    sisl_offset_quiet = saved_quiet;
    sisl_offset_eval_wait_hook = SISL_NULL;
    sisl_offset_eval_enter_hook = SISL_NULL;
    sisl_offset_progress_hook = SISL_NULL;
    current_job = SISL_NULL;
  }

  return 0;
}

void sisl_offset_job_free(SISLOffsetJob *job)
{
  if (job == SISL_NULL) return;

  /* A kernel still suspended at a gate holds temporary curves and arrays.
     Cancel and unwind it so nothing leaks. */
  if (job->worker_live && !job->done)
    (void)sisl_offset_job_cancel(job);

  if (!job->result_taken && job->result != SISL_NULL)
    freeSurf(job->result);

  if (job->source != SISL_NULL)
    freeSurf(job->source);

  free(job->stack);
  free(job);
}
