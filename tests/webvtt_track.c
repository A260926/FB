/*
    Tests for embedded WebVTT ('wvtt') subtitle tracks.

    Muxing is checked by walking the produced boxes independently (not through
    MP4D_*), and demuxing is checked against hand-built 'wvtt' files (not
    through MP4E_*), so the two sides cannot hide each other's bugs.
*/
#include "../task_support/check.h"
#include "../minimp4.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MC4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

static uint32_t rd32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/************************************************************************/
/*          In-memory file used by both the writer and the reader       */
/************************************************************************/
typedef struct
{
    unsigned char *data;
    size_t size;
    size_t cap;
} mbuf_t;

static void mbuf_reset(mbuf_t *m)
{
    free(m->data);
    m->data = NULL;
    m->size = m->cap = 0;
}

static int mbuf_write(int64_t offset, const void *buffer, size_t size, void *token)
{
    mbuf_t *m = (mbuf_t*)token;
    size_t off = (size_t)offset, need;
    if (offset < 0 || size > (size_t)-1 - off)
        return 1;
    need = off + size;
    if (need > m->cap)
    {
        size_t cap = m->cap ? m->cap : 256;
        unsigned char *p;
        while (cap < need)
        {
            if (cap > ((size_t)-1) / 2)
            {
                cap = need;
                break;
            }
            cap *= 2;
        }
        p = (unsigned char*)realloc(m->data, cap);
        if (!p)
            return 1;
        m->data = p;
        m->cap = cap;
    }
    if (size)
        memcpy(m->data + off, buffer, size);
    if (need > m->size)
        m->size = need;
    return 0;
}

static int mbuf_read(int64_t offset, void *buffer, size_t size, void *token)
{
    mbuf_t *m = (mbuf_t*)token;
    if (offset < 0 || (uint64_t)offset > m->size || size > m->size - (size_t)offset)
        return 1;
    memcpy(buffer, m->data + (size_t)offset, size);
    return 0;
}

/************************************************************************/
/*          Generic box iteration                                       */
/************************************************************************/
typedef struct
{
    const unsigned char *p;
    const unsigned char *end;
} box_iter_t;

static void iter_init(box_iter_t *it, const unsigned char *p, size_t n)
{
    it->p = p;
    it->end = p + n;
}

static int iter_next(box_iter_t *it, uint32_t *name, const unsigned char **payload, size_t *size)
{
    uint32_t s;
    if (it->p + 8 > it->end)
        return 0;
    s = rd32(it->p);
    if (s < 8 || (size_t)(it->end - it->p) < s)
        return 0;
    *name = rd32(it->p + 4);
    *payload = it->p + 8;
    *size = s - 8;
    it->p += s;
    return 1;
}

static const unsigned char *first_box(const unsigned char *p, size_t n, uint32_t name, size_t *size)
{
    box_iter_t it;
    uint32_t nm;
    const unsigned char *pl;
    size_t sz;
    iter_init(&it, p, n);
    while (iter_next(&it, &nm, &pl, &sz))
    {
        if (nm == name)
        {
            *size = sz;
            return pl;
        }
    }
    return NULL;
}

/************************************************************************/
/*          Muxed-file sample table reader (independent of MP4D_*)      */
/************************************************************************/
#define MAX_TEST_SAMPLES 16
#define MAX_STTS_ENTRIES  8

typedef struct
{
    int found;
    unsigned sample_count;
    uint32_t size[MAX_TEST_SAMPLES];
    uint32_t offset[MAX_TEST_SAMPLES];

    unsigned stts_count;
    uint32_t stts_n[MAX_STTS_ENTRIES];
    uint32_t stts_d[MAX_STTS_ENTRIES];

    uint32_t movie_timescale;
    uint32_t movie_duration;
    uint32_t track_duration;
    uint32_t media_timescale;
    uint32_t media_duration;
    uint32_t handler_type;
    uint16_t language_code;

    unsigned stsd_entries;
    unsigned has_wvtt;
    unsigned has_nmhd;
    unsigned has_vmhd;
    unsigned has_smhd;
    const unsigned char *wvtt_entry;        // wvtt box payload (after size/type)
    size_t wvtt_entry_size;
    const unsigned char *vttc_payload;
    size_t vttc_size;
} track_view_t;

static int load_track(const mbuf_t *m, unsigned index, track_view_t *v)
{
    size_t moov_size, trak_size = 0, mdia_size, minf_size, stbl_size, tmp;
    const unsigned char *moov, *trak = NULL, *mdia, *minf, *stbl;
    box_iter_t it;
    uint32_t name;
    const unsigned char *pl;
    size_t sz;
    unsigned ti = 0;
    const unsigned char *p;

    memset(v, 0, sizeof(*v));
    if (!m->data)
        return 0;
    moov = first_box(m->data, m->size, MC4('m','o','o','v'), &moov_size);
    if (!moov)
        return 0;
    p = first_box(moov, moov_size, MC4('m','v','h','d'), &tmp);
    if (p && tmp >= 20)
    {
        v->movie_timescale = rd32(p + 12);
        v->movie_duration = rd32(p + 16);
    }
    iter_init(&it, moov, moov_size);
    while (iter_next(&it, &name, &pl, &sz))
    {
        if (name == MC4('t','r','a','k'))
        {
            if (ti == index)
            {
                trak = pl;
                trak_size = sz;
                break;
            }
            ti++;
        }
    }
    if (!trak)
        return 0;

    p = first_box(trak, trak_size, MC4('t','k','h','d'), &tmp);
    if (p && tmp >= 24)
        v->track_duration = rd32(p + 20);

    mdia = first_box(trak, trak_size, MC4('m','d','i','a'), &mdia_size);
    if (!mdia)
        return 0;
    p = first_box(mdia, mdia_size, MC4('m','d','h','d'), &tmp);
    if (p && tmp >= 22)
    {
        v->media_timescale = rd32(p + 12);
        v->media_duration = rd32(p + 16);
        v->language_code = rd16(p + 20);
    }
    p = first_box(mdia, mdia_size, MC4('h','d','l','r'), &tmp);
    if (p && tmp >= 12)
        v->handler_type = rd32(p + 8);

    minf = first_box(mdia, mdia_size, MC4('m','i','n','f'), &minf_size);
    if (!minf)
        return 0;
    iter_init(&it, minf, minf_size);
    while (iter_next(&it, &name, &pl, &sz))
    {
        if (name == MC4('n','m','h','d'))
            v->has_nmhd = 1;
        if (name == MC4('v','m','h','d'))
            v->has_vmhd = 1;
        if (name == MC4('s','m','h','d'))
            v->has_smhd = 1;
    }

    stbl = first_box(minf, minf_size, MC4('s','t','b','l'), &stbl_size);
    if (!stbl)
        return 0;
    iter_init(&it, stbl, stbl_size);
    while (iter_next(&it, &name, &pl, &sz))
    {
        if (name == MC4('s','t','s','d'))
        {
            box_iter_t ei;
            uint32_t en;
            const unsigned char *ep;
            size_t es;
            v->stsd_entries = rd32(pl + 4);
            iter_init(&ei, pl + 8, sz - 8);
            while (iter_next(&ei, &en, &ep, &es))
            {
                if (en == MC4('w','v','t','t'))
                {
                    const unsigned char *cfg;
                    size_t cfg_size;
                    v->has_wvtt = 1;
                    v->wvtt_entry = ep;
                    v->wvtt_entry_size = es;
                    if (es >= 8)
                    {
                        cfg = first_box(ep + 8, es - 8, MC4('v','t','t','C'), &cfg_size);
                        if (cfg)
                        {
                            v->vttc_payload = cfg;
                            v->vttc_size = cfg_size;
                        }
                    }
                }
            }
        } else if (name == MC4('s','t','t','s'))
        {
            unsigned i, cnt = rd32(pl + 4);
            v->stts_count = cnt > MAX_STTS_ENTRIES ? MAX_STTS_ENTRIES : cnt;
            for (i = 0; i < v->stts_count; i++)
            {
                v->stts_n[i] = rd32(pl + 8 + i*8);
                v->stts_d[i] = rd32(pl + 12 + i*8);
            }
        } else if (name == MC4('s','t','s','z'))
        {
            unsigned i, cnt;
            if (rd32(pl + 4) != 0)
                return 0;   // tests only use variable-size tables
            cnt = rd32(pl + 8);
            if (cnt > MAX_TEST_SAMPLES)
                return 0;
            v->sample_count = cnt;
            for (i = 0; i < cnt; i++)
                v->size[i] = rd32(pl + 12 + i*4);
        } else if (name == MC4('s','t','c','o'))
        {
            unsigned i, cnt = rd32(pl + 4);
            if (cnt > MAX_TEST_SAMPLES)
                return 0;
            for (i = 0; i < cnt; i++)
                v->offset[i] = rd32(pl + 8 + i*4);
        }
    }
    v->found = 1;
    return 1;
}

