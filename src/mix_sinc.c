/* Extended Module Player
 * Copyright (C) 1996-2026 Claudio Matsuoka and Hipolito Carraro Jr
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * Generic causal windowed-sinc interpolator (XMP_INTERP_SINC).
 *
 * Every voice owns a 64-frame ring of actually traversed source frames
 * (two rings for stereo samples), a logical source coordinate U and a
 * cutoff glide clock.  All persistent state lives in struct mixer_voice
 * (see mixer.h), no memory is allocated while mixing.
 *
 * Numerical definition (kept in sync with docs and task_support tests):
 *
 *   q = U - 32
 *   for n = floor(q) - 31 .. floor(q) + 32:
 *       d = q - n
 *       h(d) = c * sinc(c*d) * (1 + cos(pi*d/32)) / 2
 *       h(d) = 0 for |d| >= 32
 *   out = sum_n h(d) * x[n] / sum_n h(d)
 *
 *   c_target = 0.9 * min(1, 1 / |step|)
 *
 * U advances by the absolute source step once per real output frame, so
 * read chunking and tick boundaries cannot change the result.
 */

#include <math.h>

#include "common.h"
#include "mixer.h"
#include "mix_all.h"
#include "mix_sinc.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RING_MASK	(XMP_SINC_TAPS - 1)

/* Same preamp/filter constants as mix_all.c. */
#define SINC_PREAMPLIFY_BITS	15
#define SINC_FILTER_MIN		(-65536 * (1 << SINC_PREAMPLIFY_BITS))
#define SINC_FILTER_MAX		(65535 * (1 << SINC_PREAMPLIFY_BITS))
#define SINC_FILTER_CLAMP(a) \
	((a) < SINC_FILTER_MIN ? SINC_FILTER_MIN : \
	 (a) > SINC_FILTER_MAX ? SINC_FILTER_MAX : (a))

static int sinc_round(double x)
{
	return (int)(x >= 0.0 ? floor(x + 0.5) : ceil(x - 0.5));
}

/* ------------------------------------------------------------------ */
/* Pure numerical kernel                                               */
/* ------------------------------------------------------------------ */

double libxmp_sinc_sinc(double t)
{
	double pt;

	if (t == 0.0)
		return 1.0;

	pt = M_PI * t;
	return sin(pt) / pt;
}

double libxmp_sinc_weight(double d, double c)
{
	if (fabs(d) >= XMP_SINC_HALF)
		return 0.0;

	return c * libxmp_sinc_sinc(c * d) *
	       (0.5 * (1.0 + cos(M_PI * d / XMP_SINC_HALF)));
}

double libxmp_sinc_cutoff_target(double step_abs)
{
	if (!(step_abs > 0.0))
		step_abs = 1.0;
	if (step_abs < 1.0)
		step_abs = 1.0;		/* 0.9 * min(1, 1/step) */

	return XMP_SINC_CUTOFF_FACTOR / step_abs;
}

void libxmp_sinc_ramp_attack(struct sinc_voice_state *st, double step_abs)
{
	double target = libxmp_sinc_cutoff_target(step_abs);

	/* Attacks take the target cutoff directly, no initial glide. */
	st->c = st->c_from = st->c_to = target;
	st->ramp_k = XMP_SINC_RAMP_FRAMES;
	st->step_abs = step_abs;
}

double libxmp_sinc_ramp_next(struct sinc_voice_state *st, double step_abs)
{
	double target = libxmp_sinc_cutoff_target(step_abs);

	/* A new pitch asks for a new target: restart the 32 output-frame
	 * glide from the value currently in use. */
	if (target != st->c_to) {
		st->c_from = st->c;
		st->c_to = target;
		st->ramp_k = 0;
	}

	if (st->ramp_k < XMP_SINC_RAMP_FRAMES) {
		st->ramp_k++;
		st->c = st->c_from +
			(st->c_to - st->c_from) *
			((double)st->ramp_k / XMP_SINC_RAMP_FRAMES);
	} else {
		st->c = st->c_to;
	}

	st->step_abs = step_abs;
	return st->c;
}

/* Stateless FIR over an explicit logical source array. */
double libxmp_sinc_fir(const double *x, long base, long len,
		       double U, double c)
{
	double q = U - XMP_SINC_HALF;
	long fq = (long)floor(q);
	long n_lo = fq - 31;
	long n_hi = fq + XMP_SINC_HALF;
	double acc = 0.0, wsum = 0.0;
	long n;

	for (n = n_lo; n <= n_hi; n++) {
		double d = q - (double)n;
		double w = libxmp_sinc_weight(d, c);
		long idx = n - base;
		double xv = 0.0;

		if (idx >= 0 && idx < len)
			xv = x[idx];

		acc += w * xv;
		wsum += w;
	}

	if (wsum == 0.0)
		return 0.0;

	return acc / wsum;
}

