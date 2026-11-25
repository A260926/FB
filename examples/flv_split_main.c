/**********************************************************************************************/
/* The MIT License                                                                            */
/*                                                                                            */
/* Copyright 2016-2017 Twitch Interactive, Inc. or its affiliates. All Rights Reserved.       */
/*                                                                                            */
/* Permission is hereby granted, free of charge, to any person obtaining a copy               */
/* of this software and associated documentation files (the "Software"), to deal              */
/* in the Software without restriction, including without limitation the rights               */
/* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell                  */
/* copies of the Software, and to permit persons to whom the Software is                      */
/* furnished to do so, subject to the following conditions:                                   */
/*                                                                                            */
/* The above copyright notice and this permission notice shall be included in                 */
/* all copies or substantial portions of the Software.                                        */
/*                                                                                            */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR                 */
/* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,                   */
/* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE                */
/* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER                     */
/* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,              */
/* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN                  */
/* THE SOFTWARE.                                                                              */
/**********************************************************************************************/
/*
 * flv_split - split one captioned FLV into four single-service FLVs.
 *
 * usage: flv_split input.flv out_cc1.flv out_cc2.flv out_cc3.flv out_cc4.flv
 *
 * Audio and video are copied without re-encoding.  Each output only retains
 * the CEA-608 words belonging to its own service (CC1..CC4), projected out of
 * the ATSC/GA94 caption SEI messages.
 */
#include "caption/flv_splitter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUTPUT_COUNT 4
#define IO_CHUNK 65536

/* The largest legal FLV tag: 11 byte header + 24 bit DataSize + 4 bytes. */
#define MAX_FLV_TAG (11u + 0xFFFFFFu + 4u)

typedef struct {
    FILE* files[OUTPUT_COUNT];
    int failed;
} sink_t;

/*
 * The splitter owns the bytes it passes to this callback.  They stay valid
 * until this function returns, so copy out anything that must outlive the
 * call.  Returning 0 accepts the buffer; the splitter then continues with the
 * next service.
 */
static int emit(unsigned service, const uint8_t* bytes, size_t size, void* opaque)
{
    sink_t* sink = (sink_t*)opaque;

    if (sink->failed || service < 1 || service > OUTPUT_COUNT || !sink->files[service - 1]) {
        return -1;
    }

    if (size && size != fwrite(bytes, 1, size, sink->files[service - 1])) {
        sink->failed = 1;
        return -1;
    }

    return 0;
}

static void close_all(sink_t* sink)
{
    unsigned i;

    for (i = 0; i < OUTPUT_COUNT; ++i) {
        if (sink->files[i]) {
            fclose(sink->files[i]);
            sink->files[i] = NULL;
        }
    }
}

static void remove_outputs(char** paths)
{
    unsigned i;

    for (i = 0; i < OUTPUT_COUNT; ++i) {
        if (paths[i]) {
            remove(paths[i]);
        }
    }
}

/* Feed one chunk, draining any output paused by the sink. */
static int feed_chunk(caption_flv_splitter_t* splitter, const uint8_t* data, size_t size)
{
    size_t offset = 0;

    while (offset < size) {
        size_t consumed = 0;
        int result = caption_flv_splitter_feed(splitter, data + offset, size - offset, &consumed);
        int drained;

        offset += consumed;

        if (result < 0) {
            return -1;
        }

        for (drained = result; 1 == drained;) {
            drained = caption_flv_splitter_feed(splitter, NULL, 0, NULL);
        }

        if (drained < 0) {
            return -1;
        }

        if (0 == consumed && 0 == result) {
            return -1;
        }
    }

    return 0;
}

int main(int argc, char** argv)
{
    sink_t sink;
    caption_flv_splitter_t* splitter = NULL;
    FILE* input = NULL;
    uint8_t* chunk = NULL;
    int status = EXIT_FAILURE;
    unsigned i;

    if (argc != 1 + 1 + OUTPUT_COUNT) {
        fprintf(stderr, "usage: %s input.flv out_cc1.flv out_cc2.flv out_cc3.flv out_cc4.flv\n", argv[0]);
        return EXIT_FAILURE;
    }

    memset(&sink, 0, sizeof(sink));

    input = fopen(argv[1], "rb");

    if (!input) {
        fprintf(stderr, "failed to open input '%s'\n", argv[1]);
        goto cleanup;
    }

    for (i = 0; i < OUTPUT_COUNT; ++i) {
        sink.files[i] = fopen(argv[2 + i], "wb");

        if (!sink.files[i]) {
            fprintf(stderr, "failed to open output '%s'\n", argv[2 + i]);
            goto cleanup;
        }
    }

    splitter = caption_flv_splitter_new(MAX_FLV_TAG, emit, &sink);

    if (!splitter) {
        fprintf(stderr, "failed to create splitter\n");
        goto cleanup;
    }

    chunk = (uint8_t*)malloc(IO_CHUNK);

    if (!chunk) {
        fprintf(stderr, "out of memory\n");
        goto cleanup;
    }

    for (;;) {
        size_t got = fread(chunk, 1, IO_CHUNK, input);

        if (got && feed_chunk(splitter, chunk, got)) {
            fprintf(stderr, "splitter rejected the input\n");
            goto cleanup;
        }

        if (got < IO_CHUNK) {
            if (ferror(input)) {
                fprintf(stderr, "failed to read '%s'\n", argv[1]);
                goto cleanup;
            }

            break;
        }
    }

    {
        int result = caption_flv_splitter_finish(splitter);

        while (1 == result) {
            result = caption_flv_splitter_feed(splitter, NULL, 0, NULL);
        }

        if (result < 0) {
            fprintf(stderr, "splitter failed while finishing\n");
            goto cleanup;
        }
    }

    if (sink.failed) {
        fprintf(stderr, "failed to write output\n");
        goto cleanup;
    }

    for (i = 0; i < OUTPUT_COUNT; ++i) {
        if (fflush(sink.files[i])) {
            fprintf(stderr, "failed to flush output\n");
            goto cleanup;
        }
    }

    status = EXIT_SUCCESS;

cleanup:
    close_all(&sink);

    if (EXIT_SUCCESS != status) {
        remove_outputs(argv + 2);
    }

    free(chunk);
    caption_flv_splitter_free(splitter);
    return status;
}
