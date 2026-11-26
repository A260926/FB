/*
*   Regression tests for NAL HRD signaling (VUI/HRD + buffering_period and
*   pic_timing SEI) in the single-layer AVC encoder.
*
*   All bitstream parsing is self-contained: Annex-B start codes are split
*   and NAL units are unescaped independently of the encoder internals.
*/
#include "../task_support/case_harness.h"
#include <string.h>
#include <stdint.h>
#include "minih264e.h"

#define TW 64
#define TH 64
#define TYSIZE (TW * TH)
#define TFSIZE (TW * TH * 3 / 2)

#define NAL_SPS 7
#define NAL_PPS 8
#define NAL_SEI 6
#define NAL_IDR 5
#define NAL_P   1

typedef struct
{
    const unsigned char *data;
    int len;
} nal_view_t;

/* NAL units collected through nalu_callback (copied, no start codes) */
typedef struct
{
    nal_view_t nals[512];
    int count;
    unsigned char arena[1u << 20];
    int arena_pos;
} collector_t;

static collector_t g_cb;

static void collect_nalu(const unsigned char *p, int n, void *token)
{
    collector_t *c = (collector_t *)token;
    if (c->count >= (int)(sizeof(c->nals) / sizeof(c->nals[0])) ||
        c->arena_pos + n > (int)sizeof(c->arena))
    {
        CHECK(0);
        return;
    }
    c->nals[c->count].data = c->arena + c->arena_pos;
    c->nals[c->count].len = n;
    memcpy(c->arena + c->arena_pos, p, n);
    c->arena_pos += n;
    c->count++;
}

static void collector_reset(collector_t *c)
{
    c->count = 0;
    c->arena_pos = 0;
}

/* Split an Annex-B buffer into NAL units (start codes stripped) */
static int split_annexb(const unsigned char *p, int n, nal_view_t *out, int maxn)
{
    int i = 0, cnt = 0;
    while (i + 2 < n)
    {
        int sc = 0;
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1)
            sc = 3;
        else if (i + 3 < n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1)
            sc = 4;
        if (!sc)
        {
            i++;
            continue;
        }
        i += sc;
        {
            int start = i;
            while (i < n)
            {
                if (i + 2 < n && p[i] == 0 && p[i + 1] == 0 &&
                    (p[i + 2] == 1 || (i + 3 < n && p[i + 2] == 0 && p[i + 3] == 1)))
                    break;
                i++;
            }
            if (cnt < maxn)
            {
                out[cnt].data = p + start;
                out[cnt].len = i - start;
            }
            cnt++;
        }
    }
    return cnt;
}

/* Remove emulation prevention bytes; input is a NAL unit including header */
static int unescape_nal(const nal_view_t *nal, unsigned char *rbsp, int cap)
{
    int i, k = 0, z = 0;
    for (i = 1; i < nal->len; i++)
    {
        if (z == 2 && nal->data[i] == 3)
        {
            z = 0;
            continue;
        }
        CHECK(k < cap);
        rbsp[k++] = nal->data[i];
        z = nal->data[i] == 0 ? z + 1 : 0;
    }
    return k;
}

typedef struct
{
    const unsigned char *d;
    int bitpos;
    int nbits;
} bit_reader_t;

static unsigned br_bit(bit_reader_t *b)
{
    unsigned v;
    CHECK(b->bitpos < b->nbits);
    v = (b->d[b->bitpos >> 3] >> (7 - (b->bitpos & 7))) & 1u;
    b->bitpos++;
    return v;
}

static unsigned br_bits(bit_reader_t *b, int n)
{
    unsigned v = 0;
    while (n--)
        v = v * 2 + br_bit(b);
    return v;
}

static unsigned br_ue(bit_reader_t *b)
{
    int z = 0;
    while (!br_bit(b))
    {
        CHECK(++z < 32);
    }
    return z ? ((1u << z) - 1) + br_bits(b, z) : 0;
}

typedef struct
{
    int sps_id;
    int vui_present;
    unsigned num_units_in_tick;
    unsigned time_scale;
    int fixed_frame_rate;
    int nal_hrd_present;
    int vcl_hrd_present;
    unsigned cpb_cnt_minus1;
    unsigned bit_rate_scale;
    unsigned cpb_size_scale;
    unsigned bit_rate_value_minus1;
    unsigned cpb_size_value_minus1;
    int cbr_flag;
    unsigned init_delay_len_m1;
    unsigned cpb_delay_len_m1;
    unsigned dpb_delay_len_m1;
    unsigned time_offset_length;
    int low_delay_hrd_flag;
    int pic_struct_present_flag;
    int bitstream_restriction_flag;
} sps_info_t;

