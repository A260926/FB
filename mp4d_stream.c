/*
 * Incremental fragmented-MP4 demuxer.
 *
 * The initialization segment (ftyp/moov) is buffered only until it is
 * complete, then handed to the existing MP4D parser for track and decoder
 * configuration extraction.  After that every moof is parsed into a compact
 * sample table which is checked against the following mdat; sample payloads
 * are streamed through a single one-sample buffer.
 */
#include "mp4d_stream.h"
#include "minimp4.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FOURCC(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

#define BOX_FTYP FOURCC('f', 't', 'y', 'p')
#define BOX_MOOV FOURCC('m', 'o', 'o', 'v')
#define BOX_MOOF FOURCC('m', 'o', 'o', 'f')
#define BOX_MDAT FOURCC('m', 'd', 'a', 't')
#define BOX_TRAK FOURCC('t', 'r', 'a', 'k')
#define BOX_TKHD FOURCC('t', 'k', 'h', 'd')
#define BOX_MDIA FOURCC('m', 'd', 'i', 'a')
#define BOX_MDHD FOURCC('m', 'd', 'h', 'd')
#define BOX_MINF FOURCC('m', 'i', 'n', 'f')
#define BOX_STBL FOURCC('s', 't', 'b', 'l')
#define BOX_STSD FOURCC('s', 't', 's', 'd')
#define BOX_DINF FOURCC('d', 'i', 'n', 'f')
#define BOX_DREF FOURCC('d', 'r', 'e', 'f')
#define BOX_URL_ FOURCC('u', 'r', 'l', ' ')
#define BOX_MVEX FOURCC('m', 'v', 'e', 'x')
#define BOX_TREX FOURCC('t', 'r', 'e', 'x')
#define BOX_MFHD FOURCC('m', 'f', 'h', 'd')
#define BOX_TRAF FOURCC('t', 'r', 'a', 'f')
#define BOX_TFHD FOURCC('t', 'f', 'h', 'd')
#define BOX_TFDT FOURCC('t', 'f', 'd', 't')
#define BOX_TRUN FOURCC('t', 'r', 'u', 'n')
#define BOX_SENC FOURCC('s', 'e', 'n', 'c')
#define BOX_SAIZ FOURCC('s', 'a', 'i', 'z')
#define BOX_SAIO FOURCC('s', 'a', 'i', 'o')
#define BOX_SCHM FOURCC('s', 'c', 'h', 'm')

#define TFHD_BASE_DATA_OFFSET       0x000001u
#define TFHD_SAMPLE_DESC_INDEX      0x000002u
#define TFHD_DEFAULT_DURATION       0x000008u
#define TFHD_DEFAULT_SIZE           0x000010u
#define TFHD_DEFAULT_FLAGS          0x000020u
#define TFHD_DURATION_IS_EMPTY      0x010000u
#define TFHD_DEFAULT_BASE_IS_MOOF   0x020000u
#define TFHD_KNOWN_FLAGS (TFHD_BASE_DATA_OFFSET | TFHD_SAMPLE_DESC_INDEX | \
    TFHD_DEFAULT_DURATION | TFHD_DEFAULT_SIZE | TFHD_DEFAULT_FLAGS | \
    TFHD_DURATION_IS_EMPTY | TFHD_DEFAULT_BASE_IS_MOOF)

#define TRUN_DATA_OFFSET            0x000001u
#define TRUN_FIRST_SAMPLE_FLAGS     0x000004u
#define TRUN_SAMPLE_DURATION        0x000100u
#define TRUN_SAMPLE_SIZE            0x000200u
#define TRUN_SAMPLE_FLAGS           0x000400u
#define TRUN_SAMPLE_CTO             0x000800u
#define TRUN_KNOWN_FLAGS (TRUN_DATA_OFFSET | TRUN_FIRST_SAMPLE_FLAGS | \
    TRUN_SAMPLE_DURATION | TRUN_SAMPLE_SIZE | TRUN_SAMPLE_FLAGS | TRUN_SAMPLE_CTO)

enum
{
    ST_HEADER = 0,
    ST_INIT_BOX,
    ST_MOOF,
    ST_SKIP,
    ST_MDAT_INIT,
    ST_MDAT
};

typedef struct
{
    uint64_t offset;
    uint64_t dts;
    uint64_t duration;
    int64_t cto;
    int64_t pts;
    uint32_t size;
    uint32_t flags;
    uint32_t track;
} sample_desc_t;

typedef struct
{
    uint32_t track_id;
    uint32_t timescale;
    int has_trex;
    uint32_t trex_duration;
    uint32_t trex_size;
    uint32_t trex_flags;
    uint64_t next_dts;
} stream_track_t;

typedef struct
{
    uint32_t track_id;
    uint32_t desc_index;
    uint32_t duration;
    uint32_t size;
    uint32_t flags;
} trex_def_t;

typedef struct
{
    uint32_t track_id;
    int started;
    int any_trun;
    int has_base;
    uint64_t base;
    int has_default_duration;
    int has_default_size;
    int has_default_flags;
    uint32_t default_duration;
    uint32_t default_size;
    uint32_t default_flags;
    uint64_t run_pos;
    int has_tfdt;
    uint64_t tfdt;
    size_t first_sample;
    uint32_t track_index;
} traf_ctx_t;

struct MP4D_stream
{
    size_t limit;
    MP4D_stream_cb cb;
    void *token;

    int failed;
    int finished_ok;
    int state;
    uint64_t pos;

    unsigned char hdr[16];
    size_t hdr_len;

    int init_done;
    unsigned char *init_buf;
    size_t init_len;
    size_t init_cap;
    int init_box_is_moov;

    uint32_t box_name;
    uint64_t box_start;
    uint64_t remaining;

    int have_moof;
    unsigned char *moof_buf;
    size_t moof_len;
    uint64_t moof_total;

