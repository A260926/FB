/* Contract tests for the QOA beam search encoder.
 *
 * This file defines QOA_IMPLEMENTATION itself and installs a controllable
 * allocator before including qoa.h, so allocation failures can be injected
 * deterministically. The fixed driver checks/search_contract_main.c only calls
 * the six named entry points below. The reference oracle here is an
 * independent fixed-scale simulator; production beam/selection internals are
 * never called from the reference side. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* --- Controllable allocator ---------------------------------------------- */

static int g_alloc_calls;      /* total QOA_MALLOC calls */
static int g_fail_on;          /* 1-based call index that fails, 0 = never */
static int g_live;             /* currently outstanding allocations */

#define QOA_REGISTRY_CAP 64
static void *g_live_ptrs[QOA_REGISTRY_CAP];

static void *test_malloc(size_t size) {
	g_alloc_calls++;
	if (g_fail_on && g_alloc_calls == g_fail_on) {
		return NULL;
	}
	void *p = malloc(size);
	if (!p) {
		return NULL;
	}
	if (g_live >= QOA_REGISTRY_CAP) {
		free(p);
		return NULL;
	}
	g_live_ptrs[g_live++] = p;
	return p;
}

static void test_free(void *p) {
	if (!p) {
		return;
	}
	for (int i = 0; i < g_live; i++) {
		if (g_live_ptrs[i] == p) {
			g_live_ptrs[i] = g_live_ptrs[--g_live];
			g_live_ptrs[g_live] = NULL;
			free(p);
			return;
		}
	}
	fprintf(stderr, "QOA_TEST_FREE_ERROR: freeing unknown pointer %p\n", p);
	abort();
}

#define QOA_MALLOC(sz) test_malloc(sz)
#define QOA_FREE(p)    test_free(p)

#define QOA_IMPLEMENTATION
#define QOA_NO_STDIO
#include "qoa.h"
#include "search_contract.h"
#include "search_witness.h"

/* --- Small shared helpers ------------------------------------------------ */

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "QOA_TEST_ASSERT_FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		return 1; \
	} \
} while (0)

static unsigned g_rng_state;
static unsigned test_lcg(void) {
	g_rng_state = g_rng_state * 1664525u + 1013904223u;
	return g_rng_state;
}

static uint64_t sse16(const short *a, const short *b, unsigned n) {
	uint64_t s = 0;
	for (unsigned i = 0; i < n; i++) {
		int64_t d = (int)a[i] - (int)b[i];
		s += (uint64_t)(d * d);
	}
	return s;
}

static void put64(unsigned char *p, uint64_t v) {
	for (int i = 0; i < 8; i++) {
		p[i] = (unsigned char)(v >> (56 - 8 * i));
	}
}

static int16_t read_s16(const unsigned char *p) {
	return (int16_t)((p[0] << 8) | p[1]);
}

static int clamp_s16(int v) {
	return v < -32768 ? -32768 : (v > 32767 ? 32767 : v);
}

/* Independent fixed-scale, two-slice (40 sample) oracle. It keeps its own
 * int64 LMS and reads only qoa's static data tables, never the encoder's
 * candidate/selection code. Emits the two 64 bit slice words. */
typedef struct {
	int64_t history[4];
	int64_t weights[4];
} oracle_lms_t;

static int oracle_clamp_s16(int64_t v) {
	return v < -32768 ? -32768 : (v > 32767 ? 32767 : (int)v);
}

static int oracle_div(int64_t v, int scalefactor) {
	int64_t n = (v * qoa_reciprocal_tab[scalefactor] + (1LL << 15)) >> 16;
	n += ((v > 0) - (v < 0)) - ((n > 0) - (n < 0));
	if (n < -8) { n = -8; }
	if (n > 8) { n = 8; }
	return qoa_quant_tab[(int)n + 8];
}

static uint64_t oracle_simulate(const short *pcm, int s0, int s1, uint64_t words[2]) {
	oracle_lms_t l = {
		{0, 0, 0, 0},
		{0, 0, -(1 << 13), (1 << 14)}
	};
	uint64_t sse = 0;
	for (int sl = 0; sl < 2; sl++) {
		int sf = (sl == 0) ? s0 : s1;
		uint64_t word = (uint64_t)sf;
		for (int k = 0; k < 20; k++) {
			int idx = sl * 20 + k;
			int64_t pred = (l.weights[0] * l.history[0] + l.weights[1] * l.history[1] +
				l.weights[2] * l.history[2] + l.weights[3] * l.history[3]) >> 13;
			int64_t residual = (int64_t)pcm[idx] - pred;
			int q = oracle_div(residual, sf);
			int dq = qoa_dequant_tab[sf][q];
			int64_t reconstructed = oracle_clamp_s16(pred + dq);
			int64_t e = (int64_t)pcm[idx] - reconstructed;
			sse += (uint64_t)(e * e);
			int64_t delta = dq >> 4;
			for (int i = 0; i < 4; i++) {
				l.weights[i] += l.history[i] < 0 ? -delta : delta;
			}
			for (int i = 0; i < 3; i++) {
				l.history[i] = l.history[i + 1];
			}
			l.history[3] = reconstructed;
			word = (word << 3) | (uint64_t)q;
		}
		words[sl] = word;
	}
	return sse;
}

