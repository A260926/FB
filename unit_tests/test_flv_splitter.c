/*
 * Tests for the FLV caption channel splitter and the shared SEI wire codec.
 *
 * The readers below are deliberately independent of the implementation:
 * FLV tags and SEI NALs are walked with plain byte arithmetic, and expected
 * outputs are built from hand written byte strings rather than by round
 * tripping through the library under test.
 */
#include "caption/flv_splitter.h"
#include "caption/sei_wire.h"
#include "check.h"
#include "caption/mpeg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* minimal test helpers                                                      */

static int bytes_equal(const uint8_t* a, size_t an, const uint8_t* b, size_t bn)
{
    return an == bn && (!an || 0 == memcmp(a, b, an));
}

static uint8_t* dup_bytes(const uint8_t* data, size_t size)
{
    uint8_t* copy = (uint8_t*)malloc(size ? size : 1);

    if (copy && size) {
        memcpy(copy, data, size);
    }

    return copy;
}

/* ------------------------------------------------------------------------- */
/* independent SEI reader                                                    */

typedef struct {
    unsigned type;
    size_t size;
    uint8_t* data;
} wire_message_t;

typedef struct {
    wire_message_t* messages;
    size_t count;
    size_t capacity;
} wire_sei_t;

static void wire_sei_free(wire_sei_t* w)
{
    size_t i;
    for (i = 0; i < w->count; ++i) {
        free(w->messages[i].data);
    }
    free(w->messages);
    w->messages = NULL;
    w->count = w->capacity = 0;
}

/* Unescape a complete NAL body byte by byte, tracking the zero run. */
static uint8_t* wire_unescape(const uint8_t* src, size_t size, size_t* out_size)
{
    uint8_t* dst = (uint8_t*)malloc(size + 1);
    size_t i, n = 0, zeros = 0;

    if (!dst) {
        return NULL;
    }

    for (i = 0; i < size; ++i) {
        if (zeros >= 2 && 0x03 == src[i]) {
            zeros = 0;
            continue;
        }

        dst[n++] = src[i];
        zeros = (0 == src[i]) ? zeros + 1 : 0;
    }

    *out_size = n;
    return dst;
}

