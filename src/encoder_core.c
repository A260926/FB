/*
   libltc - en+decode linear timecode

   Copyright (C) 2006-2022 Robin Gareus <robin@gareus.org>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as
   published by the Free Software Foundation, either version 3 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library.
   If not, see <http://www.gnu.org/licenses/>.
*/

#include <math.h>

#include "encoder_core.h"

int libltc_bmc_segment_lengths(
		double samples_per_clock,
		double samples_per_clock_2,
		double *sample_remainder,
		int bit,
		double speed,
		int length[2])
{
	const double spc = samples_per_clock * fabs(speed);
	const double sph = samples_per_clock_2 * fabs(speed);
	int n;

	if (!bit) {
		n = (int)(spc + *sample_remainder);
		*sample_remainder = spc + *sample_remainder - n;
		length[0] = n;
		return 1;
	}

	n = (int)(sph + *sample_remainder);
	*sample_remainder = sph + *sample_remainder - n;
	length[0] = n;

	n = (int)(sph + *sample_remainder);
	*sample_remainder = sph + *sample_remainder - n;
	length[1] = n;
	return 2;
}

int libltc_bmc_bit_mask(int step, int direction) {
	if (direction >= 0) {
		return 1 << step;
	}
	return 128 >> step;
}

double libltc_encoder_filter_const(double sample_rate, double rise_time) {
	/* low-pass-filter
	 * LTC signal should have a rise time of 40 us +/- 10 us.
	 *
	 * rise-time means from <10% to >90% of the signal.
	 * in each segment we start at 50% (SAMPLE_CENTER), so
	 * here we need half-of it.
	 */
	if (rise_time <= 0)
		return 0;
	if (sample_rate <= 0)
		return 0;
	return 1.0 - exp( -1.0 / (sample_rate * rise_time / 2000000.0 / exp(1.0)) );
}

int libltc_encoder_volume_levels(double dbfs, ltcsnd_sample_t *lo, ltcsnd_sample_t *hi) {
	if (dbfs > 0)
		return -1;
	double pp = rint(127.0 * pow(10, dbfs / 20.0));
	if (pp < 1 || pp > 127)
		return -1;
	ltcsnd_sample_t diff = ((ltcsnd_sample_t) pp) & 0x7f;
	*lo = SAMPLE_CENTER - diff;
	*hi = SAMPLE_CENTER + diff;
	return 0;
}

ltcsnd_sample_t libltc_filter_ramp_step(
		double filter_const,
		ltcsnd_sample_t value,
		ltcsnd_sample_t target)
{
	if (filter_const <= 0) {
		return target;
	}
	return value + filter_const * (target - (double)value);
}

void libltc_filter_gen_init(LibltcFilterGen *g, double filter_const, int n, ltcsnd_sample_t target) {
	int i;
	g->filter_const = filter_const;
	g->target = target;
	g->n = n;
	g->i = 0;
	g->m = (n + 1) >> 1;
	g->ramp_count = 0;
	g->value = SAMPLE_CENTER;

	if (filter_const <= 0) {
		/* perfect square wave, no state to precompute */
		return;
	}

	/* Precompute the rising half of the segment once. The legacy
	 * encoder evaluates the same recurrence for the first (n+1)/2
	 * samples and then mirrors the result, so this is bit-identical
	 * while only a small fixed buffer is needed. */
	for (i = 0; i < g->m && i < LTC_FILTER_RAMP_MAX; i++) {
		g->value = libltc_filter_ramp_step(filter_const, g->value, target);
		g->ramp[i] = g->value;
	}
	g->ramp_count = i;
}

ltcsnd_sample_t libltc_filter_gen_next(LibltcFilterGen *g) {
	const int i = g->i++;

	if (g->filter_const <= 0) {
		return g->target;
	}
	if (g->ramp_count <= 0) {
		return SAMPLE_CENTER;
	}

	/* The segment is a palindrome around its middle. For the first
	 * half the precomputed recurrence result is used, the second
	 * half replays it in reverse. */
	{
		const int j = (i < g->m) ? i : (g->n - i - 1);
		if (j >= 0 && j < g->ramp_count) {
			return g->ramp[j];
		}
		return g->ramp[g->ramp_count - 1];
	}
}

void libltc_encoder_init_frame(LTCFrame *f, double fps, enum LTC_TV_STANDARD standard, int flags) {
	if (flags & LTC_BGF_DONT_TOUCH) {
		f->col_frame = 0;
		if (flags & LTC_TC_CLOCK) {
			f->binary_group_flag_bit1 = 1;
		} else {
			f->binary_group_flag_bit1 = 0;
		}
		switch (standard) {
			case LTC_TV_625_50: /* 25 fps mode */
				f->biphase_mark_phase_correction = 0; /* BGF0 */
				f->binary_group_flag_bit0 = (flags & LTC_USE_DATE) ? 1 : 0; /* BGF2 */
				break;
			default:
				f->binary_group_flag_bit0 = 0;
				f->binary_group_flag_bit2 = (flags & LTC_USE_DATE) ? 1 : 0;
				break;
		}
	}
	if ((flags & LTC_NO_PARITY) == 0) {
		ltc_frame_set_parity(f, standard);
	}

	if ((int)rint(fps * 100.0) == 2997) {
		f->dfbit = 1;
	} else {
		f->dfbit = 0;
	}
}