/* Build a decodable 40-byte mono frame (header + default LMS + two slices). */
static void build_frame40(unsigned char out[40], uint64_t w0, uint64_t w1) {
	put64(out, ((uint64_t)1 << 56) | ((uint64_t)32000 << 32) | ((uint64_t)40 << 16) | 40u);
	put64(out + 8, 0);
	uint64_t weights_word = 0;
	int default_weights[4] = {0, 0, -(1 << 13), (1 << 14)};
	for (int i = 0; i < 4; i++) {
		weights_word = (weights_word << 16) | (default_weights[i] & 0xffff);
	}
	put64(out + 16, weights_word);
	put64(out + 24, w0);
	put64(out + 32, w1);
}

/* Decode a whole beam-encoded file two ways (qoa_decode and a frame walk) and
 * verify full PCM, byte consumption, frame lengths and final LMS. */
static int verify_whole_file(const unsigned char *data, unsigned len,
	unsigned channels, unsigned samplerate, unsigned samples,
	const qoa_desc *encoder_desc, const char *tag)
{
	qoa_desc full = {0};
	short *full_pcm = qoa_decode(data, (int)len, &full);
	CHECK(full_pcm != NULL);
	CHECK(full.channels == channels);
	CHECK(full.samplerate == samplerate);
	CHECK(full.samples == samples);

	qoa_desc walked = {0};
	walked.channels = channels;
	walked.samplerate = samplerate;
	unsigned cap = ((samples + QOA_FRAME_LEN - 1) / QOA_FRAME_LEN) *
		QOA_FRAME_LEN * channels;
	short *walk_pcm = calloc(cap, sizeof(short));
	CHECK(walk_pcm != NULL);

	unsigned pos = 8, got = 0, frame_index = 0;
	while (got < samples) {
		unsigned frame_len = 0;
		unsigned used = qoa_decode_frame(data + pos, len - pos, &walked,
			walk_pcm + got * channels, &frame_len);
		if (!used || !frame_len) {
			fprintf(stderr, "%s: frame %u failed to decode\n", tag, frame_index);
			QOA_FREE(full_pcm); free(walk_pcm);
			return 1;
		}
		if (frame_len != ((got + QOA_FRAME_LEN <= samples) ? QOA_FRAME_LEN : samples - got)) {
			fprintf(stderr, "%s: frame %u bad length %u\n", tag, frame_index, frame_len);
			QOA_FREE(full_pcm); free(walk_pcm);
			return 1;
		}
		pos += used;
		got += frame_len;
		frame_index++;
	}
	if (pos != len) {
		fprintf(stderr, "%s: consumed %u of %u bytes\n", tag, pos, len);
		QOA_FREE(full_pcm); free(walk_pcm);
		return 1;
	}
	for (unsigned i = 0; i < (unsigned)samples * channels; i++) {
		if (full_pcm[i] != walk_pcm[i]) {
			fprintf(stderr, "%s: first PCM diff at frame/channel index %u (ch=%u)\n",
				tag, i / channels, i % channels);
			QOA_FREE(full_pcm); free(walk_pcm);
			return 1;
		}
	}
	if (memcmp(full.lms, walked.lms, sizeof(qoa_lms_t)) != 0 ||
		memcmp(full.lms, encoder_desc->lms, sizeof(qoa_lms_t)) != 0) {
		fprintf(stderr, "%s: final LMS mismatch\n", tag);
		QOA_FREE(full_pcm); free(walk_pcm);
		return 1;
	}
	QOA_FREE(full_pcm);
	free(walk_pcm);
	return 0;
}

/* --- 1. fixture_improves ------------------------------------------------- */

