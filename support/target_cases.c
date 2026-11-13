/*
** Target cases for SFD_TPDF_SHAPED. Registered through cases.h; this file
** never defines main(). All signals are generated in code from dyadic
** rationals so float and double entry points carry identical values.
**
** Three groups:
**   numeric : per sample verification against an independent double
**             precision recurrence, driven by the actual u/v variates
**             exposed by the test only observer (never production e or q).
**   stream  : chunking, float/double mixing, interleaved files and
**             reconfiguration across the bounded internal work buffers.
**   compat  : legacy byte output, parameter errors, containers, bit
**             widths, integer insertion and non-finite input sentinels.
*/

#include "cases.h"

#include <sndfile.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test only read only hook, exported by the static archive but absent from
** the public symbol lists. It exposes PRNG state and the drawn variates;
** it never exposes the production error term or the quantized integer. */
extern int sf_tpdf_test_observer (SNDFILE *sndfile, int channel, uint64_t *base_seed, int *bits,
								sf_count_t *samples, double *last_u, double *last_v, uint64_t *rng_state) ;

#define	TC_SEED			((uint64_t) 0x2545F4914F6CDD1DULL)
#define	TC_SEED2		((uint64_t) 0x0123456789ABCDEFULL)
#define	TC_MAX_CHANNELS	8

/* Independent SplitMix64 copy for the test's own expectation stream. */
static uint64_t
tc_splitmix64 (uint64_t *state)
{	uint64_t z ;

	*state += 0x9E3779B97F4A7C15ULL ;
	z = *state ;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL ;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL ;
	return z ^ (z >> 31) ;
} /* tc_splitmix64 */

static uint64_t
tc_channel_state (uint64_t base_seed, int channel)
{	uint64_t mixed = base_seed ^ (0x9E3779B97F4A7C15ULL * ((uint64_t) channel + 1)) ;

	return tc_splitmix64 (&mixed) ;
} /* tc_channel_state */

static double
tc_uniform (uint64_t word)
{
	return (double) (word >> 11) * (1.0 / 9007199254740992.0) ;
} /* tc_uniform */

typedef struct
{	uint64_t	state ;
	double		error ;
} TC_REF ;

static void
tc_ref_init (TC_REF *r, uint64_t base_seed, int channel)
{	r->state = tc_channel_state (base_seed, channel) ;
	r->error = 0.0 ;
} /* tc_ref_init */

/* Independent double precision quantization recurrence. Mirrors the
** documented rule: z = x*2^(B-1) + e ; q = round(z + u - v) with half away
** from zero ; saturation clears e, otherwise e = z - q (no dither in e). */
static int
tc_ref_step (TC_REF *r, double input, int bits, double u, double v)
{	double	scale = (bits == 16) ? 32768.0 : 8388608.0 ;
	int		qmin = -(1 << (bits - 1)) ;
	int		qmax = (1 << (bits - 1)) - 1 ;
	double	x, z, rr ;
	int		q ;

	if (! isfinite (input))
	{	r->error = 0.0 ;
		if (input > 0.0)
			return qmax ;
		if (input < 0.0)
			return qmin ;
		return 0 ;
		} ;

	x = input * scale ;
	z = x + r->error ;
	rr = round (z + u - v) ;

	if (rr >= (double) qmax + 1.0)
	{	r->error = 0.0 ;
		return qmax ;
		} ;
	if (rr <= (double) qmin - 1.0)
	{	r->error = 0.0 ;
		return qmin ;
		} ;

	q = (int) rr ;
	r->error = z - q ;
	return q ;
} /* tc_ref_step */

/* Deliberately wrong feedback: the dither noise is folded into the error
** term. The numeric cases prove the on-disk PCM does not follow this rule. */
static int
tc_wrong_step (TC_REF *r, double input, int bits, double u, double v)
{	double	scale = (bits == 16) ? 32768.0 : 8388608.0 ;
	int		qmin = -(1 << (bits - 1)) ;
	int		qmax = (1 << (bits - 1)) - 1 ;
	double	x, z, rr ;
	int		q ;

	if (! isfinite (input))
	{	r->error = 0.0 ;
		if (input > 0.0)
			return qmax ;
		if (input < 0.0)
			return qmin ;
		return 0 ;
		} ;

	x = input * scale ;
	z = x + r->error ;
	rr = round (z + u - v) ;

	if (rr >= (double) qmax + 1.0)
	{	r->error = 0.0 ;
		return qmax ;
		} ;
	if (rr <= (double) qmin - 1.0)
	{	r->error = 0.0 ;
		return qmin ;
		} ;

	q = (int) rr ;
	r->error = (z + u - v) - q ;		/* wrong: dither leaks into feedback */
	return q ;
} /* tc_wrong_step */

static SNDFILE *
tc_open_write (const char *path, int container, int sub, int channels)
{	SF_INFO	info ;

	memset (&info, 0, sizeof (info)) ;
	info.samplerate = 48000 ;
	info.channels = channels ;
	info.format = container | sub ;
	return sf_open (path, SFM_WRITE, &info) ;
} /* tc_open_write */

static int
tc_arm (SNDFILE *f, uint64_t seed)
{	SF_DITHER_INFO	di ;

	memset (&di, 0, sizeof (di)) ;
	di.type = SFD_TPDF_SHAPED ;
	di.level = 1.0 ;

	if (sf_command (f, SFC_SET_DITHER_SEED, &seed, sizeof (seed)) != 0)
		return -1 ;
	if (sf_command (f, SFC_SET_DITHER_ON_WRITE, &di, sizeof (di)) != 0)
		return -2 ;
	return 0 ;
} /* tc_arm */

static int
tc_read_ints (const char *path, sf_count_t frames, int channels, int *out)
{	SF_INFO		info ;
	SNDFILE		*f ;
	sf_count_t	got ;

	memset (&info, 0, sizeof (info)) ;
	if ((f = sf_open (path, SFM_READ, &info)) == NULL)
		return -1 ;
	if (info.channels != channels)
	{	sf_close (f) ;
		return -2 ;
		} ;
	got = sf_readf_int (f, out, frames) ;
	sf_close (f) ;
	return (got == frames) ? 0 : -3 ;
} /* tc_read_ints */

static int
tc_shift (int sub)
{
	return (sub == SF_FORMAT_PCM_24) ? 8 : 16 ;
} /* tc_shift */

static int
tc_file_bytes (const char *path, unsigned char *buf, int cap)
{	FILE	*f ;
	size_t	n ;

	f = fopen (path, "rb") ;
	if (f == NULL)
		return -1 ;
	n = fread (buf, 1, (size_t) cap, f) ;
	fclose (f) ;
	return (int) n ;
} /* tc_file_bytes */

