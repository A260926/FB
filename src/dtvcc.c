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
#include "dtvcc.h"
#include "eia608_charmap.h"
#include <stdlib.h>
#include <string.h>

// A DTVCC packet holds at most 64 * 2 - 1 payload bytes
#define DTVCC_MAX_PACKET_SIZE (64 * 2 - 1)

// C1 commands
enum {
    DTVCC_CLW = 0x88,
    DTVCC_DSW = 0x89,
    DTVCC_HDW = 0x8A,
    DTVCC_TGW = 0x8B,
    DTVCC_DLW = 0x8C,
    DTVCC_RST = 0x8F,
    DTVCC_SPL = 0x92,
    DTVCC_DF0 = 0x98,
};

typedef struct {
    utf8_char_t data[5]; //< single utf8 character plus null terminator
} dtvcc_cell_t;

typedef struct {
    int defined;
    int visible;
    int priority;
    int row_lock;
    int col_lock;
    int relative_pos;
    int anchor_vertical;
    int anchor_horizontal;
    int anchor_point;
    int rows;
    int cols;
    int pen_row;
    int pen_col;
    int window_style;
    int pen_style;
    dtvcc_cell_t* cells; //< rows * cols cells when defined
} dtvcc_window_t;

typedef struct {
    int current_window;
    dtvcc_window_t window[DTVCC_WINDOW_COUNT];
} dtvcc_service_t;

struct dtvcc_decoder_t {
    dtvcc_service_t* service[DTVCC_SERVICE_COUNT]; //< lazily allocated, service 0 is padding only
    uint8_t packet[DTVCC_MAX_PACKET_SIZE];
    size_t packet_have; //< payload bytes assembled so far
    size_t packet_need; //< payload bytes required to complete the open packet
    int packet_open; //< a packet is being assembled
    int expect_seq; //< next expected packet sequence number, -1 = no packet seen yet
    double timestamp; //< timestamp of the most recent consumed cc_data
};

////////////////////////////////////////////////////////////////////////////////
// windows

static void dtvcc_window_undefine(dtvcc_window_t* window)
{
    free(window->cells);
    memset(window, 0, sizeof(dtvcc_window_t));
}

static void dtvcc_window_clear_text(dtvcc_window_t* window)
{
    memset(window->cells, 0, (size_t)window->rows * window->cols * sizeof(dtvcc_cell_t));
    window->pen_row = 0;
    window->pen_col = 0;
}

// p points at the 5 DefineWindow parameter bytes
static void dtvcc_window_define(dtvcc_window_t* window, const uint8_t* p)
{
    int rows = (p[2] & 0x0F) + 1;
    int cols = (p[3] & 0x3F) + 1;

    if (window->defined && (window->rows != rows || window->cols != cols)) {
        // size changed, keep the intersection and clip what falls outside
        dtvcc_cell_t* cells = (dtvcc_cell_t*)calloc((size_t)rows * cols, sizeof(dtvcc_cell_t));
        int max_row = rows < window->rows ? rows : window->rows;
        int max_col = cols < window->cols ? cols : window->cols;

        for (int row = 0; row < max_row; ++row) {
            for (int col = 0; col < max_col; ++col) {
                cells[row * cols + col] = window->cells[row * window->cols + col];
            }
        }

        free(window->cells);
        window->cells = cells;
    } else if (!window->defined) {
        window->cells = (dtvcc_cell_t*)calloc((size_t)rows * cols, sizeof(dtvcc_cell_t));
        window->pen_row = 0;
        window->pen_col = 0;
    }
    // redefining a window with the same size keeps its text untouched

    window->defined = 1;
    window->visible = !!(p[0] & 0x20);
    window->row_lock = !!(p[0] & 0x10);
    window->col_lock = !!(p[0] & 0x08);
    window->priority = p[0] & 0x07;
    window->relative_pos = !!(p[1] & 0x40);
    window->anchor_vertical = (p[1] >> 4) & 0x03;
    window->anchor_horizontal = p[1] & 0x0F;
    window->anchor_point = (p[2] >> 4) & 0x0F;
    window->window_style = (p[4] >> 3) & 0x07;
    window->pen_style = p[4] & 0x07;
    window->rows = rows;
    window->cols = cols;

    if (window->pen_row >= rows) {
        window->pen_row = rows - 1;
    }

    if (window->pen_col >= cols) {
        window->pen_col = cols - 1;
    }
}

