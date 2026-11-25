/*
 * Tests for the incremental fragmented-MP4 demuxer.
 *
 * Every case builds its input from independent box writers instead of reusing
 * the production muxer, so the tests exercise layouts the muxer never emits
 * (several truns, signed offsets, tfdt versions, sparse fragments, ...).
 */
#define _POSIX_C_SOURCE 200809L

#include "check.h"
#include "mp4d_stream.h"
#include "minimp4.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Byte buffer helpers                                                */
/* ------------------------------------------------------------------ */

typedef struct
{
    unsigned char *p;
    size_t n;
    size_t cap;
} buf_t;

static int buf_reserve(buf_t *b, size_t extra)
{
    size_t need = b->n + extra, cap;
    unsigned char *mem;
    if (need <= b->cap)
        return 0;
    cap = b->cap ? b->cap : 256;
    while (cap < need)
        cap *= 2;
    mem = (unsigned char *)realloc(b->p, cap);
    if (!mem)
        return -1;
    b->p = mem;
    b->cap = cap;
    return 0;
}

static int buf_u8(buf_t *b, unsigned v)
{
    unsigned char c = (unsigned char)v;
    return buf_reserve(b, 1) ? -1 : (b->p[b->n++] = c, 0);
}

static int buf_u16(buf_t *b, unsigned v)
{
    return buf_u8(b, v >> 8) ? -1 : buf_u8(b, v);
}

static int buf_u32(buf_t *b, uint32_t v)
{
    return buf_u16(b, v >> 16) ? -1 : buf_u16(b, v & 0xFFFF);
}

static int buf_u64(buf_t *b, uint64_t v)
{
    return buf_u32(b, (uint32_t)(v >> 32)) ? -1 : buf_u32(b, (uint32_t)v);
}

static int buf_bytes(buf_t *b, const void *p, size_t n)
{
    if (buf_reserve(b, n))
        return -1;
    memcpy(b->p + b->n, p, n);
    b->n += n;
    return 0;
}

static uint32_t fourcc(const char *s)
{
    return ((uint32_t)(unsigned char)s[0] << 24) | ((uint32_t)(unsigned char)s[1] << 16) |
        ((uint32_t)(unsigned char)s[2] << 8) | (uint32_t)(unsigned char)s[3];
}

static int box_begin(buf_t *b, const char *name)
{
    return buf_u32(b, 0) ? -1 : buf_u32(b, fourcc(name));
}

static int box_end(buf_t *b, size_t start)
{
    uint32_t size = (uint32_t)(b->n - start);
    b->p[start + 0] = (unsigned char)(size >> 24);
    b->p[start + 1] = (unsigned char)(size >> 16);
    b->p[start + 2] = (unsigned char)(size >> 8);
    b->p[start + 3] = (unsigned char)size;
    return 0;
}

/* Extended-size box header. */
static int box_begin_ext(buf_t *b, const char *name)
{
    return buf_u32(b, 1) ? -1 : buf_u32(b, fourcc(name));
}

static int box_end_ext(buf_t *b, size_t start)
{
    uint64_t size = (uint64_t)(b->n - start) + 8;
    uint32_t lo = (uint32_t)size, hi = (uint32_t)(size >> 32);
    if (buf_reserve(b, 8))
        return -1;
    /* Insert the 64-bit size right after the 1/'name' header. */
    memmove(b->p + start + 16, b->p + start + 8, b->n - (start + 8));
    b->p[start + 8] = (unsigned char)(hi >> 24);
    b->p[start + 9] = (unsigned char)(hi >> 16);
    b->p[start + 10] = (unsigned char)(hi >> 8);
    b->p[start + 11] = (unsigned char)hi;
    b->p[start + 12] = (unsigned char)(lo >> 24);
    b->p[start + 13] = (unsigned char)(lo >> 16);
    b->p[start + 14] = (unsigned char)(lo >> 8);
    b->p[start + 15] = (unsigned char)lo;
    b->n += 8;
    return 0;
}

static int fullbox_header(buf_t *b, unsigned version, unsigned flags)
{
    return buf_u8(b, version) ? -1 : buf_u8(b, flags >> 16) ? -1 :
        buf_u8(b, flags >> 8) ? -1 : buf_u8(b, flags);
}

/* ------------------------------------------------------------------ */
/* Init-segment writer                                                */
/* ------------------------------------------------------------------ */

/* AVC sample entry + avcC (SPS/PPS equal to the ones the muxer uses). */
static const unsigned char avc1_entry[] =
{
    '\x00', '\x00', '\x00', '\x76', 'a', 'v', 'c', '1',
    0, 0, 0, 0, 0, 0, 0, 1,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0x10, 0, 0x10, 0, 0x48, 0, 0, 0, 0x48, 0, 0, 0, 0, 0, 0,
    0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0x18, 0xff, 0xff,
    0, 0, 0, 0x20, 'a', 'v', 'c', 'C',
    1, 0x42, 0, 0x1e, 0xff, 0xe1, 0, 9,
    0x67, 0x42, 0, 0x1e, 0xe9, 1, 0x40, 0x7b, 0x20,
    1, 0, 4, 0x68, 0xce, 6, 0xe2
};

/* mp4a sample entry + esds (AAC-LC, 2 channels). */
static const unsigned char mp4a_entry[] =
{
    '\x00', '\x00', '\x00', '\x48', 'm', 'p', '4', 'a',
    0, 0, 0, 0, 0, 0, 0, 1,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0x10, 0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0x24, 'e', 's', 'd', 's',
    0, 0, 0, 0,
    3, 0x16, 0, 0, 0, 4, 0x11, 0x40, 0x14, 0, 6, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 5, 2, 0x11, 0x90
};

typedef struct
{
    uint32_t track_id;
    uint32_t timescale;
    int video;
    uint32_t trex_duration;
    uint32_t trex_size;
    uint32_t trex_flags;
} track_def_t;

/* Write ftyp + moov with mvex/trex for the given tracks. */
static buf_t make_init(const track_def_t *tracks, size_t count)
{
    buf_t b;
    size_t moov, mvex, i;
    memset(&b, 0, sizeof(b));

    box_begin(&b, "ftyp");
    buf_bytes(&b, "mp42", 4);
    buf_u32(&b, 0);
    buf_bytes(&b, "mp42isom", 8);
    box_end(&b, 0);

    moov = b.n;
    box_begin(&b, "moov");
    {
        size_t mvhd = b.n;
        box_begin(&b, "mvhd");
        fullbox_header(&b, 0, 0);
        buf_u32(&b, 0); buf_u32(&b, 0);
        buf_u32(&b, 1000); buf_u32(&b, 0);
        buf_u32(&b, 0x00010000); buf_u16(&b, 0x0100);
        buf_u16(&b, 0); buf_u32(&b, 0); buf_u32(&b, 0);
        { int k; for (k = 0; k < 9; ++k) buf_u32(&b, k % 4 == 0 ? 0x00010000 : 0); }
        { int k; for (k = 0; k < 6; ++k) buf_u32(&b, 0); }
        buf_u32(&b, (uint32_t)(count + 1));
        box_end(&b, mvhd);
    }
    for (i = 0; i < count; ++i)
    {
        size_t trak, mdia, minf, stbl, mdhd, tkhd, stsd;
        unsigned scale = tracks[i].timescale;
        trak = b.n;
        box_begin(&b, "trak");
        {
            tkhd = b.n;
            box_begin(&b, "tkhd");
            fullbox_header(&b, 0, 7);
            buf_u32(&b, 0); buf_u32(&b, 0);
            buf_u32(&b, tracks[i].track_id); buf_u32(&b, 0);
            buf_u32(&b, 0);
            buf_u32(&b, 0); buf_u32(&b, 0);
            buf_u16(&b, 0); buf_u16(&b, 0); buf_u16(&b, 0); buf_u16(&b, 0);
            { int k; for (k = 0; k < 9; ++k) buf_u32(&b, k % 4 == 0 ? 0x00010000 : 0); }
            buf_u32(&b, tracks[i].video ? (16u << 16) : 0);
            buf_u32(&b, tracks[i].video ? (16u << 16) : 0);
            box_end(&b, tkhd);
        }
        mdia = b.n;
        box_begin(&b, "mdia");
        {
            mdhd = b.n;
            box_begin(&b, "mdhd");
            fullbox_header(&b, 0, 0);
            buf_u32(&b, 0); buf_u32(&b, 0);
            buf_u32(&b, scale); buf_u32(&b, 0);
            buf_u16(&b, 0x55c4); buf_u16(&b, 0);
            box_end(&b, mdhd);
        }
        {
            size_t hdlr = b.n;
            box_begin(&b, "hdlr");
            fullbox_header(&b, 0, 0);
            buf_u32(&b, 0);
            buf_bytes(&b, tracks[i].video ? "vide" : "soun", 4);
            buf_u32(&b, 0); buf_u32(&b, 0); buf_u32(&b, 0);
            buf_bytes(&b, "Handler", 8);
            box_end(&b, hdlr);
        }
        minf = b.n;
        box_begin(&b, "minf");
        {
            size_t dinf = b.n, dref;
            box_begin(&b, "dinf");
            dref = b.n;
            box_begin(&b, "dref");
            fullbox_header(&b, 0, 0);
            buf_u32(&b, 1);
            {
                size_t url = b.n;
                box_begin(&b, "url ");
                fullbox_header(&b, 0, 1); /* self-contained */
                box_end(&b, url);
            }
            box_end(&b, dref);
            box_end(&b, dinf);
        }
        stbl = b.n;
        box_begin(&b, "stbl");
        stsd = b.n;
        box_begin(&b, "stsd");
        fullbox_header(&b, 0, 0);
        buf_u32(&b, 1);
        if (tracks[i].video)
            buf_bytes(&b, avc1_entry, sizeof(avc1_entry));
        else
            buf_bytes(&b, mp4a_entry, sizeof(mp4a_entry));
        box_end(&b, stsd);
        {
            size_t stts = b.n;
            box_begin(&b, "stts");
            fullbox_header(&b, 0, 0);
            buf_u32(&b, 0);
            box_end(&b, stts);
            {
                size_t stsc = b.n;
                box_begin(&b, "stsc"); fullbox_header(&b, 0, 0); buf_u32(&b, 0);
                box_end(&b, stsc);
            }
            {
                size_t stsz = b.n;
                box_begin(&b, "stsz"); fullbox_header(&b, 0, 0);
                buf_u32(&b, 0); buf_u32(&b, 0);
                box_end(&b, stsz);
            }
            {
                size_t stco = b.n;
                box_begin(&b, "stco"); fullbox_header(&b, 0, 0); buf_u32(&b, 0);
                box_end(&b, stco);
            }
        }
        box_end(&b, stbl);
        box_end(&b, minf);
        box_end(&b, mdia);
        box_end(&b, trak);
    }

    mvex = b.n;
    box_begin(&b, "mvex");
    {
        size_t mehd = b.n;
        box_begin(&b, "mehd"); fullbox_header(&b, 0, 0); buf_u32(&b, 0);
        box_end(&b, mehd);
    }
    for (i = 0; i < count; ++i)
    {
        size_t trex = b.n;
        box_begin(&b, "trex");
        fullbox_header(&b, 0, 0);
        buf_u32(&b, tracks[i].track_id);
        buf_u32(&b, 1);
        buf_u32(&b, tracks[i].trex_duration);
        buf_u32(&b, tracks[i].trex_size);
        buf_u32(&b, tracks[i].trex_flags);
        box_end(&b, trex);
    }
    box_end(&b, mvex);
    box_end(&b, moov);
    return b;
}