int search_fixture_improves(void) {
	qoa_desc enc = {0}, dec = {0};
	enc.channels = 1;
	enc.samplerate = 32000;
	enc.samples = 40;

	unsigned beam_len = 0;
	unsigned char *beam = qoa_encode_beam(witness_pcm, &enc, &beam_len, 16);
	CHECK(beam != NULL);
	CHECK(beam_len == 48);

	short *beam_pcm = qoa_decode(beam, beam_len, &dec);
	CHECK(beam_pcm != NULL);
	CHECK(dec.samples == 40 && dec.channels == 1);
	uint64_t beam_sse = sse16(witness_pcm, beam_pcm, 40);

	/* The witness's feasible (13,15) path is the bar to reach. */
	CHECK(beam_sse <= 327723244u);

	qoa_desc fixed = {0};
	fixed.channels = 1;
	fixed.samplerate = 32000;
	short alt[40];
	unsigned alt_len = 0;
	CHECK(qoa_decode_frame(witness_frame, sizeof(witness_frame), &fixed, alt, &alt_len) == 40);
	CHECK(alt_len == 40);
	CHECK(sse16(witness_pcm, alt, 40) == 327723244u);

	/* The legacy greedy path is the documented upper bound. */
	qoa_desc legacy = {0}, ldec = {0};
	legacy.channels = 1;
	legacy.samplerate = 32000;
	legacy.samples = 40;
	unsigned legacy_len = 0;
	unsigned char *legacy_bytes = qoa_encode(witness_pcm, &legacy, &legacy_len);
	CHECK(legacy_bytes != NULL);
	short *legacy_pcm = qoa_decode(legacy_bytes, legacy_len, &ldec);
	CHECK(legacy_pcm != NULL);
	uint64_t greedy_sse = sse16(witness_pcm, legacy_pcm, 40);
	CHECK(greedy_sse == 360667757u);
	CHECK(beam_sse <= greedy_sse);

	/* Width 1 must not beat the greedy control (it runs the same strategy). */
	qoa_desc w1 = {0}, w1dec = {0};
	w1.channels = 1;
	w1.samplerate = 32000;
	w1.samples = 40;
	unsigned w1_len = 0;
	unsigned char *w1_bytes = qoa_encode_beam(witness_pcm, &w1, &w1_len, 1);
	CHECK(w1_bytes != NULL);
	short *w1_pcm = qoa_decode(w1_bytes, w1_len, &w1dec);
	CHECK(w1_pcm != NULL);
	CHECK(sse16(witness_pcm, w1_pcm, 40) <= greedy_sse);

	QOA_FREE(beam);
	QOA_FREE(beam_pcm);
	QOA_FREE(legacy_bytes);
	QOA_FREE(legacy_pcm);
	QOA_FREE(w1_bytes);
	QOA_FREE(w1_pcm);
	CHECK(g_live == 0);
	return 0;
}

/* --- 2. two_slice_oracle ------------------------------------------------- */

int search_two_slice_oracle(void) {
	/* Fixture A: 2-way tie (0,0)/(0,1) at the optimum; lexicographic (0,0). */
	short tie_pcm[40];
	g_rng_state = 10647;
	for (int i = 0; i < 40; i++) {
		tie_pcm[i] = (short)((int)(test_lcg() % 9) - 4);
	}

	/* Fixture B: 2-way tie (0,0)/(1,0); greedy takes (1,0) and must win the
	 * equal-SSE final comparison over the lexicographic (0,0). */
	short greedy_tie_pcm[40];
	g_rng_state = 55412;
	for (int i = 0; i < 40; i++) {
		greedy_tie_pcm[i] = (short)((int)(test_lcg() % 9) - 4);
	}

	const short *fixtures[2] = { tie_pcm, greedy_tie_pcm };
	const int expect_lex[2][2] = {{0, 0}, {0, 0}};
	const int expect_final[2][2] = {{0, 0}, {1, 0}};
	const uint64_t expect_sse[2] = {252, 296};

	for (int fx = 0; fx < 2; fx++) {
		const short *pcm = fixtures[fx];

		/* Exhaust all 256 fixed scalefactor pairs with the independent
		 * simulator, and cross-check every SSE through the real decoder. */
		uint64_t oracle_min = UINT64_MAX;
		int tie_count = 0;
		int lex_s0 = -1, lex_s1 = -1;
		for (int a = 0; a < 16; a++) {
			for (int b = 0; b < 16; b++) {
				uint64_t words[2];
				uint64_t sim_sse = oracle_simulate(pcm, a, b, words);

				unsigned char frame[40];
				build_frame40(frame, words[0], words[1]);
				qoa_desc d = {0};
				d.channels = 1;
				d.samplerate = 32000;
				short decoded[40];
				unsigned fl = 0;
				CHECK(qoa_decode_frame(frame, sizeof frame, &d, decoded, &fl) == 40);
				CHECK(fl == 40);
				CHECK(sse16(pcm, decoded, 40) == sim_sse);

				if (sim_sse < oracle_min) {
					oracle_min = sim_sse;
					tie_count = 1;
					lex_s0 = a;
					lex_s1 = b;
				}
				else if (sim_sse == oracle_min) {
					tie_count++;
				}
			}
		}
		CHECK(tie_count >= 2);
		CHECK(oracle_min == expect_sse[fx]);
		CHECK(lex_s0 == expect_lex[fx][0] && lex_s1 == expect_lex[fx][1]);

		/* Width 16 must reach the oracle optimum and honor the final choice. */
		qoa_desc enc = {0}, dec = {0};
		enc.channels = 1;
		enc.samplerate = 32000;
		enc.samples = 40;
		unsigned len = 0;
		unsigned char *out = qoa_encode_beam(pcm, &enc, &len, 16);
		CHECK(out != NULL && len == 48);
		CHECK((out[32] >> 4) == (unsigned)expect_final[fx][0]);
		CHECK((out[40] >> 4) == (unsigned)expect_final[fx][1]);
		short *dec_pcm = qoa_decode(out, len, &dec);
		CHECK(dec_pcm != NULL);
		CHECK(sse16(pcm, dec_pcm, 40) == oracle_min);

		/* Every width from the same start stays at or above the optimum and
		 * never exceeds the greedy control. */
		qoa_desc g_enc = {0}, g_dec = {0};
		g_enc.channels = 1;
		g_enc.samplerate = 32000;
		g_enc.samples = 40;
		unsigned g_len = 0;
		unsigned char *g_out = qoa_encode(pcm, &g_enc, &g_len);
		CHECK(g_out != NULL);
		short *g_pcm = qoa_decode(g_out, g_len, &g_dec);
		CHECK(g_pcm != NULL);
		uint64_t greedy_sse = sse16(pcm, g_pcm, 40);
		CHECK(greedy_sse >= oracle_min);

		for (unsigned w = 1; w <= 16; w++) {
			qoa_desc f = {0}, fd = {0};
			f.channels = 1;
			f.samplerate = 32000;
			f.lms[0].weights[2] = -(1 << 13);
			f.lms[0].weights[3] = (1 << 14);
			unsigned char frame[64];
			unsigned n = qoa_encode_frame_beam(pcm, &f, 40, frame, sizeof frame, w);
			CHECK(n == 40);
			fd.channels = 1;
			fd.samplerate = 32000;
			short fp[40];
			unsigned fl = 0;
			CHECK(qoa_decode_frame(frame, n, &fd, fp, &fl) == 40);
			uint64_t got = sse16(pcm, fp, 40);
			CHECK(got >= oracle_min);
			CHECK(got <= greedy_sse);
		}

		QOA_FREE(out);
		QOA_FREE(dec_pcm);
		QOA_FREE(g_out);
		QOA_FREE(g_pcm);
	}
	CHECK(g_live == 0);
	return 0;
}

