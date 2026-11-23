/*
** Internal lookahead limiter module for cmixer.
**
** This header is self-contained: it may be included on its own. The public
** limiter entry points are declared in cmixer.h; the mixer core (cmixer.c)
** drives this stateful module. All functions must be called from the control
** thread between cm_process() calls, except cm_lim_source_put(),
** cm_lim_source_put_silence() and cm_lim_push() which form the render path
** (they never allocate).
**
** Units: frames are stereo output frames; `ceiling` is a peak amplitude in
** 16-bit sample units; gains on the wire are Q12 fixed point.
*/

#ifndef CM_LIMITER_H
#define CM_LIMITER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque per-source limiter context: one ring of queued (not yet delivered)
** stereo contributions, used to revoke a source's pending audio. */
typedef struct cm_LimSource cm_LimSource;

/* Opaque prepared configuration (two-phase commit). */
typedef struct cm_LimPlan cm_LimPlan;

/* Validates arguments and allocates the shared ring storage. Returns NULL on
** failure and stores a static message in `err` when `err` is non-NULL. */
cm_LimPlan  *cm_lim_plan_new(int lookahead_frames, int ceiling, int release_frames, const char **err);
/* Frees a prepared-but-uncommitted plan. The running state is untouched. */
void         cm_lim_plan_abort(cm_LimPlan *plan);
/* Atomically replaces the current configuration: queued PCM and the envelope
** are dropped, the frame clock restarts at zero. Frees `plan` itself. */
void         cm_lim_plan_commit(cm_LimPlan *plan);

/* Per-source contexts, sized for `lookahead_frames`. */
cm_LimSource *cm_lim_source_new(int lookahead_frames);
void          cm_lim_source_free(cm_LimSource *src);
/* Zeroes the queued contributions of the source (used by reset). */
void          cm_lim_source_clear(cm_LimSource *src);

/* Module state. */
int    cm_lim_enabled(void);
int    cm_lim_lookahead(void);   /* 0 when disabled */
double cm_lim_last_gain(void);   /* envelope of the last emitted frame, 1 initially */
void   cm_lim_reset(void);       /* clears history, keeps the configuration */
void   cm_lim_disable(void);     /* releases all shared storage */
int    cm_lim_set_parameters(int ceiling, int release_frames, const char **err);

/* Render path: stores the source's contribution for the next `nframes`
** absolute frames (overwrites, does not accumulate). */
void   cm_lim_source_put(cm_LimSource *src, const int *stereo_frames, int nframes);
void   cm_lim_source_put_silence(cm_LimSource *src, int nframes);
/* Removes the source's queued contributions from the pending sums and
** rebuilds the peak window from what remains. Delivered frames keep their
** gain; the envelope recursion continues from its current value. */
void   cm_lim_revoke(cm_LimSource *src);
/* Pushes `nframes` of pre-master-gain sums and emits `nframes` of limited
** output. The first lookahead frames after a (re)configuration are silence. */
void   cm_lim_push(const int *stereo_sum, int nframes, int master_gain_q12, short *dst);

#ifdef __cplusplus
}
#endif

#endif
