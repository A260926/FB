#ifdef __APPLE__
#include <sys/param.h>
#endif
#include "minih264e.h"
#include "task_support/case_harness.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/*
*   Regression tests for the optional NAL HRD signaling:
*     - VUI timing info / NAL HRD parameters in the SPS
*     - buffering_period and pic_timing SEI NAL units
*   Every parse below de-escapes the NAL unit on its own and reads the
*   RBSP with a small bit reader; no external parser is used.
*/

#define TEST_W 64
#define TEST_H 64
#define TEST_FRAME_SIZE (TEST_W * TEST_H * 3 / 2)
#define TEST_BITRATE 64000u
#define TEST_TICK 1001u
#define TEST_TIMESCALE 60000u
#define TEST_VBV 8192

/**************************** bit reader ****************************/

typedef struct
{
    const unsigned char *data;
    int nbits;
    int pos;
    int overread;
} bitreader_t;

static void br_init(bitreader_t *br, const unsigned char *data, int nbytes)
{
    br->data = data;
    br->nbits = nbytes * 8;
    br->pos = 0;
    br->overread = 0;
}

static unsigned br_bit(bitreader_t *br)
{
    unsigned v;
    if (br->pos >= br->nbits)
    {
        br->overread = 1;
        return 0;
    }
    v = (br->data[br->pos >> 3] >> (7 - (br->pos & 7))) & 1u;
    br->pos++;
    return v;
}

static unsigned br_bits(bitreader_t *br, int n)
{
    unsigned v = 0;
    while (n--)
    {
        v = (v << 1) | br_bit(br);
    }
    return v;
}

static unsigned br_ue(bitreader_t *br)
{
    int zeros = 0;
    while (!br_bit(br))
    {
        if (++zeros > 30)
        {
            br->overread = 1;
            return 0;
        }
    }
    return ((1u << zeros) - 1u) + br_bits(br, zeros);
}

/* Remove NAL emulation prevention bytes (00 00 03), NAL header skipped. */
static int deescape(const unsigned char *nal, int n, unsigned char *rbsp)
{
    int i, k = 0, zeros = 0;
    for (i = 1; i < n; i++)
    {
        if (zeros == 2 && nal[i] == 3)
        {
            zeros = 0;
            continue;
        }
        rbsp[k++] = nal[i];
        zeros = nal[i] ? 0 : zeros + 1;
    }
    return k;
}

/**************************** NAL handling ****************************/

typedef struct
{
    const unsigned char *p;
    int len;
    int type;
} nal_view_t;

/* Split an Annex-B buffer into NAL units with start codes removed. */
static int split_annexb(const unsigned char *d, int n, nal_view_t *out, int maxv)
{
    int i, cnt = 0, start = -1;
    for (i = 0; i + 2 < n; i++)
    {
        if (d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 ||
            (d[i + 2] == 0 && i + 3 < n && d[i + 3] == 1)))
        {
            int sc = d[i + 2] == 1 ? 3 : 4;
            if (start >= 0 && cnt < maxv)
            {
                out[cnt].p = d + start;
                out[cnt].len = i - start;
                out[cnt].type = out[cnt].p[0] & 31;
                cnt++;
            }
            start = i + sc;
            i += sc - 1;
        }
    }
    if (start >= 0 && start < n && cnt < maxv)
    {
        out[cnt].p = d + start;
        out[cnt].len = n - start;
        out[cnt].type = out[cnt].p[0] & 31;
        cnt++;
    }
    return cnt;
}

typedef struct
{
    unsigned char storage[1 << 18];
    int used;
    nal_view_t nals[64];
    int nnal;
} recorder_t;

static void record_callback(const unsigned char *nal, int n, void *token)
{
    recorder_t *rec = (recorder_t *)token;
    memcpy(rec->storage + rec->used, nal, n);
    if (rec->nnal < 64)
    {
        rec->nals[rec->nnal].p = rec->storage + rec->used;
        rec->nals[rec->nnal].len = n;
        rec->nals[rec->nnal].type = nal[0] & 31;
        rec->nnal++;
    }
    rec->used += n;
}

static void recorder_reset(recorder_t *rec)
{
    rec->used = 0;
    rec->nnal = 0;
}

/**************************** parsed syntax ****************************/