    sample_desc_t *samples;
    size_t sample_count;
    size_t sample_cap;
    size_t sample_idx;
    unsigned char *sample_buf;
    uint64_t sample_got;
    uint64_t mdat_end;
    int mdat_finished;

    stream_track_t *tracks;
    size_t track_count;
    size_t track_cap;

    trex_def_t *trex;
    size_t trex_count;
    size_t trex_cap;

    MP4D_demux_t demux;
    int demux_open;

    unsigned char zero_byte;
};

typedef struct
{
    const unsigned char *p;
    const unsigned char *end;
} box_reader_t;

static int st_fail_line(MP4D_stream_t *st, int line)
{
#ifdef MP4D_STREAM_DEBUG
    fprintf(stderr, "stream fail line=%d state=%d pos=%llu box=%08x hdr_len=%zu remaining=%llu "
        "sidx=%zu scount=%zu sgot=%llu mend=%llu\n",
        line, st->state, (unsigned long long)st->pos, st->box_name,
        st->hdr_len, (unsigned long long)st->remaining,
        st->sample_idx, st->sample_count, (unsigned long long)st->sample_got,
        (unsigned long long)st->mdat_end);
#else
    (void)line;
#endif
    st->failed = 1;
    return -1;
}
#define st_fail(st) st_fail_line((st), __LINE__)

static uint32_t rd32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd64(const unsigned char *p)
{
    return ((uint64_t)rd32(p) << 32) | (uint64_t)rd32(p + 4);
}

/* Read the next box from a bounded in-memory buffer. */
static int next_box(box_reader_t *r, uint32_t *name, const unsigned char **payload,
    size_t *payload_bytes)
{
    uint64_t size;
    size_t header;
    uint32_t size32;

    if (r->p >= r->end)
        return 0;
    if ((size_t)(r->end - r->p) < 8)
        return -1;
    size32 = rd32(r->p);
    if (size32 == 1)
    {
        if ((size_t)(r->end - r->p) < 16)
            return -1;
        size = rd64(r->p + 8);
        if (size < 16)
            return -1;
        header = 16;
    } else
    {
        if (size32 < 8)
            return -1;
        size = size32;
        header = 8;
    }
    if (size > (uint64_t)(r->end - r->p))
        return -1;
    *name = rd32(r->p + 4);
    *payload = r->p + header;
    *payload_bytes = (size_t)(size - header);
    r->p += (size_t)size;
    return 1;
}

static int grow_array(void **ptr, size_t *cap, size_t need, size_t elem)
{
    size_t ncap;
    void *mem;
    if (need <= *cap)
        return 0;
    ncap = *cap ? *cap : 8;
    while (ncap < need)
    {
        if (ncap > (size_t)-1 / 2)
        {
            ncap = need;
            break;
        }
        ncap *= 2;
    }
    if (elem && ncap > (size_t)-1 / elem)
        return -1;
    mem = realloc(*ptr, ncap * elem);
    if (!mem)
        return -1;
    *ptr = mem;
    *cap = ncap;
    return 0;
}

static int init_append(MP4D_stream_t *st, const void *data, size_t bytes)
{
    size_t need;
    if (bytes > st->limit || st->init_len > st->limit - bytes)
        return -1;
    need = st->init_len + bytes;
    if (need > st->init_cap)
    {
        size_t ncap = st->init_cap ? st->init_cap : 256;
        unsigned char *mem;
        while (ncap < need)
            ncap *= 2;
        if (ncap > st->limit)
            ncap = st->limit;
        if (ncap < need)
            ncap = need;
        mem = (unsigned char *)realloc(st->init_buf, ncap);
        if (!mem)
            return -1;
        st->init_buf = mem;
        st->init_cap = ncap;
    }
    memcpy(st->init_buf + st->init_len, data, bytes);
    st->init_len += bytes;
    return 0;
}

static stream_track_t *init_add_track(MP4D_stream_t *st)
{
    stream_track_t *tr;
    if (grow_array((void **)&st->tracks, &st->track_cap, st->track_count + 1,
        sizeof(stream_track_t)))
        return NULL;
    tr = &st->tracks[st->track_count++];
    memset(tr, 0, sizeof(*tr));
    return tr;
}

static int trex_add(MP4D_stream_t *st, const trex_def_t *def)
{
    size_t i;
    for (i = 0; i < st->trex_count; ++i)
        if (st->trex[i].track_id == def->track_id)
            return -1;
    if (grow_array((void **)&st->trex, &st->trex_cap, st->trex_count + 1,
        sizeof(trex_def_t)))
        return -1;
    st->trex[st->trex_count++] = *def;
    return 0;
}

static stream_track_t *find_track_by_id(MP4D_stream_t *st, uint32_t track_id)
{
    size_t i;
    for (i = 0; i < st->track_count; ++i)
        if (st->tracks[i].track_id == track_id)
            return &st->tracks[i];
    return NULL;
}

static int is_encrypted_entry(uint32_t type)
{
    /* encv, enca, encs, enct, encm, encf, ... */
    return (type >> 8) == ((FOURCC('e', 'n', 'c', 0)) >> 8);
}

static int parse_stsd(MP4D_stream_t *st, const unsigned char *p, size_t bytes,
    int *track_index)
{
    uint32_t count, entry_size, entry_type;
    size_t off;
    (void)st;
    if (bytes < 8)
        return -1;
    count = rd32(p + 4);
    if (count != 1)
        return -1;
    off = 8;
    /* SampleEntry: size, type, 6 reserved bytes, then data_reference_index. */
    if (bytes - off < 16)
        return -1;
    entry_size = rd32(p + off);
    entry_type = rd32(p + off + 4);
    if (entry_size < 16 || (uint64_t)entry_size > (uint64_t)(bytes - off))
        return -1;
    if (is_encrypted_entry(entry_type))
        return -1;
    if ((((unsigned)p[off + 14] << 8) | p[off + 15]) != 1)
        return -1; /* only the implicit self-contained data reference is supported */
    *track_index = 0; /* marks that a sample description was found */
    return 0;
}

