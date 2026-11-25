#ifndef SMK_AVI_H
#define SMK_AVI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Description of one PCM output stream. */
typedef struct smk_avi_audio_format {
    uint32_t samples_per_second;
    uint16_t channels;
    uint16_t bits_per_sample;
} smk_avi_audio_format;

/* Opaque RIFF/AVI muxer. */
typedef struct smk_avi_mux smk_avi_mux;

/*
 * Create an interleaved AVI file.  Stream 0 is always video; audio streams
 * are numbered 1..audio_count and get chunk ids 01wb, 02wb, ...
 */
int smk_avi_open(const char *path,
                 uint32_t width,
                 uint32_t height,
                 uint32_t frame_us,
                 uint32_t reduced_scale,
                 uint32_t reduced_rate,
                 const smk_avi_audio_format *audio,
                 unsigned audio_count,
                 smk_avi_mux **out);

/* Number of bytes in one bottom-up, 24 bpp BGR DIB row. */
uint32_t smk_avi_bgr_stride(uint32_t width);

/* Start a media chunk.  Exactly declared_size payload bytes must follow. */
int smk_avi_video_begin(smk_avi_mux *mux, uint32_t declared_size);
int smk_avi_audio_begin(smk_avi_mux *mux, unsigned audio_index, uint32_t declared_size);

/* Write part of the current chunk. */
int smk_avi_write(smk_avi_mux *mux, const void *data, size_t size);

/* Finish the current chunk, adding RIFF padding and an idx1 record. */
int smk_avi_chunk_end(smk_avi_mux *mux);

/*
 * Patch headers and idx1, flush and close.  On failure the mux object remains
 * allocated with its FILE closed; call smk_avi_abort() to remove the output.
 */
int smk_avi_finish(smk_avi_mux **mux);

/* Close if necessary, remove the incomplete output and free the mux. */
void smk_avi_abort(smk_avi_mux **mux);

#ifdef __cplusplus
}
#endif

#endif
