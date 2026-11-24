/*
 * Task-local tests for gsm_decode_plc().
 *
 * The reference concealment in this file uses 64-bit integers and explicit
 * floor division.  It deliberately does not call gsm_decode_plc().
 */

#include "task_support/check.h"
#include "gsm.h"
#include "private.h"

#include <string.h>

#define TEST_FRAMES 18
#define TEST_BLOCKS 6

typedef struct {
	gsm_signal pcm[TEST_FRAMES * 160];
	gsm_byte frame[TEST_FRAMES][33];
} Source;

typedef struct {
	int have;
	word lar[8];
	word Nc[4];
	word bc[4];
	word excitation[4][40];
} Good;

static const long long REF_GAIN[6] = {
	29491, 26214, 19661, 13107, 6554, 0
};

static void make_source(Source *src, unsigned seed)
{
	gsm encoder = gsm_create();
	unsigned t, f;

	memset(src, 0, sizeof(*src));
	for (t = 0; t < TEST_FRAMES * 160u; ++t) {
		unsigned phase = t % 53u;
		int triangle;
		int irregular;
		int value;

		if (phase < 27u)
			triangle = (int)phase * 55 - 715;
		else
			triangle = (int)(53u - phase) * 55 - 715;

		irregular = (int)((t * 73u + seed * 131u + t * t * 5u) %
				 9001u) - 4500;
		value = (triangle * 3 + irregular) / 4;
		if (value > 32767) value = 32767;
		if (value < -32768) value = -32768;
		src->pcm[t] = (gsm_signal)value;
	}

	for (f = 0; f < TEST_FRAMES; ++f)
		gsm_encode(encoder, src->pcm + f * 160, src->frame[f]);

	gsm_destroy(encoder);
}

static void make_wav_blocks(Source *src, gsm_byte blocks[][66])
{
	gsm encoder = gsm_create();
	int wav49 = 1;
	unsigned b;

	gsm_option(encoder, GSM_OPT_WAV49, &wav49);
	memset(blocks, 0, sizeof(gsm_byte) * TEST_BLOCKS * 66u);
	for (b = 0; b < TEST_BLOCKS; ++b) {
		gsm_encode(encoder, src->pcm + (2 * b) * 160, blocks[b]);
		gsm_encode(encoder, src->pcm + (2 * b + 1) * 160,
			   blocks[b] + 32);
	}
	gsm_destroy(encoder);
}

static int check_pcm(const gsm_signal *a, const gsm_signal *b, unsigned n)
{
	unsigned i;

	for (i = 0; i < n; ++i)
		TASK_CHECK(a[i] == b[i]);
	return 0;
}

static int check_voice_state(const gsm a, const gsm b)
{
	TASK_CHECK(memcmp(a->dp0, b->dp0, sizeof(a->dp0)) == 0);
	TASK_CHECK(memcmp(a->e, b->e, sizeof(a->e)) == 0);
	TASK_CHECK(a->z1 == b->z1);
	TASK_CHECK(a->L_z2 == b->L_z2);
	TASK_CHECK(a->mp == b->mp);
	TASK_CHECK(memcmp(a->u, b->u, sizeof(a->u)) == 0);
	TASK_CHECK(memcmp(a->LARpp, b->LARpp, sizeof(a->LARpp)) == 0);
	TASK_CHECK(a->j == b->j);
	TASK_CHECK(a->nrp == b->nrp);
	TASK_CHECK(memcmp(a->v, b->v, sizeof(a->v)) == 0);
	TASK_CHECK(a->msr == b->msr);

	TASK_CHECK(a->plc_have_good == b->plc_have_good);
	TASK_CHECK(a->plc_loss_count == b->plc_loss_count);
	if (a->plc_have_good) {
		TASK_CHECK(memcmp(a->plc_LARc, b->plc_LARc,
				  sizeof(a->plc_LARc)) == 0);
		TASK_CHECK(memcmp(a->plc_Nc, b->plc_Nc,
				  sizeof(a->plc_Nc)) == 0);
		TASK_CHECK(memcmp(a->plc_bc, b->plc_bc,
				  sizeof(a->plc_bc)) == 0);
		TASK_CHECK(memcmp(a->plc_excitation, b->plc_excitation,
				  sizeof(a->plc_excitation)) == 0);
	}
	return 0;
}