static int parse_stbl(MP4D_stream_t *st, const unsigned char *p, size_t bytes,
    stream_track_t *tr, int *found_stsd)
{
    box_reader_t r;
    uint32_t name;
    const unsigned char *payload;
    size_t plen;
    int rc;
    (void)tr;
    r.p = p;
    r.end = p + bytes;
    while ((rc = next_box(&r, &name, &payload, &plen)) > 0)
    {
        if (name == BOX_STSD)
        {
            int dummy;
            if (found_stsd && *found_stsd)
                return -1;
            if (parse_stsd(st, payload, plen, &dummy))
                return -1;
            if (found_stsd)
                *found_stsd = 1;
        }
    }
    return rc < 0 ? -1 : 0;
}

/* Reject data references other than the built-in 'self-contained' url box:
 * media data must live in the file itself. */
static int parse_dref(const unsigned char *p, size_t bytes)
{
    box_reader_t r;
    uint32_t name, count, i;
    const unsigned char *payload;
    size_t plen;
    int rc;

    if (bytes < 8)
        return -1;
    count = rd32(p + 4);
    r.p = p + 8;
    r.end = p + bytes;
    for (i = 0; i < count; ++i)
    {
        if ((rc = next_box(&r, &name, &payload, &plen)) <= 0)
            return -1;
        if (name != BOX_URL_ || plen < 4)
            return -1;
        if (!(rd32(payload) & 1))
            return -1; /* external (non self-contained) URL */
    }
    return 0;
}

static int parse_dinf(const unsigned char *p, size_t bytes)
{
    box_reader_t r;
    uint32_t name;
    const unsigned char *payload;
    size_t plen;
    int rc;

    r.p = p;
    r.end = p + bytes;
    while ((rc = next_box(&r, &name, &payload, &plen)) > 0)
        if (name == BOX_DREF && parse_dref(payload, plen))
            return -1;
    return rc < 0 ? -1 : 0;
}

static int parse_minf(MP4D_stream_t *st, const unsigned char *p, size_t bytes,
    stream_track_t *tr, int *found_stsd)
{
    box_reader_t r;
    uint32_t name;
    const unsigned char *payload;
    size_t plen;
    int rc;
    (void)tr;
    r.p = p;
    r.end = p + bytes;
    while ((rc = next_box(&r, &name, &payload, &plen)) > 0)
    {
        if (name == BOX_STBL && parse_stbl(st, payload, plen, tr, found_stsd))
            return -1;
        if (name == BOX_DINF && parse_dinf(payload, plen))
            return -1;
    }
    return rc < 0 ? -1 : 0;
}

static int parse_mdhd(stream_track_t *tr, const unsigned char *p, size_t bytes)
{
    unsigned version;
    size_t off;
    if (bytes < 4)
        return -1;
    version = p[0];
    if (version > 1)
        return -1;
    off = 4 + (version == 1 ? 16 : 8);
    if (bytes < off + 4)
        return -1;
    tr->timescale = rd32(p + off);
    return tr->timescale ? 0 : -1;
}

static int parse_mdia(MP4D_stream_t *st, const unsigned char *p, size_t bytes,
    stream_track_t *tr, int *found_mdhd, int *found_stsd)
{
    box_reader_t r;
    uint32_t name;
    const unsigned char *payload;
    size_t plen;
    int rc;
    r.p = p;
    r.end = p + bytes;
    while ((rc = next_box(&r, &name, &payload, &plen)) > 0)
    {
        if (name == BOX_MDHD)
        {
            if (*found_mdhd)
                return -1;
            if (parse_mdhd(tr, payload, plen))
                return -1;
            *found_mdhd = 1;
        } else if (name == BOX_MINF)
        {
            if (parse_minf(st, payload, plen, tr, found_stsd))
                return -1;
        }
    }
    return rc < 0 ? -1 : 0;
}

static int parse_tkhd(stream_track_t *tr, const unsigned char *p, size_t bytes)
{
    unsigned version;
    size_t off;
    if (bytes < 4)
        return -1;
    version = p[0];
    if (version > 1)
        return -1;
    off = 4 + (version == 1 ? 16 : 8);
    if (bytes < off + 4)
        return -1;
    tr->track_id = rd32(p + off);
    return tr->track_id ? 0 : -1;
}

static int parse_trak(MP4D_stream_t *st, const unsigned char *p, size_t bytes,
    stream_track_t *tr)
{
    box_reader_t r;
    uint32_t name;
    const unsigned char *payload;
    size_t plen;
    int rc, found_tkhd = 0, found_mdhd = 0, found_stsd = 0;
    r.p = p;
    r.end = p + bytes;
    while ((rc = next_box(&r, &name, &payload, &plen)) > 0)
    {
        if (name == BOX_TKHD)
        {
            if (found_tkhd)
                return -1;
            if (parse_tkhd(tr, payload, plen))
                return -1;
            found_tkhd = 1;
        } else if (name == BOX_MDIA)
        {
            if (parse_mdia(st, payload, plen, tr, &found_mdhd, &found_stsd))
                return -1;
        }
    }
    if (rc < 0 || !found_tkhd || !found_mdhd || !found_stsd)
        return -1;
    return 0;
}

static int parse_trex(MP4D_stream_t *st, const unsigned char *p, size_t bytes)
{
    trex_def_t def;
    if (bytes < 24)
        return -1;
    def.track_id = rd32(p + 4);
    def.desc_index = rd32(p + 8);
    def.duration = rd32(p + 12);
    def.size = rd32(p + 16);
    def.flags = rd32(p + 20);
    if (!def.track_id || def.desc_index != 1)
        return -1;
    return trex_add(st, &def);
}

