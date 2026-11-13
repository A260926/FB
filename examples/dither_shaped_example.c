/*
** Dither example for SFD_TPDF_SHAPED.
**
** Generates a low level, sub-LSB signal entirely in code and exports it to
** 16 bit PCM twice: once through the plain conversion path and once with
** triangular PDF dither plus first order error feedback. The program prints
** the first few code values from each file so the difference is visible:
** the plain path truncates the quiet signal to silence, the dithered path
** preserves its average energy as sparse +/-1 LSB activity.
**
** Build against a static libsndfile with:
**   cc -std=c99 dither_shaped_example.c libsndfile.a -lm -o dither_shaped_example
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <sndfile.h>

#define	EXAMPLE_FRAMES		32
#define	EXAMPLE_CHANNELS	1

static int
write_pcm (const char *path, int use_dither, const double *input, sf_count_t frames)
{	SF_INFO			info ;
	SNDFILE			*file ;
	SF_DITHER_INFO	dither ;
	uint64_t		seed = (uint64_t) 0x2545F4914F6CDD1DULL ;

	memset (&info, 0, sizeof (info)) ;
	info.samplerate = 48000 ;
	info.channels = EXAMPLE_CHANNELS ;
	info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16 ;

	if ((file = sf_open (path, SFM_WRITE, &info)) == NULL)
	{	fprintf (stderr, "sf_open failed: %s\n", sf_strerror (NULL)) ;
		return 1 ;
		} ;

	if (use_dither)
	{	memset (&dither, 0, sizeof (dither)) ;
		dither.type = SFD_TPDF_SHAPED ;
		dither.level = 1.0 ;	/* the only accepted level */

		/* Optional: a fixed seed makes the export reproducible. */
		if (sf_command (file, SFC_SET_DITHER_SEED, &seed, sizeof (seed)) != 0)
		{	fprintf (stderr, "SFC_SET_DITHER_SEED failed: %s\n", sf_strerror (file)) ;
			sf_close (file) ;
			return 1 ;
			} ;

		if (sf_command (file, SFC_SET_DITHER_ON_WRITE, &dither, sizeof (dither)) != 0)
		{	fprintf (stderr, "SFC_SET_DITHER_ON_WRITE failed: %s\n", sf_strerror (file)) ;
			sf_close (file) ;
			return 1 ;
			} ;
		} ;

	if (sf_writef_double (file, input, frames) != frames)
	{	fprintf (stderr, "sf_writef_double failed: %s\n", sf_strerror (file)) ;
		sf_close (file) ;
		return 1 ;
		} ;

	return sf_close (file) ;
} /* write_pcm */

static int
read_codes (const char *path, int *codes, sf_count_t frames)
{	SF_INFO		info ;
	SNDFILE		*file ;
	int			aligned [EXAMPLE_FRAMES] ;
	sf_count_t	k ;

	memset (&info, 0, sizeof (info)) ;
	if ((file = sf_open (path, SFM_READ, &info)) == NULL)
		return 1 ;

	if (sf_readf_int (file, aligned, frames) != frames)
	{	sf_close (file) ;
		return 1 ;
		} ;

	/* sf_readf_int returns left aligned 32 bit values; recover the 16 bit code. */
	for (k = 0 ; k < frames ; k++)
		codes [k] = aligned [k] >> 16 ;

	sf_close (file) ;
	return 0 ;
} /* read_codes */

int
main (void)
{	double	input [EXAMPLE_FRAMES] ;
	int		plain [EXAMPLE_FRAMES], shaped [EXAMPLE_FRAMES] ;
	int		k, plain_nonzero = 0, shaped_pos = 0, shaped_neg = 0 ;

	/* A constant signal sitting one quarter of an LSB below zero. */
	for (k = 0 ; k < EXAMPLE_FRAMES ; k++)
		input [k] = 0.25 / 32768.0 ;

	if (write_pcm ("example_plain.wav", 0, input, EXAMPLE_FRAMES) != 0)
		return 1 ;
	if (write_pcm ("example_shaped.wav", 1, input, EXAMPLE_FRAMES) != 0)
		return 1 ;

	if (read_codes ("example_plain.wav", plain, EXAMPLE_FRAMES) != 0)
		return 1 ;
	if (read_codes ("example_shaped.wav", shaped, EXAMPLE_FRAMES) != 0)
		return 1 ;

	printf ("frame  plain  shaped\n") ;
	for (k = 0 ; k < EXAMPLE_FRAMES ; k++)
	{	printf ("%5d  %5d  %6d\n", k, plain [k], shaped [k]) ;
		if (plain [k] != 0)
			plain_nonzero++ ;
		if (shaped [k] > 0)
			shaped_pos++ ;
		if (shaped [k] < 0)
			shaped_neg++ ;
		} ;

	printf ("plain non-zero samples : %d\n", plain_nonzero) ;
	printf ("shaped positive codes  : %d\n", shaped_pos) ;
	printf ("shaped negative codes  : %d\n", shaped_neg) ;
	printf ("shaped mean code       : %.4f (input was +0.25 LSB)\n",
			(shaped_pos - shaped_neg) / (double) EXAMPLE_FRAMES) ;

	remove ("example_plain.wav") ;
	remove ("example_shaped.wav") ;

	return 0 ;
} /* main */