static int check_expected_cache(const gsm d, const Good *g)
{
	int j, k;

	TASK_CHECK(d->plc_have_good == 1);
	for (k = 0; k < 8; ++k)
		TASK_CHECK(d->plc_LARc[k] == g->lar[k]);
	for (j = 0; j < 4; ++j) {
		TASK_CHECK(d->plc_Nc[j] == g->Nc[j]);
		TASK_CHECK(d->plc_bc[j] == g->bc[j]);
		for (k = 0; k < 40; ++k)
			TASK_CHECK(d->plc_excitation[j][k] ==
				    g->excitation[j][k]);
	}
	return 0;
}

static void capture_good(gsm state, const gsm_byte *frame, Good *g)
{
	gsm_signal p[76];
	word nrp;
	int j, k;

	memset(p, 0, sizeof(p));
	if (gsm_explode(state, (gsm_byte *)(void *)frame, p) != 0)
		return;

	for (k = 0; k < 8; ++k)
		g->lar[k] = (word)p[k];

	nrp = state->nrp;
	for (j = 0; j < 4; ++j) {
		word xmc[13];
		word Nc = (word)p[8 + 17 * j];
		word bc = (word)p[9 + 17 * j];
		word Mc = (word)p[10 + 17 * j];
		word xmaxc = (word)p[11 + 17 * j];

		if (Nc < 40 || Nc > 120)
			Nc = nrp;
		nrp = Nc;

		g->Nc[j] = Nc;
		g->bc[j] = bc;
		for (k = 0; k < 13; ++k)
			xmc[k] = (word)p[12 + 17 * j + k];
		Gsm_RPE_Decoding(state, xmaxc, Mc, xmc, g->excitation[j]);
	}
	g->have = 1;
}

static long long ref_q15(long long x, long long gain)
{
	long long numerator = x * gain + 16384;
	long long quotient = numerator / 32768;

	if (numerator % 32768 < 0)
		--quotient;
	return quotient;
}

static word ref_sat16(long long x)
{
	if (x < -32768) return -32768;
	if (x > 32767) return 32767;
	return (word)x;
}

/* Wide-integer reference for one concealed 160-sample frame. */
static void ref_lost_frame(gsm state, const Good *good,
			   unsigned loss_number, gsm_signal *out)
{
	word lar[8];
	word wt[160];
	word *drp = state->dp0 + 120;
	long long gain;
	int j, k;

	gain = loss_number == 0 ? 0 :
		(loss_number <= 5 ? REF_GAIN[loss_number - 1] : 0);
	state->plc_loss_count = loss_number;

	for (k = 0; k < 8; ++k)
		lar[k] = good->have ? good->lar[k] : 0;

	for (j = 0; j < 4; ++j) {
		int Nc = good->have ? good->Nc[j] : 40;
		int bc = good->have ? good->bc[j] : 0;
		int Nr;
		word qlb;

		Nr = (Nc < 40 || Nc > 120) ? (int)state->nrp : Nc;
		state->nrp = (word)Nr;
		qlb = gsm_QLB[bc];

		for (k = 0; k <= 39; ++k) {
			long long history = drp[k - Nr];
			word prediction = ref_sat16(ref_q15(history, qlb));
			long long old_excitation = good->have ?
				good->excitation[j][k] : 0;
			long long residual;

			residual = ref_sat16(
				ref_q15(old_excitation, gain) +
				ref_q15(prediction, gain));
			drp[k] = (word)residual;
		}

		for (k = 0; k <= 119; ++k)
			drp[-120 + k] = drp[-80 + k];
		for (k = 0; k <= 39; ++k)
			wt[j * 40 + k] = drp[k];
	}

	Gsm_Short_Term_Synthesis_Filter(state, lar, wt, out);
	Gsm_Postprocessing(state, out);
}

