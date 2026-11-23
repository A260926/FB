/*
 * Copyright 1992 by Jutta Degener and Carsten Bormann, Technische
 * Universitaet Berlin.  See the accompanying file "COPYRIGHT" for
 * details.  THERE IS ABSOLUTELY NO WARRANTY FOR THIS SOFTWARE.
 */

/*
 *  Shared full-frame decoding engine and packet loss concealment (PLC).
 *
 *  The legacy gsm_decode() entry and the new gsm_decode_plc() block
 *  entry process whole 160-sample frames through the same routines in
 *  this file, so the most-recent-good-frame information they maintain
 *  is identical and the two entries can be interleaved at frame
 *  boundaries.
 *
 *  The concealment is a deterministic, self-contained policy; it does
 *  not claim to implement any external PLC standard.  Only the integer
 *  path is supported (FAST / USE_FLOAT_MUL / LTP_CUT are not extended).
 */

#include "config.h"

#ifdef HAS_STRING_H
#include <string.h>
#endif

#include "private.h"
#include "gsm.h"
#include "proto.h"

/* Q15 attenuation of the n-th consecutive lost frame (n is 1-based);
 * the sixth and later frames use 0.
 */
static const word plc_gain[5] = { 29491, 26214, 19661, 13107, 6554 };

static word sat16 P1((v), longword v)
{
	if (v > MAX_WORD)  return MAX_WORD;
	if (v < MIN_WORD)  return MIN_WORD;
	return (word)v;
}

/* q15(x, g) = floor((x*g + 16384) / 32768).  g >= 0 here, so the
 * arithmetic shift right implements the floor exactly.
 */
static word q15 P2((x, g), word x, word g)
{
	return (word)SASR((longword)x * (longword)g + 16384, 15);
}

/* Advance a consecutive-loss counter without ever wrapping, so a very
 * long burst cannot accidentally re-enable a non-zero attenuation.
 */
static unsigned char bump_lost P1((c), unsigned char c)
{
	return c < 255 ? (unsigned char)(c + 1) : (unsigned char)255;
}

/* Deemphasis, truncation and upscaling, copied verbatim from the
 * Postprocessing() routine in decode.c.
 */
static void postprocess P2((S, s), struct gsm_state * S, word * s)
{
	register int		k;
	register word		msr = S->msr;
	register longword	ltmp;
	register word		tmp;

	for (k = 160; k--; s++) {
		tmp = GSM_MULT_R(msr, 28180);
		msr = GSM_ADD(*s, tmp);  	      /* Deemphasis */
		*s  = GSM_ADD(msr, msr) & 0xFFF8;     /* Trunc/up  */
	}
	S->msr = msr;
}

/*
 *  Unpack one 33-byte ordinary GSM frame into the six parameter
 *  arrays.  Mirrors the non-WAV49 branch of the legacy gsm_decode();
 *  returns -1 on a bad magic nibble, 0 otherwise.  Reads only.
 */