/* ------------------------------------------------------------------ */
/* Voice lifetime                                                      */
/* ------------------------------------------------------------------ */

void libxmp_sinc_reposition(struct mixer_voice *vi)
{
	struct sinc_voice_state *st = &vi->sinc;

	/* Explicit sample offset / seek restore / setpatch: logical
	 * coordinate goes back to U = 0 and all past input is invalid. */
	memset(st, 0, sizeof(*st));
	st->newest = -1;
	st->ready = 0;
}

int libxmp_sinc_tail_done(const struct mixer_voice *vi)
{
	return vi->sinc.tail_done != 0;
}

int libxmp_sinc_tail_pending(const struct mixer_voice *vi)
{
	return vi->sinc.ready && vi->sinc.ended && !vi->sinc.tail_done;
}

static void sinc_lazy_init(struct mixer_voice *vi,
			   struct sinc_voice_state *st, double step_abs)
{
	double p = vi->pos;
	int phys;

	/* Keep loop/bidir hints already refreshed by mixer.c. */
	st->U = 0.0;
	st->newest = -1;
	st->ended = 0;
	st->tail_done = 0;
	st->last_valid = -1;

	if (!(p >= 0.0))
		p = 0.0;
	phys = (int)floor(p);

	st->dir = (vi->flags & VOICE_REVERSE) ? -1 : 1;

	/* voicepos() leaves reversed voices at the exclusive end alias and
	 * one-shot offsets clamped at end; normalize into the frame range. */
	if (st->dir < 0 && phys >= vi->end)
		phys = vi->end - 1;
	if (phys < 0)
		phys = 0;

	st->phys = phys;

	if (!(step_abs > 0.0))
		step_abs = 1.0;
	libxmp_sinc_ramp_attack(st, step_abs);

	st->ready = 1;
}

/* Decode one physical sample frame into signed-16-bit-scale doubles. */
static void sinc_decode(struct mixer_voice *vi, int bits, int smp_stereo,
			int phys, double *out_l, double *out_r)
{
	if (bits == 16) {
		const int16 *sptr = (const int16 *)vi->sptr;
		*out_l = sptr[phys * smp_stereo];
		if (smp_stereo == 2)
			*out_r = sptr[phys * smp_stereo + 1];
		else
			*out_r = *out_l;
	} else {
		const int8 *sptr = (const int8 *)vi->sptr;
		*out_l = (int)XMP_ASL(sptr[phys * smp_stereo], 8);
		if (smp_stereo == 2)
			*out_r = (int)XMP_ASL(sptr[phys * smp_stereo + 1], 8);
		else
			*out_r = *out_l;
	}
}

static int sinc_phys_valid(struct mixer_voice *vi, int phys)
{
	return phys >= 0 && phys < vi->end;
}

/* Move st->phys to the next one-source-frame traversal step, folding
 * short loops any number of times using the current voice endpoints.
 * Endpoint changes caused by sustain release only affect these future
 * steps; already buffered history is never recomputed. */
static void sinc_advance_phys(struct mixer_voice *vi,
			      struct sinc_voice_state *st)
{
	int S = vi->start;
	int E = vi->end;
	int np;

	if (st->dir > 0) {
		np = st->phys + 1;

		if (st->loop_active) {
			if (np >= E) {
				if (st->bidir && E - S >= 2) {
					/* Ping-pong: emit each turning
					 * endpoint only once. */
					st->dir = -1;
					np = E - 2;
				} else {
					np = S;
				}
			}
		} else if (np >= E) {
			st->ended = 1;
			return;
		}

		st->phys = np;

		/* Degenerate 1-frame loops / unusual endpoints. */
		if (st->loop_active && !sinc_phys_valid(vi, st->phys))
			st->phys = S;
	} else {
		np = st->phys - 1;

		if (st->loop_active) {
			if (np < S) {
				if (st->bidir && E - S >= 2) {
					st->dir = 1;
					np = S + 1;
				} else {
					np = E - 1;
				}
			}
		} else if (np < S || np >= E) {
			st->ended = 1;
			return;
		}

		st->phys = np;

		if (st->loop_active && !sinc_phys_valid(vi, st->phys))
			st->phys = (E > S) ? E - 1 : S;
	}
}

