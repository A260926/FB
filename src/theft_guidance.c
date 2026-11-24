#include "theft_guidance.h"

#include "theft.h"
#include "theft_run.h"
#include "theft_call.h"

#include <string.h>
#include <unistd.h>
#include <errno.h>

/* Guided tapes are bounded per argument. Going over either cap after
 * the finite allocator returns aborts the guided run with ERROR rather
 * than asserting or silently truncating. */
#define GUIDE_MAX_CONSUMED_BITS 4096
#define GUIDE_MAX_REQUESTS 128
#define GUIDE_CORPUS_LIMIT_MAX 16

/* SplitMix64-style mixing constant used for the per-argument private
 * extension RNG seed. Multiplication wraps mod 2^64. */
#define GUIDE_EXT_SEED_CONST UINT64_C(0x9e3779b97f4a7c15)

struct guide_tape {
    /* Packed consumed bits, low-bit-first words; NULL when bits == 0. */
    uint64_t *words;
    size_t bits;

    /* Each positive random request's width, in request order. */
    uint32_t *requests;
    size_t request_count;
};

struct guide_entry {
    uint64_t id;                 /* monotonic admission id */
    struct guide_tape args[THEFT_MAX_ARITY];
};

struct theft_guidance {
    size_t corpus_limit;
    theft_guidance_post_cb *post;
    void *post_env;
    uint8_t arity;

    /* FIFO: entries[0] is the oldest. */
    struct guide_entry *entries[GUIDE_CORPUS_LIMIT_MAX];
    size_t count;

    uint64_t next_admission_id;
    uint64_t next_donor_id;
    uint64_t ever_seen;

    /* Feature collection for the active primary property call. */
    bool cover_active;
    uint64_t cover_mask;

    /* Set around the one primary theft_call so a forked child knows to
     * arm cover after the fork_post hook. */
    bool primary_armed;
    uint64_t primary_mask;

    /* This trial's generation decision / descriptor coordinates. */
    bool cur_mutated;
    uint64_t cur_donor_id;
    struct guide_entry *cur_donor;
    uint8_t cur_mut_arg;
    size_t cur_mut_req;
    uint32_t cur_mut_bit;
    size_t cur_flip_offset;
};

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

bool theft_cover(struct theft *t, uint8_t feature) {
    if (t == NULL || t->guidance == NULL) {
        return false;
    }
    struct theft_guidance *g = t->guidance;
    if (!g->cover_active) {
        return false;
    }
    if (feature >= 64) {
        return false;
    }
    g->cover_mask |= (UINT64_C(1) << feature);
    return true;
}

/* Validate the guided-specific configuration before any user alloc.
 * Returns the inferred arity, or 0 when arguments are invalid. */
static uint8_t
guided_valid_arity(const struct theft_run_config *cfg,
        const struct theft_guidance_config *gc) {
    if (cfg == NULL || gc == NULL) {
        return 0;
    }
    if (gc->corpus_limit < 1 || gc->corpus_limit > GUIDE_CORPUS_LIMIT_MAX) {
        return 0;
    }

    uint8_t arity = 0;
    for (uint8_t i = 0; i < THEFT_MAX_ARITY; i++) {
        if (cfg->type_info[i] == NULL) {
            break;
        }
        arity = (uint8_t)(i + 1);
    }
    if (arity < 1 || arity > THEFT_MAX_ARITY) {
        return 0;
    }

    for (uint8_t i = 0; i < arity; i++) {
        const struct theft_type_info *ti = cfg->type_info[i];
        if (ti == NULL || ti->alloc == NULL) {
            return 0;
        }
        /* Guided replay drives the real allocator through an
         * autoshrink bit pool; custom shrink callbacks are excluded. */
        if (!ti->autoshrink_config.enable || ti->shrink != NULL) {
            return 0;
        }
    }
    return arity;
}

