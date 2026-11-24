#ifndef KVZ_LTR_H_
#define KVZ_LTR_H_

#include "cu.h"
#include "image.h"
#include "threadqueue.h"

struct encoder_state_t;

#define KVZ_LTR_MAX_SLOTS 2

typedef struct kvz_ltr_slot {
  int initialized;
  int32_t poc;
  kvz_picture *pic;
  cu_array_t *cu;
  threadqueue_job_t *done_job;
} kvz_ltr_slot_t;

typedef struct kvz_ltr {
  int enabled;
  int accepted_pics;
  kvz_ltr_slot_t slots[KVZ_LTR_MAX_SLOTS];
} kvz_ltr_t;

void kvz_ltr_init(kvz_ltr_t *ltr, int enabled);
void kvz_ltr_destroy(kvz_ltr_t *ltr);

/* Validate a public input before it is fed to the encoder. Does not mutate
 * any encoder, input-sequence, slot or reference state. */
int kvz_ltr_validate_input(const kvz_ltr_t *ltr, const kvz_picture *pic);

void kvz_ltr_reconcile_refs(struct encoder_state_t *state);
void kvz_ltr_apply_ref_lists(struct encoder_state_t *state);
void kvz_ltr_commit_mark(struct encoder_state_t *state);
int kvz_ltr_msb_present(const struct encoder_state_t *state, int32_t lt_poc);
int kvz_ltr_msb_cycle_delta(const struct encoder_state_t *state, int32_t lt_poc);

#endif /* KVZ_LTR_H_ */