/* Parse a complete SEI NAL (0x06 header included) with an independent reader. */
static int wire_sei_parse(wire_sei_t* w, const uint8_t* nal, size_t size)
{
    uint8_t* rbsp;
    size_t rbsp_size, pos = 0;

    memset(w, 0, sizeof(*w));

    if (size < 2 || 6 != (nal[0] & 0x1F)) {
        return -1;
    }

    rbsp = wire_unescape(nal + 1, size - 1, &rbsp_size);

    if (!rbsp) {
        return -1;
    }

    while (pos < rbsp_size) {
        unsigned type = 0, payload = 0;
        wire_message_t* m;

        if (1 == rbsp_size - pos) {
            break; /* trailing stop byte */
        }

        while (pos < rbsp_size && 0xFF == rbsp[pos]) {
            type += 255;
            ++pos;
        }

        if (pos >= rbsp_size) {
            free(rbsp);
            wire_sei_free(w);
            return -1;
        }

        type += rbsp[pos++];

        while (pos < rbsp_size && 0xFF == rbsp[pos]) {
            payload += 255;
            ++pos;
        }

        if (pos >= rbsp_size) {
            free(rbsp);
            wire_sei_free(w);
            return -1;
        }

        payload += rbsp[pos++];

        if (payload > rbsp_size - pos) {
            free(rbsp);
            wire_sei_free(w);
            return -1;
        }

        if (w->count == w->capacity) {
            size_t cap = w->capacity ? w->capacity * 2 : 4;
            wire_message_t* grown = (wire_message_t*)realloc(w->messages, cap * sizeof(*grown));

            if (!grown) {
                free(rbsp);
                wire_sei_free(w);
                return -1;
            }

            w->messages = grown;
            w->capacity = cap;
        }

        m = &w->messages[w->count++];
        m->type = type;
        m->size = payload;
        m->data = dup_bytes(rbsp + pos, payload);

        if (!m->data) {
            free(rbsp);
            wire_sei_free(w);
            return -1;
        }

        pos += payload;
    }

    free(rbsp);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* independent FLV reader                                                    */

typedef struct {
    uint8_t type;
    uint32_t timestamp;
    size_t data_size;
    uint8_t* body;
    size_t raw_size;
    uint8_t* raw;
} wire_tag_t;

typedef struct {
    wire_tag_t* tags;
    size_t count;
    size_t capacity;
    int has_video;
    int has_audio;
    int header_ok;
} wire_flv_t;

static void wire_flv_free(wire_flv_t* f)
{
    size_t i;
    for (i = 0; i < f->count; ++i) {
        free(f->tags[i].body);
        free(f->tags[i].raw);
    }
    free(f->tags);
    memset(f, 0, sizeof(*f));
}

static uint32_t wire_u24(const uint8_t* p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static uint32_t wire_u32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/*
 * Strict independent parser: validates signature, DataOffset 9, the zero
 * PreviousTagSize0, every tag's StreamID 0 and back pointer, and rebuilds each
 * body.  Returns 0 on a fully consistent stream.
 */
static int wire_flv_parse(wire_flv_t* f, const uint8_t* data, size_t size)
{
    size_t pos;
    uint32_t previous;

    memset(f, 0, sizeof(*f));

    if (size < 13) {
        return -1;
    }

    if (memcmp(data, "FLV", 3) || 1 != data[3]) {
        return -1;
    }

    if (9 != wire_u32(data + 5) || 0 != wire_u32(data + 9)) {
        return -1;
    }

    f->has_audio = !!(data[4] & 0x04);
    f->has_video = !!(data[4] & 0x01);
    f->header_ok = 1;
    pos = 13;
    previous = 0;

    while (pos < size) {
        const uint8_t* raw;
        uint32_t data_size, back;
        wire_tag_t* t;

        if (size - pos < 15) {
            return -1;
        }

        raw = data + pos;
        data_size = wire_u24(raw + 1);

        if (size - pos < 11 + (size_t)data_size + 4) {
            return -1;
        }

        if (wire_u24(raw + 8) != 0) {
            return -1;
        }

        back = wire_u32(raw + 11 + data_size);

        if (back != 11 + data_size) {
            return -1;
        }

        (void)previous;

        if (f->count == f->capacity) {
            size_t cap = f->capacity ? f->capacity * 2 : 8;
            wire_tag_t* grown = (wire_tag_t*)realloc(f->tags, cap * sizeof(*grown));

            if (!grown) {
                return -1;
            }

            f->tags = grown;
            f->capacity = cap;
        }

        t = &f->tags[f->count++];
        t->type = raw[0];
        t->data_size = data_size;
        t->timestamp = ((uint32_t)raw[7] << 24) | ((uint32_t)raw[4] << 16) | ((uint32_t)raw[5] << 8) | raw[6];
        t->raw_size = 11 + (size_t)data_size + 4;
        t->raw = dup_bytes(raw, t->raw_size);
        t->body = dup_bytes(raw + 11, data_size);

        if (!t->raw || !t->body) {
            return -1;
        }

        previous = back;
        pos += t->raw_size;
    }

    return 0;
}

/* ------------------------------------------------------------------------- */
/* FLV writers used to build fixtures                                        */

static void put_u24(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)v;
}

static void put_u32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

typedef struct {
    uint8_t* data;
    size_t size;
    size_t capacity;
} build_t;

static int build_append(build_t* b, const void* data, size_t size)
{
    if (b->size + size > b->capacity) {
        size_t cap = b->capacity ? b->capacity : 128;
        uint8_t* grown;

        while (cap < b->size + size) {
            cap *= 2;
        }

        grown = (uint8_t*)realloc(b->data, cap);

        if (!grown) {
            return 0;
        }

        b->data = grown;
        b->capacity = cap;
    }

    if (size) {
        memcpy(b->data + b->size, data, size);
    }

    b->size += size;
    return 1;
}

static void build_free(build_t* b)
{
    free(b->data);
    memset(b, 0, sizeof(*b));
}

static int build_header(build_t* b, int has_audio, int has_video)
{
    uint8_t h[13] = { 'F', 'L', 'V', 1, 0, 0, 0, 0, 9, 0, 0, 0, 0 };
    h[4] = (uint8_t)((has_audio ? 0x04 : 0) | (has_video ? 0x01 : 0));
    return build_append(b, h, 13);
}

static int build_tag(build_t* b, uint8_t type, uint32_t timestamp, const uint8_t* body, size_t body_size)
{
    uint8_t h[11] = { 0 };
    uint8_t footer[4];

    h[0] = type;
    put_u24(h + 1, (uint32_t)body_size);
    /** timestamp is stored as three low bytes followed by the high byte */
    h[4] = (uint8_t)(timestamp >> 16);
    h[5] = (uint8_t)(timestamp >> 8);
    h[6] = (uint8_t)timestamp;
    h[7] = (uint8_t)(timestamp >> 24);
    put_u24(h + 8, 0);
    put_u32(footer, (uint32_t)(11 + body_size));

    return build_append(b, h, sizeof(h)) && build_append(b, body, body_size) && build_append(b, footer, sizeof(footer));
}

/* ------------------------------------------------------------------------- */
/* SEI payload builders                                                      */

static uint16_t parity_word(uint16_t value)
{
    uint8_t bytes[2];
    unsigned i;

    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;

    /* Odd parity, computed the portable way. */
    for (i = 0; i < 2; ++i) {
        unsigned b = bytes[i], ones = 0, j;

        for (j = 0; j < 8; ++j) {
            ones += (b >> j) & 1;
        }

        bytes[i] = (uint8_t)((b & 0x7F) | ((ones & 1) ? 0x00 : 0x80));
    }

    return (uint16_t)((bytes[0] << 8) | bytes[1]);
}

static uint16_t basicna_pair(char a, char b)
{
    return parity_word((uint16_t)(((a - 0x20) << 8) | (b - 0x20)));
}

static uint16_t control_word(unsigned cc, unsigned code)
{
    uint16_t base = (uint16_t)(0x1420 | (code & 0x1F));

    if (cc & 2) {
        base = (uint16_t)(0x1420 | 0x0100 | (code & 0x1F));
    }

    if (cc & 1) {
        base |= 0x0800;
    }

    return parity_word(base);
}

/* RCL (resume caption loading) routed to a channel. */
static uint16_t rcl(unsigned channel)
{
    unsigned cc = (0 == channel || 1 == channel) ? (0 == channel ? 0 : 1) : (2 == channel ? 2 : 3);
    return control_word(cc, 0x00);
}

/* Field selector: cc_type is 0 for field 1 and 1 for field 2. */
/* Preamble setting row 0, column 0 on the given channel. */
static uint16_t preamble(unsigned channel)
{
    unsigned chan = (0 == channel || 2 == channel) ? 0 : 1;
    return parity_word((uint16_t)(0x1040 | (chan ? 0x0800 : 0) | (1 << 5)));
}

/* Mid row style change targeting the given channel. */
static uint16_t midrow(unsigned channel)
{
    unsigned chan = (0 == channel || 2 == channel) ? 0 : 1;
    return parity_word((uint16_t)(0x1120 | (chan ? 0x0800 : 0)));
}

/* Special character, e.g. registered sign, on a channel. */
static uint16_t special(unsigned channel)
{
    unsigned chan = (0 == channel || 2 == channel) ? 0 : 1;
    return parity_word((uint16_t)(0x1130 | (chan ? 0x0800 : 0)));
}

/* Extended western european character on a channel. */
static uint16_t westeu(unsigned channel)
{
    unsigned chan = (0 == channel || 2 == channel) ? 0 : 1;
    return parity_word((uint16_t)(0x1222 | (chan ? 0x0800 : 0)));
}

#define CC_VALID(word) ((uint8_t)(0xFC | 1)) /* marker 11111, valid, type 1 */
#define CC_FIELD2(word) ((uint8_t)(0xFD)) /* marker 11111, valid, type 1, field 2 */

static uint8_t entry_header(int valid, unsigned cc_type)
{
    return (uint8_t)(0xF8 | (valid ? 0x04 : 0x00) | (cc_type & 3));
}

typedef struct {
    uint8_t* data;
    size_t size;
} blob_t;

static void blob_free(blob_t* b)
{
    free(b->data);
    b->data = NULL;
    b->size = 0;
}

/* Compose a complete NAL from message type/payload pairs (no escaping). */
typedef struct {
    unsigned type;
    const uint8_t* data;
    size_t size;
} nal_msg_t;

static blob_t make_sei_nal(const nal_msg_t* messages, size_t count)
{
    build_t rbsp;
    build_t out;
    blob_t result;
    size_t i, zeros = 0;

    memset(&rbsp, 0, sizeof(rbsp));
    memset(&out, 0, sizeof(out));

    for (i = 0; i < count; ++i) {
        unsigned type = messages[i].type;
        size_t size = messages[i].size;
        uint8_t byte;

        while (type >= 255) {
            byte = 0xFF;
            build_append(&rbsp, &byte, 1);
            type -= 255;
        }
        byte = (uint8_t)type;
        build_append(&rbsp, &byte, 1);

        while (size >= 255) {
            byte = 0xFF;
            build_append(&rbsp, &byte, 1);
            size -= 255;
        }
        byte = (uint8_t)size;
        build_append(&rbsp, &byte, 1);
        build_append(&rbsp, messages[i].data, messages[i].size);
    }

    {
        uint8_t stop = 0x80;
        build_append(&rbsp, &stop, 1);
    }

    {
        uint8_t header = 6;
        build_append(&out, &header, 1);
    }

    for (i = 0; i < rbsp.size; ++i) {
        uint8_t b = rbsp.data[i];

        if (zeros >= 2 && b <= 3) {
            uint8_t esc = 3;
            build_append(&out, &esc, 1);
            zeros = 0;
        }

        build_append(&out, &b, 1);
        zeros = (0 == b) ? zeros + 1 : 0;
    }

    build_free(&rbsp);
    result.data = out.data;
    result.size = out.size;
    return result;
}

/* Build an AVC NALU tag body: video header, then length-prefixed NALs. */
static blob_t make_nalu_body(uint8_t frame, const blob_t* nals, size_t count, unsigned length_size)
{
    blob_t b = { NULL, 0 };
    build_t out;
    size_t i;

    memset(&out, 0, sizeof(out));

    {
        uint8_t head[5];
        head[0] = (uint8_t)((frame << 4) | 7);
        head[1] = 1;
        head[2] = 0;
        head[3] = 0;
        head[4] = 0;
        build_append(&out, head, sizeof(head));
    }

    for (i = 0; i < count; ++i) {
        uint8_t len[4] = { 0 };
        unsigned j;

        for (j = 0; j < length_size; ++j) {
            len[length_size - 1 - j] = (uint8_t)(nals[i].size >> (8 * j));
        }

        build_append(&out, len, length_size);
        build_append(&out, nals[i].data, nals[i].size);
    }

    b.data = out.data;
    b.size = out.size;
    return b;
}

/* ------------------------------------------------------------------------- */
/* capture callback                                                          */

typedef struct {
    unsigned service;
    uint8_t* bytes;
    size_t size;
} emitted_t;

typedef struct {
    emitted_t items[4096];
    size_t count;
    size_t capacity;
    size_t calls;
    /* Optional per-call scripted behaviour, indexed by call number. */
    int (*policy)(void* user, unsigned service, size_t call);
    void* user;
} capture_t;

static void capture_init(capture_t* c)
{
    memset(c, 0, sizeof(*c));
    c->capacity = sizeof(c->items) / sizeof(c->items[0]);
}

static void capture_free(capture_t* c)
{
    size_t i;
    for (i = 0; i < c->count; ++i) {
        free(c->items[i].bytes);
    }
    c->count = 0;
}

static int capture_emit(unsigned service, const uint8_t* bytes, size_t size, void* opaque)
{
    capture_t* c = (capture_t*)opaque;
    int decision = 0;

    if (c->policy) {
        decision = c->policy(c->user, service, c->calls);
    }

    ++c->calls;

    if (decision < 0) {
        return -1;
    }

    if (decision > 0) {
        return 1;
    }

    if (c->count < c->capacity) {
        c->items[c->count].service = service;
        c->items[c->count].bytes = dup_bytes(bytes, size);
        c->items[c->count].size = size;
        ++c->count;
    }

    return 0;
}

/* Drive a splitter with one chunk, retrying paused output via zero feeds. */
static int drive(caption_flv_splitter_t* ctx, const uint8_t* data, size_t size)
{
    size_t offset = 0;

    while (offset < size) {
        size_t consumed = 0;
        int r = caption_flv_splitter_feed(ctx, data + offset, size - offset, &consumed);
        offset += consumed;

        if (r < 0) {
            return -1;
        }

        if (0 == consumed && 0 == r) {
            return -1;
        }
    }

    for (;;) {
        int r = caption_flv_splitter_feed(ctx, NULL, 0, NULL);

        if (r < 0) {
            return -1;
        }

        if (0 == r) {
            break;
        }
    }

    return 0;
}

/* Concatenate all emitted buffers for one service. */
static int service_stream(const capture_t* c, unsigned service, uint8_t** out, size_t* out_size)
{
    build_t b;
    size_t i;

    memset(&b, 0, sizeof(b));

    for (i = 0; i < c->count; ++i) {
        if (c->items[i].service == service) {
            build_append(&b, c->items[i].bytes, c->items[i].size);
        }
    }

    *out = b.data;
    *out_size = b.size;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* fixture: a multi-tag FLV carrying the interesting caption shapes          */

typedef struct {
    uint8_t* data;
    size_t size;
} stream_t;

/* ------------------------------------------------------------------------- */
/* expected GA94 payload model                                               */

#define S1 0x1u
#define S2 0x2u
#define S3 0x4u
#define S4 0x8u
#define ALL 0xFu

typedef struct {
    uint8_t b0, b1, b2;
    unsigned mask;
} exp_entry_t;

static exp_entry_t make_entry(int valid, unsigned cc_type, uint16_t word, unsigned mask)
{
    exp_entry_t e;
    e.b0 = entry_header(valid, cc_type);
    e.b1 = (uint8_t)(word >> 8);
    e.b2 = (uint8_t)word;
    e.mask = mask;
    return e;
}

/* Build the GA94 payload for one service (svc 1..4) or the unfiltered input
 * (svc 0, every entry present).  Only cc_count (in the flags byte) changes. */
static blob_t build_ga94_payload(const exp_entry_t* entries, size_t count, uint8_t flags_base, uint8_t em_data, const uint8_t* tail, size_t tail_size, unsigned svc)
{
    build_t out;
    uint8_t head[10];
    unsigned kept = 0;
    size_t i;
    blob_t b;

    memset(&out, 0, sizeof(out));

    for (i = 0; i < count; ++i) {
        if (0 == svc || (entries[i].mask & (1u << (svc - 1)))) {
            ++kept;
        }
    }

    head[0] = 181;
    head[1] = 0x00;
    head[2] = 0x31;
    head[3] = 'G';
    head[4] = 'A';
    head[5] = '9';
    head[6] = '4';
    head[7] = 3;
    head[8] = (uint8_t)((flags_base & 0xE0) | (kept & 0x1F));
    head[9] = em_data;
    build_append(&out, head, sizeof(head));

    for (i = 0; i < count; ++i) {
        if (0 == svc || (entries[i].mask & (1u << (svc - 1)))) {
            uint8_t e[3];
            e[0] = entries[i].b0;
            e[1] = entries[i].b1;
            e[2] = entries[i].b2;
            build_append(&out, e, 3);
        }
    }

    build_append(&out, tail, tail_size);

    b.data = out.data;
    b.size = out.size;
    return b;
}

#define SERVICE_COUNT_LOCAL 4

/* ------------------------------------------------------------------------- */
/* fixture model                                                             */

#define MAX_EXP_TAGS 64
#define MAX_NALS 16

typedef struct {
    uint8_t type;
    uint32_t timestamp;
    uint8_t* body[SERVICE_COUNT_LOCAL];
    size_t body_size[SERVICE_COUNT_LOCAL];
} exp_tag_t;

typedef struct {
    build_t stream;
    exp_tag_t tags[MAX_EXP_TAGS];
    size_t tag_count;
} fixture_t;

static void fixture_init(fixture_t* f) { memset(f, 0, sizeof(*f)); }

static void fixture_free(fixture_t* f)
{
    size_t i;
    unsigned s;

    build_free(&f->stream);

    for (i = 0; i < f->tag_count; ++i) {
        for (s = 0; s < SERVICE_COUNT_LOCAL; ++s) {
            free(f->tags[i].body[s]);
        }
    }

    f->tag_count = 0;
}

static exp_tag_t* fixture_add_tag(fixture_t* f, uint8_t type, uint32_t timestamp)
{
    exp_tag_t* t;

    if (f->tag_count == MAX_EXP_TAGS) {
        return NULL;
    }

    t = &f->tags[f->tag_count++];
    memset(t, 0, sizeof(*t));
    t->type = type;
    t->timestamp = timestamp;
    return t;
}

static int fixture_add_raw_tag(fixture_t* f, uint8_t type, uint32_t timestamp, const uint8_t* body, size_t size)
{
    exp_tag_t* t;
    unsigned s;

    if (!build_tag(&f->stream, type, timestamp, body, size)) {
        return 0;
    }

    t = fixture_add_tag(f, type, timestamp);

    if (!t) {
        return 0;
    }

    for (s = 0; s < SERVICE_COUNT_LOCAL; ++s) {
        t->body[s] = dup_bytes(body, size);
        t->body_size[s] = size;
    }

    return 1;
}

/*
 * Add a video NALU tag.  nals holds the original NALs (input body); svc_nals
 * holds the expected NALs for each service.  Both use `length_size` byte
 * length prefixes in the tag.
 */
static int fixture_add_video_tag(fixture_t* f, uint32_t timestamp, uint8_t frame, const blob_t* nals, const blob_t svc_nals[SERVICE_COUNT_LOCAL][MAX_NALS], size_t nal_count, unsigned length_size)
{
    blob_t input_body;
    exp_tag_t* t;
    unsigned s;

    input_body = make_nalu_body(frame, nals, nal_count, length_size);

    if (!build_tag(&f->stream, 9, timestamp, input_body.data, input_body.size)) {
        blob_free(&input_body);
        return 0;
    }

    t = fixture_add_tag(f, 9, timestamp);

    if (!t) {
        blob_free(&input_body);
        return 0;
    }

    for (s = 0; s < SERVICE_COUNT_LOCAL; ++s) {
        build_t body;
        size_t i;

        memset(&body, 0, sizeof(body));
        build_append(&body, input_body.data, 5); /* frame/codec + packet type + cts */

        for (i = 0; i < nal_count; ++i) {
            uint8_t len[4] = { 0 };
            unsigned j;

            for (j = 0; j < length_size; ++j) {
                len[length_size - 1 - j] = (uint8_t)(svc_nals[s][i].size >> (8 * j));
            }

            build_append(&body, len, length_size);
            build_append(&body, svc_nals[s][i].data, svc_nals[s][i].size);
        }

        t->body[s] = body.data;
        t->body_size[s] = body.size;
    }

    blob_free(&input_body);
    return 1;
}

/* Feed a whole buffer with retries; used by the routing tests. */
static int run_whole(const fixture_t* f, capture_t* cap)
{
    caption_flv_splitter_t* ctx = caption_flv_splitter_new(4096, capture_emit, cap);
    int r;

    if (!ctx) {
        return -1;
    }

    r = drive(ctx, f->stream.data, f->stream.size);

    if (0 == r) {
        r = caption_flv_splitter_finish(ctx);

        if (0 > r) {
            r = -1;
        }
    }

    caption_flv_splitter_free(ctx);
    return r;
}

/* Verify captured output against the fixture expectations. */
static int verify_fixture(const capture_t* cap, const fixture_t* f)
{
    unsigned s;
    size_t unit;

    /* Every unit emits exactly one callback per service, ordered 1..4. */
    TASK_CHECK(cap->count == f->tag_count * SERVICE_COUNT_LOCAL + SERVICE_COUNT_LOCAL);

    for (unit = 0; unit < cap->count; ++unit) {
        unsigned expected = (unsigned)(unit % SERVICE_COUNT_LOCAL) + 1;
        TASK_CHECK(cap->items[unit].service == expected);
    }

    for (s = 1; s <= SERVICE_COUNT_LOCAL; ++s) {
        uint8_t* stream = NULL;
        size_t stream_size = 0;
        wire_flv_t parsed;
        size_t i;

        service_stream(cap, s, &stream, &stream_size);
        TASK_CHECK(0 == wire_flv_parse(&parsed, stream, stream_size));
        TASK_CHECK(parsed.header_ok);
        TASK_CHECK(parsed.count == f->tag_count);

        for (i = 0; i < f->tag_count && i < parsed.count; ++i) {
            TASK_CHECK(parsed.tags[i].type == f->tags[i].type);
            TASK_CHECK(parsed.tags[i].timestamp == f->tags[i].timestamp);
            TASK_CHECK(bytes_equal(parsed.tags[i].body, parsed.tags[i].data_size, f->tags[i].body[s - 1], f->tags[i].body_size[s - 1]));
        }

        wire_flv_free(&parsed);
        free(stream);
    }

    return 0;
}


/* ------------------------------------------------------------------------- */
/* scenario builder                                                          */

/*
 * A compact scenario description: a list of tags, each either a raw pass
 * through tag or a video NALU tag whose SEI payloads are described by
 * messages.  The input bytes and every expected per-service body are derived
 * from the same description, keeping the expectations independent of the
 * implementation under test.
 */

typedef struct {
    unsigned type;
    blob_t all;
    blob_t svc[SERVICE_COUNT_LOCAL];
} scenario_message_t;

static scenario_message_t scenario_message_ga94(const exp_entry_t* entries, size_t count, uint8_t flags_base, uint8_t em_data, const uint8_t* tail, size_t tail_size)
{
    scenario_message_t m;
    unsigned s;

    memset(&m, 0, sizeof(m));
    m.type = 4;
    m.all = build_ga94_payload(entries, count, flags_base, em_data, tail, tail_size, 0);

    for (s = 0; s < SERVICE_COUNT_LOCAL; ++s) {
        m.svc[s] = build_ga94_payload(entries, count, flags_base, em_data, tail, tail_size, s + 1);
    }

    return m;
}

static scenario_message_t scenario_message_raw(unsigned type, const uint8_t* data, size_t size)
{
    scenario_message_t m;
    unsigned s;

    memset(&m, 0, sizeof(m));
    m.type = type;
    m.all.data = dup_bytes(data, size);
    m.all.size = size;

    for (s = 0; s < SERVICE_COUNT_LOCAL; ++s) {
        m.svc[s].data = dup_bytes(data, size);
        m.svc[s].size = size;
    }

    return m;
}

static void scenario_message_free(scenario_message_t* m)
{
    unsigned s;
    blob_free(&m->all);
    for (s = 0; s < SERVICE_COUNT_LOCAL; ++s) {
        blob_free(&m->svc[s]);
    }
}

static blob_t scenario_nal(const scenario_message_t* messages, size_t count, int service)
{
    nal_msg_t wire[16];
    size_t i;

    for (i = 0; i < count; ++i) {
        wire[i].type = messages[i].type;
        wire[i].data = (service < 0) ? messages[i].all.data : messages[i].svc[service].data;
        wire[i].size = (service < 0) ? messages[i].all.size : messages[i].svc[service].size;
    }

    return make_sei_nal(wire, count);
}

/*
 * Add an AVC video tag combining `prefix` and `suffix` raw NALs with the SEI
 * NAL derived from `messages`.  The SEI NAL is emitted right after the prefix
 * NALs, preserving its position.  When messages is NULL no SEI NAL is added.
 */
static int scenario_add_video(fixture_t* f, uint32_t timestamp, uint8_t frame,
    const blob_t* prefix, size_t prefix_count,
    const scenario_message_t* messages, size_t message_count,
    const blob_t* suffix, size_t suffix_count, unsigned length_size)
{
    blob_t input[MAX_NALS];
    blob_t per_service[SERVICE_COUNT_LOCAL][MAX_NALS];
    size_t in_count = 0, total;
    int ok;
    unsigned s;
    size_t i;

    memset(input, 0, sizeof(input));
    memset(per_service, 0, sizeof(per_service));

    for (i = 0; i < prefix_count; ++i) {
        input[in_count++] = prefix[i];
    }

    if (messages) {
        input[in_count++] = scenario_nal(messages, message_count, -1);
    }

    for (i = 0; i < suffix_count; ++i) {
        input[in_count++] = suffix[i];
    }

    total = in_count;

    for (s = 0; s < SERVICE_COUNT_LOCAL; ++s) {
        size_t k = 0;

        for (i = 0; i < prefix_count; ++i) {
            per_service[s][k++] = prefix[i];
        }

        if (messages) {
            per_service[s][k++] = scenario_nal(messages, message_count, (int)s);
        }

        for (i = 0; i < suffix_count; ++i) {
            per_service[s][k++] = suffix[i];
        }
    }

    ok = fixture_add_video_tag(f, timestamp, frame, input, (const blob_t(*)[MAX_NALS])per_service, total, length_size);

    if (messages) {
        blob_free(&input[prefix_count]);
        for (s = 0; s < SERVICE_COUNT_LOCAL; ++s) {
            blob_free(&per_service[s][prefix_count]);
        }
    }

    return ok;
}

static blob_t blob_copy(const uint8_t* data, size_t size)
{
    blob_t b;
    b.data = dup_bytes(data, size);
    b.size = size;
    return b;
}

/* ------------------------------------------------------------------------- */
/* fixture stream                                                            */

static const uint8_t k_script[] = { 2, 0, 4, 't', 'e', 's', 't' };
static const uint8_t k_audio1[] = { 0xAF, 0x01, 0x10 };
static const uint8_t k_audio2[] = { 0xAF, 0x01, 0x20 };
static const uint8_t k_audio3[] = { 0xAF, 0x01, 0x30 };
static const uint8_t k_config4[] = { 0x17, 0x00, 0, 0, 0, 0x01, 0x42, 0x00, 0x1E, 0xFF, 0xE0, 0x00 };
static const uint8_t k_config2[] = { 0x17, 0x00, 0, 0, 0, 0x01, 0x42, 0x00, 0x1E, 0xFD, 0xE0, 0x00 };
static const uint8_t k_eos[] = { 0x17, 0x02, 0x00, 0x00, 0x00 };
static const uint8_t k_unregistered[] = { 0x00, 0x11, 0x22 };
static const uint8_t k_tail_a[] = { 0xDE, 0xAD };
static const uint8_t k_tail_b[] = { 0xBE, 0xEF, 0x00, 0x00 };
static const uint8_t k_aud[] = { 0x09, 0xF0 };
static const uint8_t k_slice[] = { 0x41, 0x9A, 0x00, 0x00, 0x03, 0x21 };
static const uint8_t k_filler[] = { 0x0C, 0x12, 0x34 };

/*
 * Build the complete scenario.  The ordering exercises a script tag, audio
 * interleaving, an interleaved second field, native 708, invalid and padding
 * entries, multiple SEI messages (unregistered and trailing payloads), XDS
 * spanning three tags, 32 bit timestamps and a mid-stream NAL length change.
 */
static int build_fixture(fixture_t* f)
{
    exp_entry_t a[24];
    exp_entry_t b[6];
    exp_entry_t d[3];
    exp_entry_t e[6];
    exp_entry_t g[3];
    scenario_message_t msgs[3];
    blob_t aud, slice, filler;
    int ok = 0;

    fixture_init(f);

    aud = blob_copy(k_aud, sizeof(k_aud));
    slice = blob_copy(k_slice, sizeof(k_slice));
    filler = blob_copy(k_filler, sizeof(k_filler));

    if (!build_header(&f->stream, 1, 1) || !fixture_add_raw_tag(f, 18, 0, k_script, sizeof(k_script)) || !fixture_add_raw_tag(f, 8, 0x01000001u, k_audio1, sizeof(k_audio1)) || !fixture_add_raw_tag(f, 9, 0, k_config4, sizeof(k_config4))) {
        goto done;
    }

    /* ---- message A: channel walking in both fields plus raw entry kinds */
    a[0] = make_entry(1, 0, rcl(0), S1);
    a[1] = make_entry(1, 0, preamble(0), S1);
    a[2] = make_entry(1, 0, basicna_pair('A', 'B'), S1);
    a[3] = make_entry(1, 0, rcl(1), S2);
    a[4] = make_entry(1, 0, basicna_pair('E', 'F'), S2);
    a[5] = make_entry(1, 0, preamble(1), S2);
    a[6] = make_entry(1, 0, basicna_pair('G', 'H'), S2);
    a[7] = make_entry(1, 0, special(0), S1);
    a[8] = make_entry(1, 0, basicna_pair('I', 'J'), S1);
    a[9] = make_entry(1, 0, midrow(1), S2);
    a[10] = make_entry(1, 0, basicna_pair('K', 'L'), S2);
    a[11] = make_entry(1, 0, westeu(0), S1);
    a[12] = make_entry(1, 0, basicna_pair('M', 'N'), S1);
    a[13] = make_entry(1, 1, rcl(2), S3);
    a[14] = make_entry(1, 1, preamble(2), S3);
    a[15] = make_entry(1, 1, basicna_pair('O', 'P'), S3);
    a[16] = make_entry(1, 1, rcl(3), S4);
    a[17] = make_entry(1, 1, basicna_pair('Q', 'R'), S4);
    a[18] = make_entry(1, 1, rcl(2), S3);
    a[19] = make_entry(1, 1, basicna_pair('S', 'T'), S3);
    a[20] = make_entry(1, 2, 0x1234, ALL); /* native 708 */
    a[21] = make_entry(0, 0, basicna_pair('U', 'V'), ALL); /* invalid entry */
    a[22] = make_entry(1, 0, 0x8080, ALL); /* padding */
    a[23] = make_entry(1, 3, 0x5678, ALL); /* native 708 type 3 */

    b[0] = make_entry(1, 0, rcl(1), S2);
    b[1] = make_entry(1, 0, basicna_pair('U', 'V'), S2);
    b[2] = make_entry(1, 2, 0x0A0B, ALL);
    b[3] = make_entry(0, 1, 0x1111, ALL);
    b[4] = make_entry(1, 1, 0x8080, ALL);
    b[5] = make_entry(1, 0, basicna_pair('W', 'X'), S2);

    msgs[0] = scenario_message_ga94(a, 24, 0x40, 0xAA, k_tail_a, sizeof(k_tail_a));
    msgs[1] = scenario_message_raw(5, k_unregistered, sizeof(k_unregistered));
    msgs[2] = scenario_message_ga94(b, 6, 0xC0, 0xBB, k_tail_b, sizeof(k_tail_b));

    if (!scenario_add_video(f, 1000, 1, NULL, 0, msgs, 3, NULL, 0, 4)) {
        goto done;
    }

    scenario_message_free(&msgs[0]);
    scenario_message_free(&msgs[1]);
    scenario_message_free(&msgs[2]);

    if (!fixture_add_raw_tag(f, 8, 1001, k_audio2, sizeof(k_audio2))) {
        goto done;
    }

    /* ---- non-SEI video tag: audio then filler NAL, copied verbatim */
    {
        blob_t nals[2];
        nals[0] = aud;
        nals[1] = filler;

        if (!scenario_add_video(f, 1002, 2, nals, 2, NULL, 0, NULL, 0, 4)) {
            goto done;
        }
    }

    /* ---- XDS start and cross tag continuation, interleaved audio */
    d[0] = make_entry(1, 1, 0x0101, ALL);
    d[1] = make_entry(1, 1, 0x2520, ALL);
    d[2] = make_entry(1, 1, 0x4546, ALL);

    msgs[0] = scenario_message_ga94(d, 3, 0x40, 0xCC, NULL, 0);
    {
        blob_t nl[1];
        nl[0] = aud;

        if (!scenario_add_video(f, 0x01000001u, 1, NULL, 0, msgs, 1, nl, 1, 4)) {
            scenario_message_free(&msgs[0]);
            goto done;
        }

        scenario_message_free(&msgs[0]);
    }

    if (!fixture_add_raw_tag(f, 8, 0x01000002u, k_audio3, sizeof(k_audio3))) {
        goto done;
    }

    /* ---- XDS data and checksum, then field 1 and field 2 switches */
    e[0] = make_entry(1, 1, 0x4546, ALL);
    e[1] = make_entry(1, 1, 0x8F01, ALL);
    e[2] = make_entry(1, 1, rcl(2), S3);
    e[3] = make_entry(1, 1, basicna_pair('G', 'H'), S3);
    e[4] = make_entry(1, 0, rcl(1), S2);
    e[5] = make_entry(1, 0, basicna_pair('Z', 'Z'), S2);

    msgs[0] = scenario_message_ga94(e, 6, 0x40, 0xDD, NULL, 0);

    if (!scenario_add_video(f, 0x01000003u, 2, NULL, 0, msgs, 1, &slice, 1, 4)) {
        scenario_message_free(&msgs[0]);
        goto done;
    }

    scenario_message_free(&msgs[0]);

    /* ---- mid stream configuration change: 2 byte NAL lengths */
    if (!fixture_add_raw_tag(f, 9, 0x01000004u, k_config2, sizeof(k_config2))) {
        goto done;
    }

    g[0] = make_entry(1, 0, basicna_pair('a', 'b'), S2);
    g[1] = make_entry(1, 0, special(0), S1);
    g[2] = make_entry(1, 0, basicna_pair('c', 'd'), S1);

    msgs[0] = scenario_message_ga94(g, 3, 0x40, 0xEE, NULL, 0);
    {
        blob_t nl[1];
        nl[0] = slice;

        if (!scenario_add_video(f, 0x01000005u, 1, NULL, 0, msgs, 1, nl, 1, 2)) {
            scenario_message_free(&msgs[0]);
            goto done;
        }

        scenario_message_free(&msgs[0]);
    }

    /* ---- AVC end of sequence with a full 32 bit timestamp */
    if (!fixture_add_raw_tag(f, 9, 0xFF123456u, k_eos, sizeof(k_eos))) {
        goto done;
    }

    ok = 1;

done:
    blob_free(&aud);
    blob_free(&slice);
    blob_free(&filler);
    return ok;
}

/* ------------------------------------------------------------------------- */
/* wire tests                                                                */

static int test_wire_roundtrip_basic(void)
{
    sei_t sei;
    uint8_t* encoded = NULL;
    size_t encoded_size = 0;
    wire_sei_t parsed;

    sei_init(&sei, 0);
    TASK_CHECK(0 == caption_sei_encode(&sei, &encoded, &encoded_size));
    TASK_CHECK(encoded_size == 2 && encoded[0] == 6 && encoded[1] == 0x80);
    TASK_CHECK(0 == wire_sei_parse(&parsed, encoded, encoded_size));
    TASK_CHECK(parsed.count == 0);
    wire_sei_free(&parsed);
    free(encoded);
    encoded = NULL;
    encoded_size = 0;

    {
        static const uint8_t payload[] = { 0x01, 0x02, 0x03 };
        sei_message_t* m = sei_message_new((sei_msgtype_t)4, (uint8_t*)payload, sizeof(payload));
        sei_message_append(&sei, m);
    }

    TASK_CHECK(0 == caption_sei_encode(&sei, &encoded, &encoded_size));
    TASK_CHECK(0 == wire_sei_parse(&parsed, encoded, encoded_size));
    TASK_CHECK(parsed.count == 1 && parsed.messages[0].type == 4 && parsed.messages[0].size == 3);
    TASK_CHECK(0 == memcmp(parsed.messages[0].data, "\x01\x02\x03", 3));
    wire_sei_free(&parsed);
    free(encoded);
    sei_free(&sei);
    return 0;
}

/* 00 00 03 boundaries: escaping must be computed over the whole RBSP. */
static int test_wire_boundary_escape(void)
{
    /* Payload ends in two zeros and the next message's payload type is 0, so
     * the 00 00 00 sequence straddles the message boundary and needs an
     * emulation prevention byte there.  An encoder that reset its zero count
     * per payload would omit it. */
    static const uint8_t payload_a[] = { 0x41, 0x00, 0x00 };
    static const uint8_t payload_b[] = { 0xAA };
    static const size_t second_type = 0;
    sei_t sei;
    uint8_t* encoded = NULL;
    size_t encoded_size = 0;
    wire_sei_t parsed;
    size_t i;
    int found_boundary = 0;

    sei_init(&sei, 0);
    sei_message_append(&sei, sei_message_new((sei_msgtype_t)4, (uint8_t*)payload_a, sizeof(payload_a)));
    sei_message_append(&sei, sei_message_new((sei_msgtype_t)second_type, (uint8_t*)payload_b, sizeof(payload_b)));

    TASK_CHECK(0 == caption_sei_encode(&sei, &encoded, &encoded_size));

    /* A 00 00 03 spanning the payload_a / header boundary must be present. */
    for (i = 0; i + 5 < encoded_size; ++i) {
        if (0x00 == encoded[i] && 0x00 == encoded[i + 1] && 0x03 == encoded[i + 2]) {
            found_boundary = 1;
        }
    }

    TASK_CHECK(found_boundary);

    TASK_CHECK(0 == wire_sei_parse(&parsed, encoded, encoded_size));
    TASK_CHECK(parsed.count == 2);
    TASK_CHECK(parsed.messages[0].size == 3 && 0 == memcmp(parsed.messages[0].data, payload_a, 3));
    TASK_CHECK(parsed.messages[1].type == second_type);
    TASK_CHECK(parsed.messages[1].size == 1 && 0 == memcmp(parsed.messages[1].data, payload_b, 1));
    wire_sei_free(&parsed);

    /* The library decoder must reproduce the same chain. */
    {
        sei_t round;
        sei_message_t* first;
        sei_message_t* second;
        sei_init(&round, 0);
        TASK_CHECK(0 == caption_sei_decode(&round, encoded, encoded_size));
        first = sei_message_head(&round);
        TASK_CHECK(first && 4 == sei_message_type(first) && 3 == sei_message_size(first));
        TASK_CHECK(0 == memcmp(sei_message_data(first), payload_a, 3));
        second = sei_message_next(first);
        TASK_CHECK(second && (unsigned)second_type == sei_message_type(second) && 1 == sei_message_size(second));
        TASK_CHECK(0 == memcmp(sei_message_data(second), payload_b, 1));
        TASK_CHECK(sei_message_next(second) == NULL);
        sei_free(&round);
    }

    free(encoded);
    sei_free(&sei);
    return 0;
}

static int test_wire_decode_escaped_headers(void)
{
    /* Hand written NAL where a payload type and size use multiple 0xFF bytes
     * and a zero length message is present, with escaping inside the header. */
    static const uint8_t nal[] = {
        0x06,
        0xFF, 0xFF, 0x06, 0x00, /* payload type 516, size 0 */
        0x05, 0x03, 0x00, 0x00, 0x03, 0x01, /* type 5, size 3, payload 00 00 01 */
        0x80
    };
    sei_t sei;
    wire_sei_t parsed;

    sei_init(&sei, 0);
    TASK_CHECK(0 == caption_sei_decode(&sei, nal, sizeof(nal)));
    TASK_CHECK(sei.head != NULL);
    TASK_CHECK(sei_message_type(sei.head) == 516);
    TASK_CHECK(sei_message_size(sei.head) == 0);
    TASK_CHECK(sei_message_next(sei.head) != NULL);
    TASK_CHECK(sei_message_type(sei_message_next(sei.head)) == 5);
    TASK_CHECK(sei_message_size(sei_message_next(sei.head)) == 3);
    TASK_CHECK(0 == memcmp(sei_message_data(sei_message_next(sei.head)), "\x00\x00\x01", 3));
    TASK_CHECK(sei.timestamp == 0);

    TASK_CHECK(0 == wire_sei_parse(&parsed, nal, sizeof(nal)));
    TASK_CHECK(parsed.count == 2);
    TASK_CHECK(parsed.messages[0].type == 516 && parsed.messages[0].size == 0);
    TASK_CHECK(parsed.messages[1].type == 5 && parsed.messages[1].size == 3);
    wire_sei_free(&parsed);
    sei_free(&sei);
    return 0;
}

static int test_wire_decode_failures(void)
{
    sei_t sei;
    uint8_t* encoded = NULL;
    size_t encoded_size = 1234;

    sei_init(&sei, 0);
    TASK_CHECK(-1 == caption_sei_decode(&sei, NULL, 0));
    TASK_CHECK(sei.head == NULL);

    {
        static const uint8_t not_sei[] = { 0x05, 0x80 };
        TASK_CHECK(-1 == caption_sei_decode(&sei, not_sei, sizeof(not_sei)));
        TASK_CHECK(sei.head == NULL);
    }

    {
        /* Header claims 5 payload bytes but only 2 follow the stop bit. */
        static const uint8_t trunc[] = { 0x06, 0x04, 0x05, 0xAA, 0xBB, 0x80 };
        TASK_CHECK(-1 == caption_sei_decode(&sei, trunc, sizeof(trunc)));
        TASK_CHECK(sei.head == NULL);
    }

    {
        /* Trailing 0xFF with no terminating type byte. */
        static const uint8_t trunc[] = { 0x06, 0xFF, 0x80 };
        TASK_CHECK(-1 == caption_sei_decode(&sei, trunc, sizeof(trunc)));
        TASK_CHECK(sei.head == NULL);
    }

    TASK_CHECK(-1 == caption_sei_encode(NULL, &encoded, &encoded_size));
    TASK_CHECK(encoded == NULL && encoded_size == 0);
    sei_free(&sei);
    return 0;
}

static int test_wire_multi_extension(void)
{
    /* Payload type and size both require several 255 extension bytes. */
    uint8_t payload[520];
    sei_t sei;
    uint8_t* encoded = NULL;
    size_t encoded_size = 0;
    wire_sei_t parsed;
    size_t i;

    for (i = 0; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)(i * 7 + 1);
    }

    sei_init(&sei, 0);
    sei_message_append(&sei, sei_message_new((sei_msgtype_t)260, payload, 510));
    sei_message_append(&sei, sei_message_new((sei_msgtype_t)0, payload, 0));
    sei_message_append(&sei, sei_message_new((sei_msgtype_t)765, payload, 255));

    TASK_CHECK(0 == caption_sei_encode(&sei, &encoded, &encoded_size));
    TASK_CHECK(0 == wire_sei_parse(&parsed, encoded, encoded_size));
    TASK_CHECK(parsed.count == 3);
    TASK_CHECK(parsed.messages[0].type == 260 && parsed.messages[0].size == 510);
    TASK_CHECK(parsed.messages[1].type == 0 && parsed.messages[1].size == 0);
    TASK_CHECK(parsed.messages[2].type == 765 && parsed.messages[2].size == 255);
    TASK_CHECK(0 == memcmp(parsed.messages[0].data, payload, 510));
    wire_sei_free(&parsed);
    free(encoded);
    sei_free(&sei);
    return 0;
}
/* ------------------------------------------------------------------------- */
/* routing tests                                                             */

static int test_routing_full_stream(void)
{
    fixture_t f;
    capture_t cap;

    capture_init(&cap);
    TASK_CHECK(build_fixture(&f));
    TASK_CHECK(0 == run_whole(&f, &cap));
    TASK_CHECK(0 == verify_fixture(&cap, &f));

    capture_free(&cap);
    fixture_free(&f);
    return 0;
}

/* Fresh routing state per splitter: a second run must produce the same bytes. */
static int test_routing_repeatable(void)
{
    fixture_t f;
    capture_t first, second;
    size_t i;

    capture_init(&first);
    capture_init(&second);
    TASK_CHECK(build_fixture(&f));
    TASK_CHECK(0 == run_whole(&f, &first));
    TASK_CHECK(0 == run_whole(&f, &second));
    TASK_CHECK(first.count == second.count);

    for (i = 0; i < first.count && i < second.count; ++i) {
        TASK_CHECK(first.items[i].service == second.items[i].service);
        TASK_CHECK(bytes_equal(first.items[i].bytes, first.items[i].size, second.items[i].bytes, second.items[i].size));
    }

    capture_free(&first);
    capture_free(&second);
    fixture_free(&f);
    return 0;
}

/* A SEI carried on its own tag still starts routing from the same state. */
static int test_routing_isolated_message(void)
{
    fixture_t f;
    capture_t cap;
    exp_entry_t words[4];
    scenario_message_t msg;

    fixture_init(&f);
    capture_init(&cap);
    TASK_CHECK(build_header(&f.stream, 1, 1));
    TASK_CHECK(fixture_add_raw_tag(&f, 9, 0, k_config4, sizeof(k_config4)));

    words[0] = make_entry(1, 0, rcl(1), S2); /* switch field 1 to CC2 */
    words[1] = make_entry(1, 0, basicna_pair('A', 'B'), S2);
    words[2] = make_entry(1, 1, rcl(3), S4); /* switch field 2 to CC4 */
    words[3] = make_entry(1, 1, basicna_pair('C', 'D'), S4);

    msg = scenario_message_ga94(words, 4, 0x60, 0x11, NULL, 0);
    TASK_CHECK(scenario_add_video(&f, 500, 1, NULL, 0, &msg, 1, NULL, 0, 4));
    scenario_message_free(&msg);

    TASK_CHECK(0 == run_whole(&f, &cap));
    TASK_CHECK(0 == verify_fixture(&cap, &f));

    capture_free(&cap);
    fixture_free(&f);
    return 0;
}

/* GA94 messages that must not be projected (wrong provider/flags) are kept. */
static int test_routing_non_caption_messages(void)
{
    fixture_t f;
    capture_t cap;
    scenario_message_t msg;
    uint8_t other_provider[12];
    uint8_t no_process[12];
    uint8_t wrong_type[12];
    unsigned s;

    fixture_init(&f);
    capture_init(&cap);
    TASK_CHECK(build_header(&f.stream, 1, 1));
    TASK_CHECK(fixture_add_raw_tag(&f, 9, 0, k_config4, sizeof(k_config4)));

    /* Same cc_data with a non ATSC provider (country 181, provider 47). */
    memcpy(other_provider, "\xB5\x00\x2FGA94\x03\x40\x08\xFC\x94\x20", 12);
    /* ATSC/GA94 but process_cc_data_flag clear. */
    memcpy(no_process, "\xB5\x00\x31GA94\x03\x00\x08\xFC\x94\x20", 12);
    /* ATSC/GA94 with user_data_type_code 4. */
    memcpy(wrong_type, "\xB5\x00\x31GA94\x04\x40\x08\xFC\x94\x20", 12);

    msg = scenario_message_raw(4, other_provider, sizeof(other_provider));
    TASK_CHECK(scenario_add_video(&f, 1, 1, NULL, 0, &msg, 1, NULL, 0, 4));
    scenario_message_free(&msg);

    msg = scenario_message_raw(4, no_process, sizeof(no_process));
    TASK_CHECK(scenario_add_video(&f, 2, 1, NULL, 0, &msg, 1, NULL, 0, 4));
    scenario_message_free(&msg);

    msg = scenario_message_raw(4, wrong_type, sizeof(wrong_type));
    TASK_CHECK(scenario_add_video(&f, 3, 1, NULL, 0, &msg, 1, NULL, 0, 4));
    scenario_message_free(&msg);

    TASK_CHECK(0 == run_whole(&f, &cap));

    /* Each service receives the identical, unmodified tag bytes. */
    for (s = 2; s <= SERVICE_COUNT_LOCAL; ++s) {
        size_t i;
        for (i = 0; i < f.tag_count; ++i) {
            TASK_CHECK(bytes_equal(f.tags[i].body[s - 1], f.tags[i].body_size[s - 1], f.tags[i].body[0], f.tags[i].body_size[0]));
        }
    }

    TASK_CHECK(0 == verify_fixture(&cap, &f));

    capture_free(&cap);
    fixture_free(&f);
    return 0;
}

/*
 * A control code inside an XDS sequence interrupts it: the in-progress packet
 * is abandoned, the control word routes as ordinary caption data, and a later
 * data word is no longer treated as XDS.
 */
static int test_routing_xds_interrupted(void)
{
    fixture_t f;
    capture_t cap;
    scenario_message_t msg;
    exp_entry_t words[4];

    fixture_init(&f);
    capture_init(&cap);
    TASK_CHECK(build_header(&f.stream, 1, 1));
    TASK_CHECK(fixture_add_raw_tag(&f, 9, 0, k_config4, sizeof(k_config4)));

    words[0] = make_entry(1, 1, 0x0101, ALL); /* XDS start */
    words[1] = make_entry(1, 1, rcl(2), S3); /* control interrupts, selects CC3 */
    words[2] = make_entry(1, 1, 0x2520, S3); /* no longer XDS */
    words[3] = make_entry(1, 1, basicna_pair('X', 'Y'), S3);

    msg = scenario_message_ga94(words, 4, 0x40, 0x00, NULL, 0);
    TASK_CHECK(scenario_add_video(&f, 40, 1, NULL, 0, &msg, 1, NULL, 0, 4));
    scenario_message_free(&msg);

    TASK_CHECK(0 == run_whole(&f, &cap));
    TASK_CHECK(0 == verify_fixture(&cap, &f));

    capture_free(&cap);
    fixture_free(&f);
    return 0;
}

/*
 * Multiple GA94 messages inside one SEI NAL share routing state, and messages
 * with no surviving entries stay present with cc_count 0.
 */
static int test_routing_multi_message_nal(void)
{
    fixture_t f;
    capture_t cap;
    scenario_message_t msgs[3];
    exp_entry_t first[2];
    exp_entry_t second[2];
    exp_entry_t third[1];
    static const uint8_t other_payload[] = { 0x01, 0x02 };

    fixture_init(&f);
    capture_init(&cap);
    TASK_CHECK(build_header(&f.stream, 1, 1));
    TASK_CHECK(fixture_add_raw_tag(&f, 9, 0, k_config4, sizeof(k_config4)));

    first[0] = make_entry(1, 0, rcl(1), S2); /* CC2 selected */
    first[1] = make_entry(1, 0, basicna_pair('A', 'B'), S2);

    second[0] = make_entry(1, 1, rcl(3), S4); /* CC4 selected */
    second[1] = make_entry(1, 1, basicna_pair('C', 'D'), S4);

    /* Only an unregistered entry: this message is projected to nothing on
     * every service but must remain in the chain. */
    third[0] = make_entry(1, 2, 0x4242, ALL);

    msgs[0] = scenario_message_ga94(first, 2, 0x40, 0x01, NULL, 0);
    msgs[1] = scenario_message_raw(5, other_payload, sizeof(other_payload));
    msgs[2] = scenario_message_ga94(third, 1, 0x40, 0x02, NULL, 0);

    TASK_CHECK(scenario_add_video(&f, 10, 1, NULL, 0, msgs, 3, NULL, 0, 4));
    scenario_message_free(&msgs[0]);
    scenario_message_free(&msgs[1]);
    scenario_message_free(&msgs[2]);

    TASK_CHECK(0 == run_whole(&f, &cap));
    TASK_CHECK(0 == verify_fixture(&cap, &f));

    capture_free(&cap);
    fixture_free(&f);
    return 0;
}

/* Messages whose cc_count drops to zero remain with a zero count and no
 * entries, while the rest of the message (em_data, tail) is untouched. */
static int test_routing_empty_projection(void)
{
    fixture_t f;
    capture_t cap;
    scenario_message_t msg;
    exp_entry_t words[2];
    static const uint8_t tail[] = { 0x00, 0x00, 0x7F };

    fixture_init(&f);
    capture_init(&cap);
    TASK_CHECK(build_header(&f.stream, 1, 1));
    TASK_CHECK(fixture_add_raw_tag(&f, 9, 0, k_config4, sizeof(k_config4)));

    /* Field 1 starts on CC1 and field 2 on CC3, so each word survives on
     * exactly one service; the other three keep a cc_count 0 message. */
    words[0] = make_entry(1, 0, basicna_pair('P', 'Q'), S1);
    words[1] = make_entry(1, 1, basicna_pair('R', 'S'), S3);

    msg = scenario_message_ga94(words, 2, 0xE0, 0x5A, tail, sizeof(tail));
    TASK_CHECK(scenario_add_video(&f, 20, 1, NULL, 0, &msg, 1, NULL, 0, 4));
    scenario_message_free(&msg);

    TASK_CHECK(0 == run_whole(&f, &cap));
    TASK_CHECK(0 == verify_fixture(&cap, &f));

    capture_free(&cap);
    fixture_free(&f);
    return 0;
}

/* An undecodable SEI NAL and an empty message chain are passed through. */
static int test_routing_passthrough_nal(void)
{
    fixture_t f;
    capture_t cap;
    blob_t truncated;
    unsigned s;

    fixture_init(&f);
    capture_init(&cap);
    TASK_CHECK(build_header(&f.stream, 1, 1));
    TASK_CHECK(fixture_add_raw_tag(&f, 9, 0, k_config4, sizeof(k_config4)));

    /* SEI NAL whose header claims more payload than is present. */
    truncated = blob_copy((const uint8_t*)"\x06\x04\x20\xAA", 4);

    {
        blob_t nals[1];
        blob_t same[SERVICE_COUNT_LOCAL][MAX_NALS];
        size_t i;

        nals[0] = truncated;

        for (s = 0; s < SERVICE_COUNT_LOCAL; ++s) {
            same[s][0] = truncated;
        }

        TASK_CHECK(fixture_add_video_tag(&f, 30, 1, nals, (const blob_t(*)[MAX_NALS])same, 1, 4));
        (void)i;
    }

    blob_free(&truncated);

    TASK_CHECK(0 == run_whole(&f, &cap));
    TASK_CHECK(0 == verify_fixture(&cap, &f));

    capture_free(&cap);
    fixture_free(&f);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* backpressure tests                                                        */


typedef struct {
    unsigned stall_service; /* reject attempts of this service */
    unsigned stall_count; /* how many attempts to reject */
    unsigned stalled; /* number of rejections issued so far */
    unsigned expected_next; /* next service the callback may accept */
} backpressure_state_t;

/*
 * Strict backpressure callback used by several tests.
 *
 *  - the first attempt at `stall_service` (if 1..4) is rejected once, so the
 *    following zero byte feed must retry the very same bytes;
 *  - accepted services must arrive strictly 1,2,3,4 and never repeat;
 *  - the bytes of a rejected buffer are copied so they can be compared with
 *    the retry.
 */
typedef struct {
    capture_t capture;
    backpressure_state_t state;
    uint8_t* rejected_bytes;
    size_t rejected_size;
    unsigned rejected_service;
} strict_capture_t;

static int strict_emit(unsigned service, const uint8_t* bytes, size_t size, void* opaque)
{
    strict_capture_t* sc = (strict_capture_t*)opaque;
    int ret = 0;

    TASK_CHECK(service >= 1 && service <= SERVICE_COUNT_LOCAL);

    if (sc->rejected_bytes && service == sc->rejected_service) {
        /* This is the retry of a rejected buffer: it must match byte for byte. */
        TASK_CHECK(bytes_equal(sc->rejected_bytes, sc->rejected_size, bytes, size));
        free(sc->rejected_bytes);
        sc->rejected_bytes = NULL;
    }

    if (sc->state.stalled < sc->state.stall_count && service == sc->state.stall_service) {
        if (sc->state.stalled == 0) {
            sc->rejected_bytes = dup_bytes(bytes, size);
            sc->rejected_size = size;
            sc->rejected_service = service;
        }

        ++sc->state.stalled;
        return 1;
    }

    TASK_CHECK(service == sc->state.expected_next);
    sc->state.expected_next = (service % SERVICE_COUNT_LOCAL) + 1;

    ret = capture_emit(service, bytes, size, &sc->capture);
    return ret;
}

static int test_backpressure_stall_same_service(void)
{
    fixture_t f;
    strict_capture_t sc;
    caption_flv_splitter_t* ctx;
    size_t consumed = 0;
    int r;

    fixture_init(&f);
    capture_init(&sc.capture);
    memset(&sc.state, 0, sizeof(sc.state));
    sc.state.stall_service = 1;
    sc.state.stall_count = 3;
    sc.state.expected_next = 1;
    sc.rejected_bytes = NULL;
    sc.rejected_size = 0;
    sc.rejected_service = 0;

    TASK_CHECK(build_fixture(&f));
    ctx = caption_flv_splitter_new(4096, strict_emit, &sc);
    TASK_CHECK(ctx != NULL);

    /* The first unit is produced but service 1 stalls. */
    r = caption_flv_splitter_feed(ctx, f.stream.data, f.stream.size, &consumed);
    TASK_CHECK(1 == r);
    TASK_CHECK(1 == sc.state.stalled);
    TASK_CHECK(sc.capture.count == 0);
    TASK_CHECK(consumed > 0 && consumed < f.stream.size);

    /* Repeated zero byte feeds keep retrying only service 1, unchanged. */
    r = caption_flv_splitter_feed(ctx, NULL, 0, NULL);
    TASK_CHECK(1 == r);
    TASK_CHECK(2 == sc.state.stalled);
    TASK_CHECK(sc.capture.count == 0);

    r = caption_flv_splitter_feed(ctx, NULL, 0, NULL);
    TASK_CHECK(1 == r);
    TASK_CHECK(3 == sc.state.stalled);
    TASK_CHECK(sc.capture.count == 0);

    /* The fourth attempt is accepted, then services 2..4 follow. */
    r = caption_flv_splitter_feed(ctx, NULL, 0, NULL);
    TASK_CHECK(0 == r);
    TASK_CHECK(NULL == sc.rejected_bytes);
    TASK_CHECK(sc.capture.count == SERVICE_COUNT_LOCAL);
    TASK_CHECK(sc.capture.items[0].service == 1);

    TASK_CHECK(0 == caption_flv_splitter_finish(ctx));

    capture_free(&sc.capture);
    free(sc.rejected_bytes);
    caption_flv_splitter_free(ctx);
    fixture_free(&f);
    return 0;
}

/* Reject every service exactly once on its first attempt. */
static int test_backpressure_each_service(void)
{
    unsigned target;

    for (target = 1; target <= SERVICE_COUNT_LOCAL; ++target) {
        fixture_t f;
        strict_capture_t sc;
        caption_flv_splitter_t* ctx;
        size_t consumed = 0;
        size_t offset = 0;
        int r;

        fixture_init(&f);
        capture_init(&sc.capture);
        memset(&sc.state, 0, sizeof(sc.state));
        sc.state.stall_service = target;
        sc.state.stall_count = 1;
        sc.state.expected_next = 1;
        sc.rejected_bytes = NULL;
        sc.rejected_size = 0;
        sc.rejected_service = 0;

        TASK_CHECK(build_fixture(&f));
        ctx = caption_flv_splitter_new(4096, strict_emit, &sc);
        TASK_CHECK(ctx != NULL);

        while (offset < f.stream.size) {
            r = caption_flv_splitter_feed(ctx, f.stream.data + offset, f.stream.size - offset, &consumed);
            offset += consumed;

            if (r < 0) {
                break;
            }

            while (1 == r) {
                r = caption_flv_splitter_feed(ctx, NULL, 0, NULL);
            }
        }

        TASK_CHECK(r >= 0);
        TASK_CHECK(0 == caption_flv_splitter_finish(ctx));
        TASK_CHECK(0 == verify_fixture(&sc.capture, &f));

        free(sc.rejected_bytes);
        caption_flv_splitter_free(ctx);
        capture_free(&sc.capture);
        fixture_free(&f);
    }

    return 0;
}

/* Callback abort is terminal and no further callback happens. */
static int abort_policy(void* user, unsigned service, size_t call)
{
    (void)user;
    (void)call;
    return (service == 2) ? -1 : 0;
}

static int test_backpressure_abort(void)
{
    fixture_t f;
    capture_t cap;
    caption_flv_splitter_t* ctx;
    size_t consumed = 0;
    size_t calls_after;

    fixture_init(&f);
    capture_init(&cap);
    TASK_CHECK(build_fixture(&f));
    ctx = caption_flv_splitter_new(4096, capture_emit, &cap);
    TASK_CHECK(ctx != NULL);
    cap.policy = abort_policy;

    TASK_CHECK(-1 == caption_flv_splitter_feed(ctx, f.stream.data, f.stream.size, &consumed));
    calls_after = cap.calls;

    /* Once failed: finish and feed stay failed and no callback occurs. */
    TASK_CHECK(-1 == caption_flv_splitter_finish(ctx));
    TASK_CHECK(-1 == caption_flv_splitter_feed(ctx, NULL, 0, NULL));
    TASK_CHECK(cap.calls == calls_after);

    caption_flv_splitter_free(ctx);
    capture_free(&cap);
    fixture_free(&f);
    return 0;
}

/* Zero byte feeds drive pending output; consumed stays 0 while paused. */
static int test_backpressure_zero_feed(void)
{
    fixture_t f;
    capture_t cap;
    caption_flv_splitter_t* ctx;
    size_t consumed = 12345;
    int r;

    fixture_init(&f);
    capture_init(&cap);
    TASK_CHECK(build_fixture(&f));
    ctx = caption_flv_splitter_new(4096, capture_emit, &cap);
    TASK_CHECK(ctx != NULL);

    /* Feed only the header; all four service callbacks happen immediately. */
    r = caption_flv_splitter_feed(ctx, f.stream.data, 13, &consumed);
    TASK_CHECK(0 == r);
    TASK_CHECK(13 == consumed);
    TASK_CHECK(cap.count == SERVICE_COUNT_LOCAL);

    /* A zero byte feed with nothing pending is a no-op. */
    consumed = 777;
    r = caption_flv_splitter_feed(ctx, NULL, 0, &consumed);
    TASK_CHECK(0 == r);
    TASK_CHECK(0 == consumed);

    caption_flv_splitter_free(ctx);
    capture_free(&cap);
    fixture_free(&f);
    return 0;
}

/* Truncation at any prefix fails, and failure is persistent without callbacks. */
static int test_backpressure_truncation(void)
{
    fixture_t f;
    size_t cut;

    fixture_init(&f);
    TASK_CHECK(build_fixture(&f));

    for (cut = 0; cut < f.stream.size; ++cut) {
        capture_t cap;
        caption_flv_splitter_t* ctx;
        size_t calls;

        capture_init(&cap);
        ctx = caption_flv_splitter_new(4096, capture_emit, &cap);
        TASK_CHECK(ctx != NULL);

        /* Feeding a prefix alone never finishes successfully. */
        {
            size_t off = 0;
            int r = 0;

            while (off < cut) {
                size_t consumed = 0;
                r = caption_flv_splitter_feed(ctx, f.stream.data + off, cut - off, &consumed);
                off += consumed;

                if (r < 0) {
                    break;
                }

                if (0 == consumed && 0 == r) {
                    break;
                }
            }

            TASK_CHECK(r >= 0);
        }

        {
            int finish = caption_flv_splitter_finish(ctx);

            if (cut == f.stream.size) {
                TASK_CHECK(0 == finish);
            } else if (0 == finish) {
                /* A prefix that happens to end on a unit boundary is a valid
                 * stream; feeding more after finishing must be refused. */
                calls = cap.calls;
                TASK_CHECK(-1 == caption_flv_splitter_feed(ctx, f.stream.data + cut, f.stream.size - cut, NULL));
                TASK_CHECK(cap.calls == calls);
            } else {
                TASK_CHECK(-1 == finish);
                /* Truncation is persistent: finish and feed keep failing and
                 * no further callbacks occur. */
                calls = cap.calls;
                TASK_CHECK(-1 == caption_flv_splitter_finish(ctx));
                TASK_CHECK(-1 == caption_flv_splitter_feed(ctx, f.stream.data, 1, NULL));
                TASK_CHECK(cap.calls == calls);
            }
        }

        caption_flv_splitter_free(ctx);
        capture_free(&cap);
    }

    fixture_free(&f);
    return 0;
}

/* Whole and fragmented input must yield byte identical output. */
static int test_backpressure_fragments(void)
{
    fixture_t f;
    capture_t whole;
    size_t chunk;

    fixture_init(&f);
    capture_init(&whole);
    TASK_CHECK(build_fixture(&f));
    TASK_CHECK(0 == run_whole(&f, &whole));

    for (chunk = 1; chunk <= 17; ++chunk) {
        capture_t frag;
        caption_flv_splitter_t* ctx;
        size_t off = 0;
        size_t i;

        capture_init(&frag);
        ctx = caption_flv_splitter_new(4096, capture_emit, &frag);
        TASK_CHECK(ctx != NULL);

        while (off < f.stream.size) {
            size_t take = f.stream.size - off;

            if (take > chunk) {
                take = chunk;
            }

            TASK_CHECK(0 == drive(ctx, f.stream.data + off, take));
            off += take;
        }

        TASK_CHECK(0 == caption_flv_splitter_finish(ctx));
        TASK_CHECK(frag.count == whole.count);

        for (i = 0; i < frag.count && i < whole.count; ++i) {
            TASK_CHECK(frag.items[i].service == whole.items[i].service);
            TASK_CHECK(bytes_equal(frag.items[i].bytes, frag.items[i].size, whole.items[i].bytes, whole.items[i].size));
        }

        caption_flv_splitter_free(ctx);
        capture_free(&frag);
    }

    capture_free(&whole);
    fixture_free(&f);
    return 0;
}

/* Argument and state validation. */
static int test_backpressure_api(void)
{
    capture_t cap;
    caption_flv_splitter_t* ctx;
    uint8_t header[13] = { 'F', 'L', 'V', 1, 5, 0, 0, 0, 9, 0, 0, 0, 0 };
    size_t consumed = 0;
    static const uint8_t bad_header_do[13] = { 'F', 'L', 'V', 1, 5, 0, 0, 0, 10, 0, 0, 0, 0 };
    static const uint8_t bad_header_prev[13] = { 'F', 'L', 'V', 1, 5, 0, 0, 0, 9, 0, 0, 0, 1 };
    static const uint8_t not_flv[13] = { 'X', 'L', 'V', 1, 5, 0, 0, 0, 9, 0, 0, 0, 0 };

    capture_init(&cap);
    TASK_CHECK(NULL == caption_flv_splitter_new(12, capture_emit, &cap));
    TASK_CHECK(NULL == caption_flv_splitter_new(13, NULL, &cap));

    ctx = caption_flv_splitter_new(13, capture_emit, &cap);
    TASK_CHECK(ctx != NULL);
    TASK_CHECK(0 == caption_flv_splitter_feed(ctx, header, 13, &consumed));
    TASK_CHECK(0 == caption_flv_splitter_finish(ctx));
    TASK_CHECK(0 == caption_flv_splitter_finish(ctx));
    /* Feeding after a successful finish is an error. */
    TASK_CHECK(-1 == caption_flv_splitter_feed(ctx, header, 13, NULL));
    caption_flv_splitter_free(ctx);

    /* Bad FLV headers fail immediately. */
    ctx = caption_flv_splitter_new(4096, capture_emit, &cap);
    TASK_CHECK(-1 == caption_flv_splitter_feed(ctx, bad_header_do, 13, &consumed));
    caption_flv_splitter_free(ctx);

    ctx = caption_flv_splitter_new(4096, capture_emit, &cap);
    TASK_CHECK(-1 == caption_flv_splitter_feed(ctx, bad_header_prev, 13, &consumed));
    caption_flv_splitter_free(ctx);

    ctx = caption_flv_splitter_new(4096, capture_emit, &cap);
    TASK_CHECK(-1 == caption_flv_splitter_feed(ctx, not_flv, 13, &consumed));
    caption_flv_splitter_free(ctx);

    capture_free(&cap);
    return 0;
}

/* Bad tags fail: StreamID, back pointer, oversized tag. */
static int test_backpressure_bad_tags(void)
{
    fixture_t f;
    capture_t cap;
    caption_flv_splitter_t* ctx;
    size_t consumed = 0;

    fixture_init(&f);
    capture_init(&cap);
    TASK_CHECK(build_header(&f.stream, 1, 1));

    /* A tag with a non-zero StreamID. */
    {
        uint8_t body[] = { 0xAF, 0x01 };
        size_t pos = f.stream.size;
        TASK_CHECK(build_tag(&f.stream, 8, 0, body, sizeof(body)));
        f.stream.data[pos + 8] = 1;
    }

    ctx = caption_flv_splitter_new(4096, capture_emit, &cap);
    TASK_CHECK(ctx != NULL);
    TASK_CHECK(-1 == caption_flv_splitter_feed(ctx, f.stream.data, f.stream.size, &consumed));
    TASK_CHECK(-1 == caption_flv_splitter_finish(ctx));
    caption_flv_splitter_free(ctx);

    /* A tag whose previous tag size is wrong. */
    fixture_free(&f);
    fixture_init(&f);
    TASK_CHECK(build_header(&f.stream, 1, 1));
    {
        uint8_t body[] = { 0xAF, 0x01 };
        size_t pos = f.stream.size;
        TASK_CHECK(build_tag(&f.stream, 8, 0, body, sizeof(body)));
        f.stream.data[f.stream.size - 1] ^= 0xFF;
        (void)pos;
    }

    ctx = caption_flv_splitter_new(4096, capture_emit, &cap);
    TASK_CHECK(ctx != NULL);
    TASK_CHECK(-1 == caption_flv_splitter_feed(ctx, f.stream.data, f.stream.size, &consumed));
    caption_flv_splitter_free(ctx);

    /* A tag larger than max_tag is rejected as soon as its header arrives. */
    fixture_free(&f);
    fixture_init(&f);
    cap.calls = 0;
    TASK_CHECK(build_header(&f.stream, 1, 1));
    {
        uint8_t body[64];
        memset(body, 0xAF, sizeof(body));
        TASK_CHECK(build_tag(&f.stream, 8, 0, body, sizeof(body)));
    }

    ctx = caption_flv_splitter_new(40, capture_emit, &cap);
    TASK_CHECK(ctx != NULL);
    TASK_CHECK(-1 == caption_flv_splitter_feed(ctx, f.stream.data, f.stream.size, &consumed));
    caption_flv_splitter_free(ctx);

    capture_free(&cap);
    fixture_free(&f);
    return 0;
}

/* finish() may return 1 while output is paused, then succeed. */
static int delayed_policy(void* user, unsigned service, size_t call)
{
    unsigned* rejects = (unsigned*)user;
    (void)service;

    if (*rejects) {
        --*rejects;
        return 1;
    }

    (void)call;
    return 0;
}

static int test_backpressure_finish_paused(void)
{
    fixture_t f;
    capture_t cap;
    caption_flv_splitter_t* ctx;
    unsigned rejects = 2;
    size_t consumed = 0;

    fixture_init(&f);
    capture_init(&cap);
    TASK_CHECK(build_header(&f.stream, 1, 1));
    ctx = caption_flv_splitter_new(4096, capture_emit, &cap);
    TASK_CHECK(ctx != NULL);
    cap.policy = delayed_policy;
    cap.user = &rejects;

    /* The header output is rejected twice: once while feeding, once while
     * finishing, so both report the pause. */
    TASK_CHECK(1 == caption_flv_splitter_feed(ctx, f.stream.data, f.stream.size, &consumed));
    TASK_CHECK(consumed == f.stream.size);
    TASK_CHECK(1 == caption_flv_splitter_finish(ctx));
    TASK_CHECK(cap.count == 0);

    /* Draining through a zero feed completes the header, then finish works. */
    TASK_CHECK(0 == caption_flv_splitter_feed(ctx, NULL, 0, NULL));
    TASK_CHECK(cap.count == SERVICE_COUNT_LOCAL);
    TASK_CHECK(0 == caption_flv_splitter_finish(ctx));

    caption_flv_splitter_free(ctx);
    capture_free(&cap);
    fixture_free(&f);
    return 0;
}
/* ------------------------------------------------------------------------- */
/* case registration                                                         */

const struct task_case task_cases[] = {
    { "wire", "basic", test_wire_roundtrip_basic },
    { "wire", "boundary_escape", test_wire_boundary_escape },
    { "wire", "escaped_headers", test_wire_decode_escaped_headers },
    { "wire", "failures", test_wire_decode_failures },
    { "wire", "multi_extension", test_wire_multi_extension },
    { "routing", "full_stream", test_routing_full_stream },
    { "routing", "repeatable", test_routing_repeatable },
    { "routing", "isolated_message", test_routing_isolated_message },
    { "routing", "non_caption", test_routing_non_caption_messages },
    { "routing", "multi_message_nal", test_routing_multi_message_nal },
    { "routing", "xds_interrupted", test_routing_xds_interrupted },
    { "routing", "empty_projection", test_routing_empty_projection },
    { "routing", "passthrough_nal", test_routing_passthrough_nal },
    { "backpressure", "stall_same_service", test_backpressure_stall_same_service },
    { "backpressure", "each_service", test_backpressure_each_service },
    { "backpressure", "abort", test_backpressure_abort },
    { "backpressure", "zero_feed", test_backpressure_zero_feed },
    { "backpressure", "truncation", test_backpressure_truncation },
    { "backpressure", "fragments", test_backpressure_fragments },
    { "backpressure", "api", test_backpressure_api },
    { "backpressure", "bad_tags", test_backpressure_bad_tags },
    { "backpressure", "finish_paused", test_backpressure_finish_paused },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
