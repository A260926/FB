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

#ifndef SISL_OFFSET_JOB_H
#define SISL_OFFSET_JOB_H

/* The resumable session runs on a coroutine.  On POSIX systems ucontext
   needs the X/Open feature-test macro, and because this header is also
   force-included by the regression build the macro has to be set before
   any system header is pulled in.  sisl.h does not depend on it. */

#if (defined(__unix__) || defined(__APPLE__)) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 600
#endif

#include "sisl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes returned by sisl_offset_job_step(). */

#define SISL_OFFSET_JOB_MORE       1   /* budget spent, work remains      */
#define SISL_OFFSET_JOB_DONE       0   /* the result surface is ready     */
#define SISL_OFFSET_JOB_CANCELLED -1   /* the session was cancelled       */

/* Error codes reported through the step statistics.  These are all
   negative and deliberately distinct from the SISL library status codes
   that are forwarded unchanged (for instance -101, -105, -179, -184).  */

#define SISL_OFFSET_JOB_ERR_ARGUMENT  (-2001)  /* invalid create argument */
#define SISL_OFFSET_JOB_ERR_BUDGET    (-2002)  /* negative step budget    */
#define SISL_OFFSET_JOB_ERR_STATE     (-2003)  /* illegal state/sequence  */
#define SISL_OFFSET_JOB_ERR_MEMORY    (-2004)  /* allocation failed       */
#define SISL_OFFSET_JOB_ERR_NUMERIC   (-2005)  /* non-finite geometry     */

/* Lifecycle phases, reported in the statistics. */

#define SISL_OFFSET_PHASE_EDGES     0  /* extracting the edge curves      */
#define SISL_OFFSET_PHASE_CURVES    1  /* offsetting the edge curves      */
#define SISL_OFFSET_PHASE_COMMON    2  /* common basis for opposite edges */
#define SISL_OFFSET_PHASE_SURFACE   3  /* evaluating/interpolating surface*/
#define SISL_OFFSET_PHASE_DONE      4  /* finished                        */

/* Opaque session type.  One session owns an independent copy of the
   source surface and all of its working state; two sessions never share
   mutable state, so they may be advanced alternately. */

typedef struct SISLOffsetJob SISLOffsetJob;

/* Per-step statistics.  "recent" refers to the step that just ran,
   "cumulative" to the whole session. */

typedef struct SISLOffsetJobStats
{
  int status;          /* one of the SISL_OFFSET_JOB_* status/error codes */
  int evaluations;     /* real s1221/s1227/s1424 calls in this step       */
  int evaluations_total;
  int phase;           /* SISL_OFFSET_PHASE_*                             */
  int boundary;        /* edge index currently in progress, or -1         */
  int result_taken;    /* 1 once take_result() has handed the surface out */
} SISLOffsetJobStats;

/* Create a session that will approximate the offset surface of ps.

   The arguments mirror s1365(): aoffset is the signed offset distance,
   aepsge the geometric tolerance, amax the maximal stepping length
   (0 to derive it from the surface box, as in s1365()) and idim the
   ambient dimension (3 is required).

   Only validation, input copying and state initialisation happen here;
   no curve or surface evaluation is performed.  The source surface is
   copied, so it may be modified or freed once create returns.

   Returns NULL on failure; *jstat receives a negative code. */

SISLOffsetJob *sisl_offset_job_create(const SISLSurf *ps, double aoffset,
                                       double aepsge, double amax, int idim,
                                       int *jstat);

/* Advance the session by at most budget real evaluations.

   budget must be non-negative.  With budget 0 the session is not touched
   and no evaluator is called.  The step stops before the evaluation that
   would exceed the budget, so the evaluation sequence observed for any
   split of the budget is identical to the sequence for one large budget.

   On return *stats describes the step and the session.  Returns
   SISL_OFFSET_JOB_MORE while work remains, SISL_OFFSET_JOB_DONE once the
   result is available, SISL_OFFSET_JOB_CANCELLED after cancellation, or a
   negative error code (also reported in stats->status).  A negative budget
   returns SISL_OFFSET_JOB_ERR_BUDGET without changing the session. */

int sisl_offset_job_step(SISLOffsetJob *job, int budget,
                         SISLOffsetJobStats *stats);

/* Hand out the finished offset surface.

   Before DONE this returns NULL and leaves *rs empty.  The first call
   after DONE transfers ownership of the surface to the caller; later
   calls report SISL_OFFSET_JOB_ERR_STATE through *jstat and hand out
   nothing, so the surface is never owned twice.  *jstat is 0 on success. */

SISLSurf *sisl_offset_job_take_result(SISLOffsetJob *job, int *jstat);

/* Cancel the session.  Before DONE this releases the unfinished work and
   moves the session to CANCELLED; repeated calls are harmless and later
   steps do not evaluate.  After DONE the call is a no-op: the state and
   the (possibly already taken) result are untouched.  Returns 0 if the
   session is CANCELLED afterwards, SISL_OFFSET_JOB_ERR_ARGUMENT for a
   NULL job. */

int sisl_offset_job_cancel(SISLOffsetJob *job);

/* Release a session and any work it still owns.  A result already handed
   out by take_result() belongs to the caller and is not freed here.  A
   failed session can be freed the same way.  NULL is accepted. */

void sisl_offset_job_free(SISLOffsetJob *job);

#ifdef __cplusplus
}
#endif

#endif /* SISL_OFFSET_JOB_H */