static void dtvcc_window_write(dtvcc_window_t* window, const utf8_char_t* c)
{
    if (!window->defined) {
        return;
    }

    // writes past the locked bounds are clipped, they never reach adjacent windows
    if (0 <= window->pen_row && window->pen_row < window->rows && 0 <= window->pen_col && window->pen_col < window->cols) {
        utf8_char_copy(window->cells[window->pen_row * window->cols + window->pen_col].data, c);
    }

    window->pen_col += 1;
}

static void dtvcc_window_backspace(dtvcc_window_t* window)
{
    if (window->defined && 0 < window->pen_col) {
        window->pen_col -= 1;
    }
}

static void dtvcc_window_formfeed(dtvcc_window_t* window)
{
    if (window->defined) {
        dtvcc_window_clear_text(window);
    }
}

static void dtvcc_window_carriage_return(dtvcc_window_t* window)
{
    if (!window->defined) {
        return;
    }

    window->pen_col = 0;

    if (window->pen_row < window->rows - 1) {
        window->pen_row += 1;
    } else {
        // at the bottom, scroll this window only
        size_t row_size = (size_t)window->cols * sizeof(dtvcc_cell_t);
        memmove(&window->cells[0], &window->cells[window->cols], (size_t)(window->rows - 1) * row_size);
        memset(&window->cells[(window->rows - 1) * window->cols], 0, row_size);
    }
}

////////////////////////////////////////////////////////////////////////////////
// services

static void dtvcc_service_free(dtvcc_service_t* service)
{
    if (service) {
        for (int i = 0; i < DTVCC_WINDOW_COUNT; ++i) {
            free(service->window[i].cells);
        }

        free(service);
    }
}

static dtvcc_service_t* dtvcc_service_clone(const dtvcc_service_t* service)
{
    if (!service) {
        return 0;
    }

    dtvcc_service_t* clone = (dtvcc_service_t*)malloc(sizeof(dtvcc_service_t));
    (*clone) = (*service);

    for (int i = 0; i < DTVCC_WINDOW_COUNT; ++i) {
        dtvcc_window_t* window = &clone->window[i];

        if (window->defined && window->cells) {
            size_t size = (size_t)window->rows * window->cols * sizeof(dtvcc_cell_t);
            window->cells = (dtvcc_cell_t*)malloc(size);
            memcpy(window->cells, service->window[i].cells, size);
        } else {
            window->cells = 0;
        }
    }

    return clone;
}

static dtvcc_service_t* dtvcc_decoder_service(dtvcc_decoder_t* decoder, int number, int create)
{
    if (0 >= number || DTVCC_SERVICE_COUNT <= number) {
        return 0;
    }

    if (!decoder->service[number] && create) {
        decoder->service[number] = (dtvcc_service_t*)calloc(1, sizeof(dtvcc_service_t));
    }

    return decoder->service[number];
}

static void dtvcc_decoder_clear_services(dtvcc_decoder_t* decoder)
{
    for (int i = 0; i < DTVCC_SERVICE_COUNT; ++i) {
        dtvcc_service_free(decoder->service[i]);
        decoder->service[i] = 0;
    }
}

// CLW, DSW, HDW, TGW and DLW carry a window bitmap, bit i addresses window i
static void dtvcc_service_windows_mask(dtvcc_service_t* service, uint8_t command, uint8_t mask)
{
    for (int i = 0; i < DTVCC_WINDOW_COUNT; ++i) {
        if (!(mask & (1 << i))) {
            continue;
        }

        dtvcc_window_t* window = &service->window[i];

        switch (command) {
        case DTVCC_CLW: // clear text, keep definition and visibility
            if (window->defined) {
                dtvcc_window_clear_text(window);
            }
            break;
        case DTVCC_DSW:
            if (window->defined) {
                window->visible = 1;
            }
            break;
        case DTVCC_HDW: // hidden windows keep their text
            if (window->defined) {
                window->visible = 0;
            }
            break;
        case DTVCC_TGW:
            if (window->defined) {
                window->visible = !window->visible;
            }
            break;
        case DTVCC_DLW: // revoke the definition
            dtvcc_window_undefine(window);
            break;
        }
    }
}

