/*
** cm_limiter.h -- internal stereo lookahead limiter module for cmixer.
**
** This header is self-contained: it may be included on its own from any
** translation unit. The implementation lives in src/cm_limiter.c; the
** public entry points are declared in src/cmixer.h and are driven by
** src/cmixer.c, which owns the source list and the master gain.
**
** The module keeps, per attached source, a small ring of that source's
** not-yet-emitted stereo contributions (pre master-gain, Q12 domain), plus
** one shared ring of the summed pre-gain mix and the master gain that was
** current when each frame entered the delay line. Output frames are
** emitted `lookahead` frames late, scaled by a gain computed from a
** sliding peak window (monotonic deque, amortized O(1) per frame).
*/

#ifndef CM_LIMITER_H
#define CM_LIMITER_H

#include "cmixer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque per-source delay-line handle. The mixer stores one pointer per
** cm_Source; the module owns the storage behind it and clears the pointer
** whenever the slot is released. */
typedef struct cm_LimSlot cm_LimSlot;

/* Module state queries. */
int    cm_lim_enabled(void);
int    cm_lim_latency(void);    /* lookahead in output frames, 0 when off */
double cm_lim_last_gain(void);  /* gain used by the last emitted frame */

/* Two-phase configuration. cm_lim_prepare() allocates the limiter ring
** storage for a candidate configuration without touching the live one.
** cm_lim_slot_create() -- while a configuration is being prepared --
** stages a fresh delay line for one source (a replacement if the source
** already has one); cm_lim_commit() atomically installs everything that
** was staged and resets the runtime state, cm_lim_rollback() discards it.
** Outside of a prepare/commit cycle, cm_lim_slot_create() attaches a slot
** to a source at once (used when a source is created while enabled). */
int        cm_lim_prepare(int lookahead_frames, int ceiling, int release_frames);
cm_LimSlot* cm_lim_slot_create(cm_LimSlot **owner);
void       cm_lim_commit(void);
void       cm_lim_rollback(void);

/* Runtime controls; validity and enabled-state are checked by the caller. */
void cm_lim_set_parameters(int ceiling, int release_frames);
void cm_lim_reset(void);
void cm_lim_disable(void);

/* Per-source slots. */
void cm_lim_slot_destroy(cm_LimSlot *slot);
/* Withdraws the source's not-yet-emitted contributions from the shared
** sum and rebuilds the affected peak windows; already emitted gain is
** not rolled back. */
void cm_lim_slot_retract(cm_LimSlot *slot);
/* Records `count` stereo frames (interleaved, pre master-gain) of this
** source's contribution, starting at the module's current input frame. */
void cm_lim_slot_write(cm_LimSlot *slot, const cm_Int32 *frames, int count);

/* Render: consumes `frames` stereo pre-gain sums (interleaved), applies
** `mgain` (Q12) and writes `frames` limited stereo frames to dst. */
void cm_lim_process(const cm_Int32 *sum, cm_Int32 mgain, int frames, cm_Int16 *dst);

#ifdef __cplusplus
}
#endif

#endif
