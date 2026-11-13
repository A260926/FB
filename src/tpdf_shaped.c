/*
** Copyright (C) 2026 Erik de Castro Lopo <erikd@mega-nerd.com>
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU Lesser General Public License as published by
** the Free Software Foundation; either version 2.1 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU Lesser General Public License for more details.
**
** You should have received a copy of the GNU Lesser General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

/*
** SFD_TPDF_SHAPED : triangular probability density function dither
** (two independent uniform variates per sample) combined with first order
** error feedback, for SFM_WRITE PCM_16/PCM_24 files fed from normalized
** float/double input.
**
** See DITHER_SHAPED.md for the full specification.
*/

#include	"sfconfig.h"

#include	<stdlib.h>
#include	<string.h>
#include	<math.h>
#include	<stdint.h>

#include	"sndfile.h"
#include	"sfendian.h"
#include	"common.h"
#include	"tpdf_shaped.h"

/* Documented fixed seed used when the application never sets one. */
#define	TPDF_DEFAULT_SEED		((uint64_t) 0x9E3779B97F4A7C15ULL)

/* SplitMix64 increment (the golden ratio derived constant). */
#define	TPDF_GAMMA				((uint64_t) 0x9E3779B97F4A7C15ULL)

/* 2^-53 : turns the top 53 bits of a PRNG word into a [0, 1) double. */
#define	TPDF_ONE_OVER_2P53		(1.0 / 9007199254740992.0)

/* Fixed, bounded work buffer (in interleaved int samples). */
#define	TPDF_WORK_SAMPLES		2048

/* Magic stored in the first field so a TPDF_DATA can never be confused with
** the legacy DITHER_DATA in dither.c (both live in psf->dither). */
#define	TPDF_MAGICK				((uint32_t) 0x54504453)	/* 'TPDS' */

typedef struct
{	/* SplitMix64 state. Every value, including zero, is a legal state. */
	uint64_t		state ;

	/* First order error feedback term e, in units of one target LSB. */
	double			error ;

	/* Number of dithered samples drawn on this channel (test observer). */
	sf_count_t		drawn ;

	/* Most recent uniform variates actually drawn (test observer). */
	double			last_u ;
	double			last_v ;
} TPDF_CHANNEL ;

typedef struct
{
	uint32_t		magick ;

	int				active ;
	int				bits ;				/* 16 or 24 */
	int				channels ;
	int				scale_mult ;		/* 65536 for PCM_16, 256 for PCM_24;
										** multiplication keeps negative codes
										** well defined (a left shift would be UB) */
	double			scale ;				/* 2^(bits - 1) */
	int				qmin, qmax ;		/* signed B bit range */

	uint64_t		base_seed ;			/* configured seed */

	/* Fixed size interleave work buffer; the whole recording is never
	** retained and the caller's input array is never modified. */
	int				ibuf [TPDF_WORK_SAMPLES] ;

	/* Saved codec level write callbacks. */
	sf_count_t		(*write_short)	(SF_PRIVATE *psf, const short *ptr, sf_count_t len) ;
	sf_count_t		(*write_int)	(SF_PRIVATE *psf, const int *ptr, sf_count_t len) ;
	sf_count_t		(*write_float)	(SF_PRIVATE *psf, const float *ptr, sf_count_t len) ;
	sf_count_t		(*write_double)	(SF_PRIVATE *psf, const double *ptr, sf_count_t len) ;

	/* Per channel state lives in the same allocation (flexible array member),
	** so psf_close() can release everything with a single free(). */
	TPDF_CHANNEL	chan [] ;
} TPDF_DATA ;

int
tpdf_owns (SF_PRIVATE *psf)
{	TPDF_DATA *pd ;

	if (psf == NULL || psf->dither == NULL)
		return SF_FALSE ;

	pd = psf->dither ;
	return pd->magick == TPDF_MAGICK ? SF_TRUE : SF_FALSE ;
} /* tpdf_owns */

/*==============================================================================
** SplitMix64 (Sebastiano Vigna). A pure integer PRNG with a single 64 bit
** state word: each call adds the gamma constant and runs three xor-shift /
** multiply mixing rounds. The state fully determines the next output, so the
** generator needs no global state and no wall clock entropy.
*/

static inline uint64_t
tpdf_splitmix64 (uint64_t *state)
{	uint64_t z ;

	*state += TPDF_GAMMA ;
	z = *state ;
	z = (z ^ (z >> 30)) * ((uint64_t) 0xBF58476D1CE4E5B9ULL) ;
	z = (z ^ (z >> 27)) * ((uint64_t) 0x94D049BB133111EBULL) ;
	return z ^ (z >> 31) ;
} /* tpdf_splitmix64 */