/* Write one frame at a time, capture the actual u/v through the observer,
** and build the fully independent expected q sequence. */
static int
tc_observer_reference (const char *path, int sub, int channels, sf_count_t frames,
						const double *input, uint64_t seed, int *expect,
						double *cap_u, double *cap_v)
{	SNDFILE		*f ;
	TC_REF		ref [TC_MAX_CHANNELS] ;
	int			bits = (sub == SF_FORMAT_PCM_24) ? 24 : 16 ;
	sf_count_t	k ;
	int			ch ;

	f = tc_open_write (path, SF_FORMAT_WAV, sub, channels) ;
	if (f == NULL)
		return -1 ;
	if (tc_arm (f, seed) != 0)
	{	sf_close (f) ;
		return -2 ;
		} ;

	for (ch = 0 ; ch < channels ; ch++)
		tc_ref_init (&ref [ch], seed, ch) ;

	for (k = 0 ; k < frames ; k++)
	{	if (sf_writef_double (f, input + k * channels, 1) != 1)
		{	sf_close (f) ;
			return -3 ;
			} ;

		for (ch = 0 ; ch < channels ; ch++)
		{	uint64_t	base_seed, rng_state ;
			int			got_bits ;
			sf_count_t	drawn ;
			double		u, v ;
			uint64_t	word ;
			double		eu, ev ;

			if (sf_tpdf_test_observer (f, ch, &base_seed, &got_bits, &drawn, &u, &v, &rng_state) != 0)
			{	sf_close (f) ;
				return -4 ;
				} ;

			/* Independent prediction of the same variates and state. */
			word = tc_splitmix64 (&ref [ch].state) ;
			eu = tc_uniform (word) ;
			word = tc_splitmix64 (&ref [ch].state) ;
			ev = tc_uniform (word) ;

			if (eu != u || ev != v || rng_state != ref [ch].state ||
					base_seed != seed || got_bits != bits || drawn != k + 1)
			{	sf_close (f) ;
				return -5 ;
				} ;

			if (cap_u != NULL)
				cap_u [k * channels + ch] = u ;
			if (cap_v != NULL)
				cap_v [k * channels + ch] = v ;

			expect [k * channels + ch] = tc_ref_step (&ref [ch], input [k * channels + ch], bits, u, v) ;
			} ;
		} ;

	if (sf_close (f) != 0)
		return -6 ;
	return 0 ;
} /* tc_observer_reference */

/* Read a dithered file back and convert the left aligned ints into B bit
** signed code values q. */
static int
tc_read_codes (const char *path, int sub, int channels, sf_count_t frames, int *codes)
{	int	shift = tc_shift (sub) ;
	int	rc, i ;

	rc = tc_read_ints (path, frames, channels, codes) ;
	if (rc != 0)
		return rc ;

	for (i = 0 ; i < frames * channels ; i++)
		codes [i] = codes [i] / (1 << shift) ;

	return 0 ;
} /* tc_read_codes */

/*============================================================================
** numeric group
*/

/* Sub-LSB DC: dither must toggle the output around zero with both signs,
** match the independent recurrence sample by sample, and reject the wrong
** feedback formula. */
TASK_CASE ("numeric", sub_lsb_dc)
{	enum { FRAMES = 64, CH = 2, N = FRAMES * CH } ;
	double		in [N] ;
	int			expect [N], actual [N], wrong [N] ;
	double		uu [N], vv [N] ;
	TC_REF		wref [CH] ;
	int			rc, i, positives, negatives, noise_both, wrong_mismatch ;

	for (i = 0 ; i < N ; i++)
		in [i] = 1.0 / 131072.0 ;			/* 0.25 of a 16 bit LSB */

	rc = tc_observer_reference ("tc_num_dc_obs.wav", SF_FORMAT_PCM_16, CH, FRAMES, in, TC_SEED, expect, uu, vv) ;
	TASK_CHECK (rc == 0) ;
	rc = tc_read_codes ("tc_num_dc_obs.wav", SF_FORMAT_PCM_16, CH, FRAMES, actual) ;
	TASK_CHECK (rc == 0) ;

	for (i = 0 ; i < N ; i++)
		TASK_CHECK (actual [i] == expect [i]) ;

	/* The same signal written in one bulk call must be identical. */
	{	SNDFILE	*f = tc_open_write ("tc_num_dc_bulk.wav", SF_FORMAT_WAV, SF_FORMAT_PCM_16, CH) ;
		int		bulk [N] ;
		TASK_CHECK (f != NULL) ;
		TASK_CHECK (tc_arm (f, TC_SEED) == 0) ;
		TASK_CHECK (sf_writef_double (f, in, FRAMES) == FRAMES) ;
		TASK_CHECK (sf_close (f) == 0) ;
		TASK_CHECK (tc_read_codes ("tc_num_dc_bulk.wav", SF_FORMAT_PCM_16, CH, FRAMES, bulk) == 0) ;
		for (i = 0 ; i < N ; i++)
			TASK_CHECK (bulk [i] == expect [i]) ;
		remove ("tc_num_dc_bulk.wav") ;
		} ;

	positives = negatives = noise_both = 0 ;
	for (i = 0 ; i < N ; i++)
	{	if (actual [i] > 0)
			positives++ ;
		if (actual [i] < 0)
			negatives++ ;
		if (uu [i] - vv [i] > 0.0 || vv [i] - uu [i] > 0.0)
			noise_both++ ;
		} ;
	TASK_CHECK (positives > 0 && negatives > 0) ;
	TASK_CHECK (noise_both == N) ;
	TASK_CHECK (positives + negatives > 0) ;

	/* Wrong feedback formula must diverge from the recorded PCM. */
	for (i = 0 ; i < CH ; i++)
		tc_ref_init (&wref [i], TC_SEED, i) ;
	wrong_mismatch = 0 ;
	for (i = 0 ; i < N ; i++)
	{	int ch = i % CH ;
		wrong [i] = tc_wrong_step (&wref [ch], in [i], 16, uu [i], vv [i]) ;
		if (wrong [i] != actual [i])
			wrong_mismatch++ ;
		} ;
	TASK_CHECK (wrong_mismatch > 0) ;

	remove ("tc_num_dc_obs.wav") ;
} /* sub_lsb_dc */

/* Non-flat sub-LSB signal over both target bit widths. */
TASK_CASE ("numeric", small_varying_signal)
{	enum { FRAMES = 72, CH = 2, N = FRAMES * CH } ;
	double		in16 [N], in24 [N] ;
	int			exp16 [N], got16 [N], exp24 [N], got24 [N] ;
	int			rc, i, nonzero ;

	for (i = 0 ; i < N ; i++)
	{	in16 [i] = ((i % 7) - 3) / 262144.0 ;			/* within +-0.375 LSB16 */
		in24 [i] = ((i % 7) - 3) / (262144.0 * 256.0) ;	/* same fraction of a 24 bit LSB */
		} ;

	rc = tc_observer_reference ("tc_num_var16.wav", SF_FORMAT_PCM_16, CH, FRAMES, in16, TC_SEED2, exp16, NULL, NULL) ;
	TASK_CHECK (rc == 0) ;
	TASK_CHECK (tc_read_codes ("tc_num_var16.wav", SF_FORMAT_PCM_16, CH, FRAMES, got16) == 0) ;
	for (i = 0 ; i < N ; i++)
		TASK_CHECK (got16 [i] == exp16 [i]) ;

	rc = tc_observer_reference ("tc_num_var24.wav", SF_FORMAT_PCM_24, CH, FRAMES, in24, TC_SEED2, exp24, NULL, NULL) ;
	TASK_CHECK (rc == 0) ;
	TASK_CHECK (tc_read_codes ("tc_num_var24.wav", SF_FORMAT_PCM_24, CH, FRAMES, got24) == 0) ;
	for (i = 0 ; i < N ; i++)
		TASK_CHECK (got24 [i] == exp24 [i]) ;

	nonzero = 0 ;
	for (i = 0 ; i < N ; i++)
		if (got16 [i] != 0 || got24 [i] != 0)
			nonzero++ ;
	TASK_CHECK (nonzero > 0) ;

	remove ("tc_num_var16.wav") ;
	remove ("tc_num_var24.wav") ;
} /* small_varying_signal */

