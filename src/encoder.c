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

/*
 * -+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * Shared low level encoding primitives.  See encoder.h for a summary.
 */

void ltc_frame_set_user_bits(LTCFrame *f, unsigned long data) {
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
}

void ltc_filter_segment(ltcsnd_sample_t *wave, int n, double tcf, ltcsnd_sample_t tgtval) {
	if (tcf > 0) {
		/* low-pass-filter
		 * LTC signal should have a rise time of 40 us +/- 10 us.
		 *
		 * rise-time means from <10% to >90% of the signal.
		 * in each call to addvalues() we start at 50%, so
		 * here we need half-of it. (0.000020 sec)
		 *
		 * e->cutoff = 1.0 -exp( -1.0 / (sample_rate * .000020 / exp(1.0)) );
		 */
		int i;
		ltcsnd_sample_t val = SAMPLE_CENTER;
		int m = (n+1)>>1;
		for (i = 0 ; i < m ; i++) {
			val = val + tcf * (tgtval - val);
			wave[n-i-1] = wave[i] = val;
		}
	} else {
		/* perfect square wave */
		memset(wave, tgtval, n);
	}
}

void ltc_bmc_byte_begin(LTCBmcByte *b, unsigned char byte, int reverse) {
	b->byte         = byte;
	b->reverse      = reverse < 0 ? 1 : 0;
	b->mask         = b->reverse ? 128 : 1;
	b->is_one       = (byte & b->mask) ? 1 : 0;
	b->halves_left  = b->is_one ? 2 : 1;
}

static void bmc_advance(LTCBmcByte *b) {
	/* this is based on the assumption that with every compiler
	 * ((unsigned char) 128)<<1 == ((unsigned char 1)>>1) == 0
	 */
	b->mask = b->reverse ? (unsigned char)(b->mask >> 1) : (unsigned char)(b->mask << 1);
	b->is_one = b->mask ? ((b->byte & b->mask) ? 1 : 0) : 0;
	b->halves_left = b->is_one ? 2 : 1;
}

int ltc_bmc_byte_next(LTCBmcByte *b, LTCBmcState *s, int *n, int *level) {
	if (!b->mask) return 0;

	const double step = b->is_one ? s->samples_per_clock_2 : s->samples_per_clock;
	*n = (int)(step + s->sample_remainder);
	s->sample_remainder = step + s->sample_remainder - *n;
	s->state = !s->state;
	*level = s->state;

	if (b->is_one && --b->halves_left > 0) {
		/* still inside the two half-cells of a one bit */
	} else {
		bmc_advance(b);
	}
	return 1;
}

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

	ltc_filter_segment(&(e->buf[e->offset]), n, e->filter_const, tgtval);
	e->offset += n;
	return 0;
}

int encode_byte(LTCEncoder *e, int byte, double speed) {
	if (byte < 0 || byte > 9) return -1;
	if (speed ==0) return -1;

	int err = 0;
	LTCBmcByte b;
	LTCBmcState s;
	int n, level;

	s.samples_per_clock   = e->samples_per_clock * fabs(speed);
	s.samples_per_clock_2 = e->samples_per_clock_2 * fabs(speed);
	s.sample_remainder    = e->sample_remainder;
	s.state               = e->state;

	ltc_bmc_byte_begin(&b, ((const unsigned char*)&e->f)[byte], speed < 0 ? -1 : 1);

	while (ltc_bmc_byte_next(&b, &s, &n, &level)) {
		e->state = level;
		err |= addvalues(e, n);
	}

	e->sample_remainder = s.sample_remainder;
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
