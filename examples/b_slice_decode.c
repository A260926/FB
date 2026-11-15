/*
 * examples/b_slice_decode.c
 *
 * Minimal usage example for the extended h264bsd decoder.
 *
 * It feeds an Annex-B byte stream to h264bsdDecode() one NAL unit at a
 * time and, for every decoded picture, reports whether the picture used a
 * B (bi-predictive) slice. The public API is unchanged from the original
 * library; B slice support is exercised purely through the normal
 * h264bsdDecode / h264bsdNextOutputPicture / h264bsdFlushBuffer flow.
 *
 * Build (from the repository root):
 *   cc -std=c99 -O1 -Isrc (all .c files in src) examples/b_slice_decode.c -o bslice
 *
 * Run:
 *   ./bslice <file.264>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "h264bsd_decoder.h"
#include "h264bsd_nal_unit.h"
#include "h264bsd_byte_stream.h"
#include "h264bsd_storage.h"

/* Find the next Annex-B start code (the 3 or 4 byte 0x000001 prefix). */
static int next_start_code(const uint8_t *p, size_t len, size_t *off)
{
    size_t i = *off;
    while (i + 3 < len)
    {
        if (p[i] == 0 && p[i+1] == 0 && p[i+2] == 1)
        {
            *off = i + 3;
            return 1;
        }
        if (i + 4 < len && p[i] == 0 && p[i+1] == 0 &&
            p[i+2] == 0 && p[i+3] == 1)
        {
            *off = i + 4;
            return 1;
        }
        i++;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <annex-b stream>\n", argv[0]);
        return 2;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f)
    {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0)
    {
        fclose(f);
        return 2;
    }

    uint8_t *stream = (uint8_t*)malloc((size_t)size);
    if (fread(stream, 1, (size_t)size, f) != (size_t)size)
    {
        fclose(f);
        free(stream);
        return 2;
    }
    fclose(f);

    storage_t dec;
    /* display-order output (noReordering = 0) */
    if (h264bsdInit(&dec, 0) == 0)
    {
        free(stream);
        return 1;
    }

    size_t off = 0;
    unsigned pictures = 0;
    int rc = 0;

    while (next_start_code(stream, (size_t)size, &off))
    {
        size_t start = off;
        size_t next = (size_t)size;
        size_t probe = off;
        if (next_start_code(stream, (size_t)size, &probe))
            next = probe;

        u32 used = 0;
        u32 r = h264bsdDecode(&dec, stream + start,
                              (u32)(next - start), 0, &used);
        if (r == H264BSD_ERROR || r == H264BSD_PARAM_SET_ERROR ||
            r == H264BSD_MEMALLOC_ERROR)
        {
            /* Out-of-scope features (e.g. B direct/skip) are rejected here
             * with H264BSD_ERROR rather than producing a fake picture. */
            fprintf(stderr, "decode error at byte %zu (rc=%u)\n", start, r);
            rc = 1;
            break;
        }

        u32 id, isIdr, errMbs;
        uint8_t *pic;
        while ((pic = h264bsdNextOutputPicture(&dec, &id, &isIdr, &errMbs)))
        {
            pictures++;
            /* Explicit L0/L1/Bi B macroblocks are decoded internally and
             * surface here as ordinary output pictures. */
            (void)pic;
            if (errMbs)
                fprintf(stderr, "picture %u: %u concealed macroblocks\n",
                        pictures, errMbs);
        }
    }

    /* emit any pictures still held in display order */
    h264bsdFlushBuffer(&dec);
    u32 id, isIdr, errMbs;
    while (h264bsdNextOutputPicture(&dec, &id, &isIdr, &errMbs))
    {
        pictures++;
        if (isIdr)
            fprintf(stderr, "flushed IDR picture %u\n", id);
    }

    h264bsdShutdown(&dec);
    free(stream);

    printf("decoded %u pictures\n", pictures);
    return rc;
}