static int unpack_normal P7((c, LARc, Nc, bc, Mc, xmaxc, xmc),
	const gsm_byte * c,
	word * LARc, word * Nc, word * bc, word * Mc, word * xmaxc,
	word * xmc)
{
	if (((*c >> 4) & 0x0F) != GSM_MAGIC) return -1;

	LARc[0]  = (*c++ & 0xF) << 2;		/* 1 */
	LARc[0] |= (*c >> 6) & 0x3;
	LARc[1]  = *c++ & 0x3F;
	LARc[2]  = (*c >> 3) & 0x1F;
	LARc[3]  = (*c++ & 0x7) << 2;
	LARc[3] |= (*c >> 6) & 0x3;
	LARc[4]  = (*c >> 2) & 0xF;
	LARc[5]  = (*c++ & 0x3) << 2;
	LARc[5] |= (*c >> 6) & 0x3;
	LARc[6]  = (*c >> 3) & 0x7;
	LARc[7]  = *c++ & 0x7;
	Nc[0]  = (*c >> 1) & 0x7F;
	bc[0]  = (*c++ & 0x1) << 1;
	bc[0] |= (*c >> 7) & 0x1;
	Mc[0]  = (*c >> 5) & 0x3;
	xmaxc[0]  = (*c++ & 0x1F) << 1;
	xmaxc[0] |= (*c >> 7) & 0x1;
	xmc[0]  = (*c >> 4) & 0x7;
	xmc[1]  = (*c >> 1) & 0x7;
	xmc[2]  = (*c++ & 0x1) << 2;
	xmc[2] |= (*c >> 6) & 0x3;
	xmc[3]  = (*c >> 3) & 0x7;
	xmc[4]  = *c++ & 0x7;
	xmc[5]  = (*c >> 5) & 0x7;
	xmc[6]  = (*c >> 2) & 0x7;
	xmc[7]  = (*c++ & 0x3) << 1;		/* 10 */
	xmc[7] |= (*c >> 7) & 0x1;
	xmc[8]  = (*c >> 4) & 0x7;
	xmc[9]  = (*c >> 1) & 0x7;
	xmc[10]  = (*c++ & 0x1) << 2;
	xmc[10] |= (*c >> 6) & 0x3;
	xmc[11]  = (*c >> 3) & 0x7;
	xmc[12]  = *c++ & 0x7;
	Nc[1]  = (*c >> 1) & 0x7F;
	bc[1]  = (*c++ & 0x1) << 1;
	bc[1] |= (*c >> 7) & 0x1;
	Mc[1]  = (*c >> 5) & 0x3;
	xmaxc[1]  = (*c++ & 0x1F) << 1;
	xmaxc[1] |= (*c >> 7) & 0x1;
	xmc[13]  = (*c >> 4) & 0x7;
	xmc[14]  = (*c >> 1) & 0x7;
	xmc[15]  = (*c++ & 0x1) << 2;
	xmc[15] |= (*c >> 6) & 0x3;
	xmc[16]  = (*c >> 3) & 0x7;
	xmc[17]  = *c++ & 0x7;
	xmc[18]  = (*c >> 5) & 0x7;
	xmc[19]  = (*c >> 2) & 0x7;
	xmc[20]  = (*c++ & 0x3) << 1;
	xmc[20] |= (*c >> 7) & 0x1;
	xmc[21]  = (*c >> 4) & 0x7;
	xmc[22]  = (*c >> 1) & 0x7;
	xmc[23]  = (*c++ & 0x1) << 2;
	xmc[23] |= (*c >> 6) & 0x3;
	xmc[24]  = (*c >> 3) & 0x7;
	xmc[25]  = *c++ & 0x7;
	Nc[2]  = (*c >> 1) & 0x7F;
	bc[2]  = (*c++ & 0x1) << 1;		/* 20 */
	bc[2] |= (*c >> 7) & 0x1;
	Mc[2]  = (*c >> 5) & 0x3;
	xmaxc[2]  = (*c++ & 0x1F) << 1;
	xmaxc[2] |= (*c >> 7) & 0x1;
	xmc[26]  = (*c >> 4) & 0x7;
	xmc[27]  = (*c >> 1) & 0x7;
	xmc[28]  = (*c++ & 0x1) << 2;
	xmc[28] |= (*c >> 6) & 0x3;
	xmc[29]  = (*c >> 3) & 0x7;
	xmc[30]  = *c++ & 0x7;
	xmc[31]  = (*c >> 5) & 0x7;
	xmc[32]  = (*c >> 2) & 0x7;
	xmc[33]  = (*c++ & 0x3) << 1;
	xmc[33] |= (*c >> 7) & 0x1;
	xmc[34]  = (*c >> 4) & 0x7;
	xmc[35]  = (*c >> 1) & 0x7;
	xmc[36]  = (*c++ & 0x1) << 2;
	xmc[36] |= (*c >> 6) & 0x3;
	xmc[37]  = (*c >> 3) & 0x7;
	xmc[38]  = *c++ & 0x7;
	Nc[3]  = (*c >> 1) & 0x7F;
	bc[3]  = (*c++ & 0x1) << 1;
	bc[3] |= (*c >> 7) & 0x1;
	Mc[3]  = (*c >> 5) & 0x3;
	xmaxc[3]  = (*c++ & 0x1F) << 1;
	xmaxc[3] |= (*c >> 7) & 0x1;
	xmc[39]  = (*c >> 4) & 0x7;
	xmc[40]  = (*c >> 1) & 0x7;
	xmc[41]  = (*c++ & 0x1) << 2;
	xmc[41] |= (*c >> 6) & 0x3;
	xmc[42]  = (*c >> 3) & 0x7;
	xmc[43]  = *c++ & 0x7;			/* 30  */
	xmc[44]  = (*c >> 5) & 0x7;
	xmc[45]  = (*c >> 2) & 0x7;
	xmc[46]  = (*c++ & 0x3) << 1;
	xmc[46] |= (*c >> 7) & 0x1;
	xmc[47]  = (*c >> 4) & 0x7;
	xmc[48]  = (*c >> 1) & 0x7;
	xmc[49]  = (*c++ & 0x1) << 2;
	xmc[49] |= (*c >> 6) & 0x3;
	xmc[50]  = (*c >> 3) & 0x7;
	xmc[51]  = *c & 0x7;			/* 33 */
	return 0;
}

