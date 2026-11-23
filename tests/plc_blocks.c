/*
 * tests/plc_blocks.c -- tests for the gsm_decode_plc() block entry.
 *
 * The expected concealed residuals, long-term history and PCM are
 * derived here from an independent wide-integer (64 bit) recurrence.
 * The original library's short-term synthesis filter and deemphasis
 * are reused only to bound the scope of the reference; none of the
 * production PLC functions are used to compute an expected value.
 *
 * This file defines no main(): task_support/driver.c supplies it.
 */

#include "gsm.h"
#include "private.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "task_support/check.h"

#define SENTINEL	((gsm_signal)0x5A5A)
#define MAXF		12		/* frames used by the longest stream */

/* ------------------------------------------------------------------ */
/* Independent fixed-point helpers (wide integers, explicit rounding)  */
/* ------------------------------------------------------------------ */

static int64_t floor_div(int64_t a, int64_t b)
{
	int64_t q = a / b;
	int64_t r = a % b;
	if (r != 0 && ((a < 0) != (b < 0))) q--;	/* b is always > 0 */
	return q;
}

static int16_t sat16(int64_t v)
{
	if (v > 32767)  return 32767;
	if (v < -32768) return -32768;
	return (int16_t)v;
}

/* Independent equivalent of GSM_MULT_R, including the MIN_WORD guard. */
static int16_t mult_r(int16_t a, int16_t b)
{
	if (a == -32768 && b == -32768) return 32767;
	return (int16_t)floor_div((int64_t)a * b + 16384, 32768);
}

static int16_t add_sat(int32_t a, int32_t b)
{
	int32_t s = a + b;
	if (s > 32767)  return 32767;
	if (s < -32768) return -32768;
	return (int16_t)s;
}

/* q15(x, g) = floor((x*g + 16384) / 32768); g is non-negative here. */
static int16_t q15(int16_t x, int16_t g)
{
	return (int16_t)floor_div((int64_t)x * g + 16384, 32768);
}

static const int16_t gain_table[5] = { 29491, 26214, 19661, 13107, 6554 };

/* Independent RPE inverse quantization + grid positioning. */
static void ref_rpe_decode(int16_t xmaxc, int16_t Mc,
	const int16_t * xMc, int16_t erp[40])
{
	int exp, mant, i;
	int16_t xMp[13];

	exp = 0;
	if (xmaxc > 15) exp = (xmaxc >> 3) - 1;
	mant = xmaxc - (exp << 3);
	if (mant == 0) {
		exp = -4;
		mant = 7;
	} else {
		while (mant <= 7) {
			mant = (mant << 1) | 1;
			exp--;
		}
		mant -= 8;
	}

	{
		int16_t temp1 = gsm_FAC[mant];
		int temp2 = 6 - exp;
		int32_t temp3 = (int32_t)1 << (temp2 - 1);

		for (i = 0; i < 13; i++) {
			int32_t t = (xMc[i] * 2 - 7);
			t <<= 12;
			t = mult_r(temp1, (int16_t)t);
			t = add_sat(t, temp3);
			xMp[i] = (int16_t)(t >> temp2);
		}
	}

	for (i = 0; i < 40; i++) erp[i] = 0;
	for (i = 0; i < 13; i++) erp[Mc + 3 * i] = xMp[i];
}

/* Deemphasis / truncation / upscaling, copied from decode.c. */
static void ref_postprocess(struct gsm_state * S, int16_t * s)
{
	int k;
	int16_t msr = S->msr, tmp;
	for (k = 0; k < 160; k++, s++) {
		tmp = mult_r(msr, 28180);
		msr = add_sat(*s, tmp);
		*s = (int16_t)(add_sat(msr, msr) & 0xFFF8);
	}
	S->msr = msr;
}

/* ------------------------------------------------------------------ */
/* Independent per-frame reference model                               */
/* ------------------------------------------------------------------ */

struct ref_cache {
	int16_t lar[8];
	int16_t nc[4];
	int16_t bc[4];
	int16_t e[4][40];
	int     have;
};

