/*
 * Progressive intra refresh test suite.
 *
 * Three groups, all driven by refresh_support/driver.c:
 *   recovery   - two real h264bsd decoders, one with a corrupted short-term
 *                reference, verify the trusted prefix grows row by row and the
 *                whole picture converges without extra IDRs.
 *   prediction - moving texture, band/intra modes, fractional motion sampling
 *                support, deblocking and soft-slice compatibility.
 *   compat     - rejected configurations, failed-call retry semantics, NALU
 *                callback ordering and legacy behaviour with refresh disabled.
 *
 * All source material is generated procedurally; nothing is downloaded and no
 * error concealment is triggered in the decoder.
 */
#include "cases.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minih264e.h"
#include "h264bsd_decoder.h"

#define MAX_W       160
#define MAX_H       128
#define MAX_FSIZE   (MAX_W*MAX_H*3/2)
#define MAX_NMB     (MAX_W/16 * MAX_H/16)
#define MAX_FRAMES  24
#define MAX_NALS    64

/* ------------------------------------------------------------------ */
/* Small harness                                                      */
/* ------------------------------------------------------------------ */

typedef struct
{
    int w, h, fsize, nmbx, nmby;
    H264E_create_param_t cp;
    H264E_run_param_t rp;
    H264E_io_yuv_t io;
    void *enc, *scratch;
    storage_t clean, corrupt;
    unsigned char input[MAX_FSIZE];
    unsigned char pic_clean[MAX_FSIZE];
    unsigned char pic_corrupt[MAX_FSIZE];
    unsigned slice_start[MAX_NALS];
    unsigned deblock_idc[MAX_NALS];
    unsigned n_slices;
    int last_errors;
} refresh_ctx_t;