static int parse_mvex(MP4D_stream_t *st, const unsigned char *p, size_t bytes,
    int *found_mvex)
{
    box_reader_t r;
    uint32_t name;
    const unsigned char *payload;
    size_t plen;
    int rc;
    *found_mvex = 1;
    r.p = p;
    r.end = p + bytes;
    while ((rc = next_box(&r, &name, &payload, &plen)) > 0)
        if (name == BOX_TREX && parse_trex(st, payload, plen))
            return -1;
    return rc < 0 ? -1 : 0;
}

static int parse_moov(MP4D_stream_t *st, const unsigned char *p, size_t bytes,
    int *found_mvex)
{
    box_reader_t r;
    uint32_t name;
    const unsigned char *payload;
    size_t plen;
    int rc;
    r.p = p;
    r.end = p + bytes;
    while ((rc = next_box(&r, &name, &payload, &plen)) > 0)
    {
        if (name == BOX_TRAK)
        {
            stream_track_t *tr = init_add_track(st);
            if (!tr || parse_trak(st, payload, plen, tr))
                return -1;
        } else if (name == BOX_MVEX)
        {
            if (*found_mvex)
                return -1;
            if (parse_mvex(st, payload, plen, found_mvex))
                return -1;
        }
    }
    return rc < 0 ? -1 : 0;
}

/* Buffer-backed read callback used to run the regular MP4D parser over the
 * buffered initialization segment. */
typedef struct
{
    const unsigned char *data;
    size_t bytes;
} init_reader_t;

static int init_read_cb(int64_t offset, void *buffer, size_t bytes, void *token)
{
    init_reader_t *r = (init_reader_t *)token;
    if (offset < 0 || (uint64_t)offset > r->bytes)
        return 1;
    if (bytes > r->bytes - (size_t)offset)
        return 1;
    memcpy(buffer, r->data + (size_t)offset, bytes);
    return 0;
}

static int finalize_init(MP4D_stream_t *st)
{
    box_reader_t r;
    uint32_t name;
    const unsigned char *payload;
    size_t plen;
    int rc, found_moov = 0, found_mvex = 0;
    size_t i, j;
    init_reader_t ir;

    r.p = st->init_buf;
    r.end = st->init_buf + st->init_len;
    while ((rc = next_box(&r, &name, &payload, &plen)) > 0)
    {
        if (name == BOX_MOOV)
        {
            if (found_moov)
                return -1;
            found_moov = 1;
            if (parse_moov(st, payload, plen, &found_mvex))
                return -1;
        }
    }
    if (rc < 0 || !found_moov || !found_mvex || !st->track_count || !st->trex_count)
        return -1;

    /* Track IDs must be unique, and every trex must name a real track. */
    for (i = 0; i < st->track_count; ++i)
        for (j = i + 1; j < st->track_count; ++j)
            if (st->tracks[i].track_id == st->tracks[j].track_id)
                return -1;
    for (i = 0; i < st->trex_count; ++i)
    {
        stream_track_t *tr = find_track_by_id(st, st->trex[i].track_id);
        if (!tr || tr->has_trex)
            return -1;
        tr->has_trex = 1;
        tr->trex_duration = st->trex[i].duration;
        tr->trex_size = st->trex[i].size;
        tr->trex_flags = st->trex[i].flags;
        tr->next_dts = 0;
    }

    ir.data = st->init_buf;
    ir.bytes = st->init_len;
    memset(&st->demux, 0, sizeof(st->demux));
    if (!MP4D_open(&st->demux, init_read_cb, &ir, (int64_t)st->init_len))
        return -1;
    st->demux_open = 1;
    if (st->demux.track_count != st->track_count)
        return -1;

    free(st->init_buf);
    st->init_buf = NULL;
    st->init_len = 0;
    st->init_cap = 0;
    free(st->trex);
    st->trex = NULL;
    st->trex_count = 0;
    st->trex_cap = 0;
    st->init_done = 1;
    return 0;
}

static int samples_reserve(MP4D_stream_t *st, size_t need)
{
    size_t ncap;
    uint64_t budget, bytes;
    sample_desc_t *mem;

    if (need <= st->sample_cap)
        return 0;
    /* The moof payload and its derived sample table share one limit. */
    if (st->moof_total > st->limit)
        return -1;
    budget = (uint64_t)st->limit - st->moof_total;
    if ((uint64_t)need > budget / sizeof(sample_desc_t))
        return -1;
    ncap = st->sample_cap ? st->sample_cap : 16;
    while (ncap < need)
    {
        if (ncap > (size_t)-1 / 2)
        {
            ncap = need;
            break;
        }
        ncap *= 2;
    }
    if ((uint64_t)ncap > budget / sizeof(sample_desc_t))
        ncap = need;
    bytes = (uint64_t)ncap * sizeof(sample_desc_t);
    if (bytes > (uint64_t)(size_t)-1)
        return -1;
    mem = (sample_desc_t *)realloc(st->samples, (size_t)bytes);
    if (!mem)
        return -1;
    st->samples = mem;
    st->sample_cap = ncap;
    return 0;
}

