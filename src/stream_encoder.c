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

/*
 * Allocation free, pull based LTC stream encoder.
 *
 * Unlike LTCEncoder, which renders whole frames into an internal PCM
 * buffer that the host has to drain, LTCStreamEncoder renders bit cells
 * on demand into a small fixed scratch area.  The caller may ask for any
 * number of samples; chunk boundaries may fall on a bit edge, halfway
 * through a bit cell, or in the middle of a filtered ramp.
 *
 * All timing arithmetic and waveform generation is delegated to the
 * primitives shared with the legacy encoder (see encoder.h), so a stream
 * of samples pulled from this encoder is byte for byte identical to the
 * concatenation of legacy ltc_encoder_encode_frame() /
 * ltc_encoder_encode_reversed_frame() output for the same settings.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ltc.h"
#include "encoder.h"

#ifndef SAMPLE_CENTER
#define SAMPLE_CENTER 128 /* unsigned 8 bit. */
#endif

/* Supported frame rates, compared as rounded hundredths of a frame. */
static int stream_fps_supported(double fps) {
	if (!(fps > 0.0) || isnan(fps) || isinf(fps)) {
		return 0;
	}
	const int h = (int) rint(fps * 100.0);
	return h == 2400 || h == 2500 || h == 3000 || h == 2997;
}

static int stream_standard_supported(enum LTC_TV_STANDARD standard) {
	switch (standard) {
		case LTC_TV_525_60:
		case LTC_TV_625_50:
		case LTC_TV_1125_60:
		case LTC_TV_FILM_24:
			return 1;
		default:
			return 0;
	}
}

static int stream_flags_supported(int flags) {
	const int known = LTC_USE_DATE | LTC_TC_CLOCK | LTC_BGF_DONT_TOUCH | LTC_NO_PARITY;
	return (flags & ~known) == 0;
}

LTCStreamEncoder *ltc_stream_encoder_create(
		double sample_rate, double fps,
		enum LTC_TV_STANDARD standard, int flags,
		const SMPTETimecode *start, int direction,
		double volume_db, double rise_time_us)
{
	if (!(sample_rate >= 32000.0 && sample_rate <= 192000.0)
			|| isnan(sample_rate) || isinf(sample_rate)) {
		return NULL;
	}
	if (!stream_fps_supported(fps)) return NULL;
	if (!stream_standard_supported(standard)) return NULL;
	if (!stream_flags_supported(flags)) return NULL;
	if (!start) return NULL;
	if (direction != 1 && direction != -1) return NULL;
	if (isnan(volume_db) || isinf(volume_db) || volume_db > 0.0) return NULL;
	if (isnan(rise_time_us) || isinf(rise_time_us) || rise_time_us < 0.0) return NULL;

	/* Validate / derive the 8 bit peak-to-peak amplitude the same way
	 * ltc_encoder_set_volume() does. */
	const double pp = rint(127.0 * pow(10, volume_db / 20.0));
	if (pp < 1 || pp > 127) return NULL;

	LTCStreamEncoder *e = (LTCStreamEncoder*) calloc(1, sizeof(LTCStreamEncoder));
	if (!e) return NULL;

	e->sample_rate = sample_rate;
	e->fps = fps;
	e->standard = standard;
	e->flags = flags;
	e->direction = direction;
	e->fps_int = (int) rint(fps);

	const ltcsnd_sample_t diff = ((ltcsnd_sample_t) pp) & 0x7f;
	e->enc_lo = SAMPLE_CENTER - diff;
	e->enc_hi = SAMPLE_CENTER + diff;

	e->bmc.samples_per_clock = sample_rate / (fps * 80.0);
	e->bmc.samples_per_clock_2 = e->bmc.samples_per_clock / 2.0;
	e->bmc.sample_remainder = 0.5;
	e->bmc.state = 0;

	if (rise_time_us <= 0.0) {
		e->filter_const = 0.0;
	} else {
		e->filter_const = 1.0 - exp(-1.0 / (sample_rate * rise_time_us / 2000000.0 / exp(1.0)));
	}

	/* Initialize the frame exactly like ltc_encoder_create() followed by
	 * ltc_encoder_set_timecode() would.  ltc_time_to_frame takes a
	 * non-const pointer but only reads through it for LTC_USE_DATE. */
	SMPTETimecode start_copy = *start;
	ltc_frame_reset(&e->f);

	if (flags & LTC_BGF_DONT_TOUCH) {
		e->f.col_frame = 0;
		e->f.binary_group_flag_bit1 = (flags & LTC_TC_CLOCK) ? 1 : 0;
		switch (standard) {
			case LTC_TV_625_50: /* 25 fps mode */
				e->f.biphase_mark_phase_correction = 0; /* BGF0 */
				e->f.binary_group_flag_bit0 = (flags & LTC_USE_DATE) ? 1 : 0; /* BGF2 */
				break;
			default:
				e->f.binary_group_flag_bit0 = 0;
				e->f.binary_group_flag_bit2 = (flags & LTC_USE_DATE) ? 1 : 0;
				break;
		}
	}
	if ((flags & LTC_NO_PARITY) == 0) {
		ltc_frame_set_parity(&e->f, standard);
	}
	e->f.dfbit = ((int) rint(fps * 100.0) == 2997) ? 1 : 0;

	ltc_time_to_frame(&e->f, &start_copy, standard, flags);

	e->label_valid = 0;
	e->frame_started = 0;
	e->byte_step = 0;
	e->byte_active = 0;
	e->bmc_byte.mask = 0;
	e->seg_len = 0;
	e->seg_pos = 0;
	e->seg_ends_frame = 0;
	e->pending_user_dirty = 0;
	e->pending_user = 0;

	return e;
}

