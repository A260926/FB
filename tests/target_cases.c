/*
 * Regression tests for the streaming HEVC muxer (h265_stream.c).
 *
 * The MP4 output is verified through a small, self-contained ISO box
 * walker written in this file: nothing about the expected sample table is
 * taken from the muxer's own counters. Each scenario is muxed through an
 * in-memory MP4E_mux_t in both default and sequential modes.
 */

#include "task_support/case_harness.h"
#include "h265_stream.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static uint32_t rd32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd64(const unsigned char *p)
{
    return ((uint64_t)rd32(p) << 32) | rd32(p + 4);
}

#define FCC(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))

typedef struct
{
    unsigned char *p;
    size_t n, cap;
} bb_t;

static void bb_init(bb_t *b)
{
    b->p = NULL; b->n = b->cap = 0;
}

static void bb_put(bb_t *b, const void *data, size_t n)
{
    if (b->n + n > b->cap)
    {
        size_t c = b->cap ? b->cap : 256;
        while (c < b->n + n)
            c *= 2;
        b->p = (unsigned char *)realloc(b->p, c);
        b->cap = c;
    }
    memcpy(b->p + b->n, data, n);
    b->n += n;
}

/* Append a NAL unit, with a 3- or 4-byte Annex-B start code. */
static void bb_nal(bb_t *b, int sc4, int type, int layer, int tid,
                   const unsigned char *payload, size_t plen)
{
    static const unsigned char sc3b[3] = {0, 0, 1};
    static const unsigned char sc4b[4] = {0, 0, 0, 1};
    unsigned char hdr[2];
    bb_put(b, sc4 ? sc4b : sc3b, sc4 ? 4 : 3);
    hdr[0] = (unsigned char)((type << 1) | ((layer >> 5) & 1));
    hdr[1] = (unsigned char)(((layer & 0x1f) << 3) | (tid & 7));
    bb_put(b, hdr, 2);
    bb_put(b, payload, plen);
}

/* In-memory sink for MP4E_open: supports writes at arbitrary offsets. */
typedef struct
{
    unsigned char *data;
    size_t len, cap;
} memio_t;

static int mem_write(int64_t off, const void *buf, size_t n, void *tok)
{
    memio_t *m = (memio_t *)tok;
    size_t end;
    if (off < 0 || n > (size_t)-1 - (size_t)off)
        return 1;
    end = (size_t)off + n;
    if (end > m->cap)
    {
        size_t c = m->cap ? m->cap : 4096;
        while (c < end)
            c *= 2;
        m->data = (unsigned char *)realloc(m->data, c);
        m->cap = c;
    }
    memcpy(m->data + off, buf, n);
    if (end > m->len)
        m->len = end;
    return 0;
}

/* ------------------------------------------------------------------ */
/* independent MP4 sample-table parser                                */
/* ------------------------------------------------------------------ */

#define T_MAXSAMP 1024

typedef struct
{
    unsigned sample_count;
    unsigned sizes[T_MAXSAMP];
    unsigned timestamps[T_MAXSAMP];
    unsigned durations[T_MAXSAMP];
    unsigned sync[T_MAXSAMP];          /* 0-based sample numbers */
    unsigned sync_count;
    const unsigned char *body[T_MAXSAMP];
    unsigned long total_duration;
    int have_stbl;
    int have_hvcc;
    unsigned char hvcc[512];
    size_t hvcc_len;
} table_t;

typedef struct
{
    const unsigned char *p;
    size_t n;
    uint32_t type;
} box_t;

static const unsigned char *box_first(const unsigned char *p, const unsigned char *end,
                                      uint32_t want, box_t *out, int container)
{
    while (p && p + 8 <= end)
    {
        uint64_t size = rd32(p);
        const unsigned char *head = p;
        const unsigned char *payload = p + 8;
        if (size == 1)
        {
            if (p + 16 > end)
                return NULL;
            size = rd64(p + 8);
            payload = p + 16;
        }
        else if (size == 0)
        {
            size = (uint64_t)(end - p);
        }
        if (size < (uint64_t)(payload - head) || head + size > end)
            return NULL;
        {
            uint32_t t = rd32(p + 4);
            const unsigned char *boxend = head + size;
            if (t == want)
            {
                out->p = payload;
                out->n = (size_t)(boxend - payload);
                out->type = t;
                return payload;
            }
            if (container)
            {
                const unsigned char *r = box_first(payload, boxend, want, out, 1);
                if (r)
                    return r;
            }
            p = boxend;
        }
    }
    return NULL;
}

static int mem_contains(const unsigned char *hay, size_t hn,
                        const unsigned char *needle, size_t nn)
{
    size_t i;
    if (nn > hn)
        return 0;
    for (i = 0; i + nn <= hn; i++)
        if (!memcmp(hay + i, needle, nn))
            return 1;
    return 0;
}

