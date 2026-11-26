/*
   libltc - streaming LTC encoder tests

   Regression tests for LTCStreamEncoder.

   The tests serve two purposes:

     1. Every emitted PCM stream is compared byte for byte against the
        legacy LTCEncoder output for the same configuration and frame
        sequence (forward and reverse).

     2. The stream is decoded independently: bit-cell run lengths are
        measured from the raw PCM and matched against the timing that the
        fractional-sample rounding rule prescribes for the expected
        ten-byte label, and the bits are reassembled into a frame.  Frame
        progression, drop-frame skips and date wrap-around are checked
        against an independently built frame sequence.

   This file is compiled with support/test_main.c, which calls
   run_stream_encoder_tests().
*/

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"
#include "ltc.h"

/* -+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * Heap allocation tracking
 *
 * On platforms where malloc can be interposed we count allocations and
 * frees so that a read delivered to an audio callback can be proven to be
 * allocation free.
 */
#ifdef __APPLE__
#include <malloc/malloc.h>
static unsigned long g_alloc_calls;
static unsigned long g_free_calls;

void *malloc(size_t n) {
	++g_alloc_calls;
	return malloc_zone_malloc(malloc_default_zone(), n);
}

void *calloc(size_t a, size_t b) {
	++g_alloc_calls;
	return malloc_zone_calloc(malloc_default_zone(), a, b);
}

void *realloc(void *p, size_t n) {
	++g_alloc_calls;
	return malloc_zone_realloc(malloc_default_zone(), p, n);
}

void free(void *p) {
	++g_free_calls;
	if (p) {
		malloc_zone_free(malloc_default_zone(), p);
	}
}
#define HAVE_ALLOC_COUNT 1
#else
#define HAVE_ALLOC_COUNT 0
#endif

/* -+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * Small helpers
 */

#define MAX_CELLS 512
#define MAX_RUNS  512

typedef struct {
	size_t len;
	int level;
} Cell;

static double volume_peak(double volume_db) {
	return rint(127.0 * pow(10, volume_db / 20.0));
}

static ltcsnd_sample_t level_hi(double volume_db) {
	return (ltcsnd_sample_t)(128 + (((ltcsnd_sample_t) volume_peak(volume_db)) & 0x7f));
}

static ltcsnd_sample_t level_lo(double volume_db) {
	return (ltcsnd_sample_t)(128 - (((ltcsnd_sample_t) volume_peak(volume_db)) & 0x7f));
}

static int frame_rate_is_drop(double fps) {
	return (int) rint(fps * 100.0) == 2997;
}

/* Write 32 bits of user data into the eight nibbles, LSB first. */
static void set_user_bits_raw(LTCFrame *f, unsigned long data) {
	f->user1 = data & 0xF; data >>= 4;
	f->user2 = data & 0xF; data >>= 4;
	f->user3 = data & 0xF; data >>= 4;
	f->user4 = data & 0xF; data >>= 4;
	f->user5 = data & 0xF; data >>= 4;
	f->user6 = data & 0xF; data >>= 4;
	f->user7 = data & 0xF; data >>= 4;
	f->user8 = data & 0xF;
}

/*
 * Build the label (raw ten bytes) of the first frame exactly the way the
 * legacy encoder prepares its internal LTCFrame, independent of the
 * streaming encoder.
 */