enum theft_run_res
theft_run_guided(const struct theft_run_config *cfg,
        const struct theft_guidance_config *gc) {
    const uint8_t arity = guided_valid_arity(cfg, gc);
    if (arity == 0) {
        return THEFT_RUN_ERROR_BAD_ARGS;
    }

    struct theft *t = NULL;
    enum theft_run_init_res init_res = theft_run_init(cfg, &t);
    switch (init_res) {
    case THEFT_RUN_INIT_ERROR_MEMORY:
        return THEFT_RUN_ERROR_MEMORY;
    case THEFT_RUN_INIT_ERROR_BAD_ARGS:
        return THEFT_RUN_ERROR_BAD_ARGS;
    case THEFT_RUN_INIT_OK:
        break;
    }

    struct theft_guidance *g = calloc(1, sizeof(*g));
    if (g == NULL) {
        theft_run_free(t);
        return THEFT_RUN_ERROR_MEMORY;
    }
    g->corpus_limit = gc->corpus_limit;
    g->post = gc->post;
    g->post_env = gc->env;
    g->arity = arity;
    /* IDs and the donor cursor both start at zero; calloc already
     * gives next_admission_id / next_donor_id / ever_seen = 0. */

    t->guidance = g;

    enum theft_run_res res = theft_run_trials(t);
    theft_run_free(t);       /* frees g via theft_guide_free */
    return res;
}

void theft_guide_free(struct theft_guidance *g) {
    if (g == NULL) {
        return;
    }
    for (size_t i = 0; i < g->count; i++) {
        struct guide_entry *e = g->entries[i];
        if (e == NULL) {
            continue;
        }
        for (uint8_t a = 0; a < THEFT_MAX_ARITY; a++) {
            free(e->args[a].words);
            free(e->args[a].requests);
        }
        free(e);
    }
    free(g);
}

/* ------------------------------------------------------------------ */
/* Donor selection and replay                                          */
/* ------------------------------------------------------------------ */

static size_t
entry_request_count(const struct guide_entry *e) {
    size_t n = 0;
    for (uint8_t a = 0; a < THEFT_MAX_ARITY; a++) {
        n += e->args[a].request_count;
    }
    return n;
}

void theft_guide_begin_generation(struct theft *t) {
    struct theft_guidance *g = t->guidance;
    if (g == NULL) {
        return;
    }

    /* Reset all per-trial state to fresh-generation sentinels. */
    g->cover_active = false;
    g->primary_armed = false;
    g->cover_mask = 0;
    g->primary_mask = 0;

    g->cur_mutated = false;
    g->cur_donor_id = UINT64_MAX;
    g->cur_donor = NULL;
    g->cur_mut_arg = UINT8_MAX;
    g->cur_mut_req = SIZE_MAX;
    g->cur_mut_bit = UINT32_MAX;
    g->cur_flip_offset = 0;

    const size_t trial_id = (size_t)t->trial.trial;

    if (g->count == 0 || (trial_id % 4U) == 0) {
        return;
    }

    /* Oldest entry whose id is at least the cursor. */
    long pick = -1;
    for (size_t j = 0; j < g->count; j++) {
        struct guide_entry *e = g->entries[j];
        if (e->id >= g->next_donor_id && entry_request_count(e) > 0) {
            pick = (long)j;
            break;
        }
    }
    /* Otherwise wrap to the oldest eligible entry. */
    if (pick < 0) {
        for (size_t j = 0; j < g->count; j++) {
            if (entry_request_count(g->entries[j]) > 0) {
                pick = (long)j;
                break;
            }
        }
    }
    /* No entry with any positive request: use fresh generation. */
    if (pick < 0) {
        return;
    }

    struct guide_entry *donor = g->entries[(size_t)pick];
    g->cur_donor = donor;
    g->cur_donor_id = donor->id;
    /* The cursor advances at selection time, even if replay later
     * yields SKIP/DUP. */
    g->next_donor_id = donor->id + 1;
    g->cur_mutated = true;

    const size_t R = entry_request_count(donor);
    const size_t flat = (trial_id - 1U) % R;
    const size_t quotient = (trial_id - 1U) / R;

    /* Locate the flat-th descriptor in argument/request order and the
     * in-argument bit offset of the selected bit. */
    size_t remaining = flat;
    for (uint8_t a = 0; a < g->arity; a++) {
        const struct guide_tape *tape = &donor->args[a];
        size_t arg_offset = 0;
        for (size_t r = 0; r < tape->request_count; r++) {
            const uint32_t width = tape->requests[r];
            if (remaining == 0) {
                const uint32_t bit = (width > 0)
                    ? (uint32_t)(quotient % (size_t)width)
                    : 0U;
                g->cur_mut_arg = a;
                g->cur_mut_req = r;
                g->cur_mut_bit = bit;
                g->cur_flip_offset = arg_offset + (size_t)bit;
                return;
            }
            remaining--;
            arg_offset += width;
        }
    }
    /* R > 0 and flat < R guarantees a descriptor is found. */
}