static int parse_trun(MP4D_stream_t *st, traf_ctx_t *tc, stream_track_t *trk,
    const unsigned char *p, size_t bytes)
{
    uint32_t version_flags, flags, count, first_flags = 0, i;
    unsigned version;
    size_t off, fixed, per;
    int has_off, has_first, has_dur, has_size, has_flags, has_cto;
    uint64_t run_start, run_pos;
    int32_t data_offset = 0;

    if (bytes < 4)
        return -1;
    version_flags = rd32(p);
    version = (unsigned)(version_flags >> 24);
    flags = version_flags & 0xFFFFFFu;
    if (version > 1 || (flags & ~TRUN_KNOWN_FLAGS))
        return -1;
    has_off = (flags & TRUN_DATA_OFFSET) != 0;
    has_first = (flags & TRUN_FIRST_SAMPLE_FLAGS) != 0;
    has_dur = (flags & TRUN_SAMPLE_DURATION) != 0;
    has_size = (flags & TRUN_SAMPLE_SIZE) != 0;
    has_flags = (flags & TRUN_SAMPLE_FLAGS) != 0;
    has_cto = (flags & TRUN_SAMPLE_CTO) != 0;
    if (has_first && has_flags)
        return -1;

    off = 4;
    if (bytes - off < 4)
        return -1;
    count = rd32(p + off);
    off += 4;
    fixed = 4 + (has_off ? 4u : 0u) + (has_first ? 4u : 0u);
    per = (has_dur ? 4u : 0u) + (has_size ? 4u : 0u) +
        (has_flags ? 4u : 0u) + (has_cto ? 4u : 0u);
    if (bytes < fixed)
        return -1;
    if (per && (uint64_t)count > (uint64_t)(bytes - fixed) / per)
        return -1;
    if ((uint64_t)fixed + (uint64_t)count * per > (uint64_t)bytes)
        return -1;

    if (has_off)
    {
        data_offset = (int32_t)rd32(p + off);
        off += 4;
    }
    if (has_first)
    {
        first_flags = rd32(p + off);
        off += 4;
    }

    if (has_off)
    {
        if (data_offset >= 0)
        {
            if ((uint64_t)data_offset > UINT64_MAX - tc->base)
                return -1;
            run_start = tc->base + (uint64_t)data_offset;
        } else
        {
            uint64_t mag = (uint64_t)(-(int64_t)data_offset);
            if (mag > tc->base)
                return -1;
            run_start = tc->base - mag;
        }
    } else
    {
        run_start = tc->run_pos;
    }
    run_pos = run_start;

    if ((uint64_t)st->sample_count + count > (uint64_t)(size_t)-1)
        return -1;
    if (samples_reserve(st, st->sample_count + count))
        return -1;

    for (i = 0; i < count; ++i)
    {
        sample_desc_t *s = &st->samples[st->sample_count];
        uint32_t duration = 0, size = 0, sflags = 0;
        int64_t cto = 0;

        if (has_dur)
        {
            duration = rd32(p + off);
            off += 4;
        } else if (tc->has_default_duration)
            duration = tc->default_duration;
        else if (trk->has_trex)
            duration = trk->trex_duration;
        else
            return -1;

        if (has_size)
        {
            size = rd32(p + off);
            off += 4;
        } else if (tc->has_default_size)
            size = tc->default_size;
        else if (trk->has_trex)
            size = trk->trex_size;
        else
            return -1;

        if (has_flags)
        {
            sflags = rd32(p + off);
            off += 4;
        } else if (i == 0 && has_first)
            sflags = first_flags;
        else if (tc->has_default_flags)
            sflags = tc->default_flags;
        else if (trk->has_trex)
            sflags = trk->trex_flags;
        else
            return -1;

        if (has_cto)
        {
            uint32_t raw = rd32(p + off);
            off += 4;
            cto = (version == 1) ? (int64_t)(int32_t)raw : (int64_t)(uint64_t)raw;
        }

        if ((uint64_t)size > (uint64_t)st->limit)
            return -1;
        if (run_pos > UINT64_MAX - size)
            return -1;

        s->offset = run_pos;
        s->duration = duration;
        s->size = size;
        s->flags = sflags;
        s->cto = cto;
        s->dts = 0;
        s->pts = 0;
        s->track = tc->track_index;
        run_pos += size;
        ++st->sample_count;
    }

    tc->run_pos = run_pos;
    return 0;
}

static int finalize_traf_dts(MP4D_stream_t *st, traf_ctx_t *tc, stream_track_t *trk)
{
    uint64_t dts = tc->has_tfdt ? tc->tfdt : trk->next_dts;
    size_t i;

    if (tc->has_tfdt && tc->tfdt < trk->next_dts)
        return -1;

    for (i = tc->first_sample; i < st->sample_count; ++i)
    {
        sample_desc_t *s = &st->samples[i];
        int64_t pts;
        if (s->cto >= 0)
        {
            if (dts > (uint64_t)INT64_MAX ||
                (uint64_t)s->cto > (uint64_t)INT64_MAX - dts)
                return -1;
            pts = (int64_t)dts + s->cto;
        } else
        {
            uint64_t mag = (uint64_t)(-(s->cto));
            if (dts >= mag)
            {
                if (dts - mag > (uint64_t)INT64_MAX)
                    return -1;
                pts = (int64_t)(dts - mag);
            } else
            {
                uint64_t neg = mag - dts;
                uint64_t min_mag = (uint64_t)INT64_MAX + 1u;
                if (neg > min_mag)
                    return -1;
                pts = (neg == min_mag) ? INT64_MIN : -(int64_t)neg;
            }
        }
        s->dts = dts;
        s->pts = pts;
        if (dts > UINT64_MAX - s->duration)
            return -1;
        dts += s->duration;
    }
    trk->next_dts = dts;
    return 0;
}

static int parse_tfhd(MP4D_stream_t *st, traf_ctx_t *tc, const unsigned char *p,
    size_t bytes)
{
    uint32_t version_flags, flags;
    size_t off = 4;
    stream_track_t *trk;

    if (bytes < 8)
        return -1;
    version_flags = rd32(p);
    if ((version_flags >> 24) != 0)
        return -1;
    flags = version_flags & 0xFFFFFFu;
    if (flags & ~TFHD_KNOWN_FLAGS)
        return -1;
    /* TFHD_DURATION_IS_EMPTY only annotates that the tfhd carries no
     * duration; inheritance then falls through to trex. */
    tc->track_id = rd32(p + off);
    off += 4;
    trk = find_track_by_id(st, tc->track_id);
    if (!trk)
        return -1;
    tc->track_index = (uint32_t)(trk - st->tracks);

    if (flags & TFHD_BASE_DATA_OFFSET)
    {
        if (bytes - off < 8)
            return -1;
        tc->base = rd64(p + off);
        off += 8;
        tc->has_base = 1;
    } else if (flags & TFHD_DEFAULT_BASE_IS_MOOF)
    {
        tc->base = st->box_start;
        tc->has_base = 1;
    }
    if (!tc->has_base)
    {
        /* The implicit tfhd base is deliberately unsupported. */
        return -1;
    }

    if (flags & TFHD_SAMPLE_DESC_INDEX)
    {
        if (bytes - off < 4)
            return -1;
        if (rd32(p + off) != 1)
            return -1;
        off += 4;
    }
    if (flags & TFHD_DEFAULT_DURATION)
    {
        if (bytes - off < 4)
            return -1;
        tc->default_duration = rd32(p + off);
        tc->has_default_duration = 1;
        off += 4;
    }
    if (flags & TFHD_DEFAULT_SIZE)
    {
        if (bytes - off < 4)
            return -1;
        tc->default_size = rd32(p + off);
        tc->has_default_size = 1;
        off += 4;
    }
    if (flags & TFHD_DEFAULT_FLAGS)
    {
        if (bytes - off < 4)
            return -1;
        tc->default_flags = rd32(p + off);
        tc->has_default_flags = 1;
        off += 4;
    }
    tc->run_pos = tc->base;
    return 0;
}

