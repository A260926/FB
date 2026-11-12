/*
    Regression tests for composition-offset (ctts) support:
    MP4E_put_sample_cts() on the mux side, MP4D_frame_timing() on the demux side.

    The muxer output is additionally checked by an independent box walker
    (not by this library's own demuxer), so a bug shared by the writer and
    the reader cannot cancel out. Hand-crafted ctts version 0/1 files
    (including an offset above INT32_MAX) exercise the reader directly.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MINIMP4_IMPLEMENTATION
#include "../minimp4.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define FOURCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/************************************************************************/
/*          In-memory file + read/write callbacks                       */
/************************************************************************/

typedef struct
{
    uint8_t *data;
    size_t size, cap;
} memfile_t;

static int mem_write(int64_t offset, const void *buffer, size_t size, void *token)
{
    memfile_t *m = (memfile_t *)token;
    if ((uint64_t)offset + size > m->cap)
    {
        size_t ncap = m->cap ? m->cap : 4096;
        uint8_t *nd;
        while ((uint64_t)offset + size > ncap)
            ncap *= 2;
        nd = (uint8_t *)realloc(m->data, ncap);
        if (!nd)
            return 1;
        m->data = nd;
        m->cap = ncap;
    }
    memcpy(m->data + offset, buffer, size);
    if ((size_t)offset + size > m->size)
        m->size = (size_t)offset + size;
    return 0;
}

static int mem_read(int64_t offset, void *buffer, size_t size, void *token)
{
    memfile_t *m = (memfile_t *)token;
    size_t avail, n;
    if (offset < 0 || (size_t)offset >= m->size)
        return 1;
    avail = m->size - (size_t)offset;
    n = size < avail ? size : avail;
    memcpy(buffer, m->data + offset, n);
    return n != size;
}

static void memfile_free(memfile_t *m)
{
    free(m->data);
    memset(m, 0, sizeof(*m));
}

/************************************************************************/
/*          Independent MP4 box walker (verification side)              */
/************************************************************************/

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

typedef struct
{
    unsigned ctts_count;        // how many ctts boxes were found
    int      ctts_version;      // version of the last ctts box
    unsigned ctts_entry_count;  // run count of the last ctts box
    uint32_t ctts_entries[16][2];
    unsigned mdat_count;
    uint8_t  *mdat;             // concatenation of all mdat payloads, in file order
    size_t   mdat_size, mdat_cap;
} scan_t;

static void scan_append_mdat(scan_t *sc, const uint8_t *p, size_t n)
{
    if (sc->mdat_size + n > sc->mdat_cap)
    {
        sc->mdat_cap = (sc->mdat_size + n) * 2 + 64;
        sc->mdat = (uint8_t *)realloc(sc->mdat, sc->mdat_cap);
    }
    memcpy(sc->mdat + sc->mdat_size, p, n);
    sc->mdat_size += n;
}

static void scan_boxes(const uint8_t *d, size_t start, size_t end, scan_t *sc)
{
    size_t pos = start;
    while (pos + 8 <= end)
    {
        uint64_t size = rd32(d + pos);
        uint32_t name = rd32(d + pos + 4);
        size_t hdr = 8;
        if (size == 1)
        {
            size = ((uint64_t)rd32(d + pos + 8) << 32) | rd32(d + pos + 12);
            hdr = 16;
        }
        if (size == 0)
            size = end - pos;
        if (size < hdr || size > end - pos)
            break;
        if (name == FOURCC('c', 't', 't', 's'))
        {
            const uint8_t *p = d + pos + hdr;
            unsigned i, n;
            sc->ctts_count++;
            sc->ctts_version = p[0];
            sc->ctts_entry_count = rd32(p + 4);
            n = sc->ctts_entry_count;
            if (n > 16)
                n = 16;
            for (i = 0; i < n; i++)
            {
                sc->ctts_entries[i][0] = rd32(p + 8 + i*8);
                sc->ctts_entries[i][1] = rd32(p + 8 + i*8 + 4);
            }
        } else if (name == FOURCC('m', 'd', 'a', 't'))
        {
            sc->mdat_count++;
            scan_append_mdat(sc, d + pos + hdr, (size_t)(size - hdr));
        } else if (name == FOURCC('m', 'o', 'o', 'v') || name == FOURCC('t', 'r', 'a', 'k') ||
                   name == FOURCC('m', 'd', 'i', 'a') || name == FOURCC('m', 'i', 'n', 'f') ||
                   name == FOURCC('s', 't', 'b', 'l'))
        {
            scan_boxes(d, pos + hdr, pos + (size_t)size, sc);
        }
        pos += (size_t)size;
    }
}