static void parse_sps(const nal_view_t *nal, sps_info_t *o)
{
    unsigned char rbsp[1024];
    bit_reader_t b[1];
    int k = unescape_nal(nal, rbsp, sizeof(rbsp));
    memset(o, 0, sizeof(*o));
    b->d = rbsp;
    b->bitpos = 0;
    b->nbits = k * 8;

    br_bits(b, 24);                 /* profile_idc, constraints, level_idc */
    o->sps_id = (int)br_ue(b);
    br_ue(b);                       /* log2_max_frame_num_minus4 */
    CHECK(br_ue(b) == 2);           /* pic_order_cnt_type */
    br_ue(b);                       /* max_num_ref_frames */
    br_bit(b);                      /* gaps_in_frame_num_allowed_flag */
    br_ue(b);                       /* pic_width_in_mbs_minus1 */
    br_ue(b);                       /* pic_height_in_map_units_minus1 */
    CHECK(br_bit(b) == 1);          /* frame_mbs_only_flag */
    br_bit(b);                      /* direct_8x8_inference_flag */
    if (br_bit(b))                  /* frame_cropping_flag */
    {
        br_ue(b); br_ue(b); br_ue(b); br_ue(b);
    }
    o->vui_present = (int)br_bit(b);
    if (!o->vui_present)
        return;

    CHECK(br_bit(b) == 0);          /* aspect_ratio_info_present_flag */
    CHECK(br_bit(b) == 0);          /* overscan_info_present_flag */
    CHECK(br_bit(b) == 0);          /* video_signal_type_present_flag */
    CHECK(br_bit(b) == 0);          /* chroma_loc_info_present_flag */
    CHECK(br_bit(b) == 1);          /* timing_info_present_flag */
    o->num_units_in_tick = br_bits(b, 32);
    o->time_scale = br_bits(b, 32);
    o->fixed_frame_rate = (int)br_bit(b);
    o->nal_hrd_present = (int)br_bit(b);
    o->vcl_hrd_present = (int)br_bit(b);
    CHECK(o->nal_hrd_present == 1);
    CHECK(o->vcl_hrd_present == 0);

    o->cpb_cnt_minus1 = br_ue(b);
    CHECK(o->cpb_cnt_minus1 == 0);  /* one CPB */
    o->bit_rate_scale = br_bits(b, 4);
    o->cpb_size_scale = br_bits(b, 4);
    o->bit_rate_value_minus1 = br_ue(b);
    o->cpb_size_value_minus1 = br_ue(b);
    o->cbr_flag = (int)br_bit(b);
    o->init_delay_len_m1 = br_bits(b, 5);
    o->cpb_delay_len_m1 = br_bits(b, 5);
    o->dpb_delay_len_m1 = br_bits(b, 5);
    o->time_offset_length = br_bits(b, 5);

    o->low_delay_hrd_flag = (int)br_bit(b);
    o->pic_struct_present_flag = (int)br_bit(b);
    o->bitstream_restriction_flag = (int)br_bit(b);
}

typedef struct
{
    unsigned type;
    const unsigned char *payload;
    unsigned size;
    int trailing_byte_ok;
} sei_msg_t;

static int parse_sei(const nal_view_t *nal, sei_msg_t *out, int maxmsg)
{
    unsigned char rbsp[256];
    int k = unescape_nal(nal, rbsp, sizeof(rbsp));
    int pos = 0, cnt = 0;
    while (pos < k && rbsp[pos] != 0x80)
    {
        unsigned type = 0, size = 0;
        while (pos < k && rbsp[pos] == 0xFF)
        {
            type += 255;
            pos++;
        }
        CHECK(pos < k);
        type += rbsp[pos++];
        while (pos < k && rbsp[pos] == 0xFF)
        {
            size += 255;
            pos++;
        }
        CHECK(pos < k);
        size += rbsp[pos++];
        CHECK(pos + (int)size <= k);
        if (cnt < maxmsg)
        {
            out[cnt].type = type;
            out[cnt].payload = rbsp + pos;
            out[cnt].size = size;
            out[cnt].trailing_byte_ok = (pos + (int)size + 1 == k && rbsp[k - 1] == 0x80);
        }
        cnt++;
        pos += size;
    }
    return cnt;
}

static unsigned nal_type(const nal_view_t *nal)
{
    return nal->data[0] & 0x1Fu;
}

static int has_start_code(const unsigned char *p, int n)
{
    int i;
    for (i = 0; i + 2 < n; i++)
        if (p[i] == 0 && p[i + 1] == 0 &&
            (p[i + 2] == 1 || (i + 3 < n && p[i + 2] == 0 && p[i + 3] == 1)))
            return 1;
    return 0;
}

static void gen_i420(unsigned char *frame, int f)
{
    int r, c;
    for (r = 0; r < TH; r++)
        for (c = 0; c < TW; c++)
            frame[r * TW + c] = (unsigned char)((r * 7 + c * 13 + f * 29) & 255);
    for (r = 0; r < TH / 2; r++)
        for (c = 0; c < TW / 2; c++)
        {
            frame[TYSIZE + r * (TW / 2) + c] = (unsigned char)(128 + ((r + c + f) & 15));
            frame[TYSIZE + TYSIZE / 4 + r * (TW / 2) + c] =
                (unsigned char)(128 - ((r * 3 + c + f) & 15));
        }
}

