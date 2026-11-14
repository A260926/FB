/*
 * Streaming HEVC (H.265) Annex-B to MP4 muxer.
 *
 * Feeds arbitrary Annex-B chunks into one MP4 sample per coded picture:
 * all slices of a picture plus its suffix SEI are packed together with
 * 4-byte big-endian length prefixes, and the sample is only written once
 * the picture is known to be complete. See h265_stream.h for the contract.
 */

#include "h265_stream.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* HEVC NAL unit types (ITU-T H.265 Table 7-1). */
#define H265_NAL_BLA_W_LP     16
#define H265_NAL_RSV_IRAP_23  23
#define H265_NAL_VCL_LAST     31  /* types 0..31 are VCL NAL units */
#define H265_NAL_VPS          32
#define H265_NAL_SPS          33
#define H265_NAL_PPS          34
#define H265_NAL_AUD          35
#define H265_NAL_EOS          36
#define H265_NAL_EOB          37
#define H265_NAL_PREFIX_SEI   39
#define H265_NAL_SUFFIX_SEI   40

#define H265_PHASE_CONFIG     0   /* no VCL NAL seen yet */
#define H265_PHASE_DATA       1   /* at least one VCL NAL seen */

struct MP4H265_stream_tag
{
    MP4E_mux_t *mux;
    int track_id;
    int fps_num, fps_den;

    int err;            /* first sticky error, MP4E_STATUS_* */
    int finished;       /* finish() has run */
    int phase;

    int have_vps, have_sps, have_pps;

    /* Accumulated Annex-B bytes, always starting at (or before) a NAL. */
    unsigned char *raw;
    size_t raw_len, raw_cap;

    /* Current access unit: 4-byte BE length + NAL bytes, in arrival order. */
    unsigned char *au;
    size_t au_len, au_cap;
    int au_has_vcl;    /* the buffered access unit contains a slice */
    int pic_irap;      /* first VCL of the current picture is an IRAP */

    unsigned long frame_count;
};

static int buf_reserve(unsigned char **p, size_t *cap, size_t need)
{
    size_t c;
    void *q;
    if (need <= *cap)
        return MP4E_STATUS_OK;
    c = *cap ? *cap : 256;
    while (c < need)
    {
        if (c > SIZE_MAX / 2)
            return MP4E_STATUS_NO_MEMORY;
        c *= 2;
    }
    q = realloc(*p, c);
    if (!q)
        return MP4E_STATUS_NO_MEMORY;
    *p = (unsigned char *)q;
    *cap = c;
    return MP4E_STATUS_OK;
}

static int64_t h265_clock_start(unsigned long n, int fps_num, int fps_den)
{
    return (int64_t)n * 90000LL * (int64_t)fps_den / (int64_t)fps_num;
}

/* Find the next Annex-B start code (00 00 01, possibly preceded by another 00). */
static int h265_find_sc(const unsigned char *p, size_t n, size_t *sc_len)
{
    size_t i;
    for (i = 0; i + 3 <= n; i++)
    {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1)
        {
            if (i >= 1 && p[i - 1] == 0)
            {
                *sc_len = 4;
                return (int)(i - 1);
            }
            *sc_len = 3;
            return (int)i;
        }
    }
    return -1;
}

/*
 * first_slice_segment_in_pic_flag is the very first bit of the slice
 * segment header, i.e. the top bit of the first RBSP byte after the two-byte
 * NAL header. Returns 0/1, or -1 if the NAL carries no slice header byte.
 */
static int h265_first_slice_flag(const unsigned char *nal, size_t n)
{
    if (n < 3)
        return -1;
    return (nal[2] >> 7) & 1;
}

MP4H265_stream_t *MP4H265_stream_open(MP4E_mux_t *mux, int width, int height,
                                      int fps_num, int fps_den)
{
    MP4H265_stream_t *st;
    MP4E_track_t tr;
    int64_t step;

    if (!mux || width <= 0 || height <= 0 || fps_num <= 0 || fps_den <= 0)
        return NULL;
    if (MP4E_is_fragmented(mux))
        return NULL;  /* fragmented mode is not supported by this wrapper */

    /* Every frame duration must be a positive int in 90 kHz units. */
    step = 90000LL * (int64_t)fps_den;
    if ((int64_t)fps_num > step)                       /* less than one unit */
        return NULL;
    if (step > (int64_t)INT_MAX * (int64_t)fps_num)    /* nominal step too big */
        return NULL;

    st = (MP4H265_stream_t *)calloc(1, sizeof(*st));
    if (!st)
        return NULL;
    st->mux = mux;
    st->fps_num = fps_num;
    st->fps_den = fps_den;
    st->phase = H265_PHASE_CONFIG;

    memset(&tr, 0, sizeof(tr));
    tr.track_media_kind = e_video;
    tr.language[0] = 'u';
    tr.language[1] = 'n';
    tr.language[2] = 'd';
    tr.language[3] = 0;
    tr.object_type_indication = MP4_OBJECT_TYPE_HEVC;
    tr.time_scale = 90000;
    tr.default_duration = 0;
    tr.u.v.width = width;
    tr.u.v.height = height;
    st->track_id = MP4E_add_track(mux, &tr);
    if (st->track_id < 0)
    {
        free(st);
        return NULL;
    }
    return st;
}