typedef struct
{
    int vui_present;
    unsigned num_units_in_tick;
    unsigned time_scale;
    int fixed_frame_rate;
    int nal_hrd_present;
    int bit_rate_scale;
    int cpb_size_scale;
    unsigned cpb_cnt_minus1;
    unsigned bit_rate_value_minus1;
    unsigned cpb_size_value_minus1;
    int cbr_flag;
    int init_cpb_len_m1;
    int cpb_len_m1;
    int dpb_len_m1;
    int time_offset_length;
    int vcl_hrd_present;
    int low_delay_hrd;
    int pic_struct_present;
    int bitstream_restriction;
    int overread;
} sps_info_t;

static void parse_sps(const nal_view_t *nal, sps_info_t *s)
{
    unsigned char rbsp[4096];
    bitreader_t br[1];
    int k = deescape(nal->p, nal->len, rbsp);
    unsigned poc_type, frame_flags;
    memset(s, 0, sizeof(*s));
    br_init(br, rbsp, k);    br_bits(br, 24);             // profile_idc / constraints / level_idc
    br_ue(br);                   // sps_id
    br_ue(br);                   // log2_max_frame_num_minus4
    poc_type = br_ue(br);        // pic_order_cnt_type
    if (poc_type == 0)
    {
        br_ue(br);               // log2_max_pic_order_cnt_lsb_minus4
    }
    br_ue(br);                   // num_ref_frames
    br_bit(br);                  // gaps_in_frame_num_value_allowed_flag
    br_ue(br);                   // pic_width_in_mbs_minus1
    br_ue(br);                   // pic_height_in_map_units_minus1
    frame_flags = br_bits(br, 3);
    if (frame_flags & 1u)        // frame_cropping_flag
    {
        br_ue(br); br_ue(br); br_ue(br); br_ue(br);
    }
    s->vui_present = (int)br_bit(br);
    if (!s->vui_present)
    {
        s->overread = br->overread;
        return;
    }
    br_bit(br);                  // aspect_ratio_info_present_flag
    br_bit(br);                  // overscan_info_present_flag
    br_bit(br);                  // video_signal_type_present_flag
    br_bit(br);                  // chroma_loc_info_present_flag
    s->num_units_in_tick = (br_bits(br, 16) << 16) | br_bits(br, 16);
    s->time_scale = (br_bits(br, 16) << 16) | br_bits(br, 16);
    s->fixed_frame_rate = (int)br_bit(br);
    s->nal_hrd_present = (int)br_bit(br);
    if (s->nal_hrd_present)
    {
        s->cpb_cnt_minus1 = br_ue(br);
        s->bit_rate_scale = (int)br_bits(br, 4);
        s->cpb_size_scale = (int)br_bits(br, 4);
        s->bit_rate_value_minus1 = br_ue(br);
        s->cpb_size_value_minus1 = br_ue(br);
        s->cbr_flag = (int)br_bit(br);
        s->init_cpb_len_m1 = (int)br_bits(br, 5);
        s->cpb_len_m1 = (int)br_bits(br, 5);
        s->dpb_len_m1 = (int)br_bits(br, 5);
        s->time_offset_length = (int)br_bits(br, 5);
    }
    s->vcl_hrd_present = (int)br_bit(br);
    s->low_delay_hrd = (int)br_bit(br);
    s->pic_struct_present = (int)br_bit(br);
    s->bitstream_restriction = (int)br_bit(br);
    s->overread = br->overread;
}

typedef struct
{
    int payload_type;
    int payload_size;
    unsigned sps_id;
    unsigned init_cpb_removal_delay;
    unsigned init_cpb_removal_delay_offset;
    unsigned cpb_removal_delay;
    unsigned dpb_output_delay;
    int overread;
} sei_info_t;

static void parse_sei(const nal_view_t *nal, sei_info_t *s)
{
    unsigned char rbsp[4096];
    bitreader_t br[1];
    int k = deescape(nal->p, nal->len, rbsp);
    unsigned b;
    memset(s, 0, sizeof(*s));
    br_init(br, rbsp, k);
    do
    {
        b = br_bits(br, 8);
        s->payload_type += b;
    } while (b == 255);
    do
    {
        b = br_bits(br, 8);
        s->payload_size += b;
    } while (b == 255);
    if (s->payload_type == 0)       // buffering_period
    {
        s->sps_id = br_ue(br);
        s->init_cpb_removal_delay = br_bits(br, 24);
        s->init_cpb_removal_delay_offset = br_bits(br, 24);
    } else if (s->payload_type == 1) // pic_timing
    {
        s->cpb_removal_delay = br_bits(br, 16);
        s->dpb_output_delay = br_bits(br, 16);
    }
    s->overread = br->overread;
}