enum theft_alloc_res
theft_guide_alloc_arg(struct theft *t, uint8_t arg_i,
        struct autoshrink_env *env, void **output) {
    struct theft_guidance *g = t->guidance;
    if (g == NULL || !g->cur_mutated || g->cur_donor == NULL) {
        /* Existing trial-seed autoshrink generation, unchanged. */
        return theft_autoshrink_alloc(t, env, output);
    }

    const struct guide_tape *tape = &g->cur_donor->args[arg_i];
    const uint64_t ext_seed = t->trial.seed
        ^ (GUIDE_EXT_SEED_CONST * (uint64_t)(arg_i + 1U));
    const bool do_flip = (g->cur_mut_arg == arg_i);

    return theft_autoshrink_guided_replay_alloc(t, env,
        tape->words, tape->bits,
        do_flip, g->cur_flip_offset,
        ext_seed, output);
}

bool theft_guide_pool_within_limits(const struct autoshrink_env *env) {
    if (env == NULL || env->bit_pool == NULL) {
        return true;
    }
    const struct autoshrink_bit_pool *p = env->bit_pool;
    return p->consumed <= GUIDE_MAX_CONSUMED_BITS
        && p->request_count <= GUIDE_MAX_REQUESTS;
}

/* ------------------------------------------------------------------ */
/* Primary call / feature mask                                         */
/* ------------------------------------------------------------------ */

enum theft_trial_res
theft_guide_primary_call(struct theft *t, void **args) {
    struct theft_guidance *g = t->guidance;

    g->primary_armed = true;
    g->cover_mask = 0;
    g->primary_mask = 0;

    /* In process mode the property runs directly here. In fork mode
     * cover stays inactive across fork/post and is armed in the child
     * by theft_guide_fork_child_primary_begin; the parent's mask is
     * populated by the fork message read. */
    if (!t->fork.enable) {
        g->cover_active = true;
    }

    enum theft_trial_res res = theft_call(t, args);

    if (!t->fork.enable) {
        g->cover_active = false;
    }
    g->primary_armed = false;
    g->primary_mask = g->cover_mask;
    return res;
}

void theft_guide_fork_child_primary_begin(struct theft *t) {
    struct theft_guidance *g = t->guidance;
    if (g == NULL) {
        return;
    }
    /* fork_post has already run with cover inactive. */
    if (g->primary_armed) {
        g->cover_mask = 0;
        g->cover_active = true;
    }
}

uint64_t theft_guide_fork_child_primary_end(struct theft *t) {
    struct theft_guidance *g = t->guidance;
    if (g == NULL) {
        return 0;
    }
    g->cover_active = false;
    return g->cover_mask;
}

/* ------------------------------------------------------------------ */
/* Fork mask transport                                                 */
/* ------------------------------------------------------------------ */