/* ------------------------------------------------------------------ */
/* Fragment writer                                                    */
/* ------------------------------------------------------------------ */

typedef struct
{
    uint32_t duration;
    uint32_t size;
    uint32_t flags;
    int32_t cto;          /* composition offset, applied when has_cto */
    int has_cto;
    int has_duration;
    int has_size;
    int has_flags;
} rsample_t;

typedef struct
{
    uint32_t track_id;
    int has_tfdt;
    unsigned tfdt_version;
    uint64_t tfdt;
    int base_is_moof;     /* else explicit base offset (resolved later) */
    uint64_t base_offset;
    int has_default_duration, has_default_size, has_default_flags;
    uint32_t default_duration, default_size, default_flags;
    int64_t trun_data_offset; /* relative to base; applied when has_offset */
    int has_offset;
    int raw_offset;       /* trun_data_offset used verbatim, not moof-payload relative */
    int base_raw;         /* explicit base-data-offset used verbatim */
    int first_sample_flags_set;
    uint32_t first_sample_flags;
    unsigned trun_version;
    rsample_t samples[8];
    size_t count;
    /* Optional second trun in the same traf (data_offset omitted: continues). */
    int run2_count;
    int run2_has_offset;
    int run2_raw_offset;
    int64_t run2_offset;
    unsigned run2_version;
    rsample_t run2[8];
} traf_def_t;

typedef struct
{
    traf_def_t trafs[4];
    size_t count;
    int mdat_ext;         /* the following mdat uses a 64-bit size */
    uint64_t moof_file_pos; /* where the moof will be appended (for explicit bases) */
} moof_def_t;

/* Build one moof.  Unless raw_offset is set on a traf, trun_data_offset is
 * interpreted relative to the start of the following mdat's payload, which is
 * what the cases below mean by offset 0. */
static buf_t make_moof(const moof_def_t *m, uint32_t seq, int extended)
{
    buf_t b;
    size_t moof, mfhd, i, j;
    size_t patch_pos[8];
    size_t patch_traf[8];
    int patch_run[8];
    int64_t patch_val[8];
    size_t patch_n = 0;
    size_t base_pos[4];
    size_t base_traf[4];
    size_t base_n = 0;
    size_t mdat_hdr = m->mdat_ext ? 16 : 8;
    memset(&b, 0, sizeof(b));
    moof = b.n;
    if (extended)
        box_begin_ext(&b, "moof");
    else
        box_begin(&b, "moof");
    mfhd = b.n;
    box_begin(&b, "mfhd");
    fullbox_header(&b, 0, 0);
    buf_u32(&b, seq);
    box_end(&b, mfhd);

    for (i = 0; i < m->count; ++i)
    {
        const traf_def_t *t = &m->trafs[i];
        size_t traf = b.n, tfhd, trun;
        uint32_t tfhd_flags = 0;
        uint32_t trun_flags = 0;
        box_begin(&b, "traf");
        tfhd = b.n;
        box_begin(&b, "tfhd");
        if (t->base_is_moof)
            tfhd_flags |= 0x020000; /* default-base-is-moof */
        else
            tfhd_flags |= 0x000001; /* base-data-offset-present */
        if (t->has_default_duration) tfhd_flags |= 0x000008;
        if (t->has_default_size) tfhd_flags |= 0x000010;
        if (t->has_default_flags) tfhd_flags |= 0x000020;
        fullbox_header(&b, 0, tfhd_flags);
        buf_u32(&b, t->track_id);
        if (!t->base_is_moof)
        {
            base_pos[base_n] = b.n;
            base_traf[base_n] = i;
            ++base_n;
            buf_u64(&b, 0); /* patched below */
        }
        if (t->has_default_duration) buf_u32(&b, t->default_duration);
        if (t->has_default_size) buf_u32(&b, t->default_size);
        if (t->has_default_flags) buf_u32(&b, t->default_flags);
        box_end(&b, tfhd);

        if (t->has_tfdt)
        {
            size_t tfdt = b.n;
            box_begin(&b, "tfdt");
            fullbox_header(&b, t->tfdt_version, 0);
            if (t->tfdt_version == 0)
                buf_u32(&b, (uint32_t)t->tfdt);
            else
                buf_u64(&b, t->tfdt);
            box_end(&b, tfdt);
        }

        trun = b.n;
        box_begin(&b, "trun");
        if (t->has_offset) trun_flags |= 0x000001;
        if (t->first_sample_flags_set) trun_flags |= 0x000004;
        for (j = 0; j < t->count; ++j)
        {
            if (t->samples[j].has_duration) trun_flags |= 0x000100;
            if (t->samples[j].has_size) trun_flags |= 0x000200;
            if (t->samples[j].has_flags) trun_flags |= 0x000400;
            if (t->samples[j].has_cto) trun_flags |= 0x000800;
        }
        fullbox_header(&b, t->trun_version, trun_flags);
        buf_u32(&b, (uint32_t)t->count);
        if (t->has_offset)
        {
            patch_pos[patch_n] = b.n;
            patch_traf[patch_n] = i;
            patch_run[patch_n] = 0;
            patch_val[patch_n] = t->trun_data_offset;
            ++patch_n;
            buf_u32(&b, 0); /* patched once the moof size is known */
        }
        if (t->first_sample_flags_set) buf_u32(&b, t->first_sample_flags);
        for (j = 0; j < t->count; ++j)
        {
            const rsample_t *s = &t->samples[j];
            if (s->has_duration) buf_u32(&b, s->duration);
            if (s->has_size) buf_u32(&b, s->size);
            if (s->has_flags) buf_u32(&b, s->flags);
            if (s->has_cto) buf_u32(&b, (uint32_t)s->cto);
        }
        box_end(&b, trun);

        if (t->run2_count)
        {
            size_t trun2 = b.n;
            uint32_t flags2 = 0;
            box_begin(&b, "trun");
            if (t->run2_has_offset) flags2 |= 0x000001;
            for (j = 0; j < (size_t)t->run2_count; ++j)
            {
                if (t->run2[j].has_duration) flags2 |= 0x000100;
                if (t->run2[j].has_size) flags2 |= 0x000200;
                if (t->run2[j].has_flags) flags2 |= 0x000400;
                if (t->run2[j].has_cto) flags2 |= 0x000800;
            }
            fullbox_header(&b, t->run2_version, flags2);
            buf_u32(&b, (uint32_t)t->run2_count);
            if (t->run2_has_offset)
            {
                patch_pos[patch_n] = b.n;
                patch_traf[patch_n] = i;
                patch_run[patch_n] = 1;
                patch_val[patch_n] = t->run2_offset;
                ++patch_n;
                buf_u32(&b, 0);
            }
            for (j = 0; j < (size_t)t->run2_count; ++j)
            {
                const rsample_t *s = &t->run2[j];
                if (s->has_duration) buf_u32(&b, s->duration);
                if (s->has_size) buf_u32(&b, s->size);
                if (s->has_flags) buf_u32(&b, s->flags);
                if (s->has_cto) buf_u32(&b, (uint32_t)s->cto);
            }
            box_end(&b, trun2);
        }
        box_end(&b, traf);
    }
    if (extended)
    {
        box_end_ext(&b, moof);
        /* Inserting the 64-bit size field shifted everything after the
         * 8-byte header by 8 bytes; reposition the recorded patch slots. */
        for (i = 0; i < patch_n; ++i)
            patch_pos[i] += 8;
        for (i = 0; i < base_n; ++i)
            base_pos[i] += 8;
    } else
        box_end(&b, moof);
    for (i = 0; i < patch_n; ++i)
    {
        int64_t off;
        uint32_t v;
        if (patch_run[i] == 0 ? m->trafs[patch_traf[i]].raw_offset
            : m->trafs[patch_traf[i]].run2_raw_offset)
            off = patch_val[i];
        else if (m->trafs[patch_traf[i]].base_is_moof)
            off = (int64_t)b.n + (int64_t)mdat_hdr + patch_val[i];
        else /* relative to an explicit base that is patched to the payload */
            off = patch_val[i];
        v = (uint32_t)(int32_t)off;
        b.p[patch_pos[i] + 0] = (unsigned char)(v >> 24);
        b.p[patch_pos[i] + 1] = (unsigned char)(v >> 16);
        b.p[patch_pos[i] + 2] = (unsigned char)(v >> 8);
        b.p[patch_pos[i] + 3] = (unsigned char)v;
    }
    for (i = 0; i < base_n; ++i)
    {
        uint64_t base = m->trafs[base_traf[i]].base_raw
            ? m->trafs[base_traf[i]].base_offset
            : m->moof_file_pos + (uint64_t)b.n + (uint64_t)mdat_hdr +
                m->trafs[base_traf[i]].base_offset;
        size_t q = base_pos[i];
        b.p[q + 0] = (unsigned char)(base >> 56);
        b.p[q + 1] = (unsigned char)(base >> 48);
        b.p[q + 2] = (unsigned char)(base >> 40);
        b.p[q + 3] = (unsigned char)(base >> 32);
        b.p[q + 4] = (unsigned char)(base >> 24);
        b.p[q + 5] = (unsigned char)(base >> 16);
        b.p[q + 6] = (unsigned char)(base >> 8);
        b.p[q + 7] = (unsigned char)base;
    }
    return b;
}
static buf_t make_mdat(const unsigned char *payload, size_t bytes, int extended)
{
    buf_t b;
    size_t start;
    uint32_t size;
    memset(&b, 0, sizeof(b));
    start = b.n;
    if (extended)
    {
        box_begin_ext(&b, "mdat");
        buf_bytes(&b, payload, bytes);
        box_end_ext(&b, start);
    } else
    {
        box_begin(&b, "mdat");
        buf_bytes(&b, payload, bytes);
        size = (uint32_t)(b.n - start);
        b.p[start + 0] = (unsigned char)(size >> 24);
        b.p[start + 1] = (unsigned char)(size >> 16);
        b.p[start + 2] = (unsigned char)(size >> 8);
        b.p[start + 3] = (unsigned char)size;
    }
    return b;
}