/**************************** encoder helpers ****************************/

typedef struct
{
    H264E_persist_t *enc;
    H264E_scratch_t *scratch;
    int persist_size;
    int scratch_size;
    unsigned char *yuv;
    H264E_io_yuv_t in;
    H264E_run_param_t run;
    recorder_t rec;
    int width;
    int height;
} test_enc_t;

static void fill_i420(unsigned char *yuv, int w, int h, int frame)
{
    int i;
    for (i = 0; i < w * h; i++)
    {
        yuv[i] = (unsigned char)((i + i / w + frame * 7) & 255);
    }
    memset(yuv + w * h, 128, (size_t)w * h / 4);
    memset(yuv + w * h * 5 / 4, 128, (size_t)w * h / 4);
}

static void base_params(H264E_create_param_t *p, int w, int h, int gop, int hrd)
{
    memset(p, 0, sizeof(*p));
    p->width = w;
    p->height = h;
    p->gop = gop;
    p->const_input_flag = 1;
    p->vbv_size_bytes = TEST_VBV;
    p->num_layers = 1;
    p->max_threads = 1;
    if (hrd)
    {
        p->hrd_bitrate_bps = TEST_BITRATE;
        p->hrd_num_units_in_tick = TEST_TICK;
        p->hrd_time_scale = TEST_TIMESCALE;
    }
}

static int open_enc(test_enc_t *t, int hrd, int w, int h, int gop)
{
    H264E_create_param_t p;
    int err;
    base_params(&p, w, h, gop, hrd);
    err = H264E_sizeof(&p, &t->persist_size, &t->scratch_size);
    if (err)
    {
        return err;
    }
    t->enc = (H264E_persist_t *)malloc((size_t)t->persist_size);
    t->scratch = (H264E_scratch_t *)malloc((size_t)t->scratch_size);
    t->yuv = (unsigned char *)malloc((size_t)w * h * 3 / 2);
    if (!t->enc || !t->scratch || !t->yuv)
    {
        return H264E_STATUS_BAD_ARGUMENT;
    }
    err = H264E_init(t->enc, &p);
    if (err)
    {
        return err;
    }
    t->width = w;
    t->height = h;
    t->in.yuv[0] = t->yuv;
    t->in.yuv[1] = t->yuv + w * h;
    t->in.yuv[2] = t->yuv + w * h * 5 / 4;
    t->in.stride[0] = w;
    t->in.stride[1] = w / 2;
    t->in.stride[2] = w / 2;
    memset(&t->run, 0, sizeof(t->run));
    t->run.qp_min = t->run.qp_max = 30;
    t->run.desired_frame_bytes = 1000;
    t->run.nalu_callback = record_callback;
    t->run.nalu_callback_token = &t->rec;
    recorder_reset(&t->rec);
    return 0;
}

static void close_enc(test_enc_t *t)
{
    free(t->enc);
    free(t->scratch);
    free(t->yuv);
    memset(t, 0, sizeof(*t));
}

static int encode_frame(test_enc_t *t, int frame, int frame_type,
    unsigned char **out, int *out_size)
{
    recorder_reset(&t->rec);
    fill_i420(t->yuv, t->width, t->height, frame);
    t->run.frame_type = frame_type;
    return H264E_encode(t->enc, t->scratch, &t->run, &t->in, out, out_size);
}

static unsigned expected_initial_delay(void)
{
    return (unsigned)((unsigned long long)TEST_VBV * 8ull * 90000ull / TEST_BITRATE);
}

static const nal_view_t *find_nal(const nal_view_t *nals, int n, int type)
{
    int i;
    for (i = 0; i < n; i++)
    {
        if (nals[i].type == type)
        {
            return nals + i;
        }
    }
    return 0;
}

/* Count SEI NALs of a given payload type in one frame. */
static int count_sei(const recorder_t *rec, int payload_type)
{
    int i, cnt = 0;
    for (i = 0; i < rec->nnal; i++)
    {
        sei_info_t s;
        if (rec->nals[i].type != 6)
        {
            continue;
        }
        parse_sei(rec->nals + i, &s);
        if (s.payload_type == payload_type && !s.overread)
        {
            cnt++;
        }
    }
    return cnt;
}

/* Return the single pic_timing SEI of a frame, 0 when absent. */
static const nal_view_t *pic_timing_nal(const recorder_t *rec)
{
    int i;
    for (i = 0; i < rec->nnal; i++)
    {
        sei_info_t s;
        if (rec->nals[i].type != 6)
        {
            continue;
        }
        parse_sei(rec->nals + i, &s);
        if (s.payload_type == 1 && !s.overread)
        {
            return rec->nals + i;
        }
    }
    return 0;
}

