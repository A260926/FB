#ifndef THEFT_JOINT_SHRINK_H
#define THEFT_JOINT_SHRINK_H

#include "theft_types_internal.h"

/* Whether joint shrinking was configured for this run. */
bool
theft_joint_shrink_enabled(const struct theft *t);

/* Outcome of one joint shrinking round over the current argument
 * group. Each round starts joint tactics at 0. */
enum joint_round_res {
    JOINT_ROUND_ACCEPTED,       /* a FAIL candidate was accepted */
    JOINT_ROUND_DONE,           /* tactics exhausted with no accept */
    JOINT_ROUND_HALT,           /* an observer asked shrinking to stop */
    JOINT_ROUND_ERROR,          /* a hard error occurred */
};

/* Attempt joint shrinking of the current complete argument group.
 * Only called after the ordinary per-argument shrinking has reached a
 * local stall. */
enum joint_round_res
theft_joint_shrink_round(struct theft *t);

#endif
