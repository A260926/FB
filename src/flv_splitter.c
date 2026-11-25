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
#include "flv_splitter.h"
#include "eia608.h"
#include "mpeg.h"
#include "sei_wire.h"
#include <stdlib.h>
#include <string.h>

#define FLV_HEADER_SIZE 13
#define FLV_TAG_HEADER_SIZE 11
#define FLV_TAG_FOOTER_SIZE 4

#define FLV_TAG_AUDIO 0x08
#define FLV_TAG_VIDEO 0x09
#define FLV_TAG_SCRIPT 0x12

#define AVC_CODEC_ID 7
#define AVC_PACKET_SEQUENCE_HEADER 0
#define AVC_PACKET_NALU 1

#define NAL_TYPE_SEI 6

#define SERVICE_COUNT 4

/* ------------------------------------------------------------------------- */
/* small growable byte buffer                                                */

typedef struct {
    uint8_t* data;
    size_t size;
    size_t capacity;
} byte_buffer_t;

static void buffer_init(byte_buffer_t* b)
{
    b->data = NULL;
    b->size = 0;
    b->capacity = 0;
}

static void buffer_free(byte_buffer_t* b)
{
    free(b->data);
    buffer_init(b);
}

static int buffer_reserve(byte_buffer_t* b, size_t extra)
{
    size_t need, capacity;
    uint8_t* data;

    if (extra > SIZE_MAX - b->size) {
        return 0;
    }

    need = b->size + extra;

    if (need <= b->capacity) {
        return 1;
    }

    capacity = b->capacity ? b->capacity : 64;

    while (capacity < need) {
        if (capacity > SIZE_MAX / 2) {
            capacity = need;
            break;
        }

        capacity *= 2;
    }

    data = (uint8_t*)realloc(b->data, capacity);

    if (!data) {
        return 0;
    }

    b->data = data;
    b->capacity = capacity;
    return 1;
}

static int buffer_append(byte_buffer_t* b, const void* data, size_t size)
{
    if (!size) {
        return 1;
    }

    if (!buffer_reserve(b, size)) {
        return 0;
    }

    memcpy(b->data + b->size, data, size);
    b->size += size;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* big endian helpers                                                        */

static uint32_t read_u24(const uint8_t* p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static uint32_t read_u32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void write_u24(uint8_t* p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 16);
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 0);
}

static void write_u32(uint8_t* p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)(value >> 0);
}

/* ------------------------------------------------------------------------- */
/* splitter state                                                            */

struct caption_flv_splitter_t {
    size_t max_tag;
    caption_flv_emit_fn emit;
    void* opaque;

    int failed;
    int finished;
    int have_header;

    /* One input unit (FLV header or one complete tag) being accumulated. */
    byte_buffer_t unit;

    /* AVC configuration, updated from sequence header tags. */
    int have_config;
    unsigned nal_length_size;

    /* 608 routing; index 0 is field 1, index 1 is field 2. */
    unsigned field_service[2];

    /* XDS packet state for field 2 (persists across tags). */
    int xds_active;

    /* Output buffers awaiting callback delivery, service 1..4 in out[0..3]. */
    int pending;
    unsigned next_service;
    byte_buffer_t out[SERVICE_COUNT];
};

/* ------------------------------------------------------------------------- */
/* output delivery                                                           */

static void splitter_clear_outputs(caption_flv_splitter_t* ctx)
{
    unsigned i;
    for (i = 0; i < SERVICE_COUNT; ++i) {
        buffer_free(&ctx->out[i]);
    }
    ctx->pending = 0;
    ctx->next_service = 1;
}

static void splitter_fail(caption_flv_splitter_t* ctx)
{
    ctx->failed = 1;
    splitter_clear_outputs(ctx);
}

