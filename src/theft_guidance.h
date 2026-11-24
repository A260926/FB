#ifndef THEFT_GUIDANCE_H
#define THEFT_GUIDANCE_H

#include "theft_types_internal.h"
#include "theft_autoshrink.h"

/* Internal guidance/corpus state. The public entry point
 * theft_run_guided is declared in theft.h. A NULL
 * t->guidance means an ordinary theft_run, whose paths stay inert. */

/* Begin one primary guided generation: reset per-trial cover/decision
 * state and choose fresh generation or a rotated donor replay. The
 * donor cursor is advanced here, up front, so SKIP/DUP still move it. */
void theft_guide_begin_generation(struct theft *t);

/* Allocate argument ARG_I: either through the normal autoshrink
 * generator (fresh) or by replaying the selected donor tape. */
enum theft_alloc_res
theft_guide_alloc_arg(struct theft *t, uint8_t arg_i,
    struct autoshrink_env *env, void **output);

/* True when a generated argument pool is within the guided per-argument
 * consumed-bit / positive-request caps. */
bool theft_guide_pool_within_limits(const struct autoshrink_env *env);

/* Wrap the single primary property call, arming feature collection
 * in-process and (via the fork codec below) in the forked child. */
enum theft_trial_res
theft_guide_primary_call(struct theft *t, void **args);

/* Turn the primary verdict + collected mask into an admission decision,
 * invoke the guidance post callback once, and (for gen SKIP/DUP) report
 * those verdicts too. Returns false only on fatal guidance OOM. */
bool theft_guide_publish(struct theft *t, enum theft_trial_res result);

/* Free the run-owned corpus and guidance state. */
void theft_guide_free(struct theft_guidance *g);

/* ---- fork transport (fixed 9-byte little-endian message) ---- */

/* In the child, after fork_post: arm cover for a primary call. */
void theft_guide_fork_child_primary_begin(struct theft *t);

/* In the child, after the property call: disarm and return the mask. */
uint64_t theft_guide_fork_child_primary_end(struct theft *t);

/* Write [verdict:1][mask:8 LE] with full writes / EINTR retry. */
bool theft_guide_fork_write_message(int fd,
    enum theft_trial_res result, uint64_t mask);

/* Read the same message with full reads / EINTR retry, storing the
 * mask only when all nine bytes arrived. EOF before any byte maps to
 * THEFT_TRIAL_FAIL (crashed child); a transport error maps to ERROR. */
enum theft_trial_res
theft_guide_fork_read_message(struct theft *t, int fd);

#endif
