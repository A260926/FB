#ifndef MINIMP3_PUSH_H
#define MINIMP3_PUSH_H
/*
    Push (streaming, forward-only, non-seekable) decoding API for minimp3.
    https://github.com/lieff/minimp3
    To the extent possible under law, the author(s) have dedicated all copyright and related and neighboring rights to this software to the public domain worldwide.
    This software is distributed without any warranty.
    See <http://creativecommons.org/publicdomain/zero/1.0/>.
*/
#include <stddef.h>
#include <stdint.h>
#include "minimp3.h"

/* mp3dec_push_decode() return codes */
#define MP3D_PUSH_OK        0   /* progress made or waiting for more data/drain space */
#define MP3D_PUSH_DONE      1   /* all input consumed and all PCM delivered, stream finished */
#define MP3D_PUSH_E_PARAM   -1  /* invalid argument (null decoder/length pointer, output null with capacity) */
#define MP3D_PUSH_E_TRUNC   -2  /* eof reached in the middle of a frame or tag header */
#define MP3D_PUSH_E_FORMAT  -3  /* unsupported stream: free-format, non-Layer-III, or mid-stream format change */

/* bounded internal storage: input accumulation buffer + one frame of output PCM */
#define MINIMP3_PUSH_INPUT_CAP   (16*1024)                      /* holds >=10 worst-case frames */
#define MINIMP3_PUSH_PCM_CAP     MINIMP3_MAX_SAMPLES_PER_FRAME /* 2304 interleaved scalars */

typedef struct
{
    mp3dec_t        mp3d;
    /* input accumulation state */
    uint8_t         input[MINIMP3_PUSH_INPUT_CAP];
    size_t          input_fill;
    /* decoded, not yet delivered PCM (at most one full frame) */
    mp3d_sample_t   pcm[MINIMP3_PUSH_PCM_CAP];
    size_t          pcm_fill, pcm_used;
    /* stream parsing phase */
    int             phase;          /* 0: ID3v2 header probe, 1: tag body skip, 2: audio frames */
    size_t          id3_left;       /* ID3v2 tag bytes still to discard */
    int             id3v1_checked;  /* trailing ID3v1/APE strip already applied */
    /* locked audio format */
    int             format_locked;
    int             hz, channels, layer;
    /* gapless (Xing/Info + LAME) trimming state */
    int             vbr_checked;
    int             have_gapless;
    int             to_skip;                    /* leading scalars to discard */
    unsigned long long detected_samples;       /* final scalar count when gapless */
    unsigned long long produced_samples;       /* scalars already accepted for output */
    /* terminal state */
    int             eof_seen;
    int             finished;
    int             error;                      /* sticky MP3D_PUSH_E_* code */
} mp3dec_push_t;

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* Initialize a push decoder instance. The structure holds all storage, no allocation is made. */
int  mp3dec_push_init(mp3dec_push_t *dec);

/* Tear down a push decoder instance. Currently only zeroes the structure. */
void mp3dec_push_close(mp3dec_push_t *dec);

/*
    Feed one fragment of a forward-only MP3 stream and/or drain decoded PCM.

    On input *bytes is the number of bytes available at `input` (may be 0, and
    `input` may be NULL in that case) and *samples is the PCM scalar capacity of
    `output` (may be 0). On return *bytes is the number of bytes actually
    consumed and *samples the number of interleaved PCM16 scalars written.

    Stereo output is always delivered in complete left/right frames (pairs of
    scalars); already consumed input is never decoded twice.

    eof != 0 means no further input bytes will ever follow. Further calls with
    *bytes == 0 drain the remaining PCM. When everything is drained the call
    returns MP3D_PUSH_DONE with both lengths 0, repeatedly.

    Returns MP3D_PUSH_OK while the stream is active, MP3D_PUSH_DONE when
    finished, or a negative MP3D_PUSH_E_* code. PCM already delivered before an
    error is not withdrawn.
*/
int  mp3dec_push_decode(mp3dec_push_t *dec, const uint8_t *input, size_t *bytes,
                        mp3d_sample_t *output, size_t *samples, int eof);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* MINIMP3_PUSH_H */
