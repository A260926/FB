#include "h265_stream.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* HEVC NAL unit types (ISO/IEC 23008-2, Table 7-1) */
#define H265_NAL_TRAIL_N        0
#define H265_NAL_TRAIL_R        1
#define H265_NAL_BLA_W_LP       16
#define H265_NAL_RSV_IRAP_VCL23 23
#define H265_NAL_VPS            32
#define H265_NAL_SPS            33
#define H265_NAL_PPS            34
#define H265_NAL_AUD            35
#define H265_NAL_PREFIX_SEI     39
#define H265_NAL_SUFFIX_SEI     40

#define H265_TIMESCALE          90000

typedef struct
{
    unsigned char *data;
    size_t len;
    size_t cap;
} h265_buf_t;

struct MP4H265_stream_tag
{
    MP4E_mux_t *mux;
    int track_id;
    unsigned fps_num;   /* reduced frame rate, frames per second ... */
    unsigned fps_den;   /* ... is fps_num/fps_den                    */

    /* Annex-B parser: rx holds payload bytes of the NAL unit currently
       being received (its start code is already consumed).  While
       rx_in_nal is clear, rx holds not-yet-consumed leading bytes. */
    h265_buf_t rx;
    size_t scan;        /* next rx position to test for a start code */
    int rx_in_nal;

    /* Picture assembly, MP4 sample layout: [BE32 length][NAL]... */
    h265_buf_t pic;     /* access unit being accumulated */
    h265_buf_t pending; /* prefix NALs waiting for the next picture */
    int pic_open;
    int pic_irap;

    int have_vps, have_sps, have_pps;
    int seen_vcl;

    unsigned long long frames;  /* pictures committed so far */

    int finished;
    int err;            /* sticky error code */
};

static int stream_fail(MP4H265_stream_t *s, int err)
{
    if (!s->err)
        s->err = err;
    return s->err;
}

static int buf_put(h265_buf_t *b, const void *mem, size_t n)
{
    if (n > (size_t)-1 - b->len)
        return 0;
    if (b->len + n > b->cap)
    {
        size_t cap = b->cap ? b->cap : 256;
        unsigned char *p;
        while (cap < b->len + n)
        {
            if (cap > (size_t)-1 / 2)
            {
                cap = b->len + n;
                break;
            }
            cap *= 2;
        }
        p = (unsigned char *)realloc(b->data, cap);
        if (!p)
            return 0;
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, mem, n);
    b->len += n;
    return 1;
}

static void buf_consume(h265_buf_t *b, size_t n)
{
    b->len -= n;
    memmove(b->data, b->data + n, b->len);
}

/* Append one NAL unit to an access-unit buffer in MP4 sample format */
static int au_put_nal(h265_buf_t *au, const unsigned char *nal, size_t nal_bytes)
{
    unsigned char hdr[4];
    hdr[0] = (unsigned char)(nal_bytes >> 24);
    hdr[1] = (unsigned char)(nal_bytes >> 16);
    hdr[2] = (unsigned char)(nal_bytes >>  8);
    hdr[3] = (unsigned char)(nal_bytes);
    return buf_put(au, hdr, 4) && buf_put(au, nal, nal_bytes);
}

/* Decode time of frame n on the 90 kHz clock: exact rational floor */
static unsigned long long frame_clock(const MP4H265_stream_t *s, unsigned long long n)
{
    return n * H265_TIMESCALE * (unsigned long long)s->fps_den / s->fps_num;
}

/* Commit the currently open picture as one MP4 sample, if any */
static int close_picture(MP4H265_stream_t *s)
{
    int err, kind, duration;
    if (!s->pic_open)
        return MP4E_STATUS_OK;
    {
        unsigned long long t0 = frame_clock(s, s->frames);
        unsigned long long t1 = frame_clock(s, s->frames + 1);
        duration = (int)(t1 - t0);  /* positive int, checked at open() */
    }
    kind = s->pic_irap ? MP4E_SAMPLE_RANDOM_ACCESS : MP4E_SAMPLE_DEFAULT;
    err = MP4E_put_sample(s->mux, s->track_id, s->pic.data, (int)s->pic.len, duration, kind);
    if (MP4E_STATUS_OK != err)
        return stream_fail(s, err);
    s->frames++;
    s->pic_open = 0;
    s->pic.len = 0;
    return MP4E_STATUS_OK;
}