/* Make every logical frame through `target_n` available in the rings. */
static void sinc_ensure_through(struct mixer_voice *vi,
				struct sinc_voice_state *st,
				int bits, int smp_stereo,
				long long target_n)
{
	while (st->newest < target_n) {
		long long n = st->newest + 1;
		double xl = 0.0, xr = 0.0;
		int valid = 0;

		if (!st->ended) {
			if (st->newest >= 0)
				sinc_advance_phys(vi, st);

			if (!st->ended && sinc_phys_valid(vi, st->phys)) {
				sinc_decode(vi, bits, smp_stereo,
					    st->phys, &xl, &xr);
				valid = 1;
			} else {
				st->ended = 1;
			}
		}

		if (st->ended && st->last_valid < 0)
			st->last_valid = valid ? n : st->newest;

		if (valid) {
			st->ring_l[(int)(n & RING_MASK)] = xl;
			st->ring_r[(int)(n & RING_MASK)] = xr;
		} else {
			st->ring_l[(int)(n & RING_MASK)] = 0.0;
			st->ring_r[(int)(n & RING_MASK)] = 0.0;
		}

		st->newest = n;
	}
}

static void sinc_get_xy(const struct sinc_voice_state *st,
			long long n, double *xl, double *xr)
{
	*xl = *xr = 0.0;

	if (n < 0)
		return;
	if (n > st->newest)
		return;
	if (st->ended && n > st->last_valid)
		return;

	*xl = st->ring_l[(int)(n & RING_MASK)];
	*xr = st->ring_r[(int)(n & RING_MASK)];
}

/* Render one output frame and advance U / cutoff clock. */
static void sinc_frame(struct mixer_voice *vi,
		       struct sinc_voice_state *st,
		       int bits, int smp_stereo,
		       double step_abs, double *out_l, double *out_r)
{
	double c = libxmp_sinc_ramp_next(st, step_abs);
	double q = st->U - XMP_SINC_HALF;
	long long fq = (long long)floor(q);
	long long n_lo = fq - 31;
	long long n_hi = fq + XMP_SINC_HALF;
	double acc_l = 0.0, acc_r = 0.0, wsum = 0.0;
	long long n;

	sinc_ensure_through(vi, st, bits, smp_stereo, n_hi);

	for (n = n_lo; n <= n_hi; n++) {
		double d = q - (double)n;
		double w = libxmp_sinc_weight(d, c);
		double xl, xr;

		sinc_get_xy(st, n, &xl, &xr);
		acc_l += w * xl;
		acc_r += w * xr;
		wsum += w;
	}

	if (wsum != 0.0) {
		*out_l = acc_l / wsum;
		*out_r = acc_r / wsum;
	} else {
		*out_l = *out_r = 0.0;
	}

	if (st->ended && (st->last_valid < 0 || n_lo > st->last_valid))
		st->tail_done = 1;

	st->U += step_abs;
}

void libxmp_sinc_advance_silent(struct mixer_voice *vi, int count, int step)
{
	struct sinc_voice_state *st = &vi->sinc;
	double step_abs = fabs((double)step) / (double)(1 << SMIX_SHIFT);
	int bits = (vi->fidx & FLAG_16_BITS) ? 16 : 8;
	int smp_stereo = (vi->fidx & FLAG_STEREO) ? 2 : 1;
	int i;

	if (!st->ready)
		sinc_lazy_init(vi, st, step_abs);

	for (i = 0; i < count; i++) {
		double dummy_l, dummy_r;
		sinc_frame(vi, st, bits, smp_stereo,
			   step_abs, &dummy_l, &dummy_r);
	}
}

/* ------------------------------------------------------------------ */
/* Mixer functions                                                     */
/* ------------------------------------------------------------------ */