/* returns 0 when all pending output was accepted, 1 when paused, -1 on abort */
static int splitter_drain(caption_flv_splitter_t* ctx)
{
    while (ctx->pending) {
        byte_buffer_t* buffer;
        int result;

        if (ctx->next_service < 1 || ctx->next_service > SERVICE_COUNT) {
            splitter_fail(ctx);
            return -1;
        }

        buffer = &ctx->out[ctx->next_service - 1];
        result = ctx->emit(ctx->next_service, buffer->data, buffer->size, ctx->opaque);

        if (result < 0) {
            splitter_fail(ctx);
            return -1;
        }

        if (result > 0) {
            return 1;
        }

        buffer_free(buffer);
        ++ctx->next_service;

        if (ctx->next_service > SERVICE_COUNT) {
            ctx->pending = 0;
            ctx->next_service = 1;
        }
    }

    return 0;
}

/* Mark the four prepared output buffers as pending delivery. */
static void splitter_schedule(caption_flv_splitter_t* ctx)
{
    ctx->pending = 1;
    ctx->next_service = 1;
}

/* ------------------------------------------------------------------------- */
/* 608 routing / XDS classification                                          */

/* A 608 word that carries caption formatting or control, not text. */
static int word_is_caption_structure(uint16_t word)
{
    return eia608_is_preamble(word) || eia608_is_midrowchange(word) || eia608_is_specialna(word) || eia608_is_westeu(word) || eia608_is_control(word);
}

/* XDS start / data / checksum words live in field 2 only. */
static int word_is_xds_start(uint16_t word)
{
    return eia608_is_xds(word);
}

static int word_is_xds_checksum(uint16_t word)
{
    return 0x8F00 == (word & 0xFF00);
}

/*
 * Determine which of the four output services keep one cc_data entry, and
 * advance the routing / XDS state exactly once for the original stream.
 *
 * keep[0..3] correspond to services 1..4.  cc_field is 0 for field 1
 * (CC1/CC2) and 1 for field 2 (CC3/CC4), i.e. the cc_type value.
 */
static void classify_cc_entry(caption_flv_splitter_t* ctx, unsigned cc_field, int valid, uint16_t word, int keep[SERVICE_COUNT])
{
    int s;
    int keep_all = 0;

    for (s = 0; s < SERVICE_COUNT; ++s) {
        keep[s] = 0;
    }

    if (!valid || cc_field > 1) {
        /* Invalid entries and native 708 (types 2/3) are never routed. */
        keep_all = 1;
    } else if (!eia608_parity_varify(word)) {
        /* A 608 word is only meaningful with odd parity. */
        keep_all = 1;
    } else if (0x8080 == word) {
        /* Padding is preserved for every service. */
        keep_all = 1;
    } else if (1 == cc_field) {
        /* XDS lives in field 2 only. */
        if (ctx->xds_active) {
            if (word_is_xds_checksum(word)) {
                ctx->xds_active = 0;
                keep_all = 1;
            } else if (word_is_caption_structure(word)) {
                /* Caption formatting/control interrupted the XDS packet; it
                 * then routes as ordinary caption data below. */
                ctx->xds_active = 0;
            } else {
                keep_all = 1; /* XDS payload / continues the packet */
            }
        } else if (word_is_xds_start(word)) {
            ctx->xds_active = 1;
            keep_all = 1;
        }
    }

    if (keep_all) {
        for (s = 0; s < SERVICE_COUNT; ++s) {
            keep[s] = 1;
        }
        return;
    }

    {
        unsigned channel = (word & 0x0800) ? 1u : 0u;
        unsigned* selected = &ctx->field_service[cc_field];

        if (word_is_caption_structure(word)) {
            *selected = (0 == cc_field) ? (channel ? 2u : 1u) : (channel ? 4u : 3u);
        }

        keep[*selected - 1] = 1;
    }
}

/* ------------------------------------------------------------------------- */
/* GA94 user data projection                                                 */

typedef struct {
    unsigned count;
    const uint8_t* entries;
    const uint8_t* tail;
    size_t tail_size;
    uint8_t flags;
    uint8_t em_data;
} ga94_info_t;