#ifdef WAV49

/*
 *  Unpack the first (even, frame_index == 1) 33-byte half of a WAV49
 *  block.  Mirrors the first branch of the legacy gsm_decode(); the
 *  low nibble carried over to the second half is returned in *chain
 *  rather than written into the decoder state.  Reads only.
 */
static void unpack_wav_even P8((c, LARc, Nc, bc, Mc, xmaxc, xmc, chain),
	const gsm_byte * c,
	word * LARc, word * Nc, word * bc, word * Mc, word * xmaxc,
	word * xmc, gsm_byte * chain)
{
	uword sr = 0;

	sr = *c++;
	LARc[0] = sr & 0x3f;  sr >>= 6;
	sr |= (uword)*c++ << 2;
	LARc[1] = sr & 0x3f;  sr >>= 6;
	sr |= (uword)*c++ << 4;
	LARc[2] = sr & 0x1f;  sr >>= 5;
	LARc[3] = sr & 0x1f;  sr >>= 5;
	sr |= (uword)*c++ << 2;
	LARc[4] = sr & 0xf;  sr >>= 4;
	LARc[5] = sr & 0xf;  sr >>= 4;
	sr |= (uword)*c++ << 2;			/* 5 */
	LARc[6] = sr & 0x7;  sr >>= 3;
	LARc[7] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 4;
	Nc[0] = sr & 0x7f;  sr >>= 7;
	bc[0] = sr & 0x3;  sr >>= 2;
	Mc[0] = sr & 0x3;  sr >>= 2;
	sr |= (uword)*c++ << 1;
	xmaxc[0] = sr & 0x3f;  sr >>= 6;
	xmc[0] = sr & 0x7;  sr >>= 3;
	sr = *c++;
	xmc[1] = sr & 0x7;  sr >>= 3;
	xmc[2] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 2;
	xmc[3] = sr & 0x7;  sr >>= 3;
	xmc[4] = sr & 0x7;  sr >>= 3;
	xmc[5] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 1;			/* 10 */
	xmc[6] = sr & 0x7;  sr >>= 3;
	xmc[7] = sr & 0x7;  sr >>= 3;
	xmc[8] = sr & 0x7;  sr >>= 3;
	sr = *c++;
	xmc[9] = sr & 0x7;  sr >>= 3;
	xmc[10] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 2;
	xmc[11] = sr & 0x7;  sr >>= 3;
	xmc[12] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 4;
	Nc[1] = sr & 0x7f;  sr >>= 7;
	bc[1] = sr & 0x3;  sr >>= 2;
	Mc[1] = sr & 0x3;  sr >>= 2;
	sr |= (uword)*c++ << 1;
	xmaxc[1] = sr & 0x3f;  sr >>= 6;
	xmc[13] = sr & 0x7;  sr >>= 3;
	sr = *c++;				/* 15 */
	xmc[14] = sr & 0x7;  sr >>= 3;
	xmc[15] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 2;
	xmc[16] = sr & 0x7;  sr >>= 3;
	xmc[17] = sr & 0x7;  sr >>= 3;
	xmc[18] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 1;
	xmc[19] = sr & 0x7;  sr >>= 3;
	xmc[20] = sr & 0x7;  sr >>= 3;
	xmc[21] = sr & 0x7;  sr >>= 3;
	sr = *c++;
	xmc[22] = sr & 0x7;  sr >>= 3;
	xmc[23] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 2;
	xmc[24] = sr & 0x7;  sr >>= 3;
	xmc[25] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 4;			/* 20 */
	Nc[2] = sr & 0x7f;  sr >>= 7;
	bc[2] = sr & 0x3;  sr >>= 2;
	Mc[2] = sr & 0x3;  sr >>= 2;
	sr |= (uword)*c++ << 1;
	xmaxc[2] = sr & 0x3f;  sr >>= 6;
	xmc[26] = sr & 0x7;  sr >>= 3;
	sr = *c++;
	xmc[27] = sr & 0x7;  sr >>= 3;
	xmc[28] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 2;
	xmc[29] = sr & 0x7;  sr >>= 3;
	xmc[30] = sr & 0x7;  sr >>= 3;
	xmc[31] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 1;
	xmc[32] = sr & 0x7;  sr >>= 3;
	xmc[33] = sr & 0x7;  sr >>= 3;
	xmc[34] = sr & 0x7;  sr >>= 3;
	sr = *c++;				/* 25 */
	xmc[35] = sr & 0x7;  sr >>= 3;
	xmc[36] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 2;
	xmc[37] = sr & 0x7;  sr >>= 3;
	xmc[38] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 4;
	Nc[3] = sr & 0x7f;  sr >>= 7;
	bc[3] = sr & 0x3;  sr >>= 2;
	Mc[3] = sr & 0x3;  sr >>= 2;
	sr |= (uword)*c++ << 1;
	xmaxc[3] = sr & 0x3f;  sr >>= 6;
	xmc[39] = sr & 0x7;  sr >>= 3;
	sr = *c++;
	xmc[40] = sr & 0x7;  sr >>= 3;
	xmc[41] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 2;			/* 30 */
	xmc[42] = sr & 0x7;  sr >>= 3;
	xmc[43] = sr & 0x7;  sr >>= 3;
	xmc[44] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 1;
	xmc[45] = sr & 0x7;  sr >>= 3;
	xmc[46] = sr & 0x7;  sr >>= 3;
	xmc[47] = sr & 0x7;  sr >>= 3;
	sr = *c++;
	xmc[48] = sr & 0x7;  sr >>= 3;
	xmc[49] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 2;
	xmc[50] = sr & 0x7;  sr >>= 3;
	xmc[51] = sr & 0x7;  sr >>= 3;

	*chain = (gsm_byte)(sr & 0xf);
}