static int parse_tfdt(traf_ctx_t *tc, const unsigned char *p, size_t bytes)
{
    unsigned version;
    if (bytes < 4)
        return -1;
    version = p[0];
    if (tc->has_tfdt)
        return -1;
    if (version == 0)
    {
        if (bytes < 8)
            return -1;
        tc->tfdt = rd32(p + 4);
    } else if (version == 1)
    {
        if (bytes < 12)
            return -1;
        tc->tfdt = rd64(p + 4);
    } else
        return -1;
    tc->has_tfdt = 1;
    return 0;
}

static int parse_traf(MP4D_stream_t *st, traf_ctx_t *trafs, const unsigned char *p,
    size_t bytes)
{
    box_reader_t r;
    uint32_t name;
    const unsigned char *payload;
    size_t plen;
    int rc;
    traf_ctx_t *tc = NULL;
    stream_track_t *trk;

    r.p = p;
    r.end = p + bytes;
    while ((rc = next_box(&r, &name, &payload, &plen)) > 0)
    {
        if (name == BOX_TFHD)
        {
            uint32_t tid;
            if (tc || bytes < 8 || plen < 8)
                return -1; /* tfhd must be the first meaningful child */
            tid = rd32(payload + 4);
            trk = find_track_by_id(st, tid);
            if (!trk)
                return -1;
            if (trafs[trk - st->tracks].started)
                return -1; /* more than one traf for a track */
            tc = &trafs[trk - st->tracks];
            tc->first_sample = st->sample_count;
            if (parse_tfhd(st, tc, payload, plen))
                return -1;
            tc->started = 1;
        } else if (!tc)
        {
            /* Only unknown boxes may precede tfhd. */
            if (name == BOX_TFDT || name == BOX_TRUN)
                return -1;
            continue;
        } else if (name == BOX_TFDT)
        {
            if (tc->any_trun)
                return -1;
            if (parse_tfdt(tc, payload, plen))
                return -1;
        } else if (name == BOX_TRUN)
        {
            tc->any_trun = 1;
            {
                stream_track_t *t = &st->tracks[tc->track_index];
                if (parse_trun(st, tc, t, payload, plen))
                    return -1;
            }
        } else if (name == BOX_SENC || name == BOX_SAIZ || name == BOX_SAIO ||
            name == BOX_SCHM)
        {
            return -1; /* encrypted / auxiliary-data fragments are unsupported */
        }
    }
    if (rc < 0 || !tc)
        return -1;
    trk = &st->tracks[tc->track_index];
    return finalize_traf_dts(st, tc, trk);
}

static int sample_compare(const void *a, const void *b)
{
    const sample_desc_t *x = (const sample_desc_t *)a;
    const sample_desc_t *y = (const sample_desc_t *)b;
    if (x->offset < y->offset)
        return -1;
    if (x->offset > y->offset)
        return 1;
    if (x->size < y->size)
        return -1;
    if (x->size > y->size)
        return 1;
    return 0;
}

static int parse_moof(MP4D_stream_t *st)
{
    box_reader_t r;
    uint32_t name;
    const unsigned char *payload;
    size_t plen;
    int rc, found_traf = 0;
    traf_ctx_t *trafs;
    size_t i;

    if (st->moof_len != st->moof_total || st->box_name != BOX_MOOF)
        return -1;
    /* The per-traf scratch state is part of the moof description budget. */
    if (st->moof_total > st->limit ||
        st->track_count > (st->limit - st->moof_total) / sizeof(traf_ctx_t))
        return -1;
    trafs = (traf_ctx_t *)calloc(st->track_count ? st->track_count : 1,
        sizeof(traf_ctx_t));
    if (!trafs)
        return -1;

    st->sample_count = 0;
    st->sample_idx = 0;

    /* Start after the moof box header. */
    {
        size_t hdr = rd32(st->moof_buf) == 1 ? 16u : 8u;
        if (st->moof_total < hdr)
        {
            free(trafs);
            return -1;
        }
        r.p = st->moof_buf + hdr;
        r.end = st->moof_buf + st->moof_total;
    }

    while ((rc = next_box(&r, &name, &payload, &plen)) > 0)
    {
        if (name == BOX_TRAF)
        {
            if (parse_traf(st, trafs, payload, plen))
            {
                free(trafs);
                return -1;
            }
            found_traf = 1;
        }
    }
    free(trafs);
    if (rc < 0 || !found_traf)
        return -1;

    if (st->sample_count)
        qsort(st->samples, st->sample_count, sizeof(sample_desc_t), sample_compare);
    /* After sorting by address, runs must not overlap (gaps are fine). */
    for (i = 1; i < st->sample_count; ++i)
    {
        const sample_desc_t *prev = &st->samples[i - 1];
        const sample_desc_t *cur = &st->samples[i];
        if (!prev->size)
            continue;
        if ((uint64_t)prev->offset > UINT64_MAX - prev->size)
            return -1;
        if (cur->offset < prev->offset + prev->size)
            return -1;
    }
    st->have_moof = 1;
    return 0;
}