/* --- 3. frame_snapshots -------------------------------------------------- */

int search_frame_snapshots(void) {
	short pcm[20];
	g_rng_state = 777;
	for (int i = 0; i < 20; i++) {
		pcm[i] = (short)((int)(test_lcg() % 40001) - 20000);
	}

	/* Caller state outside the signed 16 bit range. */
	int in_history[4] = {40000, -40000, 40000, -40000};
	int in_weights[4] = {40000, 100, -200, 300};

	qoa_desc q = {0};
	q.channels = 1;
	q.samplerate = 44100;
	memcpy(q.lms[0].history, in_history, sizeof(in_history));
	memcpy(q.lms[0].weights, in_weights, sizeof(in_weights));
	qoa_desc before = q;

	unsigned char frame[64];
	memset(frame, 0xA5, sizeof frame);
	unsigned n = qoa_encode_frame_beam(pcm, &q, 20, frame, sizeof frame, 16);
	CHECK(n == QOA_FRAME_SIZE(1, 1));

	/* The frame header must carry the clamped, actually-used state. */
	for (int i = 0; i < 4; i++) {
		CHECK(read_s16(frame + 8 + 2 * i) == clamp_s16(in_history[i]));
		CHECK(read_s16(frame + 16 + 2 * i) == clamp_s16(in_weights[i]));
	}

	/* Subsequent PCM is produced from that clamped state. */
	qoa_desc d = {0};
	d.channels = 1;
	d.samplerate = 44100;
	short decoded[20];
	unsigned fl = 0;
	CHECK(qoa_decode_frame(frame, n, &d, decoded, &fl) == n);
	CHECK(fl == 20);
	static const short expect_pcm[20] = {
		32767, -32768, 32767, -32768, 32767, -32768, 28313, -17555,
		17516, -20667, 5582, -9856, -3672, -29135, -11744, -7001,
		-23519, -32768, -12977, 6488
	};
	for (int i = 0; i < 20; i++) {
		if (decoded[i] != expect_pcm[i]) {
			fprintf(stderr, "frame_snapshots: PCM diff at sample %d\n", i);
			return 1;
		}
	}

	/* The encoder's committed end state matches the decoder's end state. */
	static const int expect_history[4] = {-23519, -32768, -12977, 6488};
	static const int expect_weights[4] = {20639, 7684, -14120, 6092};
	for (int i = 0; i < 4; i++) {
		CHECK(q.lms[0].history[i] == expect_history[i]);
		CHECK(q.lms[0].weights[i] == expect_weights[i]);
		CHECK(d.lms[0].history[i] == expect_history[i]);
		CHECK(d.lms[0].weights[i] == expect_weights[i]);
	}

	/* Fully extreme state (INT32_MIN/MAX weights) has no int32-safe path:
	 * the call fails cleanly and leaves everything untouched. Under
	 * -fsanitize=undefined this also proves no signed overflow occurred. */
	qoa_desc extreme = {0};
	extreme.channels = 1;
	extreme.samplerate = 44100;
	extreme.lms[0].history[0] = 40000;
	extreme.lms[0].history[1] = -40000;
	extreme.lms[0].history[2] = 32768;
	extreme.lms[0].history[3] = -32769;
	extreme.lms[0].weights[0] = 70000;
	extreme.lms[0].weights[1] = -70000;
	extreme.lms[0].weights[2] = INT32_MAX;
	extreme.lms[0].weights[3] = INT32_MIN;
	qoa_desc extreme_snap = extreme;
	unsigned char sentinel[64];
	memset(sentinel, 0xA5, sizeof sentinel);
	CHECK(qoa_encode_frame_beam(pcm, &extreme, 20, sentinel, sizeof sentinel, 16) == 0);
	CHECK(memcmp(&extreme, &extreme_snap, sizeof extreme) == 0);
	for (unsigned i = 0; i < sizeof sentinel; i++) {
		CHECK(sentinel[i] == 0xA5);
	}

	/* Whole-file entry over two frames. It resets to the default LMS like the
	 * legacy encoder, then each later frame starts from the previous frame's
	 * actually committed (16 bit expressible) state. */
	enum { MULTI = QOA_FRAME_LEN + 20 };
	short *multi = malloc((size_t)MULTI * sizeof(short));
	CHECK(multi != NULL);
	g_rng_state = 42;
	for (int i = 0; i < MULTI; i++) {
		multi[i] = (short)((int)(test_lcg() % 40001) - 20000);
	}
	qoa_desc wf = {0};
	wf.channels = 1;
	wf.samplerate = 44100;
	wf.samples = MULTI;
	memcpy(wf.lms, before.lms, sizeof(qoa_lms_t)); /* must be reset internally */
	unsigned wf_len = 0;
	unsigned char *wf_data = qoa_encode_beam(multi, &wf, &wf_len, 16);
	CHECK(wf_data != NULL);
	CHECK(wf_len == 8 + QOA_FRAME_SIZE(1, 256) + QOA_FRAME_SIZE(1, 1));

	/* Frame one header carries the default start state, not the input state. */
	CHECK(read_s16(wf_data + 8 + 8 + 0) == 0);                /* history */
	CHECK(read_s16(wf_data + 8 + 16 + 4) == -(1 << 13));      /* weight 2 */
	CHECK(read_s16(wf_data + 8 + 16 + 6) == (1 << 14));       /* weight 3 */

	/* Frame two header equals frame one's committed end state. */
	unsigned frame2 = 8 + QOA_FRAME_SIZE(1, 256);
	qoa_desc head = {0};
	head.channels = 1;
	head.samplerate = 44100;
	short scratch[QOA_FRAME_LEN];
	unsigned head_len = 0;
	CHECK(qoa_decode_frame(wf_data + 8, wf_len - 8, &head, scratch, &head_len) == QOA_FRAME_SIZE(1, 256));
	CHECK(head_len == QOA_FRAME_LEN);
	for (int i = 0; i < 4; i++) {
		CHECK(read_s16(wf_data + frame2 + 8 + 2 * i) == head.lms[0].history[i]);
		CHECK(read_s16(wf_data + frame2 + 16 + 2 * i) == head.lms[0].weights[i]);
		CHECK(head.lms[0].history[i] >= -32768 && head.lms[0].history[i] <= 32767);
		CHECK(head.lms[0].weights[i] >= -32768 && head.lms[0].weights[i] <= 32767);
	}

	CHECK(verify_whole_file(wf_data, wf_len, 1, 44100, MULTI, &wf, "frame_snapshots") == 0);

	QOA_FREE(wf_data);
	free(multi);
	CHECK(g_live == 0);
	return 0;
}