bool theft_guide_fork_write_message(int fd,
        enum theft_trial_res result, uint64_t mask) {
    uint8_t msg[9];
    msg[0] = (uint8_t)result;
    for (unsigned i = 0; i < 8; i++) {
        msg[1 + i] = (uint8_t)(mask >> (8U * i));
    }

    size_t off = 0;
    while (off < sizeof(msg)) {
        ssize_t wr = write(fd, msg + off, sizeof(msg) - off);
        if (wr > 0) {
            off += (size_t)wr;
        } else if (wr < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

enum theft_trial_res
theft_guide_fork_read_message(struct theft *t, int fd) {
    uint8_t msg[9];
    size_t off = 0;

    while (off < sizeof(msg)) {
        ssize_t rd = read(fd, msg + off, sizeof(msg) - off);
        if (rd > 0) {
            off += (size_t)rd;
        } else if (rd == 0) {
            break;                  /* EOF */
        } else if (errno == EINTR) {
            continue;
        } else {
            return THEFT_TRIAL_ERROR;
        }
    }

    if (off == 0) {
        /* Closed without any response: treated as a crash. */
        return THEFT_TRIAL_FAIL;
    }

    enum theft_trial_res result = (enum theft_trial_res)msg[0];
    uint64_t mask = 0;
    if (off == sizeof(msg)) {
        for (unsigned i = 0; i < 8; i++) {
            mask |= (uint64_t)msg[1 + i] << (8U * i);
        }
    }
    /* A short/incomplete message yields a zero mask: never publish a
     * partial mask. The verdict byte itself is still preserved. */

    if (t->guidance != NULL) {
        t->guidance->cover_mask = mask;
    }
    return result;
}

/* ------------------------------------------------------------------ */
/* Corpus capture / admission / post                                   */
/* ------------------------------------------------------------------ */

static struct guide_entry *
capture_entry(struct theft *t, struct theft_guidance *g) {
    struct guide_entry *e = calloc(1, sizeof(*e));
    if (e == NULL) {
        return NULL;
    }

    for (uint8_t a = 0; a < g->arity; a++) {
        struct autoshrink_env *env = t->trial.args[a].u.as.env;
        /* Guided runs require autoshrink for every argument. */
        struct autoshrink_bit_pool *p = env ? env->bit_pool : NULL;
        struct guide_tape *tape = &e->args[a];
        if (p == NULL) {
            continue;
        }

        tape->bits = p->consumed;
        tape->request_count = p->request_count;

        const size_t word_count = (p->consumed + 63U) / 64U;
        if (word_count > 0) {
            tape->words = malloc(word_count * sizeof(*tape->words));
            if (tape->words == NULL) {
                goto fail;
            }
            /* bits_filled is a 64-rounded count >= consumed, so the
             * packed consumed words are all valid. */
            memcpy(tape->words, p->bits, word_count * sizeof(*tape->words));
        }

        if (p->request_count > 0) {
            tape->requests =
                malloc(p->request_count * sizeof(*tape->requests));
            if (tape->requests == NULL) {
                goto fail;
            }
            memcpy(tape->requests, p->requests,
                p->request_count * sizeof(*tape->requests));
        }
    }

    return e;

fail:
    for (uint8_t a = 0; a < THEFT_MAX_ARITY; a++) {
        free(e->args[a].words);
        free(e->args[a].requests);
    }
    free(e);
    return NULL;
}

bool theft_guide_publish(struct theft *t, enum theft_trial_res result) {
    struct theft_guidance *g = t->guidance;
    if (g == NULL) {
        return true;
    }

    const uint64_t feature_mask = g->primary_mask;
    uint64_t novel_mask = 0;
    uint64_t admitted_id = UINT64_MAX;

    if (result == THEFT_TRIAL_PASS) {
        const uint64_t novel = feature_mask & ~g->ever_seen;
        if (novel != 0) {
            /* Admit immediately, while the live pools/args are still
             * allocated, by deep-copying the consumed tapes. */
            struct guide_entry *e = capture_entry(t, g);
            if (e == NULL) {
                return false;       /* fatal memory failure */
            }
            e->id = g->next_admission_id;

            if (g->count == g->corpus_limit) {
                /* Fixed capacity: evict the oldest entry FIFO. This
                 * only frees deep tape copies; it never clears
                 * ever_seen and cannot invalidate the current replay
                 * (whose pool already copied the donor bits). */
                struct guide_entry *oldest = g->entries[0];
                for (uint8_t a = 0; a < THEFT_MAX_ARITY; a++) {
                    free(oldest->args[a].words);
                    free(oldest->args[a].requests);
                }
                free(oldest);
                memmove(&g->entries[0], &g->entries[1],
                    (g->corpus_limit - 1) * sizeof(g->entries[0]));
                g->count--;
            }

            g->entries[g->count] = e;
            g->count++;
            admitted_id = e->id;
            g->next_admission_id++;
            novel_mask = novel;
        }
        /* OR-ing every PASS mask is equivalent to OR-ing only the
         * novel subset, since non-novel bits are already in ever_seen.
         * FAIL/SKIP/DUP never update ever_seen. */
        g->ever_seen |= feature_mask;
    }

    struct theft_guidance_info info = {
        .trial_id = (size_t)t->trial.trial,
        .result = result,
        .mutated = g->cur_mutated,
        .donor_id = g->cur_donor_id,
        .admitted_id = admitted_id,
        .mutation_arg = g->cur_mut_arg,
        .mutation_request = g->cur_mut_req,
        .mutation_bit = g->cur_mut_bit,
        .feature_mask = feature_mask,
        .novel_mask = novel_mask,
        .corpus_size = g->count,
    };

    /* Admission may have evicted the selected donor entry (possibly
     * the donor itself). The replay pool already owns its copied bits,
     * so drop the transient borrowed pointer rather than leave it
     * dangling; the id/coordinates in INFO are value copies. */
    g->cur_donor = NULL;

    if (g->post != NULL) {
        g->post(&info, g->post_env);
    }
    return true;
}
