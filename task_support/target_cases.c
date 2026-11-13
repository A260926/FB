/*
 *  task_support/target_cases.c
 *
 *  Test cases for 32-bit FLAC decoding in libfoxenflac. All FLAC streams are
 *  generated locally by the small bit-exact encoder below (with correct
 *  CRC8/CRC16 checksums). Expected sample values are derived from the
 *  original PCM data or explicit integer recurrences (predictor formulas),
 *  never from the decoder under test.
 */

#include "check.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "foxen-flac.h"

/* ------------------------------------------------------------------ */
/* Bit writer and CRCs                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
	uint8_t *buf;
	size_t len, cap;
	uint64_t acc;
	unsigned nbits;
} bw_t;

static void bw_init(bw_t *w, uint8_t *buf, size_t cap) {
	w->buf = buf;
	w->len = 0;
	w->cap = cap;
	w->acc = 0;
	w->nbits = 0;
}

/* Appends the n low bits of v to the stream, MSB first. n must be <= 56. */
static void bw_bits(bw_t *w, uint64_t v, unsigned n) {
	if (n == 0U) {
		return;
	}
	w->acc = (w->acc << n) | (v & ((1ULL << n) - 1ULL));
	w->nbits += n;
	while (w->nbits >= 8U) {
		if (w->len >= w->cap) {
			task_check(0, "generator buffer overflow");
		}
		w->buf[w->len++] = (uint8_t)(w->acc >> (w->nbits - 8U));
		w->nbits -= 8U;
		w->acc &= (1ULL << w->nbits) - 1ULL;
	}
}

static uint8_t gen_crc8(const uint8_t *p, size_t n) {
	uint8_t crc = 0U;
	for (size_t i = 0; i < n; i++) {
		crc ^= p[i];
		for (int k = 0; k < 8; k++) {
			crc = (uint8_t)((crc << 1) ^ ((crc & 0x80U) ? 0x07U : 0U));
		}
	}
	return crc;
}

static uint16_t gen_crc16(const uint8_t *p, size_t n) {
	uint16_t crc = 0U;
	for (size_t i = 0; i < n; i++) {
		crc = (uint16_t)(crc ^ ((uint16_t)p[i] << 8U));
		for (int k = 0; k < 8; k++) {
			crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000U) ? 0x8005U : 0U));
		}
	}
	return crc;
}

/* ------------------------------------------------------------------ */
/* FLAC stream generator                                               */
/* ------------------------------------------------------------------ */

typedef struct {
	bw_t w;
	size_t frame_start;
} gen_t;

static uint8_t GEN_BUF[131072];
static gen_t G;

static void gen_reset(void) {
	bw_init(&G.w, GEN_BUF, sizeof(GEN_BUF));
}

static void gen_streaminfo(bw_t *w, unsigned min_bs, unsigned max_bs,
                           unsigned rate, unsigned nch, unsigned bps,
                           uint64_t total) {
	bw_bits(w, 0x66, 8); /* 'f' */
	bw_bits(w, 0x4C, 8); /* 'L' */
	bw_bits(w, 0x61, 8); /* 'a' */
	bw_bits(w, 0x43, 8); /* 'C' */
	bw_bits(w, 1, 1);    /* last metadata block */
	bw_bits(w, 0, 7);    /* type: STREAMINFO */
	bw_bits(w, 34, 24);  /* length */
	bw_bits(w, min_bs, 16);
	bw_bits(w, max_bs, 16);
	bw_bits(w, 0, 24); /* min frame size */
	bw_bits(w, 0, 24); /* max frame size */
	bw_bits(w, rate, 20);
	bw_bits(w, nch - 1, 3);
	bw_bits(w, bps - 1, 5);
	bw_bits(w, total, 36);
	for (int i = 0; i < 16; i++) {
		bw_bits(w, 0, 8); /* md5 */
	}
}

/* Channel assignments */
#define CH_MONO 0U
#define CH_STEREO 1U
#define CH_LEFT_SIDE 8U
#define CH_RIGHT_SIDE 9U
#define CH_MID_SIDE 10U

/* Sample size enums (0 = inherit from STREAMINFO) */
#define SS_STREAMINFO 0U
#define SS_8BIT 1U
#define SS_12BIT 2U
#define SS_16BIT 4U
#define SS_20BIT 5U
#define SS_24BIT 6U
#define SS_32BIT 7U

static void gen_frame_begin(gen_t *g, unsigned block_size, unsigned chan_assign,
                            unsigned ss_enum, unsigned frame_no) {
	bw_t *w = &g->w;
	g->frame_start = w->len;
	bw_bits(w, 0x3FFE, 14); /* sync code */
	bw_bits(w, 0, 1);       /* reserved */
	bw_bits(w, 0, 1);       /* blocking strategy: fixed */
	if (block_size <= 256) {
		bw_bits(w, 6, 4); /* block size from 8-bit aux */
	} else {
		bw_bits(w, 7, 4); /* block size from 16-bit aux */
	}
	bw_bits(w, 0, 4); /* sample rate: from STREAMINFO */
	bw_bits(w, chan_assign, 4);
	bw_bits(w, ss_enum, 3);
	bw_bits(w, 0, 1); /* reserved */
	/* UTF-8 coded frame number */
	if (frame_no < 0x80U) {
		bw_bits(w, frame_no, 8);
	} else {
		bw_bits(w, 0xC0U | (frame_no >> 6), 8);
		bw_bits(w, 0x80U | (frame_no & 0x3FU), 8);
	}
	if (block_size <= 256) {
		bw_bits(w, block_size - 1, 8);
	} else {
		bw_bits(w, block_size - 1, 16);
	}
	bw_bits(w, gen_crc8(w->buf + g->frame_start, w->len - g->frame_start), 8);
}

static void gen_frame_end(gen_t *g) {
	bw_t *w = &g->w;
	if (w->nbits) {
		bw_bits(w, 0, 8U - w->nbits); /* zero padding to byte boundary */
	}
	bw_bits(w, gen_crc16(w->buf + g->frame_start, w->len - g->frame_start),
	        16);
}

static void gen_wasted(gen_t *g, unsigned wasted) {
	if (wasted) {
		bw_bits(&g->w, 1, 1); /* flag */
		for (unsigned i = 1; i < wasted; i++) {
			bw_bits(&g->w, 0, 1);
		}
		bw_bits(&g->w, 1, 1);
	} else {
		bw_bits(&g->w, 0, 1);
	}
}

static void gen_samples(gen_t *g, unsigned bps, const int64_t *v, unsigned n) {
	for (unsigned i = 0; i < n; i++) {
		bw_bits(&g->w, (uint64_t)v[i] & ((1ULL << bps) - 1ULL), bps);
	}
}

typedef struct {
	unsigned method;        /* 0 = rice, 1 = rice2 */
	unsigned porder;        /* partition order */
	const uint8_t *params;  /* per-partition rice parameter (or escape) */
	const uint8_t *esc_bps; /* per-partition escape bit width (0..31) */
} rice_cfg_t;

static void gen_residual(gen_t *g, const rice_cfg_t *rc, const int32_t *res,
                         unsigned n_res, unsigned blk, unsigned order) {
	const unsigned escape = (rc->method == 0U) ? 15U : 31U;
	const unsigned pbits = (rc->method == 0U) ? 4U : 5U;
	unsigned idx = 0;
	bw_bits(&g->w, rc->method, 2);
	bw_bits(&g->w, rc->porder, 4);
	for (unsigned p = 0; p < (1U << rc->porder); p++) {
		const unsigned n = (blk >> rc->porder) - ((p == 0U) ? order : 0U);
		const unsigned param = rc->params[p];
		bw_bits(&g->w, param, pbits);
		if (param == escape) {
			const unsigned eb = rc->esc_bps[p];
			bw_bits(&g->w, eb, 5);
			for (unsigned i = 0; i < n; i++) {
				bw_bits(&g->w,
				        (uint64_t)(int64_t)res[idx] &
				            ((eb > 0U) ? ((1ULL << eb) - 1ULL) : 0ULL),
				        eb);
				idx++;
			}
		} else {
			for (unsigned i = 0; i < n; i++) {
				const int32_t r = res[idx++];
				const uint64_t u = (r >= 0)
				                       ? (2ULL * (uint64_t)r)
				                       : (2ULL * (uint64_t)(-(int64_t)r) - 1ULL);
				const uint64_t q = u >> param;
				for (uint64_t k = 0; k < q; k++) {
					bw_bits(&g->w, 0, 1);
				}
				bw_bits(&g->w, 1, 1);
				if (param) {
					bw_bits(&g->w, u & ((1ULL << param) - 1ULL), param);
				}
			}
		}
	}
	task_check(idx == n_res, "generator residual count mismatch");
}

