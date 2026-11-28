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
#include "caption/caption.h"
#include "caption/diff608.h"
#include "caption/playout.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Script line grammar:
//   SET id frame TEXT   TEXT goes on the last row; "-" is an empty screen
//   CANCEL id
//   PULL count          emits "frame hexword" lines, one per pulled frame
// The output file is removed whenever a command fails, so a partial run can
// never be mistaken for a complete transcript.

static int supported_text(const char* text)
{
    const char* p;

    if (!strcmp(text, "-")) {
        return 1;
    }

    if (!*text || strlen(text) > SCREEN_COLS) {
        return 0;
    }

    for (p = text; *p; ++p) {
        if (!(('A' <= *p && *p <= 'Z') || ('0' <= *p && *p <= '9'))) {
            return 0;
        }
    }

    return 1;
}

static int parse_u64(const char* token, uint64_t* out)
{
    char* end = 0;

    if (!token || strchr(token, '-')) {
        return 0;
    }

    errno = 0;
    *out = strtoull(token, &end, 10);

    if (errno || !end || *end) {
        return 0;
    }

    return 1;
}

static int parse_size(const char* token, size_t* out)
{
    char* end = 0;
    unsigned long long value;

    if (!token || strchr(token, '-')) {
        return 0;
    }

    errno = 0;
    value = strtoull(token, &end, 10);

    if (errno || !end || *end || value > SIZE_MAX) {
        return 0;
    }

    *out = (size_t)value;
    return 1;
}

// Marks the run failed and removes any partial transcript. The output handle
// is closed once by the single cleanup at the end of main.
static int fail(const char* path, const char* line, size_t number)
{
    remove(path);
    fprintf(stderr, "invalid script line %zu: %s\n", number, line);
    return 1;
}

int main(int argc, char** argv)
{
    FILE* script;
    FILE* out = 0;
    cc_playout_t* playout;
    char line[1024];
    size_t number = 0;
    int rc = 0;

    if (3 != argc) {
        fprintf(stderr, "usage: %s edit-script output\n", argv[0]);
        return 2;
    }

    script = fopen(argv[1], "rb");

    if (!script) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }

    playout = cc_playout_create(0);

    if (!playout) {
        fclose(script);
        return 3;
    }

    while (fgets(line, sizeof(line), script)) {
        char command[16], id[512], text[128];
        char frame_token[32], extra[16];
        uint64_t frame;
        size_t count, consumed;
        int fields;

        ++number;
        command[0] = id[0] = text[0] = frame_token[0] = extra[0] = '\0';

        if ('\n' == line[0] || '\r' == line[0]) {
            continue;
        }

        fields = sscanf(line, "%15s %511s %31s %127s %15s", command, id, frame_token, text, extra);

        if (fields >= 1 && !strcmp(command, "SET")) {
            caption_frame_t snapshot;

            if (4 != fields || !parse_u64(frame_token, &frame) || !supported_text(text)) {
                rc = fail(argv[2], line, number);
                break;
            }

            caption_frame_init(&snapshot);
            snapshot.write = &snapshot.front;

            if (strcmp(text, "-")) {
                for (count = 0; text[count]; ++count) {
                    char ch[2] = { text[count], 0 };

                    if (!caption_frame_write_char(&snapshot, SCREEN_ROWS - 1, (int)count,
                            eia608_style_white, 0, ch)) {
                        rc = fail(argv[2], line, number);
                        break;
                    }
                }

                if (rc) {
                    break;
                }
            }

            {
                cc_playout_status_t status = cc_playout_set(playout, id, frame, &snapshot);

                if (CC_PLAYOUT_OK != status) {
                    if (CC_PLAYOUT_ERROR_DEADLINE == status) {
                        fprintf(stderr, "line %zu: event %s cannot meet frame %" PRIu64 "\n", number, id, frame);
                    }

                    rc = fail(argv[2], line, number);
                    break;
                }
            }
        } else if (fields >= 1 && !strcmp(command, "CANCEL")) {
            if (2 != fields || CC_PLAYOUT_OK != cc_playout_cancel(playout, id)) {
                rc = fail(argv[2], line, number);
                break;
            }
        } else if (fields >= 1 && !strcmp(command, "PULL")) {
            if (2 != fields || !parse_size(id, &count)) {
                rc = fail(argv[2], line, number);
                break;
            }

            if (!out) {
                out = fopen(argv[2], "wb");

                if (!out) {
                    fprintf(stderr, "cannot open %s\n", argv[2]);
                    rc = 2;
                    break;
                }
            }

            if (count) {
                cc_playout_record_t* records = (cc_playout_record_t*)calloc(count, sizeof(cc_playout_record_t));

                if (!records) {
                    rc = fail(argv[2], line, number);
                    break;
                }

                consumed = cc_playout_pull(playout, count, records);

                if (consumed != count) {
                    free(records);
                    rc = fail(argv[2], line, number);
                    break;
                }

                for (size_t i = 0; i < count; ++i) {
                    if (fprintf(out, "%" PRIu64 " %04" PRIX16 "\n", records[i].frame, records[i].cc_data) < 0) {
                        free(records);
                        rc = fail(argv[2], line, number);
                        break;
                    }
                }

                free(records);

                if (rc) {
                    break;
                }
            }
        } else {
            rc = fail(argv[2], line, number);
            break;
        }
    }

    if (!rc && ferror(script)) {
        rc = fail(argv[2], line, number);
    }

    fclose(script);

    if (out) {
        if (fclose(out)) {
            remove(argv[2]);
            rc = rc ? rc : 1;
        } else if (rc) {
            remove(argv[2]); // a failed run never leaves a partial transcript
        }
    } else if (!rc) {
        // A valid script with no PULL still produces an empty transcript.
        out = fopen(argv[2], "wb");

        if (!out) {
            rc = 2;
        } else {
            fclose(out);
        }
    }

    cc_playout_free(playout);
    return rc;
}
