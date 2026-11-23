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
// Drives a CC1 pop-on playout from an edit script.
//
// Script, one command per line:
//   SET id frame TEXT   schedule/replace id; TEXT goes in the last row and
//                       '-' means an empty screen
//   CANCEL id           remove a not-yet-committed event
//   PULL count          emit count "frame hexword" lines, one word per frame
//
// Commands run in order. Any malformed or rejected command exits nonzero and
// removes the partially written output file.
#include "playout.h"
#include "eia608.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(FILE* out, const char* path)
{
    if (out) {
        fclose(out);
    }

    remove(path);
    return 1;
}

// Parses a non-negative uint64 with no leading sign or trailing junk; rejects
// overflow.
static int parse_u64(const char* s, uint64_t* value)
{
    if (!s || !isdigit((unsigned char)s[0])) {
        return 0;
    }

    errno = 0;
    char* end = 0;
    unsigned long long v = strtoull(s, &end, 10);

    if (errno || *end != '\0') {
        return 0;
    }

    *value = (uint64_t)v;
    return 1;
}

static int blank_line(const char* s)
{
    while (*s) {
        if (!isspace((unsigned char)*s)) {
            return 0;
        }

        ++s;
    }

    return 1;
}

int main(int argc, char** argv)
{
    if (3 != argc) {
        fprintf(stderr, "usage: %s script output\n", argv[0]);
        return 1;
    }

    FILE* script = fopen(argv[1], "rb");

    if (!script) {
        fprintf(stderr, "cannot open script %s\n", argv[1]);
        return 1;
    }

    FILE* out = fopen(argv[2], "wb");

    if (!out) {
        fclose(script);
        fprintf(stderr, "cannot open output %s\n", argv[2]);
        return 1;
    }

    cc_playout_t* playout = cc_playout_create(0);

    if (!playout) {
        return fail(out, argv[2]);
    }

    char line[512];
    int status = 0;

    while (fgets(line, sizeof(line), script)) {
        if (blank_line(line)) {
            continue;
        }

        // Reject a line too long for the fixed buffer.
        if (!strchr(line, '\n') && !feof(script)) {
            status = 1;
            break;
        }

        char command[32], id[CC_PLAYOUT_MAX_ID], text[SCREEN_COLS + 1];
        char frame_buf[32], extra[2];
        int fields = sscanf(line, "%31s %255s %31s %32s %1s", command, id, frame_buf, text, extra);

        if (fields >= 1 && !strcmp(command, "SET")) {
            if (4 != fields) {
                status = 1;
                break;
            }

            uint64_t frame;

            if (!parse_u64(frame_buf, &frame)) {
                status = 1;
                break;
            }

            if (strlen(text) > SCREEN_COLS) {
                status = 1;
                break;
            }

            caption_frame_t screen;
            caption_frame_init(&screen);
            screen.write = &screen.front;

            if (strcmp(text, "-")) {
                for (size_t i = 0; text[i]; ++i) {
                    utf8_char_t ch[2] = { text[i], 0 };

                    if (!caption_frame_write_char(&screen, SCREEN_ROWS - 1, (int)i, eia608_style_white, 0, ch)) {
                        status = 1;
                        break;
                    }
                }
            }

            if (status) {
                break;
            }

            if (CC_PLAYOUT_OK != cc_playout_set(playout, id, frame, &screen)) {
                status = 1;
                break;
            }
        } else if (fields >= 1 && !strcmp(command, "CANCEL")) {
            if (2 != fields) {
                status = 1;
                break;
            }

            if (CC_PLAYOUT_OK != cc_playout_cancel(playout, id)) {
                status = 1;
                break;
            }
        } else if (fields >= 1 && !strcmp(command, "PULL")) {
            if (2 != fields) {
                status = 1;
                break;
            }

            uint64_t count;

            if (!parse_u64(id, &count) || count > (uint64_t)INT_MAX) {
                status = 1;
                break;
            }

            cc_playout_record_t* records = (cc_playout_record_t*)malloc(sizeof(cc_playout_record_t) * (count ? count : 1));

            if (!records) {
                status = 1;
                break;
            }

            int64_t got = cc_playout_pull(playout, count, records);

            if (got != (int64_t)count) {
                free(records);
                status = 1;
                break;
            }

            for (uint64_t i = 0; i < count; ++i) {
                if (fprintf(out, "%llu %04X\n", (unsigned long long)records[i].frame, records[i].cc_data) < 0) {
                    free(records);
                    status = 1;
                    break;
                }
            }

            free(records);

            if (status) {
                break;
            }
        } else {
            status = 1;
            break;
        }
    }

    fclose(script);
    cc_playout_free(playout);

    if (status || ferror(out) || fclose(out)) {
        return fail(0, argv[2]);
    }

    return 0;
}