static void dtvcc_service_reset(dtvcc_service_t* service)
{
    for (int i = 0; i < DTVCC_WINDOW_COUNT; ++i) {
        dtvcc_window_undefine(&service->window[i]);
    }

    service->current_window = 0;
}

////////////////////////////////////////////////////////////////////////////////
// packet decoding

static dtvcc_status_t dtvcc_decode_block(dtvcc_service_t* service, const uint8_t* data, size_t size)
{
    size_t pos = 0;

    while (pos < size) {
        uint8_t c = data[pos];

        if (0x20 > c) { // C0
            dtvcc_window_t* window = &service->window[service->current_window];

            switch (c) {
            case 0x00: // NUL
                break;
            case 0x08: // BS
                dtvcc_window_backspace(window);
                break;
            case 0x0C: // FF
                dtvcc_window_formfeed(window);
                break;
            case 0x0D: // CR
                dtvcc_window_carriage_return(window);
                break;
            default:
                return DTVCC_STATUS_UNSUPPORTED;
            }

            pos += 1;
        } else if (0x80 > c) { // G0 printable, Basic North American character set
            dtvcc_window_write(&service->window[service->current_window], eia608_char_map[c - 0x20]);
            pos += 1;
        } else if (0xA0 > c) { // C1
            if (0x88 > c) { // CW0-CW7
                service->current_window = c & 0x07;
                pos += 1;
            } else if (DTVCC_CLW <= c && DTVCC_DLW >= c) {
                if (pos + 2 > size) {
                    return DTVCC_STATUS_ERROR; // truncated window bitmap
                }

                dtvcc_service_windows_mask(service, c, data[pos + 1]);
                pos += 2;
            } else if (DTVCC_RST == c) {
                dtvcc_service_reset(service);
                pos += 1;
            } else if (DTVCC_SPL == c) {
                if (pos + 3 > size) {
                    return DTVCC_STATUS_ERROR; // truncated pen location
                }

                dtvcc_window_t* window = &service->window[service->current_window];

                if (window->defined) {
                    int row = data[pos + 1] & 0x0F;
                    int col = data[pos + 2] & 0x3F;
                    window->pen_row = row < window->rows ? row : window->rows - 1;
                    window->pen_col = col < window->cols ? col : window->cols - 1;
                }

                pos += 3;
            } else if (DTVCC_DF0 <= c) { // DF0-DF7
                if (pos + 6 > size) {
                    return DTVCC_STATUS_ERROR; // truncated window definition
                }

                dtvcc_window_define(&service->window[c - DTVCC_DF0], &data[pos + 1]);
                pos += 6;
            } else {
                return DTVCC_STATUS_UNSUPPORTED; // SPA, SPC, SWA, DLY, DLC, ...
            }
        } else {
            return DTVCC_STATUS_UNSUPPORTED; // G1 and beyond
        }
    }

    return DTVCC_STATUS_OK;
}