/* ------------------------------------------------------------------ */
/* Collecting callback                                                */
/* ------------------------------------------------------------------ */

typedef struct
{
    unsigned char payload[64];
    size_t payload_bytes;
    MP4D_stream_sample_t meta;
} got_sample_t;

typedef struct
{
    got_sample_t got[32];
    size_t count;
    int fail_after;    /* fail when a sample with this index arrives */
    int invoked;
} sink_t;

static int sink_cb(const MP4D_stream_sample_t *s, const unsigned char *payload,
    void *token)
{
    sink_t *k = (sink_t *)token;
    size_t i;
    k->invoked = 1;
    if ((int)k->count == k->fail_after)
        return 1;
    if (k->count < sizeof(k->got) / sizeof(k->got[0]))
    {
        got_sample_t *g = &k->got[k->count];
        g->meta = *s;
        g->payload_bytes = s->size < sizeof(g->payload) ? s->size : sizeof(g->payload);
        for (i = 0; i < g->payload_bytes; ++i)
            g->payload[i] = payload[i];
    }
    ++k->count;
    return 0;
}

static int feed_all(MP4D_stream_t *st, const unsigned char *p, size_t n, size_t chunk)
{
    size_t off = 0;
    while (off < n)
    {
        size_t take = n - off < chunk ? n - off : chunk;
        if (MP4D_stream_feed(st, p + off, take) < 0)
            return -1;
        off += take;
        /* Cycle the chunk size so cuts land inside headers and payloads. */
        chunk = chunk % 17 + 1;
    }
    return 0;
}

static int feed_bytewise(MP4D_stream_t *st, const unsigned char *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i)
        if (MP4D_stream_feed(st, p + i, 1) < 0)
            return -1;
    return 0;
}

static void append(buf_t *dst, const buf_t *src)
{
    buf_bytes(dst, src->p, src->n);
}

/* ------------------------------------------------------------------ */
/* Defaults group: inheritance and unresolved samples                 */
/* ------------------------------------------------------------------ */

static int case_two_tracks_interleaved(void)
{
    static const track_def_t tracks[2] = {
        {1, 90000, 1, 1000, 4, 0x02000000},
        {2, 48000, 0, 1024, 2, 0}
    };
    buf_t init = make_init(tracks, 2), file = {0};
    moof_def_t m1, m2;
    sink_t sink;
    MP4D_stream_t *st;
    unsigned char v1[4] = {1, 2, 3, 4}, a1[2] = {9, 8};
    unsigned char v2[4] = {5, 6, 7, 8}, a2[2] = {7, 6};
    buf_t moof, mdat;

    memset(&m1, 0, sizeof(m1));
    memset(&m2, 0, sizeof(m2));
    /* Fragment 1: video sample then audio sample. */
    m1.count = 2;
    m1.trafs[0].track_id = 1; m1.trafs[0].base_is_moof = 1;
    m1.trafs[0].has_offset = 1; m1.trafs[0].trun_data_offset = 0;
    m1.trafs[0].count = 1;
    m1.trafs[0].samples[0].has_size = 1; m1.trafs[0].samples[0].size = 4;
    m1.trafs[1].track_id = 2; m1.trafs[1].base_is_moof = 1;
    m1.trafs[1].has_offset = 1; m1.trafs[1].trun_data_offset = 4;
    m1.trafs[1].count = 1;
    m1.trafs[1].samples[0].has_size = 1; m1.trafs[1].samples[0].size = 2;

    memset(&m2, 0, sizeof(m2));
    m2.count = 2;
    m2.trafs[0].track_id = 1; m2.trafs[0].base_is_moof = 1;
    m2.trafs[0].has_offset = 1; m2.trafs[0].trun_data_offset = 0;
    m2.trafs[0].count = 1;
    m2.trafs[0].samples[0].has_size = 1; m2.trafs[0].samples[0].size = 4;
    m2.trafs[1].track_id = 2; m2.trafs[1].base_is_moof = 1;
    m2.trafs[1].has_offset = 1; m2.trafs[1].trun_data_offset = 4;
    m2.trafs[1].count = 1;
    m2.trafs[1].samples[0].has_size = 1; m2.trafs[1].samples[0].size = 2;

    append(&file, &init);
    moof = make_moof(&m1, 1, 0);
    mdat = make_mdat((const unsigned char *)"\x01\x02\x03\x04\x09\x08", 6, 0);
    append(&file, &moof); append(&file, &mdat);
    free(moof.p); free(mdat.p);
    moof = make_moof(&m2, 2, 0);
    mdat = make_mdat((const unsigned char *)"\x05\x06\x07\x08\x07\x06", 6, 0);
    append(&file, &moof); append(&file, &mdat);
    free(moof.p); free(mdat.p);

    st = MP4D_stream_open(4096, sink_cb, &sink);
    TASK_CHECK(st != 0);
    memset(&sink, 0, sizeof(sink));
    sink.fail_after = -1;
    /* Re-open with the initialised sink. */
    MP4D_stream_close(st);
    st = MP4D_stream_open(4096, sink_cb, &sink);
    TASK_CHECK(st != 0);
    TASK_CHECK(feed_all(st, file.p, file.n, 13) == 0);
    TASK_CHECK(MP4D_stream_finish(st) == 0);
    TASK_CHECK(sink.count == 4);
    TASK_CHECK(sink.got[0].meta.track_id == 1);
    TASK_CHECK(sink.got[0].meta.timescale == 90000);
    TASK_CHECK(sink.got[0].meta.dts == 0);
    TASK_CHECK(sink.got[0].meta.duration == 1000);
    TASK_CHECK(sink.got[0].meta.size == 4);
    TASK_CHECK(sink.got[0].meta.flags == 0x02000000);
    TASK_CHECK(memcmp(sink.got[0].payload, v1, 4) == 0);
    TASK_CHECK(sink.got[1].meta.track_id == 2);
    TASK_CHECK(sink.got[1].meta.timescale == 48000);
    TASK_CHECK(sink.got[1].meta.duration == 1024);
    TASK_CHECK(sink.got[1].meta.dts == 0);
    TASK_CHECK(sink.got[1].meta.size == 2);
    TASK_CHECK(memcmp(sink.got[1].payload, a1, 2) == 0);
    TASK_CHECK(sink.got[2].meta.track_id == 1);
    TASK_CHECK(sink.got[2].meta.dts == 1000);
    TASK_CHECK(memcmp(sink.got[2].payload, v2, 4) == 0);
    TASK_CHECK(sink.got[3].meta.track_id == 2);
    TASK_CHECK(sink.got[3].meta.dts == 1024);
    TASK_CHECK(memcmp(sink.got[3].payload, a2, 2) == 0);
    MP4D_stream_close(st);
    free(init.p);
    free(file.p);
    return 0;
}

static int case_defaults_from_trex(void)
{
    /* No tfhd defaults, no per-sample fields: everything comes from trex. */
    static const track_def_t tracks[1] = {{7, 1000, 1, 33, 3, 0x01000000}};
    buf_t init = make_init(tracks, 1), file = {0}, moof, mdat;
    moof_def_t m;
    sink_t sink;
    MP4D_stream_t *st;

    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 7;
    m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1;
    m.trafs[0].trun_data_offset = 0;
    m.trafs[0].count = 2;
    /* Samples carry only size: duration/flags inherit from trex. */
    m.trafs[0].samples[0].has_size = 1; m.trafs[0].samples[0].size = 3;
    m.trafs[0].samples[1].has_size = 1; m.trafs[0].samples[1].size = 3;

    append(&file, &init);
    moof = make_moof(&m, 1, 0);
    mdat = make_mdat((const unsigned char *)"abcdef", 6, 0);
    append(&file, &moof); append(&file, &mdat);
    free(moof.p); free(mdat.p);

    memset(&sink, 0, sizeof(sink));
    sink.fail_after = -1;
    st = MP4D_stream_open(4096, sink_cb, &sink);
    TASK_CHECK(st != 0);
    TASK_CHECK(feed_all(st, file.p, file.n, 5) == 0);
    TASK_CHECK(MP4D_stream_finish(st) == 0);
    TASK_CHECK(sink.count == 2);
    TASK_CHECK(sink.got[0].meta.duration == 33);
    TASK_CHECK(sink.got[0].meta.flags == 0x01000000);
    TASK_CHECK(sink.got[0].meta.timescale == 1000);
    TASK_CHECK(sink.got[0].meta.track_id == 7);
    TASK_CHECK(sink.got[1].meta.dts == 33);
    TASK_CHECK(sink.got[1].meta.duration == 33);
    TASK_CHECK(sink.got[1].meta.flags == 0x01000000);
    TASK_CHECK(memcmp(sink.got[0].payload, "abc", 3) == 0);
    TASK_CHECK(memcmp(sink.got[1].payload, "def", 3) == 0);
    MP4D_stream_close(st);
    free(init.p); free(file.p);
    return 0;
}

