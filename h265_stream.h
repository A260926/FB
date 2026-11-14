#ifndef H265_STREAM_H
#define H265_STREAM_H
/*
 * Streaming HEVC (H.265) Annex-B to MP4 muxer.
 *
 * MP4H265_stream_t accepts a raw HEVC byte stream fed in arbitrary chunks
 * (a start code, a NAL header or even a single byte may span two feed()
 * calls) and emits exactly one MP4 sample per coded picture, assembling all
 * slices and the suffix SEI of that picture together. The legacy
 * mp4_h26x_write_nal() entry point in minimp4.h is kept unchanged.
 *
 * Only single-layer streams without picture reordering are accepted:
 * any NAL with layer_id != 0 or temporal_id_plus1 != 1 is rejected.
 */

#include <stddef.h>
#include "minimp4.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque streaming muxer instance. */
typedef struct MP4H265_stream_tag MP4H265_stream_t;

/*
 * Create an HEVC track on the given (already opened) muxer and return a
 * streaming instance, or NULL on failure. The muxer must not use fragmented
 * mode; default and sequential modes are supported.
 *
 * The track uses a 90000 Hz time base. fps_num/fps_den must be non-zero and
 * 90000*fps_den/fps_num must be representable as a positive int, otherwise
 * the call fails. The external muxer is never closed by this wrapper.
 */
MP4H265_stream_t *MP4H265_stream_open(MP4E_mux_t *mux, int width, int height,
                                      int fps_num, int fps_den);

/*
 * Feed len bytes of Annex-B data. Splitting has no semantic meaning: NAL
 * units and start codes may be split across calls arbitrarily. A zero-length
 * feed is allowed and does nothing. Returns an MP4E_STATUS_* code. Once a
 * call fails the instance is dead and every later call fails too.
 */
int MP4H265_stream_feed(MP4H265_stream_t *st, const unsigned char *data, size_t len);

/*
 * Flush the picture still held in the parser (if any) as the last sample.
 * Safe to call repeatedly: only the first call can write a frame, later
 * calls return the same status without writing again. Feeding after a
 * successful finish fails. Returns an MP4E_STATUS_* code.
 */
int MP4H265_stream_finish(MP4H265_stream_t *st);

/*
 * Free the instance only. The MP4E_mux_t passed to open is not closed and
 * still has to be finalized with MP4E_close() by the caller.
 */
void MP4H265_stream_close(MP4H265_stream_t *st);

#ifdef __cplusplus
}
#endif

#endif /* H265_STREAM_H */