static int view_sample_time(const track_view_t *v, unsigned i, uint32_t *duration, uint32_t *timestamp)
{
    unsigned e = 0;
    uint32_t ts = 0;
    if (i >= v->sample_count)
        return 0;
    while (e < v->stts_count)
    {
        uint32_t n = v->stts_n[e], d = v->stts_d[e];
        if (i < n)
        {
            *duration = d;
            *timestamp = ts + i*d;
            return 1;
        }
        ts += n*d;
        i -= n;
        e++;
    }
    return 0;
}

static const unsigned char *view_sample_data(const mbuf_t *m, const track_view_t *v, unsigned i, size_t *size)
{
    if (i >= v->sample_count)
        return NULL;
    if ((uint64_t)v->offset[i] + v->size[i] > m->size)
        return NULL;
    *size = v->size[i];
    return m->data + v->offset[i];
}

/************************************************************************/
/*          Expected cue content checker                                */
/************************************************************************/
typedef struct
{
    const char *id;         // NULL == iden box must be absent
    const char *settings;   // NULL == sttg box must be absent
    const char *text;
} cue_expect_t;

static int check_cue_text(const unsigned char *p, size_t n, const char *expected)
{
    return n == strlen(expected) && (!n || !memcmp(p, expected, n));
}

static int check_sample_content(const unsigned char *p, size_t n, const cue_expect_t *exp, int count)
{
    box_iter_t it;
    uint32_t name;
    const unsigned char *pl;
    size_t sz;
    int idx = 0;

    if (!p)
        return 0;
    if (count == 0)
    {
        iter_init(&it, p, n);
        if (!iter_next(&it, &name, &pl, &sz))
            return 0;
        if (name != MC4('v','t','t','e') || sz != 0)
            return 0;
        return !iter_next(&it, &name, &pl, &sz);
    }
    iter_init(&it, p, n);
    while (iter_next(&it, &name, &pl, &sz))
    {
        box_iter_t ci;
        uint32_t cn;
        const unsigned char *cp;
        size_t cs;
        if (name != MC4('v','t','t','c') || idx >= count)
            return 0;
        iter_init(&ci, pl, sz);
        if (exp[idx].id)
        {
            if (!iter_next(&ci, &cn, &cp, &cs) || cn != MC4('i','d','e','n'))
                return 0;
            if (!check_cue_text(cp, cs, exp[idx].id))
                return 0;
        }
        if (exp[idx].settings)
        {
            if (!iter_next(&ci, &cn, &cp, &cs) || cn != MC4('s','t','t','g'))
                return 0;
            if (!check_cue_text(cp, cs, exp[idx].settings))
                return 0;
        }
        if (!iter_next(&ci, &cn, &cp, &cs) || cn != MC4('p','a','y','l'))
            return 0;
        if (!check_cue_text(cp, cs, exp[idx].text))
            return 0;
        if (iter_next(&ci, &cn, &cp, &cs))
            return 0;   // no extra boxes inside vttc
        idx++;
    }
    return idx == count;
}

/************************************************************************/
/*          timeline group                                              */
/************************************************************************/
static int tc_timeline_default(void)
{
    mbuf_t out = {0};
    MP4E_mux_t *mux;
    int tr;
    track_view_t v;
    static const MP4E_webvtt_cue_t cues[2] = {
        {1000, 3000, "A", NULL, "A"},
        {2000, 4000, "B", NULL, "B"}
    };
    static const cue_expect_t e1[] = { {"A", NULL, "A"} };
    static const cue_expect_t e2[] = { {"A", NULL, "A"}, {"B", NULL, "B"} };
    static const cue_expect_t e3[] = { {"B", NULL, "B"} };
    unsigned i;

    mux = MP4E_open(0, 0, &out, mbuf_write);
    TASK_CHECK(mux != NULL);
    tr = MP4E_add_webvtt_track(mux, "eng", "WEBVTT\n", 7);
    TASK_CHECK(tr == 0);
    TASK_CHECK(MP4E_write_webvtt(mux, tr, cues, 2, 5000) == 0);
    TASK_CHECK(MP4E_close(mux) == 0);

    TASK_CHECK(load_track(&out, 0, &v));
    TASK_CHECK(v.sample_count == 5);
    for (i = 0; i < 5; i++)
    {
        uint32_t dur, ts;
        size_t n;
        const unsigned char *p = view_sample_data(&out, &v, i, &n);
        TASK_CHECK(view_sample_time(&v, i, &dur, &ts));
        TASK_CHECK(dur == 1000 && ts == i*1000);
        TASK_CHECK(check_sample_content(p, n, i == 0 || i == 4 ? NULL : (i == 2 ? e2 : (i == 1 ? e1 : e3)),
                                        i == 0 || i == 4 ? 0 : (i == 2 ? 2 : 1)));
    }
    TASK_CHECK(v.media_timescale == 1000);
    TASK_CHECK(v.media_duration == 5000);
    TASK_CHECK(v.stts_count == 1 && v.stts_n[0] == 5 && v.stts_d[0] == 1000);

    mbuf_reset(&out);
    return 0;
}

static int tc_timeline_sequential(void)
{
    mbuf_t out = {0};
    MP4E_mux_t *mux;
    int tr;
    track_view_t v;
    static const MP4E_webvtt_cue_t cues[2] = {
        {1000, 3000, "A", NULL, "A"},
        {2000, 4000, "B", NULL, "B"}
    };
    static const cue_expect_t e1[] = { {"A", NULL, "A"} };
    static const cue_expect_t e2[] = { {"A", NULL, "A"}, {"B", NULL, "B"} };
    static const cue_expect_t e3[] = { {"B", NULL, "B"} };
    unsigned i;

    mux = MP4E_open(1, 0, &out, mbuf_write);
    TASK_CHECK(mux != NULL);
    tr = MP4E_add_webvtt_track(mux, "eng", "WEBVTT\n", 7);
    TASK_CHECK(tr == 0);
    TASK_CHECK(MP4E_write_webvtt(mux, tr, cues, 2, 5000) == 0);
    TASK_CHECK(MP4E_close(mux) == 0);

    TASK_CHECK(load_track(&out, 0, &v));
    TASK_CHECK(v.sample_count == 5);
    for (i = 0; i < 5; i++)
    {
        uint32_t dur, ts;
        size_t n;
        const unsigned char *p = view_sample_data(&out, &v, i, &n);
        TASK_CHECK(view_sample_time(&v, i, &dur, &ts));
        TASK_CHECK(dur == 1000 && ts == i*1000);
        TASK_CHECK(check_sample_content(p, n, i == 0 || i == 4 ? NULL : (i == 2 ? e2 : (i == 1 ? e1 : e3)),
                                        i == 0 || i == 4 ? 0 : (i == 2 ? 2 : 1)));
    }
    mbuf_reset(&out);
    return 0;
}

