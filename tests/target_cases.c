/*
    Regression tests for the streaming HEVC Annex-B -> MP4 writer
    (h265_stream.h / h265_stream.c).  Built and run by task_support/target.sh.
*/
#include "task_support/case_harness.h"
#include "h265_stream.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/************************************************************************/
/*          helpers                                                     */
/************************************************************************/

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    CHECK(p != NULL);
    return p;
}

/* memory-backed output file for the muxer */
typedef struct
{
    unsigned char *data;
    size_t size, cap;
} memfile_t;

static int mem_write(int64_t off, const void *p, size_t n, void *token)
{
    memfile_t *m = (memfile_t *)token;
    if (off < 0)
        return 1;
    if ((uint64_t)off + n > m->cap)
    {
        size_t cap = m->cap ? m->cap : 1024;
        unsigned char *q;
        while ((uint64_t)off + n > cap)
            cap *= 2;
        q = (unsigned char *)realloc(m->data, cap);
        if (!q)
            return 1;
        m->data = q;
        m->cap = cap;
    }
    memcpy(m->data + off, p, n);
    if ((size_t)off + n > m->size)
        m->size = (size_t)off + n;
    return 0;
}

static int mem_read(int64_t off, void *p, size_t n, void *token)
{
    memfile_t *m = (memfile_t *)token;
    if (off < 0 || (uint64_t)off + n > m->size)
        return 1;
    memcpy(p, m->data + off, n);
    return 0;
}

/* growable byte buffer */
typedef struct
{
    unsigned char *data;
    size_t len, cap;
} buf_t;