/* --- 4. channels_tail ---------------------------------------------------- */

int search_channels_tail(void) {
	const unsigned channels_list[3] = {1, 2, 8};
	const unsigned tails[3] = {1, 19, 20};

	for (unsigned ci = 0; ci < 3; ci++) {
		unsigned channels = channels_list[ci];
		for (unsigned ti = 0; ti < 3; ti++) {
			unsigned tail = tails[ti];
			unsigned frame_len = QOA_SLICE_LEN + tail; /* 21, 39 or 40 */
			unsigned slices = (frame_len + QOA_SLICE_LEN - 1) / QOA_SLICE_LEN;
			unsigned total = frame_len * channels;

			short *pcm = malloc((size_t)total * sizeof(short));
			CHECK(pcm != NULL);
			g_rng_state = 1000u + channels * 31u + tail;
			for (unsigned i = 0; i < total; i++) {
				pcm[i] = (short)((int)(test_lcg() % 30001) - 15000);
			}

			qoa_desc q = {0};
			q.channels = channels;
			q.samplerate = 48000;
			for (unsigned c = 0; c < channels; c++) {
				q.lms[c].weights[2] = -(1 << 13);
				q.lms[c].weights[3] = (1 << 14);
			}

			unsigned capacity = QOA_FRAME_SIZE(channels, slices);
			unsigned char *frame = malloc(capacity);
			CHECK(frame != NULL);
			unsigned n = qoa_encode_frame_beam(pcm, &q, frame_len, frame, capacity, 8);
			CHECK(n == capacity);

			qoa_desc d = {0};
			d.channels = channels;
			d.samplerate = 48000;
			short *out = calloc(total, sizeof(short));
			CHECK(out != NULL);
			unsigned got_len = 0;
			CHECK(qoa_decode_frame(frame, n, &d, out, &got_len) == n);
			CHECK(got_len == frame_len);

			/* The final slice of each channel has its unused low bits zero. */
			unsigned empty_bits = (QOA_SLICE_LEN - tail) * 3;
			uint64_t low_mask = empty_bits ? ((1ULL << empty_bits) - 1) : 0;
			for (unsigned c = 0; c < channels; c++) {
				unsigned last_slice_index = (slices - 1) * channels + c;
				const unsigned char *sp = frame + 8 + QOA_LMS_LEN * 4 * channels +
					last_slice_index * 8;
				uint64_t word = 0;
				for (int k = 0; k < 8; k++) {
					word = (word << 8) | sp[k];
				}
				CHECK((word & low_mask) == 0);
			}

			/* Re-encoding from the decoder's end state is deterministic and
			 * keeps the per-channel committed LMS consistent. */
			qoa_desc q2 = {0};
			q2.channels = channels;
			q2.samplerate = 48000;
			memcpy(q2.lms, d.lms, sizeof(qoa_lms_t));
			unsigned char *frame2 = malloc(capacity);
			CHECK(frame2 != NULL);
			CHECK(qoa_encode_frame_beam(pcm, &q2, frame_len, frame2, capacity, 8) == capacity);

			free(frame);
			free(frame2);
			free(pcm);
			free(out);
		}
	}
	CHECK(g_live == 0);
	return 0;
}

