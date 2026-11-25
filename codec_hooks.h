/*
 * libmad - MPEG audio decoder library
 *
 * Internal observation hooks used by the bounded granule session.  The task
 * build force-includes task_support/work.h, which defines these macros; in a
 * normal library build they compile away.
 */

# ifndef LIBMAD_CODEC_HOOKS_H
# define LIBMAD_CODEC_HOOKS_H

# ifndef MAD_GRANULE_SPECTRAL_BEGIN
#  define MAD_GRANULE_SPECTRAL_BEGIN()  ((void) 0)
# endif

# ifndef MAD_GRANULE_SYNTH_BEGIN
#  define MAD_GRANULE_SYNTH_BEGIN()     ((void) 0)
# endif

# endif