static int tc_timeline_ordering(void)
{
    mbuf_t out = {0};
    MP4E_mux_t *mux;
    int tr;
    track_view_t v;
    // Same id/text/settings for all three cues: they are distinct objects.
    static const MP4E_webvtt_cue_t cues[3] = {
        {2000, 4000, "dup", "line:0", "same text"},
        {1000, 3000, "dup", "line:1", "same text"},
        {1500, 2500, "dup", NULL,     "same text"}
    };
    static const cue_expect_t s1[] = { {"dup", "line:1", "same text"} };
    static const cue_expect_t s2[] = { {"dup", "line:1", "same text"}, {"dup", NULL, "same text"} };
    static const cue_expect_t s3[] = { {"dup", "line:0", "same text"},
                                       {"dup", "line:1", "same text"},
                                       {"dup", NULL, "same text"} };
    static const cue_expect_t s4[] = { {"dup", "line:0", "same text"},
                                       {"dup", "line:1", "same text"} };
    static const cue_expect_t s5[] = { {"dup", "line:0", "same text"} };
    static const uint32_t expected_dur[7] = {1000, 500, 500, 500, 500, 1000, 1000};
    const cue_expect_t *exp[7];
    int counts[7];
    unsigned i;

    mux = MP4E_open(0, 0, &out, mbuf_write);
    TASK_CHECK(mux != NULL);
    tr = MP4E_add_webvtt_track(mux, "eng", "WEBVTT\n", 7);
    TASK_CHECK(tr == 0);
    TASK_CHECK(MP4E_write_webvtt(mux, tr, cues, 3, 5000) == 0);
    TASK_CHECK(MP4E_close(mux) == 0);

    TASK_CHECK(load_track(&out, 0, &v));
    TASK_CHECK(v.sample_count == 7);    // boundaries 0,1000,1500,2000,2500,3000,4000,5000
    exp[0] = NULL; counts[0] = 0;
    exp[1] = s1;   counts[1] = 1;
    exp[2] = s2;   counts[2] = 2;
    exp[3] = s3;   counts[3] = 3;
    exp[4] = s4;   counts[4] = 2;
    exp[5] = s5;   counts[5] = 1;
    exp[6] = NULL; counts[6] = 0;
    for (i = 0; i < 7; i++)
    {
        uint32_t dur, ts;
        size_t n;
        const unsigned char *p = view_sample_data(&out, &v, i, &n);
        TASK_CHECK(view_sample_time(&v, i, &dur, &ts));
        TASK_CHECK(dur == expected_dur[i]);
        TASK_CHECK(check_sample_content(p, n, exp[i], counts[i]));
    }
    mbuf_reset(&out);
    return 0;
}

static int tc_timeline_empty(void)
{
    mbuf_t out = {0};
    MP4E_mux_t *mux;
    int tr;
    track_view_t v;
    size_t n;
    const unsigned char *p;

    mux = MP4E_open(0, 0, &out, mbuf_write);
    TASK_CHECK(mux != NULL);
    tr = MP4E_add_webvtt_track(mux, "und", "WEBVTT\n", 7);
    TASK_CHECK(tr == 0);
    TASK_CHECK(MP4E_write_webvtt(mux, tr, NULL, 0, 700) == 0);
    TASK_CHECK(MP4E_close(mux) == 0);

    TASK_CHECK(load_track(&out, 0, &v));
    TASK_CHECK(v.sample_count == 1);
    TASK_CHECK(v.size[0] == 8);
    TASK_CHECK(v.media_duration == 700 && v.movie_duration == 700);
    {
        uint32_t dur, ts;
        TASK_CHECK(view_sample_time(&v, 0, &dur, &ts));
        TASK_CHECK(dur == 700 && ts == 0);
    }
    p = view_sample_data(&out, &v, 0, &n);
    TASK_CHECK(check_sample_content(p, n, NULL, 0));
    mbuf_reset(&out);
    return 0;
}

static int tc_timeline_reject(void)
{
    mbuf_t out = {0};
    MP4E_mux_t *mux, *mux_video, *mux_frag;
    MP4E_track_t vin;
    int tr, tr_v;
    static const MP4E_webvtt_cue_t ok[1] = { {1000, 2000, "x", NULL, "x"} };
    static const MP4E_webvtt_cue_t bad_interval[1] = { {2000, 2000, "x", NULL, "x"} };
    static const MP4E_webvtt_cue_t bad_end[1] = { {1000, 6000, "x", NULL, "x"} };
    static const MP4E_webvtt_cue_t bad_text[1] = { {1000, 2000, "x", NULL, NULL} };
    static const MP4E_webvtt_cue_t empty_text[1] = { {1000, 2000, "x", NULL, ""} };
    static const MP4E_webvtt_cue_t null_id[] = { {1000, 2000, NULL, NULL, "x"} };

    mux = MP4E_open(0, 0, &out, mbuf_write);
    TASK_CHECK(mux != NULL);
    tr = MP4E_add_webvtt_track(mux, "eng", "WEBVTT\n", 7);
    TASK_CHECK(tr == 0);

    // Nothing below may write sample bytes or change the track's index.
    TASK_CHECK(MP4E_put_sample(mux, tr, "x", 1, 10, MP4E_SAMPLE_DEFAULT) == MP4E_STATUS_BAD_ARGUMENTS);
    TASK_CHECK(MP4E_write_webvtt(mux, tr, NULL, 1, 5000) == MP4E_STATUS_BAD_ARGUMENTS);
    TASK_CHECK(MP4E_write_webvtt(mux, tr, ok, -1, 5000) == MP4E_STATUS_BAD_ARGUMENTS);
    TASK_CHECK(MP4E_write_webvtt(mux, tr, ok, 1, 0) == MP4E_STATUS_BAD_ARGUMENTS);
    TASK_CHECK(MP4E_write_webvtt(mux, tr, ok, 1, (uint32_t)INT32_MAX + 1) == MP4E_STATUS_BAD_ARGUMENTS);
    TASK_CHECK(MP4E_write_webvtt(mux, tr, bad_interval, 1, 5000) == MP4E_STATUS_BAD_ARGUMENTS);
    TASK_CHECK(MP4E_write_webvtt(mux, tr, bad_end, 1, 5000) == MP4E_STATUS_BAD_ARGUMENTS);
    TASK_CHECK(MP4E_write_webvtt(mux, tr, bad_text, 1, 5000) == MP4E_STATUS_BAD_ARGUMENTS);
    TASK_CHECK(MP4E_write_webvtt(mux, tr, empty_text, 1, 5000) == MP4E_STATUS_BAD_ARGUMENTS);
    TASK_CHECK(MP4E_write_webvtt(mux, -1, ok, 1, 5000) == MP4E_STATUS_BAD_ARGUMENTS);
    TASK_CHECK(MP4E_write_webvtt(mux, 5, ok, 1, 5000) == MP4E_STATUS_BAD_ARGUMENTS);

    // A rejected call must leave the track unsubmitted, so a valid one works.
    // (NULL id and settings are allowed; the text is not.)
    TASK_CHECK(MP4E_write_webvtt(mux, tr, bad_end, 1, 5000) == MP4E_STATUS_BAD_ARGUMENTS);
    TASK_CHECK(MP4E_write_webvtt(mux, tr, null_id, 1, 3000) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_write_webvtt(mux, tr, ok, 1, 5000) == MP4E_STATUS_BAD_ARGUMENTS); // already submitted
    TASK_CHECK(MP4E_close(mux) == 0);

    {
        track_view_t v;
        TASK_CHECK(load_track(&out, 0, &v));
        TASK_CHECK(v.sample_count == 3);   // 0,1000,2000,3000
        {
            uint32_t dur, ts;
            TASK_CHECK(view_sample_time(&v, 0, &dur, &ts) && dur == 1000);
            TASK_CHECK(view_sample_time(&v, 1, &dur, &ts) && dur == 1000);
            TASK_CHECK(view_sample_time(&v, 2, &dur, &ts) && dur == 1000);
        }
    }
    mbuf_reset(&out);

    // Wrong media type: an old-style non-WebVTT track cannot take a cue table.
    memset(&vin, 0, sizeof(vin));
    vin.track_media_kind = e_video;
    vin.object_type_indication = MP4_OBJECT_TYPE_AVC;
    vin.time_scale = 90000;
    vin.language[0] = 'u'; vin.language[1] = 'n'; vin.language[2] = 'd';
    {
        mbuf_t o2 = {0};
        mux_video = MP4E_open(0, 0, &o2, mbuf_write);
        TASK_CHECK(mux_video != NULL);
        tr_v = MP4E_add_track(mux_video, &vin);
        TASK_CHECK(tr_v == 0);
        TASK_CHECK(MP4E_write_webvtt(mux_video, tr_v, ok, 1, 5000) == MP4E_STATUS_BAD_ARGUMENTS);
        TASK_CHECK(MP4E_add_track(mux_video, NULL) == MP4E_STATUS_BAD_ARGUMENTS);
        TASK_CHECK(MP4E_close(mux_video) == 0);
        mbuf_reset(&o2);
    }

    // Fragmentation mode does not support embedded WebVTT.
    {
        mbuf_t o3 = {0};
        mux_frag = MP4E_open(0, 1, &o3, mbuf_write);
        TASK_CHECK(mux_frag != NULL);
        TASK_CHECK(MP4E_add_webvtt_track(mux_frag, "eng", "WEBVTT\n", 7) == MP4E_STATUS_BAD_ARGUMENTS);
        TASK_CHECK(MP4E_close(mux_frag) == 0);
        mbuf_reset(&o3);
    }
    return 0;
}