/* Opposite polarity channels never borrow state from each other, and a
** change in the right channel leaves the left channel bit identical. */
TASK_CASE ("numeric", channel_independence)
{	enum { FRAMES = 48, CH = 2, N = FRAMES * CH } ;
	double		a [N], b [N] ;
	int			qa [N], qb [N], rc, i ;

	for (i = 0 ; i < FRAMES ; i++)
	{	a [2 * i] = 1.0 / 65536.0 ;		/* +0.5 LSB */
		a [2 * i + 1] = -1.0 / 65536.0 ;	/* -0.5 LSB */
		b [2 * i] = a [2 * i] ;
		b [2 * i + 1] = 3.0 / 65536.0 ;		/* right channel changed only */
		} ;

	rc = tc_observer_reference ("tc_num_ind_a.wav", SF_FORMAT_PCM_16, CH, FRAMES, a, TC_SEED, qa, NULL, NULL) ;
	TASK_CHECK (rc == 0) ;
	rc = tc_observer_reference ("tc_num_ind_b.wav", SF_FORMAT_PCM_16, CH, FRAMES, b, TC_SEED, qb, NULL, NULL) ;
	TASK_CHECK (rc == 0) ;

	for (i = 0 ; i < FRAMES ; i++)
		TASK_CHECK (qa [2 * i] == qb [2 * i]) ;	/* left channel untouched */

	/* The two channels of the same file follow their own recurrences. */
	{	TC_REF r0, r1 ;
		tc_ref_init (&r0, TC_SEED, 0) ;
		tc_ref_init (&r1, TC_SEED, 1) ;
		for (i = 0 ; i < FRAMES ; i++)
		{	/* Re-derive variates independently per channel. */
			double u0 = tc_uniform (tc_splitmix64 (&r0.state)) ;
			double v0 = tc_uniform (tc_splitmix64 (&r0.state)) ;
			double u1 = tc_uniform (tc_splitmix64 (&r1.state)) ;
			double v1 = tc_uniform (tc_splitmix64 (&r1.state)) ;
			TASK_CHECK (tc_ref_step (&r0, a [2 * i], 16, u0, v0) == qa [2 * i]) ;
			TASK_CHECK (tc_ref_step (&r1, a [2 * i + 1], 16, u1, v1) == qa [2 * i + 1]) ;
			} ;
		} ;

	remove ("tc_num_ind_a.wav") ;
	remove ("tc_num_ind_b.wav") ;
} /* channel_independence */

/* Multi-channel cancellation: the three channels sum to zero every frame,
** but every channel keeps its own independent stream and error term. */
TASK_CASE ("numeric", multichannel_cancellation)
{	enum { FRAMES = 56, CH = 3, N = FRAMES * CH } ;
	double		in [N] ;
	int			expect [N], actual [N] ;
	int			rc, i, per_nonzero [CH], any_cancel ;

	for (i = 0 ; i < FRAMES ; i++)
	{	double c = ((i % 9) - 4) / 262144.0 ;
		in [3 * i] = c ;			/* +c */
		in [3 * i + 1] = 0.0 ;		/* silence */
		in [3 * i + 2] = -c ;		/* -c : per frame sum is zero */
		} ;

	rc = tc_observer_reference ("tc_num_cancel.wav", SF_FORMAT_PCM_16, CH, FRAMES, in, TC_SEED2, expect, NULL, NULL) ;
	TASK_CHECK (rc == 0) ;
	TASK_CHECK (tc_read_codes ("tc_num_cancel.wav", SF_FORMAT_PCM_16, CH, FRAMES, actual) == 0) ;

	for (i = 0 ; i < N ; i++)
		TASK_CHECK (actual [i] == expect [i]) ;

	for (i = 0 ; i < CH ; i++)
		per_nonzero [i] = 0 ;
	any_cancel = 0 ;
	for (i = 0 ; i < FRAMES ; i++)
	{	long frame_sum = 0 ;
		for (int ch = 0 ; ch < CH ; ch++)
		{	if (actual [3 * i + ch] != 0)
				per_nonzero [ch]++ ;
			frame_sum += actual [3 * i + ch] ;
			} ;
		/* Even with exact input cancellation the dithered codes need not
		** cancel sample by sample, but they remain small. */
		if (frame_sum != 0)
			any_cancel = 1 ;
		} ;

	/* Both active channels actually dither; the silent middle channel also
	** receives symmetric dither rather than being forced to zero. */
	TASK_CHECK (per_nonzero [0] > 0 && per_nonzero [2] > 0) ;
	TASK_CHECK (any_cancel == 1) ;

	remove ("tc_num_cancel.wav") ;
} /* multichannel_cancellation */

/* Full scale boundaries, hard saturation and recovery with error reset. */
TASK_CASE ("numeric", saturation_and_recovery)
{	enum { FRAMES = 24, CH = 1, N = FRAMES } ;
	double		in [N] ;
	int			expect [N], actual [N] ;
	double		uu [N], vv [N] ;
	int			rc, i ;

	for (i = 0 ; i < N ; i++)
		in [i] = 1.0 / 131072.0 ;
	in [0] = 1.0 ;			/* positive full scale: always hard saturates */
	in [1] = -1.0 ;			/* negative full scale boundary */
	in [2] = 1.5 ;			/* over range positive */
	in [3] = -1.5 ;			/* over range negative */
	in [4] = 1.5 ;
	in [5] = -1.5 ;

	rc = tc_observer_reference ("tc_num_sat.wav", SF_FORMAT_PCM_16, CH, FRAMES, in, TC_SEED, expect, uu, vv) ;
	TASK_CHECK (rc == 0) ;
	TASK_CHECK (tc_read_codes ("tc_num_sat.wav", SF_FORMAT_PCM_16, CH, FRAMES, actual) == 0) ;

	for (i = 0 ; i < N ; i++)
		TASK_CHECK (actual [i] == expect [i]) ;

	TASK_CHECK (actual [0] == 32767) ;
	TASK_CHECK (actual [2] == 32767 && actual [4] == 32767) ;
	TASK_CHECK (actual [3] == -32768 && actual [5] == -32768) ;
	TASK_CHECK (actual [1] >= -32768 && actual [1] <= -32767) ;

	/* After saturation the error term is zero; recovery matches a fresh
	** recurrence seeded at frame 6. */
	{	TC_REF	r ;
		int		q ;
		tc_ref_init (&r, TC_SEED, 0) ;
		/* Advance the independent stream through the first six samples. */
		for (i = 0 ; i < 6 ; i++)
			tc_ref_step (&r, in [i], 16, uu [i], vv [i]) ;
		TASK_CHECK (r.error == 0.0) ;
		for (i = 6 ; i < N ; i++)
		{	q = tc_ref_step (&r, in [i], 16, uu [i], vv [i]) ;
			TASK_CHECK (q == actual [i]) ;
			} ;
		} ;

	remove ("tc_num_sat.wav") ;
} /* saturation_and_recovery */