static void gen_texture(unsigned char *img, int w, int h, int frame, int variant)
{
    int x, y;
    unsigned char *yp = img;
    for (y = 0; y < h; y++)
    {
        for (x = 0; x < w; x++)
        {
            double v;
            if (variant == 0)
                v = 128 + 50*sin((x + 0.6*frame)*0.35) + 35*sin((y - 0.3*frame)*0.21)
                    + 25*sin((x + y + 0.9*frame)*0.13);
            else if (variant == 1)
                v = 128 + 60*sin((x - 0.4*frame)*0.19) + 45*sin((y + 0.7*frame)*0.27);
            else
                v = 128 + 40*sin((x + y)*0.07 + frame*0.4) + 30*sin(x*0.5)
                    + 20*cos(y*0.4 - frame*0.2);
            yp[y*w + x] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
    {
        unsigned char *u = img + w*h;
        unsigned char *v2 = img + w*h*5/4;
        for (y = 0; y < h/2; y++)
        {
            for (x = 0; x < w/2; x++)
            {
                u[y*(w/2) + x]  = (unsigned char)(128 + 35*sin((x + 0.6*frame)*0.25 + 1));
                v2[y*(w/2) + x] = (unsigned char)(128 + 35*cos((y - 0.3*frame)*0.17 + 2));
            }
        }
    }
}

static void h264_decode(storage_t *d, const unsigned char *bytes, int n, int pic_id,
    unsigned char *dest, refresh_ctx_t *capture)
{
    unsigned off = 0, used = 0, got = 0, stalls = 0;
    while (off < (unsigned)n)
    {
        unsigned st = h264bsdDecode(d, (u8*)bytes + off, n - off, pic_id, &used);
        TASK_CHECK(st <= H264BSD_HDRS_RDY);
        off += used;
        TASK_CHECK(used || ++stalls < 8);
        /* SPS/PPS NALs return HDRS_RDY; coded slices return RDY or PIC_RDY */
        if (capture && capture->n_slices < MAX_NALS && used > 0 &&
            (st == H264BSD_RDY || st == H264BSD_PIC_RDY))
        {
            unsigned k = capture->n_slices++;
            capture->slice_start[k] = d->sliceHeader[0].firstMbInSlice;
            capture->deblock_idc[k] = d->sliceHeader[0].disableDeblockingFilterIdc;
        }
        if (st == H264BSD_PIC_RDY)
        {
            unsigned id, key, errors;
            unsigned char *p = h264bsdNextOutputPicture(d, &id, &key, &errors);
            TASK_CHECK(p && id == (unsigned)pic_id);
            TASK_CHECK(errors == 0);          /* no error concealment */
            memcpy(dest, p, capture ? (unsigned)capture->fsize : (unsigned)MAX_FSIZE);
            got++;
        }
    }
    TASK_CHECK(got == 1);
}

static void corrupt_reference(storage_t *d, int fsize)
{
    unsigned k;
    int refs = 0;
    for (k = 0; k <= d->dpb->dpbSize; k++)
    {
        dpbPicture_t *pic = d->dpb->buffer + k;
        if (pic->status == SHORT_TERM || pic->status == LONG_TERM)
        {
            int i;
            for (i = 0; i < fsize; i++)
            {
                pic->data[i] ^= (u8)(0x7F + (i & 31));
            }
            refs++;
        }
    }
    TASK_CHECK(refs == 1);    /* headers/DPB identity preserved, exactly one ref */
}

static void ctx_init(refresh_ctx_t *c, int w, int h, int refresh_rows)
{
    int a, b;
    memset(c, 0, sizeof(*c));
    c->w = w; c->h = h; c->fsize = w*h*3/2;
    c->nmbx = w/16; c->nmby = h/16;
    c->cp.width = w; c->cp.height = h; c->cp.num_layers = 1;
    c->cp.enableNEON = 1; c->cp.intra_refresh_rows = refresh_rows;
    TASK_CHECK(H264E_sizeof(&c->cp, &a, &b) == H264E_STATUS_SUCCESS);
    c->enc = malloc(a); c->scratch = malloc(b);
    TASK_CHECK(c->enc && c->scratch);
    TASK_CHECK(H264E_init(c->enc, &c->cp) == H264E_STATUS_SUCCESS);
    c->io.yuv[0] = c->input;
    c->io.yuv[1] = c->input + w*h;
    c->io.yuv[2] = c->input + w*h*5/4;
    c->io.stride[0] = w; c->io.stride[1] = c->io.stride[2] = w/2;
    c->rp.qp_min = c->rp.qp_max = 30;
    c->rp.encode_speed = 5;
    TASK_CHECK(h264bsdInit(&c->clean, 1) == H264BSD_RDY);
    TASK_CHECK(h264bsdInit(&c->corrupt, 1) == H264BSD_RDY);
}

static void ctx_free(refresh_ctx_t *c)
{
    h264bsdShutdown(&c->clean);
    h264bsdShutdown(&c->corrupt);
    free(c->enc);
    free(c->scratch);
}

/* One encoded+decoded frame. Returns the encoded size. */
static int ctx_frame(refresh_ctx_t *c, int frame_idx, int frame_type, int variant)
{
    unsigned char *out;
    int n, st;
    unsigned char *copy;
    gen_texture(c->input, c->w, c->h, frame_idx, variant);
    c->rp.frame_type = frame_type;
    c->n_slices = 0;
    st = H264E_encode(c->enc, c->scratch, &c->rp, &c->io, &out, &n);
    TASK_CHECK(st == H264E_STATUS_SUCCESS && n > 0);
    copy = malloc(n);
    TASK_CHECK(copy != NULL);
    memcpy(copy, out, n);
    h264_decode(&c->clean, copy, n, frame_idx, c->pic_clean, c);
    memcpy(copy, out, n);
    h264_decode(&c->corrupt, copy, n, frame_idx, c->pic_corrupt, NULL);
    free(copy);
    /* clean decoder reconstructs exactly the encoder local picture */
    TASK_CHECK(memcmp(c->input, c->pic_clean, c->fsize) == 0);
    return n;
}

static void check_trusted_prefix(refresh_ctx_t *c, int trusted_rows)
{
    int y;
    for (y = 0; y < 16*trusted_rows; y++)
    {
        TASK_CHECK(memcmp(c->pic_clean + y*c->w, c->pic_corrupt + y*c->w, c->w) == 0);
    }
    for (y = 0; y < 8*trusted_rows; y++)
    {
        TASK_CHECK(memcmp(c->pic_clean + c->w*c->h + y*c->w/2,
                          c->pic_corrupt + c->w*c->h + y*c->w/2, c->w/2) == 0);
        TASK_CHECK(memcmp(c->pic_clean + c->w*c->h*5/4 + y*c->w/2,
                          c->pic_corrupt + c->w*c->h*5/4 + y*c->w/2, c->w/2) == 0);
    }
}

static int mb_is_intra(const storage_t *d, int mb)
{
    return d->mb[mb].mbType > 5;
}

/* ------------------------------------------------------------------ */
/* recovery group                                                     */
/* ------------------------------------------------------------------ */

/* Trusted prefix after every P frame; full convergence at each cycle end,
 * over two complete cycles. 64x96 -> 6 MB rows, r=2 -> 3 P frames/cycle. */
static void recovery_prefix(void)
{
    refresh_ctx_t c;
    int r = 2, f, pf = 0;
    ctx_init(&c, 64, 96, r);
    ctx_frame(&c, 0, H264E_FRAME_TYPE_KEY, 0);
    TASK_CHECK(memcmp(c.pic_clean, c.pic_corrupt, c.fsize) == 0);
    corrupt_reference(&c.corrupt, c.fsize);
    for (f = 1; f <= 2*c.nmby/r; f++)
    {
        int rows, cycle_pos;
        ctx_frame(&c, f, H264E_FRAME_TYPE_DEFAULT, 0);
        pf++;
        cycle_pos = ((pf - 1) % (c.nmby/r)) + 1;
        rows = cycle_pos*r;
        if (rows > c.nmby) rows = c.nmby;
        check_trusted_prefix(&c, rows);
        if (cycle_pos == c.nmby/r)
        {
            /* end of cycle: whole picture recovered without a new IDR */
            TASK_CHECK(memcmp(c.pic_clean, c.pic_corrupt, c.fsize) == 0);
            /* re-damage to start the second cycle from a corrupted ref */
            corrupt_reference(&c.corrupt, c.fsize);
        }
    }
    ctx_free(&c);
}

/* A short final band when nmby is not divisible by r (80 high -> 5 rows, r=2
 * gives bands 2,2,1). The last band refreshes only one row. */
static void recovery_short_band(void)
{
    refresh_ctx_t c;
    int f;
    ctx_init(&c, 96, 80, 2);           /* nmby = 5 */
    ctx_frame(&c, 0, H264E_FRAME_TYPE_KEY, 1);
    corrupt_reference(&c.corrupt, c.fsize);
    static const int expected_rows[] = { 2, 4, 5 };
    for (f = 1; f <= 3; f++)
    {
        ctx_frame(&c, f, H264E_FRAME_TYPE_P, 1);
        check_trusted_prefix(&c, expected_rows[f - 1]);
    }
    TASK_CHECK(memcmp(c.pic_clean, c.pic_corrupt, c.fsize) == 0);
    ctx_free(&c);
}

/* Two full cycles plus subsequent ordinary P frames keep matching. */
static void recovery_two_cycles(void)
{
    refresh_ctx_t c;
    int r = 1, f, since = 0;
    ctx_init(&c, 64, 96, r);            /* 6 P frames per cycle */
    ctx_frame(&c, 0, H264E_FRAME_TYPE_KEY, 2);
    corrupt_reference(&c.corrupt, c.fsize);
    for (f = 1; f < 16; f++)
    {
        int rows;
        ctx_frame(&c, f, H264E_FRAME_TYPE_DEFAULT, 2);
        since++;
        rows = since*r;
        if (rows > c.nmby) rows = c.nmby;
        check_trusted_prefix(&c, rows);
        if (since >= c.nmby)
        {
            TASK_CHECK(memcmp(c.pic_clean, c.pic_corrupt, c.fsize) == 0);
        }
        if (since == c.nmby)
        {
            /* begin the second cycle with a freshly corrupted reference */
            corrupt_reference(&c.corrupt, c.fsize);
            since = 0;
        }
    }
    ctx_free(&c);
}

/* An explicit KEY produces a normal IDR and restarts the band at the top. */
static void recovery_key_restart(void)
{
    refresh_ctx_t c;
    int r = 2, f, since = 0;
    ctx_init(&c, 64, 96, r);
    ctx_frame(&c, 0, H264E_FRAME_TYPE_KEY, 0);
    corrupt_reference(&c.corrupt, c.fsize);
    /* partial cycle: refresh 4 of 6 rows */
    for (f = 1; f <= 2; f++)
    {
        ctx_frame(&c, f, H264E_FRAME_TYPE_P, 0);
        since++;
        check_trusted_prefix(&c, since*r);
    }
    /* explicit IDR: independent of the corrupted reference */
    ctx_frame(&c, 3, H264E_FRAME_TYPE_KEY, 0);
    TASK_CHECK(memcmp(c.pic_clean, c.pic_corrupt, c.fsize) == 0);
    corrupt_reference(&c.corrupt, c.fsize);
    /* cycle restarts at the top: only r rows trusted after the next P */
    ctx_frame(&c, 4, H264E_FRAME_TYPE_DEFAULT, 0);
    check_trusted_prefix(&c, r);
    ctx_frame(&c, 5, H264E_FRAME_TYPE_DEFAULT, 0);
    check_trusted_prefix(&c, 2*r);
    ctx_free(&c);
}

/* ------------------------------------------------------------------ */
/* prediction group                                                   */
/* ------------------------------------------------------------------ */

/* Moving texture must produce safe non-zero fractional inter motion inside
 * the trusted region, while every band MB is forced to intra. */
static void prediction_fractional_motion(void)
{
    refresh_ctx_t c;
    int r = 2, f, saw_fractional = 0, saw_inter = 0;
    ctx_init(&c, 96, 80, r);
    c.rp.encode_speed = 5;
    ctx_frame(&c, 0, H264E_FRAME_TYPE_KEY, 0);
    corrupt_reference(&c.corrupt, c.fsize);
    for (f = 1; f <= c.nmby/r + 1; f++)
    {
        int cycle_pos = ((f - 1) % (c.nmby/r)) + 1;
        int trusted = cycle_pos*r;
        int mb;
        if (trusted > c.nmby) trusted = c.nmby;
        ctx_frame(&c, f, H264E_FRAME_TYPE_DEFAULT, 0);
        check_trusted_prefix(&c, trusted);
        for (mb = 0; mb < c.nmbx*c.nmby; mb++)
        {
            int mby = mb / c.nmbx;
            if (mby >= trusted) continue;
            if (!mb_is_intra(&c.clean, mb))
            {
                int b, nsub;
                saw_inter = 1;
                /* decoder MV is stored in quarter-pel luma units */
                if ((c.clean.mb[mb].mv[0].hor & 3) || (c.clean.mb[mb].mv[0].ver & 3))
                {
                    saw_fractional = 1;
                }
                /* only the sub-blocks this MB type actually codes carry valid
                 * MVs: 16x16 -> 1, 16x8/8x16 -> 2, 8x8 -> 4 */
                switch (c.clean.mb[mb].mbType)
                {
                    case 1: nsub = 1; break;   /* P_L0_16x16 */
                    case 2: nsub = 2; break;   /* P_L0_L0_16x8 */
                    case 3: nsub = 2; break;   /* P_L0_L0_8x16 */
                    default: nsub = 4; break;  /* 8x8 family */
                }
                /* Independently verify the full interpolation support of each
                 * coded inter partition stays within the trusted prefix. Luma
                 * uses a 6-tap filter on fractional axes, chroma a bilinear
                 * cell. */
                for (b = 0; b < nsub; b++)
                {
                    int bw = (c.clean.mb[mb].mbType == 3) ? 8 : 16;
                    int bh = (c.clean.mb[mb].mbType == 2) ? 8 : 16;
                    int ox = (b & 1)*bw, oy = (b >> 1)*bh;
                    int mvy = c.clean.mb[mb].mv[b].ver;
                    int mvx = c.clean.mb[mb].mv[b].hor;
                    int lx0 = (mvx >> 2) + ox;
                    int ly0 = (mvy >> 2) + oy;
                    int lx1 = lx0 + bw - 1, ly1 = ly0 + bh - 1;
                    int fx = mvx & 3, fy = mvy & 3;
                    if (fx) { lx0 -= 2; lx1 += 2; }
                    if (fy) { ly0 -= 2; ly1 += 2; if (fx && fy == 2) ly1 += 1; }
                    /* Frame left/top borders are extended (replicated) samples,
                     * so negative coords are legal. The refresh invariant only
                     * forbids reading BELOW the trusted prefix. */
                    (void)lx0; (void)ly0;
                    TASK_CHECK(lx1 < c.w && ly1 < trusted*16);
                    {
                        int cx0 = (mvx >> 3) + ox/2;
                        int cy0 = (mvy >> 3) + oy/2;
                        int cx1 = cx0 + bw/2 - 1, cy1 = cy0 + bh/2 - 1;
                        if (mvx & 7) cx1 += 1;
                        if (mvy & 7) cy1 += 1;
                        (void)cy0;
                        TASK_CHECK(cx1 < c.w/2 && cy1 < trusted*8);
                    }
                }
            }
        }
    }
    TASK_CHECK(saw_inter);
    TASK_CHECK(saw_fractional);   /* fractional search is preserved, not banned */
    ctx_free(&c);
}

/* Band MBs are all intra, the trusted prefix is not uniformly intra, and the
 * bitstream carries a slice boundary plus idc==2 deblock control on the band
 * edge, matching the encoder's local reconstruction. */
static void prediction_modes_and_border(void)
{
    refresh_ctx_t c;
    int r = 2, f, saw_trusted_inter = 0;
    ctx_init(&c, 64, 96, r);
    ctx_frame(&c, 0, H264E_FRAME_TYPE_KEY, 1);
    for (f = 1; f <= c.nmby/r; f++)
    {
        int band_top = (f - 1)*r;
        int band_bottom = band_top + r;
        int mb, band_intra = 0, trusted_inter = 0;
        ctx_frame(&c, f, H264E_FRAME_TYPE_DEFAULT, 1);
        for (mb = 0; mb < c.nmbx*c.nmby; mb++)
        {
            int mby = mb / c.nmbx;
            if (mby >= band_top && mby < band_bottom)
            {
                TASK_CHECK(mb_is_intra(&c.clean, mb));   /* no skip/inter in band */
                band_intra++;
            } else if (mby < band_top)
            {
                if (!mb_is_intra(&c.clean, mb)) trusted_inter++;
            }
        }
        TASK_CHECK(band_intra == r*c.nmbx);
        saw_trusted_inter |= (trusted_inter > 0);
        /* a slice starts exactly at the band's first MB, unless the band
         * reaches the bottom of the frame (no following region to isolate) */
        {
            unsigned k, found = 0;
            for (k = 0; k < c.n_slices; k++)
            {
                TASK_CHECK(c.deblock_idc[k] == 2);      /* borders unfiltered, edges inside kept */
                if (c.slice_start[k] == (unsigned)(band_bottom*c.nmbx)) found = 1;
            }
            if (band_bottom < c.nmby) TASK_CHECK(found);
        }
    }
    /* once at least one earlier band exists, the trusted prefix still uses
     * normal inter prediction rather than being forced all-intra */
    TASK_CHECK(saw_trusted_inter);
    ctx_free(&c);
}

/* desired_nalu_bytes soft slicing stays decodable and exact; in refresh mode
 * splits are row-aligned and every slice signals idc==2. */
static void prediction_soft_slice(void)
{
    refresh_ctx_t c;
    int r = 2, f;
    ctx_init(&c, 96, 80, r);
    c.rp.desired_nalu_bytes = 140;
    ctx_frame(&c, 0, H264E_FRAME_TYPE_KEY, 2);
    corrupt_reference(&c.corrupt, c.fsize);
    for (f = 1; f <= 4; f++)
    {
        int trusted = ((f - 1) % (c.nmby/r) + 1)*r;
        unsigned k;
        if (trusted > c.nmby) trusted = c.nmby;
        ctx_frame(&c, f, H264E_FRAME_TYPE_DEFAULT, 2);
        check_trusted_prefix(&c, trusted);
        TASK_CHECK(c.n_slices >= 2);                    /* soft slicing happened */
        /* slice starts are ordered, row-aligned, cover every MB exactly once */
        TASK_CHECK(c.slice_start[0] == 0);
        for (k = 0; k < c.n_slices; k++)
        {
            TASK_CHECK(c.deblock_idc[k] == 2);
            TASK_CHECK(c.slice_start[k] % (unsigned)c.nmbx == 0); /* row aligned */
            if (k + 1 < c.n_slices)
            {
                TASK_CHECK(c.slice_start[k+1] > c.slice_start[k]);
            }
        }
        TASK_CHECK(c.slice_start[c.n_slices - 1] < (unsigned)(c.nmbx*c.nmby));
        {
            /* no gaps: each consecutive pair starts a whole number of rows,
             * and the last slice runs to the final macroblock row */
            unsigned last = c.slice_start[c.n_slices - 1];
            TASK_CHECK((c.nmbx*c.nmby - last) % (unsigned)c.nmbx == 0);
        }
        if (trusted == c.nmby)
        {
            TASK_CHECK(memcmp(c.pic_clean, c.pic_corrupt, c.fsize) == 0);
        }
    }
    ctx_free(&c);
}

/* ------------------------------------------------------------------ */
/* compat group                                                       */
/* ------------------------------------------------------------------ */

static void compat_invalid_configs(void)
{
    struct
    {
        int w, h, gop, ltr, vbv, den, thr, layers, rows;
    } cases[] =
    {
        { 64, 96,  0, 0,    0, 0, 0, 1, -1 },  /* negative rows */
        { 64, 96,  0, 0,    0, 0, 0, 1,  7 },  /* rows > MB rows (6) */
        { 64, 96, 20, 0,    0, 0, 0, 1,  2 },  /* finite GOP */
        { 64, 96,  0, 1,    0, 0, 0, 1,  2 },  /* long-term references */
        { 64, 96,  0, 0, 1000, 0, 0, 1,  2 },  /* VBV */
        { 64, 96,  0, 0,    0, 1, 0, 1,  2 },  /* temporal denoise */
        { 64, 96,  0, 0,    0, 0, 2, 1,  2 },  /* multi-thread */
        { 64, 96,  0, 0,    0, 0, 0, 2,  2 },  /* SVC layer */
        { 66, 96,  0, 0,    0, 0, 0, 1,  2 },  /* width not 16-aligned */
        { 64, 98,  0, 0,    0, 0, 0, 1,  2 },  /* height not 16-aligned */
        {  0,  0,  0, 0,    0, 0, 0, 1,  2 },  /* non-positive size */
    };
    unsigned i;
    for (i = 0; i < sizeof(cases)/sizeof(cases[0]); i++)
    {
        H264E_create_param_t p;
        int a, b;
        memset(&p, 0, sizeof(p));
        p.width = cases[i].w; p.height = cases[i].h;
        p.gop = cases[i].gop;
        p.max_long_term_reference_frames = cases[i].ltr;
        p.vbv_size_bytes = cases[i].vbv;
        p.temporal_denoise_flag = cases[i].den;
        p.max_threads = cases[i].thr;
        p.num_layers = cases[i].layers;
        p.intra_refresh_rows = cases[i].rows;
        TASK_CHECK(H264E_sizeof(&p, &a, &b) == H264E_STATUS_BAD_PARAMETER);
        /* init must reject the same configuration even without sizeof;
           the call fails before touching the encoder storage */
        {
            static unsigned char guard[512*1024];
            TASK_CHECK(H264E_init((H264E_persist_t*)guard, &p) == H264E_STATUS_BAD_PARAMETER);
        }
    }
    /* rows == 0 keeps every legacy combination working */
    {
        H264E_create_param_t p;
        int a, b;
        memset(&p, 0, sizeof(p));
        p.width = 64; p.height = 96; p.num_layers = 1; p.gop = 20;
        p.max_long_term_reference_frames = 2; p.vbv_size_bytes = 500;
        p.max_threads = 2; p.intra_refresh_rows = 0;
        TASK_CHECK(H264E_sizeof(&p, &a, &b) == H264E_STATUS_SUCCESS);
    }
}

/* A rejected frame call must not move the refresh position: retrying yields
 * the identical bitstream of an encoder that never saw the bad call. */
static void compat_retry(void)
{
    refresh_ctx_t good, retrying;
    static unsigned char good_bs[MAX_FRAMES][MAX_FSIZE];
    static int good_sz[MAX_FRAMES];
    int f;
    ctx_init(&good, 64, 96, 2);
    ctx_init(&retrying, 64, 96, 2);
    /* build the reference clean sequence and capture its encoded bytes */
    for (f = 0; f < 6; f++)
    {
        unsigned char *out;
        int n;
        gen_texture(good.input, 64, 96, f, 0);
        good.rp.frame_type = f ? H264E_FRAME_TYPE_DEFAULT : H264E_FRAME_TYPE_KEY;
        TASK_CHECK(H264E_encode(good.enc, good.scratch, &good.rp, &good.io, &out, &n) == 0);
        TASK_CHECK(n <= MAX_FSIZE);
        memcpy(good_bs[f], out, n);
        good_sz[f] = n;
    }
    /* retrying encoder: inject failures that must be fully rolled back */
    for (f = 0; f < 6; f++)
    {
        unsigned char *out;
        int n, st;
        gen_texture(retrying.input, 64, 96, f, 0);
        if (f == 2 || f == 4)
        {
            retrying.rp.frame_type = H264E_FRAME_TYPE_GOLDEN;   /* forbidden */
            st = H264E_encode(retrying.enc, retrying.scratch, &retrying.rp, &retrying.io, &out, &n);
            TASK_CHECK(st == H264E_STATUS_BAD_PARAMETER);
        }
        retrying.rp.frame_type = f ? H264E_FRAME_TYPE_DEFAULT : H264E_FRAME_TYPE_KEY;
        TASK_CHECK(H264E_encode(retrying.enc, retrying.scratch, &retrying.rp, &retrying.io, &out, &n) == 0);
        TASK_CHECK(n == good_sz[f]);
        TASK_CHECK(memcmp(out, good_bs[f], n) == 0);   /* identical to the clean sequence */
    }
    ctx_free(&good);
    ctx_free(&retrying);
}

/* NALU callback ordering and content versus the returned output buffer. */
static unsigned char cb_buf[MAX_FSIZE*2];
static int cb_len;
static int cb_count;
static void nalu_cb(const unsigned char *nal, int sizeof_nalu, void *token)
{
    (void)token;
    /* callback delivers NAL units without the 4-byte start code */
    memcpy(cb_buf + cb_len, nal, sizeof_nalu);
    cb_len += sizeof_nalu;
    cb_count++;
}

static void compat_callback_order(void)
{
    refresh_ctx_t c;
    int f;
    ctx_init(&c, 96, 80, 2);
    c.rp.nalu_callback = nalu_cb;
    c.rp.nalu_callback_token = NULL;
    for (f = 0; f < 5; f++)
    {
        unsigned char *out;
        int n, k, nal_count = 0, nal_bytes = 0, p = 0;
        cb_len = 0; cb_count = 0;
        gen_texture(c.input, 96, 80, f, 1);
        c.rp.frame_type = f ? H264E_FRAME_TYPE_DEFAULT : H264E_FRAME_TYPE_KEY;
        TASK_CHECK(H264E_encode(c.enc, c.scratch, &c.rp, &c.io, &out, &n) == 0);
        /* count start codes and the bytes that follow them */
        for (k = 0; k + 4 <= n; k++)
        {
            if (out[k] == 0 && out[k+1] == 0 && out[k+2] == 0 && out[k+3] == 1)
            {
                int next = n, j;
                for (j = k + 4; j + 4 <= n; j++)
                {
                    if (out[j] == 0 && out[j+1] == 0 && out[j+2] == 0 && out[j+3] == 1)
                    {
                        next = j;
                        break;
                    }
                }
                nal_count++;
                nal_bytes += next - (k + 4);
                /* callback NAL payload matches this slice of the output */
                TASK_CHECK(memcmp(cb_buf + p, out + k + 4, next - (k + 4)) == 0);
                p += next - (k + 4);
                k = next - 1;
            }
        }
        TASK_CHECK(cb_count == nal_count);
        TASK_CHECK(cb_len == nal_bytes);
        TASK_CHECK(cb_len == n - 4*nal_count);
    }
    ctx_free(&c);
}

/* With the field absent/zero the encoder takes the legacy code path: its
 * output decodes cleanly and keeps the standard deblock idc (0), not the
 * refresh-only idc 2. */
static void compat_legacy_disabled(void)
{
    refresh_ctx_t c;
    int f;
    ctx_init(&c, 64, 96, 0);
    for (f = 0; f < 8; f++)
    {
        ctx_frame(&c, f, H264E_FRAME_TYPE_DEFAULT, 2);
        /* legacy path keeps the standard deblock idc (0), never the
         * refresh-only idc 2, on every slice it emits */
        unsigned k;
        for (k = 0; k < c.n_slices; k++)
        {
            TASK_CHECK(c.deblock_idc[k] == 0);
        }
    }
    ctx_free(&c);
}

/* ------------------------------------------------------------------ */
/* Exported case table                                                */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] =
{
    { "recovery",   "prefix_growth",       recovery_prefix },
    { "recovery",   "short_final_band",    recovery_short_band },
    { "recovery",   "two_full_cycles",     recovery_two_cycles },
    { "recovery",   "key_restarts_cycle",  recovery_key_restart },

    { "prediction", "safe_fractional_mv",  prediction_fractional_motion },
    { "prediction", "band_intra_and_border", prediction_modes_and_border },
    { "prediction", "soft_slice_exact",    prediction_soft_slice },

    { "compat",     "invalid_configs",     compat_invalid_configs },
    { "compat",     "failed_call_retry",   compat_retry },
    { "compat",     "nalu_callback_order", compat_callback_order },
    { "compat",     "legacy_when_disabled", compat_legacy_disabled },
};

const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