/* --- 5. legacy_repeat ---------------------------------------------------- */

int search_legacy_repeat(void) {
	static const unsigned char golden[48] = {
		113, 111, 97, 102, 0, 0, 0, 40,
		1, 0, 125, 0, 0, 40, 0, 40,
		0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 224, 0, 64, 0,
		226, 243, 37, 157, 75, 212, 163, 47,
		253, 81, 23, 113, 169, 223, 23, 44
	};

	/* Legacy byte compatibility: the old entry point must not change output. */
	qoa_desc legacy = {0};
	legacy.channels = 1;
	legacy.samplerate = 32000;
	legacy.samples = 40;
	unsigned legacy_len = 0;
	unsigned char *legacy_bytes = qoa_encode(witness_pcm, &legacy, &legacy_len);
	CHECK(legacy_bytes != NULL && legacy_len == 48);
	CHECK(memcmp(legacy_bytes, golden, sizeof golden) == 0);

	/* Repeated legacy and beam encodes are deterministic. */
	for (int rep = 0; rep < 3; rep++) {
		qoa_desc a = {0};
		a.channels = 1;
		a.samplerate = 32000;
		a.samples = 40;
		unsigned la = 0;
		unsigned char *ba = qoa_encode(witness_pcm, &a, &la);
		CHECK(ba != NULL && la == 48);
		CHECK(memcmp(ba, golden, 48) == 0);

		qoa_desc b = {0};
		b.channels = 1;
		b.samplerate = 32000;
		b.samples = 40;
		unsigned lb = 0;
		unsigned char *bb = qoa_encode_beam(witness_pcm, &b, &lb, 16);
		CHECK(bb != NULL && lb == 48);

		qoa_desc b2 = {0};
		b2.channels = 1;
		b2.samplerate = 32000;
		b2.samples = 40;
		unsigned lb2 = 0;
		unsigned char *bb2 = qoa_encode_beam(witness_pcm, &b2, &lb2, 16);
		CHECK(bb2 != NULL && lb2 == 48);
		CHECK(memcmp(bb, bb2, 48) == 0);
		CHECK(memcmp(b.lms, b2.lms, sizeof(qoa_lms_t)) == 0);

		QOA_FREE(ba);
		QOA_FREE(bb);
		QOA_FREE(bb2);
	}
	QOA_FREE(legacy_bytes);

	/* Same-start greedy upper bound, exercised at the frame level across
	 * widths and frame lengths including a full 5120-sample frame. */
	const unsigned frame_lens[3] = {20, 39, QOA_FRAME_LEN};
	for (unsigned fi = 0; fi < 3; fi++) {
		unsigned frame_len = frame_lens[fi];
		short *pcm = malloc((size_t)frame_len * sizeof(short));
		CHECK(pcm != NULL);
		g_rng_state = 9000u + fi;
		for (unsigned i = 0; i < frame_len; i++) {
			pcm[i] = (short)((int)(test_lcg() % 50001) - 25000);
		}

		qoa_desc gq = {0};
		gq.channels = 1;
		gq.samplerate = 48000;
		gq.lms[0].weights[2] = -(1 << 13);
		gq.lms[0].weights[3] = (1 << 14);
		unsigned g_slices = (frame_len + QOA_SLICE_LEN - 1) / QOA_SLICE_LEN;
		unsigned g_cap = QOA_FRAME_SIZE(1, g_slices);
		unsigned char *g_frame = malloc(g_cap);
		CHECK(g_frame != NULL);
		CHECK(qoa_encode_frame(pcm, &gq, frame_len, g_frame) == g_cap);
		qoa_desc gd = {0};
		gd.channels = 1;
		gd.samplerate = 48000;
		short *g_pcm = calloc(frame_len, sizeof(short));
		CHECK(g_pcm != NULL);
		unsigned gfl = 0;
		CHECK(qoa_decode_frame(g_frame, g_cap, &gd, g_pcm, &gfl) == g_cap && gfl == frame_len);
		uint64_t greedy_sse = sse16(pcm, g_pcm, frame_len);

		for (unsigned w = 1; w <= 16; w++) {
			qoa_desc bq = {0};
			bq.channels = 1;
			bq.samplerate = 48000;
			bq.lms[0].weights[2] = -(1 << 13);
			bq.lms[0].weights[3] = (1 << 14);
			unsigned char *b_frame = malloc(g_cap);
			CHECK(b_frame != NULL);
			CHECK(qoa_encode_frame_beam(pcm, &bq, frame_len, b_frame, g_cap, w) == g_cap);
			qoa_desc bd = {0};
			bd.channels = 1;
			bd.samplerate = 48000;
			short *b_pcm = calloc(frame_len, sizeof(short));
			CHECK(b_pcm != NULL);
			unsigned bfl = 0;
			CHECK(qoa_decode_frame(b_frame, g_cap, &bd, b_pcm, &bfl) == g_cap && bfl == frame_len);
			uint64_t beam_sse = sse16(pcm, b_pcm, frame_len);
			CHECK(beam_sse <= greedy_sse);
			free(b_frame);
			free(b_pcm);
		}

		free(g_frame);
		free(g_pcm);
		free(pcm);
	}

	/* A full multi-frame stereo file: beam output decodes identically through
	 * both decoder entry points and lands on the same final LMS. */
	enum { STEREO = QOA_FRAME_LEN + 37 };
	short *stereo = malloc((size_t)STEREO * 2 * sizeof(short));
	CHECK(stereo != NULL);
	g_rng_state = 31337;
	for (int i = 0; i < STEREO * 2; i++) {
		stereo[i] = (short)((int)(test_lcg() % 40001) - 20000);
	}
	qoa_desc se = {0};
	se.channels = 2;
	se.samplerate = 44100;
	se.samples = STEREO;
	unsigned se_len = 0;
	unsigned char *se_data = qoa_encode_beam(stereo, &se, &se_len, 12);
	CHECK(se_data != NULL);
	CHECK(verify_whole_file(se_data, se_len, 2, 44100, STEREO, &se, "legacy_repeat") == 0);
	QOA_FREE(se_data);
	free(stereo);

	CHECK(g_live == 0);
	return 0;
}

