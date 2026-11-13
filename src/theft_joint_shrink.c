#include "theft_joint_shrink.h"

#include "theft_call.h"
#include "theft_trial.h"

#include <string.h>
#include <assert.h>

#define LOG_JOINT 0

bool
theft_joint_shrink_enabled(const struct theft *t) {
    return t->joint.shrink != NULL && t->joint.rank != NULL;
}

/* Free every instance the joint callback wrote into OUTPUT. The
 * candidate was not accepted, so the current arguments stay live. */
static void
discard_candidate(struct theft *t, void **output) {
    for (uint8_t i = 0; i < t->prop.arity; i++) {
        if (output[i] != NULL) {
            struct theft_type_info *ti = t->prop.type_info[i];
            if (ti->free != NULL) {
                ti->free(output[i], ti->env);
            }
            output[i] = NULL;
        }
    }
}

/* Commit the candidate: its replacement instances were already
 * installed in the trial args for hashing, so only the saved old
 * instances are freed. Unreplaced slots are left untouched. */
static void
accept_candidate(struct theft *t, void **output, void **previous) {
    for (uint8_t i = 0; i < t->prop.arity; i++) {
        if (output[i] != NULL) {
            struct theft_type_info *ti = t->prop.type_info[i];
            if (ti->free != NULL) {
                ti->free(previous[i], ti->env);
            }
            output[i] = NULL;
        }
    }
}

static enum theft_hook_joint_shrink_post_res
joint_shrink_post_hook(struct theft *t, void **args, uint32_t tactic,
    enum theft_trial_res result) {
    if (t->hooks.joint_shrink_post == NULL) {
        return THEFT_HOOK_JOINT_SHRINK_POST_CONTINUE;
    }

    struct theft_hook_joint_shrink_post_info info = {
        .prop_name = t->prop.name,
        .total_trials = t->prop.trial_count,
        .trial_id = t->trial.trial,
        .run_seed = t->seeds.run_seed,
        .trial_seed = t->trial.seed,
        .arity = t->prop.arity,
        .shrink_count = t->trial.shrink_count,
        .successful_shrinks = t->trial.successful_shrinks,
        .failed_shrinks = t->trial.failed_shrinks,
        .args = args,
        .tactic = tactic,
        .result = result,
    };
    return t->hooks.joint_shrink_post(&info, t->hooks.env);
}

/* Validate an OK result from the joint callback: at least two
 * replacement slots, none of them autoshrink-managed. On failure,
 * clean up the callback's candidate and return false. */
static bool
validate_candidate(struct theft *t, void **output) {
    size_t replaced = 0;
    bool touches_autoshrink = false;

    for (uint8_t i = 0; i < t->prop.arity; i++) {
        if (output[i] != NULL) {
            replaced++;
            if (t->trial.args[i].type == ARG_AUTOSHRINK) {
                touches_autoshrink = true;
            }
        }
    }

    if (replaced < 2 || touches_autoshrink) {
        LOG(1 - LOG_JOINT,
            "joint shrink candidate rejected: replaced %zd, autoshrink %d\n",
            replaced, touches_autoshrink);
        discard_candidate(t, output);
        return false;
    }
    return true;
}

/* Assemble the complete candidate group in CAND, using replacements
 * from OUTPUT and the current instances for NULL slots. */
static void
assemble_candidate(struct theft *t, void **output, void **cand) {
    for (uint8_t i = 0; i < t->prop.arity; i++) {
        cand[i] = (output[i] != NULL
            ? output[i]
            : t->trial.args[i].instance);
    }
}