/*
 *  Unpack the second (odd, frame_index == 0) 32-byte half of a WAV49
 *  block.  Mirrors the second branch of the legacy gsm_decode(), but
 *  the shared low nibble is supplied explicitly by the caller so the
 *  second half can be decoded even when the first half was lost: it
 *  is read straight from the shared block byte.  Reads only.
 */
static void unpack_wav_odd P8((chain, c, LARc, Nc, bc, Mc, xmaxc, xmc),
	gsm_byte chain, const gsm_byte * c,
	word * LARc, word * Nc, word * bc, word * Mc, word * xmaxc,
	word * xmc)
{
	uword sr = chain;

	sr |= (uword)*c++ << 4;			/* 1 */
	LARc[0] = sr & 0x3f;  sr >>= 6;
	LARc[1] = sr & 0x3f;  sr >>= 6;
	sr = *c++;
	LARc[2] = sr & 0x1f;  sr >>= 5;
	sr |= (uword)*c++ << 3;
	LARc[3] = sr & 0x1f;  sr >>= 5;
	LARc[4] = sr & 0xf;  sr >>= 4;
	sr |= (uword)*c++ << 2;
	LARc[5] = sr & 0xf;  sr >>= 4;
	LARc[6] = sr & 0x7;  sr >>= 3;
	LARc[7] = sr & 0x7;  sr >>= 3;
	sr = *c++;				/* 5 */
	Nc[0] = sr & 0x7f;  sr >>= 7;
	sr |= (uword)*c++ << 1;
	bc[0] = sr & 0x3;  sr >>= 2;
	Mc[0] = sr & 0x3;  sr >>= 2;
	sr |= (uword)*c++ << 5;
	xmaxc[0] = sr & 0x3f;  sr >>= 6;
	xmc[0] = sr & 0x7;  sr >>= 3;
	xmc[1] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 1;
	xmc[2] = sr & 0x7;  sr >>= 3;
	xmc[3] = sr & 0x7;  sr >>= 3;
	xmc[4] = sr & 0x7;  sr >>= 3;
	sr = *c++;
	xmc[5] = sr & 0x7;  sr >>= 3;
	xmc[6] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 2;			/* 10 */
	xmc[7] = sr & 0x7;  sr >>= 3;
	xmc[8] = sr & 0x7;  sr >>= 3;
	xmc[9] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 1;
	xmc[10] = sr & 0x7;  sr >>= 3;
	xmc[11] = sr & 0x7;  sr >>= 3;
	xmc[12] = sr & 0x7;  sr >>= 3;
	sr = *c++;
	Nc[1] = sr & 0x7f;  sr >>= 7;
	sr |= (uword)*c++ << 1;
	bc[1] = sr & 0x3;  sr >>= 2;
	Mc[1] = sr & 0x3;  sr >>= 2;
	sr |= (uword)*c++ << 5;
	xmaxc[1] = sr & 0x3f;  sr >>= 6;
	xmc[13] = sr & 0x7;  sr >>= 3;
	xmc[14] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 1;			/* 15 */
	xmc[15] = sr & 0x7;  sr >>= 3;
	xmc[16] = sr & 0x7;  sr >>= 3;
	xmc[17] = sr & 0x7;  sr >>= 3;
	sr = *c++;
	xmc[18] = sr & 0x7;  sr >>= 3;
	xmc[19] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 2;
	xmc[20] = sr & 0x7;  sr >>= 3;
	xmc[21] = sr & 0x7;  sr >>= 3;
	xmc[22] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 1;
	xmc[23] = sr & 0x7;  sr >>= 3;
	xmc[24] = sr & 0x7;  sr >>= 3;
	xmc[25] = sr & 0x7;  sr >>= 3;
	sr = *c++;
	Nc[2] = sr & 0x7f;  sr >>= 7;
	sr |= (uword)*c++ << 1;			/* 20 */
	bc[2] = sr & 0x3;  sr >>= 2;
	Mc[2] = sr & 0x3;  sr >>= 2;
	sr |= (uword)*c++ << 5;
	xmaxc[2] = sr & 0x3f;  sr >>= 6;
	xmc[26] = sr & 0x7;  sr >>= 3;
	xmc[27] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 1;
	xmc[28] = sr & 0x7;  sr >>= 3;
	xmc[29] = sr & 0x7;  sr >>= 3;
	xmc[30] = sr & 0x7;  sr >>= 3;
	sr = *c++;
	xmc[31] = sr & 0x7;  sr >>= 3;
	xmc[32] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 2;
	xmc[33] = sr & 0x7;  sr >>= 3;
	xmc[34] = sr & 0x7;  sr >>= 3;
	xmc[35] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 1;			/* 25 */
	xmc[36] = sr & 0x7;  sr >>= 3;
	xmc[37] = sr & 0x7;  sr >>= 3;
	xmc[38] = sr & 0x7;  sr >>= 3;
	sr = *c++;
	Nc[3] = sr & 0x7f;  sr >>= 7;
	sr |= (uword)*c++ << 1;
	bc[3] = sr & 0x3;  sr >>= 2;
	Mc[3] = sr & 0x3;  sr >>= 2;
	sr |= (uword)*c++ << 5;
	xmaxc[3] = sr & 0x3f;  sr >>= 6;
	xmc[39] = sr & 0x7;  sr >>= 3;
	xmc[40] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 1;
	xmc[41] = sr & 0x7;  sr >>= 3;
	xmc[42] = sr & 0x7;  sr >>= 3;
	xmc[43] = sr & 0x7;  sr >>= 3;
	sr = *c++;				/* 30 */
	xmc[44] = sr & 0x7;  sr >>= 3;
	xmc[45] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 2;
	xmc[46] = sr & 0x7;  sr >>= 3;
	xmc[47] = sr & 0x7;  sr >>= 3;
	xmc[48] = sr & 0x7;  sr >>= 3;
	sr |= (uword)*c++ << 1;
	xmc[49] = sr & 0x7;  sr >>= 3;
	xmc[50] = sr & 0x7;  sr >>= 3;
	xmc[51] = sr & 0x7;  sr >>= 3;
}