/* --- 6. failure_atomicity ------------------------------------------------ */

static int desc_equal(const qoa_desc *a, const qoa_desc *b) {
	return memcmp(a, b, sizeof(qoa_desc)) == 0;
}

int search_failure_atomicity(void) {
	short pcm[64];
	g_rng_state = 555;
	for (int i = 0; i < 64; i++) {
		pcm[i] = (short)((int)(test_lcg() % 40001) - 20000);
	}

	/* Invalid beam widths. */
	CHECK(qoa_encode_beam(pcm, &(qoa_desc){0}, &(unsigned){0}, 0) == NULL);
	CHECK(qoa_encode_beam(pcm, &(qoa_desc){0}, &(unsigned){0}, 17) == NULL);

	/* Null pointers. */
	{
		qoa_desc q = {0};
		unsigned len = 1234;
		CHECK(qoa_encode_beam(NULL, &q, &len, 4) == NULL);
		CHECK(len == 1234);
		CHECK(qoa_encode_beam(pcm, NULL, &len, 4) == NULL);
		CHECK(qoa_encode_beam(pcm, &q, NULL, 4) == NULL);
		unsigned char buf[64];
		CHECK(qoa_encode_frame_beam(NULL, &q, 20, buf, sizeof buf, 4) == 0);
		CHECK(qoa_encode_frame_beam(pcm, NULL, 20, buf, sizeof buf, 4) == 0);
		CHECK(qoa_encode_frame_beam(pcm, &q, 20, NULL, sizeof buf, 4) == 0);
	}

	/* Illegal audio descriptions for the whole-file entry. */
	{
		unsigned len = 77;
		qoa_desc bad_samples = {.channels = 1, .samplerate = 48000, .samples = 0};
		CHECK(qoa_encode_beam(pcm, &bad_samples, &len, 4) == NULL);
		CHECK(len == 77);
		qoa_desc bad_rate = {.channels = 1, .samplerate = 0, .samples = 40};
		CHECK(qoa_encode_beam(pcm, &bad_rate, &len, 4) == NULL);
		qoa_desc rate_too_big = {.channels = 1, .samplerate = 0x1000000, .samples = 40};
		CHECK(qoa_encode_beam(pcm, &rate_too_big, &len, 4) == NULL);
		qoa_desc no_channels = {.channels = 0, .samplerate = 48000, .samples = 40};
		CHECK(qoa_encode_beam(pcm, &no_channels, &len, 4) == NULL);
		qoa_desc too_many = {.channels = QOA_MAX_CHANNELS + 1, .samplerate = 48000, .samples = 40};
		CHECK(qoa_encode_beam(pcm, &too_many, &len, 4) == NULL);
		/* Size that cannot be expressed in a 32 bit length. */
		qoa_desc huge = {.channels = 8, .samplerate = 48000, .samples = 0xffffffffu};
		int calls_before = g_alloc_calls;
		CHECK(qoa_encode_beam(pcm, &huge, &len, 4) == NULL);
		CHECK(g_alloc_calls == calls_before);
		CHECK(len == 77);
	}

	/* Frame entry validation. */
	{
		qoa_desc q = {0};
		q.channels = 1;
		q.samplerate = 48000;
		q.lms[0].weights[2] = -(1 << 13);
		q.lms[0].weights[3] = (1 << 14);
		unsigned char buf[QOA_FRAME_SIZE(1, 256)];

		CHECK(qoa_encode_frame_beam(pcm, &q, 0, buf, sizeof buf, 4) == 0);
		CHECK(qoa_encode_frame_beam(pcm, &q, QOA_FRAME_LEN + 1, buf, sizeof buf, 4) == 0);
		CHECK(qoa_encode_frame_beam(pcm, &q, 20, buf, sizeof buf, 0) == 0);
		CHECK(qoa_encode_frame_beam(pcm, &q, 20, buf, sizeof buf, 17) == 0);

		qoa_desc bad_c = q;
		bad_c.channels = 9;
		CHECK(qoa_encode_frame_beam(pcm, &bad_c, 20, buf, sizeof buf, 4) == 0);
		qoa_desc bad_r = q;
		bad_r.samplerate = 0;
		CHECK(qoa_encode_frame_beam(pcm, &bad_r, 20, buf, sizeof buf, 4) == 0);

		/* Capacity short by one byte: fail, descriptor and output untouched. */
		qoa_desc snap = q;
		unsigned char sentinel[64];
		memset(sentinel, 0xA5, sizeof sentinel);
		CHECK(qoa_encode_frame_beam(pcm, &q, 20, sentinel, QOA_FRAME_SIZE(1, 1) - 1, 4) == 0);
		CHECK(desc_equal(&q, &snap));
		for (unsigned i = 0; i < sizeof sentinel; i++) {
			CHECK(sentinel[i] == 0xA5);
		}
	}

	/* Injected allocation failure for the frame-entry workspace. There is a
	 * single QOA_MALLOC per frame call. */
	{
		qoa_desc q = {0};
		q.channels = 1;
		q.samplerate = 48000;
		q.lms[0].weights[2] = -(1 << 13);
		q.lms[0].weights[3] = (1 << 14);
		qoa_desc snap = q;
		unsigned char sentinel[64];
		memset(sentinel, 0xA5, sizeof sentinel);

		g_fail_on = g_alloc_calls + 1;
		CHECK(qoa_encode_frame_beam(pcm, &q, 20, sentinel, sizeof sentinel, 16) == 0);
		g_fail_on = 0;
		CHECK(desc_equal(&q, &snap));
		for (unsigned i = 0; i < sizeof sentinel; i++) {
			CHECK(sentinel[i] == 0xA5);
		}
		CHECK(g_live == 0);
	}

	/* Injected failure on the whole-file output buffer. */
	{
		qoa_desc q = {0};
		q.channels = 1;
		q.samplerate = 48000;
		q.samples = 40;
		qoa_desc snap = q;
		unsigned len = 99;
		g_fail_on = g_alloc_calls + 1;
		CHECK(qoa_encode_beam(pcm, &q, &len, 16) == NULL);
		g_fail_on = 0;
		CHECK(desc_equal(&q, &snap));
		CHECK(len == 99);
		CHECK(g_live == 0);
	}

	/* Injected failure on a later frame's workspace: the output buffer and
	 * every workspace must be released and the descriptor restored. */
	{
		enum { N = QOA_FRAME_LEN + 20 };
		short *big = malloc((size_t)N * sizeof(short));
		CHECK(big != NULL);
		g_rng_state = 808;
		for (int i = 0; i < N; i++) {
			big[i] = (short)((int)(test_lcg() % 40001) - 20000);
		}
		qoa_desc q = {0};
		q.channels = 1;
		q.samplerate = 48000;
		q.samples = N;
		qoa_desc snap = q;
		unsigned len = 55;
		/* First allocation is the output buffer; second frame's workspace is
		 * allocation #3 (buffer, frame1 workspace, frame2 workspace). */
		g_fail_on = g_alloc_calls + 3;
		CHECK(qoa_encode_beam(big, &q, &len, 16) == NULL);
		g_fail_on = 0;
		CHECK(desc_equal(&q, &snap));
		CHECK(len == 55);
		CHECK(g_live == 0);
		free(big);
	}

	/* A successful frame call leaves no workspace allocated behind. */
	{
		qoa_desc q = {0};
		q.channels = 2;
		q.samplerate = 48000;
		for (unsigned c = 0; c < 2; c++) {
			q.lms[c].weights[2] = -(1 << 13);
			q.lms[c].weights[3] = (1 << 14);
		}
		short stereo[40];
		for (int i = 0; i < 40; i++) {
			stereo[i] = pcm[i];
		}
		unsigned char buf[QOA_FRAME_SIZE(2, 1)];
		int live_before = g_live;
		CHECK(qoa_encode_frame_beam(stereo, &q, 20, buf, sizeof buf, 16) == QOA_FRAME_SIZE(2, 1));
		CHECK(g_live == live_before);
	}

	CHECK(g_live == 0);
	return 0;
}