static void gen_sub_constant(gen_t *g, unsigned bps, int64_t value,
                             unsigned wasted) {
	bw_bits(&g->w, 0, 1);    /* padding */
	bw_bits(&g->w, 0x00, 6); /* constant */
	gen_wasted(g, wasted);
	bw_bits(&g->w, (uint64_t)value & ((1ULL << bps) - 1ULL), bps);
}

static void gen_sub_verbatim(gen_t *g, unsigned bps, const int64_t *v,
                             unsigned n, unsigned wasted) {
	bw_bits(&g->w, 0, 1);    /* padding */
	bw_bits(&g->w, 0x01, 6); /* verbatim */
	gen_wasted(g, wasted);
	gen_samples(g, bps, v, n);
}

static void gen_sub_fixed(gen_t *g, unsigned bps, unsigned blk, unsigned order,
                          const int64_t *warm, const int32_t *res,
                          const rice_cfg_t *rc, unsigned wasted) {
	bw_bits(&g->w, 0, 1);             /* padding */
	bw_bits(&g->w, 0x08U | order, 6); /* fixed, order 0..4 */
	gen_wasted(g, wasted);
	gen_samples(g, bps, warm, order);
	gen_residual(g, rc, res, blk - order, blk, order);
}

static void gen_sub_lpc(gen_t *g, unsigned bps, unsigned blk, unsigned order,
                        unsigned prec, unsigned shift, const int32_t *coefs,
                        const int64_t *warm, const int32_t *res,
                        const rice_cfg_t *rc, unsigned wasted) {
	bw_bits(&g->w, 0, 1);                    /* padding */
	bw_bits(&g->w, 0x20U | (order - 1U), 6); /* LPC, order 1..32 */
	gen_wasted(g, wasted);
	gen_samples(g, bps, warm, order);
	bw_bits(&g->w, prec - 1, 4);
	bw_bits(&g->w, shift, 5); /* non-negative shift only */
	for (unsigned i = 0; i < order; i++) {
		bw_bits(&g->w, (uint64_t)(int64_t)coefs[i] & ((1ULL << prec) - 1ULL),
		        prec);
	}
	gen_residual(g, rc, res, blk - order, blk, order);
}

/* ------------------------------------------------------------------ */
/* Reference integer recurrences (expected values)                     */
/* ------------------------------------------------------------------ */

/* Arithmetic (floor) right shift, matching the FLAC specification. */
static int64_t floor_shift(int64_t x, unsigned s) {
	return (x >= 0) ? (x >> s) : (-((-(x + 1)) >> s) - 1);
}

static const int32_t FIXED_COEFS[5][4] = {{0, 0, 0, 0}, {1, 0, 0, 0},
                                          {2, -1, 0, 0}, {3, -3, 1, 0},
                                          {4, -6, 4, -1}};

/* Computes expected PCM from warm-up samples and residuals using the
   predictor recurrence (explicit integer recurrence reference). */
static void ref_predict(const int32_t *coefs, unsigned order, unsigned shift,
                        const int64_t *warm, const int32_t *res,
                        unsigned n_res, int64_t *out) {
	for (unsigned i = 0; i < order; i++) {
		out[i] = warm[i];
	}
	for (unsigned i = 0; i < n_res; i++) {
		int64_t acc = 0;
		for (unsigned j = 0; j < order; j++) {
			acc += (int64_t)coefs[j] * out[i + order - 1 - j];
		}
		out[i + order] = (int64_t)res[i] + floor_shift(acc, shift);
	}
}

/* Mid/side reconstruction reference (inverse of the encoder mapping). */
static void ref_mid_side(const int64_t *mid, const int64_t *side, unsigned n,
                         int64_t *L, int64_t *R) {
	for (unsigned i = 0; i < n; i++) {
		const int64_t m = mid[i] * 2 + (side[i] & 1);
		L[i] = floor_shift(m + side[i], 1);
		R[i] = floor_shift(m - side[i], 1);
	}
}

/* Encoder-side mid/side computation from raw PCM. */
static int64_t enc_mid(int64_t L, int64_t R) {
	return floor_shift(L + R, 1);
}

static int64_t enc_side(int64_t L, int64_t R) {
	return L - R;
}

/* Left-aligns a sample of the given bit depth in a 32-bit word, matching the
   alignment convention of fx_flac_process. */
static int32_t align32(int64_t v, unsigned bps) {
	return (int32_t)(v * ((int64_t)1 << (32U - bps)));
}

/* ------------------------------------------------------------------ */
/* Decode-and-compare harness                                          */
/* ------------------------------------------------------------------ */

static void decode_all(fx_flac_t *d, const uint8_t *data, size_t len,
                       unsigned in_chunk, unsigned out_chunk,
                       const int32_t *exp, size_t n_exp, unsigned expect_bps,
                       unsigned expect_ch, const char *msg) {
	char m[192];
	size_t pos = 0, got = 0;
	int meta_checked = 0;
	for (unsigned long it = 0; it < 8000000UL; it++) {
		uint32_t ilen =
		    (uint32_t)((len - pos) < in_chunk ? (len - pos) : in_chunk);
		int32_t out[32];
		uint32_t olen = out_chunk;
		fx_flac_state_t st = fx_flac_process(d, data + pos, &ilen, out, &olen);
		task_check(st != FLAC_ERR, msg);
		task_check(ilen <= in_chunk, "input over-consumed");
		task_check(olen <= out_chunk, "output over-produced");
		if (st == FLAC_END_OF_METADATA && !meta_checked) {
			meta_checked = 1;
			if (expect_bps) {
				task_check(fx_flac_get_streaminfo(d, FLAC_KEY_SAMPLE_SIZE) ==
				               (int64_t)expect_bps,
				           "streaminfo sample size mismatch");
				task_check(fx_flac_get_streaminfo(d, FLAC_KEY_N_CHANNELS) ==
				               (int64_t)expect_ch,
				           "streaminfo channel count mismatch");
			}
		}
		pos += ilen;
		for (uint32_t i = 0; i < olen; i++) {
			if (got >= n_exp) {
				snprintf(m, sizeof(m), "%s: unexpected extra sample %zu = %ld",
				         msg, got, (long)out[i]);
				task_check(0, m);
			}
			if (out[i] != exp[got]) {
				snprintf(m, sizeof(m),
				         "%s: sample %zu got %ld, want %ld", msg, got,
				         (long)out[i], (long)exp[got]);
				task_check(0, m);
			}
			got++;
		}
		if (ilen == 0 && olen == 0) {
			if (pos == len && got == n_exp) {
				break;
			}
			snprintf(m, sizeof(m), "%s: decoder made no progress", msg);
			task_check(0, m);
		}
		if (pos == len && got == n_exp) {
			break;
		}
	}
	task_check(got == n_exp, "sample count mismatch");
	task_check(pos == len, "input not fully consumed");
	task_check(fx_flac_get_state(d) != FLAC_DECODED_FRAME,
	           "undelivered samples remain");
}

static void run_case(const char *msg, const uint8_t *data, size_t len,
                     unsigned in_chunk, unsigned out_chunk, const int32_t *exp,
                     size_t n_exp, unsigned expect_bps, unsigned expect_ch,
                     uint16_t max_bs, uint8_t max_ch) {
	fx_flac_t *d = FX_FLAC_ALLOC(max_bs, max_ch);
	task_check(d != NULL, "fx_flac_alloc failed");
	decode_all(d, data, len, in_chunk, out_chunk, exp, n_exp, expect_bps,
	           expect_ch, msg);
	free(d);
}

/* ------------------------------------------------------------------ */
/* Group "samples": subframe types and predictor boundaries            */
/* ------------------------------------------------------------------ */