/* Zero seed must be a fully supported, deterministic state. */
TASK_CASE ("numeric", zero_seed)
{	enum { FRAMES = 40, CH = 2, N = FRAMES * CH } ;
	double		in [N] ;
	int			e1 [N], g1 [N], e2 [N], g2 [N] ;
	int			i, nonzero ;

	for (i = 0 ; i < N ; i++)
		in [i] = ((i % 5) - 2) / 131072.0 ;

	TASK_CHECK (tc_observer_reference ("tc_num_zero_a.wav", SF_FORMAT_PCM_16, CH, FRAMES, in, 0, e1, NULL, NULL) == 0) ;
	TASK_CHECK (tc_observer_reference ("tc_num_zero_b.wav", SF_FORMAT_PCM_16, CH, FRAMES, in, 0, e2, NULL, NULL) == 0) ;
	TASK_CHECK (tc_read_codes ("tc_num_zero_a.wav", SF_FORMAT_PCM_16, CH, FRAMES, g1) == 0) ;
	TASK_CHECK (tc_read_codes ("tc_num_zero_b.wav", SF_FORMAT_PCM_16, CH, FRAMES, g2) == 0) ;

	nonzero = 0 ;
	for (i = 0 ; i < N ; i++)
	{	TASK_CHECK (g1 [i] == e1 [i]) ;
		TASK_CHECK (g2 [i] == e2 [i]) ;
		TASK_CHECK (g1 [i] == g2 [i]) ;
		if (g1 [i] != 0)
			nonzero++ ;
		} ;
	TASK_CHECK (nonzero > 0) ;

	remove ("tc_num_zero_a.wav") ;
	remove ("tc_num_zero_b.wav") ;
} /* zero_seed */

/*============================================================================
** stream group. Three channels, 3000 frames = 9000 samples, crossing the
** 2048 sample TPDF work buffer and the lower level PCM buffers repeatedly.
*/

#define	TC_STREAM_FRAMES	3000
#define	TC_STREAM_CH		3
#define	TC_STREAM_N			(TC_STREAM_FRAMES * TC_STREAM_CH)

static void
tc_stream_signal_double (double *d, sf_count_t frames)
{	sf_count_t i ;

	for (i = 0 ; i < frames ; i++)
	{	d [3 * i] = ((int) (i % 13) - 6) / 262144.0 ;
		d [3 * i + 1] = ((int) (i % 5) - 2) / 131072.0 ;
		d [3 * i + 2] = ((int) (i % 29) - 14) / 524288.0 ;
		} ;
} /* tc_stream_signal_double */

static void
tc_stream_signal_float (const double *d, float *f, sf_count_t frames)
{	sf_count_t i ;

	for (i = 0 ; i < frames * TC_STREAM_CH ; i++)
		f [i] = (float) d [i] ;		/* exact: dyadic values */
} /* tc_stream_signal_float */

static int
tc_write_all_double (const char *path, const double *d, sf_count_t frames)
{	SNDFILE *f = tc_open_write (path, SF_FORMAT_WAV, SF_FORMAT_PCM_16, TC_STREAM_CH) ;
	if (f == NULL)
		return -1 ;
	if (tc_arm (f, TC_SEED) != 0)
	{	sf_close (f) ;
		return -2 ;
		} ;
	if (sf_writef_double (f, d, frames) != frames)
	{	sf_close (f) ;
		return -3 ;
		} ;
	if (sf_close (f) != 0)
		return -4 ;
	return 0 ;
} /* tc_write_all_double */

static int
tc_codes_equal (const char *pa, const char *pb, sf_count_t frames, int *same_out)
{	int	*a, *b ;
	int	rc, i, same ;

	a = malloc ((size_t) frames * TC_STREAM_CH * sizeof (int)) ;
	b = malloc ((size_t) frames * TC_STREAM_CH * sizeof (int)) ;
	if (a == NULL || b == NULL)
	{	free (a) ; free (b) ; return -1 ; }

	rc = tc_read_codes (pa, SF_FORMAT_PCM_16, TC_STREAM_CH, frames, a) ;
	if (rc == 0)
		rc = tc_read_codes (pb, SF_FORMAT_PCM_16, TC_STREAM_CH, frames, b) ;
	same = (rc == 0) ;
	if (same)
		for (i = 0 ; i < frames * TC_STREAM_CH ; i++)
			if (a [i] != b [i])
			{	same = 0 ;
				break ;
				} ;

	free (a) ; free (b) ;
	if (same_out != NULL)
		*same_out = same ;
	return rc ;
} /* tc_codes_equal */

TASK_CASE ("stream", chunking_matches_bulk)
{	static double	d [TC_STREAM_N] ;
	static float	fl [TC_STREAM_N] ;
	int				same ;
	SNDFILE			*f ;
	sf_count_t		off ;
	const sf_count_t	schedule [] = { 1, 5, 300, 700, 999 } ;
	unsigned		s ;

	tc_stream_signal_double (d, TC_STREAM_FRAMES) ;
	tc_stream_signal_float (d, fl, TC_STREAM_FRAMES) ;

	TASK_CHECK (tc_write_all_double ("tc_str_ref.wav", d, TC_STREAM_FRAMES) == 0) ;

	/* Frame by frame. */
	f = tc_open_write ("tc_str_one.wav", SF_FORMAT_WAV, SF_FORMAT_PCM_16, TC_STREAM_CH) ;
	TASK_CHECK (f != NULL && tc_arm (f, TC_SEED) == 0) ;
	for (off = 0 ; off < TC_STREAM_FRAMES ; off++)
		TASK_CHECK (sf_writef_double (f, d + off * TC_STREAM_CH, 1) == 1) ;
	TASK_CHECK (sf_close (f) == 0) ;
	TASK_CHECK (tc_codes_equal ("tc_str_ref.wav", "tc_str_one.wav", TC_STREAM_FRAMES, &same) == 0) ;
	TASK_CHECK (same) ;
	remove ("tc_str_one.wav") ;

	/* Irregular chunks crossing every internal buffer boundary. */
	f = tc_open_write ("tc_str_chunk.wav", SF_FORMAT_WAV, SF_FORMAT_PCM_16, TC_STREAM_CH) ;
	TASK_CHECK (f != NULL && tc_arm (f, TC_SEED) == 0) ;
	off = 0 ;
	s = 0 ;
	while (off < TC_STREAM_FRAMES)
	{	sf_count_t n = schedule [s % (sizeof (schedule) / sizeof (schedule [0]))] ;
		if (n > TC_STREAM_FRAMES - off)
			n = TC_STREAM_FRAMES - off ;
		TASK_CHECK (sf_writef_double (f, d + off * TC_STREAM_CH, n) == n) ;
		off += n ;
		s++ ;
		} ;
	TASK_CHECK (sf_close (f) == 0) ;
	TASK_CHECK (tc_codes_equal ("tc_str_ref.wav", "tc_str_chunk.wav", TC_STREAM_FRAMES, &same) == 0) ;
	TASK_CHECK (same) ;
	remove ("tc_str_chunk.wav") ;

	/* Float only, then mixed float/double in one stream. */
	f = tc_open_write ("tc_str_float.wav", SF_FORMAT_WAV, SF_FORMAT_PCM_16, TC_STREAM_CH) ;
	TASK_CHECK (f != NULL && tc_arm (f, TC_SEED) == 0) ;
	TASK_CHECK (sf_writef_float (f, fl, TC_STREAM_FRAMES) == TC_STREAM_FRAMES) ;
	TASK_CHECK (sf_close (f) == 0) ;
	TASK_CHECK (tc_codes_equal ("tc_str_ref.wav", "tc_str_float.wav", TC_STREAM_FRAMES, &same) == 0) ;
	TASK_CHECK (same) ;
	remove ("tc_str_float.wav") ;

	f = tc_open_write ("tc_str_mix.wav", SF_FORMAT_WAV, SF_FORMAT_PCM_16, TC_STREAM_CH) ;
	TASK_CHECK (f != NULL && tc_arm (f, TC_SEED) == 0) ;
	TASK_CHECK (sf_writef_float (f, fl, TC_STREAM_FRAMES / 2) == TC_STREAM_FRAMES / 2) ;
	TASK_CHECK (sf_writef_double (f, d + (TC_STREAM_FRAMES / 2) * TC_STREAM_CH,
									TC_STREAM_FRAMES - TC_STREAM_FRAMES / 2) ==
									TC_STREAM_FRAMES - TC_STREAM_FRAMES / 2) ;
	TASK_CHECK (sf_close (f) == 0) ;
	TASK_CHECK (tc_codes_equal ("tc_str_ref.wav", "tc_str_mix.wav", TC_STREAM_FRAMES, &same) == 0) ;
	TASK_CHECK (same) ;
	remove ("tc_str_mix.wav") ;

	/* Sanity: the reference actually dithers. */
	{	int codes [TC_STREAM_N] ;
		int nonzero = 0, i ;
		TASK_CHECK (tc_read_codes ("tc_str_ref.wav", SF_FORMAT_PCM_16, TC_STREAM_CH, TC_STREAM_FRAMES, codes) == 0) ;
		for (i = 0 ; i < TC_STREAM_N ; i++)
			if (codes [i] != 0)
				nonzero++ ;
		TASK_CHECK (nonzero > 0) ;
		}

	remove ("tc_str_ref.wav") ;
} /* chunking_matches_bulk */