static void sinc_render(struct mixer_voice * LIBXMP_RESTRICT vi,
			int32 * LIBXMP_RESTRICT buffer, int count,
			int vl, int vr, int step, int ramp,
			int delta_l, int delta_r,
			int out_stereo, int smp_stereo,
			int bits, int use_filter)
{
	struct sinc_voice_state *st = &vi->sinc;
	double step_abs = fabs((double)step) / (double)(1 << SMIX_SHIFT);
	int old_vl = vi->old_vl;
	int old_vr = vi->old_vr;
	int ac = count - ramp;
	int i;

#ifndef LIBXMP_CORE_DISABLE_IT
	int fl1 = vi->filter.l1, fl2 = vi->filter.l2;
	int fr1 = vi->filter.r1, fr2 = vi->filter.r2;
	int64 a0 = vi->filter.a0, b0 = vi->filter.b0, b1 = vi->filter.b1;
#endif

	if (ac < 0)
		ac = 0;
	if (ac > count)
		ac = count;

	if (!st->ready)
		sinc_lazy_init(vi, st, step_abs);

	for (i = 0; i < count; i++) {
		double yl, yr;
		int smpl, smpr, mono;
		int ml, mr;

		sinc_frame(vi, st, bits, smp_stereo, step_abs, &yl, &yr);
		smpl = sinc_round(yl);
		smpr = sinc_round(yr);

#ifndef LIBXMP_CORE_DISABLE_IT
		if (use_filter) {
			int64 s64;
			int s;

			s64 = (a0 * XMP_ASL(smpl, SINC_PREAMPLIFY_BITS) +
			       b0 * fl1 + b1 * fl2) >> FILTER_SHIFT;
			s = SINC_FILTER_CLAMP(s64);
			fl2 = fl1; fl1 = s;
			smpl = s >> SINC_PREAMPLIFY_BITS;

			s64 = (a0 * XMP_ASL(smpr, SINC_PREAMPLIFY_BITS) +
			       b0 * fr1 + b1 * fr2) >> FILTER_SHIFT;
			s = SINC_FILTER_CLAMP(s64);
			fr2 = fr1; fr1 = s;
			smpr = s >> SINC_PREAMPLIFY_BITS;
		}
#endif

		if (i < ac) {
			ml = old_vl >> 8;
			mr = old_vr >> 8;
			old_vl += delta_l;
			old_vr += delta_r;
		} else {
			ml = vl;
			mr = vr;
		}

		if (out_stereo) {
			*buffer++ += smpl * ml;
			*buffer++ += smpr * mr;
		} else if (smp_stereo == 2) {
			mono = (smpl + smpr) >> 1;
			*buffer++ += mono * ml;
		} else {
			*buffer++ += smpl * ml;
		}
	}

#ifndef LIBXMP_CORE_DISABLE_IT
	if (use_filter) {
		vi->filter.l1 = fl1;
		vi->filter.l2 = fl2;
		if (smp_stereo == 2) {
			vi->filter.r1 = fr1;
			vi->filter.r2 = fr2;
		} else {
			vi->filter.r1 = fl1;
			vi->filter.r2 = fl2;
		}
	}
#endif
}

#define SINC_FN(f, out_stereo, smp_stereo, bits, filt) \
MIXER(f) \
{ \
	sinc_render(vi, buffer, count, vl, vr, step, ramp, \
		    delta_l, delta_r, out_stereo, smp_stereo, bits, filt); \
}

SINC_FN(monoout_mono_8bit_sinc,    0, 1, 8, 0)
SINC_FN(monoout_mono_16bit_sinc,   0, 1, 16, 0)
SINC_FN(monoout_stereo_8bit_sinc,  0, 2, 8, 0)
SINC_FN(monoout_stereo_16bit_sinc,  0, 2, 16, 0)
SINC_FN(stereoout_mono_8bit_sinc,  1, 1, 8, 0)
SINC_FN(stereoout_mono_16bit_sinc, 1, 1, 16, 0)
SINC_FN(stereoout_stereo_8bit_sinc,  1, 2, 8, 0)
SINC_FN(stereoout_stereo_16bit_sinc, 1, 2, 16, 0)

#ifndef LIBXMP_CORE_DISABLE_IT
SINC_FN(monoout_mono_8bit_sinc_filter,    0, 1, 8, 1)
SINC_FN(monoout_mono_16bit_sinc_filter,   0, 1, 16, 1)
SINC_FN(monoout_stereo_8bit_sinc_filter,  0, 2, 8, 1)
SINC_FN(monoout_stereo_16bit_sinc_filter, 0, 2, 16, 1)
SINC_FN(stereoout_mono_8bit_sinc_filter,  1, 1, 8, 1)
SINC_FN(stereoout_mono_16bit_sinc_filter, 1, 1, 16, 1)
SINC_FN(stereoout_stereo_8bit_sinc_filter,  1, 2, 8, 1)
SINC_FN(stereoout_stereo_16bit_sinc_filter, 1, 2, 16, 1)
#endif

const MIXER_FP libxmp_sinc_mixers[] = {
	LIST_MIX_FUNCTIONS(sinc),
#ifndef LIBXMP_CORE_DISABLE_IT
	LIST_MIX_FUNCTIONS(sinc_filter)
#endif
};