static void parse_table(const memio_t *m, table_t *t)
{
    box_t box;
    const unsigned char *end = m->data + m->len;
    const unsigned char *stbl, *p, *pe;
    unsigned i;
    uint64_t acc;

    memset(t, 0, sizeof(*t));
    stbl = box_first(m->data, end, FCC('s','t','b','l'), &box, 1);
    if (!stbl)
        return;
    t->have_stbl = 1;
    pe = box.p + box.n;

    /* hvcC lives under stsd/hvc1; skip stsd header and the VisualSampleEntry. */
    if (box_first(stbl, pe, FCC('s','t','s','d'), &box, 0))
    {
        const unsigned char *q = box.p + 8;            /* v/f + entry_count */
        const unsigned char *qe = box.p + box.n;
        if (q + 8 <= qe)
        {
            uint64_t esize = rd32(q);
            const unsigned char *entry_end;
            if (esize == 1 && q + 16 <= qe)
            {
                esize = rd64(q + 8);
                entry_end = q + esize;
                q += 16;
            }
            else
            {
                if (esize == 0)
                    esize = (uint64_t)(qe - q);
                entry_end = q + esize;
                q += 8;
            }
            q += 78;                                   /* VisualSampleEntry fixed fields */
            while (q + 8 <= entry_end)
            {
                uint64_t cs = rd32(q);
                const unsigned char *ce;
                if (cs == 1 && q + 16 <= entry_end) { cs = rd64(q + 8); ce = q + cs; q += 16; }
                else { if (cs == 0) cs = entry_end - q; ce = q + cs; q += 8; }
                if (ce > entry_end)
                    break;
                if (rd32(q - 4) == FCC('h','v','c','C'))
                {
                    size_t hn = (size_t)(ce - q);
                    if (hn > sizeof(t->hvcc))
                        hn = sizeof(t->hvcc);
                    memcpy(t->hvcc, q, hn);
                    t->hvcc_len = hn;
                    t->have_hvcc = 1;
                    break;
                }
                q = ce;
            }
        }
    }

    if (box_first(stbl, pe, FCC('s','t','s','z'), &box, 0))
    {
        p = box.p + 4;                                /* version/flags */
        t->sample_count = rd32(p + 4);
        if (t->sample_count > T_MAXSAMP)
            t->sample_count = T_MAXSAMP;
        if (rd32(p) == 0)
        {
            p += 8;
            for (i = 0; i < t->sample_count; i++)
                t->sizes[i] = rd32(p + 4 * i);
        }
    }

    if (box_first(stbl, pe, FCC('s','t','t','s'), &box, 0))
    {
        unsigned entries, k = 0;
        p = box.p + 4;
        entries = rd32(p);
        p += 4;
        acc = 0;
        for (i = 0; i < entries && k < T_MAXSAMP; i++)
        {
            unsigned cnt = rd32(p);
            unsigned delta = rd32(p + 4);
            unsigned j;
            p += 8;
            for (j = 0; j < cnt && k < T_MAXSAMP; j++, k++)
            {
                t->timestamps[k] = (unsigned)acc;
                t->durations[k] = delta;
                t->total_duration += delta;
                acc += delta;
            }
        }
    }

    if (box_first(stbl, pe, FCC('s','t','s','s'), &box, 0))
    {
        unsigned entries;
        p = box.p + 4;
        entries = rd32(p);
        p += 4;
        if (entries > T_MAXSAMP)
            entries = T_MAXSAMP;
        for (i = 0; i < entries; i++)
            t->sync[i] = rd32(p + 4 * i) - 1;         /* stss is 1-based */
        t->sync_count = entries;
    }
    else
    {
        for (i = 0; i < t->sample_count; i++)
            t->sync[i] = i;
        t->sync_count = t->sample_count;
    }

    if (box_first(stbl, pe, FCC('s','t','c','o'), &box, 0))
    {
        unsigned entries;
        p = box.p + 4;
        entries = rd32(p);
        p += 4;
        if (entries > T_MAXSAMP)
            entries = T_MAXSAMP;
        for (i = 0; i < entries && i < t->sample_count; i++)
            t->body[i] = m->data + rd32(p + 4 * i);
    }
    else if (box_first(stbl, pe, FCC('c','o','6','4'), &box, 0))
    {
        unsigned entries;
        p = box.p + 4;
        entries = rd32(p);
        p += 4;
        if (entries > T_MAXSAMP)
            entries = T_MAXSAMP;
        for (i = 0; i < entries && i < t->sample_count; i++)
            t->body[i] = m->data + (size_t)rd64(p + 8 * i);
    }
}

/* ------------------------------------------------------------------ */
/* muxing pipeline                                                    */
/* ------------------------------------------------------------------ */

static MP4E_mux_t *open_mem(memio_t *m, int sequential, int fragmented)
{
    memset(m, 0, sizeof(*m));
    return MP4E_open(sequential, fragmented, m, mem_write);
}

/* Feed `data` as the listed (offset,length) segments; zero lengths kept. */
static int run_segments(int sequential, const unsigned char *data,
                        const size_t *seg_off, const size_t *seg_len, size_t nseg,
                        memio_t *out, int *finish_rc)
{
    memio_t mem;
    MP4E_mux_t *mux = open_mem(&mem, sequential, 0);
    MP4H265_stream_t *st;
    int rc = MP4E_STATUS_OK;
    size_t i;
    CHECK(mux != NULL);
    st = MP4H265_stream_open(mux, 64, 48, 24000, 1001);
    CHECK(st != NULL);
    for (i = 0; i < nseg; i++)
    {
        rc = MP4H265_stream_feed(st, data + seg_off[i], seg_len[i]);
        if (rc != MP4E_STATUS_OK)
            break;
    }
    *finish_rc = MP4H265_stream_finish(st);
    if (rc == MP4E_STATUS_OK)
        rc = *finish_rc;
    MP4H265_stream_close(st);
    CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
    *out = mem;
    return rc;
}