/*
 * Compare one production PLC step with an independently maintained decoder.
 * streak is the consecutive loss count before the step.
 */
static int reference_step(gsm prod, gsm ref, const Source *src,
			  unsigned frame, int good,
			  unsigned *streak, Good *expected,
			  gsm_signal *prod_out, gsm_signal *ref_out)
{
	int rc;

	if (good) {
		capture_good(ref, src->frame[frame], expected);
		TASK_CHECK(gsm_decode(ref, (gsm_byte *)(void *)
				       src->frame[frame], ref_out) == 0);
		rc = gsm_decode_plc(prod, src->frame[frame], 33, 1,
				    prod_out, 160);
		TASK_CHECK(rc == 160);
		*streak = 0;
		TASK_CHECK(check_pcm(prod_out, ref_out, 160) == 0);
		TASK_CHECK(check_expected_cache(prod, expected) == 0);
	} else {
		++*streak;
		ref_lost_frame(ref, expected, *streak, ref_out);
		rc = gsm_decode_plc(prod, NULL, 0, 0, prod_out, 160);
		TASK_CHECK(rc == 160);
		TASK_CHECK(prod->plc_loss_count == *streak);
		TASK_CHECK(check_pcm(prod_out, ref_out, 160) == 0);
	}

	TASK_CHECK(check_voice_state(prod, ref) == 0);
	return 0;
}

static int case_cache_records_good_frame(void)
{
	Source src;
	Good expected;
	gsm plc = gsm_create();
	gsm oldp = gsm_create();
	gsm_signal out[160];

	TASK_CHECK(plc && oldp);
	make_source(&src, 17);
	memset(&expected, 0, sizeof(expected));

	capture_good(plc, src.frame[0], &expected);
	TASK_CHECK(gsm_decode_plc(plc, src.frame[0], 33, 1, out, 160) == 160);
	TASK_CHECK(check_expected_cache(plc, &expected) == 0);

	memset(&expected, 0, sizeof(expected));
	capture_good(oldp, src.frame[1], &expected);
	TASK_CHECK(gsm_decode(oldp, (gsm_byte *)(void *)src.frame[1],
			      out) == 0);
	TASK_CHECK(check_expected_cache(oldp, &expected) == 0);

	gsm_destroy(plc);
	gsm_destroy(oldp);
	return 0;
}

static int case_nrp_fallback(void)
{
	Source src;
	gsm d = gsm_create();
	gsm_signal params[76];
	gsm_byte bad[33];
	gsm_signal out[160];

	TASK_CHECK(d);
	make_source(&src, 29);
	memset(params, 0, sizeof(params));
	TASK_CHECK(gsm_explode(d, (gsm_byte *)(void *)src.frame[0],
			       params) == 0);

	params[8] = 39;		/* Nc[0]: below range */
	params[8 + 17] = 121;	/* Nc[1]: above range */
	params[8 + 34] = 77;	/* Nc[2]: valid */
	params[8 + 51] = 0;	/* Nc[3]: below range */
	gsm_implode(d, params, bad);
	TASK_CHECK(((bad[0] >> 4) & 0x0f) == GSM_MAGIC);

	TASK_CHECK(gsm_decode(d, bad, out) == 0);
	TASK_CHECK(d->plc_Nc[0] == 40);
	TASK_CHECK(d->plc_Nc[1] == 40);
	TASK_CHECK(d->plc_Nc[2] == 77);
	TASK_CHECK(d->plc_Nc[3] == 77);
	TASK_CHECK(d->nrp == 77);

	gsm_destroy(d);
	return 0;
}