static dtvcc_status_t dtvcc_decode_packet(dtvcc_decoder_t* decoder, const uint8_t* data, size_t size)
{
    // checkpoint, a packet that fails to decode must not publish half built windows
    dtvcc_service_t* checkpoint[DTVCC_SERVICE_COUNT];

    for (int i = 0; i < DTVCC_SERVICE_COUNT; ++i) {
        checkpoint[i] = dtvcc_service_clone(decoder->service[i]);
    }

    dtvcc_status_t status = DTVCC_STATUS_OK;
    size_t pos = 0;

    while (pos < size && DTVCC_STATUS_OK == status) {
        uint8_t header = data[pos++];
        int service_number = (header >> 5) & 0x07;
        int block_size = header & 0x1F;

        if (0 == service_number) {
            break; // service 0 is padding, the rest of the packet is stuffing
        }

        if (7 == service_number) { // extended service number
            if (pos >= size) {
                status = DTVCC_STATUS_ERROR;
                break;
            }

            service_number = data[pos++] & 0x3F;

            if (7 > service_number) {
                status = DTVCC_STATUS_ERROR; // extended numbers start at 7
                break;
            }
        }

        if (pos + block_size > size) {
            status = DTVCC_STATUS_ERROR; // block reaches past the end of the packet
            break;
        }

        dtvcc_service_t* service = dtvcc_decoder_service(decoder, service_number, 1);

        if (!service) {
            status = DTVCC_STATUS_ERROR;
            break;
        }

        // the block length constrains this block only, the next service starts after it
        status = dtvcc_decode_block(service, &data[pos], block_size);
        pos += block_size;
    }

    if (DTVCC_STATUS_OK == status) {
        for (int i = 0; i < DTVCC_SERVICE_COUNT; ++i) {
            dtvcc_service_free(checkpoint[i]);
        }
    } else {
        for (int i = 0; i < DTVCC_SERVICE_COUNT; ++i) {
            dtvcc_service_free(decoder->service[i]);
            decoder->service[i] = checkpoint[i];
        }
    }

    return status;
}

////////////////////////////////////////////////////////////////////////////////
// packet assembly

static dtvcc_status_t dtvcc_assemble_start(dtvcc_decoder_t* decoder, uint8_t cc_data1, uint8_t cc_data2)
{
    if (decoder->packet_open) {
        // a new start interrupted an unfinished packet, that is a packet loss
        dtvcc_decoder_clear_services(decoder);
        decoder->packet_open = 0;
        decoder->expect_seq = -1;
    }

    int seq = (cc_data1 >> 6) & 0x03;
    int size_code = cc_data1 & 0x3F;
    size_t packet_size = (0 == size_code) ? 64 : (size_t)size_code;

    if (0 <= decoder->expect_seq && seq != decoder->expect_seq) {
        // sequence number jump, packets were lost
        dtvcc_decoder_clear_services(decoder);
    }

    decoder->expect_seq = (seq + 1) & 0x03;
    decoder->packet_need = packet_size * 2 - 1;
    decoder->packet_have = 0;
    decoder->packet_open = 1;
    decoder->packet[decoder->packet_have++] = cc_data2;

    if (decoder->packet_have >= decoder->packet_need) {
        decoder->packet_open = 0;
        return dtvcc_decode_packet(decoder, decoder->packet, decoder->packet_need);
    }

    return DTVCC_STATUS_OK;
}

static dtvcc_status_t dtvcc_assemble_data(dtvcc_decoder_t* decoder, uint8_t cc_data1, uint8_t cc_data2)
{
    if (!decoder->packet_open) {
        return DTVCC_STATUS_OK; // continuation without a start, nothing to append to
    }

    // the final pair of an odd sized packet carries one padding byte
    if (decoder->packet_have < decoder->packet_need) {
        decoder->packet[decoder->packet_have++] = cc_data1;
    }

    if (decoder->packet_have < decoder->packet_need) {
        decoder->packet[decoder->packet_have++] = cc_data2;
    }

    if (decoder->packet_have >= decoder->packet_need) {
        decoder->packet_open = 0;
        return dtvcc_decode_packet(decoder, decoder->packet, decoder->packet_need);
    }

    return DTVCC_STATUS_OK;
}

////////////////////////////////////////////////////////////////////////////////
// public interface

dtvcc_decoder_t* dtvcc_decoder_create(void)
{
    dtvcc_decoder_t* decoder = (dtvcc_decoder_t*)calloc(1, sizeof(dtvcc_decoder_t));

    if (decoder) {
        decoder->expect_seq = -1;
    }

    return decoder;
}