static H264E_create_param_t base_param(void)
{
    H264E_create_param_t p;
    memset(&p, 0, sizeof(p));
    p.width = TW;
    p.height = TH;
    p.gop = 4;
    p.const_input_flag = 1;
    p.vbv_size_bytes = 8192;
#if H264E_SVC_API
    p.num_layers = 1;
#endif
#if H264E_MAX_THREADS
    p.max_threads = 1;
#endif
    p.sps_id = 0;
    return p;
}

static H264E_persist_t *create_enc(const H264E_create_param_t *p, H264E_scratch_t **scratch)
{
    int sp = 0, ss = 0;
    void *mem;
    H264E_persist_t *enc;
    CHECK(H264E_sizeof(p, &sp, &ss) == H264E_STATUS_SUCCESS);
    mem = malloc((size_t)sp);
    *scratch = malloc((size_t)ss);
    CHECK(mem != 0 && *scratch != 0);
    enc = (H264E_persist_t *)mem;
    CHECK(H264E_init(enc, p) == H264E_STATUS_SUCCESS);
    return enc;
}

static int encode_frame(H264E_persist_t *enc, H264E_scratch_t *scratch,
    H264E_run_param_t *rp, unsigned char *frame, int frame_type,
    unsigned char **out, int *out_n)
{
    H264E_io_yuv_t in;
    in.yuv[0] = frame;
    in.yuv[1] = frame + TYSIZE;
    in.yuv[2] = frame + TYSIZE + TYSIZE / 4;
    in.stride[0] = TW;
    in.stride[1] = TW / 2;
    in.stride[2] = TW / 2;
    rp->frame_type = frame_type;
    return H264E_encode(enc, scratch, rp, &in, out, out_n);
}

static H264E_run_param_t fixed_qp_run(void)
{
    H264E_run_param_t r;
    memset(&r, 0, sizeof(r));
    r.qp_min = r.qp_max = 30;
    r.desired_frame_bytes = 1000;
    r.nalu_callback = collect_nalu;
    r.nalu_callback_token = &g_cb;
    return r;
}