/************************************************************************/
/*          container group                                             */
/************************************************************************/
static int tc_container_boxes(void)
{
    mbuf_t out = {0};
    MP4E_mux_t *mux;
    int tr;
    track_view_t v;
    static const char config[] = "WEBVTT\nKind: captions\nLanguage: jpn\n";
    static const MP4E_webvtt_cue_t cue = {1000, 2000, "id", "line:0", "hello"};
    unsigned expected_lang = (((unsigned)('j' - 0x60)) << 10) | (((unsigned)('p' - 0x60)) << 5) | ((unsigned)('n' - 0x60));

    mux = MP4E_open(0, 0, &out, mbuf_write);
    TASK_CHECK(mux != NULL);
    tr = MP4E_add_webvtt_track(mux, "jpn", config, (int)sizeof(config) - 1);
    TASK_CHECK(tr == 0);
    TASK_CHECK(MP4E_write_webvtt(mux, tr, &cue, 1, 5000) == 0);
    TASK_CHECK(MP4E_close(mux) == 0);

    TASK_CHECK(load_track(&out, 0, &v));
    TASK_CHECK(v.stsd_entries == 1);
    TASK_CHECK(v.has_wvtt);
    TASK_CHECK(v.handler_type == MC4('t','e','x','t'));
    TASK_CHECK(v.has_nmhd && !v.has_vmhd && !v.has_smhd);
    TASK_CHECK(v.wvtt_entry_size >= 8 + 8 + sizeof(config) - 1);
    // SampleEntry common header: reserved[6] == 0, data_reference_index == 1.
    TASK_CHECK(v.wvtt_entry[0] == 0 && v.wvtt_entry[1] == 0 && v.wvtt_entry[2] == 0);
    TASK_CHECK(v.wvtt_entry[3] == 0 && v.wvtt_entry[4] == 0 && v.wvtt_entry[5] == 0);
    TASK_CHECK(rd16(v.wvtt_entry + 6) == 1);
    // vttC holds the configuration bytes verbatim (no terminator, no escaping).
    TASK_CHECK(v.vttc_payload != NULL);
    TASK_CHECK(v.vttc_size == sizeof(config) - 1);
    TASK_CHECK(memcmp(v.vttc_payload, config, v.vttc_size) == 0);

    TASK_CHECK(v.media_timescale == 1000);
    TASK_CHECK(v.media_duration == 5000);
    TASK_CHECK(v.track_duration == 5000);
    TASK_CHECK(v.movie_timescale == 1000);
    TASK_CHECK(v.movie_duration == 5000);
    TASK_CHECK(v.language_code == expected_lang);

    mbuf_reset(&out);
    return 0;
}

static int tc_container_duration_max(void)
{
    mbuf_t out = {0};
    MP4E_mux_t *mux;
    MP4E_track_t ain;
    int ta, tw;
    unsigned char dsi[2] = {0x12, 0x10};
    static const unsigned char a0[] = {1, 2, 3, 4, 5};
    static const unsigned char a1[] = {6, 7, 8, 9};
    static const MP4E_webvtt_cue_t cue = {1000, 2000, "s", NULL, "sub"};
    track_view_t v;
    size_t n;
    const unsigned char *p;

    memset(&ain, 0, sizeof(ain));
    ain.object_type_indication = MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3;
    ain.track_media_kind = e_audio;
    ain.time_scale = 48000;
    ain.default_duration = 1024;
    ain.u.a.channelcount = 2;
    ain.language[0] = 'u'; ain.language[1] = 'n'; ain.language[2] = 'd';

    mux = MP4E_open(0, 0, &out, mbuf_write);
    TASK_CHECK(mux != NULL);
    ta = MP4E_add_track(mux, &ain);
    TASK_CHECK(ta == 0);
    TASK_CHECK(MP4E_set_dsi(mux, ta, dsi, 2) == MP4E_STATUS_OK);

    // Audio frames written before the subtitle submission...
    TASK_CHECK(MP4E_put_sample(mux, ta, a0, sizeof(a0), 1024, MP4E_SAMPLE_DEFAULT) == MP4E_STATUS_OK);

    tw = MP4E_add_webvtt_track(mux, "eng", "WEBVTT\n", 7);
    TASK_CHECK(tw == 1);
    TASK_CHECK(MP4E_write_webvtt(mux, tw, &cue, 1, 5000) == MP4E_STATUS_OK);

    // ... and after it. Both must land correctly.
    TASK_CHECK(MP4E_put_sample(mux, ta, a1, sizeof(a1), 1024, MP4E_SAMPLE_DEFAULT) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_close(mux) == 0);

    TASK_CHECK(load_track(&out, 0, &v));
    TASK_CHECK(v.sample_count == 2);
    p = view_sample_data(&out, &v, 0, &n);
    TASK_CHECK(p != NULL && n == sizeof(a0) && !memcmp(p, a0, n));
    p = view_sample_data(&out, &v, 1, &n);
    TASK_CHECK(p != NULL && n == sizeof(a1) && !memcmp(p, a1, n));
    TASK_CHECK(v.media_duration == 2048);   // 42.7 ms at 48 kHz

    TASK_CHECK(load_track(&out, 1, &v));
    TASK_CHECK(v.sample_count == 3);        // 0,1000,2000,5000
    TASK_CHECK(v.media_duration == 5000);
    TASK_CHECK(v.movie_duration == 5000);   // subtitle is longer than the first track

    mbuf_reset(&out);
    return 0;
}

