# Resumable offset-surface sessions

`include/sisl_offset_job.h` exposes the offset-surface approximation of
SISL's `s1365` as a session that can be advanced in many small,
interleaved steps instead of one blocking call.  The old synchronous
entry point keeps its signature and behaviour; both run the same kernel,
so the geometry cannot drift between the two.

## Supported class

* Three-dimensional surfaces (`idim == 3`).
* Regular, open-clamped tensor-product surfaces: no periodic directions.
* Polynomial or rational with strictly positive weights.
* Degree 2 or 3 in each parameter direction, i.e. spline order 3 or 4.
* At least C2 in the interior (the analysis of the adaptive curve
  stepping in `s1360` relies on it).
* Both the surface and its offset are free of singularities.
* Positive offset along the unit direction of `Su x Sv`.

Nothing beyond the synchronous path is added: no periodic or degenerate
surfaces.  The offset distance, geometric tolerance and maximal step
length follow exactly the rules of `s1365` (`amax == 0` derives the step
bound from the surface box, as before).

## API

```c
typedef struct SISLOffsetJob SISLOffsetJob;

SISLOffsetJob *sisl_offset_job_create(const SISLSurf *ps,
                                       double aoffset, double aepsge,
                                       double amax, int idim, int *jstat);

int  sisl_offset_job_step(SISLOffsetJob *job, int budget,
                          SISLOffsetJobStats *stats);
SISLSurf *sisl_offset_job_take_result(SISLOffsetJob *job, int *jstat);
int  sisl_offset_job_cancel(SISLOffsetJob *job);
void sisl_offset_job_free(SISLOffsetJob *job);
```

### `create`

Validates the arguments, copies the input surface and initialises the
session state.  It performs **no** curve or surface evaluation.  Because
the surface is copied, the caller may modify or free `ps` as soon as
`create` returns.  It returns `NULL` and stores one of

| code                          | meaning                          |
|-------------------------------|----------------------------------|
| `SISL_OFFSET_JOB_ERR_ARGUMENT`| invalid surface, dimension, tolerance or step bound |
| `SISL_OFFSET_JOB_ERR_MEMORY`  | allocation failed                |

through `*jstat`.

### `step`

Advances the session by at most `budget` **real evaluations** and fills
`stats`.  It returns

| return                        | meaning                                        |
|-------------------------------|------------------------------------------------|
| `SISL_OFFSET_JOB_MORE` (1)    | budget spent, work remains                     |
| `SISL_OFFSET_JOB_DONE` (0)    | the result surface is ready                    |
| `SISL_OFFSET_JOB_CANCELLED`(-1)| the session was cancelled                     |
| negative                      | a specific error (see below)                   |

A *real evaluation* is exactly one call to `s1221`, `s1227` or `s1424`.
The indirect evaluations performed inside `s1362` and `s1367`, and the
evaluations of rejected trial steps, are counted once each.  Nothing is
counted twice: the accounting happens at the real evaluation call sites,
not in the caller of the evaluator.

Budget semantics:

* `budget < 0` is rejected with `SISL_OFFSET_JOB_ERR_BUDGET`; the session
  is not touched and no evaluator is called.
* `budget == 0` changes no state and calls no evaluator.
* The session stops **before** the evaluation that would exceed the
  budget; it never performs the extra evaluation and then refunds it.
* The sequence of evaluation calls - names, arguments and order - and the
  final knots and control points are identical for every way of splitting
  the budget.  Advancing with budget 1 repeatedly therefore reproduces
  one large step exactly.

`stats` (all fields valid on return, including on error terms):

| field               | meaning                                          |
|---------------------|--------------------------------------------------|
| `status`            | same as the return value                         |
| `evaluations`       | real evaluations performed by this step          |
| `evaluations_total` | real evaluations in the whole session            |
| `phase`             | `SISL_OFFSET_PHASE_*`                            |
| `boundary`          | edge index `0..3` in progress, or `-1`           |
| `result_taken`      | non-zero once `take_result` has handed out the surface |

