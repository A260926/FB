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
#include <stdlib.h>

#include "encoder_core.h"

#define LTC_STREAM_MIN_SAMPLE_RATE 32000.0
#define LTC_STREAM_MAX_SAMPLE_RATE 192000.0

/* every flag currently defined in enum LTC_BG_FLAGS */
#define LTC_STREAM_KNOWN_FLAGS \
	(LTC_USE_DATE | LTC_TC_CLOCK | LTC_BGF_DONT_TOUCH | LTC_NO_PARITY)

struct LTCStreamEncoder {
	double fps;
	double sample_rate;
	double filter_const;
	int flags;
	enum LTC_TV_STANDARD standard;
	int direction; /* +1 forward, -1 reverse, fixed for the lifetime */

	ltcsnd_sample_t enc_lo, enc_hi;

	double samples_per_clock;
	double samples_per_clock_2;
	double sample_remainder;

	char state;

	/** timecode of the frame that owns the next sample to be emitted */
	LTCFrame f;
	/** user-bits queued for the next frame while one is in progress */
	unsigned long pending_user_bits;
	int pending_user_bits_valid;
	/** whether at least one sample of the current frame was emitted */
	int frame_samples_emitted;

	/* position inside the frame: 0..79 */
	int bit;
	int seg_index;
	int seg_count;
	int seg_length[2];
	int seg_left;
	LibltcFilterGen gen;
};

static int fps_is_supported(double fps) {
	if (!(fps > 0.0) || !isfinite(fps)) {
		return 0;
	}
	if (fabs(fps - 24.0) < 1e-6) return 1;
	if (fabs(fps - 25.0) < 1e-6) return 1;
	if (fabs(fps - 30.0) < 1e-6) return 1;
	/* 30000/1001 and its common decimal alias 29.97 */
	if ((int)rint(fps * 100.0) == 2997) return 1;
	return 0;
}

static void stream_set_user_bits(LTCFrame *f, unsigned long data, enum LTC_TV_STANDARD standard, int flags) {
	f->user1 = data & 0xF;
	data >>= 4;
	f->user2 = data & 0xF;
	data >>= 4;
	f->user3 = data & 0xF;
	data >>= 4;
	f->user4 = data & 0xF;
	data >>= 4;
	f->user5 = data & 0xF;
	data >>= 4;
	f->user6 = data & 0xF;
	data >>= 4;
	f->user7 = data & 0xF;
	data >>= 4;
	f->user8 = data & 0xF;

	if ((flags & LTC_NO_PARITY) == 0) {
		ltc_frame_set_parity(f, standard);
	}
}

static int stream_frame_is_fresh(const LTCStreamEncoder *e) {
	return e->frame_samples_emitted == 0;
}

/** Move to the frame that owns the samples following the current one. */
static void stream_advance_frame(LTCStreamEncoder *e) {
	if (e->direction < 0) {
		ltc_frame_decrement(&e->f, (int)rint(e->fps), e->standard, e->flags);
	} else {
		ltc_frame_increment(&e->f, (int)rint(e->fps), e->standard, e->flags);
	}

	if (e->pending_user_bits_valid) {
		/* A request that arrived while the previous frame was already
		 * being emitted belongs to this new frame. It is applied
		 * before its first sample is generated. */
		stream_set_user_bits(&e->f, e->pending_user_bits, e->standard, e->flags);
		e->pending_user_bits_valid = 0;
	}

	e->frame_samples_emitted = 0;
	e->bit = 0;
	e->seg_index = 0;
	e->seg_count = 0;
	e->seg_left = 0;
}

/** Start the next BMC bit of the current frame. */
static void stream_start_bit(LTCStreamEncoder *e) {
	const unsigned char *bytes = (const unsigned char *)&e->f;
	const int byte_index = (e->direction < 0) ? (9 - e->bit / 8) : (e->bit / 8);
	const int step = e->bit % 8;
	const int bit = (bytes[byte_index] & libltc_bmc_bit_mask(step, e->direction)) ? 1 : 0;

	e->seg_count = libltc_bmc_segment_lengths(
			e->samples_per_clock, e->samples_per_clock_2,
			&e->sample_remainder, bit, (double)e->direction, e->seg_length);
	e->seg_index = 0;
	e->seg_left = e->seg_length[0];
	e->state = !e->state;
	libltc_filter_gen_init(&e->gen, e->filter_const, e->seg_left,
			e->state ? e->enc_hi : e->enc_lo);
}

/** Start the next segment (the second half of a BMC '1' bit). */
static void stream_next_segment(LTCStreamEncoder *e) {
	e->seg_index++;
	if (e->seg_index < e->seg_count) {
		e->seg_left = e->seg_length[e->seg_index];
		e->state = !e->state;
		libltc_filter_gen_init(&e->gen, e->filter_const, e->seg_left,
				e->state ? e->enc_hi : e->enc_lo);
	}
}