static int case_tfhd_defaults_override_trex(void)
{
    static const track_def_t tracks[1] = {{3, 1000, 1, 10, 4, 0x0}};
    buf_t init = make_init(tracks, 1), file = {0}, moof, mdat;
    moof_def_t m;
    sink_t sink;
    MP4D_stream_t *st;

    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 3;
    m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1;
    m.trafs[0].trun_data_offset = 0;
    m.trafs[0].count = 1;
    m.trafs[0].has_default_duration = 1;
    m.trafs[0].default_duration = 25;
    m.trafs[0].has_default_flags = 1;
    m.trafs[0].default_flags = 0x02000000;
    m.trafs[0].samples[0].has_size = 1;
    m.trafs[0].samples[0].size = 4;

    append(&file, &init);
    moof = make_moof(&m, 1, 0);
    mdat = make_mdat((const unsigned char *)"wxyz", 4, 0);
    append(&file, &moof); append(&file, &mdat);
    free(moof.p); free(mdat.p);

    memset(&sink, 0, sizeof(sink));
    sink.fail_after = -1;
    st = MP4D_stream_open(4096, sink_cb, &sink);
    TASK_CHECK(st != 0);
    TASK_CHECK(feed_all(st, file.p, file.n, 17) == 0);
    TASK_CHECK(MP4D_stream_finish(st) == 0);
    TASK_CHECK(sink.count == 1);
    TASK_CHECK(sink.got[0].meta.duration == 25);
    TASK_CHECK(sink.got[0].meta.flags == 0x02000000);
    MP4D_stream_close(st);
    free(init.p); free(file.p);
    return 0;
}

static int case_first_sample_flags_only_first(void)
{
    static const track_def_t tracks[1] = {{1, 1000, 1, 10, 4, 0x02000000}};
    buf_t init = make_init(tracks, 1), file = {0}, moof, mdat;
    moof_def_t m;
    sink_t sink;
    MP4D_stream_t *st;

    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 1;
    m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1;
    m.trafs[0].trun_data_offset = 0;
    m.trafs[0].count = 3;
    m.trafs[0].first_sample_flags_set = 1;
    m.trafs[0].first_sample_flags = 0x01000000;
    m.trafs[0].samples[0].has_size = 1; m.trafs[0].samples[0].size = 2;
    m.trafs[0].samples[1].has_size = 1; m.trafs[0].samples[1].size = 2;
    m.trafs[0].samples[2].has_size = 1; m.trafs[0].samples[2].size = 2;

    append(&file, &init);
    moof = make_moof(&m, 1, 0);
    mdat = make_mdat((const unsigned char *)"aabbcc", 6, 0);
    append(&file, &moof); append(&file, &mdat);
    free(moof.p); free(mdat.p);

    memset(&sink, 0, sizeof(sink));
    sink.fail_after = -1;
    st = MP4D_stream_open(4096, sink_cb, &sink);
    TASK_CHECK(st != 0);
    TASK_CHECK(feed_all(st, file.p, file.n, 9) == 0);
    TASK_CHECK(MP4D_stream_finish(st) == 0);
    TASK_CHECK(sink.count == 3);
    TASK_CHECK(sink.got[0].meta.flags == 0x01000000);
    TASK_CHECK(sink.got[1].meta.flags == 0x02000000);
    TASK_CHECK(sink.got[2].meta.flags == 0x02000000);
    MP4D_stream_close(st);
    free(init.p); free(file.p);
    return 0;
}