static int tc_container_config_copies(void)
{
    mbuf_t out = {0};
    MP4E_mux_t *mux;
    int tr;
    unsigned char config[12] = {0xEF, 0xBB, 0xBF, 'W', 'E', 'B', 'V', 'T', 'T', '\n', 0x00, 'x'};
    unsigned char copy[12];
    char lang[4] = "und";
    track_view_t v;
    static const MP4E_webvtt_cue_t cue = {0, 1000, NULL, NULL, "t"};

    memcpy(copy, config, sizeof(config));
    mux = MP4E_open(0, 0, &out, mbuf_write);
    TASK_CHECK(mux != NULL);
    tr = MP4E_add_webvtt_track(mux, lang, config, sizeof(config));
    TASK_CHECK(tr == 0);
    // Mutating the caller's buffers afterwards must not affect the output:
    // the library owns its copies.
    memset(config, 0xEE, sizeof(config));
    lang[0] = 'x'; lang[1] = 'y'; lang[2] = 'z';
    TASK_CHECK(MP4E_write_webvtt(mux, tr, &cue, 1, 1000) == 0);
    TASK_CHECK(MP4E_close(mux) == 0);

    TASK_CHECK(load_track(&out, 0, &v));
    TASK_CHECK(v.vttc_size == sizeof(copy));
    TASK_CHECK(v.vttc_payload && !memcmp(v.vttc_payload, copy, sizeof(copy)));
    TASK_CHECK(v.language_code == ((((unsigned)('u' - 0x60)) << 10) | (((unsigned)('n' - 0x60)) << 5) | ((unsigned)('d' - 0x60))));
    TASK_CHECK(v.sample_count == 1);   // cue starts at 0 and ends at total
    mbuf_reset(&out);
    return 0;
}

static int tc_container_sequential_pending(void)
{
    mbuf_t out = {0};
    MP4E_mux_t *mux;
    MP4E_track_t ain;
    int ta, tw;
    unsigned char dsi[2] = {0x12, 0x10};
    static const unsigned char a0[] = {9, 8, 7};
    static const unsigned char a1[] = {4, 5, 6};
    static const MP4E_webvtt_cue_t cue = {1500, 2500, "id", "line:0", "text"};
    track_view_t v;
    size_t n;
    const unsigned char *p;

    memset(&ain, 0, sizeof(ain));
    ain.object_type_indication = MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3;
    ain.track_media_kind = e_audio;
    ain.time_scale = 48000;
    ain.default_duration = 1024;
    ain.u.a.channelcount = 1;
    ain.language[0] = 'u'; ain.language[1] = 'n'; ain.language[2] = 'd';

    mux = MP4E_open(1, 0, &out, mbuf_write);    // sequential mode
    TASK_CHECK(mux != NULL);
    ta = MP4E_add_track(mux, &ain);
    TASK_CHECK(ta == 0);
    TASK_CHECK(MP4E_set_dsi(mux, ta, dsi, 2) == MP4E_STATUS_OK);

    // The first audio sample stays pending while the subtitle track is
    // submitted; it must still be written at the right offset afterwards.
    TASK_CHECK(MP4E_put_sample(mux, ta, a0, sizeof(a0), 1024, MP4E_SAMPLE_DEFAULT) == MP4E_STATUS_OK);
    tw = MP4E_add_webvtt_track(mux, "eng", "WEBVTT\n", 7);
    TASK_CHECK(tw == 1);
    TASK_CHECK(MP4E_write_webvtt(mux, tw, &cue, 1, 4000) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_put_sample(mux, ta, a1, sizeof(a1), 1024, MP4E_SAMPLE_DEFAULT) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_close(mux) == 0);

    TASK_CHECK(load_track(&out, 0, &v));
    TASK_CHECK(v.sample_count == 2);
    p = view_sample_data(&out, &v, 0, &n);
    TASK_CHECK(p != NULL && n == sizeof(a0) && !memcmp(p, a0, n));
    p = view_sample_data(&out, &v, 1, &n);
    TASK_CHECK(p != NULL && n == sizeof(a1) && !memcmp(p, a1, n));

    TASK_CHECK(load_track(&out, 1, &v));
    TASK_CHECK(v.sample_count == 3);    // 0,1500,2500,4000
    {
        uint32_t dur, ts;
        TASK_CHECK(view_sample_time(&v, 0, &dur, &ts));
        TASK_CHECK(dur == 1500);
        TASK_CHECK(view_sample_time(&v, 1, &dur, &ts));
        TASK_CHECK(dur == 1000);
        TASK_CHECK(view_sample_time(&v, 2, &dur, &ts));
        TASK_CHECK(dur == 1500);
    }
    mbuf_reset(&out);
    return 0;
}