#endif /* WAV49 */

/*
 *  Decode one good 160-sample frame from the six parameter arrays and
 *  record it as the most recent good frame.  Mirrors the per-subframe
 *  loop of Gsm_Decoder() (RPE decoding, long-term synthesis with the
 *  40..120 / nrp fallback rule, short-term synthesis and deemphasis),
 *  additionally capturing the RPE-dequantized 40-point excitation,
 *  the effective Nc and the coded bc of every subframe.
 */
static void decode_good_frame P8((S, LARc, Nc, bc, Mc, xmaxc, xmc, target),
	struct gsm_state * S,
	word * LARc, word * Nc, word * bc, word * Mc, word * xmaxc,
	word * xmc, word * target)
{
	int	j, k;
	word	erp[40], wt[160];
	word *	drp = S->dp0 + 120;

	for (j = 0; j <= 3; j++, xmaxc++, bc++, Nc++, Mc++, xmc += 13) {

		Gsm_RPE_Decoding(S, *xmaxc, *Mc, xmc, erp);
		Gsm_Long_Term_Synthesis_Filtering(S, *Nc, *bc, erp, drp);

		/* S->nrp now holds the effective Nc after fallback */
		S->plc_nc[j] = S->nrp;
		S->plc_bc[j] = *bc;
		for (k = 0; k <= 39; k++) {
			wt[j * 40 + k] = drp[k];
			S->plc_e[j][k] = erp[k];
		}
	}

	for (k = 0; k <= 7; k++) S->plc_lar[k] = LARc[k];
	S->plc_have = 1;

	Gsm_Short_Term_Synthesis_Filter(S, LARc, wt, target);
	postprocess(S, target);
}

