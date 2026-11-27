#ifndef H265_STREAM_H
#define H265_STREAM_H

#include <stddef.h>
#include "minimp4.h"

#ifdef __cplusplus
extern "C" {
#endif

/************************************************************************/
/*          Streaming HEVC (H.265) Annex-B -> MP4 writer                */
/*                                                                      */
/*  Accepts a raw HEVC elementary stream in arbitrary byte chunks,      */
/*  groups NAL units into access units (pictures) and commits one       */
/*  MP4 sample per picture.  Unlike mp4_h26x_write_nal(), chunk         */
/*  boundaries carry no meaning: start codes, NAL headers and slice     */
/*  headers may be split across two feed() calls.                       */
/*                                                                      */
/*  Restrictions: single layer (nuh_layer_id == 0), single temporal     */
/*  layer (nuh_temporal_id_plus1 == 1), no parameter sets after the     */
/*  first VCL NAL unit, and the first VCL NAL unit must be an IRAP      */
/*  picture with VPS, SPS and PPS already received.                     */
/*                                                                      */
/*  Works with the default and the sequential mux modes; the            */
/*  fragmented (fMP4) mux mode is not supported.                        */
/************************************************************************/

typedef struct MP4H265_stream_tag MP4H265_stream_t;

/**
*   Create a stream writer on top of an existing mux.  Adds one HEVC
*   video track (timescale 90000) to the mux.
*   fps_num/fps_den is the average frame rate; frame n (counted from 0)
*   gets decode time floor(n*90000*fps_den/fps_num) and the duration
*   given by the difference of successive decode times.
*   Both fps_num and fps_den must be positive and the per-frame duration
*   must be representable as a positive int.
*   Returns the writer instance, or NULL on failure.
*/
MP4H265_stream_t *MP4H265_stream_open(MP4E_mux_t *mux, int width, int height, int fps_num, int fps_den);

/**
*   Feed a chunk of Annex-B byte stream.  The chunk may start and end
*   anywhere, including in the middle of a start code or a NAL header.
*   Feeding zero bytes is allowed and does nothing.
*   Returns MP4E_STATUS_OK or an MP4E_STATUS_* error code.  After an
*   error the instance is broken: all subsequent feed/finish calls
*   return the same error and no more samples are written.
*/
int MP4H265_stream_feed(MP4H265_stream_t *stream, const unsigned char *data, size_t bytes);

/**
*   Signal end of stream: commit the last pending picture, if any.
*   Returns the result of committing the last frame (MP4E_STATUS_OK
*   when there was nothing left to commit).  May be called repeatedly;
*   successful calls after the first one do nothing.  Feeding after a
*   successful finish is an error.
*/
int MP4H265_stream_finish(MP4H265_stream_t *stream);

/**
*   Free the writer instance.  Does NOT close the mux it was created on.
*/
void MP4H265_stream_close(MP4H265_stream_t *stream);

#ifdef __cplusplus
}
#endif

#endif /* H265_STREAM_H */
