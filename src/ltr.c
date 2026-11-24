#include "ltr.h"

#include <stdio.h>
#include <string.h>

#include "encoderstate.h"
#include "imagelist.h"
#include "kvazaar_internal.h"

void kvz_ltr_init(kvz_ltr_t *ltr, int enabled)
{
  memset(ltr, 0, sizeof(*ltr));
  ltr->enabled = enabled ? 1 : 0;
}

void kvz_ltr_destroy(kvz_ltr_t *ltr)
{
  if (!ltr) return;
  for (int i = 0; i < KVZ_LTR_MAX_SLOTS; ++i) {
    kvz_ltr_slot_t *slot = &ltr->slots[i];
    if (slot->pic) kvz_image_free(slot->pic);
    if (slot->cu) kvz_cu_array_free(&slot->cu);
    if (slot->done_job) kvz_threadqueue_free_job(&slot->done_job);
    slot->initialized = 0;
  }
  ltr->enabled = 0;
}

int kvz_ltr_validate_input(const kvz_ltr_t *ltr, const kvz_picture *pic)
{
  if (!ltr || !ltr->enabled || !pic) return 1;

  if (pic->ltr_mark < -1 || pic->ltr_mark > 1 ||
      pic->ltr_use < -1 || pic->ltr_use > 1) {
    return 0;
  }

  // The first coded picture is an IDR. It may establish a slot, but cannot
  // predict from a previously stored picture.
  if (ltr->accepted_pics == 0 && pic->ltr_use != -1) {
    return 0;
  }

  if (pic->ltr_use != -1) {
    const int s = pic->ltr_use;
    if (s < 0 || s >= KVZ_LTR_MAX_SLOTS || !ltr->slots[s].initialized) {
      return 0;
    }
  }

  return 1;
}

static int ltr_desc_find_poc(const encoder_state_t *state, int32_t poc)
{
  for (int i = 0; i < state->frame->ltr_lt_count; ++i) {
    if (state->frame->ltr_lt[i].poc == poc) return i;
  }
  return -1;
}

static int ltr_ref_find_pic(const image_list_t *refs,
                            const kvz_picture *pic,
                            int32_t poc)
{
  for (unsigned i = 0; i < refs->used_size; ++i) {
    if (refs->pocs[i] == poc || refs->images[i] == pic) return (int)i;
  }
  return -1;
}

void kvz_ltr_reconcile_refs(encoder_state_t *state)
{
  const kvz_config *cfg = &state->encoder_control->cfg;
  kvz_ltr_t *ltr = state->encoder_control->ltr;
  encoder_state_config_frame_t *f = state->frame;
  image_list_t *refs = f->ref;

  f->ltr_short_count = 0;
  f->ltr_lt_count = 0;

  if (!cfg->ltr_slots || !ltr || !ltr->enabled) return;

  // Snapshot the RPS set before applying this picture's mark. A mark updates
  // the slot only after the current frame has been set up, so use+mark on one
  // slot still predicts from the old slot picture.
  for (int s = 0; s < KVZ_LTR_MAX_SLOTS; ++s) {
    kvz_ltr_slot_t *slot = &ltr->slots[s];
    if (!slot->initialized || !slot->pic) continue;

    int d = ltr_desc_find_poc(state, slot->poc);
    if (d < 0) {
      d = f->ltr_lt_count++;
      f->ltr_lt[d].poc = slot->poc;
      f->ltr_lt[d].slot = s;
      f->ltr_lt[d].used_by_curr = (f->ltr_use == s);
    } else if (f->ltr_use == s) {
      // Two slots may alias one DPB image/POC.
      f->ltr_lt[d].used_by_curr = 1;
    }
  }

  if (f->ltr_use != -1) {
    kvz_ltr_slot_t *slot = &ltr->slots[f->ltr_use];
    // The validation at the API boundary makes this initialized. Wait for its
    // reconstruction jobs before search reads the pixels/CU data.
    if (slot->done_job && state->encoder_control->threadqueue) {
      kvz_threadqueue_waitfor(state->encoder_control->threadqueue, slot->done_job);
    }
  }

  // Make every retained LTR picture present in the one DPB image list. Add
  // each distinct image only once even if two slots alias it.
  for (int s = 0; s < KVZ_LTR_MAX_SLOTS; ++s) {
    kvz_ltr_slot_t *slot = &ltr->slots[s];
    if (!slot->initialized || !slot->pic || !slot->cu) continue;

    int idx = ltr_ref_find_pic(refs, slot->pic, slot->poc);
    if (idx < 0) {
      uint8_t ref_lx[2][16] = { { 0 } };
      if (!kvz_image_list_add(refs, slot->pic, slot->cu, slot->poc, ref_lx)) {
        fprintf(stderr, "LTR: failed to retain reference POC %d\n", slot->poc);
      }
    }
  }

  const int target = cfg->ref_frames;

  // Keep the newest ref_frames pictures as the sliding window, counting LTR
  // pictures that happen to be in that window. Independently keep every LTR
  // picture. Remove everything else.
  for (int i = (int)refs->used_size - 1; i >= 0; --i) {
    const int32_t poc = refs->pocs[i];
    if (poc >= f->poc || ltr_desc_find_poc(state, poc) >= 0) continue;

    int newer = 0;
    for (unsigned j = 0; j < refs->used_size; ++j) {
      if ((int)j == i) continue;
      if (refs->pocs[j] < f->poc && refs->pocs[j] > poc) ++newer;
    }

    if (newer >= target) {
      kvz_image_list_rem(refs, (unsigned)i);
    }
  }

  // Snapshot the non-LTR part of the actual sliding window, nearest POC first.
  for (int delta = 1; delta <= target && f->ltr_short_count < 16; ++delta) {
    const int32_t wanted = f->poc - delta;
    int idx = -1;
    for (unsigned i = 0; i < refs->used_size; ++i) {
      if (refs->pocs[i] == wanted) { idx = (int)i; break; }
    }
    if (idx >= 0 && ltr_desc_find_poc(state, wanted) < 0) {
      f->ltr_short_poc[f->ltr_short_count++] = wanted;
    }
  }
}