/* ------------------------------------------------------------------ */
/* test_sps: validation and exact SPS/VUI/HRD contents                */
/* ------------------------------------------------------------------ */
void test_sps(void)
{
    H264E_create_param_t p;
    H264E_persist_t *enc;
    H264E_scratch_t *scratch;
    H264E_run_param_t rp;
    unsigned char *frame = malloc(TFSIZE);
    unsigned char *out;
    int out_n;

    CASE();
    /* Disabled HRD always stays valid regardless of the other fields */
    p = base_param();
    {
        int sp = 0, ss = 0;
        CHECK(H264E_sizeof(&p, &sp, &ss) == H264E_STATUS_SUCCESS);
        CHECK(sp > 0 && ss > 0);
    }

    CASE();
    p = base_param();
    p.hrd_bitrate_bps = 1000000;
    p.hrd_num_units_in_tick = 1001;
    p.hrd_time_scale = 48000;
    {
        int sp = 0, ss = 0;
        CHECK(H264E_sizeof(&p, &sp, &ss) == H264E_STATUS_SUCCESS);
    }

    /* Every illegal combination must be rejected by sizeof and init */
    p = base_param();
    p.hrd_bitrate_bps = 1000000;
    p.hrd_num_units_in_tick = 1001;
    p.hrd_time_scale = 48000;

    CASE();
    p.hrd_num_units_in_tick = 0;
    {
        int sp = 0, ss = 0;
        void *mem = malloc(1u << 16);
        CHECK(H264E_sizeof(&p, &sp, &ss) == H264E_STATUS_BAD_PARAMETER);
        CHECK(H264E_init(mem, &p) == H264E_STATUS_BAD_PARAMETER);
        free(mem);
    }

    CASE();
    p.hrd_num_units_in_tick = 1001;
    p.hrd_time_scale = 0;
    {
        int sp = 0, ss = 0;
        CHECK(H264E_sizeof(&p, &sp, &ss) == H264E_STATUS_BAD_PARAMETER);
    }

    CASE();
    p.hrd_time_scale = 48000;
    p.vbv_size_bytes = 0;
    {
        int sp = 0, ss = 0;
        CHECK(H264E_sizeof(&p, &sp, &ss) == H264E_STATUS_BAD_PARAMETER);
    }

    CASE();
    p.vbv_size_bytes = 8192;
#if H264E_SVC_API
    p.num_layers = 2;
    {
        int sp = 0, ss = 0;
        CHECK(H264E_sizeof(&p, &sp, &ss) == H264E_STATUS_BAD_PARAMETER);
    }
    p.num_layers = 1;
#endif

    CASE();
#if H264E_MAX_THREADS
    p.max_threads = 2;
    {
        int sp = 0, ss = 0;
        CHECK(H264E_sizeof(&p, &sp, &ss) == H264E_STATUS_BAD_PARAMETER);
    }
    p.max_threads = 1;
#endif

    CASE();
    /* Odd bitrate cannot be expressed with 64-bit base units */
    p.hrd_bitrate_bps = 1000001;
    {
        int sp = 0, ss = 0;
        CHECK(H264E_sizeof(&p, &sp, &ss) == H264E_STATUS_BAD_PARAMETER);
    }

    CASE();
    /* 8 bits of CPB do not divide the 16-bit base unit */
    p.hrd_bitrate_bps = 1000000;
    p.vbv_size_bytes = 1;
    {
        int sp = 0, ss = 0;
        CHECK(H264E_sizeof(&p, &sp, &ss) == H264E_STATUS_BAD_PARAMETER);
    }

    CASE();
    /* initial_cpb_removal_delay overflows 24 bits */
    p.vbv_size_bytes = 1 << 20;
    p.hrd_bitrate_bps = 64;
    {
        int sp = 0, ss = 0;
        CHECK(H264E_sizeof(&p, &sp, &ss) == H264E_STATUS_BAD_PARAMETER);
    }

    /* Valid stream: parse SPS/VUI/HRD and first SEI pair */
    CASE();
    p = base_param();
    p.hrd_bitrate_bps = 1000000;
    p.hrd_num_units_in_tick = 1001;
    p.hrd_time_scale = 48000;
    enc = create_enc(&p, &scratch);
    rp = fixed_qp_run();
    collector_reset(&g_cb);
    gen_i420(frame, 0);
    CHECK(encode_frame(enc, scratch, &rp, frame, H264E_FRAME_TYPE_DEFAULT, &out, &out_n) == 0);
    CHECK(g_cb.count >= 5);
    CHECK(nal_type(&g_cb.nals[0]) == NAL_SPS);
    CHECK(nal_type(&g_cb.nals[1]) == NAL_PPS);
    CHECK(nal_type(&g_cb.nals[2]) == NAL_SEI);
    CHECK(nal_type(&g_cb.nals[3]) == NAL_SEI);
    CHECK(nal_type(&g_cb.nals[4]) == NAL_IDR);
    {
        sps_info_t s;
        sei_msg_t sei[4];
        int nsei;
        parse_sps(&g_cb.nals[0], &s);
        CHECK(s.vui_present == 1);
        CHECK(s.sps_id == 0);
        CHECK(s.num_units_in_tick == 1001);
        CHECK(s.time_scale == 48000);
        CHECK(s.fixed_frame_rate == 1);
        CHECK(s.nal_hrd_present == 1 && s.vcl_hrd_present == 0);
        CHECK(s.cpb_cnt_minus1 == 0);
        CHECK(s.bit_rate_scale == 0 && s.bit_rate_value_minus1 == 15624);
        CHECK(s.cpb_size_scale == 0 && s.cpb_size_value_minus1 == 4095);
        CHECK(s.cbr_flag == 0);
        CHECK(s.init_delay_len_m1 == 23);
        CHECK(s.cpb_delay_len_m1 == 15);
        CHECK(s.dpb_delay_len_m1 == 15);
        CHECK(s.time_offset_length == 0);
        CHECK(s.low_delay_hrd_flag == 1);
        CHECK(s.pic_struct_present_flag == 0);
        CHECK(s.bitstream_restriction_flag == 0);

        nsei = parse_sei(&g_cb.nals[2], sei, 4);
        CHECK(nsei == 1 && sei[0].type == 0 && sei[0].size == 6);
        CHECK(sei[0].trailing_byte_ok == 1);
        CHECK((sei[0].payload[0] << 16 | sei[0].payload[1] << 8 | sei[0].payload[2]) == 5898);
        CHECK(sei[0].payload[3] == 0 && sei[0].payload[4] == 0 && sei[0].payload[5] == 0);

        nsei = parse_sei(&g_cb.nals[3], sei, 4);
        CHECK(nsei == 1 && sei[0].type == 1 && sei[0].size == 4);
        CHECK(sei[0].trailing_byte_ok == 1);
        CHECK((sei[0].payload[0] << 8 | sei[0].payload[1]) == 0);
        CHECK((sei[0].payload[2] << 8 | sei[0].payload[3]) == 0);
    }
    free(enc);
    free(scratch);

    /* Non-minimal scales are selected only when required */
    CASE();
    p = base_param();
    p.hrd_bitrate_bps = 16000000;   /* 62500 * 256, needs scale 2 */
    p.hrd_num_units_in_tick = 1001;
    p.hrd_time_scale = 48000;
    p.vbv_size_bytes = 16384;       /* 8192 * 16, scale 0 */
    enc = create_enc(&p, &scratch);
    rp = fixed_qp_run();
    collector_reset(&g_cb);
    gen_i420(frame, 0);
    CHECK(encode_frame(enc, scratch, &rp, frame, H264E_FRAME_TYPE_DEFAULT, &out, &out_n) == 0);
    {
        sps_info_t s;
        sei_msg_t sei[4];
        parse_sps(&g_cb.nals[0], &s);
        CHECK(s.bit_rate_scale == 2 && s.bit_rate_value_minus1 == 62499);
        CHECK(s.cpb_size_scale == 0 && s.cpb_size_value_minus1 == 8191);
        CHECK(parse_sei(&g_cb.nals[2], sei, 4) == 1);
        CHECK((sei[0].payload[0] << 16 | sei[0].payload[1] << 8 | sei[0].payload[2]) == 737);
    }
    free(enc);
    free(scratch);

    /* buffering_period follows the SPS of the active sps_id */
    CASE();
    p = base_param();
    p.sps_id = 2;
    p.hrd_bitrate_bps = 1000000;
    p.hrd_num_units_in_tick = 1;
    p.hrd_time_scale = 50;
    enc = create_enc(&p, &scratch);
    rp = fixed_qp_run();
    collector_reset(&g_cb);
    gen_i420(frame, 0);
    CHECK(encode_frame(enc, scratch, &rp, frame, H264E_FRAME_TYPE_DEFAULT, &out, &out_n) == 0);
    {
        sps_info_t s;
        parse_sps(&g_cb.nals[0], &s);
        CHECK(s.sps_id == 2);
        CHECK(s.num_units_in_tick == 1 && s.time_scale == 50);
        CHECK(nal_type(&g_cb.nals[1]) == NAL_PPS);
        CHECK(nal_type(&g_cb.nals[2]) == NAL_SEI);
    }
    free(enc);
    free(scratch);
    free(frame);
}

