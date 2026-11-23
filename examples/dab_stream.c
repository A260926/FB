/*
 *  examples/dab_stream.c
 *
 *  Encode PCM to a DAB-ready MP2 file in DAB streaming mode: the library
 *  inserts each frame's ScF-CRC into the previous frame by itself, so the
 *  caller just feeds arbitrarily sized blocks of audio and writes out
 *  finished MP2 frames.
 *
 *  Usage: dab_stream <output.mp2>
 */

#include "twolame.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 48000
#define CHANNELS 2
#define BITRATE 192
#define DAB_CRC_LEN 2
/* room for the ScF-CRC plus 2 bytes of F-PAD */
#define ANCILLARY_BITS (8 * (DAB_CRC_LEN + 2))

/* three full frames plus a partial frame of non-zero sound */
#define TOTAL (3 * TWOLAME_SAMPLES_PER_FRAME + 373)

static short pcm_left[TOTAL];
static short pcm_right[TOTAL];

int main(int argc, char **argv)
{
    twolame_options *opts;
    FILE *outfile;
    int i, pos, written = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <output.mp2>\n", argv[0]);
        return 2;
    }

    /* non-zero test sound: two sines plus a little deterministic noise */
    {
        unsigned int rng = 0x12345678u;
        for (i = 0; i < TOTAL; i++) {
            double t = (double) i / (double) RATE;
            double v;
            rng = rng * 1664525u + 1013904223u;
            v = 10000.0 * sin(6.283185307179586 * 440.0 * t)
                + 4000.0 * sin(6.283185307179586 * 1250.0 * t)
                + 1500.0 * ((double) (int) (rng >> 16) - 32768.0) / 32768.0;
            pcm_left[i] = (short) v;
            pcm_right[i] = (short) (0.8 * v + 2000.0 * sin(6.283185307179586 * 880.0 * t));
        }
    }

    opts = twolame_init();
    if (opts == NULL) {
        fprintf(stderr, "twolame_init failed\n");
        return 1;
    }
    /* every parameter is set before twolame_init_params() */
    twolame_set_num_channels(opts, CHANNELS);
    twolame_set_in_samplerate(opts, RATE);
    twolame_set_out_samplerate(opts, RATE);
    twolame_set_bitrate(opts, BITRATE);
    twolame_set_mode(opts, TWOLAME_STEREO);
    twolame_set_verbosity(opts, 0);
    twolame_set_padding(opts, TWOLAME_PAD_ALL);
    twolame_set_DAB(opts, 1);
    twolame_set_DAB_crc_length(opts, DAB_CRC_LEN);
    twolame_set_DAB_xpad_length(opts, 0);
    twolame_set_num_ancillary_bits(opts, ANCILLARY_BITS);
    twolame_set_DAB_streaming(opts, 1);
    if (twolame_init_params(opts) != 0) {
        fprintf(stderr, "twolame_init_params failed\n");
        twolame_close(&opts);
        return 1;
    }

    outfile = fopen(argv[1], "wb");
    if (outfile == NULL) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        twolame_close(&opts);
        return 1;
    }

    /* feed the audio in irregular blocks, like a capture service would */
    pos = 0;
    while (pos < TOTAL) {
        int chunk = 997;
        unsigned char out[7200 + 2 * 997];
        int n;
        if (chunk > TOTAL - pos)
            chunk = TOTAL - pos;
        n = twolame_encode_buffer(opts, pcm_left + pos, pcm_right + pos, chunk,
                                  out, sizeof(out));
        if (n < 0) {
            fprintf(stderr, "twolame_encode_buffer failed\n");
            fclose(outfile);
            twolame_close(&opts);
            return 1;
        }
        /* returned frames are finished: the ScF-CRC backfill already happened */
        fwrite(out, 1, n, outfile);
        written += n;
        pos += chunk;
    }

    /* finish the stream: pads the partial frame, backfills the previous one
       and hands over the last frame(s); the final frame carries a zero
       ScF-CRC as the end-of-stream marker */
    {
        unsigned char out[7200];
        int n = twolame_encode_flush(opts, out, sizeof(out));
        if (n < 0) {
            fprintf(stderr, "twolame_encode_flush failed\n");
            fclose(outfile);
            twolame_close(&opts);
            return 1;
        }
        fwrite(out, 1, n, outfile);
        written += n;
    }

    fclose(outfile);
    twolame_close(&opts);

    printf("DAB_STREAM_EXAMPLE file=%s samples=%d bytes=%d frames=%d\n",
           argv[1], TOTAL, written, (TOTAL + TWOLAME_SAMPLES_PER_FRAME - 1)
           / TWOLAME_SAMPLES_PER_FRAME);
    return 0;
}
