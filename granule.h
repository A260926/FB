/*
 * libmad - MPEG audio decoder library
 *
 * Bounded granule pull API.
 *
 * This interface decodes one granule at a time from a single-threaded pull
 * loop.  The caller feeds compressed bytes, then calls mad_granule_next()
 * once per delivered granule.  All decoder state (the compressed queue, the
 * in-flight physical frame, the Layer III reservoir, the overlap history, the
 * synthesis filterbank, and the 18-subband-step phase) is owned by the opaque
 * object, so the caller may move or overwrite its own input between calls.
 */

# ifndef LIBMAD_GRANULE_H
# define LIBMAD_GRANULE_H

# include <stddef.h>
# include <stdint.h>

# include "stream.h"
# include "synth.h"

struct mad_granule_decoder;

enum mad_granule_status {
  MAD_GRANULE_ERROR     = -1,
  MAD_GRANULE_NEED_INPUT = 0,
  MAD_GRANULE_PCM       = 1,
  MAD_GRANULE_END       = 2
};

struct mad_granule_progress {
  uint64_t granules;		/* successfully synthesized granules */
  uint64_t physical_frames;	/* fully committed physical frames */
  uint64_t sample_frames;	/* delivered samples per channel */
  size_t buffered_bytes;	/* owned compressed + pending frame bytes */
  enum mad_error error;		/* latched stream error, or MAD_ERROR_NONE */
};

struct mad_granule_decoder *mad_granule_new(int options);
void mad_granule_free(struct mad_granule_decoder *);

int mad_granule_feed(struct mad_granule_decoder *,
		     unsigned char const *, size_t length, size_t *accepted);

int mad_granule_finish(struct mad_granule_decoder *);

int mad_granule_next(struct mad_granule_decoder *, struct mad_pcm *);

int mad_granule_progress(struct mad_granule_decoder const *,
			 struct mad_granule_progress *);

void mad_granule_reset(struct mad_granule_decoder *);

# endif