static void scan_free(scan_t *sc)
{
    free(sc->mdat);
    memset(sc, 0, sizeof(*sc));
}

/************************************************************************/
/*          Hand-crafted MP4 builder (reader-side fixtures)             */
/************************************************************************/

typedef struct
{
    uint8_t *d;
    size_t size, cap;
    size_t stack[16];
    int sp;
} bw_t;

static void bw_put(bw_t *b, const void *p, size_t n)
{
    if (b->size + n > b->cap)
    {
        b->cap = (b->size + n) * 2 + 64;
        b->d = (uint8_t *)realloc(b->d, b->cap);
    }
    memcpy(b->d + b->size, p, n);
    b->size += n;
}

static void bw_u32(bw_t *b, uint32_t v)
{
    uint8_t t[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    bw_put(b, t, 4);
}

static void bw_u16(bw_t *b, uint32_t v)
{
    uint8_t t[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    bw_put(b, t, 2);
}

static void bw_begin(bw_t *b, uint32_t name)
{
    bw_u32(b, 0);
    bw_u32(b, name);
    b->stack[b->sp++] = b->size - 8;
}

static void bw_end(bw_t *b)
{
    size_t pos = b->stack[--b->sp];
    uint32_t sz = (uint32_t)(b->size - pos);
    b->d[pos + 0] = (uint8_t)(sz >> 24);
    b->d[pos + 1] = (uint8_t)(sz >> 16);
    b->d[pos + 2] = (uint8_t)(sz >> 8);
    b->d[pos + 3] = (uint8_t)sz;
}

static void bw_ctts(bw_t *b, uint32_t version, int zero_count, int truncated,
                    const uint32_t *entries, unsigned nentries)
{
    unsigned i;
    bw_begin(b, FOURCC('c', 't', 't', 's'));
    bw_u32(b, version << 24);                                   // version + flags
    bw_u32(b, zero_count ? 0 : (truncated ? nentries + 1 : nentries));
    for (i = 0; i < nentries; i++)
    {
        bw_u32(b, entries[2*i]);
        bw_u32(b, entries[2*i + 1]);
    }
    bw_end(b);
}

/*
    Minimal one-track video MP4: 3 samples of 4 bytes each, stts {3, 100}.
    The ctts box (optional) can be placed before or after stts, duplicated,
    emptied, or truncated, to exercise the demuxer's validation paths.
*/
static void craft_mp4(memfile_t *f, uint32_t ctts_version, int ctts_before_stts,
                      int ctts_duplicate, int ctts_zero_count, int ctts_truncated,
                      const uint32_t *ctts_entries, unsigned ctts_nentries)
{
    bw_t b = { 0, 0, 0, { 0 }, 0 };
    size_t stco_pos, mdat_data_pos;
    static const uint8_t samples[12] = {
        0xA0, 0xA1, 0xA2, 0xA3,
        0xB0, 0xB1, 0xB2, 0xB3,
        0xC0, 0xC1, 0xC2, 0xC3
    };

    bw_begin(&b, FOURCC('f', 't', 'y', 'p'));
    bw_u32(&b, FOURCC('m', 'p', '4', '2'));
    bw_u32(&b, 0);
    bw_u32(&b, FOURCC('m', 'p', '4', '2'));
    bw_u32(&b, FOURCC('i', 's', 'o', 'm'));
    bw_end(&b);

    bw_begin(&b, FOURCC('m', 'o', 'o', 'v'));
        bw_begin(&b, FOURCC('m', 'v', 'h', 'd'));
        bw_u32(&b, 0);              // version + flags
        bw_u32(&b, 0);              // creation_time
        bw_u32(&b, 0);              // modification_time
        bw_u32(&b, 1000);           // timescale
        bw_u32(&b, 300);            // duration
        bw_u32(&b, 0x00010000);     // rate
        bw_u16(&b, 0x0100);         // volume
        bw_u16(&b, 0);              // reserved
        bw_u32(&b, 0); bw_u32(&b, 0);
        bw_u32(&b, 0x00010000); bw_u32(&b, 0); bw_u32(&b, 0);
        bw_u32(&b, 0); bw_u32(&b, 0x00010000); bw_u32(&b, 0);
        bw_u32(&b, 0); bw_u32(&b, 0); bw_u32(&b, 0x40000000);
        bw_u32(&b, 0); bw_u32(&b, 0); bw_u32(&b, 0);
        bw_u32(&b, 0); bw_u32(&b, 0); bw_u32(&b, 0);
        bw_u32(&b, 2);              // next_track_ID
        bw_end(&b);

        bw_begin(&b, FOURCC('t', 'r', 'a', 'k'));
            bw_begin(&b, FOURCC('m', 'd', 'i', 'a'));
                bw_begin(&b, FOURCC('m', 'd', 'h', 'd'));
                bw_u32(&b, 0);      // version + flags
                bw_u32(&b, 0);      // creation_time
                bw_u32(&b, 0);      // modification_time
                bw_u32(&b, 90000);  // timescale
                bw_u32(&b, 300);    // duration
                bw_u16(&b, 0x15c4); // language 'und'
                bw_u16(&b, 0);
                bw_end(&b);

                bw_begin(&b, FOURCC('h', 'd', 'l', 'r'));
                bw_u32(&b, 0);      // version + flags
                bw_u32(&b, 0);      // pre_defined
                bw_u32(&b, FOURCC('v', 'i', 'd', 'e'));
                bw_u32(&b, 0); bw_u32(&b, 0); bw_u32(&b, 0);
                bw_end(&b);

                bw_begin(&b, FOURCC('m', 'i', 'n', 'f'));
                    bw_begin(&b, FOURCC('v', 'm', 'h', 'd'));
                    bw_u32(&b, 1);  // version + flags
                    bw_u16(&b, 0);  // graphicsmode
                    bw_u16(&b, 0); bw_u16(&b, 0); bw_u16(&b, 0);
                    bw_end(&b);

                    bw_begin(&b, FOURCC('s', 't', 'b', 'l'));
                        bw_begin(&b, FOURCC('s', 't', 's', 'd'));
                        bw_u32(&b, 0);  // version + flags
                        bw_u32(&b, 0);  // entry_count
                        bw_end(&b);

                        if (ctts_entries && ctts_before_stts)
                        {
                            bw_ctts(&b, ctts_version, ctts_zero_count, ctts_truncated, ctts_entries, ctts_nentries);
                            if (ctts_duplicate)
                                bw_ctts(&b, ctts_version, ctts_zero_count, ctts_truncated, ctts_entries, ctts_nentries);
                        }

                        bw_begin(&b, FOURCC('s', 't', 't', 's'));
                        bw_u32(&b, 0);  // version + flags
                        bw_u32(&b, 1);  // entry_count
                        bw_u32(&b, 3);  // sample_count
                        bw_u32(&b, 100);// sample_delta
                        bw_end(&b);

                        if (ctts_entries && !ctts_before_stts)
                        {
                            bw_ctts(&b, ctts_version, ctts_zero_count, ctts_truncated, ctts_entries, ctts_nentries);
                            if (ctts_duplicate)
                                bw_ctts(&b, ctts_version, ctts_zero_count, ctts_truncated, ctts_entries, ctts_nentries);
                        }

                        bw_begin(&b, FOURCC('s', 't', 's', 'c'));
                        bw_u32(&b, 0);  // version + flags
                        bw_u32(&b, 1);  // entry_count
                        bw_u32(&b, 1);  // first_chunk
                        bw_u32(&b, 1);  // samples_per_chunk
                        bw_u32(&b, 1);  // sample_description_index
                        bw_end(&b);

                        bw_begin(&b, FOURCC('s', 't', 's', 'z'));
                        bw_u32(&b, 0);  // version + flags
                        bw_u32(&b, 0);  // sample_size (0 => table follows)
                        bw_u32(&b, 3);  // sample_count
                        bw_u32(&b, 4); bw_u32(&b, 4); bw_u32(&b, 4);
                        bw_end(&b);

                        bw_begin(&b, FOURCC('s', 't', 'c', 'o'));
                        bw_u32(&b, 0);  // version + flags
                        bw_u32(&b, 1);  // entry_count
                        stco_pos = b.size;
                        bw_u32(&b, 0);  // chunk_offset, patched below
                        bw_end(&b);
                    bw_end(&b); // stbl
                bw_end(&b); // minf
            bw_end(&b); // mdia
        bw_end(&b); // trak
    bw_end(&b); // moov

    bw_u32(&b, 8 + (uint32_t)sizeof(samples));
    bw_u32(&b, FOURCC('m', 'd', 'a', 't'));
    mdat_data_pos = b.size;
    bw_put(&b, samples, sizeof(samples));

    // patch the single stco entry with the absolute offset of sample data
    b.d[stco_pos + 0] = (uint8_t)(mdat_data_pos >> 24);
    b.d[stco_pos + 1] = (uint8_t)(mdat_data_pos >> 16);
    b.d[stco_pos + 2] = (uint8_t)(mdat_data_pos >> 8);
    b.d[stco_pos + 3] = (uint8_t)mdat_data_pos;

    f->data = b.d;
    f->size = b.size;
    f->cap = b.cap;
}

/************************************************************************/
/*          Mux helpers                                                 */
/************************************************************************/

static int add_video_track(MP4E_mux_t *mux, unsigned timescale, unsigned default_duration)
{
    MP4E_track_t tr;
    int id;
    static const uint8_t sps[] = { 0x67, 0x42, 0x00, 0x1e };
    static const uint8_t pps[] = { 0x68, 0xce, 0x06, 0xe2 };
    memset(&tr, 0, sizeof(tr));
    tr.track_media_kind = e_video;
    tr.language[0] = 'u';
    tr.language[1] = 'n';
    tr.language[2] = 'd';
    tr.object_type_indication = MP4_OBJECT_TYPE_AVC;
    tr.time_scale = timescale;
    tr.default_duration = default_duration;
    tr.u.v.width = 16;
    tr.u.v.height = 16;
    id = MP4E_add_track(mux, &tr);
    MP4E_set_sps(mux, id, sps, sizeof(sps));
    MP4E_set_pps(mux, id, pps, sizeof(pps));
    return id;
}

static void fill(uint8_t *p, int n, uint8_t v)
{
    int i;
    for (i = 0; i < n; i++)
        p[i] = v;
}

/************************************************************************/
/*          Tests                                                       */
/************************************************************************/

/*
    Variable frame durations, negative offsets (=> ctts version 1), negative
    PTS, merging of adjacent equal offsets; independent check of the ctts box
    and of mdat decode-order layout; MP4D_frame_timing() round-trip.
*/
static void test_basic_roundtrip(void)
{
    memfile_t f = { 0, 0, 0 };
    MP4E_mux_t *mux;
    MP4D_demux_t mp4;
    scan_t sc;
    int tr, i;
    static const int durations[4] = { 100, 200, 100, 100 };
    static const int32_t offsets[4] = { -5, 3, 3, -1 };
    static const uint64_t dts_exp[4] = { 0, 100, 300, 400 };
    uint8_t payload[4][8];

    memset(&sc, 0, sizeof(sc));
    mux = MP4E_open(0, 0, &f, mem_write);
    tr = add_video_track(mux, 90000, 0);
    for (i = 0; i < 4; i++)
    {
        fill(payload[i], 8, (uint8_t)(0x10 + i));
        CHECK(MP4E_put_sample_cts(mux, tr, payload[i], 8, durations[i],
            i == 0 ? MP4E_SAMPLE_RANDOM_ACCESS : MP4E_SAMPLE_DEFAULT, offsets[i]) == MP4E_STATUS_OK);
    }
    CHECK(MP4E_close(mux) == MP4E_STATUS_OK);

    // independent parse: ctts version 1 (negative offsets present),
    // adjacent equal offsets merged into one run
    scan_boxes(f.data, 0, f.size, &sc);
    CHECK(sc.ctts_count == 1);
    CHECK(sc.ctts_version == 1);
    CHECK(sc.ctts_entry_count == 3);
    CHECK(sc.ctts_entries[0][0] == 1 && sc.ctts_entries[0][1] == (uint32_t)-5);
    CHECK(sc.ctts_entries[1][0] == 2 && sc.ctts_entries[1][1] == 3);
    CHECK(sc.ctts_entries[2][0] == 1 && sc.ctts_entries[2][1] == (uint32_t)-1);
    // mdat holds the samples in decode (submission) order
    CHECK(sc.mdat_size == 32);
    for (i = 0; i < 4; i++)
        CHECK(!memcmp(sc.mdat + i*8, payload[i], 8));

    // demux side
    CHECK(MP4D_open(&mp4, mem_read, &f, (int64_t)f.size));
    CHECK(mp4.track_count == 1);
    for (i = 0; i < 4; i++)
    {
        uint64_t dts = 0;
        int64_t pts = 0;
        uint32_t dur = 0;
        CHECK(MP4D_frame_timing(&mp4, 0, (unsigned)i, &dts, &pts, &dur));
        CHECK(dts == dts_exp[i]);
        CHECK(dur == (uint32_t)durations[i]);
        CHECK(pts == (int64_t)dts_exp[i] + offsets[i]);
    }
    // sample 0 has a negative presentation time
    {
        uint64_t dts;
        int64_t pts;
        uint32_t dur;
        CHECK(MP4D_frame_timing(&mp4, 0, 0, &dts, &pts, &dur));
        CHECK(pts == -5);
    }
    // invalid indexes fail and leave outputs untouched
    {
        uint64_t dts = 0xDEADBEEF;
        int64_t pts = 0xDEADBEEF;
        uint32_t dur = 0xDEADBEEF;
        CHECK(!MP4D_frame_timing(&mp4, 0, 4, &dts, &pts, &dur));
        CHECK(!MP4D_frame_timing(&mp4, 7, 0, &dts, &pts, &dur));
        CHECK(dts == 0xDEADBEEF && pts == 0xDEADBEEF && dur == 0xDEADBEEF);
    }
    // MP4D_frame_offset() keeps its old meaning on a ctts file
    {
        unsigned frame_bytes = 0, timestamp = 0, duration = 0;
        MP4D_file_offset_t ofs = MP4D_frame_offset(&mp4, 0, 1, &frame_bytes, &timestamp, &duration);
        CHECK(ofs != 0);
        CHECK(frame_bytes == 8);
        CHECK(timestamp == 100 && duration == 200);
    }
    MP4D_close(&mp4);
    scan_free(&sc);
    memfile_free(&f);
}

/*
    All-zero offsets: no ctts box is written (old files stay byte-identical),
    and PTS == DTS on readback. Also covers duration=0 => track default.
*/
static void test_zero_offsets_no_ctts(void)
{
    memfile_t f = { 0, 0, 0 };
    MP4E_mux_t *mux;
    MP4D_demux_t mp4;
    scan_t sc;
    int tr, i;
    uint8_t payload[4];

    memset(&sc, 0, sizeof(sc));
    fill(payload, 4, 0x77);

    // old entry point
    mux = MP4E_open(0, 0, &f, mem_write);
    tr = add_video_track(mux, 90000, 250);
    for (i = 0; i < 3; i++)
        CHECK(MP4E_put_sample(mux, tr, payload, 4, i == 2 ? 100 : 0, MP4E_SAMPLE_DEFAULT) == MP4E_STATUS_OK);
    CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
    scan_boxes(f.data, 0, f.size, &sc);
    CHECK(sc.ctts_count == 0);

    CHECK(MP4D_open(&mp4, mem_read, &f, (int64_t)f.size));
    {
        static const uint64_t dts_exp[3] = { 0, 250, 500 };
        static const uint32_t dur_exp[3] = { 250, 250, 100 };
        for (i = 0; i < 3; i++)
        {
            uint64_t dts = 0;
            int64_t pts = 0;
            uint32_t dur = 0;
            CHECK(MP4D_frame_timing(&mp4, 0, (unsigned)i, &dts, &pts, &dur));
            CHECK(dts == dts_exp[i]);
            CHECK(pts == (int64_t)dts_exp[i]); // no ctts => PTS == DTS
            CHECK(dur == dur_exp[i]);          // duration=0 => track default 250
        }
    }
    MP4D_close(&mp4);
    scan_free(&sc);
    memfile_free(&f);

    // new entry point with explicit zero offsets: still no ctts box
    memset(&sc, 0, sizeof(sc));
    mux = MP4E_open(0, 0, &f, mem_write);
    tr = add_video_track(mux, 90000, 0);
    for (i = 0; i < 3; i++)
        CHECK(MP4E_put_sample_cts(mux, tr, payload, 4, 100, MP4E_SAMPLE_DEFAULT, 0) == MP4E_STATUS_OK);
    CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
    scan_boxes(f.data, 0, f.size, &sc);
    CHECK(sc.ctts_count == 0);
    scan_free(&sc);
    memfile_free(&f);
}

/*
    Sequential mode: positive-only offsets (=> ctts version 0), a multi-slice
    sample written via CONTINUATION, mdat decode-order check.
*/
static void test_sequential_and_continuation(void)
{
    memfile_t f = { 0, 0, 0 };
    MP4E_mux_t *mux;
    MP4D_demux_t mp4;
    scan_t sc;
    int tr, i;
    static const int32_t offsets[3] = { 1, 2, 2 };
    static const uint64_t dts_exp[3] = { 0, 300, 600 };
    uint8_t payload[4][8];

    memset(&sc, 0, sizeof(sc));
    mux = MP4E_open(1, 0, &f, mem_write); // sequential mode
    tr = add_video_track(mux, 90000, 0);
    for (i = 0; i < 4; i++)
        fill(payload[i], i == 3 ? 4 : 8, (uint8_t)(0x20 + i));

    CHECK(MP4E_put_sample_cts(mux, tr, payload[0], 8, 300, MP4E_SAMPLE_RANDOM_ACCESS, offsets[0]) == MP4E_STATUS_OK);
    CHECK(MP4E_put_sample_cts(mux, tr, payload[1], 8, 300, MP4E_SAMPLE_DEFAULT, offsets[1]) == MP4E_STATUS_OK);
    // third sample is delivered as two slices
    CHECK(MP4E_put_sample_cts(mux, tr, payload[2], 8, 300, MP4E_SAMPLE_DEFAULT, offsets[2]) == MP4E_STATUS_OK);
    CHECK(MP4E_put_sample_cts(mux, tr, payload[3], 4, 300, MP4E_SAMPLE_CONTINUATION, offsets[2]) == MP4E_STATUS_OK);
    CHECK(MP4E_close(mux) == MP4E_STATUS_OK);

    scan_boxes(f.data, 0, f.size, &sc);
    CHECK(sc.ctts_count == 1);
    CHECK(sc.ctts_version == 0); // no negative offsets
    CHECK(sc.ctts_entry_count == 2);
    CHECK(sc.ctts_entries[0][0] == 1 && sc.ctts_entries[0][1] == 1);
    CHECK(sc.ctts_entries[1][0] == 2 && sc.ctts_entries[1][1] == 2);
    // sequential mode writes one mdat per sample; concatenation is decode order
    CHECK(sc.mdat_size == 28);
    CHECK(!memcmp(sc.mdat, payload[0], 8));
    CHECK(!memcmp(sc.mdat + 8, payload[1], 8));
    CHECK(!memcmp(sc.mdat + 16, payload[2], 8));
    CHECK(!memcmp(sc.mdat + 24, payload[3], 4));

    CHECK(MP4D_open(&mp4, mem_read, &f, (int64_t)f.size));
    CHECK(mp4.track[0].sample_count == 3); // continuation did not add a sample
    for (i = 0; i < 3; i++)
    {
        uint64_t dts = 0;
        int64_t pts = 0;
        uint32_t dur = 0;
        CHECK(MP4D_frame_timing(&mp4, 0, (unsigned)i, &dts, &pts, &dur));
        CHECK(dts == dts_exp[i]);
        CHECK(pts == (int64_t)dts_exp[i] + offsets[i]);
        CHECK(dur == 300);
    }
    // the continuation extended the previous sample's byte count only
    {
        unsigned frame_bytes = 0;
        MP4D_frame_offset(&mp4, 0, 2, &frame_bytes, 0, 0);
        CHECK(frame_bytes == 12);
    }
    MP4D_close(&mp4);
    scan_free(&sc);
    memfile_free(&f);
}

/*
    Two tracks written interleaved: ctts belongs to its own track, the other
    track (no offsets) reports PTS == DTS.
*/
static void test_two_tracks_interleaved(void)
{
    memfile_t f = { 0, 0, 0 };
    MP4E_mux_t *mux;
    MP4D_demux_t mp4;
    scan_t sc;
    MP4E_track_t atr;
    int vtr, atr_id, i;
    uint8_t payload[8];
    static const uint8_t dsi[] = { 0x11, 0x90 };
    static const int32_t v_offsets[3] = { 6000, -1500, 0 };
    static const uint64_t v_dts[3] = { 0, 3000, 6000 };
    static const uint64_t a_dts[3] = { 0, 1024, 2048 };

    memset(&sc, 0, sizeof(sc));
    fill(payload, 8, 0x55);
    mux = MP4E_open(0, 0, &f, mem_write);
    vtr = add_video_track(mux, 90000, 0);

    memset(&atr, 0, sizeof(atr));
    atr.track_media_kind = e_audio;
    atr.language[0] = 'u';
    atr.language[1] = 'n';
    atr.language[2] = 'd';
    atr.object_type_indication = MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3;
    atr.time_scale = 48000;
    atr.default_duration = 0;
    atr.u.a.channelcount = 2;
    atr_id = MP4E_add_track(mux, &atr);
    MP4E_set_dsi(mux, atr_id, dsi, sizeof(dsi));

    for (i = 0; i < 3; i++)
    {
        CHECK(MP4E_put_sample_cts(mux, vtr, payload, 8, 3000,
            i == 0 ? MP4E_SAMPLE_RANDOM_ACCESS : MP4E_SAMPLE_DEFAULT, v_offsets[i]) == MP4E_STATUS_OK);
        CHECK(MP4E_put_sample(mux, atr_id, payload, 4, 1024, MP4E_SAMPLE_DEFAULT) == MP4E_STATUS_OK);
    }
    CHECK(MP4E_close(mux) == MP4E_STATUS_OK);

    scan_boxes(f.data, 0, f.size, &sc);
    CHECK(sc.ctts_count == 1); // video track only
    CHECK(sc.ctts_version == 1);

    CHECK(MP4D_open(&mp4, mem_read, &f, (int64_t)f.size));
    CHECK(mp4.track_count == 2);
    for (i = 0; i < 3; i++)
    {
        uint64_t dts = 0;
        int64_t pts = 0;
        uint32_t dur = 0;
        CHECK(MP4D_frame_timing(&mp4, 0, (unsigned)i, &dts, &pts, &dur));
        CHECK(dts == v_dts[i]);
        CHECK(pts == (int64_t)v_dts[i] + v_offsets[i]);
        CHECK(dur == 3000);
        CHECK(MP4D_frame_timing(&mp4, 1, (unsigned)i, &dts, &pts, &dur));
        CHECK(dts == a_dts[i]);
        CHECK(pts == (int64_t)a_dts[i]); // audio track has no ctts
        CHECK(dur == 1024);
    }
    MP4D_close(&mp4);
    scan_free(&sc);
    memfile_free(&f);
}

/*
    Continuation error paths: orphan continuation and offset mismatch are
    rejected with MP4E_STATUS_BAD_ARGUMENTS, and the rejected slice's bytes
    never reach the file.
*/
static void test_continuation_rejects(void)
{
    memfile_t f = { 0, 0, 0 };
    MP4E_mux_t *mux;
    MP4D_demux_t mp4;
    scan_t sc;
    int tr;

    memset(&sc, 0, sizeof(sc));
    mux = MP4E_open(0, 0, &f, mem_write);
    tr = add_video_track(mux, 90000, 0);

    // continuation with no preceding sample
    CHECK(MP4E_put_sample_cts(mux, tr, "XX", 2, 100, MP4E_SAMPLE_CONTINUATION, 0) == MP4E_STATUS_BAD_ARGUMENTS);
    CHECK(MP4E_put_sample_cts(mux, tr, "AAAA", 4, 100, MP4E_SAMPLE_RANDOM_ACCESS, 7) == MP4E_STATUS_OK);
    CHECK(MP4E_put_sample_cts(mux, tr, "BB", 2, 100, MP4E_SAMPLE_CONTINUATION, 7) == MP4E_STATUS_OK);
    // continuation with a different offset than the sample it extends
    CHECK(MP4E_put_sample_cts(mux, tr, "CC", 2, 100, MP4E_SAMPLE_CONTINUATION, 8) == MP4E_STATUS_BAD_ARGUMENTS);
    CHECK(MP4E_put_sample_cts(mux, tr, "DDDD", 4, 100, MP4E_SAMPLE_DEFAULT, 0) == MP4E_STATUS_OK);
    CHECK(MP4E_close(mux) == MP4E_STATUS_OK);

    scan_boxes(f.data, 0, f.size, &sc);
    CHECK(sc.mdat_size == 10); // "AAAA" + "BB" + "DDDD"; "XX"/"CC" never written
    CHECK(!memcmp(sc.mdat, "AAAABBDDDD", 10));

    CHECK(MP4D_open(&mp4, mem_read, &f, (int64_t)f.size));
    CHECK(mp4.track[0].sample_count == 2);
    {
        uint64_t dts = 0;
        int64_t pts = 0;
        uint32_t dur = 0;
        unsigned frame_bytes = 0;
        CHECK(MP4D_frame_timing(&mp4, 0, 0, &dts, &pts, &dur));
        CHECK(pts == 7 && dur == 100);
        CHECK(MP4D_frame_timing(&mp4, 0, 1, &dts, &pts, &dur));
        CHECK(dts == 100 && pts == 100);
        MP4D_frame_offset(&mp4, 0, 0, &frame_bytes, 0, 0);
        CHECK(frame_bytes == 6);
    }
    MP4D_close(&mp4);
    scan_free(&sc);
    memfile_free(&f);
}

/*
    The CTS entry point is rejected in fragmentation mode; the old entry
    point keeps working there.
*/
static void test_fragmentation_rejects_cts(void)
{
    memfile_t f = { 0, 0, 0 };
    MP4E_mux_t *mux;
    int tr;

    mux = MP4E_open(0, 1, &f, mem_write); // fragmentation mode
    tr = add_video_track(mux, 90000, 0);
    CHECK(MP4E_put_sample_cts(mux, tr, "AAAA", 4, 100, MP4E_SAMPLE_RANDOM_ACCESS, 5) == MP4E_STATUS_BAD_ARGUMENTS);
    CHECK(MP4E_put_sample(mux, tr, "AAAA", 4, 100, MP4E_SAMPLE_RANDOM_ACCESS) == MP4E_STATUS_OK);
    CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
    memfile_free(&f);
}

/*
    Hand-crafted ctts version 0 file, placed *before* stts/stsz in stbl,
    carrying an offset above INT32_MAX: version 0 offsets are unsigned.
*/
static void test_crafted_ctts_v0(void)
{
    memfile_t f = { 0, 0, 0 };
    MP4D_demux_t mp4;
    static const uint32_t entries[] = { 1, 0x90000000u, 2, 5 };
    int i;

    craft_mp4(&f, 0, 1 /* ctts before stts */, 0, 0, 0, entries, 2);
    CHECK(MP4D_open(&mp4, mem_read, &f, (int64_t)f.size));
    for (i = 0; i < 3; i++)
    {
        uint64_t dts = 0;
        int64_t pts = 0;
        uint32_t dur = 0;
        CHECK(MP4D_frame_timing(&mp4, 0, (unsigned)i, &dts, &pts, &dur));
        CHECK(dts == (uint64_t)i*100);
        CHECK(dur == 100);
        if (i == 0)
            CHECK(pts == 0x90000000LL); // > INT32_MAX, interpreted unsigned
        else
            CHECK(pts == (int64_t)i*100 + 5);
    }
    MP4D_close(&mp4);
    memfile_free(&f);
}

/*
    Hand-crafted ctts version 1 file: offsets are signed 32-bit.
*/
static void test_crafted_ctts_v1(void)
{
    memfile_t f = { 0, 0, 0 };
    MP4D_demux_t mp4;
    static const uint32_t entries[] = { 2, 0xFFFFFFFEu, 1, 10 };
    static const int64_t pts_exp[3] = { -2, 98, 210 };
    int i;

    craft_mp4(&f, 1, 0, 0, 0, 0, entries, 2);
    CHECK(MP4D_open(&mp4, mem_read, &f, (int64_t)f.size));
    for (i = 0; i < 3; i++)
    {
        uint64_t dts = 0;
        int64_t pts = 0;
        uint32_t dur = 0;
        CHECK(MP4D_frame_timing(&mp4, 0, (unsigned)i, &dts, &pts, &dur));
        CHECK(dts == (uint64_t)i*100);
        CHECK(pts == pts_exp[i]);
    }
    MP4D_close(&mp4);
    memfile_free(&f);
}

/*
    Malformed ctts boxes must be rejected by MP4D_open().
*/
static void test_crafted_ctts_rejects(void)
{
    static const uint32_t ok_entries[] = { 1, 5, 2, 7 };
    static const uint32_t mismatch_entries[] = { 2, 7 };        // covers 2 of 3 samples
    static const uint32_t zero_run_entries[] = { 0, 5, 3, 7 };  // zero-length run
    memfile_t f;
    MP4D_demux_t mp4;

#define CHECK_REJECT(version, before, dup, zero, trunc, entries, n) \
    do { \
        memset(&f, 0, sizeof(f)); \
        craft_mp4(&f, version, before, dup, zero, trunc, entries, n); \
        CHECK(!MP4D_open(&mp4, mem_read, &f, (int64_t)f.size)); \
        memfile_free(&f); \
    } while (0)

    CHECK_REJECT(2, 0, 0, 0, 0, ok_entries, 2);        // unknown ctts version
    CHECK_REJECT(0, 0, 1, 0, 0, ok_entries, 2);      // duplicate ctts box
    CHECK_REJECT(0, 0, 0, 1, 0, ok_entries, 2);      // zero entry count
    CHECK_REJECT(0, 0, 0, 0, 1, ok_entries, 2);      // truncated entry table
    CHECK_REJECT(0, 0, 0, 0, 0, mismatch_entries, 1);// run total != sample count
    CHECK_REJECT(0, 0, 0, 0, 0, zero_run_entries, 2);// zero-length run
#undef CHECK_REJECT
}

int main(void)
{
    test_basic_roundtrip();
    test_zero_offsets_no_ctts();
    test_sequential_and_continuation();
    test_two_tracks_interleaved();
    test_continuation_rejects();
    test_fragmentation_rejects_cts();
    test_crafted_ctts_v0();
    test_crafted_ctts_v1();
    test_crafted_ctts_rejects();

    if (failures)
    {
        printf("cts: %d check(s) failed\n", failures);
        return 1;
    }
    printf("cts: all checks passed\n");
    return 0;
}