static void group_samples(void) {
	int32_t exp[160];

	/* S1: constant subframes, 32-bit depth inherited from STREAMINFO */
	{
		gen_reset();
		gen_streaminfo(&G.w, 8, 8, 44100, 1, 32, 16);
		gen_frame_begin(&G, 8, CH_MONO, SS_STREAMINFO, 0);
		gen_sub_constant(&G, 32, INT32_MIN, 0);
		gen_frame_end(&G);
		gen_frame_begin(&G, 8, CH_MONO, SS_STREAMINFO, 1);
		gen_sub_constant(&G, 32, INT32_MAX, 0);
		gen_frame_end(&G);
		for (int i = 0; i < 8; i++) {
			exp[i] = INT32_MIN;
			exp[8 + i] = INT32_MAX;
		}
		run_case("S1 constant 32-bit inherited", GEN_BUF, G.w.len, 5, 7, exp,
		         16, 32, 1, 8, 1);
	}

	/* S2: constant subframes, 32-bit depth from frame header code 7.
	   Values chosen so that any truncation to 24 bits is visible. */
	{
		gen_reset();
		gen_streaminfo(&G.w, 8, 8, 44100, 1, 32, 16);
		gen_frame_begin(&G, 8, CH_MONO, SS_32BIT, 0);
		gen_sub_constant(&G, 32, -2147483647LL, 0); /* low 24 bits = 1 */
		gen_frame_end(&G);
		gen_frame_begin(&G, 8, CH_MONO, SS_32BIT, 1);
		gen_sub_constant(&G, 32, 0x7F000000LL, 0); /* low 24 bits = 0 */
		gen_frame_end(&G);
		for (int i = 0; i < 8; i++) {
			exp[i] = -2147483647;
			exp[8 + i] = 0x7F000000;
		}
		run_case("S2 constant 32-bit code 7", GEN_BUF, G.w.len, 3, 5, exp, 16,
		         32, 1, 8, 1);
	}

	/* S3: verbatim subframes, independent stereo 32-bit */
	{
		static const int64_t L[8] = {INT32_MIN, INT32_MAX, 0x01000000,
		                             -2147483647LL, 0x12345678, -1, 0, 1};
		static const int64_t R[8] = {INT32_MAX, INT32_MIN, -0x01000000,
		                             2147483647LL, -0x12345678, 1, 0, -1};
		gen_reset();
		gen_streaminfo(&G.w, 8, 8, 44100, 2, 32, 8);
		gen_frame_begin(&G, 8, CH_STEREO, SS_STREAMINFO, 0);
		gen_sub_verbatim(&G, 32, L, 8, 0);
		gen_sub_verbatim(&G, 32, R, 8, 0);
		gen_frame_end(&G);
		for (int i = 0; i < 8; i++) {
			exp[2 * i] = (int32_t)L[i];
			exp[2 * i + 1] = (int32_t)R[i];
		}
		run_case("S3 verbatim stereo 32-bit", GEN_BUF, G.w.len, 11, 4, exp, 16,
		         32, 2, 8, 2);
	}

	/* S4: fixed predictors, orders 0..4, 32-bit mono */
	{
		gen_reset();
		gen_streaminfo(&G.w, 16, 16, 44100, 1, 32, 16 * 8);
		unsigned fn = 0;

		/* order 0: residuals are the samples themselves, at the edges of
		   the legal residual range; Rice2 with a large parameter */
		static const int32_t S4A[16] = {
		    2147483647, -2147483647, 0, 1, -1, 1073741824, -1073741824,
		    123456789, -123456789, 65536, -65536, 2147483646, -2147483646,
		    2, -2, 0};
		static const uint8_t P20[1] = {20};
		rice_cfg_t rc = {1, 0, P20, NULL};
		gen_frame_begin(&G, 16, CH_MONO, SS_STREAMINFO, fn++);
		gen_sub_fixed(&G, 32, 16, 0, NULL, S4A, &rc, 0);
		gen_frame_end(&G);

		/* order 1: walks around INT32_MIN / INT32_MAX */
		static const int64_t W_MIN[1] = {INT32_MIN};
		static const int32_t R_MIN[15] = {0, 0, 1, -1, 0, 0, 2, -2,
		                                  0, 0, 0, 0,  1, 0, -1};
		static const uint8_t P4[1] = {4};
		rice_cfg_t rc4 = {0, 0, P4, NULL};
		gen_frame_begin(&G, 16, CH_MONO, SS_STREAMINFO, fn++);
		gen_sub_fixed(&G, 32, 16, 1, W_MIN, R_MIN, &rc4, 0);
		gen_frame_end(&G);

		static const int64_t W_MAX[1] = {INT32_MAX};
		static const int32_t R_MAX[15] = {0, 0, -1, 1, 0, 0, -2, 2,
		                                  0, 0, 0,  0, -1, 0, 1};
		gen_frame_begin(&G, 16, CH_MONO, SS_STREAMINFO, fn++);
		gen_sub_fixed(&G, 32, 16, 1, W_MAX, R_MAX, &rc4, 0);
		gen_frame_end(&G);

		/* order 1 with large jumps within the residual range */
		static const int64_t W_0[1] = {0};
		static const int32_t R_JMP[15] = {1073741824, -1073741824, 536870912,
		                                  -536870912, 268435456, -268435456,
		                                  1, -1, 0, 0, 0, 0, 0, 0, 0};
		gen_frame_begin(&G, 16, CH_MONO, SS_STREAMINFO, fn++);
		gen_sub_fixed(&G, 32, 16, 1, W_0, R_JMP, &rc, 0);
		gen_frame_end(&G);

		/* order 2: constant extremes, predictor intermediate 2*x exceeds
		   32 bits */
		static const int64_t W2_MAX[2] = {INT32_MAX, INT32_MAX};
		static const int64_t W2_MIN[2] = {INT32_MIN, INT32_MIN};
		static const int32_t R_Z[14] = {0};
		gen_frame_begin(&G, 16, CH_MONO, SS_STREAMINFO, fn++);
		gen_sub_fixed(&G, 32, 16, 2, W2_MAX, R_Z, &rc4, 0);
		gen_frame_end(&G);
		gen_frame_begin(&G, 16, CH_MONO, SS_STREAMINFO, fn++);
		gen_sub_fixed(&G, 32, 16, 2, W2_MIN, R_Z, &rc4, 0);
		gen_frame_end(&G);

		/* order 3: quadratic sequence, exact prediction */
		int64_t w3[3];
		int32_t r13[13] = {0};
		for (int n = 0; n < 3; n++) {
			w3[n] = 1000000LL * n * n - 2000000000LL;
		}
		gen_frame_begin(&G, 16, CH_MONO, SS_STREAMINFO, fn++);
		gen_sub_fixed(&G, 32, 16, 3, w3, r13, &rc4, 0);
		gen_frame_end(&G);

		/* order 4: cubic sequence, exact prediction */
		int64_t w4[4];
		int32_t r12[12] = {0};
		for (int n = 0; n < 4; n++) {
			w4[n] = 12345LL * n * n * n - 1000000000LL;
		}
		gen_frame_begin(&G, 16, CH_MONO, SS_STREAMINFO, fn++);
		gen_sub_fixed(&G, 32, 16, 4, w4, r12, &rc4, 0);
		gen_frame_end(&G);

		/* expected values via the predictor recurrence */
		size_t k = 0;
		for (int i = 0; i < 16; i++) {
			exp[k++] = S4A[i];
		}
		int64_t out[16];
		ref_predict(FIXED_COEFS[1], 1, 0, W_MIN, R_MIN, 15, out);
		for (int i = 0; i < 16; i++) {
			exp[k++] = (int32_t)out[i];
		}
		ref_predict(FIXED_COEFS[1], 1, 0, W_MAX, R_MAX, 15, out);
		for (int i = 0; i < 16; i++) {
			exp[k++] = (int32_t)out[i];
		}
		ref_predict(FIXED_COEFS[1], 1, 0, W_0, R_JMP, 15, out);
		for (int i = 0; i < 16; i++) {
			exp[k++] = (int32_t)out[i];
		}
		ref_predict(FIXED_COEFS[2], 2, 0, W2_MAX, R_Z, 14, out);
		for (int i = 0; i < 16; i++) {
			exp[k++] = (int32_t)out[i];
		}
		ref_predict(FIXED_COEFS[2], 2, 0, W2_MIN, R_Z, 14, out);
		for (int i = 0; i < 16; i++) {
			exp[k++] = (int32_t)out[i];
		}
		ref_predict(FIXED_COEFS[3], 3, 0, w3, r13, 13, out);
		for (int i = 0; i < 16; i++) {
			exp[k++] = (int32_t)out[i];
		}
		ref_predict(FIXED_COEFS[4], 4, 0, w4, r12, 12, out);
		for (int i = 0; i < 16; i++) {
			exp[k++] = (int32_t)out[i];
		}
		run_case("S4 fixed predictors 32-bit", GEN_BUF, G.w.len, 7, 9, exp,
		         16 * 8, 32, 1, 16, 1);
	}

	/* S5: LPC subframes, 32-bit mono; accumulator and shift boundaries */
	{
		gen_reset();
		gen_streaminfo(&G.w, 36, 36, 44100, 1, 32, 80);
		unsigned fn = 0;
		static const uint8_t P4[1] = {4};
		rice_cfg_t rc4 = {0, 0, P4, NULL};
		static const uint8_t P20[1] = {20};
		rice_cfg_t rc20 = {1, 0, P20, NULL};

		/* L1: order 1, coeff 1, shift 0, walking off INT32_MIN */
		static const int64_t L1W[1] = {INT32_MIN + 4};
		static const int32_t L1R[15] = {0, 1, -1, 2, -2, 0, 0, 0,
		                                1, -1, 0, 0, 0, 0, 0};
		static const int32_t C1[1] = {1};
		gen_frame_begin(&G, 16, CH_MONO, SS_STREAMINFO, fn++);
		gen_sub_lpc(&G, 32, 16, 1, 2, 0, C1, L1W, L1R, &rc4, 0);
		gen_frame_end(&G);

		/* L2: order 1, 15-bit precision, coeff -16384, shift 15; the
		   accumulator reaches 2^45, must not be truncated to 32 bits */
		static const int64_t L2W[1] = {INT32_MIN};
		static const int32_t L2R[3] = {-1073741823, 0, 0};
		static const int32_t C_BIG[1] = {-16384};
		gen_frame_begin(&G, 4, CH_MONO, SS_STREAMINFO, fn++);
		gen_sub_lpc(&G, 32, 4, 1, 15, 15, C_BIG, L2W, L2R, &rc20, 0);
		gen_frame_end(&G);

		/* L3: same, with INT32_MAX warm-up; prediction crosses zero */
		static const int64_t L3W[1] = {INT32_MAX};
		static const int32_t L3R[3] = {1073741823, 0, 0};
		gen_frame_begin(&G, 4, CH_MONO, SS_STREAMINFO, fn++);
		gen_sub_lpc(&G, 32, 4, 1, 15, 15, C_BIG, L3W, L3R, &rc20, 0);
		gen_frame_end(&G);

		/* L4: order 2, coeffs [3,-1], shift 1, exact -1 progression from
		   INT32_MAX */
		static const int64_t L4W[2] = {INT32_MAX, INT32_MAX - 2};
		static const int32_t L4R[6] = {0, 0, 0, 0, 0, 0};
		static const int32_t C31[2] = {3, -1};
		gen_frame_begin(&G, 8, CH_MONO, SS_STREAMINFO, fn++);
		gen_sub_lpc(&G, 32, 8, 2, 4, 1, C31, L4W, L4R, &rc4, 0);
		gen_frame_end(&G);

		/* L5: order 32 (maximum), first coeff 1, others 0 */
		int64_t w32[32];
		int32_t c32[32];
		int32_t r32[4] = {0, 0, 0, 0};
		for (int i = 0; i < 32; i++) {
			w32[i] = (i & 1) ? (INT32_MIN + i) : (INT32_MAX - i);
			c32[i] = (i == 0) ? 1 : 0;
		}
		gen_frame_begin(&G, 36, CH_MONO, SS_STREAMINFO, fn++);
		gen_sub_lpc(&G, 32, 36, 32, 2, 0, c32, w32, r32, &rc4, 0);
		gen_frame_end(&G);

		/* L6: shift 15 boundary with coeff 1 */
		static const int64_t L6W[1] = {INT32_MIN};
		static const int32_t L6R[3] = {65536, 0, 0};
		gen_frame_begin(&G, 4, CH_MONO, SS_STREAMINFO, fn++);
		gen_sub_lpc(&G, 32, 4, 1, 2, 15, C1, L6W, L6R, &rc4, 0);
		gen_frame_end(&G);

		/* L7: precision 1 boundary, coeff -1 */
		static const int64_t L7W[1] = {100};
		static const int32_t L7R[7] = {5, -3, 2, -7, 0, 1, -1};
		static const int32_t C_M1[1] = {-1};
		gen_frame_begin(&G, 8, CH_MONO, SS_STREAMINFO, fn++);
		gen_sub_lpc(&G, 32, 8, 1, 1, 0, C_M1, L7W, L7R, &rc4, 0);
		gen_frame_end(&G);

		/* expected values via the predictor recurrence */
		size_t k = 0;
		int64_t out[36];
		ref_predict(C1, 1, 0, L1W, L1R, 15, out);
		for (int i = 0; i < 16; i++) {
			exp[k++] = (int32_t)out[i];
		}
		ref_predict(C_BIG, 1, 15, L2W, L2R, 3, out);
		for (int i = 0; i < 4; i++) {
			exp[k++] = (int32_t)out[i];
		}
		ref_predict(C_BIG, 1, 15, L3W, L3R, 3, out);
		for (int i = 0; i < 4; i++) {
			exp[k++] = (int32_t)out[i];
		}
		ref_predict(C31, 2, 1, L4W, L4R, 6, out);
		for (int i = 0; i < 8; i++) {
			exp[k++] = (int32_t)out[i];
		}
		ref_predict(c32, 32, 0, w32, r32, 4, out);
		for (int i = 0; i < 36; i++) {
			exp[k++] = (int32_t)out[i];
		}
		ref_predict(C1, 1, 15, L6W, L6R, 3, out);
		for (int i = 0; i < 4; i++) {
			exp[k++] = (int32_t)out[i];
		}
		ref_predict(C_M1, 1, 0, L7W, L7R, 7, out);
		for (int i = 0; i < 8; i++) {
			exp[k++] = (int32_t)out[i];
		}
		run_case("S5 LPC 32-bit", GEN_BUF, G.w.len, 13, 11, exp, k, 32, 1, 36,
		         1);
	}

	/* S6: wasted bits, 32-bit mono */
	{
		gen_reset();
		gen_streaminfo(&G.w, 8, 8, 44100, 1, 32, 24);
		unsigned fn = 0;

		/* W1: constant -1 with 31 wasted bits -> INT32_MIN */
		gen_frame_begin(&G, 8, CH_MONO, SS_STREAMINFO, fn++);
		gen_sub_constant(&G, 1, -1, 31);
		gen_frame_end(&G);

		/* W2: verbatim with 8 wasted bits, raw values at 24-bit extremes */
		static const int64_t W2V[8] = {0x7FFFFF, -0x800000, 0x123456, -1,
		                               0,        1,        0x400000, -0x400000};
		gen_frame_begin(&G, 8, CH_MONO, SS_STREAMINFO, fn++);
		gen_sub_verbatim(&G, 24, W2V, 8, 8);
		gen_frame_end(&G);

		/* W3: fixed order 1 with 4 wasted bits */
		static const int64_t W3W[1] = {0x7FFFFFF};
		static const int32_t W3R[7] = {1, -1, 2, -2, 0, 0, 0};
		static const uint8_t P4[1] = {4};
		rice_cfg_t rc4 = {0, 0, P4, NULL};
		gen_frame_begin(&G, 8, CH_MONO, SS_STREAMINFO, fn++);
		gen_sub_fixed(&G, 28, 8, 1, W3W, W3R, &rc4, 4);
		gen_frame_end(&G);

		size_t k = 0;
		for (int i = 0; i < 8; i++) {
			exp[k++] = INT32_MIN; /* W1 */
		}
		for (int i = 0; i < 8; i++) {
			exp[k++] = (int32_t)(W2V[i] * 256); /* W2 */
		}
		int64_t out[8];
		ref_predict(FIXED_COEFS[1], 1, 0, W3W, W3R, 7, out);
		for (int i = 0; i < 8; i++) {
			exp[k++] = (int32_t)(out[i] * 16); /* W3 */
		}
		run_case("S6 wasted bits 32-bit", GEN_BUF, G.w.len, 9, 6, exp, k, 32,
		         1, 8, 1);
	}

	/* S7: Rice/Rice2, escape codes, partition orders, 32-bit mono */
	{
		gen_reset();
		gen_streaminfo(&G.w, 32, 32, 44100, 1, 32, 32 * 5);
		unsigned fn = 0;
		static const int64_t WARM[1] = {INT32_MIN + 100};

		/* R1: Rice, partition order 2, params incl. 14 (max for Rice) */
		static const int32_t R1RES[31] = {
		    3, -5, 2, -1, 0, 1, -2, /* p0: param 4 */
		    100, -100, 200, -200, 300, -300, 0, 0, /* p1: param 14 */
		    -3, 2, -1, 0, 5, -8, 1, -1, /* p2: param 0 */
		    7, -9, 3, -3, 2, -2, 1, -1 /* p3: param 4 */
		};
		static const uint8_t R1P[4] = {4, 14, 0, 4};
		rice_cfg_t rc1 = {0, 2, R1P, NULL};
		gen_frame_begin(&G, 32, CH_MONO, SS_STREAMINFO, fn++);
		gen_sub_fixed(&G, 32, 32, 1, WARM, R1RES, &rc1, 0);
		gen_frame_end(&G);

		/* R2: Rice2, partition order 2, param 20 */
		static const int32_t R2RES[31] = {
		    100000, -100000, 50000, -50000, 0, 1, -1,
		    200000, -200000, 0, 0, 0, 0, 0, 0,
		    1, -1, 2, -2, 3, -3, 4, -4,
		    5, -5, 6, -6, 7, -7, 8, -8
		};
		static const uint8_t R2P[4] = {20, 20, 20, 20};
		rice_cfg_t rc2 = {1, 2, R2P, NULL};
		gen_frame_begin(&G, 32, CH_MONO, SS_STREAMINFO, fn++);
		gen_sub_fixed(&G, 32, 32, 1, WARM, R2RES, &rc2, 0);
		gen_frame_end(&G);

		/* R3: Rice, mixed params with escape partitions (bps 31 and 0) */
		static const int32_t R3RES[31] = {
		    3, -5, 2, -1, 0, 1, -2, /* p0: rice param 4 */
		    -1073741824, 1073741823, -1, 0, 1, 536870911, -536870912, 7, /* p1: escape 31 */
		    -3, 2, -1, 0, 5, -8, 1, -1, /* p2: rice param 0 */
		    0, 0, 0, 0, 0, 0, 0, 0 /* p3: escape bps 0 */
		};
		static const uint8_t R3P[4] = {4, 15, 0, 15};
		static const uint8_t R3E[4] = {0, 31, 0, 0};
		rice_cfg_t rc3 = {0, 2, R3P, R3E};
		gen_frame_begin(&G, 32, CH_MONO, SS_STREAMINFO, fn++);
		gen_sub_fixed(&G, 32, 32, 1, WARM, R3RES, &rc3, 0);
		gen_frame_end(&G);

		/* R4: Rice2 escape partition, raw 31-bit residuals */
		static const int32_t R4RES[31] = {
		    -1073741824, 1073741823, -1, 0, 1, 536870911, -536870912,
		    123456789, -123456789, 2, -2, 100, -100, 0,
		    1, -1, 2, -2, 3, -3, 4, -4, 5, -5, 6, -6, 7, -7, 8, -8, 9
		};
		static const uint8_t R4P[1] = {31};
		static const uint8_t R4E[1] = {31};
		rice_cfg_t rc4e = {1, 0, R4P, R4E};
		gen_frame_begin(&G, 32, CH_MONO, SS_STREAMINFO, fn++);
		gen_sub_fixed(&G, 32, 32, 1, WARM, R4RES, &rc4e, 0);
		gen_frame_end(&G);

		/* R5: Rice parameter 0 (pure unary) */
		static const int32_t R5RES[31] = {
		    -3, 2, -1, 0, 5, -8, 1, -1, 0, 0, 7, -9, 3, -3, 2,
		    -2, 1, -1, 0, 4, -6, 2, -1, 0, 0, 1, -1, 2, -2, 1, 0
		};
		static const uint8_t R5P[1] = {0};
		rice_cfg_t rc5 = {0, 0, R5P, NULL};
		gen_frame_begin(&G, 32, CH_MONO, SS_STREAMINFO, fn++);
		gen_sub_fixed(&G, 32, 32, 1, WARM, R5RES, &rc5, 0);
		gen_frame_end(&G);

		size_t k = 0;
		int64_t out[32];
		ref_predict(FIXED_COEFS[1], 1, 0, WARM, R1RES, 31, out);
		for (int i = 0; i < 32; i++) {
			exp[k++] = (int32_t)out[i];
		}
		ref_predict(FIXED_COEFS[1], 1, 0, WARM, R2RES, 31, out);
		for (int i = 0; i < 32; i++) {
			exp[k++] = (int32_t)out[i];
		}
		ref_predict(FIXED_COEFS[1], 1, 0, WARM, R3RES, 31, out);
		for (int i = 0; i < 32; i++) {
			exp[k++] = (int32_t)out[i];
		}
		ref_predict(FIXED_COEFS[1], 1, 0, WARM, R4RES, 31, out);
		for (int i = 0; i < 32; i++) {
			exp[k++] = (int32_t)out[i];
		}
		ref_predict(FIXED_COEFS[1], 1, 0, WARM, R5RES, 31, out);
		for (int i = 0; i < 32; i++) {
			exp[k++] = (int32_t)out[i];
		}
		run_case("S7 rice and escape 32-bit", GEN_BUF, G.w.len, 17, 13, exp, k,
		         32, 1, 32, 1);
	}

	/* S8: narrow bit-depth controls, left-aligned output */
	{
		/* 8-bit mono: constant -128 -> INT32_MIN; fixed order 1 */
		gen_reset();
		gen_streaminfo(&G.w, 8, 8, 44100, 1, 8, 16);
		gen_frame_begin(&G, 8, CH_MONO, SS_8BIT, 0);
		gen_sub_constant(&G, 8, -128, 0);
		gen_frame_end(&G);
		static const int64_t W8[1] = {-128};
		static const int32_t R8[7] = {1, 1, -2, 0, 0, 1, -1};
		static const uint8_t P2[1] = {2};
		rice_cfg_t rc2 = {0, 0, P2, NULL};
		gen_frame_begin(&G, 8, CH_MONO, SS_STREAMINFO, 1);
		gen_sub_fixed(&G, 8, 8, 1, W8, R8, &rc2, 0);
		gen_frame_end(&G);
		size_t k = 0;
		for (int i = 0; i < 8; i++) {
			exp[k++] = align32(-128, 8);
		}
		int64_t out[8];
		ref_predict(FIXED_COEFS[1], 1, 0, W8, R8, 7, out);
		for (int i = 0; i < 8; i++) {
			exp[k++] = align32(out[i], 8);
		}
		run_case("S8a 8-bit control", GEN_BUF, G.w.len, 4, 3, exp, k, 8, 1, 8,
		         1);

		/* 12-bit mono: verbatim */
		gen_reset();
		gen_streaminfo(&G.w, 4, 4, 44100, 1, 12, 4);
		static const int64_t V12[4] = {2047, -2048, 291, -1};
		gen_frame_begin(&G, 4, CH_MONO, SS_12BIT, 0);
		gen_sub_verbatim(&G, 12, V12, 4, 0);
		gen_frame_end(&G);
		for (int i = 0; i < 4; i++) {
			exp[i] = align32(V12[i], 12);
		}
		run_case("S8b 12-bit control", GEN_BUF, G.w.len, 6, 2, exp, 4, 12, 1,
		         4, 1);

		/* 20-bit mono: constant -524288 -> INT32_MIN */
		gen_reset();
		gen_streaminfo(&G.w, 4, 4, 44100, 1, 20, 4);
		gen_frame_begin(&G, 4, CH_MONO, SS_20BIT, 0);
		gen_sub_constant(&G, 20, -524288, 0);
		gen_frame_end(&G);
		for (int i = 0; i < 4; i++) {
			exp[i] = align32(-524288, 20);
		}
		run_case("S8c 20-bit control", GEN_BUF, G.w.len, 6, 2, exp, 4, 20, 1,
		         4, 1);

		/* 16-bit stereo: fixed order 2 + LPC order 1 */
		gen_reset();
		gen_streaminfo(&G.w, 16, 16, 44100, 2, 16, 32);
		static const int64_t F2L[2] = {32767, 32766};
		static const int64_t F2R[2] = {-32768, -32767};
		static const int32_t Z14[14] = {0};
		static const uint8_t P4[1] = {4};
		rice_cfg_t rc4 = {0, 0, P4, NULL};
		gen_frame_begin(&G, 16, CH_STEREO, SS_STREAMINFO, 0);
		gen_sub_fixed(&G, 16, 16, 2, F2L, Z14, &rc4, 0);
		gen_sub_fixed(&G, 16, 16, 2, F2R, Z14, &rc4, 0);
		gen_frame_end(&G);
		static const int64_t L1W[1] = {1000};
		static const int64_t R1W[1] = {-1000};
		static const int32_t L1R[15] = {1, -2, 3, -4, 5, -6, 7, -8,
		                                9, -10, 11, -12, 13, -14, 15};
		static const int32_t R1R[15] = {-1, 2, -3, 4, -5, 6, -7, 8,
		                                -9, 10, -11, 12, -13, 14, -15};
		static const int32_t C1[1] = {1};
		gen_frame_begin(&G, 16, CH_STEREO, SS_16BIT, 1);
		gen_sub_lpc(&G, 16, 16, 1, 2, 0, C1, L1W, L1R, &rc4, 0);
		gen_sub_lpc(&G, 16, 16, 1, 2, 0, C1, R1W, R1R, &rc4, 0);
		gen_frame_end(&G);
		k = 0;
		int64_t ol[16], orr[16];
		ref_predict(FIXED_COEFS[2], 2, 0, F2L, Z14, 14, ol);
		ref_predict(FIXED_COEFS[2], 2, 0, F2R, Z14, 14, orr);
		for (int i = 0; i < 16; i++) {
			exp[k++] = align32(ol[i], 16);
			exp[k++] = align32(orr[i], 16);
		}
		ref_predict(C1, 1, 0, L1W, L1R, 15, ol);
		ref_predict(C1, 1, 0, R1W, R1R, 15, orr);
		for (int i = 0; i < 16; i++) {
			exp[k++] = align32(ol[i], 16);
			exp[k++] = align32(orr[i], 16);
		}
		run_case("S8d 16-bit stereo control", GEN_BUF, G.w.len, 10, 8, exp, k,
		         16, 2, 16, 2);
	}
}