static void bput(buf_t *b, const void *p, size_t n)
{
    if (b->len + n > b->cap)
    {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n)
            cap *= 2;
        b->data = (unsigned char *)realloc(b->data, cap);
        CHECK(b->data != NULL);
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

/* append one Annex-B NAL unit (4-byte start code + payload) */
static void bnal(buf_t *b, const unsigned char *nal, size_t n)
{
    static const unsigned char sc[4] = {0, 0, 0, 1};
    bput(b, sc, 4);
    bput(b, nal, n);
}

/* append one NAL unit in MP4 sample format (BE32 length + payload) */
static void au_nal(buf_t *b, const unsigned char *nal, size_t n)
{
    unsigned char hdr[4];
    hdr[0] = (unsigned char)(n >> 24);
    hdr[1] = (unsigned char)(n >> 16);
    hdr[2] = (unsigned char)(n >> 8);
    hdr[3] = (unsigned char)(n);
    bput(b, hdr, 4);
    bput(b, nal, n);
}

/************************************************************************/
/*          NAL fixtures                                                */
/*                                                                      */
/*  Two-byte headers are (type << 1, 0x01): layer 0, temporal id 0.     */
/*  Several payloads carry emulation-prevention bytes (00 00 03 xx)     */
/*  which must survive into the samples unmodified.                     */
/************************************************************************/

static const unsigned char NAL_VPS[]  = {0x40, 0x01, 0x0C, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x11, 0x22};
static const unsigned char NAL_SPS[]  = {0x42, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x33, 0x44, 0x55};
static const unsigned char NAL_PPS[]  = {0x44, 0x01, 0xC0, 0x66, 0x77};
static const unsigned char NAL_AUD[]  = {0x46, 0x01, 0x50};
static const unsigned char NAL_IDR1[] = {0x26, 0x01, 0x88, 0x11, 0x00, 0x00, 0x03, 0x01, 0xAA}; /* IDR, first slice */
static const unsigned char NAL_IDR2[] = {0x26, 0x01, 0x08, 0x22, 0xBB};                        /* IDR, continuation */
static const unsigned char NAL_SSEI[] = {0x50, 0x01, 0x04, 0x99, 0x88, 0x77};                  /* suffix SEI */
static const unsigned char NAL_PSEI[] = {0x4E, 0x01, 0x05, 0x66, 0x00, 0x00, 0x03, 0x00, 0x55};/* prefix SEI */
static const unsigned char NAL_TR1[]  = {0x02, 0x01, 0x96, 0x33, 0x44};                        /* TRAIL_R, first slice */
static const unsigned char NAL_TR2[]  = {0x02, 0x01, 0x06, 0x55, 0x00, 0x00, 0x03, 0x03, 0x66};/* TRAIL_R, continuation */
static const unsigned char NAL_IDR3[] = {0x26, 0x01, 0x84, 0x77};                              /* IDR, single slice */
static const unsigned char NAL_TR4[]  = {0x02, 0x01, 0x92, 0x88};                              /* TRAIL_R, first slice */

#define NAL_ARRAY(x) x, sizeof(x)

/*
    Four pictures:
      0: IDR1 IDR2 SSEI        (random access)
      1: PSEI TR1 TR2          (prefix SEI belongs to this picture)
      2: IDR3                  (random access)
      3: TR4                   (closed by the trailing prefix SEI / EOF)
    The final PSEI has no following VCL and must not produce a frame.
*/
static void build_stream(buf_t *b, int with_aud)
{
    bnal(b, NAL_ARRAY(NAL_VPS));
    bnal(b, NAL_ARRAY(NAL_SPS));
    bnal(b, NAL_ARRAY(NAL_PPS));
    if (with_aud)
        bnal(b, NAL_ARRAY(NAL_AUD));
    bnal(b, NAL_ARRAY(NAL_IDR1));
    bnal(b, NAL_ARRAY(NAL_IDR2));
    bnal(b, NAL_ARRAY(NAL_SSEI));
    bnal(b, NAL_ARRAY(NAL_PSEI));
    bnal(b, NAL_ARRAY(NAL_TR1));
    bnal(b, NAL_ARRAY(NAL_TR2));
    if (with_aud)
        bnal(b, NAL_ARRAY(NAL_AUD));
    bnal(b, NAL_ARRAY(NAL_IDR3));
    bnal(b, NAL_ARRAY(NAL_TR4));
    bnal(b, NAL_ARRAY(NAL_PSEI));
}

/* run one mux session; cuts[] holds ascending feed split points */
static void run_mux(memfile_t *out, int sequential, const unsigned char *stream, size_t n,
                    int fps_num, int fps_den, const size_t *cuts, size_t ncuts)
{
    MP4E_mux_t *mux;
    MP4H265_stream_t *s;
    size_t pos = 0, i;
    memset(out, 0, sizeof(*out));
    mux = MP4E_open(sequential, 0, out, mem_write);
    CHECK(mux != NULL);
    s = MP4H265_stream_open(mux, 64, 48, fps_num, fps_den);
    CHECK(s != NULL);
    for (i = 0; i <= ncuts; i++)
    {
        size_t end = (i < ncuts) ? cuts[i] : n;
        CHECK(end >= pos && end <= n);
        CHECK(MP4H265_stream_feed(s, stream + pos, end - pos) == MP4E_STATUS_OK);
        pos = end;
    }
    CHECK(MP4H265_stream_finish(s) == MP4E_STATUS_OK);
    MP4H265_stream_close(s);
    CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
}

/************************************************************************/
/*          independent sample-table readback                           */
/************************************************************************/

typedef struct
{
    unsigned count;
    unsigned char *bytes;   /* concatenated sample payloads */
    size_t *off;            /* per-sample offset inside bytes */
    unsigned *size, *ts, *dur;
    unsigned total;         /* track duration */
    unsigned timescale;
    unsigned nsync;
    unsigned *sync;         /* 1-based sample numbers from stss */
} table_t;

static unsigned be32(const unsigned char *p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}

/* find a child box of the given 4cc inside a box payload */
static const unsigned char *box_find(const unsigned char *p, size_t n, const char *type, size_t *payload)
{
    while (n >= 8)
    {
        uint64_t size = be32(p);
        size_t hdr = 8;
        if (size == 1)
        {
            if (n < 16)
                return NULL;
            size = ((uint64_t)be32(p + 8) << 32) | be32(p + 12);
            hdr = 16;
        } else if (size == 0)
            size = n;
        if (size < hdr || size > n)
            return NULL;
        if (!memcmp(p + 4, type, 4))
        {
            *payload = (size_t)size - hdr;
            return p + hdr;
        }
        p += (size_t)size;
        n -= (size_t)size;
    }
    return NULL;
}

static const unsigned char *find_stbl(const memfile_t *m, size_t *n)
{
    const unsigned char *p;
    p = box_find(m->data, m->size, "moov", n); CHECK(p != NULL);
    p = box_find(p, *n, "trak", n);            CHECK(p != NULL);
    p = box_find(p, *n, "mdia", n);            CHECK(p != NULL);
    p = box_find(p, *n, "minf", n);            CHECK(p != NULL);
    p = box_find(p, *n, "stbl", n);            CHECK(p != NULL);
    return p;
}

static void read_stss(const memfile_t *m, table_t *t)
{
    size_t n = 0, i;
    const unsigned char *stbl = find_stbl(m, &n);
    const unsigned char *p = box_find(stbl, n, "stss", &n);
    t->sync = (unsigned *)xmalloc(sizeof(unsigned) * (t->count ? t->count : 1));
    if (!p)
    {
        /* no stss box: every sample is a random access point */
        t->nsync = t->count;
        for (i = 0; i < t->count; i++)
            t->sync[i] = i + 1;
        return;
    }
    CHECK(n >= 8);
    t->nsync = be32(p + 4);
    CHECK(t->nsync <= t->count);
    CHECK(n >= 8 + 4 * (size_t)t->nsync);
    for (i = 0; i < t->nsync; i++)
        t->sync[i] = be32(p + 8 + 4 * i);
}

static void read_table(memfile_t *m, table_t *t)
{
    MP4D_demux_t d;
    unsigned i;
    memset(t, 0, sizeof(*t));
    if (!MP4D_open(&d, mem_read, m, (int64_t)m->size))
    {
        /* the demuxer rejects movies without tracks; confirm via the
           box tree that the movie really has no trak, then report an
           empty sample table */
        size_t n = 0;
        const unsigned char *moov = box_find(m->data, m->size, "moov", &n);
        CHECK(moov != NULL);
        CHECK(box_find(moov, n, "trak", &n) == NULL);
        return;
    }
    CHECK(d.track_count == 1);
    t->timescale = d.track[0].timescale;
    t->total = d.track[0].duration_lo;
    t->count = d.track[0].sample_count;
    t->off  = (size_t *)xmalloc(sizeof(size_t) * (t->count ? t->count : 1));
    t->size = (unsigned *)xmalloc(sizeof(unsigned) * (t->count ? t->count : 1));
    t->ts   = (unsigned *)xmalloc(sizeof(unsigned) * (t->count ? t->count : 1));
    t->dur  = (unsigned *)xmalloc(sizeof(unsigned) * (t->count ? t->count : 1));
    for (i = 0; i < t->count; i++)
    {
        unsigned frame_bytes = 0, timestamp = 0, duration = 0;
        MP4D_file_offset_t fo = MP4D_frame_offset(&d, 0, i, &frame_bytes, &timestamp, &duration);
        size_t pos;
        CHECK((uint64_t)fo + frame_bytes <= m->size);
        t->size[i] = frame_bytes;
        t->ts[i] = timestamp;
        t->dur[i] = duration;
        pos = i ? t->off[i - 1] + t->size[i - 1] : 0;
        t->off[i] = pos;
        t->bytes = (unsigned char *)realloc(t->bytes, pos + frame_bytes + 1);
        CHECK(t->bytes != NULL);
        memcpy(t->bytes + pos, m->data + (size_t)fo, frame_bytes);
    }
    MP4D_close(&d);
    read_stss(m, t);
}

static void free_table(table_t *t)
{
    free(t->bytes);
    free(t->off);
    free(t->size);
    free(t->ts);
    free(t->dur);
    free(t->sync);
    memset(t, 0, sizeof(*t));
}

static void cmp_tables(const table_t *a, const table_t *b)
{
    unsigned i;
    CHECK(a->count == b->count);
    CHECK(a->total == b->total);
    CHECK(a->timescale == b->timescale);
    CHECK(a->nsync == b->nsync);
    for (i = 0; i < a->count; i++)
    {
        CHECK(a->size[i] == b->size[i]);
        CHECK(a->ts[i] == b->ts[i]);
        CHECK(a->dur[i] == b->dur[i]);
        CHECK(!memcmp(a->bytes + a->off[i], b->bytes + b->off[i], a->size[i]));
    }
    for (i = 0; i < a->nsync; i++)
        CHECK(a->sync[i] == b->sync[i]);
}

/* verify the hvcC decoder configuration record carries the parameter sets */
static void check_hvcc(const memfile_t *m)
{
    static const unsigned types[3] = {32, 33, 34};
    const unsigned char *sets[3];
    size_t set_bytes[3], n = 0;
    const unsigned char *p, *q, *end;
    unsigned arrays, a;

    sets[0] = NAL_VPS; set_bytes[0] = sizeof(NAL_VPS);
    sets[1] = NAL_SPS; set_bytes[1] = sizeof(NAL_SPS);
    sets[2] = NAL_PPS; set_bytes[2] = sizeof(NAL_PPS);

    p = box_find(m->data, m->size, "moov", &n); CHECK(p != NULL);
    p = box_find(p, n, "trak", &n);             CHECK(p != NULL);
    p = box_find(p, n, "mdia", &n);             CHECK(p != NULL);
    p = box_find(p, n, "minf", &n);             CHECK(p != NULL);
    p = box_find(p, n, "stbl", &n);             CHECK(p != NULL);
    p = box_find(p, n, "stsd", &n);             CHECK(p != NULL);
    CHECK(n >= 8);                              /* version/flags + entry_count */
    p = box_find(p + 8, n - 8, "hvc1", &n);     CHECK(p != NULL);
    CHECK(n >= 78);                             /* VisualSampleEntry */
    p = box_find(p + 78, n - 78, "hvcC", &n);   CHECK(p != NULL);
    CHECK(n >= 23);                             /* 22-byte header + numArrays */
    q = p + 22;
    end = p + n;
    arrays = *q++;
    CHECK(arrays == 3);
    for (a = 0; a < 3; a++)
    {
        unsigned count, len;
        CHECK(q + 3 <= end);
        CHECK((*q & 0x3f) == types[a]);
        count = ((unsigned)q[1] << 8) | q[2];
        q += 3;
        CHECK(count == 1);
        CHECK(q + 2 <= end);
        len = ((unsigned)q[0] << 8) | q[1];
        q += 2;
        CHECK(len == set_bytes[a]);
        CHECK(q + len <= end);
        CHECK(!memcmp(q, sets[a], len));
        q += len;
    }
}

/************************************************************************/
/*          test_chunks: chunking must not change the output            */
/************************************************************************/

void test_chunks(void)
{
    int variant, mode;
    for (variant = 0; variant < 2; variant++)
    {
        buf_t stream = {0, 0, 0};
        table_t ref;
        memfile_t m = {0, 0, 0};
        build_stream(&stream, variant == 0);

        CASE();   /* reference: whole stream in one feed */
        run_mux(&m, 0, stream.data, stream.len, 25, 1, NULL, 0);
        read_table(&m, &ref);
        CHECK(ref.count == 4);
        CHECK(ref.timescale == 90000);
        free(m.data);

        CASE();   /* byte-by-byte, default and sequential modes */
        for (mode = 0; mode < 2; mode++)
        {
            table_t t;
            size_t *cuts = (size_t *)xmalloc(sizeof(size_t) * stream.len);
            size_t i, ncuts = 0;
            for (i = 1; i < stream.len; i++)
                cuts[ncuts++] = i;
            memset(&m, 0, sizeof(m));
            run_mux(&m, mode, stream.data, stream.len, 25, 1, cuts, ncuts);
            read_table(&m, &t);
            cmp_tables(&ref, &t);
            free_table(&t);
            free(m.data);
            free(cuts);
        }

        CASE();   /* every single split position, default and sequential modes */
        for (mode = 0; mode < 2; mode++)
        {
            size_t i;
            for (i = 0; i <= stream.len; i++)
            {
                table_t t;
                memset(&m, 0, sizeof(m));
                run_mux(&m, mode, stream.data, stream.len, 25, 1, &i, 1);
                read_table(&m, &t);
                cmp_tables(&ref, &t);
                free_table(&t);
                free(m.data);
            }
        }

        CASE();   /* irregular chunk sizes mixed with empty feeds */
        {
            size_t cuts[512];
            size_t ncuts = 0, pos = 0;
            unsigned step = 0;
            table_t t;
            while (pos < stream.len)
            {
                step = step % 13 + 1;
                pos += step;
                if (pos > stream.len)
                    pos = stream.len;
                cuts[ncuts++] = pos;
                if (step & 1)
                    cuts[ncuts++] = pos;    /* duplicate cut -> empty feed */
            }
            memset(&m, 0, sizeof(m));
            run_mux(&m, 0, stream.data, stream.len, 25, 1, cuts, ncuts);
            read_table(&m, &t);
            cmp_tables(&ref, &t);
            free_table(&t);
            free(m.data);
        }

        free_table(&ref);
        free(stream.data);
    }
}

/************************************************************************/
/*          test_access_units: picture grouping semantics               */
/************************************************************************/

void test_access_units(void)
{
    buf_t stream = {0, 0, 0};
    buf_t e0 = {0, 0, 0}, e1 = {0, 0, 0}, e2 = {0, 0, 0}, e3 = {0, 0, 0};
    memfile_t m = {0, 0, 0};
    table_t t;

    CASE();   /* multi-slice pictures, SEI placement, AUD handling, EPB bytes */
    build_stream(&stream, 1);
    run_mux(&m, 0, stream.data, stream.len, 25, 1, NULL, 0);
    read_table(&m, &t);

    au_nal(&e0, NAL_ARRAY(NAL_IDR1));
    au_nal(&e0, NAL_ARRAY(NAL_IDR2));
    au_nal(&e0, NAL_ARRAY(NAL_SSEI));
    au_nal(&e1, NAL_ARRAY(NAL_PSEI));
    au_nal(&e1, NAL_ARRAY(NAL_TR1));
    au_nal(&e1, NAL_ARRAY(NAL_TR2));
    au_nal(&e2, NAL_ARRAY(NAL_IDR3));
    au_nal(&e3, NAL_ARRAY(NAL_TR4));

    CHECK(t.count == 4);
    CHECK(t.timescale == 90000);
    CHECK(t.total == 4 * 3600);
    CHECK(t.size[0] == e0.len && !memcmp(t.bytes + t.off[0], e0.data, e0.len));
    CHECK(t.size[1] == e1.len && !memcmp(t.bytes + t.off[1], e1.data, e1.len));
    CHECK(t.size[2] == e2.len && !memcmp(t.bytes + t.off[2], e2.data, e2.len));
    CHECK(t.size[3] == e3.len && !memcmp(t.bytes + t.off[3], e3.data, e3.len));
    CHECK(t.ts[0] == 0     && t.dur[0] == 3600);
    CHECK(t.ts[1] == 3600  && t.dur[1] == 3600);
    CHECK(t.ts[2] == 7200  && t.dur[2] == 3600);
    CHECK(t.ts[3] == 10800 && t.dur[3] == 3600);
    CHECK(t.nsync == 2 && t.sync[0] == 1 && t.sync[1] == 3);
    check_hvcc(&m);
    free(m.data);

    CASE();   /* the AUD-free variant must produce the identical sample table */
    {
        buf_t s2 = {0, 0, 0};
        memfile_t m2 = {0, 0, 0};
        table_t t2;
        build_stream(&s2, 0);
        run_mux(&m2, 0, s2.data, s2.len, 25, 1, NULL, 0);
        read_table(&m2, &t2);
        cmp_tables(&t, &t2);
        free_table(&t2);
        free(m2.data);
        free(s2.data);
    }
    free_table(&t);

    CASE();   /* a prefix with no VCL must not conjure a frame */
    {
        buf_t s3 = {0, 0, 0};
        memfile_t m3 = {0, 0, 0};
        table_t t3;
        bnal(&s3, NAL_ARRAY(NAL_VPS));
        bnal(&s3, NAL_ARRAY(NAL_SPS));
        bnal(&s3, NAL_ARRAY(NAL_PPS));
        bnal(&s3, NAL_ARRAY(NAL_PSEI));
        bnal(&s3, NAL_ARRAY(NAL_SSEI));
        run_mux(&m3, 0, s3.data, s3.len, 25, 1, NULL, 0);
        read_table(&m3, &t3);
        CHECK(t3.count == 0);
        free_table(&t3);
        free(m3.data);

        /* one real picture followed by a dangling prefix SEI: one sample */
        {
            buf_t s4 = {0, 0, 0};
            memfile_t m4 = {0, 0, 0};
            table_t t4;
            buf_t exp = {0, 0, 0};
            bnal(&s4, NAL_ARRAY(NAL_VPS));
            bnal(&s4, NAL_ARRAY(NAL_SPS));
            bnal(&s4, NAL_ARRAY(NAL_PPS));
            bnal(&s4, NAL_ARRAY(NAL_IDR3));
            bnal(&s4, NAL_ARRAY(NAL_PSEI));
            run_mux(&m4, 1, s4.data, s4.len, 25, 1, NULL, 0);
            read_table(&m4, &t4);
            au_nal(&exp, NAL_ARRAY(NAL_IDR3));
            CHECK(t4.count == 1);
            CHECK(t4.size[0] == exp.len && !memcmp(t4.bytes + t4.off[0], exp.data, exp.len));
            CHECK(t4.nsync == 1 && t4.sync[0] == 1);
            free(exp.data);
            free_table(&t4);
            free(m4.data);
            free(s4.data);
        }
        free(s3.data);
    }

    CASE();   /* parameter sets only: clean finish, zero samples */
    {
        buf_t s5 = {0, 0, 0};
        memfile_t m5 = {0, 0, 0};
        table_t t5;
        bnal(&s5, NAL_ARRAY(NAL_VPS));
        bnal(&s5, NAL_ARRAY(NAL_SPS));
        bnal(&s5, NAL_ARRAY(NAL_PPS));
        run_mux(&m5, 0, s5.data, s5.len, 25, 1, NULL, 0);
        read_table(&m5, &t5);
        CHECK(t5.count == 0);
        free_table(&t5);
        free(m5.data);
        free(s5.data);
    }

    CASE();   /* trailing NAL before any picture joins the next picture */
    {
        buf_t s6 = {0, 0, 0};
        memfile_t m6 = {0, 0, 0};
        table_t t6;
        buf_t exp = {0, 0, 0};
        bnal(&s6, NAL_ARRAY(NAL_VPS));
        bnal(&s6, NAL_ARRAY(NAL_SPS));
        bnal(&s6, NAL_ARRAY(NAL_PPS));
        bnal(&s6, NAL_ARRAY(NAL_SSEI));
        bnal(&s6, NAL_ARRAY(NAL_IDR3));
        run_mux(&m6, 0, s6.data, s6.len, 25, 1, NULL, 0);
        read_table(&m6, &t6);
        au_nal(&exp, NAL_ARRAY(NAL_SSEI));
        au_nal(&exp, NAL_ARRAY(NAL_IDR3));
        CHECK(t6.count == 1);
        CHECK(t6.size[0] == exp.len && !memcmp(t6.bytes + t6.off[0], exp.data, exp.len));
        free(exp.data);
        free_table(&t6);
        free(m6.data);
        free(s6.data);
    }

    free(e0.data);
    free(e1.data);
    free(e2.data);
    free(e3.data);
    free(stream.data);
}

/************************************************************************/
/*          test_clock: exact rational 90 kHz timestamps                */
/************************************************************************/

static void build_frames(buf_t *b, unsigned nframes)
{
    static const unsigned char idr[] = {0x26, 0x01, 0x80, 0x33};
    static const unsigned char tr[]  = {0x02, 0x01, 0x80, 0x11, 0x22};
    unsigned i;
    bnal(b, NAL_ARRAY(NAL_VPS));
    bnal(b, NAL_ARRAY(NAL_SPS));
    bnal(b, NAL_ARRAY(NAL_PPS));
    bnal(b, idr, sizeof(idr));
    for (i = 1; i < nframes; i++)
        bnal(b, tr, sizeof(tr));
}

void test_clock(void)
{
    CASE();   /* 24000/1001: floor-based clock, durations 3753 and 3754 */
    {
        const unsigned n = 300;
        buf_t s = {0, 0, 0};
        memfile_t m = {0, 0, 0};
        table_t t;
        unsigned i;
        int seen3753 = 0, seen3754 = 0;
        build_frames(&s, n);
        run_mux(&m, 0, s.data, s.len, 24000, 1001, NULL, 0);
        read_table(&m, &t);
        CHECK(t.count == n);
        CHECK(t.timescale == 90000);
        for (i = 0; i < n; i++)
        {
            uint64_t want_ts   = (uint64_t)i * 90000 * 1001 / 24000;
            uint64_t want_next = (uint64_t)(i + 1) * 90000 * 1001 / 24000;
            CHECK(t.ts[i] == (unsigned)want_ts);
            CHECK(t.dur[i] == (unsigned)(want_next - want_ts));
            if (t.dur[i] == 3753)
                seen3753 = 1;
            if (t.dur[i] == 3754)
                seen3754 = 1;
        }
        CHECK(seen3753 && seen3754);
        CHECK(t.total == (unsigned)((uint64_t)n * 90000 * 1001 / 24000));

        /* sequential mode must produce the identical time table */
        {
            memfile_t m2 = {0, 0, 0};
            table_t t2;
            run_mux(&m2, 1, s.data, s.len, 24000, 1001, NULL, 0);
            read_table(&m2, &t2);
            cmp_tables(&t, &t2);
            CHECK(t2.total == t.total);
            free_table(&t2);
            free(m2.data);
        }
        free_table(&t);
        free(m.data);
        free(s.data);
    }

    CASE();   /* long sequence: equal total duration in both write modes */
    {
        const unsigned n = 2000;
        buf_t s = {0, 0, 0};
        memfile_t ma = {0, 0, 0}, mb = {0, 0, 0};
        table_t ta, tb;
        build_frames(&s, n);
        run_mux(&ma, 0, s.data, s.len, 24000, 1001, NULL, 0);
        run_mux(&mb, 1, s.data, s.len, 24000, 1001, NULL, 0);
        read_table(&ma, &ta);
        read_table(&mb, &tb);
        CHECK(ta.count == n && tb.count == n);
        CHECK(ta.total == (unsigned)((uint64_t)n * 90000 * 1001 / 24000));
        CHECK(tb.total == ta.total);
        cmp_tables(&ta, &tb);
        free_table(&ta);
        free_table(&tb);
        free(ma.data);
        free(mb.data);
        free(s.data);
    }

    CASE();   /* integer frame rates: constant durations */
    {
        const unsigned n = 50;
        buf_t s = {0, 0, 0};
        memfile_t m = {0, 0, 0};
        table_t t;
        unsigned i;
        build_frames(&s, n);
        run_mux(&m, 0, s.data, s.len, 50, 1, NULL, 0);
        read_table(&m, &t);
        CHECK(t.count == n);
        for (i = 0; i < n; i++)
        {
            CHECK(t.ts[i] == i * 1800);
            CHECK(t.dur[i] == 1800);
        }
        CHECK(t.total == n * 1800);
        free_table(&t);
        free(m.data);
        free(s.data);
    }
}

/************************************************************************/
/*          test_invalid: malformed streams and API misuse              */
/************************************************************************/

typedef struct
{
    int feed_rc, finish_rc, feed_after_rc, finish2_rc;
} bad_run_t;

/* feed the whole stream at once, then probe the sticky error behavior */
static void run_bad(bad_run_t *r, const unsigned char *stream, size_t n)
{
    memfile_t m = {0, 0, 0};
    MP4E_mux_t *mux = MP4E_open(0, 0, &m, mem_write);
    MP4H265_stream_t *s;
    CHECK(mux != NULL);
    s = MP4H265_stream_open(mux, 64, 48, 25, 1);
    CHECK(s != NULL);
    r->feed_rc = MP4H265_stream_feed(s, stream, n);
    r->finish_rc = MP4H265_stream_finish(s);
    r->feed_after_rc = MP4H265_stream_feed(s, stream, n);
    r->finish2_rc = MP4H265_stream_finish(s);
    MP4H265_stream_close(s);
    CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
    free(m.data);
}

/* the first error must stick to the instance */
static void check_sticky(const bad_run_t *r)
{
    int e = r->feed_rc ? r->feed_rc : r->finish_rc;
    CHECK(e != MP4E_STATUS_OK);
    if (r->feed_rc != MP4E_STATUS_OK)
        CHECK(r->finish_rc == e);
    CHECK(r->feed_after_rc == e);
    CHECK(r->finish2_rc == e);
}

void test_invalid(void)
{
    CASE();   /* open() argument validation */
    {
        memfile_t m = {0, 0, 0};
        MP4E_mux_t *mux;
        MP4H265_stream_t *s;
        CHECK(MP4H265_stream_open(NULL, 64, 48, 25, 1) == NULL);
        mux = MP4E_open(0, 0, &m, mem_write);
        CHECK(mux != NULL);
        CHECK(MP4H265_stream_open(mux, 64, 48, 0, 1) == NULL);        /* zero fps_num */
        CHECK(MP4H265_stream_open(mux, 64, 48, 25, 0) == NULL);       /* zero fps_den */
        CHECK(MP4H265_stream_open(mux, 64, 48, -25, 1) == NULL);      /* negative fps */
        CHECK(MP4H265_stream_open(mux, 64, 48, 90001, 1) == NULL);    /* sub-1 frame duration */
        CHECK(MP4H265_stream_open(mux, 64, 48, 1, 100000) == NULL);   /* duration overflows int */
        CHECK(MP4H265_stream_open(mux, 0, 48, 25, 1) == NULL);        /* bad dimensions */
        s = MP4H265_stream_open(mux, 64, 48, 24000, 1001);
        CHECK(s != NULL);
        MP4H265_stream_close(s);
        CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
        free(m.data);
    }

    CASE();   /* first VCL is not IRAP */
    {
        buf_t s = {0, 0, 0};
        bad_run_t r;
        bnal(&s, NAL_ARRAY(NAL_VPS));
        bnal(&s, NAL_ARRAY(NAL_SPS));
        bnal(&s, NAL_ARRAY(NAL_PPS));
        bnal(&s, NAL_ARRAY(NAL_TR1));
        bnal(&s, NAL_ARRAY(NAL_IDR3));
        run_bad(&r, s.data, s.len);
        check_sticky(&r);
        free(s.data);
    }

    CASE();   /* incomplete configuration before the first VCL */
    {
        /* missing PPS */
        buf_t s = {0, 0, 0};
        bad_run_t r;
        bnal(&s, NAL_ARRAY(NAL_VPS));
        bnal(&s, NAL_ARRAY(NAL_SPS));
        bnal(&s, NAL_ARRAY(NAL_IDR3));
        bnal(&s, NAL_ARRAY(NAL_TR1));
        run_bad(&r, s.data, s.len);
        check_sticky(&r);
        free(s.data);
    }
    {
        /* missing VPS */
        buf_t s = {0, 0, 0};
        bad_run_t r;
        bnal(&s, NAL_ARRAY(NAL_SPS));
        bnal(&s, NAL_ARRAY(NAL_PPS));
        bnal(&s, NAL_ARRAY(NAL_IDR3));
        bnal(&s, NAL_ARRAY(NAL_TR1));
        run_bad(&r, s.data, s.len);
        check_sticky(&r);
        free(s.data);
    }

    CASE();   /* continuation slice with no first slice */
    {
        buf_t s = {0, 0, 0};
        bad_run_t r;
        bnal(&s, NAL_ARRAY(NAL_VPS));
        bnal(&s, NAL_ARRAY(NAL_SPS));
        bnal(&s, NAL_ARRAY(NAL_PPS));
        bnal(&s, NAL_ARRAY(NAL_TR2));   /* first_slice_segment_in_pic_flag = 0 */
        bnal(&s, NAL_ARRAY(NAL_IDR3));
        run_bad(&r, s.data, s.len);
        check_sticky(&r);
        free(s.data);
    }

    CASE();   /* parameter set after the first VCL */
    {
        buf_t s = {0, 0, 0};
        bad_run_t r;
        bnal(&s, NAL_ARRAY(NAL_VPS));
        bnal(&s, NAL_ARRAY(NAL_SPS));
        bnal(&s, NAL_ARRAY(NAL_PPS));
        bnal(&s, NAL_ARRAY(NAL_IDR3));
        bnal(&s, NAL_ARRAY(NAL_TR1));
        bnal(&s, NAL_ARRAY(NAL_VPS));   /* too late */
        bnal(&s, NAL_ARRAY(NAL_TR1));
        run_bad(&r, s.data, s.len);
        check_sticky(&r);
        free(s.data);
    }

    CASE();   /* multi-layer or multi-temporal-layer NAL units */
    {
        static const unsigned char bad_layer[] = {0x02, 0x09, 0x80, 0x11};  /* layer_id 1 */
        static const unsigned char bad_tid2[]  = {0x02, 0x02, 0x80, 0x11};  /* temporal_id_plus1 2 */
        static const unsigned char bad_tid0[]  = {0x02, 0x00, 0x80, 0x11};  /* temporal_id_plus1 0 */
        static const unsigned char bad_vps[]   = {0x40, 0x09, 0x0C, 0x01};  /* layer_id 1 */
        const unsigned char *bad[4];
        size_t bad_len[4];
        int k;
        bad[0] = bad_layer; bad_len[0] = sizeof(bad_layer);
        bad[1] = bad_tid2;  bad_len[1] = sizeof(bad_tid2);
        bad[2] = bad_tid0;  bad_len[2] = sizeof(bad_tid0);
        bad[3] = bad_vps;   bad_len[3] = sizeof(bad_vps);
        for (k = 0; k < 4; k++)
        {
            buf_t s = {0, 0, 0};
            bad_run_t r;
            if (k == 3)
            {
                bnal(&s, bad[k], bad_len[k]);   /* bad VPS replaces the good one */
            } else
            {
                bnal(&s, NAL_ARRAY(NAL_VPS));
            }
            bnal(&s, NAL_ARRAY(NAL_SPS));
            bnal(&s, NAL_ARRAY(NAL_PPS));
            if (k != 3)
                bnal(&s, bad[k], bad_len[k]);
            bnal(&s, NAL_ARRAY(NAL_IDR3));
            run_bad(&r, s.data, s.len);
            check_sticky(&r);
            free(s.data);
        }
    }

    CASE();   /* truncated NAL header */
    {
        /* one-byte NAL at end of stream: detected at finish() */
        buf_t s = {0, 0, 0};
        bad_run_t r;
        static const unsigned char tail[] = {0, 0, 0, 1, 0x40};
        bnal(&s, NAL_ARRAY(NAL_VPS));
        bnal(&s, NAL_ARRAY(NAL_SPS));
        bnal(&s, NAL_ARRAY(NAL_PPS));
        bnal(&s, NAL_ARRAY(NAL_IDR3));
        bput(&s, tail, sizeof(tail));
        run_bad(&r, s.data, s.len);
        CHECK(r.feed_rc == MP4E_STATUS_OK);     /* surfaces only at EOF */
        check_sticky(&r);
        free(s.data);
    }
    {
        /* one-byte NAL in the middle of the stream: detected at feed() */
        buf_t s = {0, 0, 0};
        bad_run_t r;
        static const unsigned char tiny[] = {0, 0, 0, 1, 0x40};
        bnal(&s, NAL_ARRAY(NAL_VPS));
        bnal(&s, NAL_ARRAY(NAL_SPS));
        bnal(&s, NAL_ARRAY(NAL_PPS));
        bput(&s, tiny, sizeof(tiny));
        bnal(&s, NAL_ARRAY(NAL_IDR3));
        run_bad(&r, s.data, s.len);
        CHECK(r.feed_rc != MP4E_STATUS_OK);
        check_sticky(&r);
        free(s.data);
    }

    CASE();   /* VCL NAL without a slice header byte */
    {
        buf_t s = {0, 0, 0};
        bad_run_t r;
        static const unsigned char bare_vcl[] = {0, 0, 0, 1, 0x26, 0x01};
        bnal(&s, NAL_ARRAY(NAL_VPS));
        bnal(&s, NAL_ARRAY(NAL_SPS));
        bnal(&s, NAL_ARRAY(NAL_PPS));
        bput(&s, bare_vcl, sizeof(bare_vcl));
        bnal(&s, NAL_ARRAY(NAL_IDR3));
        run_bad(&r, s.data, s.len);
        check_sticky(&r);
        free(s.data);
    }

    CASE();   /* error surfaces on the feed that completes the bad NAL */
    {
        memfile_t m = {0, 0, 0};
        MP4E_mux_t *mux = MP4E_open(0, 0, &m, mem_write);
        MP4H265_stream_t *s;
        buf_t head = {0, 0, 0}, bad = {0, 0, 0};
        static const unsigned char bad_layer[] = {0x02, 0x09, 0x80, 0x11};
        CHECK(mux != NULL);
        s = MP4H265_stream_open(mux, 64, 48, 25, 1);
        CHECK(s != NULL);
        bnal(&head, NAL_ARRAY(NAL_VPS));
        bnal(&head, NAL_ARRAY(NAL_SPS));
        bnal(&head, NAL_ARRAY(NAL_PPS));
        CHECK(MP4H265_stream_feed(s, head.data, head.len) == MP4E_STATUS_OK);
        bnal(&bad, bad_layer, sizeof(bad_layer));
        bnal(&bad, NAL_ARRAY(NAL_IDR3));
        CHECK(MP4H265_stream_feed(s, bad.data, bad.len) != MP4E_STATUS_OK);
        CHECK(MP4H265_stream_feed(s, bad.data, bad.len) != MP4E_STATUS_OK);
        CHECK(MP4H265_stream_finish(s) != MP4E_STATUS_OK);
        MP4H265_stream_close(s);
        CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
        free(head.data);
        free(bad.data);
        free(m.data);
    }

    CASE();   /* repeated finish is a no-op; feed after finish fails */
    {
        memfile_t m = {0, 0, 0};
        MP4E_mux_t *mux = MP4E_open(0, 0, &m, mem_write);
        MP4H265_stream_t *s;
        buf_t str = {0, 0, 0};
        table_t t;
        CHECK(mux != NULL);
        s = MP4H265_stream_open(mux, 64, 48, 25, 1);
        CHECK(s != NULL);
        bnal(&str, NAL_ARRAY(NAL_VPS));
        bnal(&str, NAL_ARRAY(NAL_SPS));
        bnal(&str, NAL_ARRAY(NAL_PPS));
        bnal(&str, NAL_ARRAY(NAL_IDR3));
        CHECK(MP4H265_stream_feed(s, str.data, str.len) == MP4E_STATUS_OK);
        CHECK(MP4H265_stream_finish(s) == MP4E_STATUS_OK);
        CHECK(MP4H265_stream_finish(s) == MP4E_STATUS_OK);   /* no duplicate frame */
        CHECK(MP4H265_stream_feed(s, str.data, str.len) != MP4E_STATUS_OK);
        MP4H265_stream_close(s);
        CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
        read_table(&m, &t);
        CHECK(t.count == 1);                                /* exactly one frame committed */
        free_table(&t);
        free(str.data);
        free(m.data);
    }

    CASE();   /* an error does not retract already committed samples */
    {
        memfile_t m = {0, 0, 0};
        MP4E_mux_t *mux = MP4E_open(0, 0, &m, mem_write);
        MP4H265_stream_t *s;
        buf_t str = {0, 0, 0};
        table_t t;
        static const unsigned char bad_layer[] = {0x02, 0x09, 0x80, 0x11};
        CHECK(mux != NULL);
        s = MP4H265_stream_open(mux, 64, 48, 25, 1);
        CHECK(s != NULL);
        bnal(&str, NAL_ARRAY(NAL_VPS));
        bnal(&str, NAL_ARRAY(NAL_SPS));
        bnal(&str, NAL_ARRAY(NAL_PPS));
        bnal(&str, NAL_ARRAY(NAL_IDR3));
        bnal(&str, NAL_ARRAY(NAL_TR1));     /* commits the IDR picture */
        bnal(&str, bad_layer, sizeof(bad_layer));
        bnal(&str, NAL_ARRAY(NAL_TR1));
        CHECK(MP4H265_stream_feed(s, str.data, str.len) != MP4E_STATUS_OK);
        CHECK(MP4H265_stream_finish(s) != MP4E_STATUS_OK);
        MP4H265_stream_close(s);
        CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
        read_table(&m, &t);
        CHECK(t.count == 1);                /* the finished picture stays */
        CHECK(t.nsync == 1 && t.sync[0] == 1);
        free_table(&t);
        free(str.data);
        free(m.data);
    }

    CASE();   /* empty feeds are allowed; NULL arguments are not */
    {
        memfile_t m = {0, 0, 0};
        MP4E_mux_t *mux = MP4E_open(0, 0, &m, mem_write);
        MP4H265_stream_t *s;
        buf_t str = {0, 0, 0};
        CHECK(mux != NULL);
        s = MP4H265_stream_open(mux, 64, 48, 25, 1);
        CHECK(s != NULL);
        CHECK(MP4H265_stream_feed(s, NULL, 0) == MP4E_STATUS_OK);
        CHECK(MP4H265_stream_feed(s, str.data, 0) == MP4E_STATUS_OK);
        CHECK(MP4H265_stream_feed(NULL, str.data, 0) != MP4E_STATUS_OK);
        CHECK(MP4H265_stream_finish(NULL) != MP4E_STATUS_OK);
        CHECK(MP4H265_stream_feed(s, NULL, 5) != MP4E_STATUS_OK);   /* sticky */
        CHECK(MP4H265_stream_feed(s, str.data, 0) != MP4E_STATUS_OK);
        MP4H265_stream_close(s);
        MP4H265_stream_close(NULL);                                 /* no-op */
        CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
        free(str.data);
        free(m.data);
    }
}