/* Decode a good 33-byte frame on the reference state and cache it. */
static void ref_good(struct gsm_state * R, struct ref_cache * c,
	const gsm_byte * f33, gsm_signal out[160])
{
	gsm_signal ex[76];
	int16_t wt[160];
	int16_t * drp = R->dp0 + 120;
	int j, k;

	memset(ex, 0, sizeof ex);
	if (gsm_explode((gsm)R, (gsm_byte *)f33, ex) != 0) {
		fprintf(stderr, "ref explode failed\n");
		exit(2);
	}

	for (k = 0; k < 8; k++) c->lar[k] = (int16_t)ex[k];

	for (j = 0; j < 4; j++) {
		int16_t Nc = (int16_t)ex[8 + 17 * j + 0];
		int16_t bc = (int16_t)ex[8 + 17 * j + 1];
		int16_t Mc = (int16_t)ex[8 + 17 * j + 2];
		int16_t xmaxc = (int16_t)ex[8 + 17 * j + 3];
		int16_t xmc[13];
		int16_t erp[40];

		for (k = 0; k < 13; k++)
			xmc[k] = (int16_t)ex[8 + 17 * j + 4 + k];

		ref_rpe_decode(xmaxc, Mc, xmc, erp);

		/* Stock long-term synthesis: exercises the 40..120 /
		 * nrp fallback and keeps R->dp0 / R->nrp coherent.
		 */
		Gsm_Long_Term_Synthesis_Filtering(R, Nc, bc, erp, drp);

		c->nc[j] = R->nrp;		/* effective Nc */
		c->bc[j] = bc;
		for (k = 0; k < 40; k++) {
			wt[j * 40 + k] = drp[k];
			c->e[j][k] = erp[k];
		}
	}
	c->have = 1;

	Gsm_Short_Term_Synthesis_Filter(R, c->lar, wt, out);
	ref_postprocess(R, out);
}

/* Independent wide-integer concealment of one 160-sample frame. */
static void ref_lost(struct gsm_state * R, const struct ref_cache * c,
	int n, gsm_signal out[160])
{
	int16_t wt[160];
	int16_t g = (n >= 1 && n <= 5) ? gain_table[n - 1] : 0;
	word lar[8];
	int j, k;

	for (k = 0; k < 8; k++) lar[k] = c->lar[k];

	if (!c->have) {
		int16_t zero[8];
		for (k = 0; k < 160; k++) wt[k] = 0;
		for (k = 0; k < 8; k++) zero[k] = 0;
		Gsm_Short_Term_Synthesis_Filter(R, zero, wt, out);
		ref_postprocess(R, out);
		return;
	}

	{
		int16_t * base = R->dp0 + 120;

		for (j = 0; j < 4; j++) {
			int16_t brp = gsm_QLB[c->bc[j]];
			int16_t Nr = c->nc[j];
			R->nrp = Nr;
			for (k = 0; k < 40; k++) {
				int16_t p = mult_r(brp, base[k - Nr]);
				int16_t r = sat16((int64_t)q15(c->e[j][k], g)
						+ (int64_t)q15(p, g));
				base[k] = r;
				wt[j * 40 + k] = r;
			}
			for (k = 0; k < 120; k++)
				R->dp0[k] = R->dp0[k + 40];
		}
	}

	Gsm_Short_Term_Synthesis_Filter(R, lar, wt, out);
	ref_postprocess(R, out);
}

/* ------------------------------------------------------------------ */
/* Deterministic speech + coded frame streams                          */
/* ------------------------------------------------------------------ */

struct stream {
	int n;
	gsm_signal pcm[MAXF * 160];
	gsm_byte nf[MAXF][33];		/* ordinary 33-byte frames   */
	gsm_byte wb[MAXF / 2][65];	/* WAV49 65-byte blocks      */
};

static void build_stream(struct stream * st, int n)
{
	gsm en = gsm_create();
	gsm ew = gsm_create();
	int one = 1, f, i;

	st->n = n;
	gsm_option(ew, GSM_OPT_WAV49, &one);

	for (f = 0; f < n; f++) {
		for (i = 0; i < 160; i++) {
			unsigned t = (unsigned)(f * 160 + i);
			st->pcm[f * 160 + i] =
				(gsm_signal)((int)((t * 73 + t * t * 3) % 24001)
					- 12000);
		}
		memset(st->nf[f], 0, 33);
		gsm_encode(en, st->pcm + f * 160, st->nf[f]);
	}

	for (f = 0; f + 1 < n; f += 2) {
		memset(st->wb[f / 2], 0, 65);
		gsm_encode(ew, st->pcm + f * 160,       st->wb[f / 2]);
		gsm_encode(ew, st->pcm + (f + 1) * 160, st->wb[f / 2] + 32);
	}

	gsm_destroy(en);
	gsm_destroy(ew);
}

/* Run the reference over a per-frame good/lost pattern (1 = good). */
static void run_reference(const struct stream * st, const int * good,
	gsm_signal out[MAXF][160])
{
	struct gsm_state * R = (struct gsm_state *)gsm_create();
	struct ref_cache c;
	int lost = 0, f;

	memset(&c, 0, sizeof c);
	for (f = 0; f < st->n; f++) {
		if (good[f]) {
			lost = 0;
			ref_good(R, &c, st->nf[f], out[f]);
		} else {
			lost++;
			ref_lost(R, &c, lost, out[f]);
		}
	}
	gsm_destroy((gsm)R);
}