LTCStreamEncoder* ltc_stream_encoder_create(
		double sample_rate, double fps, enum LTC_TV_STANDARD standard,
		int flags, const SMPTETimecode *start, int direction,
		double volume_dbfs, double rise_time)
{
	LTCStreamEncoder *e;

	if (!(sample_rate >= LTC_STREAM_MIN_SAMPLE_RATE &&
	      sample_rate <= LTC_STREAM_MAX_SAMPLE_RATE)) {
		return NULL;
	}
	if (!fps_is_supported(fps)) {
		return NULL;
	}
	if ((int)standard < (int)LTC_TV_525_60 || (int)standard > (int)LTC_TV_FILM_24) {
		return NULL;
	}
	if (flags & ~LTC_STREAM_KNOWN_FLAGS) {
		return NULL;
	}
	if (direction != 1 && direction != -1) {
		return NULL;
	}
	if (!(rise_time >= 0.0) || !isfinite(rise_time) || !isfinite(volume_dbfs)) {
		return NULL;
	}
	if (!start) {
		return NULL;
	}

	e = (LTCStreamEncoder *) calloc(1, sizeof(LTCStreamEncoder));
	if (!e) {
		return NULL;
	}

	if (libltc_encoder_volume_levels(volume_dbfs, &e->enc_lo, &e->enc_hi)) {
		free(e);
		return NULL;
	}

	e->sample_rate = sample_rate;
	e->fps = fps;
	e->standard = standard;
	e->flags = flags;
	e->direction = direction;
	e->filter_const = libltc_encoder_filter_const(sample_rate, rise_time);

	e->samples_per_clock = sample_rate / (fps * 80.0);
	e->samples_per_clock_2 = e->samples_per_clock / 2.0;
	e->sample_remainder = 0.5;
	e->state = 0;

	ltc_frame_reset(&e->f);
	libltc_encoder_init_frame(&e->f, fps, standard, flags);
	{
		/* ltc_time_to_frame only reads the timecode */
		SMPTETimecode t = *start;
		ltc_time_to_frame(&e->f, &t, standard, flags);
	}

	e->bit = 0;
	e->frame_samples_emitted = 0;
	return e;
}

void ltc_stream_encoder_free(LTCStreamEncoder *e) {
	free(e);
}

int ltc_stream_encoder_read(LTCStreamEncoder *e, ltcsnd_sample_t *output, size_t count) {
	size_t i;

	if (!e) {
		return -1;
	}
	if (!output && count != 0) {
		return -1;
	}
	if (count == 0) {
		return 0;
	}

	for (i = 0; i < count; i++) {
		/* Make sure a segment with at least one sample is active. */
		while (e->seg_left <= 0) {
			if (e->seg_count > 0 && e->seg_index + 1 < e->seg_count) {
				/* second half of a BMC '1' bit */
				stream_next_segment(e);
			} else {
				if (e->seg_count > 0) {
					e->bit++;
					if (e->bit >= LTC_FRAME_BIT_COUNT) {
						/* the previous frame is complete, the next bit
						 * belongs to the following frame */
						stream_advance_frame(e);
					}
				}
				stream_start_bit(e);
			}
		}

		output[i] = libltc_filter_gen_next(&e->gen);
		e->frame_samples_emitted = 1;
		e->seg_left--;

		/* Once the last sample of the last bit has been emitted the
		 * next sample belongs to the following frame. Advance right
		 * away so that ltc_stream_encoder_get_timecode() already
		 * reports the new frame. */
		if (e->seg_left == 0
				&& e->seg_index + 1 >= e->seg_count
				&& e->bit + 1 >= LTC_FRAME_BIT_COUNT) {
			stream_advance_frame(e);
		}
	}

	return 0;
}

int ltc_stream_encoder_set_user_bits(LTCStreamEncoder *e, unsigned long data) {
	if (!e) {
		return -1;
	}
	if (e->flags & LTC_USE_DATE) {
		/* the user-bits carry the date, refuse to clobber them */
		return -1;
	}

	if (stream_frame_is_fresh(e)) {
		/* no sample of the current frame has been emitted yet, so
		 * changing it here still updates this frame. */
		stream_set_user_bits(&e->f, data, e->standard, e->flags);
	} else {
		/* mid-frame: only the next frame may change. The request is
		 * applied when the encoder advances past the current frame. */
		e->pending_user_bits = data;
		e->pending_user_bits_valid = 1;
	}
	return 0;
}

void ltc_stream_encoder_get_timecode(LTCStreamEncoder *e, SMPTETimecode *t) {
	if (!e || !t) {
		return;
	}
	ltc_frame_to_time(t, &e->f, e->flags);
}