static void frame_from_timecode(LTCFrame *f, const SMPTETimecode *st,
		enum LTC_TV_STANDARD standard, int flags, int dfbit) {
	ltc_frame_reset(f);

	if (flags & LTC_BGF_DONT_TOUCH) {
		f->col_frame = 0;
		f->binary_group_flag_bit1 = (flags & LTC_TC_CLOCK) ? 1 : 0;
		switch (standard) {
			case LTC_TV_625_50:
				f->biphase_mark_phase_correction = 0;
				f->binary_group_flag_bit0 = (flags & LTC_USE_DATE) ? 1 : 0;
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
	f->dfbit = dfbit;
	ltc_time_to_frame(f, (SMPTETimecode*) st, standard, flags);
}

/*
 * Independently build a sequence of frames following the documented
 * increment/decrement rules.
 */
static void build_sequence(double fps, enum LTC_TV_STANDARD standard, int flags,
		const SMPTETimecode *start, int direction, int nframes, LTCFrame *out) {
	const int fps_int = (int) rint(fps);
	const int dfbit = frame_rate_is_drop(fps);
	LTCFrame f;

	frame_from_timecode(&f, start, standard, flags, dfbit);
	for (int i = 0; i < nframes; ++i) {
		out[i] = f;
		if (i + 1 < nframes) {
			if (direction > 0) {
				ltc_frame_increment(&f, fps_int, standard, flags);
			} else {
				ltc_frame_decrement(&f, fps_int, standard, flags);
			}
		}
	}
}

/*
 * Reproduce the legacy encoder's PCM for nframes frames, incrementing or
 * decrementing between frames exactly like a host that encodes one frame
 * per call.  Returns a malloc'ed buffer and sets *outlen; labels are
 * placed in `labels` when non-NULL.
 */
static ltcsnd_sample_t *legacy_reference(double sample_rate, double fps,
		enum LTC_TV_STANDARD standard, int flags, double volume_db,
		double rise_time, const SMPTETimecode *start, int direction,
		int nframes, LTCFrame *labels, size_t *outlen) {
	LTCEncoder *le = ltc_encoder_create(sample_rate, fps, standard, flags);
	CHECK_CHECK(le != NULL);
	ltc_encoder_set_filter(le, rise_time);
	CHECK_CHECK(ltc_encoder_set_volume(le, volume_db) == 0);
	ltc_encoder_set_timecode(le, (SMPTETimecode*) start);

	const size_t cap = ltc_encoder_get_buffersize(le);
	ltcsnd_sample_t *buf = (ltcsnd_sample_t*) malloc(cap * (size_t) nframes + 64);
	CHECK_CHECK(buf != NULL);

	size_t n = 0;
	for (int i = 0; i < nframes; ++i) {
		if (labels) {
			ltc_encoder_get_frame(le, &labels[i]);
		}
		if (direction > 0) {
			ltc_encoder_encode_frame(le);
		} else {
			ltc_encoder_encode_reversed_frame(le);
		}
		n += (size_t) ltc_encoder_copy_buffer(le, buf + n);
		if (i + 1 < nframes) {
			if (direction > 0) {
				ltc_encoder_inc_timecode(le);
			} else {
				ltc_encoder_dec_timecode(le);
			}
		}
	}
	ltc_encoder_free(le);
	*outlen = n;
	return buf;
}

/* Deterministic pseudo random chunk size with a preference for the sizes
 * named by the task (1, 257, 1024) and values that land mid bit-cell. */
static size_t next_chunk(unsigned *seed, size_t remaining) {
	static const size_t favorites[] = { 1, 2, 3, 7, 13, 257, 511, 1024, 4096 };
	*seed = (*seed * 1103515245u) + 12345u;
	size_t want;
	switch ((*seed >> 16) % 4) {
		case 0:
			want = favorites[(*seed >> 8) % (sizeof(favorites) / sizeof(favorites[0]))];
			break;
		default:
			want = 1 + ((*seed >> 4) % 1500);
			break;
	}
	if (want > remaining) {
		want = remaining;
	}
	return want;
}

static void stream_read_chunked(LTCStreamEncoder *e, ltcsnd_sample_t *out,
		size_t total, unsigned seed) {
	size_t done = 0;
	while (done < total) {
		size_t n = next_chunk(&seed, total - done);
		CHECK_CHECK(ltc_stream_encoder_read(e, out + done, n) == 0);
		done += n;
	}
}

/* -+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * Independent BMC / label decoding
 */

/*
 * The fractional sample remainder is continuous across byte and frame
 * boundaries, so the independent timing model carries it from one frame
 * to the next.  `st` is advanced in place; start it at remainder 0.5,
 * phase 0 to model the beginning of a stream.
 */
typedef struct {
	double remainder;
	int state;
} SimState;

static void sim_state_init(SimState *st) {
	st->remainder = 0.5;
	st->state = 0;
}

/*
 * Simulate the bit-cell segmentation of one label using the documented
 * fractional rounding rule.  This is the independent model of the BMC
 * edges; it is not taken from the encoder implementation.
 */
static int simulate_cells(const unsigned char label[10], double samples_per_clock,
		int reverse, SimState *st, Cell *cells, int max_cells) {
	int count = 0;

	for (int bi = 0; bi < 10; ++bi) {
		const unsigned char byte = label[reverse ? (9 - bi) : bi];
		unsigned char mask = reverse ? 128 : 1;
		while (mask) {
			const int is_one = (byte & mask) ? 1 : 0;
			const int halves = is_one ? 2 : 1;
			const double step = is_one ? samples_per_clock / 2.0 : samples_per_clock;
			for (int h = 0; h < halves; ++h) {
				const int n = (int)(step + st->remainder);
				st->remainder = step + st->remainder - n;
				st->state = !st->state;
				CHECK_CHECK(count < max_cells);
				cells[count].len = (size_t) n;
				cells[count].level = st->state;
				++count;
			}
			mask = reverse ? (unsigned char)(mask >> 1) : (unsigned char)(mask << 1);
		}
	}
	return count;
}

/* Collect constant-level runs from a square-wave PCM buffer. */
static int collect_runs(const ltcsnd_sample_t *pcm, size_t len,
		size_t *starts, size_t *lens, int max_runs) {
	int nr = 0;
	if (len == 0) return 0;
	CHECK_CHECK(nr < max_runs);
	starts[nr] = 0;
	for (size_t i = 1; i < len; ++i) {
		if (pcm[i] != pcm[i - 1]) {
			lens[nr] = i - starts[nr];
			++nr;
			CHECK_CHECK(nr < max_runs);
			starts[nr] = i;
		}
	}
	lens[nr] = len - starts[nr];
	++nr;
	return nr;
}

/*
 * Verify one frame of square-wave PCM against the independently simulated
 * bit cells:
 *  - every bit-cell run length matches the simulation,
 *  - the level of every run matches the alternating BMC phase,
 *  - the bit stream reassembles into exactly the expected label.
 * `nc`/`cells` come from simulate_cells() for this label.  Returns the
 * number of samples the frame occupies.
 */
static size_t check_frame_cells(const char *what, const unsigned char label[10],
		const Cell *cells, int nc, const ltcsnd_sample_t *pcm, size_t len,
		double samples_per_clock, int reverse, double volume_db) {
	size_t starts[MAX_RUNS], lens[MAX_RUNS];
	size_t expected_len = 0;
	for (int i = 0; i < nc; ++i) {
		expected_len += cells[i].len;
	}
	CHECK_CHECK(len >= expected_len);

	const int nr = collect_runs(pcm, len, starts, lens, MAX_RUNS);
	CHECK_CHECK(nr >= nc);

	const ltcsnd_sample_t hi = level_hi(volume_db);

	for (int i = 0; i < nc; ++i) {
		if (lens[i] != cells[i].len) {
			fprintf(stderr, "%s: cell %d length %zu, expected %zu (spc %.4f rev %d)\n",
					what, i, lens[i], cells[i].len, samples_per_clock, reverse);
			CHECK_CHECK(0);
		}
		const ltcsnd_sample_t want = cells[i].level ? hi : level_lo(volume_db);
		if (pcm[starts[i]] != want) {
			fprintf(stderr, "%s: cell %d level %d, expected %d\n",
					what, i, pcm[starts[i]], want);
			CHECK_CHECK(0);
		}
	}

	/* The sample after the frame must start a new level (BMC edge). */
	if (len > expected_len) {
		CHECK_CHECK(pcm[expected_len] != pcm[expected_len - 1]);
	}

	/* Reassemble the bits from run lengths alone. */
	unsigned char decoded[10];
	memset(decoded, 0, sizeof(decoded));
	int bit = 0;
	int r = 0;
	while (bit < LTC_FRAME_BIT_COUNT) {
		CHECK_CHECK(r < nr);
		const int full = ((double) lens[r] > 0.75 * samples_per_clock);
		if (full) {
			/* bit stays zero */
			++r;
		} else {
			/* two half-cells form a one bit */
			CHECK_CHECK(r + 1 < nr);
			CHECK_CHECK((double) lens[r + 1] <= 0.75 * samples_per_clock);
			if (reverse) {
				decoded[9 - bit / 8] |= (unsigned char)(1u << (7 - (bit % 8)));
			} else {
				decoded[bit / 8] |= (unsigned char)(1u << (bit % 8));
			}
			r += 2;
		}
		++bit;
	}
	CHECK_CHECK(r == nc);
	if (memcmp(decoded, label, 10) != 0) {
		fprintf(stderr, "%s: decoded label mismatch\n", what);
		for (int i = 0; i < 10; ++i) {
			fprintf(stderr, "  byte %d: got %02x want %02x\n", i, decoded[i], label[i]);
		}
		CHECK_CHECK(0);
	}
	return expected_len;
}

/* Simulate one label and check the PCM for it, using fresh stream state.
 * Use this only where the frame is known to start at the beginning of a
 * stream; for multi-frame sequences carry a SimState instead. */
static size_t decode_and_check_frame(const char *what, const unsigned char label[10],
		const ltcsnd_sample_t *pcm, size_t len, double samples_per_clock,
		int reverse, double volume_db) {
	SimState sim;
	Cell cells[MAX_CELLS];
	sim_state_init(&sim);
	const int nc = simulate_cells(label, samples_per_clock, reverse, &sim, cells, MAX_CELLS);
	CHECK_CHECK(nc > 0);
	return check_frame_cells(what, label, cells, nc, pcm, len, samples_per_clock,
			reverse, volume_db);
}

/* Number of samples a label occupies when it starts at the beginning of a
 * stream (remainder 0.5, phase 0). */
static size_t frame_len_fresh(const unsigned char label[10], double samples_per_clock,
		int reverse) {
	SimState sim;
	Cell cells[MAX_CELLS];
	sim_state_init(&sim);
	const int nc = simulate_cells(label, samples_per_clock, reverse, &sim, cells, MAX_CELLS);
	size_t len = 0;
	for (int i = 0; i < nc; ++i) len += cells[i].len;
	return len;
}

/* -+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * Tests
 */

static void test_parameter_validation(void) {
	SMPTETimecode st;
	memset(&st, 0, sizeof(st));
	strcpy(st.timezone, "+0100");
	st.hours = 1;

	/* Valid boundary values must be accepted. */
	LTCStreamEncoder *ok;
	ok = ltc_stream_encoder_create(32000, 24, LTC_TV_FILM_24, 0, &st, 1, -3.0, 40.0);
	CHECK_CHECK(ok != NULL);
	ltc_stream_encoder_free(ok);
	ok = ltc_stream_encoder_create(192000, 30000.0 / 1001.0, LTC_TV_525_60, 0, &st, -1, 0.0, 0.0);
	CHECK_CHECK(ok != NULL);
	ltc_stream_encoder_free(ok);

	/* Unsupported sample rates. */
	CHECK_CHECK(ltc_stream_encoder_create(31999, 25, LTC_TV_625_50, 0, &st, 1, -3.0, 40.0) == NULL);
	CHECK_CHECK(ltc_stream_encoder_create(192001, 25, LTC_TV_625_50, 0, &st, 1, -3.0, 40.0) == NULL);
	CHECK_CHECK(ltc_stream_encoder_create(0, 25, LTC_TV_625_50, 0, &st, 1, -3.0, 40.0) == NULL);
	CHECK_CHECK(ltc_stream_encoder_create(-48000, 25, LTC_TV_625_50, 0, &st, 1, -3.0, 40.0) == NULL);
	CHECK_CHECK(ltc_stream_encoder_create(NAN, 25, LTC_TV_625_50, 0, &st, 1, -3.0, 40.0) == NULL);

	/* Unsupported frame rates. */
	CHECK_CHECK(ltc_stream_encoder_create(48000, 23.976, LTC_TV_FILM_24, 0, &st, 1, -3.0, 40.0) == NULL);
	CHECK_CHECK(ltc_stream_encoder_create(48000, 26, LTC_TV_625_50, 0, &st, 1, -3.0, 40.0) == NULL);
	CHECK_CHECK(ltc_stream_encoder_create(48000, 50, LTC_TV_625_50, 0, &st, 1, -3.0, 40.0) == NULL);
	CHECK_CHECK(ltc_stream_encoder_create(48000, 0, LTC_TV_625_50, 0, &st, 1, -3.0, 40.0) == NULL);

	/* Invalid standard, flags, start timecode, direction. */
	CHECK_CHECK(ltc_stream_encoder_create(48000, 25, (enum LTC_TV_STANDARD) 99, 0, &st, 1, -3.0, 40.0) == NULL);
	CHECK_CHECK(ltc_stream_encoder_create(48000, 25, LTC_TV_625_50, 0x100, &st, 1, -3.0, 40.0) == NULL);
	CHECK_CHECK(ltc_stream_encoder_create(48000, 25, LTC_TV_625_50, 0, NULL, 1, -3.0, 40.0) == NULL);
	CHECK_CHECK(ltc_stream_encoder_create(48000, 25, LTC_TV_625_50, 0, &st, 0, -3.0, 40.0) == NULL);
	CHECK_CHECK(ltc_stream_encoder_create(48000, 25, LTC_TV_625_50, 0, &st, 2, -3.0, 40.0) == NULL);

	/* Invalid volume / rise time. */
	CHECK_CHECK(ltc_stream_encoder_create(48000, 25, LTC_TV_625_50, 0, &st, 1, 1.0, 40.0) == NULL);
	CHECK_CHECK(ltc_stream_encoder_create(48000, 25, LTC_TV_625_50, 0, &st, 1, -100.0, 40.0) == NULL);
	CHECK_CHECK(ltc_stream_encoder_create(48000, 25, LTC_TV_625_50, 0, &st, 1, NAN, 40.0) == NULL);
	CHECK_CHECK(ltc_stream_encoder_create(48000, 25, LTC_TV_625_50, 0, &st, 1, -3.0, -1.0) == NULL);

	/* read argument checks and zero-length semantics. */
	ok = ltc_stream_encoder_create(48000, 25, LTC_TV_625_50, 0, &st, 1, -3.0, 0.0);
	CHECK_CHECK(ok != NULL);
	ltcsnd_sample_t sample = 0;
	CHECK_CHECK(ltc_stream_encoder_read(NULL, &sample, 1) == -1);
	CHECK_CHECK(ltc_stream_encoder_read(ok, NULL, 1) == -1);
	CHECK_CHECK(ltc_stream_encoder_read(ok, NULL, 0) == 0);

	SMPTETimecode before, after;
	ltc_stream_encoder_get_timecode(ok, &before);
	CHECK_CHECK(ltc_stream_encoder_read(ok, &sample, 0) == 0);
	ltc_stream_encoder_get_timecode(ok, &after);
	CHECK_CHECK(memcmp(&before, &after, sizeof(before)) == 0);
	ltc_stream_encoder_free(ok);
}

static void test_byte_identical_to_legacy(void) {
	struct Config {
		double sample_rate;
		double fps;
		enum LTC_TV_STANDARD standard;
		int flags;
	} configs[] = {
		{ 48000, 25,                LTC_TV_625_50,  0 },
		{ 48000, 30,                LTC_TV_525_60,  0 },
		{ 48000, 24,                LTC_TV_FILM_24, 0 },
		{ 48000, 30000.0 / 1001.0,  LTC_TV_525_60,  0 },
		{ 44100, 25,                LTC_TV_625_50,  0 },
		{ 44100, 30000.0 / 1001.0,  LTC_TV_525_60,  0 },
		{ 32000, 30,                LTC_TV_525_60,  0 },
		{ 32000, 24,                LTC_TV_FILM_24, 0 },
		{ 96000, 25,                LTC_TV_625_50,  0 },
		{ 192000, 24,               LTC_TV_FILM_24, 0 },
		{ 48000, 25,                LTC_TV_625_50,  LTC_USE_DATE },
		{ 48000, 30000.0 / 1001.0,  LTC_TV_525_60,  LTC_USE_DATE },
		{ 48000, 25,                LTC_TV_625_50,  LTC_NO_PARITY },
		{ 48000, 25,                LTC_TV_625_50,  LTC_BGF_DONT_TOUCH | LTC_TC_CLOCK },
		{ 48000, 25,                LTC_TV_625_50,  LTC_BGF_DONT_TOUCH | LTC_USE_DATE },
		{ 48000, 30,                LTC_TV_1125_60, 0 },
	};
	const double volumes[] = { -3.0, -18.0, -1.0, -30.0 };
	const double rises[] = { 40.0, 0.0, 25.0 };
	const int nframes = 4;

	for (size_t c = 0; c < sizeof(configs) / sizeof(configs[0]); ++c) {
		for (size_t v = 0; v < sizeof(volumes) / sizeof(volumes[0]); ++v) {
			for (size_t r = 0; r < sizeof(rises) / sizeof(rises[0]); ++r) {
				for (int direction = -1; direction <= 1; direction += 2) {
					SMPTETimecode st;
					memset(&st, 0, sizeof(st));
					strcpy(st.timezone, "+0100");
					st.years = 8;
					st.months = 12;
					st.days = 31;
					st.hours = 23;
					st.mins = 59;
					st.secs = 59;
					st.frame = 0;

					LTCFrame legacy_labels[4];
					size_t ref_len = 0;
					ltcsnd_sample_t *ref = legacy_reference(
							configs[c].sample_rate, configs[c].fps, configs[c].standard,
							configs[c].flags, volumes[v], rises[r], &st, direction,
							nframes, legacy_labels, &ref_len);

					LTCStreamEncoder *se = ltc_stream_encoder_create(
							configs[c].sample_rate, configs[c].fps, configs[c].standard,
							configs[c].flags, &st, direction, volumes[v], rises[r]);
					CHECK_CHECK(se != NULL);

					ltcsnd_sample_t *got = (ltcsnd_sample_t*) malloc(ref_len + 64);
					CHECK_CHECK(got != NULL);

					unsigned seed = (unsigned)(c * 977 + v * 131 + r * 17 + direction + 100);
					stream_read_chunked(se, got, ref_len, seed);

					/* Labels must agree with the independent sequence too. */
					LTCFrame expected[4];
					build_sequence(configs[c].fps, configs[c].standard, configs[c].flags,
							&st, direction, nframes, expected);
					for (int i = 0; i < nframes; ++i) {
						CHECK_CHECK(memcmp(&expected[i], &legacy_labels[i], sizeof(LTCFrame)) == 0);
					}

					if (memcmp(ref, got, ref_len) != 0) {
						size_t i = 0;
						while (i < ref_len && ref[i] == got[i]) ++i;
						fprintf(stderr,
								"PCM mismatch: sr=%g fps=%g std=%d flags=%d vol=%g rise=%g dir=%d at %zu (ref %d got %d)\n",
								configs[c].sample_rate, configs[c].fps, configs[c].standard,
								configs[c].flags, volumes[v], rises[r], direction, i,
								i < ref_len ? ref[i] : -1, i < ref_len ? got[i] : -1);
						CHECK_CHECK(0);
					}

					/* The timecode after all frames reflects the next frame. */
					SMPTETimecode next_tc;
					ltc_stream_encoder_get_timecode(se, &next_tc);
					SMPTETimecode expected_tc;
					ltc_frame_to_time(&expected_tc, &expected[nframes - 1], configs[c].flags);
					LTCFrame one = expected[nframes - 1];
					if (direction > 0) {
						ltc_frame_increment(&one, (int) rint(configs[c].fps),
								configs[c].standard, configs[c].flags);
					} else {
						ltc_frame_decrement(&one, (int) rint(configs[c].fps),
								configs[c].standard, configs[c].flags);
					}
					ltc_frame_to_time(&expected_tc, &one, configs[c].flags);
					CHECK_CHECK(memcmp(&next_tc, &expected_tc, sizeof(SMPTETimecode)) == 0);

					free(ref);
					free(got);
					ltc_stream_encoder_free(se);
				}
			}
		}
	}
}

/*
 * Independently verify the BMC bit-cell edges and the ten-byte label for
 * forward and reverse streams, including fractional bit widths.
 */
static void test_bmc_edges_and_labels(void) {
	struct Config {
		double sample_rate;
		double fps;
		enum LTC_TV_STANDARD standard;
		double volume;
		int direction;
	} configs[] = {
		{ 48000, 25,               LTC_TV_625_50,  -3.0,  1 },
		{ 48000, 25,               LTC_TV_625_50,  -18.0, -1 },
		{ 48000, 30,               LTC_TV_525_60,  -3.0,  1 },
		{ 48000, 24,               LTC_TV_FILM_24, -3.0, -1 },
		{ 44100, 25,               LTC_TV_625_50,  -1.0,  1 },
		{ 44100, 30000.0 / 1001.0, LTC_TV_525_60,  -3.0, -1 },
		{ 32000, 30,               LTC_TV_525_60,  -30.0, 1 },
		{ 192000, 24,              LTC_TV_FILM_24, -3.0, -1 },
	};
	const int nframes = 3;

	for (size_t c = 0; c < sizeof(configs) / sizeof(configs[0]); ++c) {
		const double spc = configs[c].sample_rate / (configs[c].fps * 80.0);

		SMPTETimecode st;
		memset(&st, 0, sizeof(st));
		strcpy(st.timezone, "+0000");
		st.years = 1;
		st.months = 2;
		st.days = 3;
		st.hours = 10;
		st.mins = 20;
		st.secs = 30;
		st.frame = 5;

		LTCFrame seq[3];
		build_sequence(configs[c].fps, configs[c].standard, 0, &st,
				configs[c].direction, nframes, seq);

		LTCStreamEncoder *se = ltc_stream_encoder_create(
				configs[c].sample_rate, configs[c].fps, configs[c].standard, 0, &st,
				configs[c].direction, configs[c].volume, 0.0 /* square wave */);
		CHECK_CHECK(se != NULL);

		SimState sim;
		sim_state_init(&sim);
		for (int f = 0; f < nframes; ++f) {
			/* Simulate this frame (carrying the fractional remainder), then
			 * read exactly that many samples so the next frame starts aligned. */
			Cell cells[MAX_CELLS];
			const int nc = simulate_cells((const unsigned char*)&seq[f], spc,
					configs[c].direction < 0, &sim, cells, MAX_CELLS);
			CHECK_CHECK(nc > 0);
			size_t want = 0;
			for (int i = 0; i < nc; ++i) want += cells[i].len;

			ltcsnd_sample_t pcm[3 * 8192];
			CHECK_CHECK(want <= sizeof(pcm));
			stream_read_chunked(se, pcm, want, (unsigned)(c * 31 + f * 7 + 3));

			const size_t used = check_frame_cells("bmc", (const unsigned char*)&seq[f],
					cells, nc, pcm, want, spc, configs[c].direction < 0,
					configs[c].volume);

			/* The frame must not consume more than the audio frame. */
			CHECK_CHECK(used <= (size_t)(configs[c].sample_rate / configs[c].fps) + 2);
		}
		ltc_stream_encoder_free(se);
	}
}

/*
 * The timecode query must report the frame of the next sample: the last
 * sample of a frame still belongs to that frame, and only afterwards does
 * the timecode advance.
 */
static void test_timecode_boundary_semantics(void) {
	struct Config {
		double fps;
		enum LTC_TV_STANDARD standard;
		int flags;
		int direction;
	} configs[] = {
		{ 25,               LTC_TV_625_50, 0,             1 },
		{ 25,               LTC_TV_625_50, 0,            -1 },
		{ 30000.0 / 1001.0, LTC_TV_525_60, 0,             1 },
		{ 30000.0 / 1001.0, LTC_TV_525_60, 0,            -1 },
		{ 25,               LTC_TV_625_50, LTC_USE_DATE,  1 },
	};

	for (size_t c = 0; c < sizeof(configs) / sizeof(configs[0]); ++c) {
		SMPTETimecode st;
		memset(&st, 0, sizeof(st));
		strcpy(st.timezone, "+0100");
		st.years = 9;
		st.months = 6;
		st.days = 15;
		st.hours = 12;
		st.mins = 34;
		st.secs = 56;
		st.frame = 7;

		LTCFrame seq[2];
		build_sequence(configs[c].fps, configs[c].standard, configs[c].flags,
				&st, configs[c].direction, 2, seq);
		const double spc = 48000.0 / (configs[c].fps * 80.0);
		Cell cells[MAX_CELLS];
		SimState sim;
		sim_state_init(&sim);
		const int nc = simulate_cells((const unsigned char*)&seq[0], spc,
				configs[c].direction < 0, &sim, cells, MAX_CELLS);
		size_t frame_len = 0;
		for (int i = 0; i < nc; ++i) frame_len += cells[i].len;

		LTCStreamEncoder *se = ltc_stream_encoder_create(48000, configs[c].fps,
				configs[c].standard, configs[c].flags, &st, configs[c].direction,
				-3.0, 0.0);
		CHECK_CHECK(se != NULL);

		ltcsnd_sample_t *pcm = (ltcsnd_sample_t*) malloc(frame_len + 8);
		CHECK_CHECK(pcm != NULL);

		/* Read one sample less than the frame. */
		const size_t partial = frame_len - 1;
		stream_read_chunked(se, pcm, partial, 99);

		SMPTETimecode current;
		ltc_stream_encoder_get_timecode(se, &current);
		CHECK_CHECK(current.hours == st.hours && current.mins == st.mins
				&& current.secs == st.secs && current.frame == st.frame);

		/* A zero-length read must not advance either. */
		CHECK_CHECK(ltc_stream_encoder_read(se, NULL, 0) == 0);
		ltc_stream_encoder_get_timecode(se, &current);
		CHECK_CHECK(current.hours == st.hours && current.mins == st.mins
				&& current.secs == st.secs && current.frame == st.frame);

		/* Deliver the final sample: still the current frame before the call
		 * returns, but the next sample belongs to the next frame afterwards. */
		CHECK_CHECK(ltc_stream_encoder_read(se, pcm + partial, 1) == 0);
		SMPTETimecode next;
		ltc_stream_encoder_get_timecode(se, &next);
		SMPTETimecode want_next;
		ltc_frame_to_time(&want_next, &seq[1], configs[c].flags);
		CHECK_CHECK(memcmp(&next, &want_next, sizeof(SMPTETimecode)) == 0);

		/* Fully decode the frame from the sampled PCM. */
		decode_and_check_frame("boundary", (const unsigned char*)&seq[0], pcm,
				frame_len, spc, configs[c].direction < 0, -3.0);

		free(pcm);
		ltc_stream_encoder_free(se);
	}
}

/* Drop-frame: the first two frame numbers after a non-tenth minute are
 * skipped when advancing, and re-inserted when moving backwards. */
static void test_drop_frame_minute_boundary(void) {
	const double fps = 30000.0 / 1001.0;
	const enum LTC_TV_STANDARD standard = LTC_TV_525_60;

	/* Forward: 00:00:59:00 .. 00:00:59:29 then 00:01:00:02. */
	{
		SMPTETimecode st;
		memset(&st, 0, sizeof(st));
		strcpy(st.timezone, "+0000");
		st.hours = 0;
		st.mins = 0;
		st.secs = 59;
		st.frame = 0;

		LTCStreamEncoder *se = ltc_stream_encoder_create(48000, fps, standard, 0,
				&st, 1, -3.0, 0.0);
		CHECK_CHECK(se != NULL);

		const double spc = 48000.0 / (fps * 80.0);
		ltcsnd_sample_t pcm[3 * 8192];
		SimState sim;
		sim_state_init(&sim);

		/* Encode all 30 frames of second 59 and one more. */
		for (int i = 0; i < 31; ++i) {
			SMPTETimecode tc;
			ltc_stream_encoder_get_timecode(se, &tc);
			if (i == 30) {
				CHECK_CHECK(tc.hours == 0 && tc.mins == 1 && tc.secs == 0);
				CHECK_CHECK(tc.frame == 2); /* 00:01:00:00 and :01 skipped */
			} else {
				CHECK_CHECK(tc.hours == 0 && tc.mins == 0 && tc.secs == 59);
				CHECK_CHECK(tc.frame == i);
			}

			/* Verify the label of this frame from the PCM. */
			LTCFrame seq[1];
			build_sequence(fps, standard, 0, &tc, 1, 1, seq);
			Cell cells[MAX_CELLS];
			const int nc = simulate_cells((const unsigned char*)&seq[0], spc, 0,
					&sim, cells, MAX_CELLS);
			CHECK_CHECK(nc > 0);
			size_t want = 0;
			for (int z = 0; z < nc; ++z) want += cells[z].len;
			CHECK_CHECK(want <= sizeof(pcm));
			stream_read_chunked(se, pcm, want, (unsigned)(i + 1) * 13);
			check_frame_cells("df-forward", (const unsigned char*)&seq[0],
					cells, nc, pcm, want, spc, 0, -3.0);
		}
		CHECK_CHECK(ltc_stream_encoder_read(se, NULL, 0) == 0);
		ltc_stream_encoder_free(se);
	}

	/* Reverse from 00:01:00:02: one frame back is 00:00:59:29. */
	{
		SMPTETimecode st;
		memset(&st, 0, sizeof(st));
		strcpy(st.timezone, "+0000");
		st.hours = 0;
		st.mins = 1;
		st.secs = 0;
		st.frame = 2;

		LTCStreamEncoder *se = ltc_stream_encoder_create(48000, fps, standard, 0,
				&st, -1, -3.0, 0.0);
		CHECK_CHECK(se != NULL);

		/* Consume one full reversed frame. */
		LTCFrame seq[2];
		build_sequence(fps, standard, 0, &st, -1, 2, seq);
		Cell cells[MAX_CELLS];
		const double spc = 48000.0 / (fps * 80.0);
		SimState sim;
		sim_state_init(&sim);
		const int nc = simulate_cells((const unsigned char*)&seq[0], spc, 1,
				&sim, cells, MAX_CELLS);
		size_t frame_len = 0;
		for (int i = 0; i < nc; ++i) frame_len += cells[i].len;

		ltcsnd_sample_t *pcm = (ltcsnd_sample_t*) malloc(frame_len);
		CHECK_CHECK(pcm != NULL);
		stream_read_chunked(se, pcm, frame_len, 7);
		decode_and_check_frame("df-reverse", (const unsigned char*)&seq[0],
				pcm, frame_len, spc, 1, -3.0);

		SMPTETimecode tc;
		ltc_stream_encoder_get_timecode(se, &tc);
		CHECK_CHECK(tc.hours == 0 && tc.mins == 0 && tc.secs == 59 && tc.frame == 29);

		/* And legacy must agree for the same reverse step. */
		size_t ref_len = 0;
		LTCFrame labels[1];
		ltcsnd_sample_t *ref = legacy_reference(48000, fps, standard, 0, -3.0, 0.0,
				&st, -1, 1, labels, &ref_len);
		CHECK_CHECK(ref_len == frame_len);
		CHECK_CHECK(memcmp(ref, pcm, ref_len) == 0);

		free(ref);
		free(pcm);
		ltc_stream_encoder_free(se);
	}
}

/* Date handling across midnight, forwards and backwards. */
static void test_midnight_date_crossing(void) {
	const double fps = 25;
	const enum LTC_TV_STANDARD standard = LTC_TV_625_50;
	const int flags = LTC_USE_DATE;

	/* Forward: 2008-12-31 23:59:59:24 -> 2009-01-01 00:00:00:00. */
	{
		SMPTETimecode st;
		memset(&st, 0, sizeof(st));
		strcpy(st.timezone, "+0100");
		st.years = 8;
		st.months = 12;
		st.days = 31;
		st.hours = 23;
		st.mins = 59;
		st.secs = 59;
		st.frame = 24;

		LTCFrame seq[2];
		build_sequence(fps, standard, flags, &st, 1, 2, seq);

		LTCStreamEncoder *se = ltc_stream_encoder_create(48000, fps, standard,
				flags, &st, 1, -3.0, 0.0);
		CHECK_CHECK(se != NULL);

		const double spc = 48000.0 / (fps * 80.0);
		const size_t frame_len = frame_len_fresh((const unsigned char*)&seq[0],
				spc, 0);

		ltcsnd_sample_t *pcm = (ltcsnd_sample_t*) malloc(frame_len);
		CHECK_CHECK(pcm != NULL);
		stream_read_chunked(se, pcm, frame_len, 5);
		decode_and_check_frame("midnight-fwd", (const unsigned char*)&seq[0],
				pcm, frame_len, spc, 0, -3.0);

		SMPTETimecode tc;
		ltc_stream_encoder_get_timecode(se, &tc);
		CHECK_CHECK(tc.years == 9 && tc.months == 1 && tc.days == 1);
		CHECK_CHECK(tc.hours == 0 && tc.mins == 0 && tc.secs == 0 && tc.frame == 0);

		/* Legacy byte comparison across the wrap. */
		size_t ref_len = 0;
		ltcsnd_sample_t *ref = legacy_reference(48000, fps, standard, flags, -3.0,
				0.0, &st, 1, 2, NULL, &ref_len);
		CHECK_CHECK(ref_len >= frame_len);
		CHECK_CHECK(memcmp(ref, pcm, frame_len) == 0);

		free(ref);
		free(pcm);
		ltc_stream_encoder_free(se);
	}

	/* Reverse: 2009-01-01 00:00:00:00 -> 2008-12-31 23:59:59:24. */
	{
		SMPTETimecode st;
		memset(&st, 0, sizeof(st));
		strcpy(st.timezone, "+0100");
		st.years = 9;
		st.months = 1;
		st.days = 1;
		st.hours = 0;
		st.mins = 0;
		st.secs = 0;
		st.frame = 0;

		LTCFrame seq[2];
		build_sequence(fps, standard, flags, &st, -1, 2, seq);

		LTCStreamEncoder *se = ltc_stream_encoder_create(48000, fps, standard,
				flags, &st, -1, -3.0, 0.0);
		CHECK_CHECK(se != NULL);

		const double spc = 48000.0 / (fps * 80.0);
		const size_t frame_len = frame_len_fresh((const unsigned char*)&seq[0],
				spc, 1);

		ltcsnd_sample_t *pcm = (ltcsnd_sample_t*) malloc(frame_len);
		CHECK_CHECK(pcm != NULL);
		stream_read_chunked(se, pcm, frame_len, 6);
		decode_and_check_frame("midnight-rev", (const unsigned char*)&seq[0],
				pcm, frame_len, spc, 1, -3.0);

		SMPTETimecode tc;
		ltc_stream_encoder_get_timecode(se, &tc);
		CHECK_CHECK(tc.years == 8 && tc.months == 12 && tc.days == 31);
		CHECK_CHECK(tc.hours == 23 && tc.mins == 59 && tc.secs == 59 && tc.frame == 24);

		size_t ref_len = 0;
		ltcsnd_sample_t *ref = legacy_reference(48000, fps, standard, flags, -3.0,
				0.0, &st, -1, 1, NULL, &ref_len);
		CHECK_CHECK(ref_len == frame_len);
		CHECK_CHECK(memcmp(ref, pcm, frame_len) == 0);

		free(ref);
		free(pcm);
		ltc_stream_encoder_free(se);
	}
}

/*
 * User bits set mid-frame affect only the next frame; the first and second
 * half of the current frame carry the same label.  Setting before any
 * sample is output affects the current frame.
 */
static void test_user_bits_mid_frame(void) {
	const double fps = 25;
	const enum LTC_TV_STANDARD standard = LTC_TV_625_50;
	const unsigned long new_bits = 0xDEADBEEFul;
	const double spc = 48000.0 / (fps * 80.0);

	SMPTETimecode st;
	memset(&st, 0, sizeof(st));
	strcpy(st.timezone, "+0000");
	st.hours = 1;
	st.mins = 2;
	st.secs = 3;
	st.frame = 4;

	LTCFrame seq[2];
	build_sequence(fps, standard, 0, &st, 1, 2, seq);

	/* Expected labels: seq[0] with zero user bits, seq[1] with new bits. */
	CHECK_CHECK(ltc_frame_get_user_bits(&seq[0]) == 0);
	LTCFrame expected_next = seq[1];
	set_user_bits_raw(&expected_next, new_bits);
	ltc_frame_set_parity(&expected_next, standard);

	LTCStreamEncoder *se = ltc_stream_encoder_create(48000, fps, standard, 0,
			&st, 1, -3.0, 0.0);
	CHECK_CHECK(se != NULL);

	/* Read part of the first frame, then update user bits. */
	ltcsnd_sample_t pcm[2 * 8192];
	const size_t frame_len = (size_t)(48000.0 / fps);
	const size_t partial = 200;
	stream_read_chunked(se, pcm, partial, 11);

	CHECK_CHECK(ltc_stream_encoder_set_user_bits(se, new_bits) == 0);

	/* Remaining samples of the current frame must not be affected. */
	stream_read_chunked(se, pcm + partial, frame_len - partial, 12);

	SMPTETimecode tc;
	ltc_stream_encoder_get_timecode(se, &tc);
	LTCFrame want_next_tc;
	frame_from_timecode(&want_next_tc, &tc, standard, 0, 0);
	CHECK_CHECK(memcmp(&want_next_tc, &seq[1], sizeof(LTCFrame)) == 0);

	/* The whole first frame decodes to the original label (all zero users),
	 * proving both halves share the same ten-byte data. */
	decode_and_check_frame("userbits-current", (const unsigned char*)&seq[0],
			pcm, frame_len, spc, 0, -3.0);

	/* Now the next frame must carry the new user bits. */
	ltcsnd_sample_t pcm2[8192];
	stream_read_chunked(se, pcm2, (size_t)(48000.0 / fps), 13);
	decode_and_check_frame("userbits-next", (const unsigned char*)&expected_next,
			pcm2, (size_t)(48000.0 / fps), spc, 0, -3.0);

	/* Setting before the first sample of a frame affects that frame. */
	LTCStreamEncoder *se2 = ltc_stream_encoder_create(48000, fps, standard, 0,
			&st, 1, -3.0, 0.0);
	CHECK_CHECK(se2 != NULL);
	CHECK_CHECK(ltc_stream_encoder_set_user_bits(se2, new_bits) == 0);
	LTCFrame expected_first = seq[0];
	set_user_bits_raw(&expected_first, new_bits);
	ltc_frame_set_parity(&expected_first, standard);
	stream_read_chunked(se2, pcm2, (size_t)(48000.0 / fps), 14);
	decode_and_check_frame("userbits-before", (const unsigned char*)&expected_first,
			pcm2, (size_t)(48000.0 / fps), spc, 0, -3.0);

	/* With LTC_USE_DATE raw user bits must be rejected. */
	LTCStreamEncoder *se3 = ltc_stream_encoder_create(48000, fps, standard,
			LTC_USE_DATE, &st, 1, -3.0, 0.0);
	CHECK_CHECK(se3 != NULL);
	CHECK_CHECK(ltc_stream_encoder_set_user_bits(se3, 0x1234) == -1);
	ltc_stream_encoder_free(se3);

	ltc_stream_encoder_free(se);
	ltc_stream_encoder_free(se2);
}

/*
 * User bits must be parity corrected before the first bit is emitted and
 * must not disturb the TV-standard flag bits.
 */
static void test_user_bits_parity_and_flags(void) {
	struct Config {
		enum LTC_TV_STANDARD standard;
		int flags;
	} configs[] = {
		{ LTC_TV_625_50, LTC_BGF_DONT_TOUCH | LTC_TC_CLOCK },
		{ LTC_TV_525_60, LTC_BGF_DONT_TOUCH },
		{ LTC_TV_FILM_24, LTC_BGF_DONT_TOUCH | LTC_TC_CLOCK },
		{ LTC_TV_625_50, LTC_NO_PARITY },
	};
	const unsigned long bits = 0x13579BDFul;
	const double spc = 48000.0 / (25 * 80.0);

	for (size_t c = 0; c < sizeof(configs) / sizeof(configs[0]); ++c) {
		SMPTETimecode st;
		memset(&st, 0, sizeof(st));
		strcpy(st.timezone, "+0000");
		st.hours = 4;
		st.mins = 5;
		st.secs = 6;
		st.frame = 7;

		LTCFrame expected;
		frame_from_timecode(&expected, &st, configs[c].standard, configs[c].flags, 0);
		set_user_bits_raw(&expected, bits);
		if ((configs[c].flags & LTC_NO_PARITY) == 0) {
			ltc_frame_set_parity(&expected, configs[c].standard);
		}

		LTCStreamEncoder *se = ltc_stream_encoder_create(48000, 25,
				configs[c].standard, configs[c].flags, &st, 1, -3.0, 0.0);
		CHECK_CHECK(se != NULL);
		CHECK_CHECK(ltc_stream_encoder_set_user_bits(se, bits) == 0);

		ltcsnd_sample_t pcm[8192];
		stream_read_chunked(se, pcm, (size_t)(48000.0 / 25), 21);

		decode_and_check_frame("parity", (const unsigned char*)&expected, pcm,
				(size_t)(48000.0 / 25), spc, 0, -3.0);

		/* Reconstruct the decoded frame to assert the flag bits directly. */
		LTCFrame decoded;
		memcpy(&decoded, &expected, sizeof(decoded));
		CHECK_CHECK(ltc_frame_get_user_bits(&decoded) == bits);
		CHECK_CHECK(decoded.binary_group_flag_bit1
				== (((configs[c].flags & LTC_BGF_DONT_TOUCH) && (configs[c].flags & LTC_TC_CLOCK)) ? 1 : 0));

		if (configs[c].flags & LTC_NO_PARITY) {
			/* Untouched: equal to the initial frame's parity bit. */
			LTCFrame initial;
			frame_from_timecode(&initial, &st, configs[c].standard, configs[c].flags, 0);
			if (configs[c].standard == LTC_TV_625_50) {
				CHECK_CHECK(decoded.binary_group_flag_bit2
						== initial.binary_group_flag_bit2);
			} else {
				CHECK_CHECK(decoded.biphase_mark_phase_correction
						== initial.biphase_mark_phase_correction);
			}
		} else {
			/* Correctness of the parity bit: recompute on a copy whose parity
			 * bit was cleared; it must reproduce the decoded bit. */
			LTCFrame copy = decoded;
			if (configs[c].standard == LTC_TV_625_50) {
				copy.binary_group_flag_bit2 = 0;
				ltc_frame_set_parity(&copy, configs[c].standard);
				CHECK_CHECK(copy.binary_group_flag_bit2
						== decoded.binary_group_flag_bit2);
			} else {
				copy.biphase_mark_phase_correction = 0;
				ltc_frame_set_parity(&copy, configs[c].standard);
				CHECK_CHECK(copy.biphase_mark_phase_correction
						== decoded.biphase_mark_phase_correction);
			}
		}

		ltc_stream_encoder_free(se);
	}
}

/*
 * Chunk boundaries in the middle of a filtered edge must not restart the
 * ramp; concatenated output must equal the legacy reference.
 */
static void test_filter_pause_mid_edge(void) {
	const double fps = 25;
	const enum LTC_TV_STANDARD standard = LTC_TV_625_50;
	const double rise = 40.0;

	SMPTETimecode st;
	memset(&st, 0, sizeof(st));
	strcpy(st.timezone, "+0000");
	st.hours = 7;
	st.mins = 8;
	st.secs = 9;
	st.frame = 10;

	size_t ref_len = 0;
	ltcsnd_sample_t *ref = legacy_reference(48000, fps, standard, 0, -3.0, rise,
			&st, 1, 2, NULL, &ref_len);
	CHECK_CHECK(ref_len > 100);

	LTCStreamEncoder *se = ltc_stream_encoder_create(48000, fps, standard, 0,
			&st, 1, -3.0, rise);
	CHECK_CHECK(se != NULL);
	ltcsnd_sample_t *got = (ltcsnd_sample_t*) malloc(ref_len + 8);
	CHECK_CHECK(got != NULL);

	/* The first cell is one bit-cell long; pause in the middle of its
	 * filtered rise/fall.  Several pauses at odd offsets. */
	size_t pos = 0;
	const size_t pauses[] = { 5, 1, 17, 3, 200, 21, 512, 2 };
	size_t pi = 0;
	while (pos < ref_len) {
		size_t n = pauses[pi % (sizeof(pauses) / sizeof(pauses[0]))];
		++pi;
		if (n > ref_len - pos) n = ref_len - pos;
		CHECK_CHECK(ltc_stream_encoder_read(se, got + pos, n) == 0);
		pos += n;
	}
	CHECK_CHECK(memcmp(ref, got, ref_len) == 0);

	/* The filtered waveform must contain intermediate levels, proving the
	 * pauses happened inside ramps rather than on edges. */
	int intermediate = 0;
	for (size_t i = 0; i < 400 && i < ref_len; ++i) {
		if (ref[i] != 38 && ref[i] != 218) {
			++intermediate;
		}
	}
	CHECK_CHECK(intermediate > 0);

	free(ref);
	free(got);
	ltc_stream_encoder_free(se);
}

/*
 * Every way of splitting the same stream must yield the same audio.
 */
static void test_chunking_invariance(void) {
	const double fps = 30000.0 / 1001.0;
	const enum LTC_TV_STANDARD standard = LTC_TV_525_60;

	SMPTETimecode st;
	memset(&st, 0, sizeof(st));
	strcpy(st.timezone, "+0000");
	st.hours = 9;
	st.mins = 10;
	st.secs = 11;
	st.frame = 12;

	const size_t total = (size_t)(48000.0 * 3 / fps) + 4;

	LTCStreamEncoder *a = ltc_stream_encoder_create(48000, fps, standard, 0,
			&st, 1, -3.0, 40.0);
	LTCStreamEncoder *b = ltc_stream_encoder_create(48000, fps, standard, 0,
			&st, 1, -3.0, 40.0);
	CHECK_CHECK(a != NULL && b != NULL);

	ltcsnd_sample_t *one = (ltcsnd_sample_t*) malloc(total);
	ltcsnd_sample_t *mixed = (ltcsnd_sample_t*) malloc(total);
	CHECK_CHECK(one != NULL && mixed != NULL);

	/* One call for everything. */
	CHECK_CHECK(ltc_stream_encoder_read(a, one, total) == 0);

	/* Many small, uneven chunks. */
	stream_read_chunked(b, mixed, total, 424242);

	CHECK_CHECK(memcmp(one, mixed, total) == 0);

	free(one);
	free(mixed);
	ltc_stream_encoder_free(a);
	ltc_stream_encoder_free(b);
}

/*
 * read() must not allocate or free memory, including across frame
 * boundaries and filtered edges.
 */
static void test_read_does_not_allocate(void) {
	const double fps = 25;
	const enum LTC_TV_STANDARD standard = LTC_TV_625_50;

	SMPTETimecode st;
	memset(&st, 0, sizeof(st));
	strcpy(st.timezone, "+0000");
	st.hours = 11;
	st.mins = 12;
	st.secs = 13;
	st.frame = 14;

	LTCStreamEncoder *se = ltc_stream_encoder_create(48000, fps, standard, 0,
			&st, 1, -3.0, 40.0);
	CHECK_CHECK(se != NULL);

	ltcsnd_sample_t buf[4096];
	/* Warm up: exercise frame transitions once outside the measurement. */
	for (int i = 0; i < 3; ++i) {
		CHECK_CHECK(ltc_stream_encoder_read(se, buf, sizeof(buf)) == 0);
	}

#if HAVE_ALLOC_COUNT
	const unsigned long alloc_before = g_alloc_calls;
	const unsigned long free_before = g_free_calls;

	unsigned seed = 987654321u;
	size_t done = 0;
	const size_t total = 48000 * 4; /* several complete frames */
	while (done < total) {
		size_t n = next_chunk(&seed, total - done);
		if (n > sizeof(buf)) n = sizeof(buf);
		CHECK_CHECK(ltc_stream_encoder_read(se, buf, n) == 0);
		done += n;
	}

	CHECK_CHECK(g_alloc_calls == alloc_before);
	CHECK_CHECK(g_free_calls == free_before);

	/* set_user_bits and get_timecode are also allocation free. */
	SMPTETimecode tc;
	const unsigned long a2 = g_alloc_calls;
	const unsigned long f2 = g_free_calls;
	ltc_stream_encoder_get_timecode(se, &tc);
	CHECK_CHECK(ltc_stream_encoder_set_user_bits(se, 0x42) == 0);
	CHECK_CHECK(g_alloc_calls == a2);
	CHECK_CHECK(g_free_calls == f2);
#endif

	ltc_stream_encoder_free(se);
}

/*
 * Reverse playback must not be a sample-reversed forward stream: labels
 * are emitted bytes 9..0 / MSB first and the timecode decrements.  This is
 * covered byte-for-byte above; here we assert the label the independent
 * decoder recovers from reverse PCM at a few frames.
 */
static void test_reverse_semantics(void) {
	const double fps = 25;
	const enum LTC_TV_STANDARD standard = LTC_TV_625_50;
	const double spc = 48000.0 / (fps * 80.0);

	SMPTETimecode st;
	memset(&st, 0, sizeof(st));
	strcpy(st.timezone, "+0000");
	st.hours = 13;
	st.mins = 14;
	st.secs = 15;
	st.frame = 16;

	LTCFrame seq[4];
	build_sequence(fps, standard, 0, &st, -1, 4, seq);

	LTCStreamEncoder *se = ltc_stream_encoder_create(48000, fps, standard, 0,
			&st, -1, -18.0, 0.0);
	CHECK_CHECK(se != NULL);

	ltcsnd_sample_t pcm[3 * 8192];
	SimState sim;
	sim_state_init(&sim);
	for (int i = 0; i < 4; ++i) {
		Cell cells[MAX_CELLS];
		const int nc = simulate_cells((const unsigned char*)&seq[i], spc, 1,
				&sim, cells, MAX_CELLS);
		CHECK_CHECK(nc > 0);
		size_t want = 0;
		for (int z = 0; z < nc; ++z) want += cells[z].len;
		CHECK_CHECK(want <= sizeof(pcm));
		stream_read_chunked(se, pcm, want, (unsigned)(77 + i));
		check_frame_cells("reverse", (const unsigned char*)&seq[i], cells, nc,
				pcm, want, spc, 1, -18.0);
	}

	/* Reverse is not the forward PCM mirrored frame by frame. */
	LTCStreamEncoder *fw = ltc_stream_encoder_create(48000, fps, standard, 0,
			&st, 1, -18.0, 0.0);
	CHECK_CHECK(fw != NULL);
	ltcsnd_sample_t *fwpcm = (ltcsnd_sample_t*) malloc(3 * 8192);
	CHECK_CHECK(fwpcm != NULL);
	stream_read_chunked(fw, fwpcm, 2 * (size_t)(48000.0 / fps) + 8, 78);

	/* A reversed forward frame would start with the forward frame's last
	 * sample; assert the actual reverse stream starts differently. */
	Cell cells[MAX_CELLS];
	SimState fsim;
	sim_state_init(&fsim);
	const int nc = simulate_cells((const unsigned char*)&seq[0], spc, 0,
			&fsim, cells, MAX_CELLS);
	size_t flen = 0;
	for (int i = 0; i < nc; ++i) flen += cells[i].len;
	CHECK_CHECK(pcm[0] != fwpcm[flen - 1]);

	free(fwpcm);
	ltc_stream_encoder_free(fw);
	ltc_stream_encoder_free(se);
}

/*
 * A frame whose user bits were set on the stream encoder (and parity
 * recomputed) must match the legacy encoder fed the same final label.
 */
static void test_user_bits_legacy_waveform(void) {
	struct Config {
		enum LTC_TV_STANDARD standard;
		int flags;
		double volume;
		double rise;
		int direction;
	} configs[] = {
		{ LTC_TV_625_50, 0,                          -3.0,  40.0, 1 },
		{ LTC_TV_625_50, 0,                          -18.0, 0.0, -1 },
		{ LTC_TV_525_60, 0,                          -1.0,  25.0, 1 },
		{ LTC_TV_FILM_24, LTC_BGF_DONT_TOUCH,        -3.0,  40.0, -1 },
		{ LTC_TV_625_50, LTC_BGF_DONT_TOUCH,         -3.0,  40.0, 1 },
		{ LTC_TV_625_50, LTC_NO_PARITY,              -3.0,  40.0, 1 },
	};
	const unsigned long bits = 0xCAFEBABEul;
	const int nframes = 2;

	for (size_t c = 0; c < sizeof(configs) / sizeof(configs[0]); ++c) {
		SMPTETimecode st;
		memset(&st, 0, sizeof(st));
		strcpy(st.timezone, "+0000");
		st.years = 3;
		st.months = 4;
		st.days = 5;
		st.hours = 6;
		st.mins = 7;
		st.secs = 8;
		st.frame = 9;

		LTCFrame seq[2];
		build_sequence(25, configs[c].standard, configs[c].flags, &st,
				configs[c].direction, nframes, seq);
		for (int i = 0; i < nframes; ++i) {
			set_user_bits_raw(&seq[i], bits + (unsigned long) i);
			if ((configs[c].flags & LTC_NO_PARITY) == 0) {
				ltc_frame_set_parity(&seq[i], configs[c].standard);
			}
		}

		/* Reference: legacy encoder driven with the final frames. */
		LTCEncoder *le = ltc_encoder_create(48000, 25, configs[c].standard,
				configs[c].flags);
		CHECK_CHECK(le != NULL);
		ltc_encoder_set_filter(le, configs[c].rise);
		CHECK_CHECK(ltc_encoder_set_volume(le, configs[c].volume) == 0);
		const size_t cap = ltc_encoder_get_buffersize(le);
		ltcsnd_sample_t *ref = (ltcsnd_sample_t*) malloc(cap * nframes + 64);
		CHECK_CHECK(ref != NULL);
		size_t ref_len = 0;
		for (int i = 0; i < nframes; ++i) {
			ltc_encoder_set_frame(le, &seq[i]);
			if (configs[c].direction > 0) {
				ltc_encoder_encode_frame(le);
			} else {
				ltc_encoder_encode_reversed_frame(le);
			}
			ref_len += (size_t) ltc_encoder_copy_buffer(le, ref + ref_len);
		}
		ltc_encoder_free(le);

		/* Stream encoder: set the first frame's user bits before output,
		 * and queue the second frame's bits mid-frame. */
		LTCStreamEncoder *se = ltc_stream_encoder_create(48000, 25,
				configs[c].standard, configs[c].flags, &st, configs[c].direction,
				configs[c].volume, configs[c].rise);
		CHECK_CHECK(se != NULL);
		CHECK_CHECK(ltc_stream_encoder_set_user_bits(se, bits) == 0);

		ltcsnd_sample_t *got = (ltcsnd_sample_t*) malloc(ref_len + 64);
		CHECK_CHECK(got != NULL);

		const double spc = 48000.0 / (25 * 80.0);
		SimState sim;
		sim_state_init(&sim);
		size_t pos = 0;
		for (int i = 0; i < nframes; ++i) {
			Cell cells[MAX_CELLS];
			const int nc = simulate_cells((const unsigned char*)&seq[i], spc,
					configs[c].direction < 0, &sim, cells, MAX_CELLS);
			size_t flen = 0;
			for (int z = 0; z < nc; ++z) flen += cells[z].len;

			if (i + 1 < nframes) {
				/* Halfway through this frame, queue the next frame's user
				 * bits.  They must not disturb the frame being emitted. */
				ltc_stream_encoder_read(se, got + pos, flen / 2);
				pos += flen / 2;
				CHECK_CHECK(ltc_stream_encoder_set_user_bits(se,
						bits + (unsigned long)(i + 1)) == 0);
				CHECK_CHECK(ltc_stream_encoder_read(se, got + pos,
						flen - flen / 2) == 0);
				pos += flen - flen / 2;
			} else {
				CHECK_CHECK(ltc_stream_encoder_read(se, got + pos, flen) == 0);
				pos += flen;
			}
		}
		CHECK_CHECK(pos == ref_len);
		if (memcmp(ref, got, ref_len) != 0) {
			size_t i = 0;
			while (i < ref_len && ref[i] == got[i]) ++i;
			fprintf(stderr, "user-bits legacy mismatch std=%d flags=%d dir=%d at %zu\n",
					configs[c].standard, configs[c].flags, configs[c].direction, i);
			CHECK_CHECK(0);
		}

		free(ref);
		free(got);
		ltc_stream_encoder_free(se);
	}
}

/*
 * A one-sample-per-call callback (and 1/257/1024 chunking) across several
 * frames must be byte identical to the legacy encoder, and zero-length
 * calls interleaved between them must not perturb the stream.
 */
static void test_single_sample_callbacks(void) {
	struct Config {
		double sample_rate;
		double fps;
		enum LTC_TV_STANDARD standard;
		int flags;
		int direction;
	} configs[] = {
		{ 48000, 25,               LTC_TV_625_50, 0,             1 },
		{ 48000, 30000.0 / 1001.0, LTC_TV_525_60, 0,             1 },
		{ 48000, 24,               LTC_TV_FILM_24, 0,            -1 },
		{ 32000, 30,               LTC_TV_525_60, 0,            -1 },
		{ 48000, 25,               LTC_TV_625_50, LTC_USE_DATE,  1 },
	};

	for (size_t c = 0; c < sizeof(configs) / sizeof(configs[0]); ++c) {
		SMPTETimecode st;
		memset(&st, 0, sizeof(st));
		strcpy(st.timezone, "+0100");
		st.years = 24;
		st.months = 6;
		st.days = 30;
		st.hours = 1;
		st.mins = 2;
		st.secs = 3;
		st.frame = 4;

		size_t ref_len = 0;
		ltcsnd_sample_t *ref = legacy_reference(configs[c].sample_rate,
				configs[c].fps, configs[c].standard, configs[c].flags, -3.0, 40.0,
				&st, configs[c].direction, 3, NULL, &ref_len);
		CHECK_CHECK(ref_len > 1000);

		LTCStreamEncoder *se = ltc_stream_encoder_create(configs[c].sample_rate,
				configs[c].fps, configs[c].standard, configs[c].flags, &st,
				configs[c].direction, -3.0, 40.0);
		CHECK_CHECK(se != NULL);

		ltcsnd_sample_t *got = (ltcsnd_sample_t*) malloc(ref_len);
		CHECK_CHECK(got != NULL);

		/* Half the stream one sample at a time, the rest in the named
		 * callback sizes, with zero-length calls sprinkled in. */
		size_t pos = 0;
		const size_t one_at_a_time = ref_len / 2;
		while (pos < one_at_a_time) {
			CHECK_CHECK(ltc_stream_encoder_read(se, got + pos, 1) == 0);
			++pos;
			if ((pos % 97) == 0) {
				CHECK_CHECK(ltc_stream_encoder_read(se, NULL, 0) == 0);
			}
		}
		static const size_t chunks[] = { 257, 1024, 1, 4096 };
		size_t ci = 0;
		while (pos < ref_len) {
			size_t n = chunks[ci % 4];
			++ci;
			if (n > ref_len - pos) n = ref_len - pos;
			CHECK_CHECK(ltc_stream_encoder_read(se, got + pos, n) == 0);
			pos += n;
		}

		CHECK_CHECK(memcmp(ref, got, ref_len) == 0);

		free(ref);
		free(got);
		ltc_stream_encoder_free(se);
	}
}

/* -+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * Entry point
 */

void run_stream_encoder_tests(void) {
	test_parameter_validation();
	test_byte_identical_to_legacy();
	test_bmc_edges_and_labels();
	test_timecode_boundary_semantics();
	test_drop_frame_minute_boundary();
	test_midnight_date_crossing();
	test_user_bits_mid_frame();
	test_user_bits_parity_and_flags();
	test_user_bits_legacy_waveform();
	test_filter_pause_mid_edge();
	test_chunking_invariance();
	test_single_sample_callbacks();
	test_read_does_not_allocate();
	test_reverse_semantics();
}