static void tables_equal(const table_t *a, const table_t *b)
{
    unsigned i;
    CHECK(a->have_stbl == b->have_stbl);
    CHECK(a->sample_count == b->sample_count);
    CHECK(a->sync_count == b->sync_count);
    CHECK(a->total_duration == b->total_duration);
    CHECK(a->have_hvcc == b->have_hvcc);
    if (a->have_hvcc)
    {
        CHECK(a->hvcc_len == b->hvcc_len);
        CHECK(!memcmp(a->hvcc, b->hvcc, a->hvcc_len));
    }
    for (i = 0; i < a->sample_count; i++)
    {
        CHECK(a->sizes[i] == b->sizes[i]);
        CHECK(a->timestamps[i] == b->timestamps[i]);
        CHECK(a->durations[i] == b->durations[i]);
        CHECK(a->sync[i] == b->sync[i]);
        CHECK(a->body[i] && b->body[i]);
        CHECK(!memcmp(a->body[i], b->body[i], a->sizes[i]));
    }
}

/* Walk a length-prefixed sample body, returning the NAL unit types. */
static unsigned sample_nal_types(const unsigned char *body, size_t n,
                                 unsigned *types, unsigned max)
{
    unsigned cnt = 0;
    size_t pos = 0;
    while (pos + 4 <= n && cnt < max)
    {
        uint32_t ln = rd32(body + pos);
        if ((uint64_t)ln + pos + 4 > n)
            break;
        types[cnt++] = (body[pos + 4] >> 1) & 0x3f;
        pos += 4 + ln;
    }
    CHECK(pos == n);
    return cnt;
}

/* ------------------------------------------------------------------ */
/* fixtures                                                           */
/* ------------------------------------------------------------------ */

static const unsigned char cfg_vps[] = {0x40, 0x01, 0x0c, 0x01, 0xff};
static const unsigned char cfg_sps[] = {0x42, 0x01, 0x01, 0x30};
static const unsigned char cfg_pps[] = {0x44, 0x01, 0xc0, 0xf0};
static const unsigned char sl_first[] = {0x80, 0x11, 0x22};   /* first slice flag = 1 */
static const unsigned char sl_cont[]  = {0x00, 0x33, 0x44};   /* first slice flag = 0 */
static const unsigned char sei_pre[]  = {0x06, 0x05, 0xaa};
static const unsigned char sei_suf[]  = {0x06, 0x05, 0xbb};
static const unsigned char aud_pl[]   = {0x00};
/* includes emulation-prevention bytes that must survive untouched */
static const unsigned char sl_ep[]    = {0x80, 0x00, 0x00, 0x03, 0x00, 0x01, 0x02};

#define T_IDR 19
#define T_TRAIL 1
#define T_VPS 32
#define T_SPS 33
#define T_PPS 34
#define T_AUD 35
#define T_PRE 39
#define T_SUF 40

/* Two pictures, mixed start-code sizes, no AUD. */
static void build_canonical(bb_t *b)
{
    bb_nal(b, 1, T_VPS, 0, 1, cfg_vps, sizeof(cfg_vps));
    bb_nal(b, 0, T_SPS, 0, 1, cfg_sps, sizeof(cfg_sps));
    bb_nal(b, 1, T_PPS, 0, 1, cfg_pps, sizeof(cfg_pps));
    bb_nal(b, 0, T_PRE, 0, 1, sei_pre, sizeof(sei_pre));
    bb_nal(b, 1, T_IDR, 0, 1, sl_first, sizeof(sl_first));
    bb_nal(b, 0, T_IDR, 0, 1, sl_cont, sizeof(sl_cont));
    bb_nal(b, 1, T_SUF, 0, 1, sei_suf, sizeof(sei_suf));
    bb_nal(b, 0, T_TRAIL, 0, 1, sl_first, sizeof(sl_first));
    bb_nal(b, 1, T_TRAIL, 0, 1, sl_cont, sizeof(sl_cont));
    bb_nal(b, 0, T_SUF, 0, 1, sei_suf, sizeof(sei_suf));
}

static void append_picture(bb_t *b, int irap, unsigned idx)
{
    unsigned char first[4], cont[4];
    first[0] = 0x80;
    first[1] = (unsigned char)(idx & 0xff);
    first[2] = (unsigned char)((idx >> 8) & 0xff);
    first[3] = 0x55;
    cont[0] = 0x00;
    cont[1] = (unsigned char)(idx & 0xff);
    cont[2] = 0x66;
    cont[3] = 0x77;
    bb_nal(b, idx & 1, irap ? T_IDR : T_TRAIL, 0, 1, first, sizeof(first));
    bb_nal(b, !(idx & 1), irap ? T_IDR : T_TRAIL, 0, 1, cont, sizeof(cont));
}

/* ------------------------------------------------------------------ */
/* test_chunks: chunking must not change anything                     */
/* ------------------------------------------------------------------ */