/*
 *  Synthesize one concealed 160-sample frame.  Does not touch the
 *  cached good-frame parameters.  n is the 1-based consecutive loss
 *  count for this half frame.
 */
static void conceal_frame P3((S, n, target),
	struct gsm_state * S, int n, word * target)
{
	int	j, k;
	word	wt[160];
	word *	base;
	word	g;
	const word * lar;

	if (n >= 1 && n <= 5)	g = plc_gain[n - 1];
	else			g = 0;		/* sixth and later */

	base = S->dp0 + 120;

	if (!S->plc_have) {
		/* No history: zero residual, all-zero LARc. */
		word zero_lar[8];

		for (k = 0; k < 160; k++) wt[k] = 0;
		for (k = 0; k < 8; k++) zero_lar[k] = 0;
		lar = zero_lar;

		/* Still run the filters, so their tails are advanced
		 * exactly as on a normal frame.
		 */
		Gsm_Short_Term_Synthesis_Filter(S, (word *)lar, wt, target);
		postprocess(S, target);
		return;
	}

	lar = S->plc_lar;

	for (j = 0; j <= 3; j++) {
		word	brp = gsm_QLB[S->plc_bc[j]];
		word	Nr  = S->plc_nc[j];	/* already 40..120 */
		word *	sub = base;		/* base stays fixed, as in
						 * the library decoder */

		S->nrp = Nr;

		for (k = 0; k <= 39; k++) {
			word p = GSM_MULT_R(brp, sub[k - Nr]);
			word r = sat16((longword)q15(S->plc_e[j][k], g)
				     + (longword)q15(p, g));
			sub[k] = r;		/* enters later prediction */
			wt[j * 40 + k] = r;
		}

		/* Same history update as the library long-term filter. */
		for (k = 0; k <= 119; k++)
			base[-120 + k] = base[-80 + k];
	}

	Gsm_Short_Term_Synthesis_Filter(S, (word *)lar, wt, target);
	postprocess(S, target);
}

/*
 *  Public block entry.  All validation completes before any decoder
 *  state or output sample is modified.
 */