static int deliver_sample(MP4D_stream_t *st, sample_desc_t *s)
{
    MP4D_stream_sample_t out;
    stream_track_t *tr = &st->tracks[s->track];
    int rc;

    out.track_id = tr->track_id;
    out.timescale = tr->timescale;
    out.dts = s->dts;
    out.pts = s->pts;
    out.duration = s->duration;
    out.size = s->size;
    out.flags = s->flags;
    rc = st->cb(&out, s->size ? st->sample_buf : &st->zero_byte, st->token);
    free(st->sample_buf);
    st->sample_buf = NULL;
    st->sample_got = 0;
    if (rc)
        return st_fail(st);
    return 0;
}

static int mdat_validate(MP4D_stream_t *st, uint64_t payload_start, uint64_t payload_end)
{
    size_t i;
    for (i = 0; i < st->sample_count; ++i)
    {
        sample_desc_t *s = &st->samples[i];
        if (s->offset < payload_start || s->offset > payload_end)
            return -1;
        if ((uint64_t)s->size > payload_end - s->offset)
            return -1;
    }
    return 0;
}

static int mdat_complete(MP4D_stream_t *st)
{
    /* Zero-size samples that sit exactly at the end of the box. */
    while (st->sample_idx < st->sample_count)
    {
        sample_desc_t *s = &st->samples[st->sample_idx];
        if (s->size != 0 || s->offset != st->mdat_end)
            break;
        if (deliver_sample(st, s))
            return -1;
        ++st->sample_idx;
    }
    if (st->sample_idx != st->sample_count)
        return -1;
    free(st->samples);
    st->samples = NULL;
    st->sample_count = 0;
    st->sample_cap = 0;
    st->sample_idx = 0;
    free(st->moof_buf);
    st->moof_buf = NULL;
    st->moof_len = 0;
    st->moof_total = 0;
    st->have_moof = 0;
    st->mdat_finished = 1;
    st->state = ST_HEADER;
    return 0;
}

static int box_header_start(MP4D_stream_t *st)
{
    uint32_t size32 = rd32(st->hdr);
    uint64_t total, payload;
    uint32_t header, name;
    uint64_t start;

    if (size32 == 1)
    {
        total = rd64(st->hdr + 8);
        header = 16;
        if (total < 16)
            return st_fail(st);
    } else
    {
        if (size32 < 8)
            return st_fail(st); /* zero-length (to EOF) and short boxes are unsupported */
        total = size32;
        header = 8;
    }
    name = rd32(st->hdr + 4);
    payload = total - header;
    start = st->pos - header;

    if (!st->init_done)
    {
        if (name == BOX_MOOF || name == BOX_MDAT)
            return st_fail(st);
        if (total > st->limit || st->init_len > st->limit - (size_t)total)
            return st_fail(st);
        if (init_append(st, st->hdr, header))
            return st_fail(st);
        st->box_name = name;
        st->init_box_is_moov = (name == BOX_MOOV);
        st->remaining = payload;
        st->state = ST_INIT_BOX;
        return 0;
    }

    st->box_name = name;
    st->box_start = start;

    if (name == BOX_MOOF)
    {
        if (st->have_moof)
            return st_fail(st);
        if (total > st->limit || total > (uint64_t)(size_t)-1)
            return st_fail(st);
        st->moof_buf = (unsigned char *)malloc((size_t)total);
        if (!st->moof_buf)
            return st_fail(st);
        memcpy(st->moof_buf, st->hdr, header);
        st->moof_len = header;
        st->moof_total = total;
        st->remaining = payload;
        st->state = ST_MOOF;
        return 0;
    }
    if (name == BOX_MDAT)
    {
        uint64_t payload_start = st->pos;
        uint64_t payload_end;
        if (!st->have_moof)
            return st_fail(st);
        if (payload > UINT64_MAX - payload_start)
            return st_fail(st);
        payload_end = payload_start + payload;
        if (mdat_validate(st, payload_start, payload_end))
            return st_fail(st);
        st->mdat_end = payload_end;
        st->mdat_finished = 0;
        st->sample_idx = 0;
        st->sample_got = 0;
        st->state = ST_MDAT_INIT;
        if (payload == 0)
        {
            if (mdat_complete(st))
                return st_fail(st);
        }
        return 0;
    }
    if (name == BOX_MOOV || name == BOX_FTYP)
        return st_fail(st); /* the init segment must come first */

    /* Unknown top-level non-media boxes are skipped, even between a moof and
     * its mdat (the moof offsets are absolute, so this is safe). */
    st->remaining = payload;
    st->state = ST_SKIP;
    return 0;
}

