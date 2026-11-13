/*
    push_decode_example.c - feed a file to the push decoder in fixed-size
    chunks, the way a forward-only recording source would deliver data.

    Build: cc -std=c99 -I. minimp3_push.c examples/push_decode_example.c -lm -o push_example
    Run:   ./push_example <file.mp3> [input_chunk_bytes] [output_cap_scalars]
*/
#include "minimp3_push.h"

#include <stdio.h>
#include <stdlib.h>

#define DEFAULT_IN_CHUNK   113
#define DEFAULT_OUT_CAP    257

static size_t read_all(const char *path, uint8_t **out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz)
    {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    *out = buf;
    return (size_t)sz;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("usage: %s <file.mp3> [in_chunk_bytes] [out_cap_scalars]\n", argv[0]);
        return 2;
    }
    size_t in_chunk = argc > 2 ? (size_t)strtoul(argv[2], NULL, 10) : DEFAULT_IN_CHUNK;
    size_t out_cap  = argc > 3 ? (size_t)strtoul(argv[3], NULL, 10) : DEFAULT_OUT_CAP;
    if (!in_chunk)
        in_chunk = DEFAULT_IN_CHUNK;
    if (!out_cap)
        out_cap = DEFAULT_OUT_CAP;

    uint8_t *data = NULL;
    size_t total = read_all(argv[1], &data);
    if (!data)
    {
        printf("cannot read %s\n", argv[1]);
        return 2;
    }

    mp3dec_push_t dec;
    mp3dec_push_init(&dec);
    mp3d_sample_t *pcm = (mp3d_sample_t *)malloc(out_cap*sizeof(mp3d_sample_t));

    size_t pos = 0, consumed_total = 0, pcm_total = 0, nonzero = 0;
    int rc = MP3D_PUSH_OK;

    while (rc == MP3D_PUSH_OK)
    {
        const uint8_t *chunk = NULL;
        size_t avail = 0;
        int eof = 0;
        if (pos < total)
        {
            avail = in_chunk < total - pos ? in_chunk : total - pos;
            chunk = data + pos;
            eof = (pos + avail == total);
        }
        else
        {
            eof = 1; /* once the file is exhausted, zero-input calls drain */
        }

        size_t bytes = avail, samples = out_cap;
        rc = mp3dec_push_decode(&dec, chunk, &bytes, pcm, &samples, eof);

        for (size_t i = 0; i < samples; i++)
            if (pcm[i] != 0)
                nonzero++;
        pos += bytes;             /* resend unconsumed input on the next call */
        consumed_total += bytes;
        pcm_total += samples;
    }

    printf("status=%d (%s) consumed=%zu/%zu pcm_scalars=%zu nonzero=%zu\n",
           rc, rc == MP3D_PUSH_DONE ? "done" : "error",
           consumed_total, total, pcm_total, nonzero);

    free(pcm);
    free(data);
    mp3dec_push_close(&dec);
    return rc == MP3D_PUSH_DONE ? 0 : 1;
}