int gsm_decode_plc P6((s, data, bytes, good_mask, pcm, capacity),
	gsm s, const gsm_byte * data, unsigned int bytes,
	unsigned int good_mask, gsm_signal * pcm, unsigned int capacity)
{
	word	LARc[8], Nc[4], Mc[4], bc[4], xmaxc[4], xmc[13 * 4];

	if (s == (gsm)0 || pcm == (gsm_signal *)0) return -1;

#ifdef WAV49
	if (s->wav_fmt) {
		gsm_byte chain;
		int h;

		/* Must be sitting on a whole-block boundary. */
		if (s->frame_index != 0) return -1;
		if (capacity != 320)    return -1;
		if (good_mask & ~0x3u)  return -1;	/* mask out of range */

		if (good_mask) {
			if (data == (const gsm_byte *)0 || bytes != 65)
				return -1;
		} else if (data != (const gsm_byte *)0) {
			if (bytes != 65) return -1;
		} else if (bytes != 0) {
			return -1;
		}

		/* From here on the call succeeds: mutate state/output. */
		for (h = 0; h <= 1; h++) {
			int good = (good_mask >> h) & 1;
			word * out = (word *)(pcm + h * 160);

			if (good) {
				if (h == 0) {
					unpack_wav_even(data, LARc, Nc, bc, Mc,
						xmaxc, xmc, &chain);
					/* Only a good even half supplies the
					 * carry nibble; a lost even half must
					 * never feed its bytes into chaining.
					 */
					s->frame_chain = chain;
				} else {
					/* The odd half's carry nibble is the
					 * high nibble of the shared byte 32;
					 * reading it here lets a good odd
					 * half decode even if the even half
					 * was lost.
					 */
					chain = (gsm_byte)(data[32] >> 4);
					unpack_wav_odd(chain, data + 33,
						LARc, Nc, bc, Mc,
						xmaxc, xmc);
				}

				s->plc_lost[h] = 0;
				decode_good_frame(s, LARc, Nc, bc, Mc,
					xmaxc, xmc, out);
			} else {
				s->plc_lost[h] = bump_lost(s->plc_lost[h]);
				conceal_frame(s, s->plc_lost[h], out);
			}
		}

		/* frame_index stays 0: a whole block was consumed. */
		return 320;
	}
#endif /* WAV49 */

	/* Ordinary mode: one 33-byte / 160-sample block. */
	if (capacity != 160)    return -1;
	if (good_mask & ~0x1u)  return -1;		/* mask out of range */

	if (good_mask & 1u) {
		if (data == (const gsm_byte *)0 || bytes != 33) return -1;
		if (((data[0] >> 4) & 0x0F) != GSM_MAGIC)    return -1;
	} else if (data != (const gsm_byte *)0) {
		if (bytes != 33) return -1;
	} else if (bytes != 0) {
		return -1;
	}

	/* From here on the call succeeds. */
	if (good_mask & 1u) {
		if (unpack_normal(data, LARc, Nc, bc, Mc, xmaxc, xmc) != 0)
			return -1;
		s->plc_lost[0] = 0;
		decode_good_frame(s, LARc, Nc, bc, Mc, xmaxc, xmc,
			(word *)pcm);
	} else {
		s->plc_lost[0] = bump_lost(s->plc_lost[0]);
		conceal_frame(s, s->plc_lost[0], (word *)pcm);
	}
	return 160;
}

/*
 *  Helpers used by the legacy gsm_decode() entry so both entries share
 *  the exact same decode and good-frame bookkeeping.
 */
int gsm_plc_legacy_normal P3((s, c, target),
	gsm s, gsm_byte * c, gsm_signal * target)
{
	word	LARc[8], Nc[4], Mc[4], bc[4], xmaxc[4], xmc[13 * 4];

	if (unpack_normal(c, LARc, Nc, bc, Mc, xmaxc, xmc) != 0) return -1;
	s->plc_lost[0] = 0;
	decode_good_frame(s, LARc, Nc, bc, Mc, xmaxc, xmc, (word *)target);
	return 0;
}

#ifdef WAV49
int gsm_plc_legacy_wav P3((s, c, target),
	gsm s, gsm_byte * c, gsm_signal * target)
{
	word	LARc[8], Nc[4], Mc[4], bc[4], xmaxc[4], xmc[13 * 4];
	gsm_byte chain;
	int	half;
	int	slot;		/* 0 = first/even half, 1 = second/odd */

	s->frame_index = (unsigned char)!s->frame_index;
	half = s->frame_index;		/* 1 = first/even, 0 = second/odd */
	slot = half ? 0 : 1;

	if (half) {
		unpack_wav_even(c, LARc, Nc, bc, Mc, xmaxc, xmc, &chain);
		s->frame_chain = chain;
	} else {
		unpack_wav_odd(s->frame_chain, c, LARc, Nc, bc, Mc,
			xmaxc, xmc);
	}

	s->plc_lost[slot] = 0;
	decode_good_frame(s, LARc, Nc, bc, Mc, xmaxc, xmc, (word *)target);
	return 0;
}
#endif /* WAV49 */