/**************************** test_sps ****************************/

void test_sps(void)
{
    test_enc_t t[1];
    unsigned char *out;
    int out_size, err;
    nal_view_t nals[32];
    int nnal, i;
    const nal_view_t *sps;
    sps_info_t s;

    /* Enabled HRD: SPS carries timing info and one NAL HRD CPB. */
    CASE();
    err = open_enc(t, 1, TEST_W, TEST_H, 4);
    CHECK(err == 0);
    CHECK(encode_frame(t, 0, H264E_FRAME_TYPE_DEFAULT, &out, &out_size) == 0);
    nnal = split_annexb(out, out_size, nals, 32);
    sps = find_nal(nals, nnal, 7);
    CHECK(sps != 0);
    parse_sps(sps, &s);
    CHECK(s.overread == 0);
    CHECK(s.vui_present == 1);
    CHECK(s.num_units_in_tick == TEST_TICK);
    CHECK(s.time_scale == TEST_TIMESCALE);
    CHECK(s.fixed_frame_rate == 1);
    CHECK(s.nal_hrd_present == 1);
    CHECK(s.cpb_cnt_minus1 == 0);
    CHECK(s.bit_rate_value_minus1 == TEST_BITRATE / 64u - 1u);
    CHECK(s.cpb_size_value_minus1 == (unsigned)TEST_VBV / 16u - 1u);
    CHECK(s.bit_rate_scale == 0);
    CHECK(s.cpb_size_scale == 0);
    CHECK(s.cbr_flag == 0);
    CHECK(s.init_cpb_len_m1 == 23);
    CHECK(s.cpb_len_m1 == 15);
    CHECK(s.dpb_len_m1 == 15);
    CHECK(s.time_offset_length == 0);
    CHECK(s.vcl_hrd_present == 0);
    CHECK(s.low_delay_hrd == 1);
    CHECK(s.pic_struct_present == 0);
    CHECK(s.bitstream_restriction == 0);
    close_enc(t);

    /* Values needing non-zero scales are still represented exactly. */
    CASE();
    {
        H264E_create_param_t p;
        base_params(&p, TEST_W, TEST_H, 4, 1);
        /* scale 0 overflows 16 bits; scale 1 is the smallest exact fit */
        p.hrd_bitrate_bps = 6400000u;    /* /128 = 50000 -> minus1 49999 */
        p.vbv_size_bytes = 1920000;      /* /32  = 60000 -> minus1 59999 */
        err = H264E_sizeof(&p, &t->persist_size, &t->scratch_size);
        CHECK(err == 0);
        t->enc = (H264E_persist_t *)malloc((size_t)t->persist_size);
        t->scratch = (H264E_scratch_t *)malloc((size_t)t->scratch_size);
        t->yuv = (unsigned char *)malloc(TEST_FRAME_SIZE);
        CHECK(t->enc && t->scratch && t->yuv);
        CHECK(H264E_init(t->enc, &p) == 0);
        t->width = TEST_W; t->height = TEST_H;
        t->in.yuv[0] = t->yuv;
        t->in.yuv[1] = t->yuv + TEST_W * TEST_H;
        t->in.yuv[2] = t->yuv + TEST_W * TEST_H * 5 / 4;
        t->in.stride[0] = TEST_W; t->in.stride[1] = TEST_W / 2; t->in.stride[2] = TEST_W / 2;
        memset(&t->run, 0, sizeof(t->run));
        t->run.qp_min = t->run.qp_max = 30;
        t->run.desired_frame_bytes = 1000;
        t->run.nalu_callback = record_callback;
        t->run.nalu_callback_token = &t->rec;
        recorder_reset(&t->rec);
        fill_i420(t->yuv, TEST_W, TEST_H, 0);
        CHECK(H264E_encode(t->enc, t->scratch, &t->run, &t->in, &out, &out_size) == 0);
        nnal = split_annexb(out, out_size, nals, 32);
        sps = find_nal(nals, nnal, 7);
        CHECK(sps != 0);
        parse_sps(sps, &s);
        CHECK(s.overread == 0);
        CHECK(s.bit_rate_scale == 1);
        CHECK(s.bit_rate_value_minus1 == 49999u);
        CHECK(s.cpb_size_scale == 1);
        CHECK(s.cpb_size_value_minus1 == 59999u);
        close_enc(t);
    }

    /* Disabled HRD: SPS has no VUI and no SEI is emitted. */
    CASE();
    err = open_enc(t, 0, TEST_W, TEST_H, 4);
    CHECK(err == 0);
    for (i = 0; i < 6; i++)
    {
        CHECK(encode_frame(t, i, H264E_FRAME_TYPE_DEFAULT, &out, &out_size) == 0);
        nnal = split_annexb(out, out_size, nals, 32);
        sps = find_nal(nals, nnal, 7);
        if (sps)
        {
            parse_sps(sps, &s);
            CHECK(s.overread == 0);
            CHECK(s.vui_present == 0);
        }
        CHECK(find_nal(nals, nnal, 6) == 0);
    }
    close_enc(t);

    /* Every illegal combination is rejected by both sizeof and init. */
    CASE();
    {
        H264E_create_param_t bad;
        int expect;
        for (i = 0; i < 8; i++)
        {
            base_params(&bad, TEST_W, TEST_H, 4, 1);
            switch (i)
            {
            case 0: bad.hrd_num_units_in_tick = 0; break;
            case 1: bad.hrd_time_scale = 0; break;
            case 2: bad.vbv_size_bytes = 0; break;
            case 3: bad.num_layers = 2; break;
            case 4: bad.max_threads = 2; break;
            case 5: bad.hrd_bitrate_bps = 65; break;       /* not divisible by 64 */
            case 6: bad.vbv_size_bytes = 17; break;        /* not divisible by 16 */
            case 7: bad.vbv_size_bytes = 1600000; break;   /* delay > 24 bits */
            }
            expect = H264E_sizeof(&bad, &t->persist_size, &t->scratch_size);
            CHECK(expect == H264E_STATUS_BAD_PARAMETER);
            t->enc = (H264E_persist_t *)malloc(1024);
            CHECK(t->enc != 0);
            expect = H264E_init(t->enc, &bad);
            CHECK(expect == H264E_STATUS_BAD_PARAMETER);
            free(t->enc);
            t->enc = 0;
        }
        /* bitrate 0 takes the legacy path and ignores the other fields */
        base_params(&bad, TEST_W, TEST_H, 4, 0);
        bad.hrd_num_units_in_tick = 0;
        bad.hrd_time_scale = 0;
        CHECK(H264E_sizeof(&bad, &t->persist_size, &t->scratch_size) == 0);
    }
}