static int handle_vcl(MP4H265_stream_t *s, const unsigned char *nal, size_t nal_bytes, unsigned nal_type)
{
    int first_slice, irap, err;
    if (nal_bytes < 3)
        return stream_fail(s, MP4E_STATUS_BAD_ARGUMENTS);   /* no slice header */
    /* first_slice_segment_in_pic_flag is the first RBSP bit; emulation
       prevention cannot insert a byte before it */
    first_slice = !!(nal[2] & 0x80);
    irap = (nal_type >= H265_NAL_BLA_W_LP && nal_type <= H265_NAL_RSV_IRAP_VCL23);
    if (first_slice)
    {
        if (!s->seen_vcl)
        {
            if (!irap)
                return stream_fail(s, MP4E_STATUS_BAD_ARGUMENTS);   /* first VCL must be IRAP */
            if (!s->have_vps || !s->have_sps || !s->have_pps)
                return stream_fail(s, MP4E_STATUS_BAD_ARGUMENTS);   /* incomplete configuration */
        }
        err = close_picture(s);     /* new picture ends the previous one */
        if (MP4E_STATUS_OK != err)
            return err;
        /* prefix NALs received since the last picture belong to this one
           (pic is empty here, so a plain swap moves them over) */
        {
            h265_buf_t tmp = s->pic;
            s->pic = s->pending;
            s->pending = tmp;
        }
        s->pic_open = 1;
        s->pic_irap = irap;         /* IRAP identity follows the first VCL */
        s->seen_vcl = 1;
    }
    else if (!s->pic_open)
        return stream_fail(s, MP4E_STATUS_BAD_ARGUMENTS);   /* continuation slice with no picture open */
    if (!au_put_nal(&s->pic, nal, nal_bytes))
        return stream_fail(s, MP4E_STATUS_NO_MEMORY);
    return MP4E_STATUS_OK;
}

static int handle_nal(MP4H265_stream_t *s, const unsigned char *nal, size_t nal_bytes)
{
    unsigned nal_type, layer_id, temporal_id_plus1;
    int err;
    if (nal_bytes < 2)
        return stream_fail(s, MP4E_STATUS_BAD_ARGUMENTS);   /* truncated NAL header */
    nal_type           = (nal[0] >> 1) & 0x3f;
    layer_id           = ((unsigned)(nal[0] & 1) << 5) | (nal[1] >> 3);
    temporal_id_plus1  = nal[1] & 7;
    if (layer_id != 0 || temporal_id_plus1 != 1)
        return stream_fail(s, MP4E_STATUS_BAD_ARGUMENTS);   /* single layer, single temporal layer only */

    if (nal_type <= 31)
        return handle_vcl(s, nal, nal_bytes, nal_type);

    switch (nal_type)
    {
    case H265_NAL_VPS:
    case H265_NAL_SPS:
    case H265_NAL_PPS:
        if (s->seen_vcl)
            return stream_fail(s, MP4E_STATUS_BAD_ARGUMENTS);   /* parameter sets only before the first VCL */
        if (nal_type == H265_NAL_VPS)
        {
            err = MP4E_set_vps(s->mux, s->track_id, nal, (int)nal_bytes);
            s->have_vps = 1;
        } else if (nal_type == H265_NAL_SPS)
        {
            err = MP4E_set_sps(s->mux, s->track_id, nal, (int)nal_bytes);
            s->have_sps = 1;
        } else
        {
            err = MP4E_set_pps(s->mux, s->track_id, nal, (int)nal_bytes);
            s->have_pps = 1;
        }
        if (MP4E_STATUS_OK != err)
            return stream_fail(s, err);
        return MP4E_STATUS_OK;

    case H265_NAL_AUD:
        return close_picture(s);    /* AUD is a boundary marker, never stored */

    case H265_NAL_PREFIX_SEI:
        err = close_picture(s);     /* prefix SEI belongs to the next picture */
        if (MP4E_STATUS_OK != err)
            return err;
        if (!au_put_nal(&s->pending, nal, nal_bytes))
            return stream_fail(s, MP4E_STATUS_NO_MEMORY);
        return MP4E_STATUS_OK;

    case H265_NAL_SUFFIX_SEI:
    default:                        /* suffix SEI and any other trailing NAL */
        if (!au_put_nal(s->pic_open ? &s->pic : &s->pending, nal, nal_bytes))
            return stream_fail(s, MP4E_STATUS_NO_MEMORY);
        return MP4E_STATUS_OK;
    }
}