/* ------------------------------------------------------------------ */
/* test_schedule: clock sequencing over GOPs, IDRs, drops, slices     */
/* ------------------------------------------------------------------ */
typedef struct
{
    int is_idr;
    int bp_count;
    int pt_count;
    int vcl_count;
    int ordered;
    unsigned cpb_delay;
    unsigned dpb_delay;
} frame_obs_t;

static void observe_frame(const collector_t *c, frame_obs_t *o)
{
    int j;
    int saw_pt = 0, saw_vcl = 0;
    memset(o, 0, sizeof(*o));
    o->cpb_delay = 0xDEAD;
    for (j = 0; j < c->count; j++)
    {
        unsigned t = nal_type(&c->nals[j]);
        if (t == NAL_SPS)
            o->is_idr = 1;
        if (t == NAL_SEI)
        {
            sei_msg_t m[2];
            int nm = parse_sei(&c->nals[j], m, 2);
            CHECK(nm == 1);
            if (m[0].type == 0)
            {
                o->bp_count++;
                CHECK(!saw_vcl && !saw_pt);
            } else if (m[0].type == 1)
            {
                o->pt_count++;
                CHECK(!saw_vcl);            /* timing SEI precedes all VCL */
                saw_pt = 1;
                o->cpb_delay = (m[0].payload[0] << 8) | m[0].payload[1];
                o->dpb_delay = (m[0].payload[2] << 8) | m[0].payload[3];
            }
        } else if (t == NAL_IDR || t == NAL_P)
        {
            saw_vcl = 1;
            o->vcl_count++;
        }
    }
    o->ordered = (o->pt_count == 1 && o->vcl_count >= 1 &&
        (!o->is_idr || o->bp_count == 1));
}

