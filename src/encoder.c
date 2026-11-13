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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "encoder.h"
#include "encoder_core.h"

/**
 * add values to the output buffer
 */
static int addvalues(LTCEncoder *e, int n) {
	const ltcsnd_sample_t tgtval = e->state ? e->enc_hi : e->enc_lo;

	if (e->offset + n >= e->bufsize) {
#if 0
		fprintf(stderr, "libltc: buffer overflow: %d/%lu\n", (int) e->offset, (unsigned long) e->bufsize);
#endif
		return 1;
	}

	if (e->filter_const > 0) {
		/* low-pass-filter, the exact recurrence is shared with the
		 * stream encoder through libltc_filter_ramp_step() */
		int i;
		ltcsnd_sample_t * const wave = &(e->buf[e->offset]);
		ltcsnd_sample_t val = SAMPLE_CENTER;
		int m = (n+1)>>1;
		for (i = 0 ; i < m ; i++) {
			val = libltc_filter_ramp_step(e->filter_const, val, tgtval);
			wave[n-i-1] = wave[i] = val;
		}
	} else {
		/* perfect square wave */
		memset(&(e->buf[e->offset]), tgtval, n);
	}

	e->offset += n;
	return 0;
}

int encode_byte(LTCEncoder *e, int byte, double speed) {
	if (byte < 0 || byte > 9) return -1;
	if (speed ==0) return -1;

	int err = 0;
	const unsigned char c = ((unsigned char*)&e->f)[byte];
	const int direction = (speed < 0) ? -1 : 1;
	int step;

	for (step = 0; step < 8; step++) {
		const int bit = (c & libltc_bmc_bit_mask(step, direction)) ? 1 : 0;
		int length[2];
		const int segs = libltc_bmc_segment_lengths(
				e->samples_per_clock, e->samples_per_clock_2,
				&e->sample_remainder, bit, speed, length);
		int s;
		for (s = 0; s < segs; s++) {
			e->state = !e->state;
			err |= addvalues(e, length[s]);
		}
	}

	return err;
}

int encode_transition(LTCEncoder *e) {
	if (e->offset + 1 >= e->bufsize) {
		return -1;
	}

	int off = e->offset;

	int n = e->bufsize - e->offset - 1;
	e->state = !e->state;
	int err = addvalues(e, n);

	if (e->filter_const <= 0) {
		e->offset = off + 1;
	}

	return err;
}
