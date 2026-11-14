#ifndef SHINE_GAPLESS_H
#define SHINE_GAPLESS_H

/*
 * Gapless MP3 streaming API.
 *
 * The regular Shine entry points (see layer3.h) only accept full PCM
 * blocks of shine_samples_per_pass() samples and shine_flush() merely
 * hands over whatever is sitting in the byte buffer: the tail sound still
 * held by the analysis filter bank and the MDCT overlap history is never
 * pushed out.  This layer wraps the same fixed-point encoding core to
 * provide a gapless writer suited to recordings of unknown length:
 *
 *   - arbitrary PCM fragment sizes may be passed at any time;
 *   - the real beginning and end of the recording are preserved;
 *   - every output file starts with a standard Info/LAME tag frame so
 *     ordinary players know how many samples to trim at each end;
 *   - memory use stays bounded: one PCM block, the encoder bit buffer and
 *     a single tag frame, regardless of the recording length.
 *
 * Supported configurations: MPEG-I Layer III, 44.1 kHz or 48 kHz,
 * 128 kbps, mono (PCM_MONO + MONO) or stereo (PCM_STEREO + STEREO).
 * Every other configuration, or mismatched channel modes, are rejected.
 */

#include <stddef.h>
#include <stdint.h>

#include "layer3.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes returned by the gapless API. 0 denotes success, every
 * error code is negative. */
#define SHINE_GAPLESS_OK 0
/* A parameter was invalid (unsupported configuration, NULL pointer, ...). */
#define SHINE_GAPLESS_ERR_PARAM (-1)
/* Memory allocation failed. */
#define SHINE_GAPLESS_ERR_MEMORY (-2)
/* A user write or patch callback returned a failure. */
#define SHINE_GAPLESS_ERR_CALLBACK (-3)
/* The call is not allowed in the current state (e.g. writing after
 * shine_gapless_finish()). */
#define SHINE_GAPLESS_ERR_STATE (-4)
/* shine_gapless_finish() was called on a stream that never received any
 * audio sample. */
#define SHINE_GAPLESS_ERR_EMPTY (-5)

/* Opaque gapless encoder handle. */
typedef struct shine_gapless shine_gapless_t;

/* Append bytes to the output, in order.  Return 0 on success, a negative
 * value on failure.  The data pointer is only valid for the duration of
 * the call. */
typedef int (*shine_gapless_write_cb)(void *user, const unsigned char *data,
                                      size_t len);

/* Overwrite len bytes previously handed to the write callback, starting at
 * the given absolute file offset.  Return 0 on success, a negative value on
 * failure.  The data pointer is only valid for the duration of the call. */
typedef int (*shine_gapless_patch_cb)(void *user, uint64_t offset,
                                      const unsigned char *data, size_t len);

/* Create a gapless encoder.
 *
 * The configuration is validated and copied; see the file header for the
 * supported configurations.  A complete Info/LAME tag frame (with zeroed
 * count and trimming fields) is emitted through the write callback before
 * this function returns; the final values are written back with the patch
 * callback in shine_gapless_finish().
 *
 * Returns NULL on invalid parameters, allocation failure or if the initial
 * write callback fails. */
shine_gapless_t *shine_gapless_open(const shine_config_t *config, void *user,
                                    shine_gapless_write_cb on_write,
                                    shine_gapless_patch_cb on_patch);

/* Feed interleaved 16-bit PCM.  frames is the number of sample frames,
 * i.e. the number of samples per channel; mono data has one int16 per
 * frame, stereo data two (left then right).  A zero-length write is legal
 * and changes nothing.
 *
 * Encoded bytes are delivered through the write callback as soon as a full
 * MP3 frame is available.  Returns SHINE_GAPLESS_OK or a negative error
 * code.  Once a call has failed the instance is in a sticky error state:
 * further calls return the same error and no more bytes are emitted. */
int shine_gapless_write(shine_gapless_t *g, const int16_t *pcm,
                        size_t frames);

/* Drain the encoder.
 *
 * The trailing partial PCM block is zero padded, one additional silent
 * block is encoded to flush the analysis filter bank and MDCT overlap
 * history, and the remaining bit-buffer bytes are emitted.  The Info tag
 * frame count, total byte count, encoder delay and end padding are then
 * written back with the patch callback.
 *
 * A stream with at least one input sample is fully supported; finishing an
 * empty recording returns SHINE_GAPLESS_ERR_EMPTY.  Calling finish() more
 * than once is allowed: repeat calls succeed and emit no bytes.  No audio
 * is accepted after a successful finish. */
int shine_gapless_finish(shine_gapless_t *g);

/* Release all resources held by the encoder.  The handle is invalid after
 * this call, which is the only cleanup action: it does not emit bytes and
 * never invokes a callback.  Passing NULL is allowed. */
void shine_gapless_close(shine_gapless_t *g);

#ifdef __cplusplus
}
#endif

#endif /* SHINE_GAPLESS_H */
