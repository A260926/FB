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

#ifndef LTC_ENCODER_CORE_H
#define LTC_ENCODER_CORE_H

#include "ltc.h"

#ifndef SAMPLE_CENTER
#define SAMPLE_CENTER 128 /* unsigned 8 bit */
#endif

/**
 * Capacity of the precomputed filter ramp.
 *
 * One LTC bit lasts sample_rate / (fps * 80) samples. The stream
 * encoder is limited to sample rates <= 192000 and frame rates >= 24,
 * so a whole segment is at most 100 samples and the rising half that is
 * precomputed is at most 51. The extra head-room keeps this safe if the
 * accepted range is ever widened.
 */
#define LTC_FILTER_RAMP_MAX 256

/**
 * Low-level BMC (biphase mark coded) timing shared by the legacy
 * LTCEncoder and the allocation-free LTCStreamEncoder.
 *
 * sample_remainder carries the fractional sample position from one
 * segment to the next so that no samples are lost or duplicated at
 * arbitrary (non integer) bit widths.
 */

/** Calculate the number of samples in the next one or two BMC segments.
 *
 * A BMC '0' bit is a single segment of one bit-period. A BMC '1' bit
 * is two segments of half a bit-period each.
 *
 * @param samples_per_clock samples per LTC bit-period
 * @param samples_per_clock_2 samples per half bit-period
 * @param sample_remainder fractional sample accumulator, updated
 * @param bit the BMC bit, 0 or 1
 * @param speed playback speed, only fabs() is used here
 * @param length receives one or two segment lengths in samples
 * @return number of segments, 1 or 2
 */
int libltc_bmc_segment_lengths(
		double samples_per_clock,
		double samples_per_clock_2,
		double *sample_remainder,
		int bit,
		double speed,
		int length[2]);

/** Bit mask selecting one bit of an LTC byte in BMC output order.
 *
 * Forward encoding emits the least significant bit first, reverse
 * encoding emits the most significant bit first.
 *
 * @param step 0..7 position within the byte
 * @param direction 1 for forward, -1 for reverse
 * @return the bit mask
 */
int libltc_bmc_bit_mask(int step, int direction);

/** Low-pass filter coefficient for a given rise-time.
 *
 * A rise-time <= 0 disables filtering (returns 0).
 */
double libltc_encoder_filter_const(double sample_rate, double rise_time);

/** Convert dBFS into the 8 bit unsigned low/high levels.
 *
 * @return 0 on success, -1 when dBFS is out of range
 */
int libltc_encoder_volume_levels(double dbfs, ltcsnd_sample_t *lo, ltcsnd_sample_t *hi);

/** One step of the encoder's one-pole low-pass filter.
 *
 * The filter state is kept in 8 bit unsigned arithmetic on purpose:
 * this reproduces the legacy encoder's rounding exactly.
 */
ltcsnd_sample_t libltc_filter_ramp_step(
		double filter_const,
		ltcsnd_sample_t value,
		ltcsnd_sample_t target);

/**
 * Resumable generator for one filtered BMC segment.
 *
 * A segment starts at SAMPLE_CENTER and approaches the target level.
 * The legacy encoder writes the result symmetrically, ie. the segment
 * is a palindrome around the middle of the segment. The generator
 * produces exactly those samples in playback order so that streaming
 * output is byte identical regardless of where a read is split.
 */
typedef struct LibltcFilterGen {
	double filter_const;
	ltcsnd_sample_t target;
	int n;   ///< segment length in samples
	int i;   ///< next sample index
	int m;   ///< number of rising half samples, (n + 1) >> 1
	int ramp_count;
	ltcsnd_sample_t value;
	ltcsnd_sample_t ramp[LTC_FILTER_RAMP_MAX];
} LibltcFilterGen;

void libltc_filter_gen_init(LibltcFilterGen *g, double filter_const, int n, ltcsnd_sample_t target);
ltcsnd_sample_t libltc_filter_gen_next(LibltcFilterGen *g);

/** Set the flag-dependent fields of a fresh LTC frame.
 *
 * Mirrors the initialization done by ltc_encoder_reinit(): binary
 * group flags, parity and drop-frame bit.
 */
void libltc_encoder_init_frame(LTCFrame *f, double fps, enum LTC_TV_STANDARD standard, int flags);

#endif