static int tc_container_av_subtitles(void)
{
    mbuf_t out = {0};
    MP4E_mux_t *mux;
    MP4E_track_t vin, ain;
    int tv, ta, t1, t2;
    static const unsigned char sps[] = {0x67, 0x42, 0x00, 0x1e};
    static const unsigned char pps[] = {0x68, 0xce, 0x38, 0x80};
    static const unsigned char v0[] = {0,0,0,1, 9, 9, 9};
    static const unsigned char v1[] = {0,0,0,1, 8, 8, 8};
    unsigned char dsi[2] = {0x12, 0x10};
    static const unsigned char a0[] = {1, 2};
    static const unsigned char a1[] = {3, 4, 5};
    static const MP4E_webvtt_cue_t c1 = {0, 1000, "one", NULL, "first"};
    static const MP4E_webvtt_cue_t c2 = {500, 2000, "two", "line:1", "second"};
    track_view_t v;
    size_t n;
    const unsigned char *p;

    memset(&vin, 0, sizeof(vin));
    vin.object_type_indication = MP4_OBJECT_TYPE_AVC;
    vin.track_media_kind = e_video;
    vin.time_scale = 90000;
    vin.default_duration = 3000;
    vin.u.v.width = 64;
    vin.u.v.height = 48;
    vin.language[0] = 'u'; vin.language[1] = 'n'; vin.language[2] = 'd';

    memset(&ain, 0, sizeof(ain));
    ain.object_type_indication = MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3;
    ain.track_media_kind = e_audio;
    ain.time_scale = 48000;
    ain.default_duration = 1024;
    ain.u.a.channelcount = 2;
    ain.language[0] = 'u'; ain.language[1] = 'n'; ain.language[2] = 'd';

    mux = MP4E_open(0, 0, &out, mbuf_write);
    TASK_CHECK(mux != NULL);
    tv = MP4E_add_track(mux, &vin);
    TASK_CHECK(tv == 0);
    TASK_CHECK(MP4E_set_sps(mux, tv, sps, sizeof(sps)) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_set_pps(mux, tv, pps, sizeof(pps)) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_put_sample(mux, tv, v0, sizeof(v0), 3000, MP4E_SAMPLE_RANDOM_ACCESS) == MP4E_STATUS_OK);

    ta = MP4E_add_track(mux, &ain);
    TASK_CHECK(ta == 1);
    TASK_CHECK(MP4E_set_dsi(mux, ta, dsi, 2) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_put_sample(mux, ta, a0, sizeof(a0), 1024, MP4E_SAMPLE_DEFAULT) == MP4E_STATUS_OK);

    // Two subtitle tracks, submitted in the middle of the other tracks' data.
    t1 = MP4E_add_webvtt_track(mux, "eng", "WEBVTT\n", 7);
    TASK_CHECK(t1 == 2);
    TASK_CHECK(MP4E_write_webvtt(mux, t1, &c1, 1, 1500) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_put_sample(mux, tv, v1, sizeof(v1), 3000, MP4E_SAMPLE_DEFAULT) == MP4E_STATUS_OK);
    t2 = MP4E_add_webvtt_track(mux, "fra", "WEBVTT\n", 7);
    TASK_CHECK(t2 == 3);
    TASK_CHECK(MP4E_write_webvtt(mux, t2, &c2, 1, 3000) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_put_sample(mux, ta, a1, sizeof(a1), 1024, MP4E_SAMPLE_DEFAULT) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_close(mux) == 0);

    // Video payloads are intact despite the interleaved subtitle writes.
    TASK_CHECK(load_track(&out, 0, &v));
    TASK_CHECK(v.sample_count == 2);
    p = view_sample_data(&out, &v, 0, &n);
    TASK_CHECK(p != NULL && n == sizeof(v0) && !memcmp(p, v0, n));
    p = view_sample_data(&out, &v, 1, &n);
    TASK_CHECK(p != NULL && n == sizeof(v1) && !memcmp(p, v1, n));

    TASK_CHECK(load_track(&out, 1, &v));
    TASK_CHECK(v.sample_count == 2);
    p = view_sample_data(&out, &v, 0, &n);
    TASK_CHECK(p != NULL && n == sizeof(a0) && !memcmp(p, a0, n));
    p = view_sample_data(&out, &v, 1, &n);
    TASK_CHECK(p != NULL && n == sizeof(a1) && !memcmp(p, a1, n));

    TASK_CHECK(load_track(&out, 2, &v));
    TASK_CHECK(v.sample_count == 2);    // 0,1000,1500
    {
        static const cue_expect_t e1[] = { {"one", NULL, "first"} };
        uint32_t dur, ts;
        TASK_CHECK(view_sample_time(&v, 0, &dur, &ts) && dur == 1000);
        p = view_sample_data(&out, &v, 0, &n);
        TASK_CHECK(check_sample_content(p, n, e1, 1));
        TASK_CHECK(view_sample_time(&v, 1, &dur, &ts) && dur == 500);
        p = view_sample_data(&out, &v, 1, &n);
        TASK_CHECK(check_sample_content(p, n, NULL, 0));
    }

    TASK_CHECK(load_track(&out, 3, &v));
    TASK_CHECK(v.sample_count == 3);    // 0,500,2000,3000
    {
        static const cue_expect_t e0[] = { {"two", "line:1", "second"} };
        uint32_t dur, ts;
        TASK_CHECK(view_sample_time(&v, 0, &dur, &ts) && dur == 500);
        p = view_sample_data(&out, &v, 0, &n);
        TASK_CHECK(check_sample_content(p, n, NULL, 0));
        TASK_CHECK(view_sample_time(&v, 1, &dur, &ts) && dur == 1500);
        p = view_sample_data(&out, &v, 1, &n);
        TASK_CHECK(check_sample_content(p, n, e0, 1));
        TASK_CHECK(view_sample_time(&v, 2, &dur, &ts) && dur == 1000);
    }

    // Movie duration covers everything: video is 2x3000/90000 = 66.6 ms,
    // audio ~42 ms, second subtitle 3000 ms.
    TASK_CHECK(v.movie_duration == 3000);

    mbuf_reset(&out);
    return 0;
}

/************************************************************************/
/*          Hand-built 'wvtt' fixtures for the demuxer                  */
/************************************************************************/
typedef struct
{
    unsigned char *data;
    size_t size;
    size_t cap;
} abuf_t;

static void ab_reset(abuf_t *a)
{
    free(a->data);
    a->data = NULL;
    a->size = a->cap = 0;
}

static int ab_put(abuf_t *a, const void *p, size_t n)
{
    if (a->size + n > a->cap)
    {
        size_t cap = a->cap ? a->cap * 2 : 256;
        unsigned char *q;
        while (cap < a->size + n)
            cap *= 2;
        q = (unsigned char*)realloc(a->data, cap);
        if (!q)
            return 0;
        a->data = q;
        a->cap = cap;
    }
    if (n)
        memcpy(a->data + a->size, p, n);
    a->size += n;
    return 1;
}

static int ab_u8(abuf_t *a, unsigned v)
{
    unsigned char b = (unsigned char)v;
    return ab_put(a, &b, 1);
}

static int ab_u16(abuf_t *a, unsigned v)
{
    unsigned char b[2];
    b[0] = (unsigned char)(v >> 8);
    b[1] = (unsigned char)v;
    return ab_put(a, b, 2);
}

static int ab_u32(abuf_t *a, uint32_t v)
{
    unsigned char b[4];
    b[0] = (unsigned char)(v >> 24);
    b[1] = (unsigned char)(v >> 16);
    b[2] = (unsigned char)(v >> 8);
    b[3] = (unsigned char)v;
    return ab_put(a, b, 4);
}

static size_t ab_begin(abuf_t *a, uint32_t name)
{
    size_t at = a->size;
    ab_u32(a, 0);
    ab_u32(a, name);
    return at;
}

static void ab_end(abuf_t *a, size_t at)
{
    uint32_t sz = (uint32_t)(a->size - at);
    a->data[at + 0] = (unsigned char)(sz >> 24);
    a->data[at + 1] = (unsigned char)(sz >> 16);
    a->data[at + 2] = (unsigned char)(sz >> 8);
    a->data[at + 3] = (unsigned char)sz;
}

enum { FX_NORMAL = 0, FX_NO_VTTC, FX_DUP_VTTC, FX_TWO_ENTRIES };

typedef struct
{
    const unsigned char *payload;
    uint32_t size;
} fx_sample_t;