void test_chunks(void)
{
    bb_t sb;
    memio_t ref_mem[2], mem;
    table_t ref_tab[2], tab;
    int frc;
    size_t one_off[1], one_len[1];
    size_t k;

    bb_init(&sb);
    build_canonical(&sb);

    /* Reference: whole buffer, one feed, per mux mode. */
    one_off[0] = 0;
    one_len[0] = sb.n;
    CASE();
    CHECK(run_segments(0, sb.p, one_off, one_len, 1, &ref_mem[0], &frc) == MP4E_STATUS_OK);
    CHECK(frc == MP4E_STATUS_OK);
    parse_table(&ref_mem[0], &ref_tab[0]);
    CHECK(ref_tab[0].have_stbl);
    CHECK(ref_tab[0].sample_count == 2);
    CHECK(ref_tab[0].sync_count == 1);
    CHECK(ref_tab[0].sync[0] == 0);

    CHECK(run_segments(1, sb.p, one_off, one_len, 1, &ref_mem[1], &frc) == MP4E_STATUS_OK);
    parse_table(&ref_mem[1], &ref_tab[1]);
    tables_equal(&ref_tab[0], &ref_tab[1]);

    /* Byte-at-a-time feeding, both modes. */
    {
        size_t *off = (size_t *)malloc(sb.n * sizeof(size_t));
        size_t *len = (size_t *)malloc(sb.n * sizeof(size_t));
        size_t i;
        for (i = 0; i < sb.n; i++) { off[i] = i; len[i] = 1; }
        CASE();
        CHECK(run_segments(0, sb.p, off, len, sb.n, &mem, &frc) == MP4E_STATUS_OK);
        parse_table(&mem, &tab);
        tables_equal(&ref_tab[0], &tab);
        CHECK(mem.len == ref_mem[0].len && !memcmp(mem.data, ref_mem[0].data, mem.len));
        free(mem.data);
        CHECK(run_segments(1, sb.p, off, len, sb.n, &mem, &frc) == MP4E_STATUS_OK);
        parse_table(&mem, &tab);
        tables_equal(&ref_tab[1], &tab);
        CHECK(mem.len == ref_mem[1].len && !memcmp(mem.data, ref_mem[1].data, mem.len));
        free(mem.data);
        free(off);
        free(len);
    }

    /* Every single split position k: [0,k) then [k,n), both modes. */
    CASE();
    for (k = 1; k < sb.n; k++)
    {
        size_t off2[2] = {0, k};
        size_t len2[2] = {k, sb.n - k};
        CHECK(run_segments(0, sb.p, off2, len2, 2, &mem, &frc) == MP4E_STATUS_OK);
        parse_table(&mem, &tab);
        tables_equal(&ref_tab[0], &tab);
        CHECK(mem.len == ref_mem[0].len && !memcmp(mem.data, ref_mem[0].data, mem.len));
        free(mem.data);
        CHECK(run_segments(1, sb.p, off2, len2, 2, &mem, &frc) == MP4E_STATUS_OK);
        parse_table(&mem, &tab);
        tables_equal(&ref_tab[1], &tab);
        free(mem.data);
    }

    /* Fixed-size chunks of several strides, plus empty feeds sprinkled in. */
    {
        static const size_t strides[] = {2, 3, 5, 7, 13, 31, 64};
        size_t si;
        CASE();
        for (si = 0; si < sizeof(strides) / sizeof(strides[0]); si++)
        {
            size_t stride = strides[si];
            size_t cap = (sb.n + stride - 1) / stride * 2 + 2;
            size_t *off = (size_t *)malloc(cap * sizeof(size_t));
            size_t *len = (size_t *)malloc(cap * sizeof(size_t));
            size_t pos = 0, cnt = 0, mode;
            off[cnt] = pos; len[cnt] = 0; cnt++;            /* leading empty feed */
            while (pos < sb.n)
            {
                size_t c = sb.n - pos < stride ? sb.n - pos : stride;
                off[cnt] = pos;
                len[cnt] = c;
                cnt++;
                pos += c;
                off[cnt] = pos; len[cnt] = 0; cnt++;        /* trailing empty feed */
            }
            for (mode = 0; mode < 2; mode++)
            {
                CHECK(run_segments((int)mode, sb.p, off, len, cnt, &mem, &frc) == MP4E_STATUS_OK);
                parse_table(&mem, &tab);
                tables_equal(&ref_tab[mode], &tab);
                free(mem.data);
            }
            free(off);
            free(len);
        }
    }

    free(ref_mem[0].data);
    free(ref_mem[1].data);
    free(sb.p);
}

/* ------------------------------------------------------------------ */
/* test_access_units: AU assembly, SEI, AUD, parameter sets           */
/* ------------------------------------------------------------------ */