/* Run the production normal-mode block entry over the same pattern. */
static void run_normal_plc(const struct stream * st, const int * good,
	gsm_signal out[MAXF][160])
{
	gsm s = gsm_create();
	int f;
	for (f = 0; f < st->n; f++) {
		int rc;
		if (good[f])
			rc = gsm_decode_plc(s, st->nf[f], 33, 1, out[f], 160);
		else
			rc = gsm_decode_plc(s, (const gsm_byte *)0, 0, 0,
				out[f], 160);
		if (rc != 160) { fprintf(stderr, "normal plc rc=%d\n", rc); exit(3); }
	}
	gsm_destroy(s);
}

/* Run production WAV49 block entry.  mask[pair] is the 2-bit mask. */
static void run_wav_plc(const struct stream * st, const int * mask,
	gsm_signal out[MAXF][160])
{
	gsm s = gsm_create();
	int one = 1, p;
	gsm_option(s, GSM_OPT_WAV49, &one);
	for (p = 0; p < st->n / 2; p++) {
		int rc = gsm_decode_plc(s, st->wb[p], 65, (unsigned)mask[p],
			out[2 * p], 320);
		if (rc != 320) { fprintf(stderr, "wav plc rc=%d\n", rc); exit(4); }
	}
	gsm_destroy(s);
}

/* Independent WAV49 reference: two separate consecutive-loss counters,
 * one per half frame.  Good halves are reproduced from the equivalent
 * ordinary 33-byte frame (the baseline proves normal and WAV49 decode
 * identically); lost halves use the wide-integer concealment model. */
static void run_wav_reference(const struct stream * st, const int * mask,
	gsm_signal out[MAXF][160])
{
	struct gsm_state * R = (struct gsm_state *)gsm_create();
	struct ref_cache c;
	int lost[2] = { 0, 0 };
	int p, h;

	memset(&c, 0, sizeof c);
	for (p = 0; p < st->n / 2; p++) {
		for (h = 0; h < 2; h++) {
			int f = 2 * p + h;
			if ((mask[p] >> h) & 1) {
				lost[h] = 0;
				ref_good(R, &c, st->nf[f], out[f]);
			} else {
				lost[h]++;
				ref_lost(R, &c, lost[h], out[f]);
			}
		}
	}
	gsm_destroy((gsm)R);
}

static int same_frame(const gsm_signal a[160], const gsm_signal b[160])
{
	return memcmp(a, b, 160 * sizeof(gsm_signal)) == 0;
}

/* ================================================================== */
/* Excitation group                                                    */
/* ================================================================== */

/* Recorded excitation/parameters match the independent RPE model. */
static int case_excitation_cache(void)
{
	struct stream st;
	gsm s = gsm_create();
	struct gsm_state * ps = (struct gsm_state *)s;
	gsm_signal out[160];
	gsm_signal ex[76];
	int f, j, k;

	build_stream(&st, 4);
	for (f = 0; f < 4; f++) {
		TASK_CHECK(gsm_decode_plc(s, st.nf[f], 33, 1, out, 160) == 160);
		TASK_CHECK(ps->plc_have == 1);

		memset(ex, 0, sizeof ex);
		TASK_CHECK(gsm_explode(s, st.nf[f], ex) == 0);

		for (k = 0; k < 8; k++)
			TASK_CHECK(ps->plc_lar[k] == (int16_t)ex[k]);

		for (j = 0; j < 4; j++) {
			int16_t xmc[13], erp[40];
			int16_t Nc = (int16_t)ex[8 + 17 * j + 0];
			int16_t bc = (int16_t)ex[8 + 17 * j + 1];
			int16_t Mc = (int16_t)ex[8 + 17 * j + 2];
			int16_t xmaxc = (int16_t)ex[8 + 17 * j + 3];

			for (k = 0; k < 13; k++)
				xmc[k] = (int16_t)ex[8 + 17 * j + 4 + k];
			ref_rpe_decode(xmaxc, Mc, xmc, erp);

			TASK_CHECK(ps->plc_bc[j] == bc);
			TASK_CHECK(ps->plc_nc[j] >= 40 && ps->plc_nc[j] <= 120);
			/* Effective Nc obeys the 40..120 / nrp rule. */
			if (Nc < 40 || Nc > 120)
				TASK_CHECK(ps->plc_nc[j] == ps->nrp);
			else
				TASK_CHECK(ps->plc_nc[j] == Nc);
			for (k = 0; k < 40; k++)
				TASK_CHECK(ps->plc_e[j][k] == erp[k]);
		}
	}
	gsm_destroy(s);
	return 0;
}

