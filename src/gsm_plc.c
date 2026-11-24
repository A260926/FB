/*
 * PLC entry point for complete GSM 06.10 / WAV49 blocks.
 *
 * The concealment implemented here is intentionally a deterministic player
 * policy rather than an implementation of an external PLC standard.
 */

#include "gsm.h"
#include "private.h"

#define PLC_NORMAL_BYTES	33u
#define PLC_NORMAL_SAMPLES	160u
#define PLC_WAV_BYTES		65u
#define PLC_WAV_SAMPLES		320u

static const word plc_attenuation[5] = {
	29491, 26214, 19661, 13107, 6554
};

static longword plc_floor_q15(longword x, word gain)
{
	longword numerator = x * (longword)gain + 16384;
	longword quotient = numerator / 32768;

	/* C division truncates towards zero; the policy specifies floor(). */
	if (numerator < 0 && numerator % 32768 != 0)
		--quotient;
	return quotient;
}

static word plc_sat16(longword x)
{
	if (x < MIN_WORD) return MIN_WORD;
	if (x > MAX_WORD) return MAX_WORD;
	return (word)x;
}

/*
 * Synthesize one concealed 160-sample frame.  The loss counter is advanced
 * here so that the two halves of a WAV49 block are counted as two consecutive
 * GSM frames on the audio timeline.
 */
static void plc_lost_frame(struct gsm_state *s, gsm_signal *out)
{
	static const word zero_lar[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

	word		lar[8];
	word		wt[160];
	word	      * drp = s->dp0 + 120;
	unsigned int	n;
	word		gain;
	int		j, k;

	n = ++s->plc_loss_count;
	gain = n <= 5 ? plc_attenuation[n - 1] : 0;

	for (k = 0; k < 8; k++)
		lar[k] = s->plc_have_good ? s->plc_LARc[k] : zero_lar[k];

	for (j = 0; j < 4; j++) {
		int Nc = 40;
		int bc = 0;
		int Nr;
		word brp;

		if (s->plc_have_good) {
			Nc = s->plc_Nc[j];
			bc = s->plc_bc[j];
		}
		if (bc < 0 || bc > 3)
			bc = 0;

		/* Same lag fallback as Gsm_Long_Term_Synthesis_Filtering. */
		Nr = (Nc < 40 || Nc > 120) ? (int)s->nrp : Nc;
		s->nrp = (word)Nr;
		brp = gsm_QLB[bc];

		for (k = 0; k <= 39; k++) {
			word history = drp[k - Nr];
			word prediction = plc_sat16(
				plc_floor_q15((longword)history, brp));
			longword old_excitation = s->plc_have_good
				? (longword)s->plc_excitation[j][k]
				: 0;
			longword scaled_excitation =
				plc_floor_q15(old_excitation, gain);
			longword scaled_prediction =
				plc_floor_q15((longword)prediction, gain);

			drp[k] = plc_sat16(scaled_excitation +
					   scaled_prediction);
		}

		/* Identical history shift to the original LTP synthesis. */
		for (k = 0; k <= 119; k++)
			drp[-120 + k] = drp[-80 + k];

		for (k = 0; k <= 39; k++)
			wt[j * 40 + k] = drp[k];
	}

	Gsm_Short_Term_Synthesis_Filter(s, lar, wt, out);
	Gsm_Postprocessing(s, out);
}

int gsm_decode_plc(
	gsm		 s,
	const gsm_byte	*data,
	unsigned int	 bytes,
	unsigned int	 good_mask,
	gsm_signal	*pcm,
	unsigned int	 capacity)
{
	unsigned int	wav_fmt = 0;
	unsigned int	block_bytes;
	unsigned int	samples;
	unsigned int	mask_limit;

	if (!s || !pcm)
		return -1;

#ifdef WAV49
	wav_fmt = (unsigned int)s->wav_fmt;
#endif

	if (wav_fmt) {
		block_bytes = PLC_WAV_BYTES;
		samples = PLC_WAV_SAMPLES;
		mask_limit = 3u;
	} else {
		block_bytes = PLC_NORMAL_BYTES;
		samples = PLC_NORMAL_SAMPLES;
		mask_limit = 1u;
	}

	if (good_mask > mask_limit || capacity < samples)
		return -1;

	if (good_mask != 0) {
		if (!data || bytes != block_bytes)
			return -1;
	} else {
		int null_short_block = (data == NULL && bytes == 0);
		int complete_block = (data != NULL && bytes == block_bytes);

		if (!null_short_block && !complete_block)
			return -1;
	}

#ifdef WAV49
	/* The block entry point is only legal at a complete-block boundary. */
	if (wav_fmt && s->frame_index != 0)
		return -1;
#endif

	if (!wav_fmt && (good_mask & 1u) &&
	    (((const unsigned char *)data)[0] >> 4) != GSM_MAGIC)
		return -1;

	if (!wav_fmt) {
		if (good_mask & 1u) {
			if (gsm_decode(s, (gsm_byte *)(void *)data, pcm) != 0)
				return -1;
		} else {
			plc_lost_frame(s, pcm);
		}
		return (int)PLC_NORMAL_SAMPLES;
	}

#ifdef WAV49
	switch (good_mask) {
	case 3:
		if (gsm_decode(s, (gsm_byte *)(void *)data, pcm) != 0)
			return -1;
		if (gsm_decode(s, (gsm_byte *)(void *)(data + 33),
				pcm + 160) != 0)
			return -1;
		break;

	case 1:
		/* First half good; old entry leaves us between the halves. */
		if (gsm_decode(s, (gsm_byte *)(void *)data, pcm) != 0)
			return -1;
		plc_lost_frame(s, pcm + 160);
		s->frame_index = 0;
		break;

	case 2:
		/*
		 * First half is absent.  Do not run it through the decoder:
		 * the second half starts with four bits carried by the high
		 * nibble of shared byte 32.  Set the streaming parser up to
		 * execute only its second-half branch.
		 */
		plc_lost_frame(s, pcm);
		s->frame_chain = (unsigned char)((data[32] >> 4) & 0x0f);
		s->frame_index = 1;
		if (gsm_decode(s, (gsm_byte *)(void *)(data + 33),
				pcm + 160) != 0) {
			s->frame_index = 0;
			return -1;
		}
		s->frame_index = 0;
		break;

	default: /* good_mask == 0 */
		plc_lost_frame(s, pcm);
		plc_lost_frame(s, pcm + 160);
		s->frame_index = 0;
		break;
	}

	return (int)PLC_WAV_SAMPLES;
#else
	return -1;
#endif
}