/* Two open files written in alternation keep independent, identical streams. */
TASK_CASE ("stream", interleaved_files)
{	static double	d [TC_STREAM_N] ;
	SNDFILE			*a, *b ;
	sf_count_t		off ;
	int				same ;
	const sf_count_t	step = 137 ;

	tc_stream_signal_double (d, TC_STREAM_FRAMES) ;
	TASK_CHECK (tc_write_all_double ("tc_il_ref.wav", d, TC_STREAM_FRAMES) == 0) ;

	a = tc_open_write ("tc_il_a.wav", SF_FORMAT_WAV, SF_FORMAT_PCM_16, TC_STREAM_CH) ;
	b = tc_open_write ("tc_il_b.wav", SF_FORMAT_WAV, SF_FORMAT_PCM_16, TC_STREAM_CH) ;
	TASK_CHECK (a != NULL && b != NULL) ;
	TASK_CHECK (tc_arm (a, TC_SEED) == 0 && tc_arm (b, TC_SEED) == 0) ;

	off = 0 ;
	while (off < TC_STREAM_FRAMES)
	{	sf_count_t n = step ;
		if (n > TC_STREAM_FRAMES - off)
			n = TC_STREAM_FRAMES - off ;
		TASK_CHECK (sf_writef_double (a, d + off * TC_STREAM_CH, n) == n) ;
		TASK_CHECK (sf_writef_double (b, d + off * TC_STREAM_CH, n) == n) ;
		off += n ;
		} ;
	TASK_CHECK (sf_close (a) == 0 && sf_close (b) == 0) ;

	TASK_CHECK (tc_codes_equal ("tc_il_ref.wav", "tc_il_a.wav", TC_STREAM_FRAMES, &same) == 0) ;
	TASK_CHECK (same) ;
	TASK_CHECK (tc_codes_equal ("tc_il_ref.wav", "tc_il_b.wav", TC_STREAM_FRAMES, &same) == 0) ;
	TASK_CHECK (same) ;

	remove ("tc_il_ref.wav") ;
	remove ("tc_il_a.wav") ;
	remove ("tc_il_b.wav") ;
} /* interleaved_files */

/* Re-arming must not reset the stream or wrap callbacks; disable/re-enable
** restarts from zero error and the originally configured seed. */
TASK_CASE ("stream", reconfigure)
{	static double	d [TC_STREAM_N] ;
	SNDFILE			*f ;
	SF_DITHER_INFO	di ;
	int				same ;
	sf_count_t		suffix ;

	tc_stream_signal_double (d, TC_STREAM_FRAMES) ;
	TASK_CHECK (tc_write_all_double ("tc_rc_ref.wav", d, TC_STREAM_FRAMES) == 0) ;

	/* Repeated arming mid stream is a no-op. */
	f = tc_open_write ("tc_rc_rearm.wav", SF_FORMAT_WAV, SF_FORMAT_PCM_16, TC_STREAM_CH) ;
	TASK_CHECK (f != NULL && tc_arm (f, TC_SEED) == 0) ;
	TASK_CHECK (sf_writef_double (f, d, 500) == 500) ;
	memset (&di, 0, sizeof (di)) ;
	di.type = SFD_TPDF_SHAPED ;
	di.level = 1.0 ;
	TASK_CHECK (sf_command (f, SFC_SET_DITHER_ON_WRITE, &di, sizeof (di)) == 0) ;
	TASK_CHECK (sf_writef_double (f, d + 500 * TC_STREAM_CH, TC_STREAM_FRAMES - 500) == TC_STREAM_FRAMES - 500) ;
	TASK_CHECK (sf_close (f) == 0) ;
	TASK_CHECK (tc_codes_equal ("tc_rc_ref.wav", "tc_rc_rearm.wav", TC_STREAM_FRAMES, &same) == 0) ;
	TASK_CHECK (same) ;
	remove ("tc_rc_rearm.wav") ;

	/* Disable, write through the legacy path, re-enable: the suffix uses a
	** fresh error/PRNG state derived from the same configured seed. */
	suffix = 1200 ;
	f = tc_open_write ("tc_rc_cycle.wav", SF_FORMAT_WAV, SF_FORMAT_PCM_16, TC_STREAM_CH) ;
	TASK_CHECK (f != NULL && tc_arm (f, TC_SEED) == 0) ;
	TASK_CHECK (sf_writef_double (f, d, 600) == 600) ;
	memset (&di, 0, sizeof (di)) ;
	di.type = SFD_NO_DITHER ;
	di.level = 1.0 ;
	TASK_CHECK (sf_command (f, SFC_SET_DITHER_ON_WRITE, &di, sizeof (di)) == 0) ;
	TASK_CHECK (sf_writef_double (f, d + 600 * TC_STREAM_CH, 400) == 400) ;
	memset (&di, 0, sizeof (di)) ;
	di.type = SFD_TPDF_SHAPED ;
	di.level = 1.0 ;
	TASK_CHECK (sf_command (f, SFC_SET_DITHER_ON_WRITE, &di, sizeof (di)) == 0) ;
	TASK_CHECK (sf_writef_double (f, d + 1000 * TC_STREAM_CH, suffix) == suffix) ;
	TASK_CHECK (sf_close (f) == 0) ;

	/* Fresh file writing only the suffix input from the same seed. */
	f = tc_open_write ("tc_rc_fresh.wav", SF_FORMAT_WAV, SF_FORMAT_PCM_16, TC_STREAM_CH) ;
	TASK_CHECK (f != NULL && tc_arm (f, TC_SEED) == 0) ;
	TASK_CHECK (sf_writef_double (f, d + 1000 * TC_STREAM_CH, suffix) == suffix) ;
	TASK_CHECK (sf_close (f) == 0) ;

	{	int	full [TC_STREAM_N], fresh [TC_STREAM_N] ;
		int	i ;
		TASK_CHECK (tc_read_codes ("tc_rc_cycle.wav", SF_FORMAT_PCM_16, TC_STREAM_CH, 1000 + suffix, full) == 0) ;
		TASK_CHECK (tc_read_codes ("tc_rc_fresh.wav", SF_FORMAT_PCM_16, TC_STREAM_CH, suffix, fresh) == 0) ;
		for (i = 0 ; i < suffix * TC_STREAM_CH ; i++)
			TASK_CHECK (full [1000 * TC_STREAM_CH + i] == fresh [i]) ;
		} ;

	remove ("tc_rc_cycle.wav") ;
	remove ("tc_rc_fresh.wav") ;
	remove ("tc_rc_ref.wav") ;
} /* reconfigure */