/**************************** test_schedule ****************************/

void test_schedule(void)
{
    test_enc_t t[1];
    unsigned char *out;
    int out_size, f;
    unsigned expected_delay;

    /* Normal GOP: IDR every 4 frames, clock restarts at each IDR. */
    CASE();
    CHECK(open_enc(t, 1, TEST_W, TEST_H, 4) == 0);
    expected_delay = expected_initial_delay();
    for (f = 0; f < 10; f++)
    {
        int is_idr = (f % 4 == 0);
        unsigned want = (unsigned)((2 * (f % 4)) & 0xFFFF);
        const nal_view_t *pt;
        sei_info_t sei;
        CHECK(encode_frame(t, f, H264E_FRAME_TYPE_DEFAULT, &out, &out_size) == 0);
        CHECK(count_sei(&t->rec, 0) == (is_idr ? 1 : 0));
        CHECK(count_sei(&t->rec, 1) == 1);
        pt = pic_timing_nal(&t->rec);
        CHECK(pt != 0);
        parse_sei(pt, &sei);
        CHECK(sei.overread == 0);
        CHECK(sei.cpb_removal_delay == want);
        CHECK(sei.dpb_output_delay == 0);
        if (is_idr)
        {
            int i;
            int found_bp = 0;
            for (i = 0; i < t->rec.nnal; i++)
            {
                sei_info_t bp;
                if (t->rec.nals[i].type != 6)
                {
                    continue;
                }
                parse_sei(t->rec.nals + i, &bp);
                if (bp.payload_type == 0 && !bp.overread)
                {
                    found_bp = 1;
                    CHECK(bp.sps_id == 0);
                    CHECK(bp.init_cpb_removal_delay == expected_delay);
                    CHECK(bp.init_cpb_removal_delay_offset == 0);
                }
            }
            CHECK(found_bp == 1);
        }
    }
    close_enc(t);

    /* Manual IDR restarts the clock even inside a GOP. */
    CASE();
    CHECK(open_enc(t, 1, TEST_W, TEST_H, 0) == 0);   /* infinite GOP */
    for (f = 0; f < 7; f++)
    {
        int type = (f == 4) ? H264E_FRAME_TYPE_KEY : H264E_FRAME_TYPE_DEFAULT;
        unsigned want = (f < 4) ? (unsigned)(2 * f)
                      : (f == 4) ? 0u : (unsigned)(2 * (f - 4));
        const nal_view_t *pt;
        sei_info_t sei;
        CHECK(encode_frame(t, f, type, &out, &out_size) == 0);
        CHECK(count_sei(&t->rec, 0) == (f == 0 || f == 4 ? 1 : 0));
        pt = pic_timing_nal(&t->rec);
        CHECK(pt != 0);
        parse_sei(pt, &sei);
        CHECK(sei.overread == 0);
        CHECK(sei.cpb_removal_delay == want);
    }
    close_enc(t);

    /* Droppable pictures consume time exactly like reference pictures. */
    CASE();
    CHECK(open_enc(t, 1, TEST_W, TEST_H, 0) == 0);
    for (f = 0; f < 5; f++)
    {
        int type = (f >= 2) ? H264E_FRAME_TYPE_DROPPABLE : H264E_FRAME_TYPE_DEFAULT;
        const nal_view_t *pt;
        sei_info_t sei;
        CHECK(encode_frame(t, f, type, &out, &out_size) == 0);
        CHECK(count_sei(&t->rec, 0) == (f == 0 ? 1 : 0));
        pt = pic_timing_nal(&t->rec);
        CHECK(pt != 0);
        parse_sei(pt, &sei);
        CHECK(sei.overread == 0);
        CHECK(sei.cpb_removal_delay == (unsigned)(2 * f));
    }
    close_enc(t);

    /* 16-bit wrap of cpb_removal_delay across a long single-IDR stream. */
    CASE();
    {
        H264E_create_param_t p;
        const int w = 16, h = 16;
        base_params(&p, w, h, 0, 1);
        CHECK(H264E_sizeof(&p, &t->persist_size, &t->scratch_size) == 0);
        t->enc = (H264E_persist_t *)malloc((size_t)t->persist_size);
        t->scratch = (H264E_scratch_t *)malloc((size_t)t->scratch_size);
        t->yuv = (unsigned char *)malloc((size_t)w * h * 3 / 2);
        CHECK(t->enc && t->scratch && t->yuv);
        CHECK(H264E_init(t->enc, &p) == 0);
        t->width = w; t->height = h;
        t->in.yuv[0] = t->yuv;
        t->in.yuv[1] = t->yuv + w * h;
        t->in.yuv[2] = t->yuv + w * h * 5 / 4;
        t->in.stride[0] = w; t->in.stride[1] = w / 2; t->in.stride[2] = w / 2;
        memset(&t->run, 0, sizeof(t->run));
        t->run.qp_min = t->run.qp_max = 30;
        t->run.desired_frame_bytes = 1000;
        t->run.nalu_callback = record_callback;
        t->run.nalu_callback_token = &t->rec;
        for (f = 0; f < 32769; f++)
        {
            int i;
            unsigned want = (unsigned)((2 * f) & 0xFFFF);
            const nal_view_t *pt;
            sei_info_t sei;
            recorder_reset(&t->rec);
            fill_i420(t->yuv, w, h, f & 255);
            CHECK(H264E_encode(t->enc, t->scratch, &t->run, &t->in, &out, &out_size) == 0);
            if (f < 32764)
            {
                continue;
            }
            pt = pic_timing_nal(&t->rec);
            CHECK(pt != 0);
            parse_sei(pt, &sei);
            CHECK(sei.overread == 0);
            CHECK(sei.cpb_removal_delay == want);
            for (i = 0; i < t->rec.nnal; i++)
            {
                /* only the first frame may carry a buffering_period */
                if (t->rec.nals[i].type == 6)
                {
                    sei_info_t any;
                    parse_sei(t->rec.nals + i, &any);
                    CHECK(any.payload_type != 0);
                }
            }
        }
        close_enc(t);
    }

    /* Two interleaved instances keep independent HRD clocks. */
    CASE();
    {
        test_enc_t a[1], b[1];
        CHECK(open_enc(a, 1, TEST_W, TEST_H, 4) == 0);
        CHECK(open_enc(b, 1, TEST_W, TEST_H, 4) == 0);
        for (f = 0; f < 8; f++)
        {
            const nal_view_t *pt;
            sei_info_t sa, sb;
            unsigned char *oa, *ob;
            int na, nb;
            CHECK(encode_frame(a, f, H264E_FRAME_TYPE_DEFAULT, &oa, &na) == 0);
            CHECK(encode_frame(b, f + 100, H264E_FRAME_TYPE_DEFAULT, &ob, &nb) == 0);
            pt = pic_timing_nal(&a->rec);
            CHECK(pt != 0);
            parse_sei(pt, &sa);
            CHECK(sa.overread == 0);
            CHECK(sa.cpb_removal_delay == (unsigned)((2 * (f % 4)) & 0xFFFF));
            pt = pic_timing_nal(&b->rec);
            CHECK(pt != 0);
            parse_sei(pt, &sb);
            CHECK(sb.overread == 0);
            CHECK(sb.cpb_removal_delay == (unsigned)((2 * (f % 4)) & 0xFFFF));
            CHECK(count_sei(&a->rec, 0) == (f % 4 == 0 ? 1 : 0));
            CHECK(count_sei(&b->rec, 0) == (f % 4 == 0 ? 1 : 0));
        }
        close_enc(a);
        close_enc(b);
    }
}

