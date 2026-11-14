/* Internal cut-plan solver API (implemented in splice.c).
 *
 * The cut-plan mode chooses one signed offset d_i in [-S,S] per deletion
 * segment [a_i,b_i), jointly over the whole plan, minimising the exact
 * integer sum of squared sample differences of the cross-fade windows.
 * This header is for libSoX internals and task tests only; it is not an
 * installed public header.
 */
#ifndef SOX_CUTPLAN_H
#define SOX_CUTPLAN_H

#include "sox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reads `frames` whole interleaved frames starting at source frame `frame`
 * into dst (frames * channels samples).  Returns 0 on success, non-zero on
 * failure. */
typedef int (*lsx_cutplan_reader_t)(void * ctx, uint64_t frame,
    size_t frames, sox_sample_t * dst);

/* Structural feasibility check: does any offset sequence satisfy the
 * in-input window and adjacent-segment constraints, ignoring audio?
 * Inputs are assumed pre-validated (a<b, strictly increasing, non-overlap). */
int lsx_cutplan_feasible(uint64_t nframes,
    const uint64_t * a, const uint64_t * b, unsigned m,
    unsigned W, unsigned S);

/* Joint dynamic program.  On success returns 0, fills offsets[0..m) with
 * the lexicographically smallest optimum vector and, when non-null,
 * total_cost with the exact 128-bit combined cost.  Returns non-zero when
 * no feasible sequence exists.  `scratch` must provide 2*W*channels
 * samples of temporary PCM storage owned by the caller. */
int lsx_cutplan_solve(lsx_cutplan_reader_t reader, void * ctx,
    unsigned channels, uint64_t nframes,
    const uint64_t * a, const uint64_t * b, unsigned m,
    unsigned W, unsigned S, sox_sample_t * scratch,
    int * offsets, unsigned __int128 * total_cost);

#ifdef __cplusplus
}
#endif

#endif