/* While active, seeking is rejected and leaves the state untouched. */
TASK_CASE ("stream", seek_rejected)
{	static double	d [TC_STREAM_N] ;
	SNDFILE			*f ;
	int				same ;

	tc_stream_signal_double (d, TC_STREAM_FRAMES) ;
	TASK_CHECK (tc_write_all_double ("tc_seek_ref.wav", d, TC_STREAM_FRAMES) == 0) ;

	f = tc_open_write ("tc_seek_bad.wav", SF_FORMAT_WAV, SF_FORMAT_PCM_16, TC_STREAM_CH) ;
	TASK_CHECK (f != NULL && tc_arm (f, TC_SEED) == 0) ;
	TASK_CHECK (sf_writef_double (f, d, 777) == 777) ;
	TASK_CHECK (sf_seek (f, 0, SEEK_SET) == (sf_count_t) -1) ;
	TASK_CHECK (sf_seek (f, 10, SEEK_CUR | SFM_WRITE) == (sf_count_t) -1) ;
	TASK_CHECK (sf_writef_double (f, d + 777 * TC_STREAM_CH, TC_STREAM_FRAMES - 777) == TC_STREAM_FRAMES - 777) ;
	TASK_CHECK (sf_close (f) == 0) ;

	TASK_CHECK (tc_codes_equal ("tc_seek_ref.wav", "tc_seek_bad.wav", TC_STREAM_FRAMES, &same) == 0) ;
	TASK_CHECK (same) ;

	remove ("tc_seek_ref.wav") ;
	remove ("tc_seek_bad.wav") ;
} /* seek_rejected */

/*============================================================================
** compat group
*/

/* Dither disabled reproduces the legacy bytes exactly, including the old
** all-zero behaviour for sub-LSB input. */
TASK_CASE ("compat", disabled_matches_legacy_bytes)
{	enum { FRAMES = 50, CH = 2, N = FRAMES * CH } ;
	double			in [N] ;
	int				codes [N], i ;
	SNDFILE			*f ;
	SF_DITHER_INFO	di ;
	unsigned char	ba [8192], bb [8192] ;
	int				na, nb ;

	for (i = 0 ; i < N ; i++)
		in [i] = 0.25 / 32768.0 ;

	f = tc_open_write ("tc_co_plain.wav", SF_FORMAT_WAV, SF_FORMAT_PCM_16, CH) ;
	TASK_CHECK (f != NULL) ;
	TASK_CHECK (sf_writef_double (f, in, FRAMES) == FRAMES) ;
	TASK_CHECK (sf_close (f) == 0) ;

	f = tc_open_write ("tc_co_off.wav", SF_FORMAT_WAV, SF_FORMAT_PCM_16, CH) ;
	TASK_CHECK (f != NULL) ;
	memset (&di, 0, sizeof (di)) ;
	di.type = SFD_NO_DITHER ;
	di.level = 1.0 ;
	TASK_CHECK (sf_command (f, SFC_SET_DITHER_ON_WRITE, &di, sizeof (di)) == 0) ;
	TASK_CHECK (sf_writef_double (f, in, FRAMES) == FRAMES) ;
	TASK_CHECK (sf_close (f) == 0) ;

	na = tc_file_bytes ("tc_co_plain.wav", ba, sizeof (ba)) ;
	nb = tc_file_bytes ("tc_co_off.wav", bb, sizeof (bb)) ;
	TASK_CHECK (na > 0 && na == nb) ;
	TASK_CHECK (memcmp (ba, bb, (size_t) na) == 0) ;

	TASK_CHECK (tc_read_codes ("tc_co_off.wav", SF_FORMAT_PCM_16, CH, FRAMES, codes) == 0) ;
	for (i = 0 ; i < N ; i++)
		TASK_CHECK (codes [i] == 0) ;

	/* Legacy triangular type still follows the copy path. */
	f = tc_open_write ("tc_co_tri.wav", SF_FORMAT_WAV, SF_FORMAT_PCM_16, CH) ;
	TASK_CHECK (f != NULL) ;
	memset (&di, 0, sizeof (di)) ;
	di.type = SFD_TRIANGULAR_PDF ;
	di.level = 1.0 ;
	TASK_CHECK (sf_command (f, SFC_SET_DITHER_ON_WRITE, &di, sizeof (di)) == 0) ;
	TASK_CHECK (sf_writef_double (f, in, FRAMES) == FRAMES) ;
	TASK_CHECK (sf_close (f) == 0) ;
	TASK_CHECK (tc_read_codes ("tc_co_tri.wav", SF_FORMAT_PCM_16, CH, FRAMES, codes) == 0) ;
	for (i = 0 ; i < N ; i++)
		TASK_CHECK (codes [i] == 0) ;

	remove ("tc_co_plain.wav") ;
	remove ("tc_co_off.wav") ;
	remove ("tc_co_tri.wav") ;
} /* disabled_matches_legacy_bytes */