/*
** Derive the independent starting state of one channel from the configured
** 64 bit seed. Every seed value, including zero, maps to a legal SplitMix64
** state because the first draw always adds the gamma constant before mixing.
*/
static inline uint64_t
tpdf_channel_state (uint64_t base_seed, int channel)
{	uint64_t mixed = base_seed ^ (TPDF_GAMMA * ((uint64_t) channel + 1)) ;

	return tpdf_splitmix64 (&mixed) ;
} /* tpdf_channel_state */

static inline double
tpdf_uniform (uint64_t word)
{
	return (double) (word >> 11) * TPDF_ONE_OVER_2P53 ;
} /* tpdf_uniform */

/*==============================================================================
*/

static sf_count_t	tpdf_write_short	(SF_PRIVATE *psf, const short *ptr, sf_count_t len) ;
static sf_count_t	tpdf_write_int		(SF_PRIVATE *psf, const int *ptr, sf_count_t len) ;
static sf_count_t	tpdf_write_float	(SF_PRIVATE *psf, const float *ptr, sf_count_t len) ;
static sf_count_t	tpdf_write_double	(SF_PRIVATE *psf, const double *ptr, sf_count_t len) ;

static void
tpdf_seed_channels (TPDF_DATA *pd)
{	for (int ch = 0 ; ch < pd->channels ; ch++)
	{	pd->chan [ch].state = tpdf_channel_state (pd->base_seed, ch) ;
		pd->chan [ch].error = 0.0 ;
		pd->chan [ch].drawn = 0 ;
		pd->chan [ch].last_u = 0.0 ;
		pd->chan [ch].last_v = 0.0 ;
		} ;
} /* tpdf_seed_channels */

/*
** Core per-sample quantization. Inputs are already scaled to LSB units.
** Returns the signed B bit integer q and updates the channel error term.
** The dither noise (u - v) never enters the feedback error.
*/
static inline int
tpdf_quantize (TPDF_DATA *pd, TPDF_CHANNEL *pc, double x)
{	uint64_t	ru, rv ;
	double		u, v, z, r ;
	int			q ;

	/* Exactly two PRNG words are consumed for every sample. */
	ru = tpdf_splitmix64 (&pc->state) ;
	rv = tpdf_splitmix64 (&pc->state) ;
	u = tpdf_uniform (ru) ;
	v = tpdf_uniform (rv) ;
	pc->last_u = u ;
	pc->last_v = v ;
	pc->drawn ++ ;

	if (! isfinite (x))
	{	/* Non-finite input: saturate the infinities, map NaN to zero, and
		** clear the feedback error exactly as a saturated sample would. */
		pc->error = 0.0 ;
		if (x > 0.0)
			return pd->qmax ;
		if (x < 0.0)
			return pd->qmin ;
		return 0 ;
		} ;

	z = x + pc->error ;

	/* round() implements round half away from zero, as required. */
	r = round (z + u - v) ;

	if (r >= (double) pd->qmax + 1.0)
	{	q = pd->qmax ;
		pc->error = 0.0 ;			/* saturation clears the channel error */
		return q ;
		} ;

	if (r <= (double) pd->qmin - 1.0)
	{	q = pd->qmin ;
		pc->error = 0.0 ;
		return q ;
		} ;

	q = (int) r ;
	pc->error = z - q ;			/* dither noise is not part of e */

	return q ;
} /* tpdf_quantize */

static sf_count_t
tpdf_flush_int (TPDF_DATA *pd, SF_PRIVATE *psf, int count)
{	sf_count_t	done = 0 ;
	sf_count_t	written ;

	while (done < count)
	{	written = pd->write_int (psf, pd->ibuf + done, count - done) ;
		if (written <= 0)
			break ;
		done += written ;
		} ;

	return done ;
} /* tpdf_flush_int */

static sf_count_t
tpdf_convert_double (SF_PRIVATE *psf, const double *ptr, sf_count_t len)
{	TPDF_DATA	*pd = psf->dither ;
	sf_count_t	total = 0 ;
	int			chunk, frames, count ;

	while (total < len)
	{	chunk = TPDF_WORK_SAMPLES ;
		if (chunk > len - total)
			chunk = (int) (len - total) ;
		chunk = (chunk / pd->channels) * pd->channels ;
		if (chunk == 0)
			break ;

		for (frames = 0 ; frames < chunk / pd->channels ; frames++)
		{	for (int ch = 0 ; ch < pd->channels ; ch++)
			{	double x = ptr [total + frames * pd->channels + ch] * pd->scale ;
				int q = tpdf_quantize (pd, &pd->chan [ch], x) ;

				/* Left aligned int representation; the codec callback turns
				** this into the on-disk 16 bit or three byte 24 bit sample. */
				pd->ibuf [frames * pd->channels + ch] = q * pd->scale_mult ;
				} ;
			} ;

		count = (int) tpdf_flush_int (pd, psf, chunk) ;
		total += count ;
		if (count < chunk)
			break ;
		} ;

	return total ;
} /* tpdf_convert_double */

