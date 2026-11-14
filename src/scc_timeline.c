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
#include "scc_timeline.h"
#include "eia608.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SCC_TIMELINE_HEADER "Scenarist_SCC V1.0"
#define SCC_TIMELINE_HEADER_SIZE 18

// seconds covered by one integer transmission frame at 30000/1001 fps
static double scc_frame_seconds(uint64_t frame) { return ((double)frame * 1001.0) / 30000.0; }

////////////////////////////////////////////////////////////////////////////////
// bounded scanning helpers - nothing here reads past end, no NUL required

// returns the content size of the line starting at cur (terminator excluded),
// and the distance to the next line in advance (terminator included)
static size_t scc_line(const utf8_char_t* cur, const utf8_char_t* end, size_t* advance)
{
    const utf8_char_t* p = cur;

    while (p < end && '\n' != p[0] && '\r' != p[0]) {
        ++p;
    }

    size_t size = (size_t)(p - cur);
    (*advance) = size;

    if (p < end) {
        // accept LF, CRLF, lone CR and LFCR terminators
        (*advance) += (p + 1 < end && (('\r' == p[0] && '\n' == p[1]) || ('\n' == p[0] && '\r' == p[1]))) ? 2 : 1;
    }

    return size;
}

static int scc_is_sep(utf8_char_t c) { return ' ' == c || '\t' == c; }
static int scc_is_dec(utf8_char_t c) { return '0' <= c && c <= '9'; }
static int scc_is_hex(utf8_char_t c) { return scc_is_dec(c) || ('a' <= c && c <= 'f') || ('A' <= c && c <= 'F'); }

static unsigned int scc_hex_value(utf8_char_t c)
{
    if (scc_is_dec(c)) {
        return (unsigned int)(c - '0');
    }

    return (unsigned int)(('a' <= c && c <= 'f') ? (c - 'a' + 10) : (c - 'A' + 10));
}

// parses "hh:mm:ss:ff" (non-drop) or "hh:mm:ss;ff" (drop-frame) at the start
// of a line and converts it to an absolute integer transmission frame
static int scc_timecode_frame(const utf8_char_t* p, size_t size, int* drop, uint64_t* frame)
{
    if (11 > size || !scc_is_dec(p[0]) || !scc_is_dec(p[1]) || ':' != p[2] || !scc_is_dec(p[3]) || !scc_is_dec(p[4]) || ':' != p[5] || !scc_is_dec(p[6]) || !scc_is_dec(p[7]) || (':' != p[8] && ';' != p[8]) || !scc_is_dec(p[9]) || !scc_is_dec(p[10])) {
        return 0;
    }

    int hh = 10 * (p[0] - '0') + (p[1] - '0');
    int mm = 10 * (p[3] - '0') + (p[4] - '0');
    int ss = 10 * (p[6] - '0') + (p[7] - '0');
    int ff = 10 * (p[9] - '0') + (p[10] - '0');

    if (23 < hh || 59 < mm || 59 < ss || 29 < ff) {
        return 0;
    }

    (*drop) = (';' == p[8]);

    // labels 00 and 01 do not exist at second 0 of minutes not divisible by ten
    if ((*drop) && 0 == ss && 0 != (mm % 10) && 2 > ff) {
        return 0;
    }

    uint64_t minutes = (uint64_t)hh * 60 + (uint64_t)mm;
    // hh/mm/ss/ff are range checked above, so this cannot overflow
    uint64_t base = ((uint64_t)hh * 3600 + (uint64_t)mm * 60 + (uint64_t)ss) * 30 + (uint64_t)ff;

    if ((*drop)) {
        uint64_t dropped = 2 * (minutes - minutes / 10);

        if (dropped > base) {
            return 0;
        }

        base -= dropped;
    }

    (*frame) = base;
    return 1;
}

////////////////////////////////////////////////////////////////////////////////
// the visible screen is the character, position, style and underline of every
// front buffer cell; cell data is NUL terminated within 5 bytes, so a bounded
// string compare ignores bytes left over from earlier, longer characters
static int scc_screen_changed(const caption_frame_buffer_t* a, const caption_frame_buffer_t* b)
{
    for (int r = 0; r < SCREEN_ROWS; ++r) {
        for (int c = 0; c < SCREEN_COLS; ++c) {
            const caption_frame_cell_t* x = &a->cell[r][c];
            const caption_frame_cell_t* y = &b->cell[r][c];

            if (x->uln != y->uln || x->sty != y->sty || 0 != strncmp(x->data, y->data, sizeof(x->data))) {
                return 1;
            }
        }
    }

    return 0;
}

// appends the intersection of [start, end) with the window to vtt as a cue;
// empty intersections and textless screens are skipped. returns 0 on
// allocation failure
static int scc_emit_interval(vtt_t* vtt, const caption_frame_buffer_t* front, uint64_t start, uint64_t end, uint64_t begin_frame, uint64_t end_frame)
{
    start = (start < begin_frame) ? begin_frame : start;
    end = (end > end_frame) ? end_frame : end;

    if (start >= end) {
        return 1;
    }

    caption_frame_t view;
    memset(&view, 0, sizeof(caption_frame_t));
    memcpy(&view.front, front, sizeof(caption_frame_buffer_t));

    utf8_char_t text[CAPTION_FRAME_TEXT_BYTES + 3];
    size_t size = caption_frame_to_text(&view, text);

    if (0 == size) {
        return 1;
    }

    // cue payload convention of vtt_cue_from_caption_frame
    memcpy(text + size, "\r\n", 3);
    size += 2;

    vtt_block_t* cue = vtt_block_new(vtt, text, size, VTT_CUE);

    if (!cue) {
        return 0;
    }

    cue->timestamp = scc_frame_seconds(start);
    cue->duration = scc_frame_seconds(end) - cue->timestamp;
    return 1;
}