static int case_single_loss_reference(void)
{
	Source src;
	gsm prod = gsm_create();
	gsm ref = gsm_create();
	Good expected;
	unsigned streak = 0, f, t, nonzero_prefix = 0;
	gsm_signal po[160], ro[160];

	TASK_CHECK(prod && ref);
	make_source(&src, 43);
	for (t = 0; t < 4u * 160u; ++t)
		if (src.pcm[t] != 0)
			++nonzero_prefix;
	TASK_CHECK(nonzero_prefix > 100);
	memset(&expected, 0, sizeof(expected));

	for (f = 0; f < 4; ++f)
		TASK_CHECK(reference_step(prod, ref, &src, f, 1, &streak,
					  &expected, po, ro) == 0);
	TASK_CHECK(reference_step(prod, ref, &src, 4, 0, &streak,
				  &expected, po, ro) == 0);
	TASK_CHECK(streak == 1);

	gsm_destroy(prod);
	gsm_destroy(ref);
	return 0;
}

static int case_eight_loss_reference(void)
{
	Source src;
	gsm prod = gsm_create();
	gsm ref = gsm_create();
	Good expected;
	unsigned streak = 0, f;
	gsm_signal po[160], ro[160];

	TASK_CHECK(prod && ref);
	make_source(&src, 71);
	memset(&expected, 0, sizeof(expected));

	for (f = 0; f < 3; ++f)
		TASK_CHECK(reference_step(prod, ref, &src, f, 1, &streak,
					  &expected, po, ro) == 0);
	for (f = 3; f < 11; ++f)
		TASK_CHECK(reference_step(prod, ref, &src, f, 0, &streak,
					  &expected, po, ro) == 0);
	TASK_CHECK(streak == 8);

	gsm_destroy(prod);
	gsm_destroy(ref);
	return 0;
}

static int case_first_frame_loss(void)
{
	Source src;
	gsm prod = gsm_create();
	gsm ref = gsm_create();
	Good expected;
	unsigned streak = 0, f;
	gsm_signal po[160], ro[160];

	TASK_CHECK(prod && ref);
	make_source(&src, 97);
	memset(&expected, 0, sizeof(expected));

	for (f = 0; f < 3; ++f)
		TASK_CHECK(reference_step(prod, ref, &src, f, 0, &streak,
					  &expected, po, ro) == 0);
	TASK_CHECK(expected.have == 0);
	for (f = 0; f < 160; ++f)
		TASK_CHECK(po[f] == 0);

	gsm_destroy(prod);
	gsm_destroy(ref);
	return 0;
}

static int case_normal_all_good_matches_old(void)
{
	Source src;
	gsm plc = gsm_create();
	gsm oldp = gsm_create();
	unsigned f;
	gsm_signal po[160], oo[160];

	TASK_CHECK(plc && oldp);
	make_source(&src, 109);

	for (f = 0; f < TEST_FRAMES; ++f) {
		TASK_CHECK(gsm_decode_plc(plc, src.frame[f], 33, 1,
					  po, 160) == 160);
		TASK_CHECK(gsm_decode(oldp, (gsm_byte *)(void *)src.frame[f],
				      oo) == 0);
		TASK_CHECK(check_pcm(po, oo, 160) == 0);
		TASK_CHECK(check_voice_state(plc, oldp) == 0);
	}

	gsm_destroy(plc);
	gsm_destroy(oldp);
	return 0;
}

static gsm create_wav(void)
{
	gsm d = gsm_create();
	int one = 1;
	if (d)
		gsm_option(d, GSM_OPT_WAV49, &one);
	return d;
}