/* Out-of-range coded Nc falls back to nrp; effective Nc stays 40..120. */
static int case_excitation_nrp_fallback(void)
{
	struct stream st;
	gsm s = gsm_create();
	struct gsm_state * ps = (struct gsm_state *)s;
	gsm_signal out[160];
	gsm_byte bad[33];
	word nrp_before;
	int j;

	build_stream(&st, 2);
	/* Prime history, then force the first subframe Nc of a frame to 0. */
	TASK_CHECK(gsm_decode_plc(s, st.nf[0], 33, 1, out, 160) == 160);
	nrp_before = ps->nrp;
	memcpy(bad, st.nf[1], 33);
	/* Nc[0] is bits 1..7 of byte 5; bit 0 is the high bit of bc[0]. */
	bad[5] &= (gsm_byte)0x01;
	TASK_CHECK(gsm_decode_plc(s, bad, 33, 1, out, 160) == 160);
	for (j = 0; j < 4; j++)
		TASK_CHECK(ps->plc_nc[j] >= 40 && ps->plc_nc[j] <= 120);
	/* Subframe 0 Nc was out of range, so it reused the prior nrp. */
	TASK_CHECK(ps->plc_nc[0] == nrp_before);
	gsm_destroy(s);
	return 0;
}

/* Single-loss residual and long-term history match the wide model. */
static int case_excitation_single_loss(void)
{
	struct stream st;
	int good[MAXF];
	gsm_signal ref[MAXF][160], got[MAXF][160];
	gsm s = gsm_create();
	struct gsm_state * ps = (struct gsm_state *)s;
	struct gsm_state * R = (struct gsm_state *)gsm_create();
	struct ref_cache c;
	int f, k;
	int16_t * rdp;

	build_stream(&st, 4);
	for (f = 0; f < MAXF; f++) good[f] = 1;
	good[2] = 0;				/* one loss after a prefix */

	/* Reference state advanced through the two good prefix frames. */
	memset(&c, 0, sizeof c);
	ref_good(R, &c, st.nf[0], ref[0]);
	ref_good(R, &c, st.nf[1], ref[1]);
	/* Independent concealment of frame 2. */
	ref_lost(R, &c, 1, ref[2]);

	/* Production through the same prefix then the loss. */
	TASK_CHECK(gsm_decode_plc(s, st.nf[0], 33, 1, got[0], 160) == 160);
	TASK_CHECK(gsm_decode_plc(s, st.nf[1], 33, 1, got[1], 160) == 160);
	TASK_CHECK(gsm_decode_plc(s, (const gsm_byte *)0, 0, 0,
		got[2], 160) == 160);

	/* The concealed PCM and the entire long-term history agree. */
	TASK_CHECK(same_frame(ref[2], got[2]));
	rdp = R->dp0;
	for (k = 0; k < 280; k++)
		TASK_CHECK(rdp[k] == ps->dp0[k]);
	/* Loss counter advanced; the good-frame cache is untouched. */
	TASK_CHECK(ps->plc_lost[0] == 1);
	TASK_CHECK(ps->plc_have == 1);

	gsm_destroy(s);
	gsm_destroy((gsm)R);
	return 0;
}

/* Eight consecutive losses: attenuation schedule and the zero-gain
 * tail (residual zero but the filters keep ringing). */
static int case_excitation_eight_losses(void)
{
	struct stream st;
	gsm s = gsm_create();
	struct gsm_state * ps = (struct gsm_state *)s;
	struct gsm_state * R = (struct gsm_state *)gsm_create();
	struct ref_cache c;
	gsm_signal got[160], ref[160];
	int n, k, nonzero_tail, residual_zero;

	build_stream(&st, 1);
	TASK_CHECK(gsm_decode_plc(s, st.nf[0], 33, 1, got, 160) == 160);
	memset(&c, 0, sizeof c);
	ref_good(R, &c, st.nf[0], ref);

	nonzero_tail = 0;
	residual_zero = 0;
	for (n = 1; n <= 8; n++) {
		ref_lost(R, &c, n, ref);
		TASK_CHECK(gsm_decode_plc(s, (const gsm_byte *)0, 0, 0,
			got, 160) == 160);
		TASK_CHECK(same_frame(ref, got));
		TASK_CHECK(ps->plc_lost[0] == n);
		for (k = 0; k < 280; k++)
			TASK_CHECK(R->dp0[k] == ps->dp0[k]);

		if (n == 6) {
			/* First zero-gain frame: the newest 40 residuals
			 * are all zero, yet the deemphasis/synthesis tail
			 * keeps the PCM non-zero.
			 */
			residual_zero = 1;
			for (k = 240; k < 280; k++)
				if (ps->dp0[k] != 0) residual_zero = 0;
			for (k = 0; k < 160; k++)
				if (got[k] != 0) nonzero_tail = 1;
		}
	}

	TASK_CHECK(gain_table[0] == 29491 && gain_table[4] == 6554);
	TASK_CHECK(residual_zero == 1);
	TASK_CHECK(nonzero_tail == 1);

	gsm_destroy(s);
	gsm_destroy((gsm)R);
	return 0;
}