/* Scan the receive buffer for start codes and dispatch complete NAL units */
static int process_rx(MP4H265_stream_t *s)
{
    size_t i = s->scan;
    while (i + 2 < s->rx.len)
    {
        if (!s->rx.data[i] && !s->rx.data[i + 1] && s->rx.data[i + 2] == 1)
        {
            if (s->rx_in_nal)
            {
                /* trailing zero bytes belong to the start code prefix */
                size_t end = i;
                int err;
                while (end > 0 && !s->rx.data[end - 1])
                    end--;
                if (end)
                {
                    err = handle_nal(s, s->rx.data, end);
                    if (MP4E_STATUS_OK != err)
                        return err;
                }
            }
            buf_consume(&s->rx, i + 3);
            s->rx_in_nal = 1;
            i = 0;
        } else
            i++;
    }
    s->scan = i;
    if (!s->rx_in_nal && s->rx.len > 2)
    {
        /* no NAL in progress: only the last two bytes can still become
           part of a start code, the rest is skippable leading garbage */
        buf_consume(&s->rx, s->rx.len - 2);
        s->scan = 0;
    }
    return MP4E_STATUS_OK;
}

static unsigned gcd_u(unsigned a, unsigned b)
{
    while (b)
    {
        unsigned t = a % b;
        a = b;
        b = t;
    }
    return a;
}

MP4H265_stream_t *MP4H265_stream_open(MP4E_mux_t *mux, int width, int height, int fps_num, int fps_den)
{
    MP4H265_stream_t *s;
    MP4E_track_t tr;
    unsigned g;
    unsigned long long frame_duration;

    if (!mux || width <= 0 || height <= 0 || fps_num <= 0 || fps_den <= 0)
        return NULL;
    /* the per-frame duration must be a positive int */
    frame_duration = (unsigned long long)H265_TIMESCALE * (unsigned)fps_den / (unsigned)fps_num;
    if (frame_duration < 1 || frame_duration >= (unsigned long long)INT_MAX)
        return NULL;

    s = (MP4H265_stream_t *)malloc(sizeof(*s));
    if (!s)
        return NULL;
    memset(s, 0, sizeof(*s));
    s->mux = mux;
    g = gcd_u((unsigned)fps_num, (unsigned)fps_den);
    s->fps_num = (unsigned)fps_num / g;
    s->fps_den = (unsigned)fps_den / g;

    memset(&tr, 0, sizeof(tr));
    tr.track_media_kind = e_video;
    tr.language[0] = 'u';
    tr.language[1] = 'n';
    tr.language[2] = 'd';
    tr.object_type_indication = MP4_OBJECT_TYPE_HEVC;
    tr.time_scale = H265_TIMESCALE;
    tr.u.v.width = width;
    tr.u.v.height = height;
    s->track_id = MP4E_add_track(mux, &tr);
    if (s->track_id < 0)
    {
        free(s);
        return NULL;
    }
    return s;
}

int MP4H265_stream_feed(MP4H265_stream_t *s, const unsigned char *data, size_t bytes)
{
    if (!s)
        return MP4E_STATUS_BAD_ARGUMENTS;
    if (s->err)
        return s->err;
    if (s->finished)
        return stream_fail(s, MP4E_STATUS_BAD_ARGUMENTS);
    if (!bytes)
        return MP4E_STATUS_OK;      /* empty feed is allowed */
    if (!data)
        return stream_fail(s, MP4E_STATUS_BAD_ARGUMENTS);
    if (!buf_put(&s->rx, data, bytes))
        return stream_fail(s, MP4E_STATUS_NO_MEMORY);
    return process_rx(s);
}

int MP4H265_stream_finish(MP4H265_stream_t *s)
{
    int err;
    if (!s)
        return MP4E_STATUS_BAD_ARGUMENTS;
    if (s->err)
        return s->err;
    if (s->finished)
        return MP4E_STATUS_OK;      /* no frame is committed twice */
    if (s->rx_in_nal)
    {
        /* the last NAL unit is terminated by end of stream */
        size_t end = s->rx.len;
        while (end > 0 && !s->rx.data[end - 1])
            end--;
        if (end)
        {
            err = handle_nal(s, s->rx.data, end);
            if (MP4E_STATUS_OK != err)
                return err;
        }
        s->rx.len = 0;
        s->rx_in_nal = 0;
        s->scan = 0;
    }
    err = close_picture(s);         /* end of stream commits the last frame */
    if (MP4E_STATUS_OK != err)
        return err;
    s->finished = 1;
    return MP4E_STATUS_OK;
}

void MP4H265_stream_close(MP4H265_stream_t *s)
{
    if (!s)
        return;
    free(s->rx.data);
    free(s->pic.data);
    free(s->pending.data);
    free(s);
}
