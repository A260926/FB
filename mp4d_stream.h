#ifndef MP4D_STREAM_H
#define MP4D_STREAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Incremental demuxer for fragmented MP4 (ftyp/moov init segment followed by
 * consecutive moof/mdat fragments).
 *
 * The demuxer keeps only the initialization segment, the description of the
 * current moof and one sample buffer.  Complete samples are handed to the
 * callback as soon as their last byte has been fed; the payload pointer is
 * borrowed and valid only for the duration of the callback.
 */
typedef struct MP4D_stream MP4D_stream_t;

typedef struct
{
    uint32_t track_id;
    uint32_t timescale;
    uint64_t dts;
    int64_t pts;
    uint32_t duration;
    uint32_t size;
    uint32_t flags;
} MP4D_stream_sample_t;

/* Returns 0 on success, non-zero to fail the stream permanently. */
typedef int (*MP4D_stream_cb)(const MP4D_stream_sample_t *sample,
    const unsigned char *payload, void *token);

/*
 * limit independently bounds the retained init segment, the storage for the
 * current moof description (including derived sample descriptions) and the
 * single sample buffer.
 */
MP4D_stream_t *MP4D_stream_open(size_t limit, MP4D_stream_cb cb, void *token);

/* 0 on success, negative on failure.  A failure is persistent. */
int MP4D_stream_feed(MP4D_stream_t *st, const void *data, size_t bytes);

/* 0 on success (idempotent), negative on failure. */
int MP4D_stream_finish(MP4D_stream_t *st);

void MP4D_stream_close(MP4D_stream_t *st);

#ifdef __cplusplus
}
#endif

#endif /* MP4D_STREAM_H */