void test_schedule(void)
{
    H264E_create_param_t p = base_param();
    H264E_persist_t *enc;
    H264E_scratch_t *scratch;
    H264E_run_param_t rp;
    unsigned char *frame = malloc(TFSIZE);
    unsigned char *out;
    int out_n, f;
    static const int ftypes[10] = {
        H264E_FRAME_TYPE_DEFAULT, H264E_FRAME_TYPE_DEFAULT,
        H264E_FRAME_TYPE_DEFAULT, H264E_FRAME_TYPE_DEFAULT,
        H264E_FRAME_TYPE_DEFAULT, H264E_FRAME_TYPE_DROPPABLE,
        H264E_FRAME_TYPE_DEFAULT, H264E_FRAME_TYPE_KEY,
        H264E_FRAME_TYPE_DEFAULT, H264E_FRAME_TYPE_DEFAULT
    };
    static const unsigned expect_delay[10] = {0, 2, 4, 6, 0, 2, 4, 0, 2, 4};
    static const int expect_idr[10] = {1, 0, 0, 0, 1, 0, 0, 1, 0, 0};

    p.hrd_bitrate_bps = 1000000;
    p.hrd_num_units_in_tick = 1001;
    p.hrd_time_scale = 48000;
    enc = create_enc(&p, &scratch);
    rp = fixed_qp_run();

    CASE();
    for (f = 0; f < 10; f++)
    {
        frame_obs_t o;
        collector_reset(&g_cb);
        gen_i420(frame, f);
        CHECK(encode_frame(enc, scratch, &rp, frame, ftypes[f], &out, &out_n) == 0);
        observe_frame(&g_cb, &o);
        CHECK(o.ordered == 1);
        CHECK(o.is_idr == expect_idr[f]);
        CHECK(o.bp_count == (expect_idr[f] ? 1 : 0));
        CHECK(o.pt_count == 1);
        CHECK(o.cpb_delay == expect_delay[f]);
        CHECK(o.dpb_delay == 0);
    }

    /* Multi-NAL picture via the desired_nalu_bytes soft slice threshold */
    CASE();
    collector_reset(&g_cb);
    rp.desired_nalu_bytes = 100;
    gen_i420(frame, 0);
    CHECK(encode_frame(enc, scratch, &rp, frame, H264E_FRAME_TYPE_KEY, &out, &out_n) == 0);
    {
        frame_obs_t o;
        observe_frame(&g_cb, &o);
        CHECK(o.vcl_count >= 2);          /* picture really split into slices */
        CHECK(o.pt_count == 1);           /* one pic_timing for the whole picture */
        CHECK(o.bp_count == 1);
    }
    rp.desired_nalu_bytes = 0;
    free(enc);
    free(scratch);

    /* Two interleaved instances keep independent clocks */
    CASE();
    {
        H264E_persist_t *ea, *eb;
        H264E_scratch_t *sa, *sb;
        H264E_run_param_t ra = fixed_qp_run(), rb = fixed_qp_run();
        unsigned char *fa = malloc(TFSIZE), *fb = malloc(TFSIZE);
        unsigned char *oa, *ob;
        int na, nb;
        unsigned da[3], db[4];
        int bpa = 0, bpb = 0, k;

        ea = create_enc(&p, &sa);
        eb = create_enc(&p, &sb);

        collector_reset(&g_cb);
        gen_i420(fa, 0);
        CHECK(encode_frame(ea, sa, &ra, fa, H264E_FRAME_TYPE_DEFAULT, &oa, &na) == 0);
        {
            frame_obs_t o;
            observe_frame(&g_cb, &o);
            da[0] = o.cpb_delay; bpa += o.bp_count;
        }
        collector_reset(&g_cb);
        gen_i420(fb, 0);
        CHECK(encode_frame(eb, sb, &rb, fb, H264E_FRAME_TYPE_DEFAULT, &ob, &nb) == 0);
        {
            frame_obs_t o;
            observe_frame(&g_cb, &o);
            db[0] = o.cpb_delay; bpb += o.bp_count;
        }
        collector_reset(&g_cb);
        gen_i420(fa, 1);
        CHECK(encode_frame(ea, sa, &ra, fa, H264E_FRAME_TYPE_DEFAULT, &oa, &na) == 0);
        {
            frame_obs_t o;
            observe_frame(&g_cb, &o);
            da[1] = o.cpb_delay; bpa += o.bp_count;
        }
        collector_reset(&g_cb);
        gen_i420(fb, 1);
        CHECK(encode_frame(eb, sb, &rb, fb, H264E_FRAME_TYPE_DEFAULT, &ob, &nb) == 0);
        {
            frame_obs_t o;
            observe_frame(&g_cb, &o);
            db[1] = o.cpb_delay; bpb += o.bp_count;
        }
        collector_reset(&g_cb);
        gen_i420(fb, 2);
        CHECK(encode_frame(eb, sb, &rb, fb, H264E_FRAME_TYPE_KEY, &ob, &nb) == 0);
        {
            frame_obs_t o;
            observe_frame(&g_cb, &o);
            db[2] = o.cpb_delay; bpb += o.bp_count;
        }
        collector_reset(&g_cb);
        gen_i420(fa, 2);
        CHECK(encode_frame(ea, sa, &ra, fa, H264E_FRAME_TYPE_DEFAULT, &oa, &na) == 0);
        {
            frame_obs_t o;
            observe_frame(&g_cb, &o);
            da[2] = o.cpb_delay; bpa += o.bp_count;
        }
        collector_reset(&g_cb);
        gen_i420(fb, 3);
        CHECK(encode_frame(eb, sb, &rb, fb, H264E_FRAME_TYPE_DEFAULT, &ob, &nb) == 0);
        {
            frame_obs_t o;
            observe_frame(&g_cb, &o);
            db[3] = o.cpb_delay; bpb += o.bp_count;
        }

        CHECK(da[0] == 0 && da[1] == 2 && da[2] == 4);
        CHECK(db[0] == 0 && db[1] == 2 && db[2] == 0 && db[3] == 2);
        CHECK(bpa == 1 && bpb == 2);
        for (k = 0; k < 3; k++)
            CHECK(da[k] < 0x10000);
        free(ea); free(sa); free(fa);
        free(eb); free(sb); free(fb);
    }

    /* cpb_removal_delay wraps modulo 16 bits; DROPPABLE-style flat input
       keeps 32k frames cheap enough for the unit test */
    CASE();
    {
        H264E_create_param_t wp = base_param();
        H264E_persist_t *we;
        H264E_scratch_t *ws;
        H264E_run_param_t wr;
        H264E_io_yuv_t win;
        unsigned char wf[16 * 16 * 3 / 2];
        unsigned char *wo;
        int wn, fi, ci = 0;
        unsigned cap[4];
        wp.width = wp.height = 16;
        wp.gop = 0;
        wp.hrd_bitrate_bps = 64000;
        wp.hrd_num_units_in_tick = 1;
        wp.hrd_time_scale = 50;
        we = create_enc(&wp, &ws);
        wr = fixed_qp_run();
        memset(wf, 128, sizeof(wf));
        win.yuv[0] = wf;
        win.yuv[1] = wf + 256;
        win.yuv[2] = wf + 320;
        win.stride[0] = 16;
        win.stride[1] = win.stride[2] = 8;
        wr.frame_type = H264E_FRAME_TYPE_DEFAULT;
        for (fi = 0; fi < 32770; fi++)
        {
            frame_obs_t o;
            collector_reset(&g_cb);
            CHECK(H264E_encode(we, ws, &wr, &win, &wo, &wn) == 0);
            observe_frame(&g_cb, &o);
            if (fi >= 32766 && fi <= 32769)
                cap[ci++] = o.cpb_delay;
        }
        CHECK(ci == 4);
        CHECK(cap[0] == 65532 && cap[1] == 65534 && cap[2] == 0 && cap[3] == 2);
        free(we);
        free(ws);
    }
    free(frame);
}