/* ------------------------------------------------------------------ */
/* Group "stereo": 33-bit side channels and mid/side parity            */
/* ------------------------------------------------------------------ */

static void group_stereo(void) {
	int32_t exp[512];

	/* T1: left-side 32-bit; side channel spans the full 33-bit range.
	   Frame A verbatim, frame B fixed order 1 on both channels. */
	{
		int64_t L[16], R[16], side[16];
		for (int n = 0; n < 16; n++) {
			L[n] = (int64_t)INT32_MIN + n;
			R[n] = (int64_t)INT32_MAX - n;
			side[n] = enc_side(L[n], R[n]); /* -4294967295 + 2n */
		}
		static const int32_t ONE[15] = {1, 1, 1, 1, 1, 1, 1, 1,
		                                1, 1, 1, 1, 1, 1, 1};
		static const int32_t TWO[15] = {2, 2, 2, 2, 2, 2, 2, 2,
		                                2, 2, 2, 2, 2, 2, 2};
		static const uint8_t P4[1] = {4};
		rice_cfg_t rc4 = {0, 0, P4, NULL};
		gen_reset();
		gen_streaminfo(&G.w, 8, 8, 44100, 2, 32, 16);
		gen_frame_begin(&G, 8, CH_LEFT_SIDE, SS_STREAMINFO, 0);
		gen_sub_verbatim(&G, 32, L, 8, 0);
		gen_sub_verbatim(&G, 33, side, 8, 0);
		gen_frame_end(&G);
		gen_frame_begin(&G, 8, CH_LEFT_SIDE, SS_STREAMINFO, 1);
		gen_sub_fixed(&G, 32, 8, 1, &L[8], ONE, &rc4, 0);
		gen_sub_fixed(&G, 33, 8, 1, &side[8], TWO, &rc4, 0);
		gen_frame_end(&G);
		for (int n = 0; n < 16; n++) {
			exp[2 * n] = (int32_t)L[n];
			exp[2 * n + 1] = (int32_t)R[n];
		}
		run_case("T1 left-side 33-bit", GEN_BUF, G.w.len, 7, 5, exp, 32, 32, 2,
		         8, 2);
	}

	/* T2: right-side 32-bit, mirrored extremes */
	{
		int64_t L[16], R[16], side[16];
		for (int n = 0; n < 16; n++) {
			L[n] = (int64_t)INT32_MAX - n;
			R[n] = (int64_t)INT32_MIN + n;
			side[n] = enc_side(L[n], R[n]); /* 4294967295 - 2n */
		}
		static const int32_t ONE[15] = {1, 1, 1, 1, 1, 1, 1, 1,
		                                1, 1, 1, 1, 1, 1, 1};
		static const int32_t M2[15] = {-2, -2, -2, -2, -2, -2, -2, -2,
		                               -2, -2, -2, -2, -2, -2, -2};
		static const uint8_t P4[1] = {4};
		rice_cfg_t rc4 = {0, 0, P4, NULL};
		gen_reset();
		gen_streaminfo(&G.w, 8, 8, 44100, 2, 32, 16);
		gen_frame_begin(&G, 8, CH_RIGHT_SIDE, SS_STREAMINFO, 0);
		gen_sub_verbatim(&G, 33, side, 8, 0);
		gen_sub_verbatim(&G, 32, R, 8, 0);
		gen_frame_end(&G);
		gen_frame_begin(&G, 8, CH_RIGHT_SIDE, SS_STREAMINFO, 1);
		gen_sub_fixed(&G, 33, 8, 1, &side[8], M2, &rc4, 0);
		gen_sub_fixed(&G, 32, 8, 1, &R[8], ONE, &rc4, 0);
		gen_frame_end(&G);
		for (int n = 0; n < 16; n++) {
			exp[2 * n] = (int32_t)L[n];
			exp[2 * n + 1] = (int32_t)R[n];
		}
		run_case("T2 right-side 33-bit", GEN_BUF, G.w.len, 7, 5, exp, 32, 32,
		         2, 8, 2);
	}

	/* T3: mid-side 32-bit; extremes, negative odd mids, odd/even sides */
	{
		static const int64_t PAIRS[16][2] = {
		    {INT32_MAX, INT32_MIN},     /* mid -1 (odd), side +4294967295 */
		    {INT32_MIN, INT32_MAX},     /* mid -1 (odd), side -4294967295 */
		    {0, -5},                    /* mid -3 (odd), side 5 */
		    {-5, 0},                    /* mid -3 (odd), side -5 */
		    {INT32_MAX, INT32_MAX},     /* mid MAX, side 0 */
		    {INT32_MIN, INT32_MIN},     /* mid MIN, side 0 */
		    {1, 0},                     /* mid 0, side 1 */
		    {0, 1},                     /* mid 0, side -1 */
		    {-1, 0},                    /* mid -1, side -1 */
		    {0, -1},                    /* mid -1, side 1 */
		    {2147483646, -2147483647},  /* mid -1, side 4294967293 */
		    {-2, 3},                    /* mid 0, side -5 */
		    {1073741824, -1073741824},  /* mid 0, side 2147483648 */
		    {-1073741824, 1073741824},  /* mid 0, side -2147483648 */
		    {5, -2},                    /* mid 1, side 7 */
		    {-3, 4},                    /* mid 0, side -7 */
		};
		int64_t mid[16], side[16];
		for (int n = 0; n < 16; n++) {
			mid[n] = enc_mid(PAIRS[n][0], PAIRS[n][1]);
			side[n] = enc_side(PAIRS[n][0], PAIRS[n][1]);
		}
		gen_reset();
		gen_streaminfo(&G.w, 16, 16, 44100, 2, 32, 24);
		gen_frame_begin(&G, 16, CH_MID_SIDE, SS_STREAMINFO, 0);
		gen_sub_verbatim(&G, 32, mid, 16, 0);
		gen_sub_verbatim(&G, 33, side, 16, 0);
		gen_frame_end(&G);
		/* frame B: constant odd negative mid, fixed order 1 odd sides */
		static const int64_t MIDB[1] = {-5};
		static const int64_t SIDEB_W[1] = {-1};
		static const int32_t SIDEB_R[7] = {-2, -2, -2, -2, -2, -2, -2};
		static const uint8_t P4[1] = {4};
		rice_cfg_t rc4 = {0, 0, P4, NULL};
		gen_frame_begin(&G, 8, CH_MID_SIDE, SS_STREAMINFO, 1);
		gen_sub_constant(&G, 32, -5, 0);
		gen_sub_fixed(&G, 33, 8, 1, SIDEB_W, SIDEB_R, &rc4, 0);
		gen_frame_end(&G);
		size_t k = 0;
		for (int n = 0; n < 16; n++) {
			exp[k++] = (int32_t)PAIRS[n][0];
			exp[k++] = (int32_t)PAIRS[n][1];
		}
		int64_t sb[8];
		sb[0] = SIDEB_W[0];
		for (int n = 1; n < 8; n++) {
			sb[n] = sb[n - 1] + SIDEB_R[n - 1]; /* -1, -3, -5, ... */
		}
		for (int n = 0; n < 8; n++) {
			int64_t m2 = MIDB[0] * 2 + (sb[n] & 1);
			exp[k++] = (int32_t)floor_shift(m2 + sb[n], 1);
			exp[k++] = (int32_t)floor_shift(m2 - sb[n], 1);
		}
		run_case("T3 mid-side 32-bit parity", GEN_BUF, G.w.len, 9, 7, exp, k,
		         32, 2, 16, 2);
	}

	/* T4: narrow side-channel controls: 16-bit mid-side, 24-bit left-side */
	{
		static const int64_t P16[8][2] = {
		    {32767, -32768}, {-32768, 32767}, {1000, -1000}, {0, 0},
		    {-1, 1},         {1, -1},         {32767, 32767}, {-32768, -32768},
		};
		int64_t mid[8], side[8];
		for (int n = 0; n < 8; n++) {
			mid[n] = enc_mid(P16[n][0], P16[n][1]);
			side[n] = enc_side(P16[n][0], P16[n][1]); /* 17-bit */
		}
		gen_reset();
		gen_streaminfo(&G.w, 8, 8, 44100, 2, 16, 8);
		gen_frame_begin(&G, 8, CH_MID_SIDE, SS_STREAMINFO, 0);
		gen_sub_verbatim(&G, 16, mid, 8, 0);
		gen_sub_verbatim(&G, 17, side, 8, 0);
		gen_frame_end(&G);
		size_t k = 0;
		for (int n = 0; n < 8; n++) {
			exp[k++] = align32(P16[n][0], 16);
			exp[k++] = align32(P16[n][1], 16);
		}
		run_case("T4a 16-bit mid-side", GEN_BUF, G.w.len, 5, 3, exp, k, 16, 2,
		         8, 2);

		/* 24-bit left-side */
		int64_t L[8], R[8], s24[8];
		for (int n = 0; n < 8; n++) {
			L[n] = -8388608 + n;
			R[n] = 8388607 - n;
			s24[n] = enc_side(L[n], R[n]); /* 25-bit */
		}
		gen_reset();
		gen_streaminfo(&G.w, 8, 8, 44100, 2, 24, 8);
		gen_frame_begin(&G, 8, CH_LEFT_SIDE, SS_STREAMINFO, 0);
		gen_sub_verbatim(&G, 24, L, 8, 0);
		gen_sub_verbatim(&G, 25, s24, 8, 0);
		gen_frame_end(&G);
		k = 0;
		for (int n = 0; n < 8; n++) {
			exp[k++] = align32(L[n], 24);
			exp[k++] = align32(R[n], 24);
		}
		run_case("T4b 24-bit left-side", GEN_BUF, G.w.len, 5, 3, exp, k, 24, 2,
		         8, 2);
	}
}