static int wav_compare_mask(const Source *src, gsm_byte wav[][66],
			    unsigned mask)
{
	gsm w = create_wav();
	gsm n = gsm_create();
	gsm_signal wout[320], nout[320], nref[160];
	unsigned half;

	TASK_CHECK(w && n);

	TASK_CHECK(gsm_decode_plc(w, wav[0], 65, 3, wout, 320) == 320);
	for (half = 0; half < 2; ++half)
		TASK_CHECK(gsm_decode_plc(n, src->frame[half], 33, 1,
					  nout + half * 160, 160) == 160);
	TASK_CHECK(check_pcm(wout, nout, 320) == 0);
	TASK_CHECK(check_voice_state(w, n) == 0);

	TASK_CHECK(gsm_decode_plc(w, wav[1], 65, mask, wout, 320) == 320);
	for (half = 0; half < 2; ++half) {
		unsigned good = (mask >> half) & 1u;
		if (good)
			TASK_CHECK(gsm_decode_plc(n, src->frame[2 + half],
						  33, 1,
						  nout + half * 160,
						  160) == 160);
		else
			TASK_CHECK(gsm_decode_plc(n, NULL, 0, 0,
						  nout + half * 160,
						  160) == 160);
	}
	(void)nref;
	TASK_CHECK(check_pcm(wout, nout, 320) == 0);
	TASK_CHECK(check_voice_state(w, n) == 0);

	gsm_destroy(w);
	gsm_destroy(n);
	return 0;
}

static int case_wav_four_masks(void)
{
	Source src;
	gsm_byte wav[TEST_BLOCKS][66];
	unsigned mask;

	make_source(&src, 131);
	make_wav_blocks(&src, wav);
	for (mask = 0; mask < 4; ++mask)
		TASK_CHECK(wav_compare_mask(&src, wav, mask) == 0);
	return 0;
}

static int case_wav_shared_nibble(void)
{
	Source src;
	gsm_byte wav[TEST_BLOCKS][66];
	gsm_byte mixed[65];
	gsm w = create_wav();
	gsm n = gsm_create();
	gsm_signal wout[320], nout[320];
	unsigned bit;
	unsigned chain = 0;

	TASK_CHECK(w && n);
	make_source(&src, 139);
	make_wav_blocks(&src, wav);

	TASK_CHECK(gsm_decode_plc(w, wav[0], 65, 3, wout, 320) == 320);
	TASK_CHECK(gsm_decode_plc(n, src.frame[0], 33, 1, nout, 160) == 160);
	TASK_CHECK(gsm_decode_plc(n, src.frame[1], 33, 1, nout + 160,
				  160) == 160);

	memcpy(mixed, wav[1], sizeof(mixed));
	for (bit = 0; bit < 4; ++bit)
		chain |= ((wav[1][32] >> (4 + bit)) & 1u) << bit;
	mixed[32] = (gsm_byte)((chain << 4) | 0x0a);
	for (bit = 0; bit < 32; ++bit)
		mixed[bit] = (gsm_byte)(0xa5 ^ bit);

	/* A stale carried nibble must not be used by the block entry. */
	w->frame_chain = (unsigned char)(chain ^ 0x0f);
	TASK_CHECK(gsm_decode_plc(w, mixed, 65, 2, wout, 320) == 320);

	TASK_CHECK(gsm_decode_plc(n, NULL, 0, 0, nout, 160) == 160);
	TASK_CHECK(gsm_decode_plc(n, src.frame[3], 33, 1, nout + 160,
				  160) == 160);
	TASK_CHECK(check_pcm(wout, nout, 320) == 0);
	TASK_CHECK(check_voice_state(w, n) == 0);
	TASK_CHECK(w->frame_index == 0);

	gsm_destroy(w);
	gsm_destroy(n);
	return 0;
}

