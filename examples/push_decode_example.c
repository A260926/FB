/*
    Example: decode a forward-only (non-seekable) MP3 stream with the push API.

    Reads the named file in fixed-size fragments, feeds them to
    mp3dec_push_decode() and writes the resulting interleaved PCM16 to stdout.
    Works the same when the bytes arrive from a network/socket source instead
    of a file, since the decoder never seeks backwards.

    Build: cc -std=c99 -I. minimp3_push.c examples/push_decode_example.c -lm -o push_example
    Use:   ./push_example input.mp3 > output.pcm
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "minimp3_push.h"

#define INPUT_CHUNK  256
#define OUTPUT_CAP   MINIMP3_PUSH_PCM_CAP

int main(int argc, char **argv)
{
    FILE *fp;
    uint8_t in[INPUT_CHUNK];
    mp3d_sample_t out[OUTPUT_CAP];
    mp3dec_push_t dec;
    int rc;

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s input.mp3 > output.pcm\n", argv[0]);
        return 2;
    }
    fp = fopen(argv[1], "rb");
    if (!fp)
    {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    if (mp3dec_push_init(&dec) != MP3D_PUSH_OK)
        return 2;

    for (;;)
    {
        size_t bytes = fread(in, 1, sizeof(in), fp);
        int eof = (bytes < sizeof(in));   /* last fragment (or a read error) */
        size_t samples = OUTPUT_CAP;

        rc = mp3dec_push_decode(&dec, in, &bytes, out, &samples, eof);

        if (samples && fwrite(out, sizeof(mp3d_sample_t), samples, stdout) != samples)
        {
            fprintf(stderr, "write failed\n");
            rc = MP3D_PUSH_E_TRUNC;
            break;
        }
        /* `bytes` bytes have been consumed; the rest stay in the caller's buffer
           and are simply presented again together with the next fragment. */

        if (rc == MP3D_PUSH_DONE)
            break;                        /* everything decoded and delivered */
        if (rc < 0)
        {
            fprintf(stderr, "decode error %d\n", rc);
            break;
        }
        if (eof)
        {
            /* no more input bytes: keep calling with zero input to drain the
               last buffered PCM until MP3D_PUSH_DONE is returned. */
            for (;;)
            {
                size_t zb = 0, zs = OUTPUT_CAP;
                rc = mp3dec_push_decode(&dec, NULL, &zb, out, &zs, 1);
                if (zs && fwrite(out, sizeof(mp3d_sample_t), zs, stdout) != zs)
                {
                    fprintf(stderr, "write failed\n");
                    break;
                }
                if (rc != MP3D_PUSH_OK)
                    break;
            }
            break;
        }
    }

    mp3dec_push_close(&dec);
    fclose(fp);
    return rc == MP3D_PUSH_DONE ? 0 : 1;
}
