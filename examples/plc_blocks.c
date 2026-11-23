/*
 * examples/plc_blocks.c
 *
 *  Feeds a deterministic sequence of GSM frames through the block
 *  entry gsm_decode_plc(), deliberately dropping some blocks, in both
 *  ordinary (33 byte / 160 sample) and WAV49 (65 byte / 320 sample)
 *  modes.  It prints the per-block result, the number of concealed
 *  samples and a running checksum so the deterministic concealment is
 *  easy to observe.  Only the C compiler and the standard library are
 *  used; run it after building libgsm, e.g.
 *
 *	cc -Iinc examples/plc_blocks.c lib/libgsm.a -o plc_blocks
 *	./plc_blocks
 */

#include "gsm.h"

#include <stdio.h>
#include <string.h>

#define N_FRAMES	10			/* ordinary frames	*/
#define SAMP		160

/* FNV-1a, just to give the run a compact, reproducible fingerprint. */
static unsigned long hash(unsigned long h, const void * p, unsigned n)
{
	const unsigned char * b = (const unsigned char *)p;
	while (n--)
		h = (h ^ *b++) * 1099511628211UL;
	return h;
}

static void make_pcm(gsm_signal * s, int frame)
{
	int i;
	for (i = 0; i < SAMP; i++) {
		unsigned t = (unsigned)(frame * SAMP + i);
		s[i] = (gsm_signal)((int)((t * 73 + t * t * 3) % 24001) - 12000);
	}
}

/* A fixed, reproducible loss schedule. */
static int frame_is_lost(int frame)
{
	return frame == 2 || frame == 5 || frame == 6;
}

static int run_normal(void)
{
	gsm enc = gsm_create();
	gsm dec = gsm_create();
	gsm_signal pcm[SAMP], out[SAMP];
	gsm_byte frame[33];
	unsigned long h = 1469598103934665603UL;
	int f, rc, concealed = 0, produced = 0;

	if (!enc || !dec) return 1;

	for (f = 0; f < N_FRAMES; f++) {
		make_pcm(pcm, f);
		memset(frame, 0, sizeof frame);
		gsm_encode(enc, pcm, frame);

		if (frame_is_lost(f)) {
			/* Whole block missing: NULL data, zero bytes. */
			rc = gsm_decode_plc(dec, (const gsm_byte *)0, 0, 0,
				out, SAMP);
			if (rc == SAMP) concealed += SAMP;
		} else {
			rc = gsm_decode_plc(dec, frame, 33, 1, out, SAMP);
		}

		if (rc != SAMP) {
			fprintf(stderr, "normal block %d failed: %d\n", f, rc);
			return 1;
		}
		h = hash(h, out, sizeof out);
		produced += rc;
	}

	printf("normal  frames=%d produced=%d concealed=%d hash=%016lx\n",
		N_FRAMES, produced, concealed, h);

	gsm_destroy(enc);
	gsm_destroy(dec);
	return 0;
}

/* Same loss pattern expressed on WAV49 blocks: two frames per block,
 * bit 0 marks the first 160-sample half and bit 1 the second. */
static int run_wav49(void)
{
	gsm enc = gsm_create();
	gsm dec = gsm_create();
	gsm_signal pcm[2 * SAMP], out[2 * SAMP];
	gsm_byte block[65];
	unsigned long h = 1469598103934665603UL;
	int one = 1, p, rc, concealed = 0, produced = 0;

	if (!enc || !dec) return 1;
	/* gsm_option() returns -1 if the library was built without
	 * WAV49 support; querying (NULL value pointer) does not change
	 * the option.
	 */
	if (gsm_option(enc, GSM_OPT_WAV49, (int *)0) < 0 ||
	    gsm_option(dec, GSM_OPT_WAV49, (int *)0) < 0) {
		printf("wav49   unsupported in this build; skipped\n");
		gsm_destroy(enc);
		gsm_destroy(dec);
		return 0;
	}
	gsm_option(enc, GSM_OPT_WAV49, &one);
	gsm_option(dec, GSM_OPT_WAV49, &one);

	for (p = 0; p < N_FRAMES / 2; p++) {
		unsigned mask;

		make_pcm(pcm, 2 * p);
		make_pcm(pcm + SAMP, 2 * p + 1);
		memset(block, 0, sizeof block);
		/* WAV49 shares one byte between the two halves: the second
		 * encode starts at block+32; the block entry reads the
		 * second half's payload from block+33.
		 */
		gsm_encode(enc, pcm, block);
		gsm_encode(enc, pcm + SAMP, block + 32);

		mask = (frame_is_lost(2 * p) ? 0u : 1u)
		     | (frame_is_lost(2 * p + 1) ? 0u : 2u);

		rc = gsm_decode_plc(dec, block, 65, mask, out, 2 * SAMP);
		if (rc != 2 * SAMP) {
			fprintf(stderr, "wav49 block %d failed: %d\n", p, rc);
			return 1;
		}
		if (!(mask & 1)) concealed += SAMP;
		if (!(mask & 2)) concealed += SAMP;
		h = hash(h, out, sizeof out);
		produced += rc;
	}

	printf("wav49   blocks=%d produced=%d concealed=%d hash=%016lx\n",
		N_FRAMES / 2, produced, concealed, h);

	gsm_destroy(enc);
	gsm_destroy(dec);
	return 0;
}

int main(void)
{
	if (run_normal() != 0) return 1;
	if (run_wav49() != 0) return 1;
	printf("plc_blocks: ok\n");
	return 0;
}