/**************************** test_callbacks ****************************/

void test_callbacks(void)
{
    test_enc_t t[1];
    int f;

    CASE();
    CHECK(open_enc(t, 1, TEST_W, TEST_H, 4) == 0);
    t->run.desired_nalu_bytes = 60;   /* soft slice threshold, multi-NAL IDR */
    for (f = 0; f < 6; f++)
    {
        unsigned char *out;
        int out_size, i;
        int type = (f == 2) ? H264E_FRAME_TYPE_DROPPABLE
                 : (f == 5) ? H264E_FRAME_TYPE_KEY
                 : H264E_FRAME_TYPE_DEFAULT;
        nal_view_t annex[32];
        int nannex;
        CHECK(encode_frame(t, f, type, &out, &out_size) == 0);
        nannex = split_annexb(out, out_size, annex, 32);

        /* Annex-B output and callback stream: same NAL count, order, bytes */
        CHECK(nannex == t->rec.nnal);
        CHECK(nannex >= 2);
        for (i = 0; i < nannex; i++)
        {
            CHECK(annex[i].type == t->rec.nals[i].type);
            CHECK(annex[i].len == t->rec.nals[i].len);
            CHECK(memcmp(annex[i].p, t->rec.nals[i].p, (size_t)annex[i].len) == 0);
        }

        if (f == 0 || f == 5)
        {
            sei_info_t sei;
            /* IDR picture: SPS, PPS, buffering_period, pic_timing, VCL */
            CHECK(t->rec.nals[0].type == 7);
            CHECK(t->rec.nals[1].type == 8);
            CHECK(t->rec.nals[2].type == 6);
            CHECK(t->rec.nals[3].type == 6);
            parse_sei(t->rec.nals + 2, &sei);
            CHECK(sei.payload_type == 0);
            parse_sei(t->rec.nals + 3, &sei);
            CHECK(sei.payload_type == 1);
            /* remaining NALs are all IDR slice NALs */
            for (i = 4; i < t->rec.nnal; i++)
            {
                CHECK(t->rec.nals[i].type == 5);
            }
        } else
        {
            sei_info_t sei;
            /* non-IDR: pic_timing first, then slice NALs */
            CHECK(t->rec.nals[0].type == 6);
            parse_sei(t->rec.nals + 0, &sei);
            CHECK(sei.payload_type == 1);
            for (i = 1; i < t->rec.nnal; i++)
            {
                CHECK(t->rec.nals[i].type == 1);
            }
        }
    }
    close_enc(t);

    /* Legacy mode: callback and Annex-B still match, no SEI is inserted. */
    CASE();
    CHECK(open_enc(t, 0, TEST_W, TEST_H, 4) == 0);
    t->run.desired_nalu_bytes = 60;
    for (f = 0; f < 4; f++)
    {
        unsigned char *out;
        int out_size, i;
        nal_view_t annex[32];
        int nannex;
        CHECK(encode_frame(t, f, H264E_FRAME_TYPE_DEFAULT, &out, &out_size) == 0);
        nannex = split_annexb(out, out_size, annex, 32);
        CHECK(nannex == t->rec.nnal);
        for (i = 0; i < nannex; i++)
        {
            CHECK(annex[i].type == t->rec.nals[i].type);
            CHECK(memcmp(annex[i].p, t->rec.nals[i].p, (size_t)annex[i].len) == 0);
            CHECK(annex[i].type != 6);
        }
    }
    close_enc(t);
}