static void build_wvtt_file(abuf_t *a, int variant, const unsigned char *config, uint32_t config_bytes,
                            const fx_sample_t *samples, unsigned sample_count,
                            uint32_t *durations, uint32_t total_ms)
{
    uint32_t offsets[8];
    unsigned i;
    size_t mdat_at, moov_at, trak_at, mdia_at, minf_at, stbl_at;
    size_t stsd_at, entry_at;
    size_t at;

    ab_reset(a);

    // ftyp (same shape the muxer emits)
    at = ab_begin(a, MC4('f','t','y','p'));
    ab_u32(a, MC4('m','p','4','2'));
    ab_u32(a, 0);
    ab_u32(a, MC4('m','p','4','2'));
    ab_u32(a, MC4('i','s','o','m'));
    ab_end(a, at);

    // mdat with the sample payloads
    mdat_at = ab_begin(a, MC4('m','d','a','t'));
    for (i = 0; i < sample_count; i++)
    {
        offsets[i] = (uint32_t)(a->size + 0);
        ab_put(a, samples[i].payload, samples[i].size);
    }
    ab_end(a, mdat_at);

    moov_at = ab_begin(a, MC4('m','o','o','v'));

    at = ab_begin(a, MC4('m','v','h','d'));
    ab_u32(a, 0);           // version/flags
    ab_u32(a, 0); ab_u32(a, 0);
    ab_u32(a, 1000);        // timescale
    ab_u32(a, total_ms);    // duration
    ab_u32(a, 0x00010000);  // rate
    ab_u16(a, 0x0100); ab_u16(a, 0);
    ab_u32(a, 0); ab_u32(a, 0);
    for (i = 0; i < 9; i++)
        ab_u32(a, i % 4 == 0 ? 0x00010000 : (i == 8 ? 0x40000000 : 0));
    for (i = 0; i < 6; i++)
        ab_u32(a, 0);
    ab_u32(a, 2);           // next_track_ID
    ab_end(a, at);

    trak_at = ab_begin(a, MC4('t','r','a','k'));
    at = ab_begin(a, MC4('t','k','h','d'));
    ab_u32(a, 7);           // enabled, in movie, in preview
    ab_u32(a, 0); ab_u32(a, 0);
    ab_u32(a, 1);           // track_ID
    ab_u32(a, 0);
    ab_u32(a, total_ms);    // duration
    ab_u32(a, 0); ab_u32(a, 0);
    ab_u16(a, 0); ab_u16(a, 0);
    ab_u16(a, 0x0100); ab_u16(a, 0);
    for (i = 0; i < 9; i++)
        ab_u32(a, i % 4 == 0 ? 0x00010000 : (i == 8 ? 0x40000000 : 0));
    ab_u32(a, 0);           // width
    ab_u32(a, 0);           // height
    ab_end(a, at);

    mdia_at = ab_begin(a, MC4('m','d','i','a'));
    at = ab_begin(a, MC4('m','d','h','d'));
    ab_u32(a, 0);
    ab_u32(a, 0); ab_u32(a, 0);
    ab_u32(a, 1000);        // timescale
    ab_u32(a, total_ms);    // duration
    ab_u16(a, (('e' - 0x60) << 10) | (('n' - 0x60) << 5) | ('g' - 0x60)); // 'eng'
    ab_u16(a, 0);
    ab_end(a, at);

    at = ab_begin(a, MC4('h','d','l','r'));
    ab_u32(a, 0);
    ab_u32(a, 0);                       // pre_defined
    ab_u32(a, MC4('t','e','x','t'));     // handler
    ab_u32(a, 0); ab_u32(a, 0); ab_u32(a, 0);
    ab_u8(a, 'T'); ab_u8(a, 0);         // name
    ab_end(a, at);

    minf_at = ab_begin(a, MC4('m','i','n','f'));
    at = ab_begin(a, MC4('n','m','h','d'));
    ab_u32(a, 0);
    ab_end(a, at);

    stbl_at = ab_begin(a, MC4('s','t','b','l'));

    stsd_at = ab_begin(a, MC4('s','t','s','d'));
    ab_u32(a, 0);
    ab_u32(a, variant == FX_TWO_ENTRIES ? 2 : 1);
    for (i = 0; i < (variant == FX_TWO_ENTRIES ? 2u : 1u); i++)
    {
        entry_at = ab_begin(a, MC4('w','v','t','t'));
        ab_u32(a, 0); ab_u16(a, 0);     // reserved[6]
        ab_u16(a, 1);                   // data_reference_index
        if (i == 0 && variant != FX_NO_VTTC)
        {
            at = ab_begin(a, MC4('v','t','t','C'));
            ab_put(a, config, config_bytes);
            ab_end(a, at);
        }
        if (i == 0 && variant == FX_DUP_VTTC)
        {
            at = ab_begin(a, MC4('v','t','t','C'));
            ab_put(a, config, config_bytes);
            ab_end(a, at);
        }
        // Unknown child boxes must be skipped, not fatal.
        at = ab_begin(a, MC4('z','z','z','z'));
        ab_u32(a, 0x12345678);
        ab_end(a, at);
        ab_end(a, entry_at);
    }
    ab_end(a, stsd_at);

    at = ab_begin(a, MC4('s','t','t','s'));
    ab_u32(a, 0);
    {
        uint32_t runs = 0, j;
        // Emit one run per distinct duration.
        for (j = 0; j < sample_count; j++)
        {
            if (j == 0 || durations[j] != durations[j - 1])
                runs++;
        }
        ab_u32(a, runs);
        for (j = 0; j < sample_count; j++)
        {
            uint32_t n = 1, k;
            if (j > 0 && durations[j] == durations[j - 1])
                continue;
            for (k = j + 1; k < sample_count && durations[k] == durations[j]; k++)
                n++;
            ab_u32(a, n);
            ab_u32(a, durations[j]);
        }
    }
    ab_end(a, at);

    at = ab_begin(a, MC4('s','t','s','c'));
    ab_u32(a, 0);
    ab_u32(a, 1);
    ab_u32(a, 1);   // first_chunk
    ab_u32(a, 1);   // samples_per_chunk
    ab_u32(a, 1);   // sample_description_index
    ab_end(a, at);

    at = ab_begin(a, MC4('s','t','s','z'));
    ab_u32(a, 0);
    ab_u32(a, 0);   // sample_size == 0 -> per-sample sizes follow
    ab_u32(a, sample_count);
    for (i = 0; i < sample_count; i++)
        ab_u32(a, samples[i].size);
    ab_end(a, at);

    at = ab_begin(a, MC4('s','t','c','o'));
    ab_u32(a, 0);
    ab_u32(a, sample_count);
    for (i = 0; i < sample_count; i++)
        ab_u32(a, offsets[i]);
    ab_end(a, at);

    ab_end(a, stbl_at);
    ab_end(a, minf_at);
    ab_end(a, mdia_at);
    ab_end(a, trak_at);
    ab_end(a, moov_at);
}

// Locates the first 'vttC' inside the first 'wvtt' entry of the fixture.
static uint32_t fixture_vttc_type_offset(const abuf_t *a)
{
    size_t moov_size, trak_size, mdia_size, minf_size, stbl_size, stsd_size, entry_size, tmp;
    const unsigned char *moov = first_box(a->data, a->size, MC4('m','o','o','v'), &moov_size);
    const unsigned char *trak, *mdia, *minf, *stbl, *stsd, *entry, *cfg;
    if (!moov)
        return 0;
    trak = first_box(moov, moov_size, MC4('t','r','a','k'), &trak_size);
    if (!trak)
        return 0;
    mdia = first_box(trak, trak_size, MC4('m','d','i','a'), &mdia_size);
    if (!mdia)
        return 0;
    minf = first_box(mdia, mdia_size, MC4('m','i','n','f'), &minf_size);
    if (!minf)
        return 0;
    stbl = first_box(minf, minf_size, MC4('s','t','b','l'), &stbl_size);
    if (!stbl)
        return 0;
    stsd = first_box(stbl, stbl_size, MC4('s','t','s','d'), &stsd_size);
    if (!stsd)
        return 0;
    entry = first_box(stsd + 8, stsd_size - 8, MC4('w','v','t','t'), &entry_size);
    if (!entry)
        return 0;
    cfg = first_box(entry + 8, entry_size - 8, MC4('v','t','t','C'), &tmp);
    if (!cfg)
        return 0;
    return (uint32_t)((cfg - a->data) - 8);   // offset of the 'vttC' box header
}

/************************************************************************/
/*          demux group                                                 */
/************************************************************************/
static int demux_open_fails(abuf_t *a)
{
    mbuf_t in;
    MP4D_demux_t d;
    int ok;
    in.data = a->data;
    in.size = a->size;
    in.cap = a->cap;
    memset(&d, 0, sizeof(d));
    ok = MP4D_open(&d, mbuf_read, &in, (int64_t)in.size);
    if (ok)
        MP4D_close(&d);
    return !ok;
}