void ltc_stream_encoder_free(LTCStreamEncoder *e) {
	free(e);
}

/* Move the label to the next frame, applying any user bits that were
 * queued while the previous frame was being emitted. */
static void stream_advance_frame(LTCStreamEncoder *e) {
	if (e->direction > 0) {
		ltc_frame_increment(&e->f, e->fps_int, e->standard, e->flags);
	} else {
		ltc_frame_decrement(&e->f, e->fps_int, e->standard, e->flags);
	}

	if (e->pending_user_dirty) {
		ltc_frame_set_user_bits(&e->f, e->pending_user);
		if ((e->flags & LTC_NO_PARITY) == 0) {
			ltc_frame_set_parity(&e->f, e->standard);
		}
		e->pending_user_dirty = 0;
	}

	e->label_valid = 0;
	e->frame_started = 0;
	e->byte_step = 0;
	e->byte_active = 0;
	e->bmc_byte.mask = 0;
	e->seg_len = 0;
	e->seg_pos = 0;
	e->seg_ends_frame = 0;
}

/* Produce one bit-cell segment of filtered PCM in e->segbuf.
 * Segments never cross a frame boundary. */
static int stream_fill_segment(LTCStreamEncoder *e) {
	for (;;) {
		if (!e->label_valid) {
			memcpy(e->label, &e->f, 10);
			e->label_valid = 1;
			e->byte_step = 0;
			e->byte_active = 0;
			e->bmc_byte.mask = 0;
		}

		if (!e->byte_active) {
			if (e->byte_step >= 10) {
				/* All bytes of this label were emitted; move on. */
				stream_advance_frame(e);
				continue;
			}
			const int idx = (e->direction > 0)
				? e->byte_step
				: (9 - e->byte_step);
			ltc_bmc_byte_begin(&e->bmc_byte, e->label[idx], e->direction);
			e->byte_active = 1;
		}

		int n = 0;
		int level = 0;
		if (ltc_bmc_byte_next(&e->bmc_byte, &e->bmc, &n, &level)) {
			if (n <= 0 || n > LTC_STREAM_SEG_MAX) {
				return -1;
			}
			ltc_filter_segment(e->segbuf, n, e->filter_const,
					level ? e->enc_hi : e->enc_lo);
			e->seg_len = n;
			e->seg_pos = 0;
			/* Is this the very last segment of the label? */
			e->seg_ends_frame = (e->bmc_byte.mask == 0 && e->byte_step == 9);
			return 0;
		}

		/* Current byte exhausted. */
		e->byte_active = 0;
		e->byte_step++;
	}
}

int ltc_stream_encoder_read(LTCStreamEncoder *e, ltcsnd_sample_t *output, size_t count) {
	if (!e) return -1;
	if (count == 0) return 0; /* nothing to do, state is untouched */
	if (!output) return -1;

	size_t written = 0;
	while (written < count) {
		if (e->seg_pos >= e->seg_len) {
			if (stream_fill_segment(e) != 0) {
				return -1;
			}
		}

		output[written++] = e->segbuf[e->seg_pos++];
		if (!e->frame_started) {
			e->frame_started = 1;
		}

		if (e->seg_pos >= e->seg_len && e->seg_ends_frame) {
			/* The last sample of this label has just been handed out.
			 * Advance now so that a timecode query reflects the label of
			 * the next sample to be output. */
			stream_advance_frame(e);
		}
	}
	return 0;
}

void ltc_stream_encoder_get_timecode(LTCStreamEncoder *e, SMPTETimecode *t) {
	if (!e || !t) return;
	ltc_frame_to_time(t, &e->f, e->flags);
}

int ltc_stream_encoder_set_user_bits(LTCStreamEncoder *e, unsigned long data) {
	if (!e) return -1;
	if (e->flags & LTC_USE_DATE) {
		/* The user fields carry the date; refuse to clobber them. */
		return -1;
	}

	if (!e->frame_started) {
		/* No sample of the current label has been output yet, so this
		 * update may still affect the current frame. */
		ltc_frame_set_user_bits(&e->f, data);
		if ((e->flags & LTC_NO_PARITY) == 0) {
			ltc_frame_set_parity(&e->f, e->standard);
		}
		e->label_valid = 0;
	} else {
		/* Mid-frame: only the next label is affected. */
		e->pending_user = data;
		e->pending_user_dirty = 1;
	}
	return 0;
}
