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

#include "ltc.h"
#ifndef SAMPLE_CENTER // also defined in decoder.h
#define SAMPLE_CENTER 128 // unsigned 8 bit.
#endif

struct LTCEncoder {
	double fps;
	double sample_rate;
	double filter_const;
	int flags;
	enum LTC_TV_STANDARD standard;
	ltcsnd_sample_t enc_lo, enc_hi;

	size_t offset;
	size_t bufsize;
	ltcsnd_sample_t *buf;

	char state;

	double samples_per_clock;
	double samples_per_clock_2;
	double sample_remainder;

	LTCFrame f;
};

int encode_byte(LTCEncoder *e, int byte, double speed);
int encode_transition(LTCEncoder *e);

/*
 * Shared low-level LTC encoding primitives.
 *
 * Both LTCEncoder (the legacy buffer based encoder) and LTCStreamEncoder
 * (the allocation free, pull based encoder) drive bit-cell generation
 * through these helpers.  Keeping the BMC timing arithmetic and the
 * low-pass filter waveform in one place guarantees that both entry points
 * emit bit-identical PCM, including the fractional sample remainder and
 * the mirrored filter ramp.
 */

/** BMC timing and output phase, carried across bytes and frames. */
typedef struct LTCBmcState {
	double samples_per_clock;   ///< audio samples per full bit cell
	double samples_per_clock_2; ///< audio samples per half bit cell
	double sample_remainder;    ///< fractional sample carry
	int state;                  ///< current output phase (0/1)
} LTCBmcState;

/** Bit iterator for one byte of a frame, LSB or MSB first. */
typedef struct LTCBmcByte {
	unsigned char byte; ///< byte being encoded
	unsigned char mask; ///< mask of the next bit (0 when exhausted)
	int reverse;        ///< non-zero: MSB first (reverse playback)
	int is_one;         ///< current bit value
	int halves_left;    ///< remaining half-period cells for current bit
} LTCBmcByte;

/**
 * Prepare to encode \a byte in the given direction.
 * The byte is consumed by repeated calls to \ref ltc_bmc_byte_next.
 */
void ltc_bmc_byte_begin(LTCBmcByte *b, unsigned char byte, int reverse);

/**
 * Produce the next contiguous bit-cell segment of the current byte.
 *
 * @param b byte iterator
 * @param s BMC timing state (updated in place)
 * @param n set to the number of audio samples in this segment
 * @param level set to the output phase (0/1) for this segment
 * @return 1 if a segment was produced, 0 when the byte is exhausted
 */
int ltc_bmc_byte_next(LTCBmcByte *b, LTCBmcState *s, int *n, int *level);

/**
 * Render \a n samples of filtered (or square) LTC output.
 *
 * This is the single implementation of the legacy addvalues() waveform:
 * when \a filter_const is positive the ramp is computed from the sample
 * center for the first half of the segment and mirrored for the second
 * half.  Otherwise the segment is a flat square pulse.
 *
 * @param dst destination buffer (at least \a n samples)
 * @param n number of samples
 * @param filter_const low-pass coefficient, 0 for a square wave
 * @param tgtval target output level
 */
void ltc_filter_segment(ltcsnd_sample_t *dst, int n, double filter_const, ltcsnd_sample_t tgtval);

/** Set the eight user nibbles of a frame from a 32 bit word, LSB first. */
void ltc_frame_set_user_bits(LTCFrame *f, unsigned long data);

/*
 * LTCStreamEncoder internals.
 *
 * The stream encoder never allocates during read.  It keeps the current
 * ten-byte frame label, at most one bit-cell segment worth of PCM, and a
 * fixed size decoder state.  A segment is never longer than one full bit
 * cell; with the supported 32000..192000 Hz / 24..30 fps range this is at
 * most 100 samples, hence the fixed bound below.
 */
#define LTC_STREAM_SEG_MAX 128

struct LTCStreamEncoder {
	double fps;
	double sample_rate;
	double filter_const;
	int flags;
	enum LTC_TV_STANDARD standard;
	ltcsnd_sample_t enc_lo, enc_hi;
	int direction; ///< +1 forwards, -1 backwards (fixed for lifetime)
	int fps_int;   ///< integer frame rate for increment/decrement

	LTCFrame f;              ///< label of the frame being emitted / next up
	unsigned char label[10]; ///< immutable snapshot of f while emitting
	int label_valid;         ///< label[] has been filled for this frame
	int frame_started;       ///< at least one sample of this label was output

	int byte_step;      ///< 0..10; which byte of the label is next
	int byte_active;    ///< a byte iterator is in progress
	LTCBmcByte bmc_byte;
	LTCBmcState bmc;

	int seg_pos;        ///< next sample index inside segbuf
	int seg_len;        ///< number of samples in segbuf
	int seg_ends_frame; ///< the pending segment is the frame's last
	ltcsnd_sample_t segbuf[LTC_STREAM_SEG_MAX];

	int pending_user_dirty;  ///< set_user_bits() arrived mid-frame
	unsigned long pending_user;
};