void test_access_units(void)
{
    bb_t sb;
    memio_t mem;
    table_t tab;
    int frc;
    size_t off[1], len[1];
    unsigned types[16], cnt;

    /* prefix SEI + IDR(2 slices) + suffix SEI, then AUD + second picture. */
    bb_init(&sb);
    bb_nal(&sb, 1, T_VPS, 0, 1, cfg_vps, sizeof(cfg_vps));
    bb_nal(&sb, 1, T_SPS, 0, 1, cfg_sps, sizeof(cfg_sps));
    bb_nal(&sb, 1, T_PPS, 0, 1, cfg_pps, sizeof(cfg_pps));
    bb_nal(&sb, 1, T_PRE, 0, 1, sei_pre, sizeof(sei_pre));
    bb_nal(&sb, 1, T_IDR, 0, 1, sl_first, sizeof(sl_first));
    bb_nal(&sb, 1, T_IDR, 0, 1, sl_cont, sizeof(sl_cont));
    bb_nal(&sb, 1, T_SUF, 0, 1, sei_suf, sizeof(sei_suf));
    bb_nal(&sb, 1, T_AUD, 0, 1, aud_pl, sizeof(aud_pl));
    bb_nal(&sb, 1, T_TRAIL, 0, 1, sl_first, sizeof(sl_first));
    bb_nal(&sb, 1, T_TRAIL, 0, 1, sl_cont, sizeof(sl_cont));
    off[0] = 0; len[0] = sb.n;

    CASE();
    CHECK(run_segments(0, sb.p, off, len, 1, &mem, &frc) == MP4E_STATUS_OK);
    parse_table(&mem, &tab);
    CHECK(tab.sample_count == 2);

    /* Picture 0: prefix SEI, two slices, suffix SEI, in that order. */
    cnt = sample_nal_types(tab.body[0], tab.sizes[0], types, 16);
    CHECK(cnt == 4);
    CHECK(types[0] == T_PRE);
    CHECK(types[1] == T_IDR);
    CHECK(types[2] == T_IDR);
    CHECK(types[3] == T_SUF);

    /* Picture 1: two slices only; the AUD never enters a sample. */
    cnt = sample_nal_types(tab.body[1], tab.sizes[1], types, 16);
    CHECK(cnt == 2);
    CHECK(types[0] == T_TRAIL);
    CHECK(types[1] == T_TRAIL);
    for (cnt = 0; cnt < tab.sample_count; cnt++)
    {
        size_t p;
        for (p = 0; p + 4 <= tab.sizes[cnt]; )
        {
            uint32_t ln = rd32(tab.body[cnt] + p);
            unsigned ty = (tab.body[cnt][p + 4] >> 1) & 0x3f;
            CHECK(ty != T_AUD);
            CHECK(ty != T_VPS && ty != T_SPS && ty != T_PPS);
            p += 4 + ln;
        }
    }

    /* Parameter sets land in hvcC, with their payload bytes intact. */
    CHECK(tab.have_hvcc);
    CHECK(mem_contains(tab.hvcc, tab.hvcc_len, cfg_vps, sizeof(cfg_vps)));
    CHECK(mem_contains(tab.hvcc, tab.hvcc_len, cfg_sps, sizeof(cfg_sps)));
    CHECK(mem_contains(tab.hvcc, tab.hvcc_len, cfg_pps, sizeof(cfg_pps)));
    free(mem.data);
    free(sb.p);

    /* Emulation-prevention bytes are copied verbatim into the sample. */
    bb_init(&sb);
    bb_nal(&sb, 1, T_VPS, 0, 1, cfg_vps, sizeof(cfg_vps));
    bb_nal(&sb, 1, T_SPS, 0, 1, cfg_sps, sizeof(cfg_sps));
    bb_nal(&sb, 1, T_PPS, 0, 1, cfg_pps, sizeof(cfg_pps));
    bb_nal(&sb, 1, T_IDR, 0, 1, sl_ep, sizeof(sl_ep));
    off[0] = 0; len[0] = sb.n;
    CASE();
    CHECK(run_segments(0, sb.p, off, len, 1, &mem, &frc) == MP4E_STATUS_OK);
    parse_table(&mem, &tab);
    CHECK(tab.sample_count == 1);
    CHECK(tab.sizes[0] == 4 + 2 + sizeof(sl_ep));
    CHECK(!memcmp(tab.body[0] + 4, sb.p + (sb.n - (2 + sizeof(sl_ep))), 2 + sizeof(sl_ep)));
    free(mem.data);
    free(sb.p);

    /* Prefix-only stream (SEI, no VCL) finishes with zero samples. */
    bb_init(&sb);
    bb_nal(&sb, 1, T_VPS, 0, 1, cfg_vps, sizeof(cfg_vps));
    bb_nal(&sb, 1, T_SPS, 0, 1, cfg_sps, sizeof(cfg_sps));
    bb_nal(&sb, 1, T_PPS, 0, 1, cfg_pps, sizeof(cfg_pps));
    bb_nal(&sb, 1, T_PRE, 0, 1, sei_pre, sizeof(sei_pre));
    off[0] = 0; len[0] = sb.n;
    CASE();
    CHECK(run_segments(0, sb.p, off, len, 1, &mem, &frc) == MP4E_STATUS_OK);
    parse_table(&mem, &tab);
    CHECK(!tab.have_stbl || tab.sample_count == 0);
    free(mem.data);
    free(sb.p);

    /* A prefix SEI before the first VCL attaches to the first picture. */
    bb_init(&sb);
    bb_nal(&sb, 1, T_VPS, 0, 1, cfg_vps, sizeof(cfg_vps));
    bb_nal(&sb, 1, T_SPS, 0, 1, cfg_sps, sizeof(cfg_sps));
    bb_nal(&sb, 1, T_PPS, 0, 1, cfg_pps, sizeof(cfg_pps));
    bb_nal(&sb, 1, T_PRE, 0, 1, sei_pre, sizeof(sei_pre));
    bb_nal(&sb, 1, T_IDR, 0, 1, sl_first, sizeof(sl_first));
    off[0] = 0; len[0] = sb.n;
    CASE();
    CHECK(run_segments(0, sb.p, off, len, 1, &mem, &frc) == MP4E_STATUS_OK);
    parse_table(&mem, &tab);
    CHECK(tab.sample_count == 1);
    cnt = sample_nal_types(tab.body[0], tab.sizes[0], types, 16);
    CHECK(cnt == 2);
    CHECK(types[0] == T_PRE);
    CHECK(types[1] == T_IDR);
    free(mem.data);
    free(sb.p);

    /* A new prefix SEI (no AUD) ends the previous picture and starts the next. */
    bb_init(&sb);
    bb_nal(&sb, 1, T_VPS, 0, 1, cfg_vps, sizeof(cfg_vps));
    bb_nal(&sb, 1, T_SPS, 0, 1, cfg_sps, sizeof(cfg_sps));
    bb_nal(&sb, 1, T_PPS, 0, 1, cfg_pps, sizeof(cfg_pps));
    bb_nal(&sb, 1, T_IDR, 0, 1, sl_first, sizeof(sl_first));
    bb_nal(&sb, 1, T_PRE, 0, 1, sei_pre, sizeof(sei_pre));
    bb_nal(&sb, 1, T_TRAIL, 0, 1, sl_first, sizeof(sl_first));
    off[0] = 0; len[0] = sb.n;
    CASE();
    CHECK(run_segments(0, sb.p, off, len, 1, &mem, &frc) == MP4E_STATUS_OK);
    parse_table(&mem, &tab);
    CHECK(tab.sample_count == 2);
    cnt = sample_nal_types(tab.body[0], tab.sizes[0], types, 16);
    CHECK(cnt == 1);
    CHECK(types[0] == T_IDR);
    cnt = sample_nal_types(tab.body[1], tab.sizes[1], types, 16);
    CHECK(cnt == 2);
    CHECK(types[0] == T_PRE);
    CHECK(types[1] == T_TRAIL);
    CHECK(tab.sync_count == 1);
    CHECK(tab.sync[0] == 0);
    free(mem.data);
    free(sb.p);
}