/* Write the current access unit, if it actually contains a VCL NAL. */
static int h265_flush_picture(MP4H265_stream_t *st)
{
    int duration, kind, rc;

    if (!st->au_has_vcl)
    {   /* Prefix-only NALs (e.g. a prefix SEI) wait for the next picture:
           no VCL means nothing is emitted and nothing is discarded. */
        return MP4E_STATUS_OK;
    }
    if (st->au_len > (size_t)INT_MAX)
        return MP4E_STATUS_NO_MEMORY;

    duration = (int)(h265_clock_start(st->frame_count + 1, st->fps_num, st->fps_den)
                   - h265_clock_start(st->frame_count, st->fps_num, st->fps_den));
    kind = st->pic_irap ? MP4E_SAMPLE_RANDOM_ACCESS : MP4E_SAMPLE_DEFAULT;
    rc = MP4E_put_sample(st->mux, st->track_id, st->au, (int)st->au_len,
                         duration, kind);
    if (rc != MP4E_STATUS_OK)
        return rc;

    st->frame_count++;
    st->au_len = 0;
    st->au_has_vcl = 0;
    st->pic_irap = 0;
    return MP4E_STATUS_OK;
}

static int h265_append_nal(MP4H265_stream_t *st, const unsigned char *nal, size_t n)
{
    unsigned char len4[4];
    int rc;

    if (n > 0xfffffffeu || st->au_len + 4 + n < st->au_len)
        return MP4E_STATUS_NO_MEMORY;
    rc = buf_reserve(&st->au, &st->au_cap, st->au_len + 4 + n);
    if (rc != MP4E_STATUS_OK)
        return rc;
    len4[0] = (unsigned char)(n >> 24);
    len4[1] = (unsigned char)(n >> 16);
    len4[2] = (unsigned char)(n >> 8);
    len4[3] = (unsigned char)(n);
    memcpy(st->au + st->au_len, len4, 4);
    memcpy(st->au + st->au_len + 4, nal, n);
    st->au_len += 4 + n;
    return MP4E_STATUS_OK;
}

/* Classify and absorb one complete NAL unit (including its 2-byte header). */
static int h265_process_nal(MP4H265_stream_t *st, const unsigned char *nal, size_t n)
{
    unsigned char h0, h1;
    int type, layer_id, tid, flag, rc;

    if (n < 2)
        return MP4E_STATUS_BAD_ARGUMENTS;  /* truncated NAL header */
    if (n > (size_t)INT_MAX)
        return MP4E_STATUS_BAD_ARGUMENTS;  /* muxer APIs take int lengths */
    h0 = nal[0];
    h1 = nal[1];
    if (h0 & 0x80)
        return MP4E_STATUS_BAD_ARGUMENTS;  /* forbidden_zero_bit set */
    layer_id = ((h0 & 0x01) << 5) | (h1 >> 3);
    tid = h1 & 0x07;
    if (layer_id != 0 || tid != 1)
        return MP4E_STATUS_BAD_ARGUMENTS;  /* only single-layer, no reordering */
    type = (h0 >> 1) & 0x3f;

    if (type <= H265_NAL_VCL_LAST)
    {   /* VCL NAL unit */
        flag = h265_first_slice_flag(nal, n);
        if (flag < 0)
            return MP4E_STATUS_BAD_ARGUMENTS;

        if (flag)
        {   /* First slice of a new picture: close the previous one. */
            rc = h265_flush_picture(st);
            if (rc != MP4E_STATUS_OK)
                return rc;

            if (st->phase == H265_PHASE_CONFIG)
            {
                int irap = (type >= H265_NAL_BLA_W_LP && type <= H265_NAL_RSV_IRAP_23);
                if (!irap || !st->have_vps || !st->have_sps || !st->have_pps)
                    return MP4E_STATUS_BAD_ARGUMENTS;
                st->phase = H265_PHASE_DATA;
                st->pic_irap = 1;
            }
            else
            {
                st->pic_irap = (type >= H265_NAL_BLA_W_LP &&
                                type <= H265_NAL_RSV_IRAP_23);
            }
            st->au_has_vcl = 1;
        }
        else
        {   /* Continuation slice: a first slice must already be open. */
            if (!st->au_has_vcl)
                return MP4E_STATUS_BAD_ARGUMENTS;
        }
        return h265_append_nal(st, nal, n);
    }

    switch (type)
    {
    case H265_NAL_VPS:
    case H265_NAL_SPS:
    case H265_NAL_PPS:
        if (st->phase == H265_PHASE_DATA)
            return MP4E_STATUS_BAD_ARGUMENTS;  /* parameter sets only pre-VCL */
        if (type == H265_NAL_VPS)
        {
            rc = MP4E_set_vps(st->mux, st->track_id, nal, (int)n);
            st->have_vps = 1;
        }
        else if (type == H265_NAL_SPS)
        {
            rc = MP4E_set_sps(st->mux, st->track_id, nal, (int)n);
            st->have_sps = 1;
        }
        else
        {
            rc = MP4E_set_pps(st->mux, st->track_id, nal, (int)n);
            st->have_pps = 1;
        }
        return rc;

    case H265_NAL_AUD:          /* AUD ends a picture and is not stored. */
    case H265_NAL_EOS:
    case H265_NAL_EOB:
        return h265_flush_picture(st);

    case H265_NAL_PREFIX_SEI:   /* belongs to the next picture */
        rc = h265_flush_picture(st);
        if (rc != MP4E_STATUS_OK)
            return rc;
        return h265_append_nal(st, nal, n);

    case H265_NAL_SUFFIX_SEI:   /* belongs to the current picture */
        if (!st->au_has_vcl)
            return MP4E_STATUS_BAD_ARGUMENTS;
        return h265_append_nal(st, nal, n);

    default:
        /* Filler data, reserved and unspecified NALs carry no sample data. */
        return MP4E_STATUS_OK;
    }
}