/* First frame lost with no good history: zero residual / zero LARc. */
static int case_excitation_first_loss(void)
{
	struct stream st;
	gsm s = gsm_create();
	struct gsm_state * ps = (struct gsm_state *)s;
	struct gsm_state * R = (struct gsm_state *)gsm_create();
	struct ref_cache c;
	gsm_signal got[160], ref[160];
	int k, nonzero;

	build_stream(&st, 2);
	memset(&c, 0, sizeof c);
	ref_lost(R, &c, 1, ref);

	TASK_CHECK(gsm_decode_plc(s, (const gsm_byte *)0, 0, 0,
		got, 160) == 160);
	TASK_CHECK(ps->plc_have == 0);
	TASK_CHECK(same_frame(ref, got));
	/* No excitation history, so the long-term buffer stays all zero. */
	for (k = 0; k < 280; k++) TASK_CHECK(ps->dp0[k] == 0);

	/* A good frame then establishes history (recovery begins). */
	TASK_CHECK(gsm_decode_plc(s, st.nf[0], 33, 1, got, 160) == 160);
	ref_good(R, &c, st.nf[0], ref);
	TASK_CHECK(same_frame(ref, got));
	nonzero = 0;
	for (k = 0; k < 280; k++) if (ps->dp0[k] != 0) nonzero = 1;
	TASK_CHECK(nonzero == 1);

	gsm_destroy(s);
	gsm_destroy((gsm)R);
	return 0;
}

/* ================================================================== */
/* Packing group                                                       */
/* ================================================================== */

/* No loss: the block entry is sample-identical to the legacy entry. */
static int case_packing_all_good_matches_legacy(void)
{
	struct stream st;
	gsm snew = gsm_create(), sold = gsm_create();
	gsm_signal a[160], b[160];
	int f;

	build_stream(&st, 6);
	for (f = 0; f < 6; f++) {
		TASK_CHECK(gsm_decode_plc(snew, st.nf[f], 33, 1, a, 160) == 160);
		TASK_CHECK(gsm_decode(sold, st.nf[f], b) == 0);
		TASK_CHECK(same_frame(a, b));
	}
	gsm_destroy(snew);
	gsm_destroy(sold);
	return 0;
}

/* All four WAV49 masks match an independent per-half-counter WAV
 * reference; isolated half-frame losses also equal the equivalent
 * normal-mode loss arrangement. */
static int case_packing_wav49_masks(void)
{
	struct stream st;
	gsm_signal rw[MAXF][160], refw[MAXF][160], nn[MAXF][160];
	int ngood[MAXF], wmask[MAXF / 2];
	int m, f;

	build_stream(&st, 8);

	/* Every mask applied to every block: all four masks exercised. */
	for (m = 0; m < 4; m++) {
		for (f = 0; f < 4; f++) wmask[f] = m;
		run_wav_reference(&st, wmask, refw);
		run_wav_plc(&st, wmask, rw);
		for (f = 0; f < 8; f++)
			TASK_CHECK(same_frame(refw[f], rw[f]));
	}

	/* Isolated first-half losses separated by fully good blocks:
	 * in both schemes each loss is the first (and only) loss of its
	 * counter, so the normal and WAV49 paths agree sample for sample.
	 */
	wmask[0] = 0x2; wmask[1] = 0x3; wmask[2] = 0x2; wmask[3] = 0x3;
	for (f = 0; f < 8; f++) {
		int p = f >> 1, h = f & 1;
		ngood[f] = (wmask[p] >> h) & 1;
	}
	run_normal_plc(&st, ngood, nn);
	run_wav_plc(&st, wmask, rw);
	for (f = 0; f < 8; f++)
		TASK_CHECK(same_frame(nn[f], rw[f]));

	return 0;
}

/* Independent bit-wise assembly of the shared WAV49 nibble.  With the
 * first half lost (mask 0x2) and the second half good, three decoders
 * run from identical state; only the shared byte 32 differs.  The good
 * second half must take its carry nibble from the HIGH nibble, so
 * corrupting the low nibble leaves its output unchanged while
 * corrupting the high nibble changes it. */