static int case_all_loss_input_forms(void)
{
	Source src;
	gsm_byte wav[TEST_BLOCKS][66];
	gsm an = gsm_create(), bn = gsm_create();
	gsm wf = create_wav(), w0 = create_wav();
	gsm_signal ao[160], bo[160], wfo[320], w0o[320];

	TASK_CHECK(an && bn && wf && w0);
	make_source(&src, 251);
	make_wav_blocks(&src, wav);

	TASK_CHECK(gsm_decode_plc(an, src.frame[0], 33, 1, ao, 160) == 160);
	TASK_CHECK(gsm_decode_plc(bn, src.frame[0], 33, 1, bo, 160) == 160);
	TASK_CHECK(gsm_decode_plc(an, NULL, 0, 0, ao, 160) == 160);
	TASK_CHECK(gsm_decode_plc(bn, src.frame[1], 33, 0, bo, 160) == 160);
	TASK_CHECK(check_pcm(ao, bo, 160) == 0);
	TASK_CHECK(check_voice_state(an, bn) == 0);

	TASK_CHECK(gsm_decode_plc(wf, wav[0], 65, 3, wfo, 320) == 320);
	TASK_CHECK(gsm_decode_plc(w0, wav[0], 65, 3, w0o, 320) == 320);
	TASK_CHECK(gsm_decode_plc(wf, wav[1], 65, 0, wfo, 320) == 320);
	TASK_CHECK(gsm_decode_plc(w0, NULL, 0, 0, w0o, 320) == 320);
	TASK_CHECK(check_pcm(wfo, w0o, 320) == 0);
	TASK_CHECK(check_voice_state(wf, w0) == 0);
	TASK_CHECK(wf->frame_index == 0 && w0->frame_index == 0);

	gsm_destroy(an);
	gsm_destroy(bn);
	gsm_destroy(wf);
	gsm_destroy(w0);
	return 0;
}

static void fill_sentinel(gsm_signal *p, unsigned n)
{
	unsigned i;
	for (i = 0; i < n; ++i)
		p[i] = (gsm_signal)0x5a5a;
}

static int check_sentinel(const gsm_signal *p, unsigned n)
{
	unsigned i;
	for (i = 0; i < n; ++i)
		TASK_CHECK(p[i] == (gsm_signal)0x5a5a);
	return 0;
}

static int case_normal_errors_and_retry(void)
{
	Source src;
	gsm d = gsm_create();
	gsm ref = gsm_create();
	gsm_signal out[160], expected[160];
	gsm_byte bad[33];
	struct gsm_state snap;

	TASK_CHECK(d && ref);
	make_source(&src, 151);

#define REJECT_NORMAL(call) do {					\
		fill_sentinel(out, 160);				\
		snap = *d;						\
		TASK_CHECK((call) == -1);				\
		TASK_CHECK(check_sentinel(out, 160) == 0);		\
		TASK_CHECK(memcmp(&snap, d, sizeof(snap)) == 0);	\
	} while (0)

	REJECT_NORMAL(gsm_decode_plc(d, src.frame[0], 33, 2, out, 160));
	REJECT_NORMAL(gsm_decode_plc(d, src.frame[0], 32, 1, out, 160));
	REJECT_NORMAL(gsm_decode_plc(d, src.frame[0], 34, 1, out, 160));
	REJECT_NORMAL(gsm_decode_plc(d, src.frame[0], 33, 1, out, 159));
	REJECT_NORMAL(gsm_decode_plc(d, src.frame[0], 0, 0, out, 160));
	REJECT_NORMAL(gsm_decode_plc(d, src.frame[0], 0, 0, out, 160));
	TASK_CHECK(gsm_decode_plc(d, src.frame[0], 33, 1, NULL, 160) == -1);

	memcpy(bad, src.frame[0], sizeof(bad));
	bad[0] &= 0x0f;
	REJECT_NORMAL(gsm_decode_plc(d, bad, 33, 1, out, 160));
#undef REJECT_NORMAL

	/* The rejected calls left the state usable and unchanged. */
	fill_sentinel(out, 160);
	TASK_CHECK(gsm_decode_plc(d, src.frame[1], 33, 1, out, 161) == 160);
	TASK_CHECK(gsm_decode(ref, (gsm_byte *)(void *)src.frame[1],
			      expected) == 0);
	TASK_CHECK(check_pcm(out, expected, 160) == 0);

	gsm_destroy(d);
	gsm_destroy(ref);
	return 0;
}