void dtvcc_decoder_free(dtvcc_decoder_t* decoder)
{
    if (decoder) {
        dtvcc_decoder_clear_services(decoder);
        free(decoder);
    }
}

void dtvcc_decoder_reset(dtvcc_decoder_t* decoder)
{
    if (decoder) {
        dtvcc_decoder_clear_services(decoder);
        decoder->packet_have = 0;
        decoder->packet_need = 0;
        decoder->packet_open = 0;
        decoder->expect_seq = -1;
        decoder->timestamp = 0;
    }
}

dtvcc_status_t dtvcc_decoder_decode(dtvcc_decoder_t* decoder, const cea708_t* cea708)
{
    if (!decoder || !cea708) {
        return DTVCC_STATUS_ERROR;
    }

    // native DTVCC is only carried in GA94 user data with process_cc_data_flag set,
    // 608 entries are left for caption_frame_decode
    if (GA94 != cea708->user_identifier || !cea708->user_data.process_cc_data_flag) {
        return DTVCC_STATUS_OK;
    }

    dtvcc_status_t status = DTVCC_STATUS_OK;
    int count = cea708->user_data.cc_count;

    if (count > 32) {
        count = 32;
    }

    for (int i = 0; i < count && DTVCC_STATUS_OK == status; ++i) {
        const cc_data_t* cc = &cea708->user_data.cc_data[i];

        if (!cc->cc_valid) {
            continue;
        }

        decoder->timestamp = cea708->timestamp;

        if (cc_type_dtvcc_packet_start == (cea708_cc_type_t)cc->cc_type) {
            status = dtvcc_assemble_start(decoder, (cc->cc_data >> 8) & 0xFF, cc->cc_data & 0xFF);
        } else if (cc_type_dtvcc_packet_data == (cea708_cc_type_t)cc->cc_type) {
            status = dtvcc_assemble_data(decoder, (cc->cc_data >> 8) & 0xFF, cc->cc_data & 0xFF);
        }
    }

    return status;
}

int dtvcc_decoder_window_snapshot(const dtvcc_decoder_t* decoder, int service, int window_number, dtvcc_window_snapshot_t* snapshot)
{
    if (!decoder || !snapshot || 0 >= service || DTVCC_SERVICE_COUNT <= service || 0 > window_number || DTVCC_WINDOW_COUNT <= window_number) {
        return 0;
    }

    memset(snapshot, 0, sizeof(dtvcc_window_snapshot_t));
    snapshot->service = service;
    snapshot->window = window_number;

    const dtvcc_service_t* svc = decoder->service[service];

    if (!svc) {
        return 0;
    }

    const dtvcc_window_t* window = &svc->window[window_number];

    if (!window->defined) {
        return 0;
    }

    snapshot->defined = 1;
    snapshot->visible = window->visible;
    snapshot->priority = window->priority;
    snapshot->row_lock = window->row_lock;
    snapshot->col_lock = window->col_lock;
    snapshot->relative_pos = window->relative_pos;
    snapshot->anchor_vertical = window->anchor_vertical;
    snapshot->anchor_horizontal = window->anchor_horizontal;
    snapshot->anchor_point = window->anchor_point;
    snapshot->row_count = window->rows;
    snapshot->col_count = window->cols;
    snapshot->pen_row = window->pen_row;
    snapshot->pen_col = window->pen_col;
    snapshot->window_style = window->window_style;
    snapshot->pen_style = window->pen_style;
    snapshot->timestamp = decoder->timestamp;

    for (int row = 0; row < window->rows; ++row) {
        utf8_char_t* text = snapshot->text[row];
        size_t bytes = 0;

        for (int col = 0; col < window->cols; ++col) {
            const dtvcc_cell_t* cell = &window->cells[row * window->cols + col];
            const utf8_char_t* c = cell->data[0] ? cell->data : " ";
            size_t length = strlen(c);
            memcpy(&text[bytes], c, length);
            bytes += length;
        }

        text[bytes] = 0;
        text[utf8_trimmed_length(text, utf8_char_count(text, 0))] = 0;
    }

    return 1;
}