static int case_packing_shared_nibble(void)
{
	struct stream st;
	gsm d1 = gsm_create(), d2 = gsm_create(), d3 = gsm_create();
	int one = 1, f, changed;
	gsm_signal o1[320], o2[320], o3[320];
	gsm_byte b1[65], b2[65], b3[65];

	build_stream(&st, 2);
	gsm_option(d1, GSM_OPT_WAV49, &one);
	gsm_option(d2, GSM_OPT_WAV49, &one);
	gsm_option(d3, GSM_OPT_WAV49, &one);

	memcpy(b1, st.wb[0], 65);
	memcpy(b2, st.wb[0], 65);
	memcpy(b3, st.wb[0], 65);

	/* b2: keep the high nibble, scramble the low nibble. */
	b2[32] = (gsm_byte)((b2[32] & 0xF0) ^ 0x0A);
	/* b3: keep the low nibble, scramble the high nibble. */
	b3[32] = (gsm_byte)((b3[32] & 0x0F) ^ 0x70);

	TASK_CHECK(gsm_decode_plc(d1, b1, 65, 0x2, o1, 320) == 320);
	TASK_CHECK(gsm_decode_plc(d2, b2, 65, 0x2, o2, 320) == 320);
	TASK_CHECK(gsm_decode_plc(d3, b3, 65, 0x2, o3, 320) == 320);

	/* The concealed first half and the good second half of d1 and d2
	 * are computed from the same carry nibble, so they match fully.
	 */
	for (f = 0; f < 320; f++)
		TASK_CHECK(o1[f] == o2[f]);

	/* Scrambling the high nibble must alter the good second half. */
	changed = 0;
	for (f = 160; f < 320; f++)
		if (o1[f] != o3[f]) changed = 1;
	TASK_CHECK(changed == 1);

	gsm_destroy(d1);
	gsm_destroy(d2);
	gsm_destroy(d3);
	return 0;
}

/* Validation failures leave the output sentinel and the decoder state
 * untouched, and a following valid call still produces correct audio. */
static int case_packing_error_retry(void)
{
	struct stream st;
	gsm s = gsm_create();
	gsm ref = gsm_create();
	gsm_signal buf[320], expect[160];
	int i, rc;

	build_stream(&st, 4);
	TASK_CHECK(gsm_decode_plc(s, st.nf[0], 33, 1, buf, 160) == 160);
	gsm_decode(ref, st.nf[0], expect);

	/* mask out of range (normal mode only honors bit 0) */
	for (i = 0; i < 320; i++) buf[i] = SENTINEL;
	rc = gsm_decode_plc(s, st.nf[1], 33, 2, buf, 160);
	TASK_CHECK(rc == -1);
	for (i = 0; i < 160; i++) TASK_CHECK(buf[i] == SENTINEL);

	/* bad length on a good frame */
	rc = gsm_decode_plc(s, st.nf[1], 32, 1, buf, 160);
	TASK_CHECK(rc == -1);
	for (i = 0; i < 160; i++) TASK_CHECK(buf[i] == SENTINEL);

	/* capacity mismatch */
	rc = gsm_decode_plc(s, st.nf[1], 33, 1, buf, 159);
	TASK_CHECK(rc == -1);

	/* NULL output */
	rc = gsm_decode_plc(s, st.nf[1], 33, 1, (gsm_signal *)0, 160);
	TASK_CHECK(rc == -1);

	/* bad magic on a good normal frame */
	{
		gsm_byte bad[33];
		memcpy(bad, st.nf[1], 33);
		bad[0] ^= (gsm_byte)0xF0;
		rc = gsm_decode_plc(s, bad, 33, 1, buf, 160);
		TASK_CHECK(rc == -1);
		for (i = 0; i < 160; i++) TASK_CHECK(buf[i] == SENTINEL);
	}

	/* After all the rejected calls, valid audio still matches. */
	TASK_CHECK(gsm_decode_plc(s, st.nf[1], 33, 1, buf, 160) == 160);
	gsm_decode(ref, st.nf[1], expect);
	TASK_CHECK(same_frame(buf, expect));

	gsm_destroy(s);
	gsm_destroy(ref);
	return 0;
}

/* WAV49-specific validation: bad mask/length/capacity and a decoder
 * parked in the middle of a block are all rejected before mutation. */
static int case_packing_wav49_errors(void)
{
	struct stream st;
	gsm w = gsm_create();
	int one = 1;
	gsm_signal buf[320];
	int i, rc;
	struct gsm_state * pw;

	build_stream(&st, 4);
	gsm_option(w, GSM_OPT_WAV49, &one);
	pw = (struct gsm_state *)w;

	for (i = 0; i < 320; i++) buf[i] = SENTINEL;

	/* mask out of range */
	TASK_CHECK(gsm_decode_plc(w, st.wb[0], 65, 4, buf, 320) == -1);
	/* wrong length */
	TASK_CHECK(gsm_decode_plc(w, st.wb[0], 64, 3, buf, 320) == -1);
	/* wrong capacity */
	TASK_CHECK(gsm_decode_plc(w, st.wb[0], 65, 3, buf, 319) == -1);
	for (i = 0; i < 320; i++) TASK_CHECK(buf[i] == SENTINEL);
	TASK_CHECK(pw->frame_index == 0);

	/* Park the WAV49 decoder on the first half of a block. */
	TASK_CHECK(gsm_decode(w, st.wb[0], buf) == 0);
	TASK_CHECK(pw->frame_index == 1);
	for (i = 0; i < 320; i++) buf[i] = SENTINEL;
	rc = gsm_decode_plc(w, st.wb[0], 65, 3, buf, 320);
	TASK_CHECK(rc == -1);
	TASK_CHECK(pw->frame_index == 1);	/* unchanged */
	for (i = 0; i < 320; i++) TASK_CHECK(buf[i] == SENTINEL);

	/* Completing the half block via the legacy entry still works. */
	TASK_CHECK(gsm_decode(w, st.wb[0] + 32, buf + 160) == 0);
	TASK_CHECK(pw->frame_index == 0);

	gsm_destroy(w);
	return 0;
}