/**************************** test_compatibility ****************************/

static int open_fixed_qp(test_enc_t *t, int hrd)
{
    H264E_create_param_t p;
    int err;
    memset(t, 0, sizeof(*t));
    base_params(&p, TEST_W, TEST_H, 4, hrd);
    p.const_input_flag = 0;   /* encoder reconstructs into the input plane */
    err = H264E_sizeof(&p, &t->persist_size, &t->scratch_size);
    if (err)
    {
        return err;
    }
    t->enc = (H264E_persist_t *)malloc((size_t)t->persist_size);
    t->scratch = (H264E_scratch_t *)malloc((size_t)t->scratch_size);
    t->yuv = (unsigned char *)malloc(TEST_FRAME_SIZE);
    if (!t->enc || !t->scratch || !t->yuv)
    {
        return H264E_STATUS_BAD_ARGUMENT;
    }
    if (H264E_init(t->enc, &p))
    {
        return H264E_STATUS_BAD_PARAMETER;
    }
    t->width = TEST_W;
    t->height = TEST_H;
    t->in.yuv[0] = t->yuv;
    t->in.yuv[1] = t->yuv + TEST_W * TEST_H;
    t->in.yuv[2] = t->yuv + TEST_W * TEST_H * 5 / 4;
    t->in.stride[0] = TEST_W;
    t->in.stride[1] = TEST_W / 2;
    t->in.stride[2] = TEST_W / 2;
    memset(&t->run, 0, sizeof(t->run));
    t->run.qp_min = t->run.qp_max = 30;   /* fixed QP control */
    t->run.desired_frame_bytes = 1000;    /* still supplied, never overwritten */
    t->run.nalu_callback = record_callback;
    t->run.nalu_callback_token = &t->rec;
    return 0;
}