/* ------------------------------------------------------------------ */
/* test_clock: timing rule and mode-agnostic durations                */
/* ------------------------------------------------------------------ */

static uint64_t clock_start(unsigned long n, int num, int den)
{
    return (uint64_t)n * 90000ULL * (uint64_t)den / (uint64_t)num;
}

static void run_custom_fps(int sequential, int num, int den, unsigned frames,
                           memio_t *out, int *frc)
{
    bb_t sb;
    memio_t mem;
    MP4E_mux_t *mux;
    MP4H265_stream_t *st;
    size_t off[1], len[1];
    unsigned i;
    bb_init(&sb);
    bb_nal(&sb, 1, T_VPS, 0, 1, cfg_vps, sizeof(cfg_vps));
    bb_nal(&sb, 1, T_SPS, 0, 1, cfg_sps, sizeof(cfg_sps));
    bb_nal(&sb, 1, T_PPS, 0, 1, cfg_pps, sizeof(cfg_pps));
    for (i = 0; i < frames; i++)
        append_picture(&sb, i == 0, i);
    mux = open_mem(&mem, sequential, 0);
    CHECK(mux != NULL);
    st = MP4H265_stream_open(mux, 64, 48, num, den);
    CHECK(st != NULL);
    off[0] = 0;
    len[0] = sb.n;
    CHECK(MP4H265_stream_feed(st, sb.p, sb.n) == MP4E_STATUS_OK);
    *frc = MP4H265_stream_finish(st);
    CHECK(*frc == MP4E_STATUS_OK);
    MP4H265_stream_close(st);
    CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
    free(sb.p);
    *out = mem;
}