/* ------------------------------------------------------------------ */
/* test_callbacks: Annex-B output and nalu_callback parity            */
/* ------------------------------------------------------------------ */
void test_callbacks(void)
{
    H264E_create_param_t p = base_param();
    H264E_persist_t *enc;
    H264E_scratch_t *scratch;
    H264E_run_param_t rp;
    unsigned char *frame = malloc(TFSIZE);
    unsigned char *out;
    int out_n, f;

    p.hrd_bitrate_bps = 1000000;
    p.hrd_num_units_in_tick = 1001;
    p.hrd_time_scale = 48000;
    p.gop = 3;
    enc = create_enc(&p, &scratch);
    rp = fixed_qp_run();

    CASE();
    for (f = 0; f < 6; f++)
    {
        nal_view_t annex[128];
        int n_annex, n_cb, j;
        int sei_annex = 0, sei_cb = 0;
        collector_reset(&g_cb);
        gen_i420(frame, f);
        rp.desired_nalu_bytes = (f == 2 || f == 5) ? 100 : 0;
        CHECK(encode_frame(enc, scratch, &rp, frame, H264E_FRAME_TYPE_DEFAULT, &out, &out_n) == 0);

        n_annex = split_annexb(out, out_n, annex, 128);
        n_cb = g_cb.count;
        CHECK(n_annex == n_cb);
        for (j = 0; j < n_cb; j++)
        {
            CHECK(annex[j].len == g_cb.nals[j].len);
            CHECK(memcmp(annex[j].data, g_cb.nals[j].data, annex[j].len) == 0);
            CHECK(!has_start_code(g_cb.nals[j].data, g_cb.nals[j].len));
            if (nal_type(&annex[j]) == NAL_SEI)
                sei_annex++;
            if (nal_type(&g_cb.nals[j]) == NAL_SEI)
                sei_cb++;
        }
        CHECK(sei_annex == sei_cb && sei_annex >= 1);
    }
    free(enc);
    free(scratch);

    /* Legacy path keeps the same callback/Annex-B parity with no SEI */
    CASE();
    p = base_param();
    enc = create_enc(&p, &scratch);
    rp = fixed_qp_run();
    for (f = 0; f < 3; f++)
    {
        nal_view_t annex[64];
        int n_annex, j, sei = 0;
        collector_reset(&g_cb);
        gen_i420(frame, f);
        CHECK(encode_frame(enc, scratch, &rp, frame, H264E_FRAME_TYPE_DEFAULT, &out, &out_n) == 0);
        n_annex = split_annexb(out, out_n, annex, 64);
        CHECK(n_annex == g_cb.count);
        for (j = 0; j < g_cb.count; j++)
        {
            CHECK(annex[j].len == g_cb.nals[j].len);
            CHECK(memcmp(annex[j].data, g_cb.nals[j].data, annex[j].len) == 0);
            if (nal_type(&annex[j]) == NAL_SEI)
                sei++;
        }
        CHECK(sei == 0);
    }
    free(enc);
    free(scratch);
    free(frame);
}

/* ------------------------------------------------------------------ */
/* test_compatibility: HRD adds timing only, VCL and recon unchanged  */
/* ------------------------------------------------------------------ */
static int vcl_only(const nal_view_t *nals, int n, nal_view_t *out, int maxn)
{
    int i, k = 0;
    for (i = 0; i < n; i++)
    {
        unsigned t = nal_type(&nals[i]);
        if ((t == NAL_IDR || t == NAL_P) && k < maxn)
            out[k++] = nals[i];
    }
    return k;
}