static int case_wav_errors_and_midblock_retry(void)
{
	Source src;
	gsm_byte wav[TEST_BLOCKS][66];
	gsm d = create_wav();
	gsm ref = create_wav();
	gsm_signal rejected[320], first[320], expected[320];
	struct gsm_state snap;

	TASK_CHECK(d && ref);
	make_source(&src, 163);
	make_wav_blocks(&src, wav);

#define REJECT_WAV(call) do {						\
		fill_sentinel(rejected, 320);				\
		snap = *d;						\
		TASK_CHECK((call) == -1);				\
		TASK_CHECK(check_sentinel(rejected, 320) == 0);	\
		TASK_CHECK(memcmp(&snap, d, sizeof(snap)) == 0);	\
	} while (0)

	REJECT_WAV(gsm_decode_plc(d, wav[0], 65, 4, rejected, 320));
	REJECT_WAV(gsm_decode_plc(d, wav[0], 64, 3, rejected, 320));
	REJECT_WAV(gsm_decode_plc(d, wav[0], 66, 3, rejected, 320));
	REJECT_WAV(gsm_decode_plc(d, wav[0], 65, 3, rejected, 319));
	TASK_CHECK(gsm_decode_plc(d, wav[0], 65, 3, NULL, 320) == -1);
#undef REJECT_WAV

	/* An old-entry WAV decode can stop between the two halves. */
	TASK_CHECK(gsm_decode(d, wav[0], first) == 0);
	TASK_CHECK(d->frame_index == 1);
	snap = *d;
	fill_sentinel(rejected, 320);
	TASK_CHECK(gsm_decode_plc(d, wav[1], 65, 3, rejected, 320) == -1);
	TASK_CHECK(check_sentinel(rejected, 320) == 0);
	TASK_CHECK(d->frame_index == 1);
	TASK_CHECK(memcmp(&snap, d, sizeof(snap)) == 0);

	/* Finish through the old boundary, then compare the normal pair. */
	TASK_CHECK(gsm_decode(d, wav[0] + 33, first + 160) == 0);
	TASK_CHECK(d->frame_index == 0);
	TASK_CHECK(gsm_decode(ref, wav[0], expected) == 0);
	TASK_CHECK(gsm_decode(ref, wav[0] + 33, expected + 160) == 0);
	TASK_CHECK(check_pcm(first, expected, 320) == 0);

	gsm_destroy(d);
	gsm_destroy(ref);
	return 0;
}

static int case_tail_after_six_losses(void)
{
	Source src;
	gsm prod = gsm_create();
	gsm ref = gsm_create();
	Good expected;
	unsigned streak = 0, f, nonzero;
	gsm_signal po[160], ro[160];
	int k;

	TASK_CHECK(prod && ref);
	make_source(&src, 181);
	memset(&expected, 0, sizeof(expected));

	for (f = 0; f < 6; ++f)
		TASK_CHECK(reference_step(prod, ref, &src, f, 1, &streak,
					  &expected, po, ro) == 0);
	for (f = 6; f < 12; ++f)
		TASK_CHECK(reference_step(prod, ref, &src, f, 0, &streak,
					  &expected, po, ro) == 0);
	TASK_CHECK(streak == 6);

	/* The sixth frame uses gain zero: all newly shifted residuals are 0. */
	for (k = 0; k < 160; ++k)
		TASK_CHECK(prod->dp0[k] == 0);

	nonzero = 0;
	for (k = 0; k < 160; ++k)
		if (po[k] != 0)
			++nonzero;
	TASK_CHECK(nonzero > 0);

	/* The IIR tail is still live and matches the borrowed ST reference. */
	TASK_CHECK(check_pcm(po, ro, 160) == 0);
	TASK_CHECK(check_voice_state(prod, ref) == 0);

	gsm_destroy(prod);
	gsm_destroy(ref);
	return 0;
}