/* ------------------------------------------------------------------ */
/* Group "stream": fragmented I/O, resync, memory, reset, old depths   */
/* ------------------------------------------------------------------ */

/* Builds a 32-bit mid-side stream with two frames; used by several tests. */
static size_t build_midside32(uint8_t *buf, size_t cap, int32_t *exp,
                              size_t n_exp) {
	int64_t mid[16], side[16], L[16], R[16];
	for (int n = 0; n < 16; n++) {
		mid[n] = -1000 - n;
		side[n] = -2147483648LL - n; /* 33-bit, below INT32_MIN */
	}
	ref_mid_side(mid, side, 16, L, R);
	static const int32_t M1[15] = {-1, -1, -1, -1, -1, -1, -1, -1,
	                               -1, -1, -1, -1, -1, -1, -1};
	static const uint8_t P4[1] = {4};
	rice_cfg_t rc4 = {0, 0, P4, NULL};
	gen_reset();
	gen_streaminfo(&G.w, 16, 16, 44100, 2, 32, 16);
	gen_frame_begin(&G, 8, CH_MID_SIDE, SS_STREAMINFO, 0);
	gen_sub_fixed(&G, 32, 8, 1, &mid[0], M1, &rc4, 0);
	gen_sub_fixed(&G, 33, 8, 1, &side[0], M1, &rc4, 0);
	gen_frame_end(&G);
	gen_frame_begin(&G, 8, CH_MID_SIDE, SS_STREAMINFO, 1);
	gen_sub_verbatim(&G, 32, &mid[8], 8, 0);
	gen_sub_verbatim(&G, 33, &side[8], 8, 0);
	gen_frame_end(&G);
	task_check(G.w.len <= cap, "stream buffer too small");
	for (int n = 0; n < 16; n++) {
		exp[2 * n] = (int32_t)L[n];
		exp[2 * n + 1] = (int32_t)R[n];
	}
	task_check(n_exp >= 32, "expected buffer too small");
	memcpy(buf, GEN_BUF, G.w.len);
	return G.w.len;
}