/* Every unsupported configuration must report an error and never silently
** fall back to the copy path. */
TASK_CASE ("compat", parameter_errors)
{	SNDFILE			*f ;
	SF_DITHER_INFO	di ;
	uint64_t		seed = 99 ;
	SF_INFO			info ;
	double			in [4] = { 0.1, 0.1, 0.1, 0.1 } ;

	f = tc_open_write ("tc_err_pcm32.wav", SF_FORMAT_WAV, SF_FORMAT_PCM_32, 2) ;
	TASK_CHECK (f != NULL && tc_arm (f, seed) != 0) ;
	sf_close (f) ;

	f = tc_open_write ("tc_err_float.wav", SF_FORMAT_WAV, SF_FORMAT_FLOAT, 2) ;
	TASK_CHECK (f != NULL && tc_arm (f, seed) != 0) ;
	sf_close (f) ;

	f = tc_open_write ("tc_err_u8.wav", SF_FORMAT_WAV, SF_FORMAT_PCM_U8, 2) ;
	TASK_CHECK (f != NULL && tc_arm (f, seed) != 0) ;
	sf_close (f) ;

	f = tc_open_write ("tc_err_level.wav", SF_FORMAT_WAV, SF_FORMAT_PCM_16, 2) ;
	TASK_CHECK (f != NULL) ;
	memset (&di, 0, sizeof (di)) ;
	di.type = SFD_TPDF_SHAPED ;
	di.level = 0.5 ;
	TASK_CHECK (sf_command (f, SFC_SET_DITHER_ON_WRITE, &di, sizeof (di)) != 0) ;
	sf_close (f) ;

	/* Normalization disabled. */
	f = tc_open_write ("tc_err_norm.wav", SF_FORMAT_WAV, SF_FORMAT_PCM_16, 2) ;
	TASK_CHECK (f != NULL) ;
	TASK_CHECK (sf_command (f, SFC_SET_NORM_FLOAT, NULL, SF_FALSE) == SF_TRUE) ;
	TASK_CHECK (tc_arm (f, seed) != 0) ;
	TASK_CHECK (sf_command (f, SFC_SET_NORM_DOUBLE, NULL, SF_FALSE) == SF_TRUE) ;
	TASK_CHECK (tc_arm (f, seed) != 0) ;
	sf_close (f) ;

	/* Seed command validation. */
	f = tc_open_write ("tc_err_seed.wav", SF_FORMAT_WAV, SF_FORMAT_PCM_16, 2) ;
	TASK_CHECK (f != NULL) ;
	TASK_CHECK (sf_command (f, SFC_SET_DITHER_SEED, NULL, sizeof (seed)) != 0) ;
	TASK_CHECK (sf_command (f, SFC_SET_DITHER_SEED, &seed, sizeof (seed) - 1) != 0) ;
	TASK_CHECK (sf_command (f, SFC_SET_DITHER_SEED, &seed, sizeof (seed)) == 0) ;
	TASK_CHECK (sf_writef_double (f, in, 2) == 2) ;
	TASK_CHECK (sf_command (f, SFC_SET_DITHER_SEED, &seed, sizeof (seed)) != 0) ;
	sf_close (f) ;

	/* Activating after the first sample is rejected. */
	f = tc_open_write ("tc_err_late.wav", SF_FORMAT_WAV, SF_FORMAT_PCM_16, 2) ;
	TASK_CHECK (f != NULL) ;
	TASK_CHECK (sf_writef_double (f, in, 1) == 1) ;
	TASK_CHECK (tc_arm (f, seed) != 0) ;
	sf_close (f) ;

	/* Read mode rejects both activation and seed setting. */
	memset (&info, 0, sizeof (info)) ;
	f = sf_open ("tc_err_pcm32.wav", SFM_READ, &info) ;
	TASK_CHECK (f != NULL) ;
	TASK_CHECK (sf_command (f, SFC_SET_DITHER_SEED, &seed, sizeof (seed)) != 0) ;
	memset (&di, 0, sizeof (di)) ;
	di.type = SFD_TPDF_SHAPED ;
	di.level = 1.0 ;
	TASK_CHECK (sf_command (f, SFC_SET_DITHER_ON_WRITE, &di, sizeof (di)) != 0) ;
	sf_close (f) ;

	/* Toggling normalization while active is rejected, state unchanged. */
	f = tc_open_write ("tc_err_toggle.wav", SF_FORMAT_WAV, SF_FORMAT_PCM_16, 2) ;
	TASK_CHECK (f != NULL && tc_arm (f, seed) == 0) ;
	TASK_CHECK (sf_command (f, SFC_SET_NORM_FLOAT, NULL, SF_FALSE) != 0) ;
	TASK_CHECK (sf_command (f, SFC_SET_NORM_DOUBLE, NULL, SF_FALSE) != 0) ;
	TASK_CHECK (sf_command (f, SFC_GET_NORM_FLOAT, NULL, 0) == SF_TRUE) ;
	TASK_CHECK (sf_command (f, SFC_GET_NORM_DOUBLE, NULL, 0) == SF_TRUE) ;
	sf_close (f) ;

	/* Bad SF_DITHER_INFO payload still uses the generic parameter check. */
	f = tc_open_write ("tc_err_param.wav", SF_FORMAT_WAV, SF_FORMAT_PCM_16, 2) ;
	TASK_CHECK (f != NULL) ;
	TASK_CHECK (sf_command (f, SFC_SET_DITHER_ON_WRITE, NULL, 0) != 0) ;
	TASK_CHECK (sf_command (f, SFC_SET_DITHER_SEED, NULL, 0) != 0) ;
	sf_close (f) ;

	remove ("tc_err_pcm32.wav") ;
	remove ("tc_err_float.wav") ;
	remove ("tc_err_u8.wav") ;
	remove ("tc_err_level.wav") ;
	remove ("tc_err_norm.wav") ;
	remove ("tc_err_seed.wav") ;
	remove ("tc_err_late.wav") ;
	remove ("tc_err_toggle.wav") ;
	remove ("tc_err_param.wav") ;
} /* parameter_errors */

/* Both containers and both bit widths dither correctly. */
TASK_CASE ("compat", containers_and_widths)
{	struct { int container ; const char *ext ; } containers [2] = {
		{ SF_FORMAT_WAV, "wav" }, { SF_FORMAT_AIFF, "aiff" } } ;
	int				subs [2] = { SF_FORMAT_PCM_16, SF_FORMAT_PCM_24 } ;
	enum { FRAMES = 48, CH = 2, N = FRAMES * CH } ;
	double			in [N], scaled [N] ;
	int				expect [N], got [N] ;
	int				ci, si, i, nonzero ;
	char			path [64] ;
	SNDFILE			*f ;

	for (i = 0 ; i < N ; i++)
		in [i] = ((i % 7) - 3) / 262144.0 ;

	for (ci = 0 ; ci < 2 ; ci++)
		for (si = 0 ; si < 2 ; si++)
		{	int sub = subs [si] ;
			int bits = (sub == SF_FORMAT_PCM_24) ? 24 : 16 ;
			snprintf (path, sizeof (path), "tc_cw_%d_%d.%s", ci, si, containers [ci].ext) ;

			/* The input amplitude is expressed in the same fraction of one
			** target LSB for both bit widths. */
			for (i = 0 ; i < N ; i++)
				scaled [i] = (bits == 24) ? in [i] / 256.0 : in [i] ;

			TASK_CHECK (tc_observer_reference ("tc_cw_ref.wav", sub, CH, FRAMES, scaled, TC_SEED, expect, NULL, NULL) == 0) ;

			f = tc_open_write (path, containers [ci].container, sub, CH) ;
			TASK_CHECK (f != NULL) ;
			TASK_CHECK (tc_arm (f, TC_SEED) == 0) ;
			TASK_CHECK (sf_writef_double (f, scaled, FRAMES) == FRAMES) ;
			TASK_CHECK (sf_close (f) == 0) ;
			TASK_CHECK (tc_read_codes (path, sub, CH, FRAMES, got) == 0) ;

			nonzero = 0 ;
			for (i = 0 ; i < N ; i++)
			{	TASK_CHECK (got [i] == expect [i]) ;
				if (got [i] != 0)
					nonzero++ ;
				} ;
			TASK_CHECK (nonzero > 0) ;
			remove (path) ;
			remove ("tc_cw_ref.wav") ;
			} ;
} /* containers_and_widths */