void test_clock(void)
{
    enum { NF = 300 };
    memio_t mem_d, mem_s;
    table_t td, ts;
    unsigned i;
    int frc;
    int saw_3753 = 0, saw_3754 = 0;

    CASE();
    run_custom_fps(0, 24000, 1001, NF, &mem_d, &frc);
    run_custom_fps(1, 24000, 1001, NF, &mem_s, &frc);
    parse_table(&mem_d, &td);
    parse_table(&mem_s, &ts);

    CHECK(td.sample_count == NF);
    CHECK(ts.sample_count == NF);
    tables_equal(&td, &ts);

    for (i = 0; i < NF; i++)
    {
        uint64_t ts0 = clock_start(i, 24000, 1001);
        uint64_t ts1 = clock_start(i + 1, 24000, 1001);
        CHECK(td.timestamps[i] == (unsigned)ts0);
        CHECK(td.durations[i] == (unsigned)(ts1 - ts0));
        if (td.durations[i] == 3753) saw_3753 = 1;
        if (td.durations[i] == 3754) saw_3754 = 1;
    }
    CHECK(saw_3753 && saw_3754);                 /* no per-frame rounding accumulation */
    CHECK(td.timestamps[0] == 0);
    CHECK(td.total_duration == (unsigned long)clock_start(NF, 24000, 1001));
    CHECK(td.total_duration == ts.total_duration);
    free(mem_d.data);
    free(mem_s.data);

    /* Integer frame rate gives a constant duration. */
    CASE();
    run_custom_fps(0, 30, 1, 10, &mem_d, &frc);
    parse_table(&mem_d, &td);
    CHECK(td.sample_count == 10);
    for (i = 0; i < 10; i++)
        CHECK(td.durations[i] == 3000);
    CHECK(td.total_duration == 30000);
    free(mem_d.data);

    /* FPS validation at open(). */
    {
        memio_t mem;
        MP4E_mux_t *mux = open_mem(&mem, 0, 0);
        MP4H265_stream_t *st;
        CASE();
        CHECK(mux != NULL);
        st = MP4H265_stream_open(mux, 64, 48, 0, 1001);
        CHECK(st == NULL);
        st = MP4H265_stream_open(mux, 64, 48, 24000, 0);
        CHECK(st == NULL);
        st = MP4H265_stream_open(mux, 64, 48, 100000, 1);   /* step rounds to zero */
        CHECK(st == NULL);
        st = MP4H265_stream_open(mux, 64, 48, 1, 1000000); /* nominal step > INT_MAX */
        CHECK(st == NULL);
        st = MP4H265_stream_open(mux, 0, 48, 30, 1);       /* bad width */
        CHECK(st == NULL);
        CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
        free(mem.data);
    }
}

/* ------------------------------------------------------------------ */
/* test_invalid: every rejection path and dead-instance behaviour     */
/* ------------------------------------------------------------------ */

/* Expect the pictured NAL sequence to fail through feed and/or finish.
 * A NAL that is the last one in the buffer cannot be validated until EOF,
 * so the error may legitimately surface at either call. */
static void expect_fail(const bb_t *sb)
{
    memio_t mem;
    MP4E_mux_t *mux = open_mem(&mem, 0, 0);
    MP4H265_stream_t *st;
    int rc, frc;
    CHECK(mux != NULL);
    st = MP4H265_stream_open(mux, 64, 48, 30, 1);
    CHECK(st != NULL);
    rc = MP4H265_stream_feed(st, sb->p, sb->n);
    frc = MP4H265_stream_finish(st);
    CHECK(rc != MP4E_STATUS_OK || frc != MP4E_STATUS_OK);
    /* A failed instance stays dead. */
    CHECK(MP4H265_stream_feed(st, sb->p, sb->n ? 1 : 0) != MP4E_STATUS_OK);
    CHECK(MP4H265_stream_finish(st) != MP4E_STATUS_OK);
    MP4H265_stream_close(st);
    CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
    free(mem.data);
}

