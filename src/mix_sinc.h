#ifndef LIBXMP_MIX_SINC_H
#define LIBXMP_MIX_SINC_H

/*
 * Windowed-sinc interpolation ("XMP_INTERP_SINC", value 3).
 *
 * This header is part of the production mixer and is also force-included
 * by the task regression binary.  Everything here is internal libxmp API.
 */

#include "common.h"
#include "mix_all.h"

LIBXMP_BEGIN_DECLS

/* The FIR spans 64 integer source frames and is centered 32 source
 * frames behind the logical source coordinate. */
#define XMP_SINC_TAPS		64
#define XMP_SINC_HALF		32

/* Cutoff change glide length, counted in real output frames. */
#define XMP_SINC_RAMP_FRAMES	32

/* Cutoff safety factor. */
#define XMP_SINC_CUTOFF_FACTOR	0.9

/* Per-voice persistent state.  One struct is embedded in every
 * struct mixer_voice, so the workspace grows only with the voice count
 * and the fixed FIR window and no allocation happens while mixing.
 *
 * U and the cutoff ramp are shared by the left/right sample channels of
 * one voice; the two history rings are independent.  NNA voices own
 * their copy because each mixer_voice owns this struct by value. */
struct sinc_voice_state {
	unsigned int ready;		/* lazy initialization marker */

	/* Logical source coordinate of the next output frame.  Starts at 0
	 * on attack / explicit offset and only moves forward, by the
	 * absolute source step. */
	double U;
	double step_abs;		/* step magnitude of the last call */

	/* Cutoff glide clock, advanced once per rendered output frame. */
	double c;			/* cutoff for the next frame */
	double c_from;
	double c_to;
	int ramp_k;			/* 0/32 == steady, 1..31 gliding */

	/* History of actually traversed logical source frames.  Entries
	 * are normalized signed 16-bit scale values and indexed by
	 * logical index modulo XMP_SINC_TAPS. */
	double ring_l[XMP_SINC_TAPS];
	double ring_r[XMP_SINC_TAPS];
	long long newest;		/* newest produced logical n, -1 */

	/* One-source-frame physical walker. */
	int phys;			/* physical frame matching `newest` */
	int dir;			/* +1 forward, -1 reverse */

	/* Natural (non-cut) end and FIR tail bookkeeping. */
	int ended;			/* walker reached the sample end */
	long long last_valid;		/* last logical n with real sound */
	int tail_done;			/* window no longer covers samples */

	/* Scheduler hints, refreshed by mixer.c before each call. */
	int loop_active;
	int bidir;
	int bidir_adjust;
};

/* Cutoff glide helpers (also exercised directly by the kernel tests). */
double libxmp_sinc_cutoff_target(double step_abs);
void libxmp_sinc_ramp_attack(struct sinc_voice_state *st, double step_abs);
double libxmp_sinc_ramp_next(struct sinc_voice_state *st, double step_abs);

/* Pure numerical definition. */
double libxmp_sinc_sinc(double t);
double libxmp_sinc_weight(double d, double c);

/* Stateless 64-tap FIR over a logical source sequence held as a plain
 * array.  Logical x[n] equals x[n - base] for base <= n < base + len and
 * 0 otherwise.  This is the same coefficient/FIR implementation the
 * production mixer uses, exposed for the kernel regression. */
double libxmp_sinc_fir(const double *x, long base, long len,
		       double U, double c);

/* Reset hook for explicit voice repositioning (offset / seek restore).
 * Called by libxmp_mixer_voicepos(); the new origin is read from vi->pos. */
struct mixer_voice;
void libxmp_sinc_reposition(struct mixer_voice *vi);

/* Advance U/history for `count` frames without writing audio.  Used by
 * the scheduler when a voice volume is zero and its mixer is skipped. */
void libxmp_sinc_advance_silent(struct mixer_voice *vi, int count, int step);

/* Non-zero once the natural-end FIR tail has fully decayed. */
int libxmp_sinc_tail_done(const struct mixer_voice *vi);
int libxmp_sinc_tail_pending(const struct mixer_voice *vi);

/* Mixer table: 8 sample/output combinations, then the 8 IT-filter
 * variants, matching the index map documented in mix_all.h. */
extern const MIXER_FP libxmp_sinc_mixers[];

LIBXMP_END_DECLS

#endif /* LIBXMP_MIX_SINC_H */