static int case_recover_two_good_frames(void)
{
	Source src;
	gsm prod = gsm_create();
	gsm ref = gsm_create();
	Good expected;
	unsigned streak = 0, f;
	gsm_signal po[160], ro[160], old_prod[160], old_ref[160];

	TASK_CHECK(prod && ref);
	make_source(&src, 193);
	memset(&expected, 0, sizeof(expected));

	for (f = 0; f < 3; ++f)
		TASK_CHECK(reference_step(prod, ref, &src, f, 1, &streak,
					  &expected, po, ro) == 0);
	TASK_CHECK(reference_step(prod, ref, &src, 3, 0, &streak,
				  &expected, po, ro) == 0);
	TASK_CHECK(reference_step(prod, ref, &src, 4, 0, &streak,
				  &expected, po, ro) == 0);

	/* First recovery through the block entry. */
	TASK_CHECK(reference_step(prod, ref, &src, 5, 1, &streak,
				  &expected, po, ro) == 0);
	TASK_CHECK(streak == 0);

	/* Second recovery alternates back to the legacy entry. */
	capture_good(ref, src.frame[6], &expected);
	TASK_CHECK(gsm_decode(prod, (gsm_byte *)(void *)src.frame[6],
			      old_prod) == 0);
	TASK_CHECK(gsm_decode(ref, (gsm_byte *)(void *)src.frame[6],
			      old_ref) == 0);
	TASK_CHECK(check_pcm(old_prod, old_ref, 160) == 0);
	TASK_CHECK(check_expected_cache(prod, &expected) == 0);
	TASK_CHECK(check_voice_state(prod, ref) == 0);
	TASK_CHECK(prod->plc_loss_count == 0);

	/* A later block-entry good frame remains bit-exact. */
	TASK_CHECK(reference_step(prod, ref, &src, 7, 1, &streak,
				  &expected, po, ro) == 0);

	gsm_destroy(prod);
	gsm_destroy(ref);
	return 0;
}

static int case_two_interleaved_instances(void)
{
	Source a_src, b_src;
	gsm ap = gsm_create(), ar = gsm_create();
	gsm bp = gsm_create(), br = gsm_create();
	Good ag, bg;
	unsigned as = 0, bs = 0, f;
	gsm_signal apo[160], aro[160], bpo[160], bro[160];

	TASK_CHECK(ap && ar && bp && br);
	make_source(&a_src, 211);
	make_source(&b_src, 223);
	memset(&ag, 0, sizeof(ag));
	memset(&bg, 0, sizeof(bg));

	for (f = 0; f < 12; ++f) {
		int a_good = (f != 4 && f != 5 && f != 9);
		int b_good = (f != 2 && f != 3 && f != 4 && f != 10);

		TASK_CHECK(reference_step(ap, ar, &a_src, f, a_good, &as,
					  &ag, apo, aro) == 0);
		TASK_CHECK(reference_step(bp, br, &b_src, f, b_good, &bs,
					  &bg, bpo, bro) == 0);

		/* Interleaving must not have mixed caches or voice state. */
		TASK_CHECK(check_expected_cache(ap, &ag) == 0);
		TASK_CHECK(check_expected_cache(bp, &bg) == 0);
	}

	gsm_destroy(ap);
	gsm_destroy(ar);
	gsm_destroy(bp);
	gsm_destroy(br);
	return 0;
}

const struct task_case task_cases[] = {
	{ "Excitation", "cache-records-good-frame", case_cache_records_good_frame },
	{ "Excitation", "nrp-fallback", case_nrp_fallback },
	{ "Excitation", "single-loss-wide-reference", case_single_loss_reference },
	{ "Excitation", "eight-loss-wide-reference", case_eight_loss_reference },
	{ "Excitation", "first-frame-loss", case_first_frame_loss },

	{ "Packing", "normal-all-good-matches-old", case_normal_all_good_matches_old },
	{ "Packing", "wav49-four-masks", case_wav_four_masks },
	{ "Packing", "wav49-shared-nibble", case_wav_shared_nibble },
	{ "Packing", "all-loss-input-forms", case_all_loss_input_forms },
	{ "Packing", "normal-errors-retry", case_normal_errors_and_retry },
	{ "Packing", "wav49-errors-midblock-retry", case_wav_errors_and_midblock_retry },

	{ "Recovery", "tail-after-six-losses", case_tail_after_six_losses },
	{ "Recovery", "recover-two-good-frames", case_recover_two_good_frames },
	{ "Recovery", "two-interleaved-instances", case_two_interleaved_instances },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