static sf_count_t
tpdf_write_double (SF_PRIVATE *psf, const double *ptr, sf_count_t len)
{
	return tpdf_convert_double (psf, ptr, len) ;
} /* tpdf_write_double */

static sf_count_t
tpdf_write_float (SF_PRIVATE *psf, const float *ptr, sf_count_t len)
{	TPDF_DATA	*pd = psf->dither ;
	sf_count_t	total = 0 ;
	int			chunk, frames, count ;

	while (total < len)
	{	chunk = TPDF_WORK_SAMPLES ;
		if (chunk > len - total)
			chunk = (int) (len - total) ;
		chunk = (chunk / pd->channels) * pd->channels ;
		if (chunk == 0)
			break ;

		for (frames = 0 ; frames < chunk / pd->channels ; frames++)
		{	for (int ch = 0 ; ch < pd->channels ; ch++)
			{	/* Widen to double first so float and double entry points run
				** the identical quantization arithmetic. */
				double x = ((double) ptr [total + frames * pd->channels + ch]) * pd->scale ;
				int q = tpdf_quantize (pd, &pd->chan [ch], x) ;

				pd->ibuf [frames * pd->channels + ch] = q * pd->scale_mult ;
				} ;
			} ;

		count = (int) tpdf_flush_int (pd, psf, chunk) ;
		total += count ;
		if (count < chunk)
			break ;
		} ;

	return total ;
} /* tpdf_write_float */

/*
** Integer entry points keep the legacy codec behaviour (the samples are
** already quantized) and only clear the per-channel error terms. They never
** consume PRNG words.
*/
static sf_count_t
tpdf_write_int (SF_PRIVATE *psf, const int *ptr, sf_count_t len)
{	TPDF_DATA	*pd = psf->dither ;
	sf_count_t	written ;

	written = pd->write_int (psf, ptr, len) ;

	if (written == len)
		for (int ch = 0 ; ch < pd->channels ; ch++)
			pd->chan [ch].error = 0.0 ;

	return written ;
} /* tpdf_write_int */

static sf_count_t
tpdf_write_short (SF_PRIVATE *psf, const short *ptr, sf_count_t len)
{	TPDF_DATA	*pd = psf->dither ;
	sf_count_t	written ;

	written = pd->write_short (psf, ptr, len) ;

	if (written == len)
		for (int ch = 0 ; ch < pd->channels ; ch++)
			pd->chan [ch].error = 0.0 ;

	return written ;
} /* tpdf_write_short */

/*==============================================================================
** Session management.
*/

static void
tpdf_install_callbacks (SF_PRIVATE *psf, TPDF_DATA *pd)
{	pd->write_short = psf->write_short ;
	pd->write_int = psf->write_int ;
	pd->write_float = psf->write_float ;
	pd->write_double = psf->write_double ;

	psf->write_short = tpdf_write_short ;
	psf->write_int = tpdf_write_int ;
	psf->write_float = tpdf_write_float ;
	psf->write_double = tpdf_write_double ;
	pd->active = SF_TRUE ;
} /* tpdf_install_callbacks */

void
tpdf_deactivate (SF_PRIVATE *psf)
{	TPDF_DATA *pd = psf->dither ;

	if (pd == NULL || pd->magick != TPDF_MAGICK || ! pd->active)
		return ;

	psf->write_short = pd->write_short ;
	psf->write_int = pd->write_int ;
	psf->write_float = pd->write_float ;
	psf->write_double = pd->write_double ;
	pd->active = SF_FALSE ;
} /* tpdf_deactivate */

int
tpdf_is_active (SF_PRIVATE *psf)
{	TPDF_DATA *pd = psf->dither ;

	return pd != NULL && pd->magick == TPDF_MAGICK && pd->active ;
} /* tpdf_is_active */