static void group_stream(void) {
	int32_t exp[256];

	/* R1: fragmented input (1 byte at a time) and short output (one
	   scalar at a time, and odd chunk sizes splitting channel pairs) */
	{
		uint8_t stream[4096];
		size_t len = build_midside32(stream, sizeof(stream), exp, 256);
		run_case("R1a trickle 1/1", stream, len, 1, 1, exp, 32, 32, 2, 16, 2);
		run_case("R1b odd chunks 3/5", stream, len, 3, 5, exp, 32, 32, 2, 16,
		         2);
		run_case("R1c whole stream", stream, len, (unsigned)len, 32, exp, 32,
		         32, 2, 16, 2);
	}

	/* R2: a frame with a corrupted CRC16 must not publish audio; the
	   following valid frame must still be decoded */
	{
		static const int64_t LA[8] = {1, 2, 3, 4, 5, 6, 7, 8};
		static const int64_t RA[8] = {-1, -2, -3, -4, -5, -6, -7, -8};
		static const int64_t LB[8] = {100, 200, 300, 400, 500, 600, 700, 800};
		static const int64_t RB[8] = {-100, -200, -300, -400,
		                              -500, -600, -700, -800};
		static const int64_t LC[8] = {1000, 2000, 3000, 4000,
		                              5000, 6000, 7000, 8000};
		static const int64_t RC[8] = {-1000, -2000, -3000, -4000,
		                              -5000, -6000, -7000, -8000};
		gen_reset();
		gen_streaminfo(&G.w, 8, 8, 44100, 2, 32, 24);
		gen_frame_begin(&G, 8, CH_STEREO, SS_STREAMINFO, 0);
		gen_sub_verbatim(&G, 32, LA, 8, 0);
		gen_sub_verbatim(&G, 32, RA, 8, 0);
		gen_frame_end(&G);
		const size_t bad_start = G.w.len;
		gen_frame_begin(&G, 8, CH_STEREO, SS_STREAMINFO, 1);
		gen_sub_verbatim(&G, 32, LB, 8, 0);
		gen_sub_verbatim(&G, 32, RB, 8, 0);
		gen_frame_end(&G);
		const size_t bad_end = G.w.len;
		gen_frame_begin(&G, 8, CH_STEREO, SS_STREAMINFO, 2);
		gen_sub_verbatim(&G, 32, LC, 8, 0);
		gen_sub_verbatim(&G, 32, RC, 8, 0);
		gen_frame_end(&G);
		/* corrupt the CRC16 of frame B (last two bytes of the frame) */
		GEN_BUF[bad_end - 2] ^= 0x01;
		(void)bad_start;
		size_t k = 0;
		for (int n = 0; n < 8; n++) {
			exp[k++] = (int32_t)LA[n];
			exp[k++] = (int32_t)RA[n];
		}
		for (int n = 0; n < 8; n++) {
			exp[k++] = (int32_t)LC[n];
			exp[k++] = (int32_t)RC[n];
		}
		run_case("R2 bad CRC16 resync", GEN_BUF, G.w.len, 3, 5, exp, k, 32, 2,
		         8, 2);
	}

	/* R3: a frame with a negative LPC shift (format-forbidden) must be
	   rejected; the following valid frame must still be decoded */
	{
		static const int64_t W5[1] = {5};
		static const int32_t R5[7] = {1, -1, 2, -2, 0, 0, 0};
		static const uint8_t P4[1] = {4};
		rice_cfg_t rc4 = {0, 0, P4, NULL};
		gen_reset();
		gen_streaminfo(&G.w, 8, 8, 44100, 1, 32, 16);
		gen_frame_begin(&G, 8, CH_MONO, SS_STREAMINFO, 0);
		gen_sub_constant(&G, 32, 777, 0);
		gen_frame_end(&G);
		/* bad frame: valid header (CRC8 ok), LPC order 1, shift = -1 */
		gen_frame_begin(&G, 8, CH_MONO, SS_32BIT, 1);
		bw_bits(&G.w, 0, 1);    /* padding */
		bw_bits(&G.w, 0x20, 6); /* LPC, order 1 */
		bw_bits(&G.w, 0, 1);    /* no wasted bits */
		bw_bits(&G.w, 1, 32);   /* warm-up sample */
		bw_bits(&G.w, 3, 4);    /* precision field (4 bits) */
		bw_bits(&G.w, 0x1F, 5); /* shift = 0b11111 -> -1, forbidden */
		bw_bits(&G.w, 0, 7);    /* padding to byte boundary */
		/* no CRC16; the decoder must resync to the next frame */
		gen_frame_begin(&G, 8, CH_MONO, SS_STREAMINFO, 2);
		gen_sub_fixed(&G, 32, 8, 1, W5, R5, &rc4, 0);
		gen_frame_end(&G);
		size_t k = 0;
		int64_t out[8];
		for (int i = 0; i < 8; i++) {
			exp[k++] = 777;
		}
		ref_predict(FIXED_COEFS[1], 1, 0, W5, R5, 7, out);
		for (int i = 0; i < 8; i++) {
			exp[k++] = (int32_t)out[i];
		}
		run_case("R3 negative LPC shift resync", GEN_BUF, G.w.len, 2, 4, exp,
		         k, 32, 1, 8, 1);
	}

	/* R4: memory boundaries; canaries around the decoder instance and
	   fx_flac_size consistency */
	{
		task_check(fx_flac_size(0, 2) == 0, "size with zero block size");
		task_check(fx_flac_size(64, 0) == 0, "size with zero channels");
		task_check(fx_flac_size(64, 9) == 0, "size with too many channels");
		task_check(fx_flac_size(64, 2) > 0, "size with valid params");
		task_check(fx_flac_size(FLAC_MAX_BLOCK_SIZE, FLAC_MAX_CHANNEL_COUNT) >
		               0,
		           "size with maximum params");
		const uint16_t mbs = 64;
		const uint8_t mch = 2;
		const uint32_t sz = fx_flac_size(mbs, mch);
		task_check(sz > 0, "fx_flac_size(64,2)");
		task_check((sz % 16U) == 0, "fx_flac_size alignment");
		const size_t guard = 32;
		uint8_t *region = malloc(sz + 2 * guard);
		task_check(region != NULL, "canary region alloc");
		memset(region, 0xA5, sz + 2 * guard);
		fx_flac_t *d = fx_flac_init(region + guard, mbs, mch);
		task_check(d != NULL, "fx_flac_init");
		/* decode a 32-bit mid-side stream with full-size frames */
		int64_t mid[64], side[64], L[64], R[64];
		int64_t mid2[64], side2[64], L2[64], R2[64];
		for (int n = 0; n < 64; n++) {
			mid[n] = 100LL * n * n * n - 1000000000LL; /* cubic */
			side[n] = -2147483648LL - n;               /* 33-bit */
			mid2[n] = 500000000 - 7 * n;
			side2[n] = 2147483648LL + n; /* 33-bit, above INT32_MAX */
		}
		ref_mid_side(mid, side, 64, L, R);
		ref_mid_side(mid2, side2, 64, L2, R2);
		static const int32_t Z60[60] = {0};
		static const int32_t M1[63] = {
		    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
		static const uint8_t P8[1] = {8};
		rice_cfg_t rc8 = {0, 0, P8, NULL};
		gen_reset();
		gen_streaminfo(&G.w, 64, 64, 44100, 2, 32, 128);
		gen_frame_begin(&G, 64, CH_MID_SIDE, SS_STREAMINFO, 0);
		gen_sub_fixed(&G, 32, 64, 4, mid, Z60, &rc8, 0);
		gen_sub_fixed(&G, 33, 64, 1, side, M1, &rc8, 0);
		gen_frame_end(&G);
		gen_frame_begin(&G, 64, CH_MID_SIDE, SS_STREAMINFO, 1);
		gen_sub_verbatim(&G, 32, mid2, 64, 0);
		gen_sub_verbatim(&G, 33, side2, 64, 0);
		gen_frame_end(&G);
		size_t k = 0;
		for (int n = 0; n < 64; n++) {
			exp[k++] = (int32_t)L[n];
			exp[k++] = (int32_t)R[n];
		}
		for (int n = 0; n < 64; n++) {
			exp[k++] = (int32_t)L2[n];
			exp[k++] = (int32_t)R2[n];
		}
		decode_all(d, GEN_BUF, G.w.len, 7, 11, exp, k, 32, 2,
		           "R4 canary stream");
		for (size_t i = 0; i < guard; i++) {
			task_check(region[i] == 0xA5, "canary before decoder touched");
			task_check(region[guard + sz + i] == 0xA5,
			           "canary after decoder touched");
		}
		free(region);
	}

	/* R5: fx_flac_reset must not retain wide-channel history */
	{
		/* stream X: 32-bit mid-side */
		int64_t mid[16], side[16], LX[16], RX[16];
		for (int n = 0; n < 16; n++) {
			mid[n] = -5 - n;
			side[n] = -1 - 2 * n;
		}
		ref_mid_side(mid, side, 16, LX, RX);
		static const int32_t M1[15] = {-1, -1, -1, -1, -1, -1, -1, -1,
		                               -1, -1, -1, -1, -1, -1, -1};
		static const int32_t M2[15] = {-2, -2, -2, -2, -2, -2, -2, -2,
		                               -2, -2, -2, -2, -2, -2, -2};
		static const uint8_t P4[1] = {4};
		rice_cfg_t rc4 = {0, 0, P4, NULL};
		gen_reset();
		gen_streaminfo(&G.w, 16, 16, 44100, 2, 32, 16);
		gen_frame_begin(&G, 16, CH_MID_SIDE, SS_STREAMINFO, 0);
		gen_sub_fixed(&G, 32, 16, 1, mid, M1, &rc4, 0);
		gen_sub_fixed(&G, 33, 16, 1, side, M2, &rc4, 0);
		gen_frame_end(&G);
		size_t len_x = G.w.len;
		uint8_t stream_x[1024];
		memcpy(stream_x, GEN_BUF, len_x);
		int32_t exp_x[32];
		for (int n = 0; n < 16; n++) {
			exp_x[2 * n] = (int32_t)LX[n];
			exp_x[2 * n + 1] = (int32_t)RX[n];
		}

		/* stream Y: 24-bit right-side */
		int64_t LY[8], RY[8], s24[8];
		for (int n = 0; n < 8; n++) {
			LY[n] = 8388607 - n;
			RY[n] = -8388608 + n;
			s24[n] = enc_side(LY[n], RY[n]);
		}
		gen_reset();
		gen_streaminfo(&G.w, 8, 8, 44100, 2, 24, 8);
		gen_frame_begin(&G, 8, CH_RIGHT_SIDE, SS_STREAMINFO, 0);
		gen_sub_verbatim(&G, 25, s24, 8, 0);
		gen_sub_verbatim(&G, 24, RY, 8, 0);
		gen_frame_end(&G);
		size_t len_y = G.w.len;
		uint8_t stream_y[1024];
		memcpy(stream_y, GEN_BUF, len_y);
		int32_t exp_y[16];
		for (int n = 0; n < 8; n++) {
			exp_y[2 * n] = align32(LY[n], 24);
			exp_y[2 * n + 1] = align32(RY[n], 24);
		}

		fx_flac_t *d = FX_FLAC_ALLOC(16, 2);
		task_check(d != NULL, "reset test alloc");
		decode_all(d, stream_x, len_x, 3, 3, exp_x, 32, 32, 2,
		           "R5 first 32-bit run");
		fx_flac_reset(d);
		decode_all(d, stream_y, len_y, 3, 3, exp_y, 16, 24, 2,
		           "R5 24-bit after reset");
		fx_flac_reset(d);
		decode_all(d, stream_x, len_x, 3, 3, exp_x, 32, 32, 2,
		           "R5 32-bit after reset");
		free(d);
	}

	/* R6: 16-bit stream, byte-by-byte input, left-aligned output */
	{
		static const int64_t F2L[2] = {32767, 32766};
		static const int64_t F2R[2] = {-32768, -32767};
		static const int32_t Z14[14] = {0};
		static const uint8_t P4[1] = {4};
		rice_cfg_t rc4 = {0, 0, P4, NULL};
		gen_reset();
		gen_streaminfo(&G.w, 16, 16, 44100, 2, 16, 16);
		gen_frame_begin(&G, 16, CH_STEREO, SS_STREAMINFO, 0);
		gen_sub_fixed(&G, 16, 16, 2, F2L, Z14, &rc4, 0);
		gen_sub_fixed(&G, 16, 16, 2, F2R, Z14, &rc4, 0);
		gen_frame_end(&G);
		size_t k = 0;
		int64_t ol[16], orr[16];
		ref_predict(FIXED_COEFS[2], 2, 0, F2L, Z14, 14, ol);
		ref_predict(FIXED_COEFS[2], 2, 0, F2R, Z14, 14, orr);
		for (int i = 0; i < 16; i++) {
			exp[k++] = align32(ol[i], 16);
			exp[k++] = align32(orr[i], 16);
		}
		run_case("R6 16-bit trickle", GEN_BUF, G.w.len, 1, 1, exp, k, 16, 2,
		         16, 2);
	}

	/* R7: residual value -2^31 (outside the RFC 9639 residual range) must
	   be rejected; the following valid frame must still be decoded */
	{
		static const int32_t BADRES[8] = {INT32_MIN, 0, 0, 0, 0, 0, 0, 0};
		static const uint8_t P30[1] = {30};
		rice_cfg_t rc30 = {1, 0, P30, NULL};
		gen_reset();
		gen_streaminfo(&G.w, 8, 8, 44100, 1, 32, 16);
		gen_frame_begin(&G, 8, CH_MONO, SS_STREAMINFO, 0);
		gen_sub_constant(&G, 32, 42, 0);
		gen_frame_end(&G);
		gen_frame_begin(&G, 8, CH_MONO, SS_STREAMINFO, 1);
		gen_sub_fixed(&G, 32, 8, 0, NULL, BADRES, &rc30, 0);
		gen_frame_end(&G);
		gen_frame_begin(&G, 8, CH_MONO, SS_STREAMINFO, 2);
		gen_sub_constant(&G, 32, -42, 0);
		gen_frame_end(&G);
		size_t k = 0;
		for (int i = 0; i < 8; i++) {
			exp[k++] = 42;
		}
		for (int i = 0; i < 8; i++) {
			exp[k++] = -42;
		}
		run_case("R7 illegal residual resync", GEN_BUF, G.w.len, 5, 3, exp, k,
		         32, 1, 8, 1);
	}
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int task_cases(const char *group) {
	if (strcmp(group, "samples") == 0) {
		group_samples();
		return 1;
	}
	if (strcmp(group, "stereo") == 0) {
		group_stereo();
		return 1;
	}
	if (strcmp(group, "stream") == 0) {
		group_stream();
		return 1;
	}
	return 0;
}