void test_invalid(void)
{
    bb_t sb;

    /* Truncated NAL header at EOF. */
    bb_init(&sb);
    bb_nal(&sb, 1, T_VPS, 0, 1, cfg_vps, sizeof(cfg_vps));
    bb_nal(&sb, 1, T_SPS, 0, 1, cfg_sps, sizeof(cfg_sps));
    bb_nal(&sb, 1, T_PPS, 0, 1, cfg_pps, sizeof(cfg_pps));
    {
        static const unsigned char partial[] = {0, 0, 0, 1, 0x26};  /* one header byte */
        bb_put(&sb, partial, sizeof(partial));
    }
    CASE();
    expect_fail(&sb);
    free(sb.p);

    /* Continuation slice with no first slice. */
    bb_init(&sb);
    bb_nal(&sb, 1, T_VPS, 0, 1, cfg_vps, sizeof(cfg_vps));
    bb_nal(&sb, 1, T_SPS, 0, 1, cfg_sps, sizeof(cfg_sps));
    bb_nal(&sb, 1, T_PPS, 0, 1, cfg_pps, sizeof(cfg_pps));
    bb_nal(&sb, 1, T_IDR, 0, 1, sl_cont, sizeof(sl_cont));
    CASE();
    expect_fail(&sb);
    free(sb.p);

    /* First VCL is not an IRAP. */
    bb_init(&sb);
    bb_nal(&sb, 1, T_VPS, 0, 1, cfg_vps, sizeof(cfg_vps));
    bb_nal(&sb, 1, T_SPS, 0, 1, cfg_sps, sizeof(cfg_sps));
    bb_nal(&sb, 1, T_PPS, 0, 1, cfg_pps, sizeof(cfg_pps));
    bb_nal(&sb, 1, T_TRAIL, 0, 1, sl_first, sizeof(sl_first));
    CASE();
    expect_fail(&sb);
    free(sb.p);

    /* Incomplete configuration: missing PPS at the first VCL. */
    bb_init(&sb);
    bb_nal(&sb, 1, T_VPS, 0, 1, cfg_vps, sizeof(cfg_vps));
    bb_nal(&sb, 1, T_SPS, 0, 1, cfg_sps, sizeof(cfg_sps));
    bb_nal(&sb, 1, T_IDR, 0, 1, sl_first, sizeof(sl_first));
    CASE();
    expect_fail(&sb);
    free(sb.p);

    /* Parameter set appearing after the first VCL. */
    bb_init(&sb);
    bb_nal(&sb, 1, T_VPS, 0, 1, cfg_vps, sizeof(cfg_vps));
    bb_nal(&sb, 1, T_SPS, 0, 1, cfg_sps, sizeof(cfg_sps));
    bb_nal(&sb, 1, T_PPS, 0, 1, cfg_pps, sizeof(cfg_pps));
    bb_nal(&sb, 1, T_IDR, 0, 1, sl_first, sizeof(sl_first));
    bb_nal(&sb, 1, T_SPS, 0, 1, cfg_sps, sizeof(cfg_sps));
    CASE();
    expect_fail(&sb);
    free(sb.p);

    /* Non-base layer NAL. */
    bb_init(&sb);
    bb_nal(&sb, 1, T_VPS, 0, 1, cfg_vps, sizeof(cfg_vps));
    bb_nal(&sb, 1, T_SPS, 0, 1, cfg_sps, sizeof(cfg_sps));
    bb_nal(&sb, 1, T_PPS, 0, 1, cfg_pps, sizeof(cfg_pps));
    bb_nal(&sb, 1, T_IDR, 1, 1, sl_first, sizeof(sl_first));  /* layer_id = 1 */
    CASE();
    expect_fail(&sb);
    free(sb.p);

    /* temporal_id_plus1 != 1. */
    bb_init(&sb);
    bb_nal(&sb, 1, T_VPS, 0, 1, cfg_vps, sizeof(cfg_vps));
    bb_nal(&sb, 1, T_SPS, 0, 1, cfg_sps, sizeof(cfg_sps));
    bb_nal(&sb, 1, T_PPS, 0, 1, cfg_pps, sizeof(cfg_pps));
    bb_nal(&sb, 1, T_IDR, 0, 2, sl_first, sizeof(sl_first));  /* tid field = 2 */
    CASE();
    expect_fail(&sb);
    free(sb.p);

    /* Suffix SEI with no open picture. */
    bb_init(&sb);
    bb_nal(&sb, 1, T_VPS, 0, 1, cfg_vps, sizeof(cfg_vps));
    bb_nal(&sb, 1, T_SPS, 0, 1, cfg_sps, sizeof(cfg_sps));
    bb_nal(&sb, 1, T_PPS, 0, 1, cfg_pps, sizeof(cfg_pps));
    bb_nal(&sb, 1, T_SUF, 0, 1, sei_suf, sizeof(sei_suf));
    CASE();
    expect_fail(&sb);
    free(sb.p);

    /* finish() is idempotent; feeding afterwards fails. */
    {
        memio_t mem;
        MP4E_mux_t *mux = open_mem(&mem, 0, 0);
        MP4H265_stream_t *st;
        bb_t ok;
        size_t off[1], len[1];
        int rc1, rc2;
        bb_init(&ok);
        bb_nal(&ok, 1, T_VPS, 0, 1, cfg_vps, sizeof(cfg_vps));
        bb_nal(&ok, 1, T_SPS, 0, 1, cfg_sps, sizeof(cfg_sps));
        bb_nal(&ok, 1, T_PPS, 0, 1, cfg_pps, sizeof(cfg_pps));
        bb_nal(&ok, 1, T_IDR, 0, 1, sl_first, sizeof(sl_first));
        CASE();
        st = MP4H265_stream_open(mux, 64, 48, 30, 1);
        CHECK(st != NULL);
        off[0] = 0; len[0] = ok.n;
        CHECK(MP4H265_stream_feed(st, ok.p, ok.n) == MP4E_STATUS_OK);
        rc1 = MP4H265_stream_finish(st);
        rc2 = MP4H265_stream_finish(st);
        CHECK(rc1 == MP4E_STATUS_OK);
        CHECK(rc2 == MP4E_STATUS_OK);
        CHECK(MP4H265_stream_feed(st, ok.p, 1) != MP4E_STATUS_OK);
        MP4H265_stream_close(st);
        CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
        {
            table_t tab;
            parse_table(&mem, &tab);
            CHECK(tab.sample_count == 1);   /* repeated finish wrote no extra frame */
        }
        free(mem.data);
        free(ok.p);
    }

    /* Empty stream finishes cleanly and writes no frame. */
    {
        memio_t mem;
        MP4E_mux_t *mux = open_mem(&mem, 0, 0);
        MP4H265_stream_t *st = MP4H265_stream_open(mux, 64, 48, 30, 1);
        table_t tab;
        CASE();
        CHECK(st != NULL);
        CHECK(MP4H265_stream_feed(st, NULL, 0) == MP4E_STATUS_OK);
        CHECK(MP4H265_stream_finish(st) == MP4E_STATUS_OK);
        MP4H265_stream_close(st);
        CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
        parse_table(&mem, &tab);
        CHECK(!tab.have_stbl || tab.sample_count == 0);
        free(mem.data);
    }

    /* Fragmented muxers are rejected up front. */
    {
        memio_t mem;
        MP4E_mux_t *mux = MP4E_open(0, 1, &mem, mem_write);
        MP4H265_stream_t *st;
        CASE();
        CHECK(mux != NULL);
        st = MP4H265_stream_open(mux, 64, 48, 30, 1);
        CHECK(st == NULL);
        CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
        free(mem.data);
    }
}