/* ================================================================== */
/* Recovery group                                                      */
/* ================================================================== */

/* Nonzero prefix, one loss, then two good recovery frames. */
static int case_recovery_single(void)
{
	struct stream st;
	int good[MAXF];
	gsm_signal ref[MAXF][160], got[MAXF][160];
	gsm s = gsm_create();
	struct gsm_state * ps = (struct gsm_state *)s;
	int f;

	build_stream(&st, 5);
	for (f = 0; f < MAXF; f++) good[f] = 1;
	good[2] = 0;

	run_reference(&st, good, ref);
	for (f = 0; f < 5; f++) {
		if (good[f])
			TASK_CHECK(gsm_decode_plc(s, st.nf[f], 33, 1,
				got[f], 160) == 160);
		else
			TASK_CHECK(gsm_decode_plc(s, (const gsm_byte *)0, 0, 0,
				got[f], 160) == 160);
	}
	for (f = 0; f < 5; f++) TASK_CHECK(same_frame(ref[f], got[f]));
	/* The first good frame after the loss resets the counter. */
	TASK_CHECK(ps->plc_lost[0] == 0);
	TASK_CHECK(ps->plc_have == 1);
	gsm_destroy(s);
	return 0;
}

/* Eight-in-a-row losses followed by two good recovery frames. */
static int case_recovery_long_burst(void)
{
	struct stream st;
	int good[MAXF];
	gsm_signal ref[MAXF][160], got[MAXF][160];
	gsm s = gsm_create();
	int f;

	build_stream(&st, 11);		/* 1 prefix + 8 lost + 2 recovery */
	for (f = 0; f < MAXF; f++) good[f] = 1;
	for (f = 1; f <= 8; f++) good[f] = 0;

	run_reference(&st, good, ref);
	for (f = 0; f < 11; f++) {
		if (good[f])
			TASK_CHECK(gsm_decode_plc(s, st.nf[f], 33, 1,
				got[f], 160) == 160);
		else
			TASK_CHECK(gsm_decode_plc(s, (const gsm_byte *)0, 0, 0,
				got[f], 160) == 160);
	}
	for (f = 0; f < 11; f++) TASK_CHECK(same_frame(ref[f], got[f]));
	gsm_destroy(s);
	return 0;
}

/* Two independent decoder instances driven in interleaved order must
 * not influence each other. */
static int case_recovery_two_instances(void)
{
	struct stream st;
	int gA[MAXF], gB[MAXF];
	gsm_signal rA[MAXF][160], rB[MAXF][160];
	gsm A = gsm_create(), B = gsm_create();
	int f;

	build_stream(&st, 6);
	for (f = 0; f < 6; f++) { gA[f] = 1; gB[f] = 1; }
	gA[1] = 0; gA[2] = 0;
	gB[3] = 0;

	run_reference(&st, gA, rA);
	run_reference(&st, gB, rB);

	/* Interleave calls to A and B. */
	for (f = 0; f < 6; f++) {
		gsm_signal tmp[160];
		TASK_CHECK(gsm_decode_plc(A,
			gA[f] ? st.nf[f] : (const gsm_byte *)0,
			gA[f] ? 33u : 0u, gA[f], tmp, 160) == 160);
		TASK_CHECK(same_frame(rA[f], tmp));

		TASK_CHECK(gsm_decode_plc(B,
			gB[f] ? st.nf[f] : (const gsm_byte *)0,
			gB[f] ? 33u : 0u, gB[f], tmp, 160) == 160);
		TASK_CHECK(same_frame(rB[f], tmp));
	}
	gsm_destroy(A);
	gsm_destroy(B);
	return 0;
}

/* Legacy and block entries interleaved at frame boundaries: recovered
 * audio continues from the concealed prediction/filter state. */
