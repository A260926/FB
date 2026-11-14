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
#include "caption.h"
#include "scc_timeline.h"
#include "utf8.h"
#include "vtt.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

// parses a non-negative integer transmission frame number
static int parse_frame(const char* text, uint32_t* frame)
{
    if (!text || !text[0]) {
        return 0;
    }

    for (const char* p = text; *p; ++p) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
    }

    errno = 0;
    char* end = NULL;
    unsigned long long value = strtoull(text, &end, 10);

    if (errno || !end || *end || 0xFFFFFFFFull < value) {
        return 0;
    }

    (*frame) = (uint32_t)value;
    return 1;
}

int main(int argc, char** argv)
{
    if (argc != 4) {
        fprintf(stderr, "Usage: scc2vtt input.scc begin_frame end_frame\n\n"
                        "Exports the half-open window [begin_frame, end_frame) of integer\n"
                        "transmission frames at 30000/1001 fps as WebVTT on stdout\n");
        return EXIT_FAILURE;
    }

    uint32_t begin_frame = 0, end_frame = 0;

    if (!parse_frame(argv[2], &begin_frame) || !parse_frame(argv[3], &end_frame) || begin_frame >= end_frame) {
        fprintf(stderr, "scc2vtt: invalid frame window '%s' '%s'\n", argv[2], argv[3]);
        return EXIT_FAILURE;
    }

    FILE* probe = fopen(argv[1], "rb");

    if (!probe) {
        fprintf(stderr, "scc2vtt: unable to open '%s'\n", argv[1]);
        return EXIT_FAILURE;
    }

    fclose(probe);

    size_t scc_size = 0;
    utf8_char_t* scc_data = utf8_load_text_file(argv[1], &scc_size);

    if (!scc_data) {
        fprintf(stderr, "scc2vtt: unable to read '%s'\n", argv[1]);
        return EXIT_FAILURE;
    }

    scc_timeline_error_t error = SCC_TIMELINE_OK;
    size_t error_line = 0;
    vtt_t* vtt = scc_timeline_to_vtt(scc_data, scc_size, begin_frame, end_frame, &error, &error_line);

    if (!vtt) {
        fprintf(stderr, "scc2vtt: import failed with error %d at line %zu\n", (int)error, error_line);
        free(scc_data);
        return EXIT_FAILURE;
    }

    vtt_dump(vtt);
    vtt_free(vtt);
    free(scc_data);
    return EXIT_SUCCESS;
}