int
tpdf_activate (SF_PRIVATE *psf, const SF_DITHER_INFO *info)
{	TPDF_DATA	*pd ;
	int			codec ;

	if (psf->file.mode != SFM_WRITE)
		return SFE_TPDF_NOT_WRITABLE ;

	codec = SF_CODEC (psf->sf.format) ;
	if (codec != SF_FORMAT_PCM_16 && codec != SF_FORMAT_PCM_24)
		return SFE_TPDF_BAD_FORMAT ;

	if (info->level != 1.0)
		return SFE_TPDF_BAD_LEVEL ;

	if (psf->norm_float != SF_TRUE || psf->norm_double != SF_TRUE)
		return SFE_TPDF_NO_NORMALIZE ;

	pd = psf->dither ;

	if (pd != NULL && pd->magick != TPDF_MAGICK)
	{	/* psf->dither is owned by the legacy copy-only dither; the two
		** sessions cannot share the callback slots. */
		return SFE_TPDF_BAD_FORMAT ;
		} ;

	if (pd != NULL && pd->active)
	{	/* Re-arming the active mode is a no-op: the PRNG stream, error
		** terms and callbacks must not be reset or re-wrapped. */
		return 0 ;
		} ;

	if (pd == NULL && psf->have_written)
		return SFE_TPDF_AFTER_WRITE ;

	if (pd == NULL)
	{	pd = calloc (1, sizeof (TPDF_DATA) + (size_t) psf->sf.channels * sizeof (TPDF_CHANNEL)) ;
		if (pd == NULL)
			return SFE_MALLOC_FAILED ;

		pd->magick = TPDF_MAGICK ;
		pd->channels = psf->sf.channels ;

		pd->base_seed = psf->tpdf_seed_set ? psf->tpdf_seed : TPDF_DEFAULT_SEED ;

		switch (codec)
		{	case SF_FORMAT_PCM_16 :
				pd->bits = 16 ;
				pd->scale_mult = 1 << 16 ;
				pd->scale = 32768.0 ;
				pd->qmin = -32768 ;
				pd->qmax = 32767 ;
				break ;

			case SF_FORMAT_PCM_24 :
				pd->bits = 24 ;
				pd->scale_mult = 1 << 8 ;
				pd->scale = 8388608.0 ;
				pd->qmin = -8388608 ;
				pd->qmax = 8388607 ;
				break ;

			default :
				break ;
			} ;

		psf->dither = pd ;
		tpdf_seed_channels (pd) ;
		}
	else
	{	/* Re-enabling after SFD_NO_DITHER: restart from zero error and the
		** originally configured seed. */
		pd->base_seed = psf->tpdf_seed_set ? psf->tpdf_seed : TPDF_DEFAULT_SEED ;
		tpdf_seed_channels (pd) ;
		} ;

	tpdf_install_callbacks (psf, pd) ;

	return 0 ;
} /* tpdf_activate */

int
tpdf_set_seed (SF_PRIVATE *psf, void *data, int datasize)
{	TPDF_DATA	*pd ;
	uint64_t	seed ;

	if (psf->file.mode != SFM_WRITE)
		return SFE_TPDF_NOT_WRITABLE ;

	if (data == NULL || datasize != SIGNED_SIZEOF (uint64_t))
		return SFE_TPDF_BAD_SEED ;

	if (psf->have_written)
		return SFE_TPDF_AFTER_WRITE ;

	memcpy (&seed, data, sizeof (seed)) ;

	psf->tpdf_seed = seed ;
	psf->tpdf_seed_set = SF_TRUE ;

	pd = psf->dither ;
	if (pd != NULL && pd->magick == TPDF_MAGICK)
	{	/* No audio has been written yet (have_written is false above), so
		** re-seeding the channel states is safe whether active or not. */
		pd->base_seed = seed ;
		tpdf_seed_channels (pd) ;
		} ;

	return 0 ;
} /* tpdf_set_seed */

/*==============================================================================
** Test only observer. Not present in the public exported symbol lists; the
** static archive test driver links it directly. It exposes PRNG state and the
** uniform variates drawn, never the production error term or the quantized
** integer q.
*/

int
sf_tpdf_test_observer (SNDFILE *sndfile, int channel, uint64_t *base_seed, int *bits,
						sf_count_t *samples, double *last_u, double *last_v, uint64_t *rng_state)
{	SF_PRIVATE	*psf ;
	TPDF_DATA	*pd ;

	if ((psf = (SF_PRIVATE*) sndfile) == NULL || psf->Magick != SNDFILE_MAGICK)
		return 1 ;

	pd = psf->dither ;
	if (pd == NULL || ! pd->active)
		return 1 ;

	if (channel < 0 || channel >= pd->channels)
		return 1 ;

	if (base_seed)
		*base_seed = pd->base_seed ;
	if (bits)
		*bits = pd->bits ;
	if (samples)
		*samples = pd->chan [channel].drawn ;
	if (last_u)
		*last_u = pd->chan [channel].last_u ;
	if (last_v)
		*last_v = pd->chan [channel].last_v ;
	if (rng_state)
		*rng_state = pd->chan [channel].state ;

	return 0 ;
} /* sf_tpdf_test_observer */