////////////////////////////////////////////////////////////////////////////////
vtt_t* scc_timeline_to_vtt(const utf8_char_t* data, size_t size, uint32_t begin_frame, uint32_t end_frame, scc_timeline_error_t* error, size_t* error_line)
{
    scc_timeline_error_t failure = SCC_TIMELINE_OK;
    size_t failure_line = 0;
    vtt_t* vtt = 0;

    if (0 == data || begin_frame >= end_frame) {
        failure = SCC_TIMELINE_ERROR_ARGUMENT;
        goto failed;
    }

    vtt = vtt_new();

    if (!vtt) {
        failure = SCC_TIMELINE_ERROR_MEMORY;
        goto failed;
    }

    caption_frame_t frame;
    caption_frame_init(&frame);
    caption_frame_buffer_t shown;
    memset(&shown, 0, sizeof(caption_frame_buffer_t));
    uint64_t shown_since = 0;

    const utf8_char_t* cur = data;
    const utf8_char_t* end = data + size;
    size_t line_number = 1;
    size_t advance = 0;

    // the header must be the first line of the file
    if (SCC_TIMELINE_HEADER_SIZE != scc_line(cur, end, &advance) || 0 != memcmp(cur, SCC_TIMELINE_HEADER, SCC_TIMELINE_HEADER_SIZE)) {
        failure = SCC_TIMELINE_ERROR_HEADER;
        failure_line = line_number;
        goto failed;
    }

    cur += advance, line_number += 1;

    int mode_drop = 0, mode_known = 0, have_previous = 0;
    uint64_t previous_start = 0;
    size_t previous_words = 0;

    while (cur < end) {
        size_t line_size = scc_line(cur, end, &advance);
        const utf8_char_t* line = cur;
        cur += advance;

        // skip blank lines (empty, or only spaces and tabs)
        size_t pos = 0;

        while (pos < line_size && scc_is_sep(line[pos])) {
            ++pos;
        }

        if (pos == line_size) {
            line_number += 1;
            continue;
        }

        int drop = 0;
        uint64_t start = 0;

        if (!scc_timecode_frame(line, line_size, &drop, &start)) {
            failure = SCC_TIMELINE_ERROR_TIMECODE;
            failure_line = line_number;
            goto failed;
        }

        if (!mode_known) {
            mode_drop = drop, mode_known = 1;
        } else if (mode_drop != drop) {
            failure = SCC_TIMELINE_ERROR_TIMECODE;
            failure_line = line_number;
            goto failed;
        }

        // lines may abut, but must not overlap or move backwards. start and
        // words are bounded by the input size, so the sum cannot overflow
        if (have_previous && start < previous_start + previous_words) {
            failure = SCC_TIMELINE_ERROR_ORDER;
            failure_line = line_number;
            goto failed;
        }

        size_t words = 0;
        pos = 11;

        for (;;) {
            while (pos < line_size && scc_is_sep(line[pos])) {
                ++pos;
            }

            if (pos == line_size) {
                break;
            }

            if (4 > line_size - pos || !scc_is_hex(line[pos]) || !scc_is_hex(line[pos + 1]) || !scc_is_hex(line[pos + 2]) || !scc_is_hex(line[pos + 3]) || (pos + 4 < line_size && !scc_is_sep(line[pos + 4]))) {
                failure = SCC_TIMELINE_ERROR_WORD;
                failure_line = line_number;
                goto failed;
            }

            uint16_t cc_data = (uint16_t)((scc_hex_value(line[pos]) << 12) | (scc_hex_value(line[pos + 1]) << 8) | (scc_hex_value(line[pos + 2]) << 4) | scc_hex_value(line[pos + 3]));

            if (!eia608_parity_varify(cc_data)) {
                failure = SCC_TIMELINE_ERROR_PARITY;
                failure_line = line_number;
                goto failed;
            }

            if (words > UINT64_MAX - start) {
                failure = SCC_TIMELINE_ERROR_OVERFLOW;
                failure_line = line_number;
                goto failed;
            }

            uint64_t word_frame = start + words;
            caption_frame_decode(&frame, cc_data, scc_frame_seconds(word_frame));

            if (scc_screen_changed(&frame.front, &shown)) {
                if (!scc_emit_interval(vtt, &shown, shown_since, word_frame, begin_frame, end_frame)) {
                    failure = SCC_TIMELINE_ERROR_MEMORY;
                    failure_line = line_number;
                    goto failed;
                }

                memcpy(&shown, &frame.front, sizeof(caption_frame_buffer_t));
                shown_since = word_frame;
            }

            words += 1, pos += 4;
        }

        if (0 == words) {
            // a timecode with no control words is a truncated line
            failure = SCC_TIMELINE_ERROR_WORD;
            failure_line = line_number;
            goto failed;
        }

        previous_start = start, previous_words = words, have_previous = 1;
        line_number += 1;
    }

    // the screen keeps its last state through the end of the window
    if (!scc_emit_interval(vtt, &shown, shown_since, end_frame, begin_frame, end_frame)) {
        failure = SCC_TIMELINE_ERROR_MEMORY;
        failure_line = line_number;
        goto failed;
    }

    if (error) {
        (*error) = SCC_TIMELINE_OK;
    }

    if (error_line) {
        (*error_line) = 0;
    }

    return vtt;

failed:
    if (vtt) {
        vtt_free(vtt);
    }

    if (error) {
        (*error) = failure;
    }

    if (error_line) {
        (*error_line) = failure_line;
    }

    return 0;
}
