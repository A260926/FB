/*
 * Ownership model for structured host-reference fuzzing.
 *
 * This header and its companion .c file deliberately depend only on the
 * C library: they must never include quickjs.h and never hold a JSValue.
 * The model records logical object identities and who is responsible for
 * each one (root slots, the fixed holder property, the pending exception).
 * The executor in ownership_replay.c drives the real engine and compares
 * its observations against these purely logical expectations.
 */
#ifndef FUZZ_OWNERSHIP_MODEL_H
#define FUZZ_OWNERSHIP_MODEL_H

#include <stddef.h>
#include <stdint.h>

#define QJS_OWN_ROOT_COUNT 16u
#define QJS_OWN_MAX_IDS 64u
#define QJS_OWN_MAX_STEPS 1024u
#define QJS_OWN_PLAN_MAX_SIZE (QJS_OWN_MAX_STEPS * 3u)

/* Wire opcodes, identical to the values used in the serialized plan. */
enum qjs_ownership_op {
    QJS_OWN_OP_NEW = 0,         /* NEW a: empty root a gets a tracked object */
    QJS_OWN_OP_DUP = 1,         /* DUP a,b: duplicate live root a into root b */
    QJS_OWN_OP_DROP = 2,        /* DROP a: release root a */
    QJS_OWN_OP_THROW_TAKE = 3,  /* THROW_TAKE a: transfer root a to pending */
    QJS_OWN_OP_CATCH = 4,       /* CATCH a: transfer pending to empty root a */
    QJS_OWN_OP_GC = 5,          /* GC: run GC and check identity finalize set */
    QJS_OWN_OP_STORE_TAKE = 6,  /* STORE_TAKE a: transfer root a to holder */
    QJS_OWN_OP_CLEAR = 7,       /* CLEAR: release the holder responsibility */
    QJS_OWN_OP_REJECT_TAKE = 8, /* REJECT_TAKE a: rejected assignment, root
                                   consumed; an untracked TypeError is pending */
    QJS_OWN_OP_CALL_THROW = 9,  /* CALL_THROW a: native callback duplicates the
                                   borrowed root and throws the duplicate */
};

/* Responsibility categories. They are tracked separately even when several
 * of them point at the same logical identity. */
typedef enum qjs_ownership_resp {
    QJS_OWN_RESP_ROOT = 0,
    QJS_OWN_RESP_HOLDER,
    QJS_OWN_RESP_PENDING,
    QJS_OWN_RESP__COUNT
} qjs_ownership_resp;

/* Slot / pending markers. */
#define QJS_OWN_EMPTY (-1)       /* no value held */
#define QJS_OWN_UNTRACKED (-2)   /* untracked exception, no logical identity */

typedef struct qjs_ownership_step {
    uint8_t op;
    uint8_t a;
    uint8_t b;
} qjs_ownership_step;

typedef struct qjs_ownership_plan {
    size_t count;
    qjs_ownership_step steps[QJS_OWN_MAX_STEPS];
} qjs_ownership_plan;

/* State of one logical identity. */
typedef struct qjs_ownership_id_state {
    int used;                   /* identity has been allocated */
    int alive;                  /* still has at least one responsibility */
    int tracked;                /* tracked finalizer object (always 1 here) */
    int resp[QJS_OWN_RESP__COUNT];
    int total_resp;
} qjs_ownership_id_state;

typedef struct qjs_ownership_model {
    qjs_ownership_id_state ids[QJS_OWN_MAX_IDS];
    unsigned int next_id;       /* monotonic allocator, identities never reused */
    int slot_id[QJS_OWN_ROOT_COUNT];
    int holder_id;              /* QJS_OWN_EMPTY or a tracked identity */
    int pending_id;             /* QJS_OWN_EMPTY / QJS_OWN_UNTRACKED / tracked */
    size_t steps;               /* records applied */
    size_t created;             /* tracked logical objects created */
    size_t exceptions;          /* entries into the pending exception slot */
} qjs_ownership_model;

void qjs_ownership_model_init(qjs_ownership_model *m);

/* Apply one record after checking its preconditions against logical state
 * only. Returns 0 when the step is legal and was applied, 1 when it would
 * violate the ownership rules (slot overwrite, missing pending, ...). */
int qjs_ownership_model_apply(qjs_ownership_model *m,
                              const qjs_ownership_step *step);

/* Parse and fully validate a serialized plan without touching the engine.
 * Returns 0 on success (plan_out filled), 1 for a truncated, overlong or
 * otherwise illegal plan. A zero-length input is the valid empty plan. */
int qjs_ownership_validate(const uint8_t *data, size_t size,
                           qjs_ownership_plan *plan_out);

/* Read-only queries used by the executor and by tests. */
int qjs_ownership_model_resp(const qjs_ownership_model *m, unsigned int id,
                             qjs_ownership_resp cat);
int qjs_ownership_model_alive(const qjs_ownership_model *m, unsigned int id);
int qjs_ownership_model_finalized(const qjs_ownership_model *m,
                                  unsigned int id);
/* Fill ids[0..QJS_OWN_MAX_IDS) with 1 for every identity expected to have
 * been finalized already. Returns the number of set entries. */
size_t qjs_ownership_model_finalized_set(const qjs_ownership_model *m,
                                         int out[QJS_OWN_MAX_IDS]);

const char *qjs_ownership_resp_name(qjs_ownership_resp cat);
const char *qjs_ownership_op_name(uint8_t op);

/* Compose the single first-divergence diagnostic. It names the action index,
 * object identity, responsibility category and expected/actual values, and
 * never dumps the heap. */
void qjs_ownership_format_mismatch(char *buf, size_t buf_size,
                                   size_t step_index, uint8_t op, int id,
                                   const char *category,
                                   int expected, int actual);

#endif /* FUZZ_OWNERSHIP_MODEL_H */