static int ga94_parse_message(sei_message_t* msg, ga94_info_t* info)
{
    const uint8_t* data = sei_message_data(msg);
    size_t size = sei_message_size(msg);
    size_t entries_end;

    if (sei_type_user_data_registered_itu_t_t35 != sei_message_type(msg)) {
        return 0;
    }

    if (size < 10) {
        return 0;
    }

    /* country 181, provider 49 (ATSC), user identifier "GA94". */
    if (181 != data[0] || 0x00 != data[1] || 0x31 != data[2]) {
        return 0;
    }

    if ('G' != data[3] || 'A' != data[4] || '9' != data[5] || '4' != data[6]) {
        return 0;
    }

    if (3 != data[7]) {
        return 0;
    }

    if (!(data[8] & 0x40)) {
        return 0;
    }

    info->count = data[8] & 0x1F;
    info->flags = data[8];
    info->em_data = data[9];
    info->entries = data + 10;
    entries_end = 10 + (size_t)info->count * 3;

    if (entries_end > size) {
        return 0;
    }

    info->tail = data + entries_end;
    info->tail_size = size - entries_end;
    return 1;
}

/*
 * Split one GA94 message payload into four filtered payloads, updating the
 * routing state once per original entry.  Output buffers use the input size
 * as an upper bound because entries are only ever removed.
 */