/* Integer writes keep the legacy path, consume no random numbers and reset
** the per channel error; the following float frames continue the same PRNG
** streams with a fresh e = 0. */
TASK_CASE ("compat", integer_insertion)
{	enum { FRAMES = 6, CH = 2, N = FRAMES * CH } ;
	double			in [N] ;
	int				ints [N], codes [N] ;
	SNDFILE			*f ;
	sf_count_t		drawn_before, drawn_after ;
	uint64_t		bs, rng ;
	int				bits ;
	double			u, v ;
	int				i ;

	for (i = 0 ; i < N ; i++)
		in [i] = 1.0 / 131072.0 ;
	for (i = 0 ; i < N ; i++)
		ints [i] = (i - N / 2) * 0x00040000 ;	/* left aligned, even in 16 bit */

	f = tc_open_write ("tc_int.wav", SF_FORMAT_WAV, SF_FORMAT_PCM_16, CH) ;
	TASK_CHECK (f != NULL && tc_arm (f, TC_SEED) == 0) ;
	TASK_CHECK (sf_writef_double (f, in, 2) == 2) ;
	TASK_CHECK (sf_tpdf_test_observer (f, 0, &bs, &bits, &drawn_before, &u, &v, &rng) == 0) ;

	/* One full integer frame through the old rules. */
	TASK_CHECK (sf_writef_int (f, ints + 4, 1) == 1) ;
	TASK_CHECK (sf_tpdf_test_observer (f, 0, &bs, &bits, &drawn_after, &u, &v, &rng) == 0) ;
	TASK_CHECK (drawn_after == drawn_before) ;	/* no PRNG consumption */

	/* Remaining float frames; errors were cleared by the integer frame but
	** the PRNG streams simply continue. */
	TASK_CHECK (sf_writef_double (f, in + 6, FRAMES - 3) == FRAMES - 3) ;
	TASK_CHECK (sf_close (f) == 0) ;

	TASK_CHECK (tc_read_codes ("tc_int.wav", SF_FORMAT_PCM_16, CH, FRAMES, codes) == 0) ;
	for (i = 0 ; i < CH ; i++)
		TASK_CHECK (codes [4 + i] == (ints [4 + i] / 65536)) ;

	{	TC_REF r0, r1 ;
		tc_ref_init (&r0, TC_SEED, 0) ;
		tc_ref_init (&r1, TC_SEED, 1) ;
		/* Float frames 0 and 1 consume the first two PRNG pairs. */
		for (i = 0 ; i < 2 ; i++)
		{	double u0 = tc_uniform (tc_splitmix64 (&r0.state)) ;
			double v0 = tc_uniform (tc_splitmix64 (&r0.state)) ;
			double u1 = tc_uniform (tc_splitmix64 (&r1.state)) ;
			double v1 = tc_uniform (tc_splitmix64 (&r1.state)) ;
			tc_ref_step (&r0, in [2 * i], 16, u0, v0) ;
			tc_ref_step (&r1, in [2 * i + 1], 16, u1, v1) ;
			} ;
		/* Integer frame: PRNG untouched, error reset to zero. */
		r0.error = 0.0 ;
		r1.error = 0.0 ;
		/* Float frames 3..5 continue the same streams with e = 0. */
		for (i = 3 ; i < FRAMES ; i++)
		{	double u0 = tc_uniform (tc_splitmix64 (&r0.state)) ;
			double v0 = tc_uniform (tc_splitmix64 (&r0.state)) ;
			double u1 = tc_uniform (tc_splitmix64 (&r1.state)) ;
			double v1 = tc_uniform (tc_splitmix64 (&r1.state)) ;
			TASK_CHECK (tc_ref_step (&r0, in [2 * i], 16, u0, v0) == codes [2 * i]) ;
			TASK_CHECK (tc_ref_step (&r1, in [2 * i + 1], 16, u1, v1) == codes [2 * i + 1]) ;
			} ;
		} ;

	remove ("tc_int.wav") ;
} /* integer_insertion */

/* Non-finite sentinels through both float and double entry points. */
TASK_CASE ("compat", non_finite_sentinels)
{	enum { FRAMES = 5, CH = 1 } ;
	double			d [FRAMES] ;
	float			fl [FRAMES] ;
	int				codes [FRAMES] ;
	SNDFILE			*f ;
	int				i ;

	d [0] = NAN ;
	d [1] = INFINITY ;
	d [2] = -INFINITY ;
	d [3] = 1.0 / 131072.0 ;
	d [4] = 1.0 / 131072.0 ;

	f = tc_open_write ("tc_nan_d.wav", SF_FORMAT_WAV, SF_FORMAT_PCM_16, CH) ;
	TASK_CHECK (f != NULL && tc_arm (f, TC_SEED) == 0) ;
	TASK_CHECK (sf_writef_double (f, d, FRAMES) == FRAMES) ;
	TASK_CHECK (sf_close (f) == 0) ;
	TASK_CHECK (tc_read_codes ("tc_nan_d.wav", SF_FORMAT_PCM_16, CH, FRAMES, codes) == 0) ;
	TASK_CHECK (codes [0] == 0 && codes [1] == 32767 && codes [2] == -32768) ;
	for (i = 0 ; i < FRAMES ; i++)
		TASK_CHECK (codes [i] >= -32768 && codes [i] <= 32767) ;
	remove ("tc_nan_d.wav") ;

	for (i = 0 ; i < FRAMES ; i++)
		fl [i] = (float) d [i] ;
	f = tc_open_write ("tc_nan_f.wav", SF_FORMAT_WAV, SF_FORMAT_PCM_24, CH) ;
	TASK_CHECK (f != NULL && tc_arm (f, TC_SEED) == 0) ;
	TASK_CHECK (sf_writef_float (f, fl, FRAMES) == FRAMES) ;
	TASK_CHECK (sf_close (f) == 0) ;
	TASK_CHECK (tc_read_codes ("tc_nan_f.wav", SF_FORMAT_PCM_24, CH, FRAMES, codes) == 0) ;
	TASK_CHECK (codes [0] == 0 && codes [1] == 8388607 && codes [2] == -8388608) ;
	for (i = 0 ; i < FRAMES ; i++)
		TASK_CHECK (codes [i] >= -8388608 && codes [i] <= 8388607) ;
	remove ("tc_nan_f.wav") ;
} /* non_finite_sentinels */

/* Observer rejects bad handles and never leaks production e/q. */
TASK_CASE ("compat", observer_guards)
{	SNDFILE		*f ;
	double		in [4] = { 0.0001, 0.0001, 0.0001, 0.0001 } ;
	uint64_t	bs, rng ;
	int			bits ;
	sf_count_t	drawn ;
	double		u, v ;

	f = tc_open_write ("tc_obs.wav", SF_FORMAT_WAV, SF_FORMAT_PCM_16, 2) ;
	TASK_CHECK (f != NULL) ;
	/* Inactive session is not observable. */
	TASK_CHECK (sf_tpdf_test_observer (f, 0, &bs, &bits, &drawn, &u, &v, &rng) != 0) ;
	TASK_CHECK (tc_arm (f, TC_SEED) == 0) ;
	TASK_CHECK (sf_writef_double (f, in, 2) == 2) ;
	TASK_CHECK (sf_tpdf_test_observer (f, 0, &bs, &bits, &drawn, &u, &v, &rng) == 0) ;
	TASK_CHECK (bs == TC_SEED && bits == 16 && drawn == 2) ;
	TASK_CHECK (u >= 0.0 && u < 1.0 && v >= 0.0 && v < 1.0) ;
	TASK_CHECK (sf_tpdf_test_observer (f, 2, &bs, &bits, &drawn, &u, &v, &rng) != 0) ;
	TASK_CHECK (sf_tpdf_test_observer (f, -1, &bs, &bits, &drawn, &u, &v, &rng) != 0) ;
	TASK_CHECK (sf_tpdf_test_observer (NULL, 0, &bs, &bits, &drawn, &u, &v, &rng) != 0) ;
	sf_close (f) ;
	remove ("tc_obs.wav") ;
} /* observer_guards */