void kvz_ltr_apply_ref_lists(encoder_state_t *state)
{
  kvz_ltr_t *ltr = state->encoder_control->ltr;
  encoder_state_config_frame_t *f = state->frame;

  if (!state->encoder_control->cfg.ltr_slots || !ltr || !ltr->enabled) return;

  for (int i = 0; i < f->ltr_lt_count; ++i) {
    f->ltr_lt[i].used_by_curr = 0;
  }

  if (f->ltr_use != -1) {
    const int s = f->ltr_use;
    if (s < 0 || s >= KVZ_LTR_MAX_SLOTS || !ltr->slots[s].initialized) return;

    int idx = -1;
    for (unsigned i = 0; i < f->ref->used_size; ++i) {
      if (f->ref->pocs[i] == ltr->slots[s].poc) { idx = (int)i; break; }
    }
    if (idx < 0) return;

    f->ref_LX[0][0] = (uint8_t)idx;
    f->ref_LX_size[0] = 1;
    for (int i = 1; i < 16; ++i) f->ref_LX[0][i] = 0xff;
    f->ref_LX_size[1] = 0;
    for (int i = 0; i < 16; ++i) f->ref_LX[1][i] = 0xff;

    int d = ltr_desc_find_poc(state, ltr->slots[s].poc);
    if (d >= 0) f->ltr_lt[d].used_by_curr = 1;
    return;
  }

  // use == -1: preserve the normal sliding-window L0, but cap it at
  // ref_frames despite the extra retained LTR pictures. Match HEVC default
  // list order by putting active short-term pictures before active LT ones.
  const int normal_len = f->ref_LX_size[0];
  const int window_len = MIN(normal_len, state->encoder_control->cfg.ref_frames);
  uint8_t window_idx[16];
  int window_poc[16];
  for (int i = 0; i < window_len; ++i) {
    window_idx[i] = f->ref_LX[0][i];
    window_poc[i] = f->ref->pocs[window_idx[i]];
  }

  uint8_t ordered[16];
  int n = 0;

  for (int i = 0; i < f->ltr_short_count && n < 16; ++i) {
    for (int j = 0; j < window_len; ++j) {
      if (window_poc[j] == f->ltr_short_poc[i]) {
        ordered[n++] = window_idx[j];
        break;
      }
    }
  }

  for (int j = 0; j < window_len && n < 16; ++j) {
    int d = ltr_desc_find_poc(state, window_poc[j]);
    if (d >= 0) {
      ordered[n++] = window_idx[j];
      f->ltr_lt[d].used_by_curr = 1;
    }
  }

  // Fallback should never be needed, but keeps ref indices bounded.
  if (n == 0) {
    for (int i = 0; i < window_len; ++i) ordered[n++] = window_idx[i];
  }

  for (int i = 0; i < n; ++i) f->ref_LX[0][i] = ordered[i];
  f->ref_LX_size[0] = (uint8_t)n;
  for (int i = n; i < 16; ++i) f->ref_LX[0][i] = 0xff;
}

void kvz_ltr_commit_mark(encoder_state_t *state)
{
  kvz_ltr_t *ltr = state->encoder_control->ltr;
  encoder_state_config_frame_t *f = state->frame;
  const int s = f->ltr_mark;

  if (!state->encoder_control->cfg.ltr_slots || !ltr || !ltr->enabled) return;
  if (s < 0 || s >= KVZ_LTR_MAX_SLOTS) return;

  kvz_ltr_slot_t *slot = &ltr->slots[s];

  if (slot->pic) kvz_image_free(slot->pic);
  if (slot->cu) kvz_cu_array_free(&slot->cu);
  if (slot->done_job) kvz_threadqueue_free_job(&slot->done_job);

  slot->initialized = 1;
  slot->poc = f->poc;
  slot->pic = kvz_image_copy_ref(state->tile->frame->rec);
  slot->cu = kvz_cu_array_copy_ref(state->tile->frame->cu_array);
  slot->done_job = state->tqj_bitstream_written
    ? kvz_threadqueue_copy_ref(state->tqj_bitstream_written)
    : NULL;
}

int kvz_ltr_msb_present(const encoder_state_t *state, int32_t lt_poc)
{
  (void)state;
  (void)lt_poc;
  return 1;
}

int kvz_ltr_msb_cycle_delta(const encoder_state_t *state, int32_t lt_poc)
{
  const int bits = state->encoder_control->poc_lsb_bits;
  const int32_t period = 1 << bits;
  return state->frame->poc / period - lt_poc / period;
}