static int project_ga94_payload(caption_flv_splitter_t* ctx, const ga94_info_t* info, byte_buffer_t out[SERVICE_COUNT])
{
    unsigned i, s;
    int keep[SERVICE_COUNT];

    for (s = 0; s < SERVICE_COUNT; ++s) {
        buffer_init(&out[s]);
    }

    for (s = 0; s < SERVICE_COUNT; ++s) {
        if (!buffer_reserve(&out[s], 10 + (size_t)info->count * 3 + info->tail_size)) {
            goto error;
        }
    }

    for (i = 0; i < info->count; ++i) {
        const uint8_t* entry = info->entries + i * 3;
        int valid = (entry[0] >> 2) & 1;
        unsigned cc_type = entry[0] & 3;
        uint16_t word = (uint16_t)(((uint16_t)entry[1] << 8) | entry[2]);

        classify_cc_entry(ctx, cc_type, valid, word, keep);

        for (s = 0; s < SERVICE_COUNT; ++s) {
            if (keep[s]) {
                if (!buffer_append(&out[s], entry, 3)) {
                    goto error;
                }
            }
        }
    }

    for (s = 0; s < SERVICE_COUNT; ++s) {
        unsigned kept = (unsigned)(out[s].size / 3);
        uint8_t header[2];

        /* Copy the first eight bytes (country/provider/GA94/type) verbatim. */
        if (!buffer_reserve(&out[s], 0)) {
            goto error;
        }

        {
            /* Prefix the payload with its original non-count header bytes. */
            byte_buffer_t prefixed;
            buffer_init(&prefixed);

            if (!buffer_append(&prefixed, info->entries - 10, 8)) {
                buffer_free(&prefixed);
                goto error;
            }

            header[0] = (uint8_t)((info->flags & 0xE0) | (kept & 0x1F));
            header[1] = info->em_data;

            if (!buffer_append(&prefixed, header, 2) || !buffer_append(&prefixed, out[s].data, out[s].size) || !buffer_append(&prefixed, info->tail, info->tail_size)) {
                buffer_free(&prefixed);
                goto error;
            }

            buffer_free(&out[s]);
            out[s] = prefixed;
        }
    }

    return 1;

error:
    for (s = 0; s < SERVICE_COUNT; ++s) {
        buffer_free(&out[s]);
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* SEI NAL rewriting                                                         */

static void free_sei_outputs(uint8_t* data[SERVICE_COUNT], size_t size[SERVICE_COUNT])
{
    unsigned s;
    for (s = 0; s < SERVICE_COUNT; ++s) {
        free(data[s]);
        data[s] = NULL;
        size[s] = 0;
    }
}

static void free_sei_chains(sei_t chains[SERVICE_COUNT])
{
    unsigned s;
    for (s = 0; s < SERVICE_COUNT; ++s) {
        sei_free(&chains[s]);
    }
}

/*
 * Rewrite one SEI NAL for all four services.  Returns 1 when rewritten (the
 * individual buffers must be freed by the caller), 0 when the NAL should be
 * copied verbatim (not caption data, undecodable or no projectable message)
 * and -1 on a hard failure.  Routing state advances exactly once per original
 * cc_data entry no matter how many services are produced.
 */
static int rewrite_sei_nal(caption_flv_splitter_t* ctx, const uint8_t* nal, size_t size,
    uint8_t* data[SERVICE_COUNT], size_t out_size[SERVICE_COUNT])
{
    sei_t parsed;
    sei_t chains[SERVICE_COUNT];
    sei_message_t* msg;
    int projectable = 0;
    unsigned s;

    for (s = 0; s < SERVICE_COUNT; ++s) {
        data[s] = NULL;
        out_size[s] = 0;
        sei_init(&chains[s], 0);
    }

    if (size < 2) {
        return 0;
    }

    sei_init(&parsed, 0);

    if (caption_sei_decode(&parsed, nal, size)) {
        sei_free(&parsed);
        return 0;
    }

    for (msg = sei_message_head(&parsed); msg; msg = sei_message_next(msg)) {
        ga94_info_t info;
        if (ga94_parse_message(msg, &info)) {
            projectable = 1;
            break;
        }
    }

    if (!projectable) {
        sei_free(&parsed);
        return 0;
    }

    for (msg = sei_message_head(&parsed); msg; msg = sei_message_next(msg)) {
        ga94_info_t info;

        if (ga94_parse_message(msg, &info)) {
            byte_buffer_t parts[SERVICE_COUNT];

            /* Project once; every service reuses the same filtered payloads. */
            if (!project_ga94_payload(ctx, &info, parts)) {
                sei_free(&parsed);
                free_sei_chains(chains);
                return -1;
            }

            for (s = 0; s < SERVICE_COUNT; ++s) {
                sei_message_t* filtered = sei_message_new(sei_message_type(msg), parts[s].data, parts[s].size);

                if (!filtered) {
                    unsigned k;
                    for (k = 0; k < SERVICE_COUNT; ++k) {
                        buffer_free(&parts[k]);
                    }
                    sei_free(&parsed);
                    free_sei_chains(chains);
                    return -1;
                }

                sei_message_append(&chains[s], filtered);
            }

            for (s = 0; s < SERVICE_COUNT; ++s) {
                buffer_free(&parts[s]);
            }
        } else {
            for (s = 0; s < SERVICE_COUNT; ++s) {
                sei_message_t* copy = sei_message_copy(msg);

                if (!copy) {
                    sei_free(&parsed);
                    free_sei_chains(chains);
                    return -1;
                }

                sei_message_append(&chains[s], copy);
            }
        }
    }

    sei_free(&parsed);

    for (s = 0; s < SERVICE_COUNT; ++s) {
        if (caption_sei_encode(&chains[s], &data[s], &out_size[s])) {
            free_sei_chains(chains);
            free_sei_outputs(data, out_size);
            return -1;
        }
    }

    free_sei_chains(chains);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* FLV tag construction                                                      */

/* Pass one complete, already validated tag through to all four services. */
static int emit_passthrough_tag(caption_flv_splitter_t* ctx, const uint8_t* raw, size_t size)
{
    unsigned s;

    splitter_clear_outputs(ctx);

    for (s = 0; s < SERVICE_COUNT; ++s) {
        if (!buffer_append(&ctx->out[s], raw, size)) {
            splitter_fail(ctx);
            return 0;
        }
    }

    splitter_schedule(ctx);
    return 1;
}

static void release_bodies(byte_buffer_t bodies[SERVICE_COUNT])
{
    unsigned s;
    for (s = 0; s < SERVICE_COUNT; ++s) {
        buffer_free(&bodies[s]);
    }
}

/* Rebuild an AVC NALU tag, projecting GA94 caption data in each SEI NAL. */
static int rebuild_avc_nalu_tag(caption_flv_splitter_t* ctx, const uint8_t* raw, size_t size)
{
    size_t body_size = read_u24(raw + 1);
    const uint8_t* body = raw + FLV_TAG_HEADER_SIZE;
    byte_buffer_t bodies[SERVICE_COUNT];
    size_t pos;
    unsigned s;

    if (body_size < 5 || size != FLV_TAG_HEADER_SIZE + body_size + FLV_TAG_FOOTER_SIZE) {
        splitter_fail(ctx);
        return 0;
    }

    splitter_clear_outputs(ctx);

    for (s = 0; s < SERVICE_COUNT; ++s) {
        buffer_init(&bodies[s]);
        if (!buffer_append(&bodies[s], raw, FLV_TAG_HEADER_SIZE) || !buffer_append(&bodies[s], body, 5)) {
            release_bodies(bodies);
            splitter_fail(ctx);
            return 0;
        }
    }

    pos = 5;

    while (pos < body_size) {
        uint8_t prefix[4];
        size_t nalu_size = 0;
        const uint8_t* nalu;
        unsigned i;
        uint8_t* sei_data[SERVICE_COUNT];
        size_t sei_size[SERVICE_COUNT];
        int rewritten;

        if (body_size - pos < ctx->nal_length_size) {
            release_bodies(bodies);
            splitter_fail(ctx);
            return 0;
        }

        for (i = 0; i < ctx->nal_length_size; ++i) {
            nalu_size = (nalu_size << 8) | body[pos + i];
        }

        pos += ctx->nal_length_size;

        if (nalu_size > body_size - pos) {
            release_bodies(bodies);
            splitter_fail(ctx);
            return 0;
        }

        nalu = body + pos;

        rewritten = 0;

        if (nalu_size >= 2 && NAL_TYPE_SEI == (nalu[0] & 0x1F)) {
            rewritten = rewrite_sei_nal(ctx, nalu, nalu_size, sei_data, sei_size);

            if (rewritten < 0) {
                release_bodies(bodies);
                splitter_fail(ctx);
                return 0;
            }
        }

        if (rewritten) {
            for (s = 0; s < SERVICE_COUNT; ++s) {
                size_t len = sei_size[s];
                unsigned j;

                for (j = 0; j < ctx->nal_length_size; ++j) {
                    prefix[ctx->nal_length_size - 1 - j] = (uint8_t)(len >> (8 * j));
                }

                if (!buffer_append(&bodies[s], prefix, ctx->nal_length_size) || !buffer_append(&bodies[s], sei_data[s], sei_size[s])) {
                    free_sei_outputs(sei_data, sei_size);
                    release_bodies(bodies);
                    splitter_fail(ctx);
                    return 0;
                }
            }

            free_sei_outputs(sei_data, sei_size);
        } else {
            unsigned j;

            for (j = 0; j < ctx->nal_length_size; ++j) {
                prefix[ctx->nal_length_size - 1 - j] = (uint8_t)((nalu_size >> (8 * j)) & 0xFF);
            }

            for (s = 0; s < SERVICE_COUNT; ++s) {
                if (!buffer_append(&bodies[s], prefix, ctx->nal_length_size) || !buffer_append(&bodies[s], nalu, nalu_size)) {
                    release_bodies(bodies);
                    splitter_fail(ctx);
                    return 0;
                }
            }
        }

        pos += nalu_size;
    }

    for (s = 0; s < SERVICE_COUNT; ++s) {
        size_t data_size = bodies[s].size - FLV_TAG_HEADER_SIZE;
        uint8_t footer[4];
        size_t total = bodies[s].size + FLV_TAG_FOOTER_SIZE;

        if (total > ctx->max_tag) {
            release_bodies(bodies);
            splitter_fail(ctx);
            return 0;
        }

        write_u24(bodies[s].data + 1, (uint32_t)data_size);
        write_u32(footer, (uint32_t)(FLV_TAG_HEADER_SIZE + data_size));

        if (!buffer_append(&bodies[s], footer, sizeof(footer))) {
            release_bodies(bodies);
            splitter_fail(ctx);
            return 0;
        }

        ctx->out[s] = bodies[s];
        buffer_init(&bodies[s]);
    }

    release_bodies(bodies);
    splitter_schedule(ctx);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* FLV parsing                                                               */

static int process_flv_header(caption_flv_splitter_t* ctx, const uint8_t* header, size_t size)
{
    unsigned s;

    if (size != FLV_HEADER_SIZE) {
        splitter_fail(ctx);
        return 0;
    }

    if ('F' != header[0] || 'L' != header[1] || 'V' != header[2]) {
        splitter_fail(ctx);
        return 0;
    }

    if (9 != read_u32(header + 5) || 0 != read_u32(header + 9)) {
        splitter_fail(ctx);
        return 0;
    }

    splitter_clear_outputs(ctx);

    for (s = 0; s < SERVICE_COUNT; ++s) {
        if (!buffer_append(&ctx->out[s], header, size)) {
            splitter_fail(ctx);
            return 0;
        }
    }

    ctx->have_header = 1;
    splitter_schedule(ctx);
    return 1;
}

static int process_flv_tag(caption_flv_splitter_t* ctx, const uint8_t* raw, size_t size)
{
    uint32_t data_size, stream_id, previous_tag_size;
    unsigned type;

    if (size < FLV_TAG_HEADER_SIZE + FLV_TAG_FOOTER_SIZE) {
        splitter_fail(ctx);
        return 0;
    }

    type = raw[0] & 0x1F;
    data_size = read_u24(raw + 1);
    stream_id = read_u24(raw + 8);

    if (size != FLV_TAG_HEADER_SIZE + (size_t)data_size + FLV_TAG_FOOTER_SIZE) {
        splitter_fail(ctx);
        return 0;
    }

    previous_tag_size = read_u32(raw + FLV_TAG_HEADER_SIZE + data_size);

    if (0 != stream_id) {
        splitter_fail(ctx);
        return 0;
    }

    if (previous_tag_size != FLV_TAG_HEADER_SIZE + data_size) {
        splitter_fail(ctx);
        return 0;
    }

    if (FLV_TAG_VIDEO == type && data_size >= 2 && AVC_CODEC_ID == (raw[FLV_TAG_HEADER_SIZE] & 0x0F)) {
        const uint8_t* body = raw + FLV_TAG_HEADER_SIZE;

        if (AVC_PACKET_SEQUENCE_HEADER == body[1]) {
            /* A new configuration may arrive mid-stream. */
            if (data_size >= 10) {
                ctx->nal_length_size = (body[9] & 0x03) + 1;
                ctx->have_config = 1;
            }

            return emit_passthrough_tag(ctx, raw, size);
        }

        if (AVC_PACKET_NALU == body[1] && ctx->have_config) {
            return rebuild_avc_nalu_tag(ctx, raw, size);
        }
    }

    return emit_passthrough_tag(ctx, raw, size);
}

/* ------------------------------------------------------------------------- */
/* input unit assembly                                                       */

/*
 * Compute the byte count of the unit currently being accumulated.  Returns 0
 * for an invalid/oversized tag (the caller fails the splitter).
 */
static size_t unit_required(caption_flv_splitter_t* ctx)
{
    size_t data_size;

    if (!ctx->have_header) {
        return FLV_HEADER_SIZE;
    }

    if (ctx->unit.size < FLV_TAG_HEADER_SIZE) {
        return FLV_TAG_HEADER_SIZE;
    }

    data_size = read_u24(ctx->unit.data + 1);

    if (data_size > ctx->max_tag || FLV_TAG_HEADER_SIZE + data_size + FLV_TAG_FOOTER_SIZE > ctx->max_tag) {
        return 0;
    }

    return FLV_TAG_HEADER_SIZE + data_size + FLV_TAG_FOOTER_SIZE;
}

static int splitter_process_unit(caption_flv_splitter_t* ctx)
{
    int ok;

    if (!ctx->have_header) {
        ok = process_flv_header(ctx, ctx->unit.data, ctx->unit.size);
    } else {
        ok = process_flv_tag(ctx, ctx->unit.data, ctx->unit.size);
    }

    ctx->unit.size = 0;
    return ok;
}

/* ------------------------------------------------------------------------- */
/* public API                                                                */

caption_flv_splitter_t* caption_flv_splitter_new(size_t max_tag, caption_flv_emit_fn emit, void* opaque)
{
    caption_flv_splitter_t* ctx;

    if (!emit || max_tag < FLV_HEADER_SIZE) {
        return NULL;
    }

    ctx = (caption_flv_splitter_t*)malloc(sizeof(caption_flv_splitter_t));

    if (!ctx) {
        return NULL;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->max_tag = max_tag;
    ctx->emit = emit;
    ctx->opaque = opaque;
    ctx->nal_length_size = 4;
    ctx->field_service[0] = 1; /* field 1 starts on CC1 */
    ctx->field_service[1] = 3; /* field 2 starts on CC3 */
    ctx->next_service = 1;
    buffer_init(&ctx->unit);

    return ctx;
}

int caption_flv_splitter_feed(caption_flv_splitter_t* ctx, const uint8_t* data, size_t size, size_t* consumed)
{
    size_t used = 0;

    if (consumed) {
        (*consumed) = 0;
    }

    if (!ctx) {
        return -1;
    }

    if (ctx->failed) {
        return -1;
    }

    if (ctx->finished) {
        if (size) {
            ctx->failed = 1;
            return -1;
        }

        return 0;
    }

    if (!data && size) {
        return -1;
    }

    for (;;) {
        size_t required;

        if (ctx->pending) {
            int drained = splitter_drain(ctx);

            if (drained < 0) {
                goto failed;
            }

            if (drained > 0) {
                break;
            }
        }

        required = unit_required(ctx);

        if (0 == required) {
            splitter_fail(ctx);
            goto failed;
        }

        if (ctx->unit.size >= required) {
            /* A whole unit is buffered; process it before taking more input. */
            if (!splitter_process_unit(ctx)) {
                goto failed;
            }

            continue;
        }

        if (used >= size) {
            break;
        }

        {
            size_t take = required - ctx->unit.size;

            if (take > size - used) {
                take = size - used;
            }

            if (!buffer_append(&ctx->unit, data + used, take)) {
                splitter_fail(ctx);
                goto failed;
            }

            used += take;
        }
    }

    if (consumed) {
        (*consumed) = used;
    }

    return ctx->pending ? 1 : 0;

failed:
    /* Report the bytes taken before the error; the splitter is now failed. */
    if (consumed) {
        (*consumed) = used;
    }

    return -1;
}

int caption_flv_splitter_finish(caption_flv_splitter_t* ctx)
{
    if (!ctx) {
        return -1;
    }

    if (ctx->failed) {
        return -1;
    }

    if (ctx->finished) {
        return 0;
    }

    if (ctx->pending) {
        int drained = splitter_drain(ctx);

        if (drained < 0) {
            return -1;
        }

        if (drained > 0) {
            return 1;
        }
    }

    /* A unit is only left in the buffer when it is incomplete. */
    if (!ctx->have_header || ctx->unit.size) {
        splitter_fail(ctx);
        return -1;
    }

    ctx->finished = 1;
    return 0;
}

void caption_flv_splitter_free(caption_flv_splitter_t* ctx)
{
    if (!ctx) {
        return;
    }

    buffer_free(&ctx->unit);
    splitter_clear_outputs(ctx);
    free(ctx);
}