static int tc_demux_handcrafted(void)
{
    abuf_t a = {0};
    static const unsigned char config[] = "WEBVTT\nX-TIMESTAMP-MAP=LOCAL:00:00:00.000\n";
    unsigned char s0[8] = {0,0,0,8,'v','t','t','e'};
    // vttc { iden "id1", sttg "line:0 position:10%", payl "Hello" }
    static const unsigned char s1[] = {
        0,0,0,45,'v','t','t','c',
        0,0,0,11,'i','d','e','n','i','d','1',
        0,0,0,30,'s','t','t','g','l','i','n','e',':','0',' ','p','o','s','i','t','i','o','n',':','1','0','%',
        0,0,0,13,'p','a','y','l','H','e','l','l','o'
    };
    unsigned char s2[8] = {0,0,0,8,'v','t','t','e'};
    fx_sample_t samples[3];
    uint32_t durations[3] = {1000, 1500, 500};
    uint32_t total = 3000;
    mbuf_t in;
    MP4D_demux_t d;
    unsigned i;

    samples[0].payload = s0; samples[0].size = sizeof(s0);
    samples[1].payload = s1; samples[1].size = sizeof(s1);
    samples[2].payload = s2; samples[2].size = sizeof(s2);
    build_wvtt_file(&a, FX_NORMAL, config, sizeof(config) - 1, samples, 3, durations, total);

    in.data = a.data; in.size = a.size; in.cap = a.cap;
    memset(&d, 0, sizeof(d));
    TASK_CHECK(MP4D_open(&d, mbuf_read, &in, (int64_t)in.size) == 1);
    TASK_CHECK(d.track_count == 1);
    TASK_CHECK(d.track[0].sample_entry == MP4D_SAMPLE_ENTRY_WVTT);
    TASK_CHECK(d.track[0].dsi_bytes == sizeof(config) - 1);
    TASK_CHECK(d.track[0].dsi != NULL && !memcmp(d.track[0].dsi, config, sizeof(config) - 1));
    TASK_CHECK(d.track[0].sample_count == 3);
    TASK_CHECK(d.track[0].handler_type == MC4('t','e','x','t'));
    TASK_CHECK(d.track[0].timescale == 1000);
    TASK_CHECK(d.track[0].duration_lo == 3000);
    TASK_CHECK(d.duration_lo == 3000);

    for (i = 0; i < 3; i++)
    {
        unsigned bytes, ts, dur;
        MP4D_file_offset_t off = MP4D_frame_offset(&d, 0, i, &bytes, &ts, &dur);
        TASK_CHECK(off >= 0);
        TASK_CHECK(bytes == samples[i].size);
        TASK_CHECK(ts == (i == 0 ? 0 : (i == 1 ? 1000 : 2500)));
        TASK_CHECK(dur == durations[i]);
        TASK_CHECK((uint64_t)off + bytes <= in.size);
        TASK_CHECK(!memcmp(in.data + off, samples[i].payload, bytes));
    }
    MP4D_close(&d);
    ab_reset(&a);
    return 0;
}

static int tc_demux_errors(void)
{
    abuf_t a = {0};
    static const unsigned char config[] = "WEBVTT\n";
    unsigned char s0[8] = {0,0,0,8,'v','t','t','e'};
    fx_sample_t samples[1];
    uint32_t durations[1] = {1000};

    samples[0].payload = s0; samples[0].size = sizeof(s0);

    // Missing vttC.
    build_wvtt_file(&a, FX_NO_VTTC, config, sizeof(config) - 1, samples, 1, durations, 1000);
    TASK_CHECK(demux_open_fails(&a));

    // Duplicate vttC in one entry.
    build_wvtt_file(&a, FX_DUP_VTTC, config, sizeof(config) - 1, samples, 1, durations, 1000);
    TASK_CHECK(demux_open_fails(&a));

    // Two sample descriptions (the library only supports single-description).
    build_wvtt_file(&a, FX_TWO_ENTRIES, config, sizeof(config) - 1, samples, 1, durations, 1000);
    TASK_CHECK(demux_open_fails(&a));

    // Truncated configuration box payload: EOF in the middle of vttC.
    build_wvtt_file(&a, FX_NORMAL, config, sizeof(config) - 1, samples, 1, durations, 1000);
    {
        uint32_t type_off = fixture_vttc_type_offset(&a);
        TASK_CHECK(type_off != 0);
        if (type_off)
        {
            a.size = (size_t)type_off + 8 + 3;  // keep the vttC header, cut its payload
            TASK_CHECK(demux_open_fails(&a));
        }
    }
    ab_reset(&a);
    return 0;
}

static int tc_demux_legacy_dsi(void)
{
    // An audio track's dsi keeps meaning the same thing: 'sample_entry' is
    // informational, and non-'wvtt' tracks must not gain a vttC-derived dsi.
    mbuf_t out = {0};
    MP4E_mux_t *mux;
    MP4E_track_t ain;
    int ta;
    unsigned char dsi[2] = {0x12, 0x10};
    static const unsigned char a0[] = {1, 2, 3};
    MP4D_demux_t d;

    memset(&ain, 0, sizeof(ain));
    ain.object_type_indication = MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3;
    ain.track_media_kind = e_audio;
    ain.time_scale = 44100;
    ain.default_duration = 1024;
    ain.u.a.channelcount = 2;
    ain.language[0] = 'u'; ain.language[1] = 'n'; ain.language[2] = 'd';

    mux = MP4E_open(0, 0, &out, mbuf_write);
    TASK_CHECK(mux != NULL);
    ta = MP4E_add_track(mux, &ain);
    TASK_CHECK(ta == 0);
    TASK_CHECK(MP4E_set_dsi(mux, ta, dsi, 2) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_put_sample(mux, ta, a0, sizeof(a0), 1024, MP4E_SAMPLE_DEFAULT) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_close(mux) == 0);

    memset(&d, 0, sizeof(d));
    TASK_CHECK(MP4D_open(&d, mbuf_read, &out, (int64_t)out.size) == 1);
    TASK_CHECK(d.track_count == 1);
    TASK_CHECK(d.track[0].sample_entry != MP4D_SAMPLE_ENTRY_WVTT);
    TASK_CHECK(d.track[0].dsi_bytes == 2);
    TASK_CHECK(d.track[0].dsi && !memcmp(d.track[0].dsi, dsi, 2));
    TASK_CHECK(d.track[0].handler_type == MC4('s','o','u','n'));
    MP4D_close(&d);
    mbuf_reset(&out);
    return 0;
}

/************************************************************************/
/*          Registration                                                */
/************************************************************************/
const struct task_case task_cases[] = {
    { "timeline",  "five_samples_default",  tc_timeline_default },
    { "timeline",  "five_samples_sequential", tc_timeline_sequential },
    { "timeline",  "ordering_overlap_duplicates", tc_timeline_ordering },
    { "timeline",  "empty_track",           tc_timeline_empty },
    { "timeline",  "reject_then_succeed",   tc_timeline_reject },
    { "container", "sample_entry_and_cues", tc_container_boxes },
    { "container", "movie_duration_max",    tc_container_duration_max },
    { "container", "config_and_language_copied", tc_container_config_copies },
    { "container", "sequential_pending_interleave", tc_container_sequential_pending },
    { "container", "av_and_two_subtitle_tracks", tc_container_av_subtitles },
    { "demux",     "handcrafted_wvtt",      tc_demux_handcrafted },
    { "demux",     "malformed_wvtt",       tc_demux_errors },
    { "demux",     "legacy_audio_dsi",      tc_demux_legacy_dsi },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
