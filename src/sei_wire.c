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
#include "sei_wire.h"
#include <stdlib.h>
#include <string.h>

#define SEI_NAL_TYPE 6
#define SEI_STOP_BIT 0x80

/*
 * Remove H.264 emulation prevention bytes from an escaped byte string.
 * A 0x03 is dropped only when it directly follows two 0x00 bytes, exactly the
 * condition under which the encoder is required to insert it.
 */
static int rbsp_unescape(const uint8_t* src, size_t size, uint8_t** out, size_t* out_size)
{
    uint8_t* dst;
    size_t i = 0, n = 0;

    if (size > (SIZE_MAX / 2) - 1) {
        return -1;
    }

    dst = (uint8_t*)malloc(size + 1);

    if (!dst) {
        return -1;
    }

    while (i < size) {
        if (i + 2 < size && 0x00 == src[i] && 0x00 == src[i + 1] && 0x03 == src[i + 2]) {
            dst[n++] = 0x00;
            dst[n++] = 0x00;
            i += 3;
        } else {
            dst[n++] = src[i++];
        }
    }

    (*out) = dst;
    (*out_size) = n;
    return 0;
}

/*
 * Escape a complete RBSP byte string.  The zero counter is deliberately not
 * reset between messages; every byte of the RBSP, header or payload, takes
 * part in the 00 00 -> 00 00 03 rule.
 */
static int rbsp_escape(const uint8_t* src, size_t size, uint8_t** out, size_t* out_size)
{
    uint8_t* dst;
    size_t i, n = 0, zeros = 0;

    if (size > (SIZE_MAX / 3) * 2 - 1) {
        return -1;
    }

    dst = (uint8_t*)malloc(size + size / 2 + 2);

    if (!dst) {
        return -1;
    }

    for (i = 0; i < size; ++i) {
        uint8_t b = src[i];

        if (zeros >= 2 && b <= 0x03) {
            dst[n++] = 0x03;
            zeros = 0;
        }

        dst[n++] = b;

        if (0x00 == b) {
            ++zeros;
        } else {
            zeros = 0;
        }
    }

    (*out) = dst;
    (*out_size) = n;
    return 0;
}

/* Remove every message from a chain, leaving it initialized but empty. */
static void sei_clear(sei_t* sei)
{
    while (sei->head) {
        sei_message_t* next = sei->head->next;
        free(sei->head);
        sei->head = next;
    }

    sei->head = NULL;
    sei->tail = NULL;
}

int caption_sei_decode(sei_t* out, const uint8_t* nal, size_t size)
{
    uint8_t* rbsp = NULL;
    size_t rbsp_size = 0, pos = 0;
    sei_t parsed;

    if (!out || !nal || size < 2 || SEI_NAL_TYPE != (nal[0] & 0x1F)) {
        return -1;
    }

    if (rbsp_unescape(nal + 1, size - 1, &rbsp, &rbsp_size)) {
        return -1;
    }

    sei_init(&parsed, 0);

    while (pos < rbsp_size) {
        size_t payload_type = 0, payload_size = 0;
        sei_message_t* msg;

        /* A lone trailing byte carries the rbsp_stop_one_bit. */
        if (1 == rbsp_size - pos) {
            break;
        }

        while (pos < rbsp_size && 0xFF == rbsp[pos]) {
            payload_type += 255;
            ++pos;
        }

        if (pos >= rbsp_size) {
            goto error;
        }

        payload_type += rbsp[pos++];

        while (pos < rbsp_size && 0xFF == rbsp[pos]) {
            payload_size += 255;
            ++pos;
        }

        if (pos >= rbsp_size) {
            goto error;
        }

        payload_size += rbsp[pos++];

        if (payload_size > rbsp_size - pos) {
            goto error;
        }

        msg = sei_message_new((sei_msgtype_t)payload_type, payload_size ? rbsp + pos : NULL, payload_size);

        if (!msg) {
            goto error;
        }

        sei_message_append(&parsed, msg);
        pos += payload_size;
    }

    free(rbsp);

    /* Success: hand the independently allocated chain to the caller. */
    out->head = parsed.head;
    out->tail = parsed.tail;
    out->timestamp = 0;
    return 0;

error:
    free(rbsp);
    sei_clear(&parsed);
    return -1;
}

int caption_sei_encode(const sei_t* in, uint8_t** out, size_t* size)
{
    uint8_t* rbsp = NULL;
    uint8_t* escaped = NULL;
    size_t rbsp_size = 0, escaped_size = 0, need = 1 + 1; /* NAL header + stop bit */
    const sei_message_t* msg;
    size_t pos = 0;

    if (out) {
        *out = NULL;
    }

    if (size) {
        *size = 0;
    }

    if (!out || !size) {
        return -1;
    }

    if (!in) {
        return -1;
    }

    for (msg = in->head; msg; msg = msg->next) {
        if (need > SIZE_MAX - msg->size - (msg->type / 255) - (msg->size / 255) - 4) {
            return -1;
        }

        need += (msg->type / 255) + 1 + (msg->size / 255) + 1 + msg->size;
    }

    rbsp = (uint8_t*)malloc(need);

    if (!rbsp) {
        return -1;
    }

    for (msg = in->head; msg; msg = msg->next) {
        size_t payload_type = msg->type;
        size_t payload_size = msg->size;

        while (payload_type >= 255) {
            rbsp[pos++] = 0xFF;
            payload_type -= 255;
        }

        rbsp[pos++] = (uint8_t)payload_type;

        while (payload_size >= 255) {
            rbsp[pos++] = 0xFF;
            payload_size -= 255;
        }

        rbsp[pos++] = (uint8_t)payload_size;

        if (msg->size) {
            memcpy(rbsp + pos, sei_message_data((sei_message_t*)msg), msg->size);
            pos += msg->size;
        }
    }

    rbsp[pos++] = SEI_STOP_BIT;
    rbsp_size = pos;

    if (rbsp_escape(rbsp, rbsp_size, &escaped, &escaped_size)) {
        free(rbsp);
        return -1;
    }

    free(rbsp);

    (*out) = (uint8_t*)malloc(escaped_size + 1);

    if (!(*out)) {
        free(escaped);
        return -1;
    }

    (*out)[0] = SEI_NAL_TYPE;
    memcpy((*out) + 1, escaped, escaped_size);
    (*size) = escaped_size + 1;
    free(escaped);
    return 0;
}
