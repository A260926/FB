#ifndef MINIMP3_PUSH_H
#define MINIMP3_PUSH_H
/*
    https://github.com/lieff/minimp3
    To the extent possible under law, the author(s) have dedicated all copyright and related and neighboring rights to this software to the public domain worldwide.
    This software is distributed without any warranty.
    See <http://creativecommons.org/publicdomain/zero/1.0/>.
*/
#include <stddef.h>
#include <stdint.h>
#include "minimp3.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* mp3dec_push_decode() return values */
#define MP3D_PUSH_OK            0   /* more data may be fed / more PCM may be drained */
#define MP3D_PUSH_DONE          1   /* stream finished, all PCM has been delivered */
#define MP3D_PUSH_E_PARAM       -1  /* invalid argument */
#define MP3D_PUSH_E_TRUNCATED   -2  /* eof with a partial frame or an unfinished tag */
#define MP3D_PUSH_E_UNSUPPORTED -3  /* free-format stream or mid-stream format change */

/*
    Push (streaming) decoder for non-seekable sources.

    The instance owns fixed-size storage only: the Layer III core state
    (mp3dec_t), one compressed-input staging buffer (2048 bytes, enough for
    any MPEG-1/2/2.5 non-free-format frame plus one re-sync point), one
    decoded frame buffer and one output PCM ring (MINIMP3_MAX_SAMPLES_PER_FRAME
    scalars). Total memory is below 32KiB and never grows with the recording
    length or ID3 tag body size, well under the 128KiB budget. No allocation
    is performed.

    Supported streams: MPEG-1/2/2.5 Layer III, ordinary (non-free-format)
    bitrates, mono and stereo, with a constant sample rate and channel count.
    ID3v2.3/2.4 headers (synchsafe size, optional footer) and ID3v1 trailers
    are skipped, Xing/Info gapless tags are honored in the sample domain.
*/
typedef struct
{
    mp3dec_t mp3d;                 /* Layer III decode core, incl. reservoir and synth history */
    uint8_t  in[2048];             /* compressed input staging (>= max frame 1441 + re-sync) */
    size_t   in_fill;              /* valid bytes in in[] */
    mp3d_sample_t frame[MINIMP3_MAX_SAMPLES_PER_FRAME]; /* freshly decoded frame */
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];   /* PCM waiting for delivery */
    size_t   pcm_fill, pcm_pos;    /* ring fill and next scalar to deliver */
    int      phase;                /* internal parser state */
    int      last_error;           /* sticky error, 0 while healthy */
    int      eof;                  /* latched end-of-input flag */
    int      hz, channels, layer;  /* stream geometry, 0 until first frame */
    size_t   id3_remaining;        /* ID3v2 body/footer bytes still to discard */
    size_t   to_skip;              /* gapless encoder delay, in PCM scalars */
    uint64_t detected_samples;     /* gapless target, UINT64_MAX when no tag present */
    uint64_t produced;             /* PCM scalars already accepted into the ring */
} mp3dec_push_t;

/* Initialize / reset an instance. Always succeeds. */
void mp3dec_push_init(mp3dec_push_t *dec);

/* Release an instance. The structure contains no heap memory, this only resets it. */
void mp3dec_push_close(mp3dec_push_t *dec);

/*
    Feed compressed bytes and/or pull interleaved PCM16 in a single call.

    On input:
      *bytes   = number of compressed bytes available (0 is legal);
      *samples = PCM scalar capacity of pcm (0 is legal, stereo delivery is
                 always rounded down to whole L/R frames).
    data may be NULL only when *bytes == 0; pcm may be NULL only when
    *samples == 0. eof != 0 declares that no compressed bytes will ever
    follow; zero-input calls afterwards drain the decoder.

    On return:
      *bytes   = compressed bytes actually consumed (accepted input is never
                 decoded twice, unconsumed input must be presented again);
      *samples = PCM scalars actually written.
    Returns MP3D_PUSH_OK while work remains, MP3D_PUSH_DONE once the whole
    stream and all PCM have been consumed (repeated drain calls keep returning
    DONE with both lengths 0), or a negative MP3D_PUSH_E_* code. PCM delivered
    before an error is never withdrawn.
*/
int mp3dec_push_decode(mp3dec_push_t *dec, const uint8_t *data, size_t *bytes,
                       mp3d_sample_t *pcm, size_t *samples, int eof);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* MINIMP3_PUSH_H */