static int feed_bytes(MP4D_stream_t *st, const unsigned char *data, size_t bytes)
{
    size_t used = 0;

    while (used < bytes && !st->failed)
    {
        if (st->state == ST_HEADER)
        {
            size_t need, want;
            if (st->hdr_len >= 4 && rd32(st->hdr) == 1)
                need = 16;
            else
                need = 8;
            want = need - st->hdr_len;
            if (want > bytes - used)
                want = bytes - used;
            if (want)
            {
                memcpy(st->hdr + st->hdr_len, data + used, want);
                st->hdr_len += want;
                st->pos += want;
                used += want;
            }
            /* An extended-size box needs 8 more header bytes. */
            if (st->hdr_len >= 4 && st->hdr_len < 16 && rd32(st->hdr) == 1)
                need = 16;
            if (st->hdr_len == need)
            {
                st->hdr_len = 0;
                if (box_header_start(st))
                    return -1;
            }
        } else if (st->state == ST_INIT_BOX)
        {
            size_t take = (uint64_t)(bytes - used) < st->remaining ?
                bytes - used : (size_t)st->remaining;
            if (init_append(st, data + used, take))
                return st_fail(st);
            used += take;
            st->pos += take;
            st->remaining -= take;
            if (!st->remaining)
            {
                if (st->init_box_is_moov && finalize_init(st))
                    return st_fail(st);
                st->init_box_is_moov = 0;
                st->state = ST_HEADER;
            }
        } else if (st->state == ST_MOOF)
        {
            size_t take = (uint64_t)(bytes - used) < st->remaining ?
                bytes - used : (size_t)st->remaining;
            if (st->moof_len + take > st->moof_total)
                return st_fail(st);
            memcpy(st->moof_buf + st->moof_len, data + used, take);
            st->moof_len += take;
            used += take;
            st->pos += take;
            st->remaining -= take;
            if (!st->remaining)
            {
                if (parse_moof(st))
                    return st_fail(st);
                st->state = ST_HEADER;
            }
        } else if (st->state == ST_SKIP)
        {
            size_t take = (uint64_t)(bytes - used) < st->remaining ?
                bytes - used : (size_t)st->remaining;
            used += take;
            st->pos += take;
            st->remaining -= take;
            if (!st->remaining)
                st->state = ST_HEADER;
        } else if (st->state == ST_MDAT_INIT)
        {
            size_t before = used;
            uint64_t progress_pos = st->pos;
            size_t progress_idx = st->sample_idx;
            uint64_t progress_got = st->sample_got;
            unsigned char *progress_buf = st->sample_buf;

            /* Zero-sized samples sitting at the current position. */
            while (st->sample_idx < st->sample_count)
            {
                sample_desc_t *s = &st->samples[st->sample_idx];
                if (s->size != 0 || s->offset != st->pos)
                    break;
                if (deliver_sample(st, s))
                    return -1;
                ++st->sample_idx;
            }
            if (st->pos == st->mdat_end)
            {
                if (mdat_complete(st))
                    return -1;
                continue;
            }
            if (st->sample_idx == st->sample_count)
            {
                uint64_t avail = (uint64_t)(bytes - used);
                uint64_t take = st->mdat_end - st->pos;
                if (take > avail)
                    take = avail;
                st->pos += take;
                used += (size_t)take;
            } else
            {
                sample_desc_t *s = &st->samples[st->sample_idx];
                if (st->sample_buf)
                {
                    uint64_t need = (uint64_t)s->size - st->sample_got;
                    uint64_t avail = (uint64_t)(bytes - used);
                    uint64_t take = need < avail ? need : avail;
                    uint64_t room = st->mdat_end - st->pos;
                    if (take > room)
                        take = room;
                    if (take)
                    {
                        memcpy(st->sample_buf + st->sample_got, data + used, (size_t)take);
                        st->sample_got += take;
                        st->pos += take;
                        used += (size_t)take;
                    }
                    if (st->sample_got == s->size)
                    {
                        if (deliver_sample(st, s))
                            return -1;
                        ++st->sample_idx;
                    }
                } else if (s->offset > st->pos)
                {
                    uint64_t avail = (uint64_t)(bytes - used);
                    uint64_t take = s->offset - st->pos;
                    if (take > st->mdat_end - st->pos)
                        take = st->mdat_end - st->pos;
                    if (take > avail)
                        take = avail;
                    st->pos += take;
                    used += (size_t)take;
                } else if (s->offset == st->pos)
                {
                    st->sample_buf = (unsigned char *)malloc(s->size ? s->size : 1);
                    if (!st->sample_buf)
                        return st_fail(st);
                    st->sample_got = 0;
                    continue; /* retry with the buffer in place */
                } else
                {
                    return st_fail(st);
                }
            }
            if (st->state == ST_MDAT_INIT && st->pos == st->mdat_end)
            {
                if (mdat_complete(st))
                    return -1;
            } else if (st->state == ST_MDAT_INIT && used == before &&
                st->pos == progress_pos && st->sample_idx == progress_idx &&
                st->sample_got == progress_got && st->sample_buf == progress_buf)
                return st_fail(st);
        } else
        {
            return st_fail(st);
        }
    }
    return st->failed ? -1 : 0;
}

MP4D_stream_t *MP4D_stream_open(size_t limit, MP4D_stream_cb cb, void *token)
{
    MP4D_stream_t *st;
    if (!limit || !cb)
        return NULL;
    st = (MP4D_stream_t *)calloc(1, sizeof(MP4D_stream_t));
    if (!st)
        return NULL;
    st->limit = limit;
    st->cb = cb;
    st->token = token;
    st->state = ST_HEADER;
    st->mdat_finished = 1; /* no fragment started yet */
    return st;
}

int MP4D_stream_feed(MP4D_stream_t *st, const void *data, size_t bytes)
{
    if (!st)
        return -1;
    if (st->failed)
        return -1;
    if (st->finished_ok)
    {
        if (!bytes)
            return 0;
        return st_fail(st);
    }
    if (!bytes)
        return 0;
    if (!data)
        return st_fail(st);
    if (feed_bytes(st, (const unsigned char *)data, bytes) < 0)
    {
        /* Any failure -- parse, callback or allocation -- is permanent. */
        st->failed = 1;
        return -1;
    }
    return 0;
}

int MP4D_stream_finish(MP4D_stream_t *st)
{
    if (!st)
        return -1;
    if (st->failed)
        return -1;
    if (st->finished_ok)
        return 0;
    if (!st->init_done || st->hdr_len != 0 || st->state != ST_HEADER ||
        st->have_moof || st->sample_count || st->sample_buf || !st->mdat_finished)
        return st_fail(st);
    st->finished_ok = 1;
    return 0;
}

void MP4D_stream_close(MP4D_stream_t *st)
{
    if (!st)
        return;
    if (st->demux_open)
        MP4D_close(&st->demux);
    free(st->init_buf);
    free(st->moof_buf);
    free(st->samples);
    free(st->sample_buf);
    free(st->tracks);
    free(st->trex);
    free(st);
}