static int case_recovery_interleaved_entries(void)
{
	struct stream st;
	int good[MAXF];
	gsm_signal ref[MAXF][160], got[MAXF][160];
	gsm s = gsm_create();
	int f;

	build_stream(&st, 6);
	for (f = 0; f < MAXF; f++) good[f] = 1;
	good[2] = 0;
	good[3] = 0;
	run_reference(&st, good, ref);

	/* Mix the two entries deliberately. */
	TASK_CHECK(gsm_decode(s, st.nf[0], got[0]) == 0);			/* legacy */
	TASK_CHECK(gsm_decode_plc(s, st.nf[1], 33, 1, got[1], 160) == 160);
	TASK_CHECK(gsm_decode_plc(s, (const gsm_byte *)0, 0, 0,
		got[2], 160) == 160);
	TASK_CHECK(gsm_decode_plc(s, (const gsm_byte *)0, 0, 0,
		got[3], 160) == 160);
	TASK_CHECK(gsm_decode(s, st.nf[4], got[4]) == 0);			/* legacy */
	TASK_CHECK(gsm_decode_plc(s, st.nf[5], 33, 1, got[5], 160) == 160);

	for (f = 0; f < 6; f++) TASK_CHECK(same_frame(ref[f], got[f]));
	gsm_destroy(s);
	return 0;
}

/* WAV49 per-half loss counters are independent across the two halves. */
static int case_recovery_wav49_split_counters(void)
{
	struct stream st;
	gsm w = gsm_create();
	struct gsm_state * pw;
	gsm_signal got[320];
	int one = 1, p;

	build_stream(&st, 8);
	gsm_option(w, GSM_OPT_WAV49, &one);
	pw = (struct gsm_state *)w;

	for (p = 0; p < 4; p++) {
		int mask;
		if (p == 1)      mask = 0x1;	/* second half lost */
		else if (p == 2) mask = 0x2;	/* first half lost  */
		else             mask = 0x3;	/* both good        */
		TASK_CHECK(gsm_decode_plc(w, st.wb[p], 65, (unsigned)mask,
			got, 320) == 320);
	}
	/* After a good final block both per-half counters are cleared. */
	TASK_CHECK(pw->plc_lost[0] == 0 && pw->plc_lost[1] == 0);
	TASK_CHECK(pw->frame_index == 0);

	/* Two blocks that lose alternating halves advance each counter
	 * independently; a good half resets that half's own counter. */
	TASK_CHECK(gsm_decode_plc(w, st.wb[0], 65, 0x1, got, 320) == 320);
	TASK_CHECK(pw->plc_lost[0] == 0 && pw->plc_lost[1] == 1);
	TASK_CHECK(gsm_decode_plc(w, st.wb[1], 65, 0x2, got, 320) == 320);
	TASK_CHECK(pw->plc_lost[0] == 1 && pw->plc_lost[1] == 0);
	/* A fully lost block raises both counters by one. */
	TASK_CHECK(gsm_decode_plc(w, st.wb[2], 65, 0x0, got, 320) == 320);
	TASK_CHECK(pw->plc_lost[0] == 2 && pw->plc_lost[1] == 1);
	/* A fully good block clears both. */
	TASK_CHECK(gsm_decode_plc(w, st.wb[3], 65, 0x3, got, 320) == 320);
	TASK_CHECK(pw->plc_lost[0] == 0 && pw->plc_lost[1] == 0);

	gsm_destroy(w);
	return 0;
}

const struct task_case task_cases[] = {
	{ "Excitation", "cache_matches_independent_rpe",	case_excitation_cache },
	{ "Excitation", "nrp_fallback_rule",			case_excitation_nrp_fallback },
	{ "Excitation", "single_loss_residual_history",	case_excitation_single_loss },
	{ "Excitation", "eight_losses_gain_and_tail",		case_excitation_eight_losses },
	{ "Excitation", "first_frame_lost_no_history",		case_excitation_first_loss },

	{ "Packing", "all_good_equals_legacy",		case_packing_all_good_matches_legacy },
	{ "Packing", "wav49_four_masks",		case_packing_wav49_masks },
	{ "Packing", "shared_nibble_assembly",		case_packing_shared_nibble },
	{ "Packing", "error_retry_sentinel",		case_packing_error_retry },
	{ "Packing", "wav49_validation",		case_packing_wav49_errors },

	{ "Recovery", "single_loss_two_good_frames",	case_recovery_single },
	{ "Recovery", "long_burst_then_recovery",	case_recovery_long_burst },
	{ "Recovery", "two_instances_interleaved",	case_recovery_two_instances },
	{ "Recovery", "legacy_and_block_interleaved",	case_recovery_interleaved_entries },
	{ "Recovery", "wav49_split_loss_counters",	case_recovery_wav49_split_counters },
};

const size_t task_case_count =
	sizeof(task_cases) / sizeof(task_cases[0]);