void test_compatibility(void)
{
    test_enc_t off[1], on[1];
    int f;
    unsigned char recon_off[TEST_FRAME_SIZE];
    unsigned char recon_on[TEST_FRAME_SIZE];

    CASE();
    CHECK(open_fixed_qp(off, 0) == 0);
    CHECK(open_fixed_qp(on, 1) == 0);
    for (f = 0; f < 6; f++)
    {
        unsigned char *ooff, *oon;
        int noff, non, i, j;
        recorder_t *ro = &off->rec, *rn = &on->rec;
        CHECK(encode_frame(off, f, H264E_FRAME_TYPE_DEFAULT, &ooff, &noff) == 0);
        memcpy(recon_off, off->yuv, TEST_FRAME_SIZE);
        CHECK(encode_frame(on, f, H264E_FRAME_TYPE_DEFAULT, &oon, &non) == 0);
        memcpy(recon_on, on->yuv, TEST_FRAME_SIZE);

        /* reconstructed pixels must be identical */
        CHECK(memcmp(recon_off, recon_on, TEST_FRAME_SIZE) == 0);

        /* HRD-on stream is strictly larger due to the added headers only */
        CHECK(non > noff);

        /* every VCL NAL (and PPS) matches byte for byte; HRD adds no VCL data */
        for (i = 0, j = 0; i < ro->nnal; i++)
        {
            if (ro->nals[i].type != 1 && ro->nals[i].type != 5 && ro->nals[i].type != 8)
            {
                continue;
            }
            while (j < rn->nnal && rn->nals[j].type != ro->nals[i].type)
            {
                j++;
            }
            CHECK(j < rn->nnal);
            CHECK(ro->nals[i].len == rn->nals[j].len);
            CHECK(memcmp(ro->nals[i].p, rn->nals[j].p, (size_t)ro->nals[i].len) == 0);
            j++;
        }

        /* the legacy stream never contains SEI, and its SPS has no VUI */
        for (i = 0; i < ro->nnal; i++)
        {
            sps_info_t sps;
            CHECK(ro->nals[i].type != 6);
            if (ro->nals[i].type == 7)
            {
                parse_sps(ro->nals + i, &sps);
                CHECK(sps.overread == 0);
                CHECK(sps.vui_present == 0);
            }
        }

        /* fixed-QP run params are preserved on both instances */
        CHECK(off->run.qp_min == 30 && off->run.qp_max == 30);
        CHECK(off->run.desired_frame_bytes == 1000);
        CHECK(on->run.qp_min == 30 && on->run.qp_max == 30);
        CHECK(on->run.desired_frame_bytes == 1000);
    }
    close_enc(off);
    close_enc(on);
}