A step that reports `DONE`, `CANCELLED` or an error without doing work
reports `evaluations == 0` for that call; `evaluations_total` still
reflects the whole session.

Phases: `EDGES` (0, extracting edge curves), `CURVES` (1, offsetting the
four edge curves), `COMMON` (2, common basis for opposite edges),
`SURFACE` (3, evaluating and interpolating the surface), `DONE` (4).

### `take_result`

Before `DONE` it returns `NULL`, leaves `*rs` with nothing and reports
`SISL_OFFSET_JOB_ERR_STATE` through `*jstat`.  The first call after
`DONE` transfers ownership: the returned `SISLSurf` is an independent
object the caller frees with `freeSurf`.  A second call reports
`SISL_OFFSET_JOB_ERR_STATE` and hands out nothing, so the surface is
never owned twice.

### `cancel`

Before `DONE`, `cancel` releases the unfinished work and moves the
session to `CANCELLED`; repeated calls are harmless and later `step`
calls do not evaluate.  After `DONE`, `cancel` is a no-op: neither the
state nor the result (taken or not) is changed.  A result the caller has
already taken is never destroyed.  `cancel(NULL)` returns
`SISL_OFFSET_JOB_ERR_ARGUMENT`.

### `free`

Releases the session and any work it still owns.  A result already taken
with `take_result` belongs to the caller and is not freed.  A failed or
cancelled session may be freed the same way, and `free(NULL)` is
accepted.  Freeing a session that is still suspended mid-work is safe:
the kernel is unwound first, releasing its temporary curves and arrays.

## Errors and cleanup

| code                          | meaning                                        |
|-------------------------------|------------------------------------------------|
| `SISL_OFFSET_JOB_ERR_ARGUMENT`| invalid argument to a create/step/take call    |
| `SISL_OFFSET_JOB_ERR_BUDGET`  | negative step budget                           |
| `SISL_OFFSET_JOB_ERR_STATE`   | illegal sequence (e.g. taking early)           |
| `SISL_OFFSET_JOB_ERR_MEMORY`  | allocation failed                              |
| `SISL_OFFSET_JOB_ERR_NUMERIC` | non-finite geometry / inconsistent unwind      |
| any other negative SISL code  | a geometry status forwarded from the kernel    |

A failed session is queryable (`step` returns the error again, `stats`
carries it) and can be freed; it never hands out a partial surface.
Error cleanup unwinds the same way cancellation does, so a session that
fails or is cancelled in the middle of a trial step, after some edge
curves are complete, or during growth of the surface evaluation grid
does not corrupt later sessions.

## Statefulness and ownership

A session owns its own copy of the source and all of its working state.
Two sessions can be advanced alternately; they share no static cursor,
work array or output.  The only shared state is the pair of evaluation
hooks through which the currently running session accounts for
evaluations; they are non-null only for the duration of a `step` or
`cancel` call, and are restored to null on return.

## How it works

`sisl_offset_surface_run` is the shared kernel, called by both `s1365`
and a session.  A session runs it on a small coroutine
(`getcontext`/`swapcontext`).  Every real evaluation on the offset path
passes through `SISL_OFFSET_EVAL_GATE()` (`include/sislP.h`), which
first asks the session whether it may proceed (suspending the coroutine
when the budget is spent) and then records the evaluation.  Because the
kernel's stack and locals survive suspension, pausing never loses a
partially accepted trial step and never recomputes from scratch.
Cancellation unwinds the kernel by returning `SISL_OFFSET_ABORT`, which
propagates through the original error paths of `s1360`, `s1367` and the
data-reduction chain, freeing their temporary storage on the way out.

The hooks are null for ordinary library callers, so the synchronous
entry points are numerically unchanged.

## Build

`src/sisl_offset_job.c` uses `ucontext`, so it needs
`_XOPEN_SOURCE 600` on POSIX systems (the public header arranges that).
No threads, subprocesses, windows, GPU or runtime downloads are used.