void test_compatibility(void)
{
    H264E_create_param_t po = base_param(), ph = base_param();
    H264E_persist_t *enco, *ench;
    H264E_scratch_t *scro, *scrh;
    H264E_run_param_t ro, rh;
    unsigned char *fo = malloc(TFSIZE), *fh = malloc(TFSIZE), *src = malloc(TFSIZE);
    unsigned char *oo, *oh;
    int no, nh, f;

    /* Reconstruction is written into the input, so both need own buffers */
    po.const_input_flag = 0;
    ph.const_input_flag = 0;
    ph.hrd_bitrate_bps = 1000000;
    ph.hrd_num_units_in_tick = 1001;
    ph.hrd_time_scale = 48000;
    enco = create_enc(&po, &scro);
    ench = create_enc(&ph, &scrh);
    ro = fixed_qp_run();
    rh = fixed_qp_run();

    CASE();
    for (f = 0; f < 8; f++)
    {
        nal_view_t ao[64], ah[64], vo[32], vh[32];
        int na, nh2, vo_n, vh_n, j, non_vcl_ok = 1;
        gen_i420(src, f);
        memcpy(fo, src, TFSIZE);
        memcpy(fh, src, TFSIZE);

        collector_reset(&g_cb);
        CHECK(encode_frame(enco, scro, &ro, fo, H264E_FRAME_TYPE_DEFAULT, &oo, &no) == 0);
        CHECK(encode_frame(ench, scrh, &rh, fh, H264E_FRAME_TYPE_DEFAULT, &oh, &nh) == 0);

        /* Reconstructed pixels must be identical */
        CHECK(memcmp(fo, fh, TFSIZE) == 0);

        na = split_annexb(oo, no, ao, 64);
        nh2 = split_annexb(oh, nh, ah, 64);
        vo_n = vcl_only(ao, na, vo, 32);
        vh_n = vcl_only(ah, nh2, vh, 32);
        CHECK(vo_n == vh_n && vo_n > 0);
        for (j = 0; j < vo_n; j++)
        {
            CHECK(vo[j].len == vh[j].len);
            CHECK(memcmp(vo[j].data, vh[j].data, vo[j].len) == 0);
        }
        /* HRD stream may only add SPS/PPS/SEI NAL units */
        for (j = 0; j < nh2; j++)
        {
            unsigned t = nal_type(&ah[j]);
            if (t != NAL_SPS && t != NAL_PPS && t != NAL_SEI && t != NAL_IDR && t != NAL_P)
                non_vcl_ok = 0;
        }
        CHECK(non_vcl_ok == 1);
        if (f == 0)
        {
            sps_info_t s;
            int k;
            parse_sps(&ao[0], &s);
            CHECK(s.vui_present == 0);          /* legacy SPS carries no VUI */
            for (k = 0; k < na; k++)
                CHECK(nal_type(&ao[k]) != NAL_SEI);
        }
    }
    free(enco); free(scro);
    free(ench); free(scrh);

    /* Rate-control mode: desired_frame_bytes stays owned by the caller and
       the quantizer path remains untouched by HRD signaling */
    CASE();
    {
        H264E_persist_t *e2o, *e2h;
        H264E_scratch_t *s2o, *s2h;
        H264E_run_param_t r2o, r2h;
        unsigned char *fo2 = malloc(TFSIZE), *fh2 = malloc(TFSIZE);
        unsigned char *o2o, *o2h;
        int n2o, n2h;
        nal_view_t ao[32], ah[32], vo[16], vh[16];

        po = base_param(); po.const_input_flag = 0;
        ph = base_param(); ph.const_input_flag = 0;
        ph.hrd_bitrate_bps = 1000000;
        ph.hrd_num_units_in_tick = 1001;
        ph.hrd_time_scale = 48000;
        e2o = create_enc(&po, &s2o);
        e2h = create_enc(&ph, &s2h);
        memset(&r2o, 0, sizeof(r2o));
        memset(&r2h, 0, sizeof(r2h));
        r2o.qp_min = r2h.qp_min = 10;
        r2o.qp_max = r2h.qp_max = 50;
        r2o.desired_frame_bytes = r2h.desired_frame_bytes = 777;
        r2h.nalu_callback = collect_nalu;
        r2h.nalu_callback_token = &g_cb;

        gen_i420(src, 1);
        memcpy(fo2, src, TFSIZE);
        memcpy(fh2, src, TFSIZE);
        collector_reset(&g_cb);
        CHECK(encode_frame(e2o, s2o, &r2o, fo2, H264E_FRAME_TYPE_DEFAULT, &o2o, &n2o) == 0);
        CHECK(encode_frame(e2h, s2h, &r2h, fh2, H264E_FRAME_TYPE_DEFAULT, &o2h, &n2h) == 0);
        CHECK(r2h.desired_frame_bytes == 777);
        CHECK(r2o.desired_frame_bytes == 777);
        CHECK(memcmp(fo2, fh2, TFSIZE) == 0);
        {
            int ka = split_annexb(o2o, n2o, ao, 32);
            int kb = split_annexb(o2h, n2h, ah, 32);
            int vo_n = vcl_only(ao, ka, vo, 16);
            int vh_n = vcl_only(ah, kb, vh, 16);
            int j;
            CHECK(vo_n == vh_n && vo_n == 1);
            for (j = 0; j < vo_n; j++)
            {
                CHECK(vo[j].len == vh[j].len);
                CHECK(memcmp(vo[j].data, vh[j].data, vo[j].len) == 0);
            }
        }
        free(e2o); free(s2o); free(fo2);
        free(e2h); free(s2h); free(fh2);
    }
    free(fo); free(fh); free(src);
}