enum joint_round_res
theft_joint_shrink_round(struct theft *t) {
    assert(theft_joint_shrink_enabled(t));
    const uint8_t arity = t->prop.arity;

    /* CURRENT owns the borrowed pointers to the live arguments; the
     * const view is what the callbacks are allowed to see. */
    void *current[THEFT_MAX_ARITY];
    const void *current_view[THEFT_MAX_ARITY];
    for (uint8_t i = 0; i < arity; i++) {
        current[i] = t->trial.args[i].instance;
        current_view[i] = current[i];
    }
    const uint64_t current_rank = t->joint.rank(current_view, arity,
        t->joint.env);

    for (uint32_t tactic = 0; tactic < THEFT_MAX_TACTICS; tactic++) {
        void *output[THEFT_MAX_ARITY];
        memset(output, 0, sizeof(output));

        enum theft_shrink_res sres = t->joint.shrink(t, current_view,
            arity, tactic, t->joint.env, output);

        switch (sres) {
        case THEFT_SHRINK_DEAD_END:
            /* Nothing for this tactic; try the next one. */
            continue;
        case THEFT_SHRINK_NO_MORE_TACTICS:
            return JOINT_ROUND_DONE;
        case THEFT_SHRINK_OK:
            break;
        case THEFT_SHRINK_ERROR:
        default:
            discard_candidate(t, output);
            return JOINT_ROUND_ERROR;
        }

        /* One group-level shrinking attempt per OK candidate. */
        t->trial.shrink_count++;

        if (!validate_candidate(t, output)) {
            return JOINT_ROUND_ERROR;
        }

        void *cand[THEFT_MAX_ARITY];
        assemble_candidate(t, output, cand);

        /* Only strictly lower-ranked candidates reach the property. */
        const void *cand_view[THEFT_MAX_ARITY];
        for (uint8_t i = 0; i < arity; i++) {
            cand_view[i] = cand[i];
        }
        uint64_t cand_rank = t->joint.rank(cand_view, arity,
            t->joint.env);
        if (cand_rank >= current_rank) {
            LOG(2 - LOG_JOINT,
                "joint tactic %u: rank %" PRIu64 " >= %" PRIu64
                ", discarding\n", tactic, cand_rank, current_rank);
            t->trial.failed_shrinks++;
            discard_candidate(t, output);
            continue;
        }

        /* Deduplication uses the fully assembled group: swap the
         * replacement slots in before hashing, and swap them back out
         * if the bloom filter says this group was already tried. */
        for (uint8_t i = 0; i < arity; i++) {
            if (output[i] != NULL) {
                t->trial.args[i].instance = output[i];
            }
        }

        if (t->bloom && theft_call_check_called(t)) {
            LOG(2 - LOG_JOINT,
                "joint tactic %u: duplicate group, discarding\n", tactic);
            for (uint8_t i = 0; i < arity; i++) {
                if (output[i] != NULL) {
                    t->trial.args[i].instance = current[i];
                }
            }
            t->trial.failed_shrinks++;
            discard_candidate(t, output);
            continue;
        }
        if (t->bloom) {
            theft_call_mark_called(t);
        }

        enum theft_trial_res res = theft_call(t, cand);
        LOG(2 - LOG_JOINT, "joint tactic %u: property -> %d\n",
            tactic, res);

        enum theft_hook_joint_shrink_post_res hres =
            joint_shrink_post_hook(t, cand, tactic, res);

        switch (res) {
        case THEFT_TRIAL_FAIL:
            if (hres == THEFT_HOOK_JOINT_SHRINK_POST_ERROR) {
                /* Keep the failing candidate as the current group,
                 * then report the run error; trial cleanup still frees
                 * every object exactly once. */
                accept_candidate(t, output, current);
                return JOINT_ROUND_ERROR;
            }
            t->trial.successful_shrinks++;
            accept_candidate(t, output, current);
            if (hres == THEFT_HOOK_JOINT_SHRINK_POST_HALT) {
                return JOINT_ROUND_HALT;
            }
            /* A new group was accepted: the caller restarts the
             * per-argument pass and joint tactics from 0. */
            return JOINT_ROUND_ACCEPTED;
        case THEFT_TRIAL_PASS:
        case THEFT_TRIAL_SKIP:
            /* Revert to the complete old group; only the new
             * instances are released. */
            for (uint8_t i = 0; i < arity; i++) {
                if (output[i] != NULL) {
                    t->trial.args[i].instance = current[i];
                }
            }
            t->trial.failed_shrinks++;
            discard_candidate(t, output);
            if (hres == THEFT_HOOK_JOINT_SHRINK_POST_ERROR) {
                return JOINT_ROUND_ERROR;
            } else if (hres == THEFT_HOOK_JOINT_SHRINK_POST_HALT) {
                return JOINT_ROUND_HALT;
            }
            continue;
        default:
        case THEFT_TRIAL_ERROR:
        case THEFT_TRIAL_DUP:
            /* Property error: retain the old counter-example group
             * and report the run error as usual. */
            for (uint8_t i = 0; i < arity; i++) {
                if (output[i] != NULL) {
                    t->trial.args[i].instance = current[i];
                }
            }
            discard_candidate(t, output);
            return JOINT_ROUND_ERROR;
        }
    }

    return JOINT_ROUND_DONE;
}