/* Drain every complete NAL unit out of the raw accumulation buffer. */
static int h265_drain(MP4H265_stream_t *st)
{
    while (st->raw_len >= 3)
    {
        size_t sc1_len, sc2_len, nal_start, nal_end, next_rel;
        int pos, next;
        int rc;

        pos = h265_find_sc(st->raw, st->raw_len, &sc1_len);
        if (pos < 0)
            break;
        nal_start = (size_t)pos + sc1_len;

        next = h265_find_sc(st->raw + nal_start, st->raw_len - nal_start, &sc2_len);
        if (next < 0)
        {   /* Only a partial trailing NAL so far: drop leading junk, keep it. */
            if (pos > 0)
                memmove(st->raw, st->raw + pos, st->raw_len - (size_t)pos);
            st->raw_len -= (size_t)pos;
            break;
        }
        next_rel = (size_t)next;
        nal_end = nal_start + next_rel;
        /* The zeros of the following start code are not part of the NAL. */
        while (nal_end > nal_start && st->raw[nal_end - 1] == 0)
            nal_end--;

        rc = h265_process_nal(st, st->raw + nal_start, nal_end - nal_start);
        if (rc != MP4E_STATUS_OK)
            return rc;

        /* Consume up to the start of the next start code and keep going. */
        memmove(st->raw, st->raw + nal_start + next_rel,
                st->raw_len - nal_start - next_rel);
        st->raw_len -= nal_start + next_rel;
    }
    return MP4E_STATUS_OK;
}

int MP4H265_stream_feed(MP4H265_stream_t *st, const unsigned char *data, size_t len)
{
    int rc;

    if (!st)
        return MP4E_STATUS_BAD_ARGUMENTS;
    if (st->err)
        return st->err;
    if (st->finished)
        return MP4E_STATUS_BAD_ARGUMENTS;
    if (len == 0)
        return MP4E_STATUS_OK;   /* empty feeds are allowed */
    if (!data)
        return MP4E_STATUS_BAD_ARGUMENTS;

    rc = buf_reserve(&st->raw, &st->raw_cap, st->raw_len + len);
    if (rc != MP4E_STATUS_OK)
    {
        st->err = rc;
        return rc;
    }
    memcpy(st->raw + st->raw_len, data, len);
    st->raw_len += len;

    rc = h265_drain(st);
    if (rc != MP4E_STATUS_OK)
        st->err = rc;
    return rc;
}

int MP4H265_stream_finish(MP4H265_stream_t *st)
{
    size_t sc_len, nal_start;
    int pos, rc;

    if (!st)
        return MP4E_STATUS_BAD_ARGUMENTS;
    if (st->finished)
        return st->err;
    st->finished = 1;
    if (st->err)
        return st->err;

    /* A NAL still in the buffer is complete once the stream ends. */
    pos = h265_find_sc(st->raw, st->raw_len, &sc_len);
    if (pos >= 0)
    {
        nal_start = (size_t)pos + sc_len;
        if (st->raw_len - nal_start < 2)
        {
            st->err = MP4E_STATUS_BAD_ARGUMENTS;  /* truncated NAL header */
            return st->err;
        }
        rc = h265_process_nal(st, st->raw + nal_start, st->raw_len - nal_start);
        if (rc != MP4E_STATUS_OK)
        {
            st->err = rc;
            return rc;
        }
        st->raw_len = 0;
    }

    rc = h265_flush_picture(st);
    if (rc != MP4E_STATUS_OK)
        st->err = rc;
    return rc;
}

void MP4H265_stream_close(MP4H265_stream_t *st)
{
    if (!st)
        return;
    free(st->raw);
    free(st->au);
    free(st);
}
