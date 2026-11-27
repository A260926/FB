/* libSoX internal: joint cut-plan support shared with the regression tests.
 *
 * This header is not part of the public libSoX API.  It exposes the exact
 * planner entry point and a few counters so that tests can verify the
 * planner independently and can check the PCM memory bound without having
 * to guess at private effect state.
 */

#ifndef LSX_SPLICE_CUT_PLAN_H
#define LSX_SPLICE_CUT_PLAN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LSX_CUT_PLAN_MAX_CUTS   64u
#define LSX_CUT_PLAN_MAX_CHANS  8u
#define LSX_CUT_PLAN_MAX_W      1024u
#define LSX_CUT_PLAN_MAX_S      64u

/* One deletion interval, in frames of the original input. */
typedef struct {
  uint64_t a, b;             /* half-open [a,b) */
} lsx_cut_plan_interval_t;

typedef struct {
  size_t   pcm_capacity_samples; /* PCM sample count allocated for buffers */
  size_t   pcm_peak_samples;     /* high-water mark of allocated PCM */
  size_t   pcm_live_samples;     /* PCM sample count allocated right now */
  uint64_t temp_bytes_written;   /* total bytes spooled to temporary storage */
  unsigned temp_files_opened;    /* temporary files opened so far */
  unsigned temp_files_closed;    /* temporary files closed so far */
  unsigned temp_io_errors;       /* failed temp-file read/write/flush calls */
} lsx_splice_cut_plan_stats_t;

/* The statistics above are process-global; tests may reset them. */
void lsx_cutplan_stats_reset(void);
void lsx_cutplan_stats_get(lsx_splice_cut_plan_stats_t * stats);
void lsx_cutplan_stats_pcm_alloc(size_t samples);
void lsx_cutplan_stats_pcm_free(size_t samples);
void lsx_cutplan_stats_temp_open(void);
void lsx_cutplan_stats_temp_close(void);
void lsx_cutplan_stats_temp_written(uint64_t bytes);
void lsx_cutplan_stats_temp_error(void);

/*
 * Exact joint planner.  `costs' is an ncuts by (2*S+1) matrix; entry
 * costs[i*(2*S+1) + (d + S)] is the exact (integer) cost of shifting cut i
 * by d.  Entries for offsets outside the individually valid range are
 * ignored.  On success the lexicographically smallest signed offset vector
 * among all minimum-total-cost feasible vectors is written to offsets and
 * the minimum total to *total_cost.  Returns 0 on success and -1 if the
 * arguments are invalid or no feasible vector exists.
 *
 * lsx_cutplan_feasible() answers the same feasibility question directly,
 * without needing a cost matrix; the renderer uses it before consuming any
 * input.
 */
int lsx_cutplan_solve(
    const lsx_cut_plan_interval_t * cuts, unsigned ncuts,
    uint64_t frames, unsigned overlap, unsigned search,
    const unsigned __int128 * costs, int * offsets,
    unsigned __int128 * total_cost);
int lsx_cutplan_feasible(
    const lsx_cut_plan_interval_t * cuts, unsigned ncuts,
    uint64_t frames, unsigned overlap, unsigned search);

/* Individually valid offset range [*lo,*hi] for one cut; returns 0 when the
 * range is empty. */
int lsx_cutplan_offset_range(
    const lsx_cut_plan_interval_t * cut, uint64_t frames,
    unsigned overlap, unsigned search, int * lo, int * hi);

#ifdef __cplusplus
}
#endif

#endif