static int case_zero_length_sample(void)
{
    static const track_def_t tracks[1] = {{1, 1000, 1, 10, 0, 0}};
    buf_t init = make_init(tracks, 1), file = {0}, moof, mdat;
    moof_def_t m;
    sink_t sink;
    MP4D_stream_t *st;

    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 1;
    m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1;
    m.trafs[0].trun_data_offset = 0;
    m.trafs[0].count = 2;
    m.trafs[0].samples[0].has_size = 1; m.trafs[0].samples[0].size = 0;
    m.trafs[0].samples[1].has_size = 1; m.trafs[0].samples[1].size = 2;

    append(&file, &init);
    moof = make_moof(&m, 1, 0);
    mdat = make_mdat((const unsigned char *)"\xaa\xbb", 2, 0);
    append(&file, &moof); append(&file, &mdat);
    free(moof.p); free(mdat.p);

    memset(&sink, 0, sizeof(sink));
    sink.fail_after = -1;
    st = MP4D_stream_open(4096, sink_cb, &sink);
    TASK_CHECK(st != 0);
    TASK_CHECK(feed_all(st, file.p, file.n, 3) == 0);
    TASK_CHECK(MP4D_stream_finish(st) == 0);
    TASK_CHECK(sink.count == 2);
    TASK_CHECK(sink.got[0].meta.size == 0);
    TASK_CHECK(sink.got[1].meta.size == 2);
    TASK_CHECK(memcmp(sink.got[1].payload, "\xaa\xbb", 2) == 0);
    MP4D_stream_close(st);
    free(init.p); free(file.p);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Address group: offsets, runs, ordering, gaps, extended sizes       */
/* ------------------------------------------------------------------ */

static int case_multiple_truns_continue(void)
{
    /* Two truns, second one without data_offset: continues in mdat. */
    static const track_def_t tracks[1] = {{1, 1000, 1, 10, 1, 0}};
    buf_t init = make_init(tracks, 1), file = {0}, moof, mdat;
    moof_def_t m;
    sink_t sink;
    MP4D_stream_t *st;

    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 1;
    m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1;
    m.trafs[0].trun_data_offset = 0;
    m.trafs[0].count = 2;
    m.trafs[0].samples[0].has_size = 1; m.trafs[0].samples[0].size = 1;
    m.trafs[0].samples[1].has_size = 1; m.trafs[0].samples[1].size = 2;
    /* Second trun in the same traf without data_offset: it continues at the
     * byte right after the first run. */
    m.trafs[0].run2_count = 1;
    m.trafs[0].run2_has_offset = 0;
    m.trafs[0].run2[0].has_size = 1; m.trafs[0].run2[0].size = 3;

    append(&file, &init);
    moof = make_moof(&m, 1, 0);
    mdat = make_mdat((const unsigned char *)"abcdef", 6, 0);
    append(&file, &moof); append(&file, &mdat);
    free(moof.p); free(mdat.p);

    memset(&sink, 0, sizeof(sink));
    sink.fail_after = -1;
    st = MP4D_stream_open(4096, sink_cb, &sink);
    TASK_CHECK(st != 0);
    TASK_CHECK(feed_all(st, file.p, file.n, 2) == 0);
    TASK_CHECK(MP4D_stream_finish(st) == 0);
    TASK_CHECK(sink.count == 3);
    TASK_CHECK(memcmp(sink.got[0].payload, "a", 1) == 0);
    TASK_CHECK(memcmp(sink.got[1].payload, "bc", 2) == 0);
    TASK_CHECK(memcmp(sink.got[2].payload, "def", 3) == 0);
    TASK_CHECK(sink.got[0].meta.dts == 0);
    TASK_CHECK(sink.got[1].meta.dts == 10);
    TASK_CHECK(sink.got[2].meta.dts == 20);
    MP4D_stream_close(st);
    free(init.p); free(file.p);
    return 0;
}

static int case_signed_negative_offset(void)
{
    /* Explicit base sits 4 bytes into the mdat payload and the trun's signed
     * data_offset points backwards to the payload start. */
    static const track_def_t tracks[1] = {{1, 1000, 1, 10, 2, 0}};
    buf_t init = make_init(tracks, 1), file = {0}, moof, mdat;
    moof_def_t m;
    sink_t sink;
    MP4D_stream_t *st;

    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 1;
    m.trafs[0].base_is_moof = 0;
    m.trafs[0].base_offset = 4;         /* absolute base at payload + 4 */
    m.trafs[0].base_raw = 0;
    m.trafs[0].has_offset = 1;
    m.trafs[0].trun_data_offset = -4;
    m.trafs[0].raw_offset = 1;
    m.trafs[0].count = 2;
    m.trafs[0].samples[0].has_size = 1; m.trafs[0].samples[0].size = 2;
    m.trafs[0].samples[1].has_size = 1; m.trafs[0].samples[1].size = 2;

    append(&file, &init);
    m.moof_file_pos = file.n;
    moof = make_moof(&m, 1, 0);
    mdat = make_mdat((const unsigned char *)"aabb", 4, 0);
    append(&file, &moof); append(&file, &mdat);
    free(moof.p); free(mdat.p);

    memset(&sink, 0, sizeof(sink));
    sink.fail_after = -1;
    st = MP4D_stream_open(4096, sink_cb, &sink);
    TASK_CHECK(st != 0);
    TASK_CHECK(feed_all(st, file.p, file.n, 6) == 0);
    TASK_CHECK(MP4D_stream_finish(st) == 0);
    TASK_CHECK(sink.count == 2);
    TASK_CHECK(memcmp(sink.got[0].payload, "aa", 2) == 0);
    TASK_CHECK(memcmp(sink.got[1].payload, "bb", 2) == 0);
    MP4D_stream_close(st);
    free(init.p); free(file.p);
    return 0;
}

static int case_gap_between_samples(void)
{
    /* Samples sit at offsets 0 and 4 of an 8-byte payload: the gap is skipped
     * and the callback order follows addresses, not the traf order. */
    static const track_def_t tracks[2] = {
        {1, 1000, 1, 10, 1, 0}, {2, 1000, 0, 10, 1, 0}
    };
    buf_t init = make_init(tracks, 2), file = {0}, moof, mdat;
    moof_def_t m;
    sink_t sink;
    MP4D_stream_t *st;

    memset(&m, 0, sizeof(m));
    m.count = 2;
    /* Audio declared first but stored last: address order must win. */
    m.trafs[0].track_id = 2;
    m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1;
    m.trafs[0].trun_data_offset = 4;
    m.trafs[0].count = 1;
    m.trafs[0].samples[0].has_size = 1; m.trafs[0].samples[0].size = 2;
    m.trafs[1].track_id = 1;
    m.trafs[1].base_is_moof = 1;
    m.trafs[1].has_offset = 1;
    m.trafs[1].trun_data_offset = 0;
    m.trafs[1].count = 1;
    m.trafs[1].samples[0].has_size = 1; m.trafs[1].samples[0].size = 2;

    append(&file, &init);
    moof = make_moof(&m, 1, 0);
    mdat = make_mdat((const unsigned char *)"vv\x00\x00\xaa\x00\x00\x00", 8, 0);
    append(&file, &moof); append(&file, &mdat);
    free(moof.p); free(mdat.p);

    memset(&sink, 0, sizeof(sink));
    sink.fail_after = -1;
    st = MP4D_stream_open(4096, sink_cb, &sink);
    TASK_CHECK(st != 0);
    TASK_CHECK(feed_all(st, file.p, file.n, 11) == 0);
    TASK_CHECK(MP4D_stream_finish(st) == 0);
    TASK_CHECK(sink.count == 2);
    TASK_CHECK(sink.got[0].meta.track_id == 1);
    TASK_CHECK(memcmp(sink.got[0].payload, "vv", 2) == 0);
    TASK_CHECK(sink.got[1].meta.track_id == 2);
    TASK_CHECK(memcmp(sink.got[1].payload, "\xaa\x00", 2) == 0);
    MP4D_stream_close(st);
    free(init.p); free(file.p);
    return 0;
}

static int case_extended_boxes(void)
{
    /* 64-bit moof and mdat sizes; default-base-is-moof points at the moof
     * start, so the data offset is relative to it. */
    static const track_def_t tracks[1] = {{4, 1000, 1, 10, 3, 0}};
    buf_t init = make_init(tracks, 1), file = {0};
    moof_def_t m;
    sink_t sink;
    MP4D_stream_t *st;
    buf_t moof, mdat;

    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.mdat_ext = 1;
    m.trafs[0].track_id = 4;
    m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1;
    m.trafs[0].trun_data_offset = 0;
    m.trafs[0].count = 1;
    m.trafs[0].samples[0].has_size = 1;
    m.trafs[0].samples[0].size = 3;

    append(&file, &init);
    moof = make_moof(&m, 1, 1);
    append(&file, &moof);
    mdat = make_mdat((const unsigned char *)"xyz", 3, 1);
    append(&file, &mdat);
    free(moof.p); free(mdat.p);

    memset(&sink, 0, sizeof(sink));
    sink.fail_after = -1;
    st = MP4D_stream_open(4096, sink_cb, &sink);
    TASK_CHECK(st != 0);
    TASK_CHECK(feed_all(st, file.p, file.n, 7) == 0);
    TASK_CHECK(MP4D_stream_finish(st) == 0);
    TASK_CHECK(sink.count == 1);
    TASK_CHECK(sink.got[0].meta.track_id == 4);
    TASK_CHECK(memcmp(sink.got[0].payload, "xyz", 3) == 0);
    MP4D_stream_close(st);
    free(init.p); free(file.p);
    return 0;
}

static int case_explicit_base_offset(void)
{
    /* tfhd carries an absolute base-data-offset; sample offsets are relative
     * to it. A 'free' box sits before the moof so the absolute value differs
     * from anything moof-relative. */
    static const track_def_t tracks[1] = {{2, 1000, 0, 10, 2, 0}};
    buf_t init = make_init(tracks, 1), file = {0}, moof, mdat, pre;
    moof_def_t m;
    sink_t sink;
    MP4D_stream_t *st;

    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 2;
    m.trafs[0].base_is_moof = 0;
    m.trafs[0].base_offset = 0;
    m.trafs[0].has_offset = 1;
    m.trafs[0].trun_data_offset = 0;
    m.trafs[0].count = 1;
    m.trafs[0].samples[0].has_size = 1;
    m.trafs[0].samples[0].size = 2;

    append(&file, &init);
    memset(&pre, 0, sizeof(pre));
    box_begin(&pre, "free"); buf_bytes(&pre, "\0\0\0\0", 4); box_end(&pre, 0);
    append(&file, &pre);
    free(pre.p);
    m.moof_file_pos = file.n;
    moof = make_moof(&m, 1, 0);
    append(&file, &moof);
    free(moof.p);
    mdat = make_mdat((const unsigned char *)"pq", 2, 0);
    append(&file, &mdat);
    free(mdat.p);

    memset(&sink, 0, sizeof(sink));
    sink.fail_after = -1;
    st = MP4D_stream_open(4096, sink_cb, &sink);
    TASK_CHECK(st != 0);
    TASK_CHECK(feed_all(st, file.p, file.n, 4) == 0);
    TASK_CHECK(MP4D_stream_finish(st) == 0);
    TASK_CHECK(sink.count == 1);
    TASK_CHECK(sink.got[0].meta.track_id == 2);
    TASK_CHECK(memcmp(sink.got[0].payload, "pq", 2) == 0);
    MP4D_stream_close(st);
    free(init.p); free(file.p);
    return 0;
}

static int case_unknown_boxes_skipped(void)
{
    /* A top-level 'free' box between fragments and an unknown child inside
     * moof are skipped; a 'uuid' box inside traf is skipped too. */
    static const track_def_t tracks[1] = {{1, 1000, 1, 10, 2, 0}};
    buf_t init = make_init(tracks, 1), file = {0}, moof, mdat, extra;
    moof_def_t m;
    sink_t sink;
    MP4D_stream_t *st;

    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 1;
    m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1;
    m.trafs[0].count = 1;
    m.trafs[0].samples[0].has_size = 1; m.trafs[0].samples[0].size = 2;

    append(&file, &init);
    memset(&extra, 0, sizeof(extra));
    box_begin(&extra, "free"); buf_bytes(&extra, "pad", 3); box_end(&extra, 0);
    append(&file, &extra);
    free(extra.p);
    moof = make_moof(&m, 1, 0);
    append(&file, &moof); free(moof.p);
    mdat = make_mdat((const unsigned char *)"ok", 2, 0);
    append(&file, &mdat); free(mdat.p);

    memset(&sink, 0, sizeof(sink));
    sink.fail_after = -1;
    st = MP4D_stream_open(4096, sink_cb, &sink);
    TASK_CHECK(st != 0);
    TASK_CHECK(feed_all(st, file.p, file.n, 1) == 0);
    TASK_CHECK(MP4D_stream_finish(st) == 0);
    TASK_CHECK(sink.count == 1);
    TASK_CHECK(memcmp(sink.got[0].payload, "ok", 2) == 0);
    MP4D_stream_close(st);
    free(init.p); free(file.p);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Timeline group                                                     */
/* ------------------------------------------------------------------ */

static int case_tfdt_v0_and_v1(void)
{
    static const track_def_t tracks[1] = {{1, 1000, 1, 10, 1, 0}};
    buf_t init = make_init(tracks, 1), file = {0}, moof, mdat;
    moof_def_t m;
    sink_t sink;
    MP4D_stream_t *st;

    append(&file, &init);
    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 1;
    m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1;
    m.trafs[0].count = 1;
    m.trafs[0].samples[0].has_size = 1; m.trafs[0].samples[0].size = 1;
    m.trafs[0].has_tfdt = 1;
    m.trafs[0].tfdt_version = 0;
    m.trafs[0].tfdt = 500;
    moof = make_moof(&m, 1, 0); append(&file, &moof); free(moof.p);
    mdat = make_mdat((const unsigned char *)"1", 1, 0); append(&file, &mdat); free(mdat.p);

    /* Then a v1 tfdt with a 64-bit value. */
    m.trafs[0].tfdt_version = 1;
    m.trafs[0].tfdt = 5000;
    m.trafs[0].samples[0].size = 2;
    m.trafs[0].samples[0].has_size = 1;
    m.trafs[0].has_offset = 1;
    m.trafs[0].trun_data_offset = 0;
    m.trafs[0].tfdt = 5000;
    moof = make_moof(&m, 2, 0); append(&file, &moof); free(moof.p);
    mdat = make_mdat((const unsigned char *)"23", 2, 0); append(&file, &mdat); free(mdat.p);

    memset(&sink, 0, sizeof(sink));
    sink.fail_after = -1;
    st = MP4D_stream_open(4096, sink_cb, &sink);
    TASK_CHECK(st != 0);
    TASK_CHECK(feed_all(st, file.p, file.n, 19) == 0);
    TASK_CHECK(MP4D_stream_finish(st) == 0);
    TASK_CHECK(sink.count == 2);
    TASK_CHECK(sink.got[0].meta.dts == 500);
    TASK_CHECK(sink.got[1].meta.dts == 5000);
    MP4D_stream_close(st);
    free(init.p); free(file.p);
    return 0;
}

static int case_64bit_tfdt(void)
{
    static const track_def_t tracks[1] = {{1, 90000, 1, 3000, 1, 0}};
    buf_t init = make_init(tracks, 1), file = {0}, moof, mdat;
    moof_def_t m;
    sink_t sink;
    MP4D_stream_t *st;
    const uint64_t big = 0x0000000200000001ULL;

    append(&file, &init);
    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 1;
    m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1;
    m.trafs[0].count = 1;
    m.trafs[0].samples[0].has_size = 1; m.trafs[0].samples[0].size = 1;
    m.trafs[0].has_tfdt = 1;
    m.trafs[0].tfdt_version = 1;
    m.trafs[0].tfdt = big;
    moof = make_moof(&m, 1, 0); append(&file, &moof); free(moof.p);
    mdat = make_mdat((const unsigned char *)"Z", 1, 0); append(&file, &mdat); free(mdat.p);

    memset(&sink, 0, sizeof(sink));
    sink.fail_after = -1;
    st = MP4D_stream_open(4096, sink_cb, &sink);
    TASK_CHECK(st != 0);
    TASK_CHECK(feed_all(st, file.p, file.n, 23) == 0);
    TASK_CHECK(MP4D_stream_finish(st) == 0);
    TASK_CHECK(sink.count == 1);
    TASK_CHECK(sink.got[0].meta.dts == big);
    TASK_CHECK(sink.got[0].meta.pts == (int64_t)big);
    MP4D_stream_close(st);
    free(init.p); free(file.p);
    return 0;
}

static int case_cto_unsigned_and_signed(void)
{
    /* v0 ctts offsets are unsigned (large), v1 are signed and may be negative. */
    static const track_def_t tracks[1] = {{1, 1000, 1, 10, 1, 0}};
    buf_t init = make_init(tracks, 1), file = {0}, moof, mdat;
    moof_def_t m;
    sink_t sink;
    MP4D_stream_t *st;

    append(&file, &init);
    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 1;
    m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1;
    m.trafs[0].count = 2;
    m.trafs[0].samples[0].has_size = 1; m.trafs[0].samples[0].size = 1;
    m.trafs[0].samples[1].has_size = 1; m.trafs[0].samples[1].size = 1;
    m.trafs[0].samples[0].has_cto = 1; m.trafs[0].samples[0].cto = 0x80000000u;
    m.trafs[0].samples[1].has_cto = 1; m.trafs[0].samples[1].cto = 5;
    m.trafs[0].trun_version = 0;
    m.trafs[0].trun_data_offset = 0;
    moof = make_moof(&m, 1, 0); append(&file, &moof); free(moof.p);
    mdat = make_mdat((const unsigned char *)"ab", 2, 0); append(&file, &mdat); free(mdat.p);

    m.trafs[0].trun_version = 1;
    m.trafs[0].samples[0].cto = -7;
    m.trafs[0].samples[1].cto = 3;
    moof = make_moof(&m, 2, 0); append(&file, &moof); free(moof.p);
    mdat = make_mdat((const unsigned char *)"cd", 2, 0); append(&file, &mdat); free(mdat.p);

    memset(&sink, 0, sizeof(sink));
    sink.fail_after = -1;
    st = MP4D_stream_open(4096, sink_cb, &sink);
    TASK_CHECK(st != 0);
    TASK_CHECK(feed_all(st, file.p, file.n, 29) == 0);
    TASK_CHECK(MP4D_stream_finish(st) == 0);
    TASK_CHECK(sink.count == 4);
    TASK_CHECK(sink.got[0].meta.dts == 0);
    TASK_CHECK(sink.got[0].meta.pts == (int64_t)0x80000000u);
    TASK_CHECK(sink.got[1].meta.dts == 10);
    TASK_CHECK(sink.got[1].meta.pts == 15);
    TASK_CHECK(sink.got[2].meta.dts == 20);
    TASK_CHECK(sink.got[2].meta.pts == 13);
    TASK_CHECK(sink.got[3].meta.dts == 30);
    TASK_CHECK(sink.got[3].meta.pts == 33);
    MP4D_stream_close(st);
    free(init.p); free(file.p);
    return 0;
}

static int case_timeline_continues_without_tfdt(void)
{
    static const track_def_t tracks[1] = {{1, 1000, 1, 10, 1, 0}};
    buf_t init = make_init(tracks, 1), file = {0}, moof, mdat;
    moof_def_t m;
    sink_t sink;
    MP4D_stream_t *st;
    int f;

    append(&file, &init);
    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 1;
    m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1;
    m.trafs[0].count = 1;
    m.trafs[0].samples[0].has_size = 1; m.trafs[0].samples[0].size = 1;
    for (f = 0; f < 3; ++f)
    {
        moof = make_moof(&m, (uint32_t)f + 1, 0); append(&file, &moof); free(moof.p);
        mdat = make_mdat((const unsigned char *)"x", 1, 0); append(&file, &mdat); free(mdat.p);
    }

    memset(&sink, 0, sizeof(sink));
    sink.fail_after = -1;
    st = MP4D_stream_open(4096, sink_cb, &sink);
    TASK_CHECK(st != 0);
    TASK_CHECK(feed_all(st, file.p, file.n, 37) == 0);
    TASK_CHECK(MP4D_stream_finish(st) == 0);
    TASK_CHECK(sink.count == 3);
    TASK_CHECK(sink.got[0].meta.dts == 0);
    TASK_CHECK(sink.got[1].meta.dts == 10);
    TASK_CHECK(sink.got[2].meta.dts == 20);
    MP4D_stream_close(st);
    free(init.p); free(file.p);
    return 0;
}

static int case_forward_jump_allowed(void)
{
    static const track_def_t tracks[1] = {{1, 1000, 1, 10, 1, 0}};
    buf_t init = make_init(tracks, 1), file = {0}, moof, mdat;
    moof_def_t m;
    sink_t sink;
    MP4D_stream_t *st;

    append(&file, &init);
    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 1;
    m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1;
    m.trafs[0].count = 1;
    m.trafs[0].samples[0].has_size = 1; m.trafs[0].samples[0].size = 1;

    moof = make_moof(&m, 1, 0); append(&file, &moof); free(moof.p);
    mdat = make_mdat((const unsigned char *)"a", 1, 0); append(&file, &mdat); free(mdat.p);

    m.trafs[0].has_tfdt = 1;
    m.trafs[0].tfdt_version = 0;
    m.trafs[0].tfdt = 900; /* forward jump from 10 */
    moof = make_moof(&m, 2, 0); append(&file, &moof); free(moof.p);
    mdat = make_mdat((const unsigned char *)"b", 1, 0); append(&file, &mdat); free(mdat.p);

    memset(&sink, 0, sizeof(sink));
    sink.fail_after = -1;
    st = MP4D_stream_open(4096, sink_cb, &sink);
    TASK_CHECK(st != 0);
    TASK_CHECK(feed_all(st, file.p, file.n, 3) == 0);
    TASK_CHECK(MP4D_stream_finish(st) == 0);
    TASK_CHECK(sink.count == 2);
    TASK_CHECK(sink.got[1].meta.dts == 900);
    MP4D_stream_close(st);
    free(init.p); free(file.p);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Stream group: incremental delivery, interleaving, sparse tracks    */
/* ------------------------------------------------------------------ */

static int case_a_b_a_interleave(void)
{
    /* Two tracks alternating A/B/A across fragments with a missing track in
     * the middle fragment; callback order follows media addresses. */
    static const track_def_t tracks[2] = {
        {1, 90000, 1, 3000, 1, 0}, {2, 48000, 0, 1024, 1, 0}
    };
    buf_t init = make_init(tracks, 2), file = {0}, moof, mdat;
    moof_def_t m;
    sink_t sink;
    MP4D_stream_t *st;
    static const unsigned char bits[3][2] = {{'v', '1'}, {'a', '1'}, {'v', '2'}};

    append(&file, &init);
    /* Fragment 1: video + audio. */
    memset(&m, 0, sizeof(m));
    m.count = 2;
    m.trafs[0].track_id = 1; m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1; m.trafs[0].trun_data_offset = 0;
    m.trafs[0].count = 1; m.trafs[0].samples[0].has_size = 1;
    m.trafs[0].samples[0].size = 2;
    m.trafs[1].track_id = 2; m.trafs[1].base_is_moof = 1;
    m.trafs[1].has_offset = 1; m.trafs[1].trun_data_offset = 2;
    m.trafs[1].count = 1; m.trafs[1].samples[0].has_size = 1;
    m.trafs[1].samples[0].size = 2;
    moof = make_moof(&m, 1, 0); append(&file, &moof); free(moof.p);
    mdat = make_mdat((const unsigned char *)"v1a1", 4, 0); append(&file, &mdat); free(mdat.p);
    /* Fragment 2: only audio. */
    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 2; m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1; m.trafs[0].trun_data_offset = 0;
    m.trafs[0].count = 1; m.trafs[0].samples[0].has_size = 1;
    m.trafs[0].samples[0].size = 2;
    m.trafs[0].has_tfdt = 1; m.trafs[0].tfdt = 1024;
    moof = make_moof(&m, 2, 0); append(&file, &moof); free(moof.p);
    mdat = make_mdat((const unsigned char *)"a2", 2, 0); append(&file, &mdat); free(mdat.p);
    /* Fragment 3: only video. */
    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 1; m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1; m.trafs[0].trun_data_offset = 0;
    m.trafs[0].count = 1; m.trafs[0].samples[0].has_size = 1;
    m.trafs[0].samples[0].size = 2;
    m.trafs[0].has_tfdt = 1; m.trafs[0].tfdt = 3000;
    moof = make_moof(&m, 3, 0); append(&file, &moof); free(moof.p);
    mdat = make_mdat((const unsigned char *)"v2", 2, 0); append(&file, &mdat); free(mdat.p);

    memset(&sink, 0, sizeof(sink));
    sink.fail_after = -1;
    st = MP4D_stream_open(4096, sink_cb, &sink);
    TASK_CHECK(st != 0);
    TASK_CHECK(feed_all(st, file.p, file.n, 1) == 0);
    TASK_CHECK(MP4D_stream_finish(st) == 0);
    TASK_CHECK(sink.count == 4);
    TASK_CHECK(sink.got[0].meta.track_id == 1 && sink.got[0].payload[0] == 'v');
    TASK_CHECK(sink.got[1].meta.track_id == 2 && sink.got[1].payload[0] == 'a');
    TASK_CHECK(sink.got[2].meta.track_id == 2 && sink.got[2].payload[0] == 'a');
    TASK_CHECK(sink.got[3].meta.track_id == 1 && sink.got[3].payload[0] == 'v');
    TASK_CHECK(sink.got[0].meta.dts == 0);
    TASK_CHECK(sink.got[1].meta.dts == 0);
    TASK_CHECK(sink.got[2].meta.dts == 1024);
    TASK_CHECK(sink.got[3].meta.dts == 3000);
    (void)bits;
    MP4D_stream_close(st);
    free(init.p); free(file.p);
    return 0;
}

static int case_bytewise_feed(void)
{
    static const track_def_t tracks[2] = {
        {1, 90000, 1, 3000, 3, 0}, {2, 48000, 0, 1024, 2, 0}
    };
    buf_t init = make_init(tracks, 2), file = {0}, moof, mdat;
    moof_def_t m;
    sink_t sink;
    MP4D_stream_t *st;

    append(&file, &init);
    memset(&m, 0, sizeof(m));
    m.count = 2;
    m.trafs[0].track_id = 1; m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1; m.trafs[0].trun_data_offset = 0;
    m.trafs[0].count = 2;
    m.trafs[0].samples[0].has_size = 1; m.trafs[0].samples[0].size = 3;
    m.trafs[0].samples[1].has_size = 1; m.trafs[0].samples[1].size = 3;
    m.trafs[1].track_id = 2; m.trafs[1].base_is_moof = 1;
    m.trafs[1].has_offset = 1; m.trafs[1].trun_data_offset = 6;
    m.trafs[1].count = 2;
    m.trafs[1].samples[0].has_size = 1; m.trafs[1].samples[0].size = 2;
    m.trafs[1].samples[1].has_size = 1; m.trafs[1].samples[1].size = 2;
    moof = make_moof(&m, 1, 0); append(&file, &moof); free(moof.p);
    mdat = make_mdat((const unsigned char *)"VVVWWWAAAB", 10, 0);
    append(&file, &mdat); free(mdat.p);

    memset(&sink, 0, sizeof(sink));
    sink.fail_after = -1;
    st = MP4D_stream_open(4096, sink_cb, &sink);
    TASK_CHECK(st != 0);
    TASK_CHECK(feed_bytewise(st, file.p, file.n) == 0);
    TASK_CHECK(MP4D_stream_finish(st) == 0);
    TASK_CHECK(sink.count == 4);
    TASK_CHECK(memcmp(sink.got[0].payload, "VVV", 3) == 0);
    TASK_CHECK(memcmp(sink.got[1].payload, "WWW", 3) == 0);
    TASK_CHECK(memcmp(sink.got[2].payload, "AA", 2) == 0);
    TASK_CHECK(memcmp(sink.got[3].payload, "AB", 2) == 0);
    MP4D_stream_close(st);
    free(init.p); free(file.p);
    return 0;
}

static int case_delivery_before_eof(void)
{
    /* The callback must fire on the last byte of the sample, before any
     * further feed calls (and long before finish). */
    static const track_def_t tracks[1] = {{1, 1000, 1, 10, 4, 0}};
    buf_t init = make_init(tracks, 1), file = {0}, moof, mdat;
    moof_def_t m;
    sink_t sink;
    MP4D_stream_t *st;
    size_t sample_end;

    append(&file, &init);
    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 1;
    m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1;
    m.trafs[0].count = 1;
    m.trafs[0].samples[0].has_size = 1; m.trafs[0].samples[0].size = 4;
    moof = make_moof(&m, 1, 0); append(&file, &moof); free(moof.p);
    mdat = make_mdat((const unsigned char *)"data", 4, 0); append(&file, &mdat); free(mdat.p);
    sample_end = file.n;

    memset(&sink, 0, sizeof(sink));
    sink.fail_after = -1;
    st = MP4D_stream_open(4096, sink_cb, &sink);
    TASK_CHECK(st != 0);
    TASK_CHECK(MP4D_stream_feed(st, file.p, sample_end) == 0);
    TASK_CHECK(sink.count == 1);
    /* The trailing "b" and "c" (a second mdat-like box) is never fed. */
    TASK_CHECK(MP4D_stream_finish(st) == 0);
    TASK_CHECK(MP4D_stream_finish(st) == 0);
    MP4D_stream_close(st);
    free(init.p); free(file.p);
    return 0;
}

static int case_pipe_before_eof(void)
{
    /* A real pipe: the child feeds the init + moof + one sample and then
     * blocks; the parent must observe the sample while the pipe is open. */
#ifdef _WIN32
    return 0;
#else
    static const track_def_t tracks[1] = {{1, 1000, 1, 10, 4, 0}};
    buf_t init = make_init(tracks, 1), file = {0}, moof, mdat;
    moof_def_t m;
    sink_t sink;
    int fds[2];
    pid_t pid;
    MP4D_stream_t *st;
    unsigned char tmp[64];
    ssize_t got;

    append(&file, &init);
    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 1;
    m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1;
    m.trafs[0].count = 1;
    m.trafs[0].samples[0].has_size = 1; m.trafs[0].samples[0].size = 4;
    moof = make_moof(&m, 1, 0); append(&file, &moof); free(moof.p);
    mdat = make_mdat((const unsigned char *)"PIPE", 4, 0); append(&file, &mdat); free(mdat.p);

    if (pipe(fds))
        return 1;
    pid = fork();
    if (pid < 0)
        return 1;
    if (pid == 0)
    {
        size_t off = 0;
        close(fds[0]);
        while (off < file.n)
        {
            ssize_t w = write(fds[1], file.p + off, file.n - off);
            if (w <= 0)
                _exit(2);
            off += (size_t)w;
        }
        /* Keep the write end open and idle: the parent must not need EOF. */
        sleep(5);
        _exit(0);
    }
    close(fds[1]);
    memset(&sink, 0, sizeof(sink));
    sink.fail_after = -1;
    st = MP4D_stream_open(4096, sink_cb, &sink);
    TASK_CHECK(st != 0);
    while (sink.count == 0)
    {
        got = read(fds[0], tmp, sizeof(tmp));
        if (got <= 0)
            break;
        if (MP4D_stream_feed(st, tmp, (size_t)got) < 0)
            break;
    }
    TASK_CHECK(sink.count >= 1);
    TASK_CHECK(memcmp(sink.got[0].payload, "PIPE", 4) == 0);
    MP4D_stream_close(st);
    close(fds[0]);
    kill(pid, SIGKILL);
    waitpid(pid, 0, 0);
    free(init.p); free(file.p);
    return 0;
#endif
}

static int case_close_in_any_state(void)
{
    MP4D_stream_t *st;
    static const track_def_t tracks[1] = {{1, 1000, 1, 10, 1, 0}};
    buf_t init = make_init(tracks, 1);
    TASK_CHECK(MP4D_stream_open(0, sink_cb, 0) == 0);
    TASK_CHECK(MP4D_stream_open(10, 0, 0) == 0);
    st = MP4D_stream_open(4096, sink_cb, 0);
    TASK_CHECK(st != 0);
    TASK_CHECK(MP4D_stream_feed(st, init.p, init.n / 2) == 0);
    MP4D_stream_close(st);
    st = MP4D_stream_open(4096, sink_cb, 0);
    TASK_CHECK(st != 0);
    TASK_CHECK(MP4D_stream_feed(st, init.p, init.n) == 0);
    MP4D_stream_close(st);
    free(init.p);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Failure group                                                      */
/* ------------------------------------------------------------------ */

static int expect_failure(const void *data, size_t bytes, size_t limit,
    int finish_should_fail)
{
    sink_t sink;
    MP4D_stream_t *st;
    int rc;
    memset(&sink, 0, sizeof(sink));
    sink.fail_after = -1;
    st = MP4D_stream_open(limit, sink_cb, &sink);
    if (!st)
        return 0;
    rc = feed_all(st, (const unsigned char *)data, bytes, 17);
    if (rc == 0 && finish_should_fail)
        rc = MP4D_stream_finish(st);
    MP4D_stream_close(st);
    return rc < 0 ? 0 : -1;
}

static int case_failure_cases(void)
{
    static const track_def_t tracks[1] = {{1, 1000, 1, 10, 1, 0}};
    buf_t init = make_init(tracks, 1), moof, mdat, file;
    moof_def_t m;

    /* Truncated init (half a moov): only finish can detect it. */
    TASK_CHECK(expect_failure(init.p, init.n - 5, 4096, 1) == 0);
    /* Half a header (finish must catch it). */
    TASK_CHECK(expect_failure(init.p, 3, 4096, 1) == 0);
    /* Empty input. */
    TASK_CHECK(expect_failure("", 0, 4096, 1) == 0);
    /* Zero-length box. */
    {
        unsigned char z[8] = {0, 0, 0, 0, 'f', 'r', 'e', 'e'};
        TASK_CHECK(expect_failure(z, 8, 4096, 0) == 0);
    }
    /* Oversized box. */
    {
        unsigned char z[8] = {0x7f, 0xff, 0xff, 0xff, 'm', 'o', 'o', 'v'};
        TASK_CHECK(expect_failure(z, 8, 4096, 0) == 0);
    }

    /* moof with no matching mdat (finish must fail). */
    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 1;
    m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1;
    m.trafs[0].count = 1;
    m.trafs[0].samples[0].has_size = 1; m.trafs[0].samples[0].size = 2;
    memset(&file, 0, sizeof(file));
    append(&file, &init);
    moof = make_moof(&m, 1, 0);
    append(&file, &moof);
    free(moof.p);
    TASK_CHECK(expect_failure(file.p, file.n, 4096, 1) == 0);
    free(file.p);

    /* Half a moof. */
    memset(&file, 0, sizeof(file));
    append(&file, &init);
    moof = make_moof(&m, 1, 0);
    append(&file, &moof);
    TASK_CHECK(expect_failure(file.p, file.n - 3, 4096, 1) == 0);
    free(file.p);
    free(moof.p);

    /* Half a sample: mdat claims more bytes than were fed. */
    memset(&file, 0, sizeof(file));
    append(&file, &init);
    moof = make_moof(&m, 1, 0);
    append(&file, &moof);
    mdat = make_mdat((const unsigned char *)"abc", 3, 0);
    append(&file, &mdat);
    TASK_CHECK(expect_failure(file.p, file.n - 1, 4096, 1) == 0);
    free(file.p);
    free(moof.p);
    free(mdat.p);

    /* Unknown track id. */
    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 99;
    m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1;
    m.trafs[0].count = 1;
    m.trafs[0].samples[0].has_size = 1; m.trafs[0].samples[0].size = 1;
    memset(&file, 0, sizeof(file));
    append(&file, &init);
    moof = make_moof(&m, 1, 0);
    append(&file, &moof);
    free(moof.p);
    mdat = make_mdat((const unsigned char *)"x", 1, 0);
    append(&file, &mdat);
    free(mdat.p);
    TASK_CHECK(expect_failure(file.p, file.n, 4096, 0) == 0);
    free(file.p);

    /* first_sample_flags together with per-sample flags. */
    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 1;
    m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1;
    m.trafs[0].count = 1;
    m.trafs[0].first_sample_flags_set = 1;
    m.trafs[0].samples[0].has_size = 1; m.trafs[0].samples[0].size = 1;
    m.trafs[0].samples[0].has_flags = 1; m.trafs[0].samples[0].flags = 0;
    memset(&file, 0, sizeof(file));
    append(&file, &init);
    moof = make_moof(&m, 1, 0);
    append(&file, &moof);
    free(moof.p);
    mdat = make_mdat((const unsigned char *)"x", 1, 0);
    append(&file, &mdat);
    free(mdat.p);
    TASK_CHECK(expect_failure(file.p, file.n, 4096, 0) == 0);
    free(file.p);

    /* Backwards tfdt. */
    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 1;
    m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1;
    m.trafs[0].count = 1;
    m.trafs[0].samples[0].has_size = 1; m.trafs[0].samples[0].size = 1;
    m.trafs[0].has_tfdt = 1; m.trafs[0].tfdt = 100;
    memset(&file, 0, sizeof(file));
    append(&file, &init);
    moof = make_moof(&m, 1, 0); append(&file, &moof); free(moof.p);
    mdat = make_mdat((const unsigned char *)"a", 1, 0); append(&file, &mdat); free(mdat.p);
    m.trafs[0].tfdt = 50; /* earlier than the previous fragment's end */
    moof = make_moof(&m, 2, 0); append(&file, &moof); free(moof.p);
    mdat = make_mdat((const unsigned char *)"b", 1, 0); append(&file, &mdat); free(mdat.p);
    {
        sink_t sink;
        MP4D_stream_t *st;
        int failed = 0;
        size_t off = 0;
        memset(&sink, 0, sizeof(sink));
        sink.fail_after = -1;
        st = MP4D_stream_open(4096, sink_cb, &sink);
        TASK_CHECK(st != 0);
        while (off < file.n)
        {
            size_t take = file.n - off < 5 ? file.n - off : 5;
            if (MP4D_stream_feed(st, file.p + off, take) < 0)
            {
                failed = 1;
                break;
            }
            off += take;
        }
        TASK_CHECK(failed == 1);
        TASK_CHECK(MP4D_stream_feed(st, file.p, 1) < 0);
        TASK_CHECK(MP4D_stream_finish(st) < 0);
        MP4D_stream_close(st);
        free(file.p);
    }

    /* Sample overlapping the previous one. */
    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 1;
    m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1;
    m.trafs[0].count = 2;
    m.trafs[0].samples[0].has_size = 1; m.trafs[0].samples[0].size = 4;
    m.trafs[0].samples[1].has_size = 1; m.trafs[0].samples[1].size = 4;
    memset(&file, 0, sizeof(file));
    append(&file, &init);
    moof = make_moof(&m, 1, 0); append(&file, &moof); free(moof.p);
    mdat = make_mdat((const unsigned char *)"abcd", 4, 0); append(&file, &mdat); free(mdat.p);
    TASK_CHECK(expect_failure(file.p, file.n, 4096, 0) == 0);
    free(file.p);

    /* Sample outside the mdat payload. */
    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 1;
    m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1;
    m.trafs[0].trun_data_offset = 10; /* beyond a 2 byte mdat */
    m.trafs[0].count = 1;
    m.trafs[0].samples[0].has_size = 1; m.trafs[0].samples[0].size = 1;
    memset(&file, 0, sizeof(file));
    append(&file, &init);
    moof = make_moof(&m, 1, 0); append(&file, &moof); free(moof.p);
    mdat = make_mdat((const unsigned char *)"ab", 2, 0); append(&file, &mdat); free(mdat.p);
    TASK_CHECK(expect_failure(file.p, file.n, 4096, 0) == 0);
    free(file.p);

    /* Limit smaller than one sample. */
    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 1;
    m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1;
    m.trafs[0].count = 1;
    m.trafs[0].samples[0].has_size = 1; m.trafs[0].samples[0].size = 16;
    memset(&file, 0, sizeof(file));
    append(&file, &init);
    moof = make_moof(&m, 1, 0); append(&file, &moof); free(moof.p);
    mdat = make_mdat((const unsigned char *)"0123456789abcdef", 16, 0);
    append(&file, &mdat); free(mdat.p);
    TASK_CHECK(expect_failure(file.p, file.n, 64, 0) == 0);
    free(file.p);

    free(init.p);
    return 0;
}

static int case_callback_failure_is_permanent(void)
{
    static const track_def_t tracks[1] = {{1, 1000, 1, 10, 1, 0}};
    buf_t init = make_init(tracks, 1), file = {0}, moof, mdat;
    moof_def_t m;
    sink_t sink;
    MP4D_stream_t *st;

    append(&file, &init);
    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 1;
    m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1;
    m.trafs[0].count = 2;
    m.trafs[0].samples[0].has_size = 1; m.trafs[0].samples[0].size = 1;
    m.trafs[0].samples[1].has_size = 1; m.trafs[0].samples[1].size = 1;
    moof = make_moof(&m, 1, 0); append(&file, &moof); free(moof.p);
    mdat = make_mdat((const unsigned char *)"ab", 2, 0); append(&file, &mdat); free(mdat.p);

    memset(&sink, 0, sizeof(sink));
    sink.fail_after = 0;
    st = MP4D_stream_open(4096, sink_cb, &sink);
    TASK_CHECK(st != 0);
    TASK_CHECK(feed_all(st, file.p, file.n, 4) < 0);
    TASK_CHECK(MP4D_stream_feed(st, file.p, file.n) < 0);
    TASK_CHECK(MP4D_stream_finish(st) < 0);
    TASK_CHECK(sink.count == 0);
    MP4D_stream_close(st);
    free(init.p); free(file.p);
    return 0;
}

static int case_no_samples_after_failure(void)
{
    /* Feed a good fragment, a corrupt one, then more valid data; only the
     * first fragment's sample may be delivered. */
    static const track_def_t tracks[1] = {{1, 1000, 1, 10, 1, 0}};
    buf_t init = make_init(tracks, 1), file = {0}, moof, mdat;
    moof_def_t m;
    sink_t sink;
    MP4D_stream_t *st;
    size_t split;

    append(&file, &init);
    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 1;
    m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1;
    m.trafs[0].count = 1;
    m.trafs[0].samples[0].has_size = 1; m.trafs[0].samples[0].size = 1;
    moof = make_moof(&m, 1, 0); append(&file, &moof); free(moof.p);
    mdat = make_mdat((const unsigned char *)"a", 1, 0); append(&file, &mdat); free(mdat.p);
    split = file.n;
    /* Corrupt: track id missing from the init. */
    m.trafs[0].track_id = 77;
    moof = make_moof(&m, 2, 0); append(&file, &moof); free(moof.p);
    mdat = make_mdat((const unsigned char *)"b", 1, 0); append(&file, &mdat); free(mdat.p);

    memset(&sink, 0, sizeof(sink));
    sink.fail_after = -1;
    st = MP4D_stream_open(4096, sink_cb, &sink);
    TASK_CHECK(st != 0);
    TASK_CHECK(MP4D_stream_feed(st, file.p, split) == 0);
    TASK_CHECK(sink.count == 1);
    TASK_CHECK(MP4D_stream_feed(st, file.p + split, file.n - split) < 0);
    TASK_CHECK(MP4D_stream_feed(st, file.p, file.n) < 0);
    TASK_CHECK(sink.count == 1);
    MP4D_stream_close(st);
    free(init.p); free(file.p);
    return 0;
}

static int case_feed_after_finish(void)
{
    static const track_def_t tracks[1] = {{1, 1000, 1, 10, 1, 0}};
    buf_t init = make_init(tracks, 1), file = {0}, moof, mdat;
    moof_def_t m;
    sink_t sink;
    MP4D_stream_t *st;

    append(&file, &init);
    memset(&m, 0, sizeof(m));
    m.count = 1;
    m.trafs[0].track_id = 1;
    m.trafs[0].base_is_moof = 1;
    m.trafs[0].has_offset = 1;
    m.trafs[0].count = 1;
    m.trafs[0].samples[0].has_size = 1; m.trafs[0].samples[0].size = 1;
    moof = make_moof(&m, 1, 0); append(&file, &moof); free(moof.p);
    mdat = make_mdat((const unsigned char *)"z", 1, 0); append(&file, &mdat); free(mdat.p);

    memset(&sink, 0, sizeof(sink));
    sink.fail_after = -1;
    st = MP4D_stream_open(4096, sink_cb, &sink);
    TASK_CHECK(st != 0);
    TASK_CHECK(feed_all(st, file.p, file.n, 8) == 0);
    TASK_CHECK(MP4D_stream_finish(st) == 0);
    TASK_CHECK(MP4D_stream_finish(st) == 0);
    TASK_CHECK(MP4D_stream_feed(st, 0, 0) == 0);
    TASK_CHECK(MP4D_stream_feed(st, file.p, 1) < 0);
    MP4D_stream_close(st);
    free(init.p); free(file.p);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Registration                                                       */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] =
{
    {"defaults", "two_tracks_interleaved", case_two_tracks_interleaved},
    {"defaults", "defaults_from_trex", case_defaults_from_trex},
    {"defaults", "tfhd_defaults_override_trex", case_tfhd_defaults_override_trex},
    {"defaults", "first_sample_flags_only_first", case_first_sample_flags_only_first},
    {"defaults", "zero_length_sample", case_zero_length_sample},

    {"address", "multiple_truns_continue", case_multiple_truns_continue},
    {"address", "signed_negative_offset", case_signed_negative_offset},
    {"address", "gap_between_samples", case_gap_between_samples},
    {"address", "extended_boxes", case_extended_boxes},
    {"address", "explicit_base_offset", case_explicit_base_offset},
    {"address", "unknown_boxes_skipped", case_unknown_boxes_skipped},

    {"timeline", "tfdt_v0_and_v1", case_tfdt_v0_and_v1},
    {"timeline", "64bit_tfdt", case_64bit_tfdt},
    {"timeline", "cto_unsigned_and_signed", case_cto_unsigned_and_signed},
    {"timeline", "continues_without_tfdt", case_timeline_continues_without_tfdt},
    {"timeline", "forward_jump_allowed", case_forward_jump_allowed},

    {"stream", "a_b_a_interleave", case_a_b_a_interleave},
    {"stream", "bytewise_feed", case_bytewise_feed},
    {"stream", "delivery_before_eof", case_delivery_before_eof},
    {"stream", "pipe_before_eof", case_pipe_before_eof},
    {"stream", "close_in_any_state", case_close_in_any_state},

    {"failure", "failure_cases", case_failure_cases},
    {"failure", "callback_failure_is_permanent", case_callback_failure_is_permanent},
    {"failure", "no_samples_after_failure", case_no_samples_after_failure},
    {"failure", "feed_after_finish", case_feed_after_finish}
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
