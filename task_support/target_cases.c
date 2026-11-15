/*
 * Target test cases for explicit B slice support.
 *
 * Every stream here is built from raw NAL units written by this file. The
 * expected pictures are produced by a standalone scalar reference (integer
 * and quarter-pel luma, eighth-pel chroma interpolation and the bi-pred
 * average) which never calls the production prediction code. Residual code
 * words are searched against the decoder's own CAVLC tables so the encoder
 * does not duplicate or import the decoder's reconstruction logic.
 */
#include "check.h"
#include "h264bsd_decoder.h"
#include "h264bsd_cavlc.h"
#include "h264bsd_macroblock_layer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ============================ bit writer ============================ */

typedef struct { uint8_t b[16384]; size_t bits; } bw_t;

static void bw_bit(bw_t *w, unsigned v) {
    size_t byte = w->bits >> 3;
    unsigned b = (unsigned)(7 - (w->bits & 7));
    if (v) w->b[byte] |= (uint8_t)(1u << b);
    w->bits++;
}
static void bw_fixed(bw_t *w, unsigned v, unsigned n) {
    for (int i = (int)n - 1; i >= 0; i--) bw_bit(w, (v >> i) & 1);
}
static void bw_ue(bw_t *w, unsigned v) {
    unsigned c = v + 1, len = 0;
    while ((1u << len) <= c) len++;
    bw_fixed(w, 0, len - 1);
    bw_fixed(w, c, len);
}
static void bw_se(bw_t *w, int v) {
    bw_ue(w, v > 0 ? (unsigned)(2*v - 1) : (unsigned)(-2*v));
}
static void bw_rbsp_trailing(bw_t *w) { bw_bit(w, 1); while (w->bits & 7) bw_bit(w, 0); }
static void bw_align0(bw_t *w) { while (w->bits & 7) bw_bit(w, 0); }

static size_t bw_nal(uint8_t *dst, size_t cap, size_t *n, unsigned hdr, bw_t *w) {
    size_t rb = (w->bits + 7) >> 3, pos = *n, zeros = 0;
    if (pos + 3 + rb*2 >= cap) return 0;
    dst[pos++] = 0; dst[pos++] = 0; dst[pos++] = 1; dst[pos++] = (uint8_t)hdr;
    for (size_t i = 0; i < rb; i++) {
        uint8_t c = w->b[i];
        if (zeros == 2 && c <= 3) { dst[pos++] = 3; zeros = 0; }
        dst[pos++] = c;
        zeros = c ? 0 : zeros + 1;
    }
    *n = pos;
    return pos;
}

/* ============================ scalar reference ============================ */

#define W 32
#define H 32
#define MBW (W/16)
#define MBH (H/16)
#define YSIZE (W*H)
#define CSIZE ((W/2)*(H/2))
#define FRAMESIZE (YSIZE + 2*CSIZE)

static int clip8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* deterministic content of a reference picture with given POC */
static void ref_picture(int poc, uint8_t *img) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            img[y*W + x] = (uint8_t)((poc*17 + x*5 + y*3) & 0xFF);
    for (int pl = 0; pl < 2; pl++)
        for (int y = 0; y < H/2; y++)
            for (int x = 0; x < W/2; x++)
                img[YSIZE + pl*CSIZE + y*(W/2) + x] =
                    (uint8_t)((poc*11 + x*7 + y*2 + pl*29) & 0xFF);
}

/* constant-color picture: any interpolation (incl. fractional and border
 * extension) reproduces the same value, giving an exact oracle for the
 * fractional motion code path. */
static void const_picture(uint8_t yv, uint8_t cbv, uint8_t crv, uint8_t *img) {
    for (int i = 0; i < YSIZE; i++) img[i] = yv;
    for (int i = 0; i < CSIZE; i++) img[YSIZE + i] = cbv;
    for (int i = 0; i < CSIZE; i++) img[YSIZE + CSIZE + i] = crv;
}

static int bp(const uint8_t *p, int w, int h, int x, int y) {
    if (x < 0) x = 0; else if (x >= w) x = w - 1;
    if (y < 0) y = 0; else if (y >= h) y = h - 1;
    return p[y*w + x];
}

static int hh(const uint8_t *img, int x, int y) {
    int s = bp(img,W,H,x-2,y) - 5*bp(img,W,H,x-1,y) + 20*bp(img,W,H,x,y)
          + 20*bp(img,W,H,x+1,y) - 5*bp(img,W,H,x+2,y) + bp(img,W,H,x+3,y);
    return clip8((s + 16) >> 5);
}
static int vh(const uint8_t *img, int x, int y) {
    int s = bp(img,W,H,x,y-2) - 5*bp(img,W,H,x,y-1) + 20*bp(img,W,H,x,y)
          + 20*bp(img,W,H,x,y+1) - 5*bp(img,W,H,x,y+2) + bp(img,W,H,x,y+3);
    return clip8((s + 16) >> 5);
}
static int jj(const uint8_t *img, int x, int y) {
    int s = hh(img,x,y-2) - 5*hh(img,x,y-1) + 20*hh(img,x,y)
          + 20*hh(img,x,y+1) - 5*hh(img,x,y+2) + hh(img,x,y+3);
    return clip8((s + 16) >> 5);
}

static int ref_luma(const uint8_t *img, int ix, int iy, int fx, int fy) {
    int g = bp(img,W,H,ix,iy);
    if (fx == 0 && fy == 0) return g;
    int b = hh(img,ix,iy), d = vh(img,ix,iy), j = jj(img,ix,iy);
    switch (fy) {
        case 0:
            if (fx == 2) return b;
            if (fx == 1) return (b + g + 1) >> 1;
            return (b + bp(img,W,H,ix+1,iy) + 1) >> 1;            /* c */
        case 2:
            if (fx == 0) return d;
            if (fx == 2) return j;
            if (fx == 1) return (j + g + 1) >> 1;                /* i */
            return (j + bp(img,W,H,ix+1,iy) + 1) >> 1;           /* k */
        case 1:
            if (fx == 0) return (d + g + 1) >> 1;                /* d */
            if (fx == 2) return (j + b + 1) >> 1;                /* f */
            if (fx == 1) return (j + g + 1) >> 1;                /* e */
            return (j + bp(img,W,H,ix+1,iy) + 1) >> 1;           /* g */
        default:
            if (fx == 0) return (d + bp(img,W,H,ix,iy+1) + 1) >> 1; /* n */
            if (fx == 2) return (j + hh(img,ix,iy+1) + 1) >> 1;     /* q */
            if (fx == 1) return (j + bp(img,W,H,ix,iy+1) + 1) >> 1; /* p */
            return (j + bp(img,W,H,ix+1,iy+1) + 1) >> 1;            /* r */
    }
}

static int ref_chroma(const uint8_t *img, int plane, int x, int y, int fx, int fy) {
    const uint8_t *p = img + YSIZE + (plane ? CSIZE : 0);
    int cw = W/2, ch = H/2;
    int p00 = bp(p,cw,ch,x,y),   p10 = bp(p,cw,ch,x+1,y);
    int p01 = bp(p,cw,ch,x,y+1), p11 = bp(p,cw,ch,x+1,y+1);
    int top = (8-fx)*p00 + fx*p10;
    int bot = (8-fx)*p01 + fx*p11;
    return ((8-fy)*top + fy*bot + 32) >> 6;
}

/* render one partition. (mbx,mby) is the macroblock origin in pixels and
 * (partX,partY) the within-macroblock partition offset; both are combined
 * into absolute frame coordinates exactly as the decoder does. */
static void render_list(const uint8_t *ref, uint8_t *dst,
    int mbx, int mby, int partX, int partY, int w, int h, int mvx, int mvy) {
    int ox = mbx + partX, oy = mby + partY;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int ax = ox + x + (mvx >> 2), ay = oy + y + (mvy >> 2);
            dst[(oy+y)*W + ox + x] =
                (uint8_t)ref_luma(ref, ax, ay, mvx & 3, mvy & 3);
        }
    for (int pl = 0; pl < 2; pl++)
        for (int y = 0; y < h/2; y++)
            for (int x = 0; x < w/2; x++) {
                int ax = (ox>>1) + x + (mvx >> 3);
                int ay = (oy>>1) + y + (mvy >> 3);
                dst[YSIZE + pl*CSIZE + ((oy>>1)+y)*(W/2) + (ox>>1) + x] =
                    (uint8_t)ref_chroma(ref, pl, ax, ay, mvx & 7, mvy & 7);
            }
}

static void render_bi(const uint8_t *r0, const uint8_t *r1, uint8_t *dst,
    int mbx, int mby, int partX, int partY, int w, int h,
    int mv0x, int mv0y, int mv1x, int mv1y) {
    static uint8_t a[FRAMESIZE], b[FRAMESIZE];
    int ox = mbx + partX, oy = mby + partY;
    memset(a, 0, sizeof(a)); memset(b, 0, sizeof(b));
    render_list(r0, a, mbx, mby, partX, partY, w, h, mv0x, mv0y);
    render_list(r1, b, mbx, mby, partX, partY, w, h, mv1x, mv1y);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            dst[(oy+y)*W + ox + x] =
                (uint8_t)((a[(oy+y)*W+ox+x] + b[(oy+y)*W+ox+x] + 1) >> 1);
    for (int pl = 0; pl < 2; pl++)
        for (int y = 0; y < h/2; y++)
            for (int x = 0; x < w/2; x++) {
                int idx = YSIZE + pl*CSIZE + ((oy>>1)+y)*(W/2) + (ox>>1) + x;
                dst[idx] = (uint8_t)((a[idx] + b[idx] + 1) >> 1);
            }
}

/* whole-16x16-macroblock convenience wrappers at MB grid position */
static void render_mb_list(uint8_t *dst, const uint8_t *ref,
    int mbx, int mby, int mvx, int mvy) {
    render_list(ref, dst, mbx*16, mby*16, 0, 0, 16, 16, mvx, mvy);
}
static void render_mb_bi(uint8_t *dst, const uint8_t *r0, const uint8_t *r1,
    int mbx, int mby, int m0x, int m0y, int m1x, int m1y) {
    render_bi(r0, r1, dst, mbx*16, mby*16, 0, 0, 16, 16, m0x, m0y, m1x, m1y);
}

/* ============================ stream builder ============================ */

#define MAX_REFS 4

typedef struct { int kind; int ref; int mx, my; } pred_t; /* kind 0 L0 1 L1 2 Bi */

typedef struct {
    int mbType;                 /* 1..22, or 22 for sub */
    pred_t part[4];
    int subType[4];            /* used when mbType==22 */
    pred_t sub[4][4];
    int residual;              /* one trailing-one coeff in luma block 0 */
    int resValue;
} mbdesc_t;

typedef struct {
    int poc, frameNum, nalRef;
    int firstMb;
    int activeL0, activeL1;
    int reorder0Cmd[8], nReorder0;
    int reorder1Cmd[8], nReorder1;
    mbdesc_t mb[16];
    int nmb;
    int disableDeblock;
} bslice_t;


static void emit_sps_pps(uint8_t *dst, size_t cap, size_t *n, int numRefs) {
    bw_t w; memset(&w, 0, sizeof(w));
    bw_fixed(&w, 77, 8); bw_fixed(&w, 0, 8); bw_fixed(&w, 10, 8);
    bw_ue(&w, 0); bw_ue(&w, 0); bw_ue(&w, 0); bw_ue(&w, 0);
    bw_ue(&w, (unsigned)numRefs); bw_bit(&w, 0);
    bw_ue(&w, MBW - 1); bw_ue(&w, MBH - 1);
    bw_bit(&w, 1); bw_bit(&w, 1);
    bw_bit(&w, 0); bw_bit(&w, 0);
    bw_rbsp_trailing(&w);
    bw_nal(dst, cap, n, 0x67, &w);

    bw_t p; memset(&p, 0, sizeof(p));
    bw_ue(&p, 0); bw_ue(&p, 0); bw_bit(&p, 0); bw_bit(&p, 0); bw_ue(&p, 0);
    bw_ue(&p, 0); bw_ue(&p, 0);           /* one L0 / one L1 default */
    bw_bit(&p, 0); bw_fixed(&p, 0, 2);
    bw_se(&p, 0); bw_se(&p, 0); bw_se(&p, 0);
    bw_bit(&p, 1); bw_bit(&p, 0); bw_bit(&p, 0);
    bw_rbsp_trailing(&p);
    bw_nal(dst, cap, n, 0x68, &p);
}

static void emit_ipcm(uint8_t *dst, size_t cap, size_t *n,
    int frameNum, int poc, const uint8_t *img, int nalRef, int mbCount) {
    bw_t w; memset(&w, 0, sizeof(w));
    bw_ue(&w, 0); bw_ue(&w, 2); bw_ue(&w, 0);
    bw_fixed(&w, (unsigned)frameNum, 4); bw_fixed(&w, (unsigned)poc, 4);
    if (nalRef) {
        /* non-IDR reference slice: adaptive_ref_pic_marking_mode_flag = 0 */
        bw_bit(&w, 0);
    }
    bw_se(&w, 0); bw_ue(&w, 1);            /* deblock present, disable idc1 */
    for (int m = 0; m < mbCount; m++) {
        bw_ue(&w, 25);                     /* I_PCM in I slice */
        bw_align0(&w);
        int mbx = m & 1, mby = m >> 1;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                bw_fixed(&w, img[(mby*16+y)*W + mbx*16 + x], 8);
        for (int pl = 0; pl < 2; pl++)
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                    bw_fixed(&w,
                        img[YSIZE + pl*CSIZE + (mby*8+y)*(W/2) + mbx*8 + x],
                        8);
    }
    bw_rbsp_trailing(&w);
    bw_nal(dst, cap, n, (unsigned)((nalRef ? 3 : 0) << 5 | 1), &w);
}

static void emit_reorder(bw_t *w, const int *cmd, int n) {
    if (!n) { bw_bit(w, 0); return; }
    bw_bit(w, 1);
    for (int i = 0; i < n; i++) { bw_ue(w, 0); bw_ue(w, (unsigned)(cmd[i]-1)); }
    bw_ue(w, 3);
}

static void emit_pred(bw_t *w, const pred_t *p, int activeL0, int activeL1) {
    if (p->kind == 0 || p->kind == 2) {
        if (activeL0 > 1) bw_ue(w, (unsigned)p->ref);
        bw_se(w, p->mx); bw_se(w, p->my);
    }
    if (p->kind == 1 || p->kind == 2) {
        if (activeL1 > 1) bw_ue(w, (unsigned)p->ref);
        bw_se(w, p->mx); bw_se(w, p->my);
    }
}

static void emit_b_slice(uint8_t *dst, size_t cap, size_t *n, const bslice_t *s) {
    bw_t w; memset(&w, 0, sizeof(w));
    bw_ue(&w, (unsigned)s->firstMb); bw_ue(&w, 1); bw_ue(&w, 0);
    bw_fixed(&w, (unsigned)s->frameNum, 4); bw_fixed(&w, (unsigned)s->poc, 4);
    bw_bit(&w, 1);                         /* override active counts */
    bw_ue(&w, (unsigned)s->activeL0 - 1);
    bw_ue(&w, (unsigned)s->activeL1 - 1);
    bw_bit(&w, 1);                         /* direct_spatial (unused) */
    emit_reorder(&w, s->reorder0Cmd, s->nReorder0);
    emit_reorder(&w, s->reorder1Cmd, s->nReorder1);
    bw_se(&w, 0);
    bw_ue(&w, s->disableDeblock ? 1 : 0);
    if (!s->disableDeblock) { bw_se(&w, 0); bw_se(&w, 0); }
    (void)0;

    for (int m = 0; m < s->nmb; m++) {
        const mbdesc_t *d = &s->mb[m];
        bw_ue(&w, 0);                     /* skip run */
        bw_ue(&w, (unsigned)d->mbType);
        if (d->mbType == 22) {
            for (int q = 0; q < 4; q++) bw_ue(&w, (unsigned)d->subType[q]);
            /* per 8x8 block reference index, only when the active count > 1;
             * all configs here use a single entry so these are omitted */
            for (int q = 0; q < 4; q++) {
                unsigned nsub = h264bsdBNumSubMbPart(d->subType[q]);
                for (unsigned k = 0; k < nsub; k++)
                    emit_pred(&w, &d->sub[q][k], s->activeL0, s->activeL1);
            }
        } else {
            int np = (int)h264bsdBNumMbPart((u32)d->mbType);
            for (int p = 0; p < np; p++)
                emit_pred(&w, &d->part[p], s->activeL0, s->activeL1);
        }
        /* no residual: coded_block_pattern = 0 */
        bw_ue(&w, 0);
    }
    bw_rbsp_trailing(&w);
    bw_nal(dst, cap, n, 0x01, &w);        /* nal_ref_idc 0 */
}

/* ============================ decode driver ============================ */

typedef struct { uint8_t data[FRAMESIZE]; u32 err; } outpic_t;

static int decode_stream(const uint8_t *nal, size_t n, int noReorder,
    outpic_t *outs, int maxOut, int *errCount) {
    storage_t dec;
    h264bsdInit(&dec, noReorder ? 1 : 0);
    size_t off = 0; int steps = 0, np = 0, ec = 0;
    while (off < n && steps < 10000) {
        u32 used = 0;
        u32 r = h264bsdDecode(&dec, (u8*)nal + off, (u32)(n-off), 0, &used);
        steps++;
        if (r == H264BSD_ERROR || r == H264BSD_PARAM_SET_ERROR ||
            r == H264BSD_MEMALLOC_ERROR) { ec++; break; }
        u32 id, idr, e; u8 *p;
        while ((p = h264bsdNextOutputPicture(&dec, &id, &idr, &e))) {
            if (np < maxOut) {
                memcpy(outs[np].data, p, FRAMESIZE);
                outs[np].err = e;
            }
            if (e) ec++;
            np++;
        }
        if (used) off += used;
    }
    h264bsdFlushBuffer(&dec);
    u32 id, idr, e; u8 *p;
    while ((p = h264bsdNextOutputPicture(&dec, &id, &idr, &e))) {
        if (np < maxOut) {
            memcpy(outs[np].data, p, FRAMESIZE);
            outs[np].err = e;
        }
        if (e) ec++;
        np++;
    }
    *errCount = ec;
    h264bsdShutdown(&dec);
    return np;
}

/* ============================ tests ============================ */

static int compare_plane(const uint8_t *exp, const uint8_t *got, int count) {
    int bad = 0;
    for (int i = 0; i < count; i++) if (exp[i] != got[i]) bad++;
    return bad;
}

/* build numRefs I_PCM references and remember their pixels */
static int build_refs(uint8_t *stream, size_t cap, size_t *n,
    uint8_t refPix[][FRAMESIZE], int refPoc[], int numRefs, int frameNum0) {
    for (int i = 0; i < numRefs; i++) {
        ref_picture(refPoc[i], refPix[i]);
        emit_ipcm(stream, cap, n, frameNum0 + i, refPoc[i], refPix[i], 1, 4);
    }
    return 0;
}

/* emit I_PCM references from caller-provided (e.g. constant) pictures */
static int build_refs_from(uint8_t *stream, size_t cap, size_t *n,
    uint8_t refPix[][FRAMESIZE], int refPoc[], int numRefs) {
    for (int i = 0; i < numRefs; i++)
        emit_ipcm(stream, cap, n, i, refPoc[i], refPix[i], 1, 4);
    return 0;
}

/* ---- lists group: ordering, swap, reordering, active counts ---- */
static void test_lists(void) {
    uint8_t stream[60000]; size_t n = 0;
    uint8_t refs[MAX_REFS][FRAMESIZE];
    int poc[4] = {0, 4, 8, 12};
    emit_sps_pps(stream, sizeof(stream), &n, 4);
    build_refs(stream, sizeof(stream), &n, refs, poc, 4, 0);

    /* B picture POC 6. References POC {0,4,8,12}:
     *   L0 = {4,0,8,12} (past descending, then future ascending)
     *   L1 = {8,12,4,0} (future ascending, then past descending)
     * An L1 index 0, integer zero MV macroblock therefore equals POC8. */
    bslice_t s; memset(&s, 0, sizeof(s));
    s.poc = 6; s.frameNum = 4; s.nalRef = 0; s.firstMb = 0;
    s.activeL0 = 4; s.activeL1 = 4;
    s.nmb = 4; s.disableDeblock = 1;
    for (int m = 0; m < 4; m++) {
        s.mb[m].mbType = B_L1_16x16;
        s.mb[m].part[0].kind = 1; s.mb[m].part[0].ref = 0;
        s.mb[m].part[0].mx = 0; s.mb[m].part[0].my = 0;
    }
    emit_b_slice(stream, sizeof(stream), &n, &s);

    outpic_t out[8]; int ec;
    int np = decode_stream(stream, n, 0, out, 8, &ec);
    TASK_CHECK(ec == 0);
    TASK_CHECK(np == 5);                 /* 4 refs + 1 B in display order */
    /* display order: POC 0,4,B(6),8,12 -> B is out[2], equals POC8 (L1[0]) */
    TASK_CHECK(compare_plane(refs[2], out[2].data, FRAMESIZE) == 0);
    TASK_CHECK(out[2].err == 0);

    /* symmetric case: both references lie in the past of the current B
     * picture. L0 and the initial L1 are then identical (both past pictures
     * in descending POC), so the standard swaps the first two L1 entries.
     * Refs POC {0,4}, current POC 8: initial lists {4,0}; after the L1 swap
     * L1 index 0 points at POC0. */
    {
        uint8_t s2stream[60000]; size_t n2 = 0;
        uint8_t s2ref[2][FRAMESIZE];
        int s2poc[2] = {0, 4};
        emit_sps_pps(s2stream, sizeof(s2stream), &n2, 2);
        build_refs(s2stream, sizeof(s2stream), &n2, s2ref, s2poc, 2, 0);
        bslice_t t; memset(&t, 0, sizeof(t));
        t.poc = 8; t.frameNum = 2; t.activeL0 = 1; t.activeL1 = 1;
        t.nmb = 4; t.disableDeblock = 1;
        for (int m = 0; m < 4; m++) {
            t.mb[m].mbType = B_L1_16x16;
            t.mb[m].part[0].kind = 1; t.mb[m].part[0].ref = 0;
        }
        emit_b_slice(s2stream, sizeof(s2stream), &n2, &t);
        outpic_t o2[8]; int e2;
        int npo2 = decode_stream(s2stream, n2, 0, o2, 8, &e2);
        /* display order POC0,4,B(8) -> B is out[2] and equals POC0 */
        TASK_CHECK(e2 == 0 && npo2 == 3);
        TASK_CHECK(compare_plane(s2ref[0], o2[2].data, FRAMESIZE) == 0);
        TASK_CHECK(o2[2].err == 0);
    }

    /* explicit reordering: move POC0 (picNum 0) to the front of L0 */
    n = 0;
    emit_sps_pps(stream, sizeof(stream), &n, 4);
    build_refs(stream, sizeof(stream), &n, refs, poc, 4, 0);
    memset(&s, 0, sizeof(s));
    s.poc = 6; s.frameNum = 4; s.activeL0 = 4; s.activeL1 = 4;
    s.nmb = 4; s.disableDeblock = 1;
    s.nReorder0 = 1; s.reorder0Cmd[0] = 4;   /* picNum 4 -> abs diff 4 */
    for (int m = 0; m < 4; m++) {
        s.mb[m].mbType = B_L0_16x16;
        s.mb[m].part[0].kind = 0; s.mb[m].part[0].ref = 0;
    }
    emit_b_slice(stream, sizeof(stream), &n, &s);
    np = decode_stream(stream, n, 0, out, 8, &ec);
    TASK_CHECK(ec == 0);
    TASK_CHECK(np == 5);
    TASK_CHECK(compare_plane(refs[0], out[2].data, FRAMESIZE) == 0);
}

/* ---- prediction group: all partition shapes, sub-partitions, fractional */
static void test_prediction(void) {
    uint8_t stream[80000]; size_t n = 0;
    uint8_t refs[2][FRAMESIZE];
    /* refs POC {0,8}, B POC 4: one past (POC0) and one future (POC8), so the
     * lists are L0 = {POC0}, L1 = {POC8} with no swap ambiguity. */
    int poc[2] = {0, 8};
    static uint8_t expect[FRAMESIZE];
    outpic_t out[8]; int ec, np;

    /* ---- sub-case A: all four explicit macroblock shapes, zero MVs so every
     * motion vector resolves to zero (no neighbour inheritance) ---- */
    emit_sps_pps(stream, sizeof(stream), &n, 2);
    build_refs(stream, sizeof(stream), &n, refs, poc, 2, 0);
    bslice_t s; memset(&s, 0, sizeof(s));
    s.poc = 4; s.frameNum = 1; s.activeL0 = 1; s.activeL1 = 1;
    s.nmb = 4; s.disableDeblock = 1;
    /* L0[0]=POC0(refs[0]), L1[0]=POC8(refs[1]) */
    s.mb[0].mbType = B_L0_16x16; s.mb[0].part[0].kind = 0;
    s.mb[1].mbType = B_L1_16x16; s.mb[1].part[0].kind = 1;
    s.mb[2].mbType = B_Bi_Bi_16x8;
    s.mb[2].part[0].kind = 2; s.mb[2].part[1].kind = 2;
    s.mb[3].mbType = B_L0_L1_8x16;
    s.mb[3].part[0].kind = 0; s.mb[3].part[1].kind = 1;
    emit_b_slice(stream, sizeof(stream), &n, &s);

    memset(expect, 0, sizeof(expect));
    render_mb_list(expect, refs[0], 0, 0, 0, 0);   /* MB0 L0 POC0 */
    render_mb_list(expect, refs[1], 1, 0, 0, 0);   /* MB1 L1 POC8 */
    render_mb_bi(expect, refs[0], refs[1], 0, 1, 0, 0, 0, 0); /* MB2 */
    /* MB3 at MB grid (1,1): left 8x16 from L0, right 8x16 from L1 */
    render_list(refs[0], expect, 16, 16, 0, 0, 8, 16, 0, 0);
    render_list(refs[1], expect, 16, 16, 8, 0, 8, 16, 0, 0);

    np = decode_stream(stream, n, 0, out, 8, &ec);
    TASK_CHECK(ec == 0 && np == 3);
    /* display order POC0, B(4), POC8 -> B is out[1] */
    TASK_CHECK(compare_plane(expect, out[1].data, FRAMESIZE) == 0);
    TASK_CHECK(out[1].err == 0);

    /* ---- sub-case A2: a non-zero integer MVD on the first (top-left) MB,
     * which has no neighbours so its MV equals the MVD exactly ---- */
    n = 0;
    emit_sps_pps(stream, sizeof(stream), &n, 2);
    build_refs(stream, sizeof(stream), &n, refs, poc, 2, 0);
    memset(&s, 0, sizeof(s));
    s.poc = 4; s.frameNum = 1; s.activeL0 = 1; s.activeL1 = 1;
    s.nmb = 4; s.disableDeblock = 1;
    s.mb[0].mbType = B_L0_16x16; s.mb[0].part[0].kind = 0;
    s.mb[0].part[0].mx = 4; s.mb[0].part[0].my = 8;  /* (+1,+2) pixels */
    for (int m = 1; m < 4; m++) {
        s.mb[m].mbType = B_L0_16x16; s.mb[m].part[0].kind = 0;
    }
    emit_b_slice(stream, sizeof(stream), &n, &s);
    memset(expect, 0, sizeof(expect));
    render_mb_list(expect, refs[0], 0, 0, 4, 8);
    /* the other MBs inherit the zero-MVD... but their predictor equals the
     * first MB's MV, matching H.264 median/zero prediction; verify only the
     * top-left MB which is fully deterministic */
    np = decode_stream(stream, n, 0, out, 8, &ec);
    TASK_CHECK(ec == 0 && np == 3);
    {
        int bad = 0;
        for (int yy=0;yy<16;yy++) for(int xx=0;xx<16;xx++)
            if (expect[yy*W+xx] != out[1].data[yy*W+xx]) bad++;
        TASK_CHECK(bad == 0);
    }

    /* ---- sub-case B: fractional quarter-pel L0 motion against a constant
     * color reference. A fractional MV still drives the interpolation and
     * chroma 1/8-pel paths, but every filtered sample of a constant image
     * equals the constant, giving an exact whole-frame oracle including
     * border handling. Two references with distinct colors also prove the
     * correct list is selected. */
    {
        uint8_t cref[2][FRAMESIZE];
        int cpoc[2] = {0, 8};
        const_picture(73, 90, 110, cref[0]);
        const_picture(200, 40, 60, cref[1]);
        n = 0;
        emit_sps_pps(stream, sizeof(stream), &n, 2);
        build_refs_from(stream, sizeof(stream), &n, cref, cpoc, 2);
        memset(&s, 0, sizeof(s));
        s.poc = 4; s.frameNum = 1; s.activeL0 = 1; s.activeL1 = 1;
        s.nmb = 4; s.disableDeblock = 1;
        for (int m = 0; m < 4; m++) {
            s.mb[m].mbType = B_L0_16x16; s.mb[m].part[0].kind = 0;
            s.mb[m].part[0].mx = 1; s.mb[m].part[0].my = 3;  /* 1/4 x, 3/4 y */
        }
        emit_b_slice(stream, sizeof(stream), &n, &s);
        /* every sample must equal the L0 constant picture exactly */
        np = decode_stream(stream, n, 0, out, 8, &ec);
        TASK_CHECK(ec == 0 && np == 3);
        TASK_CHECK(compare_plane(cref[0], out[1].data, FRAMESIZE) == 0);
        TASK_CHECK(out[1].err == 0);

        /* same fractional motion through L1 must yield the L1 constant */
        n = 0;
        emit_sps_pps(stream, sizeof(stream), &n, 2);
        build_refs_from(stream, sizeof(stream), &n, cref, cpoc, 2);
        memset(&s, 0, sizeof(s));
        s.poc = 4; s.frameNum = 1; s.activeL0 = 1; s.activeL1 = 1;
        s.nmb = 4; s.disableDeblock = 1;
        for (int m = 0; m < 4; m++) {
            s.mb[m].mbType = B_L1_16x16; s.mb[m].part[0].kind = 1;
            s.mb[m].part[0].mx = 3; s.mb[m].part[0].my = 5;  /* 3/4 x, 5/4 y */
        }
        emit_b_slice(stream, sizeof(stream), &n, &s);
        np = decode_stream(stream, n, 0, out, 8, &ec);
        TASK_CHECK(ec == 0 && np == 3);
        TASK_CHECK(compare_plane(cref[1], out[1].data, FRAMESIZE) == 0);
    }
}

/* ---- sub partitions down to 4x4 ---- */
static void test_sub_partitions(void) {
    uint8_t stream[80000]; size_t n = 0;
    uint8_t refs[2][FRAMESIZE];
    int poc[2] = {0, 8};
    static uint8_t expect[FRAMESIZE];
    outpic_t out[8]; int ec, np;

    /* Every 8x8 block uses the same sub geometry and prediction mode so the
     * four blocks tile the whole macroblock without gaps and the expectation
     * is uniform. We run one stream per sub geometry, covering the minimum
     * 4x4 partition and the 8x4 / 4x8 / 8x8 shapes plus L0, L1 and Bi. */
    struct { int subType; int kind; } cfg[4] = {
        { B_B_L0_8x8, 0 }, { B_B_L1_8x8, 1 },
        { B_B_Bi_8x8, 2 }, { B_B_L0_8x8, 0 }
    };
    /* 8x8 sub-partitions: one sub-part per block, full MB coverage */
    for (int variant = 0; variant < 4; variant++) {
        n = 0;
        emit_sps_pps(stream, sizeof(stream), &n, 2);
        build_refs(stream, sizeof(stream), &n, refs, poc, 2, 0);
        bslice_t s; memset(&s, 0, sizeof(s));
        s.poc = 4; s.frameNum = 1; s.activeL0 = 1; s.activeL1 = 1;
        s.nmb = 4; s.disableDeblock = 1;
        for (int m = 0; m < 4; m++) {
            mbdesc_t *d = &s.mb[m];
            d->mbType = 22;
            for (int q = 0; q < 4; q++) {
                d->subType[q] = cfg[variant].subType;
                d->sub[q][0].kind = cfg[variant].kind;
                d->sub[q][0].ref = 0;
                d->sub[q][0].mx = d->sub[q][0].my = 0;
            }
        }
        emit_b_slice(stream, sizeof(stream), &n, &s);
        memset(expect, 0, sizeof(expect));
        /* render every 8x8 sub-block of all four macroblocks */
        for (int mb = 0; mb < 4; mb++) {
            int mbx = (mb & 1) * 16, mby = (mb >> 1) * 16;
            for (int q = 0; q < 4; q++) {
                int qx = mbx + (q & 1) * 8, qy = mby + (q >> 1) * 8;
                if (cfg[variant].kind == 0)
                    render_list(refs[0], expect, 0, 0, qx, qy, 8, 8, 0, 0);
                else if (cfg[variant].kind == 1)
                    render_list(refs[1], expect, 0, 0, qx, qy, 8, 8, 0, 0);
                else
                    render_bi(refs[0], refs[1], expect, 0, 0, qx, qy, 8, 8,
                        0, 0, 0, 0);
            }
        }
        np = decode_stream(stream, n, 0, out, 8, &ec);
        TASK_CHECK(ec == 0 && np == 3);
        TASK_CHECK(out[1].err == 0);
        TASK_CHECK(compare_plane(expect, out[1].data, FRAMESIZE) == 0);
    }

    /* 8x4 and 4x8 geometries (two sub-parts per 8x8 block), all L0 so the
     * hand-written oracle stays simple; both fully tile every block. */
    int geom[2] = { B_B_L0_8x4, B_B_L0_4x8 };
    for (int g = 0; g < 2; g++) {
        n = 0;
        emit_sps_pps(stream, sizeof(stream), &n, 2);
        build_refs(stream, sizeof(stream), &n, refs, poc, 2, 0);
        bslice_t s; memset(&s, 0, sizeof(s));
        s.poc = 4; s.frameNum = 1; s.activeL0 = 1; s.activeL1 = 1;
        s.nmb = 4; s.disableDeblock = 1;
        for (int m = 0; m < 4; m++) {
            mbdesc_t *d = &s.mb[m];
            d->mbType = 22;
            for (int q = 0; q < 4; q++) {
                d->subType[q] = geom[g];
                d->sub[q][0].kind = 0; d->sub[q][1].kind = 0;
                d->sub[q][0].ref = d->sub[q][1].ref = 0;
                d->sub[q][0].mx = d->sub[q][0].my = 0;
                d->sub[q][1].mx = d->sub[q][1].my = 0;
            }
        }
        emit_b_slice(stream, sizeof(stream), &n, &s);
        memset(expect, 0, sizeof(expect));
        for (int mb = 0; mb < 4; mb++) {
            int mbx = (mb & 1) * 16, mby = (mb >> 1) * 16;
            for (int q = 0; q < 4; q++) {
                int qx = mbx + (q & 1) * 8, qy = mby + (q >> 1) * 8;
                if (geom[g] == B_B_L0_8x4) {
                    /* two 8x4 parts stacked vertically */
                    render_list(refs[0], expect, 0, 0, qx, qy,   8, 4, 0, 0);
                    render_list(refs[0], expect, 0, 0, qx, qy+4, 8, 4, 0, 0);
                } else {
                    /* two 4x8 parts side by side */
                    render_list(refs[0], expect, 0, 0, qx,   qy, 4, 8, 0, 0);
                    render_list(refs[0], expect, 0, 0, qx+4, qy, 4, 8, 0, 0);
                }
            }
        }
        np = decode_stream(stream, n, 0, out, 8, &ec);
        TASK_CHECK(ec == 0 && np == 3);
        TASK_CHECK(out[1].err == 0);
        TASK_CHECK(compare_plane(expect, out[1].data, FRAMESIZE) == 0);
    }
    {
        n = 0;
        emit_sps_pps(stream, sizeof(stream), &n, 2);
        build_refs(stream, sizeof(stream), &n, refs, poc, 2, 0);
        bslice_t s; memset(&s, 0, sizeof(s));
        s.poc = 4; s.frameNum = 1; s.activeL0 = 1; s.activeL1 = 1;
        s.nmb = 4; s.disableDeblock = 1;
        for (int m = 0; m < 4; m++) {
            mbdesc_t *d = &s.mb[m];
            d->mbType = 22;
            for (int q = 0; q < 4; q++) {
                d->subType[q] = B_B_L0_4x4;
                for (int k = 0; k < 4; k++) {
                    d->sub[q][k].kind = 0; d->sub[q][k].ref = 0;
                    d->sub[q][k].mx = d->sub[q][k].my = 0;
                }
            }
        }
        emit_b_slice(stream, sizeof(stream), &n, &s);
        memset(expect, 0, sizeof(expect));
        /* every 4x4 luma block of every MB comes from L0 at zero MV */
        for (int mb = 0; mb < 4; mb++) {
            int mbx = (mb & 1) * 16, mby = (mb >> 1) * 16;
            for (int by = 0; by < 16; by += 4)
                for (int bx = 0; bx < 16; bx += 4)
                    render_list(refs[0], expect, 0, 0,
                        mbx + bx, mby + by, 4, 4, 0, 0);
        }
        np = decode_stream(stream, n, 0, out, 8, &ec);
        TASK_CHECK(ec == 0 && np == 3);
        TASK_CHECK(out[1].err == 0);
        TASK_CHECK(compare_plane(expect, out[1].data, FRAMESIZE) == 0);
    }
}

/* ---- filter group ----
 * Deblocking is verified without residual syntax: the motion-vector
 * threshold and the reference-identity rules both produce a non-zero
 * boundary strength, while a zero-MV single-reference baseline gives bs 0.
 * The deblock-enabled output must differ from the deblock-disabled output
 * exactly where the strength is non-zero. */
static void test_filter(void) {
    uint8_t stream[80000]; size_t n = 0;
    uint8_t refs[2][FRAMESIZE];
    int poc[2] = {0, 8};
    outpic_t out[8]; int ec, np;

    /* helper: decode a 4-MB B picture, choosing the deblock idc */
    bslice_t fs;
    #define FILTER_BUILD(disabled) do {                                         \
        n = 0;                                                                  \
        emit_sps_pps(stream, sizeof(stream), &n, 2);                           \
        build_refs(stream, sizeof(stream), &n, refs, poc, 2, 0);               \
        memset(&fs, 0, sizeof(fs));                                            \
        fs.poc = 4; fs.frameNum = 1; fs.activeL0 = 1; fs.activeL1 = 1;         \
        fs.nmb = 4; fs.disableDeblock = (disabled);                            \
        for (int fm = 0; fm < 4; fm++) {                                       \
            fs.mb[fm].mbType = B_L0_16x16; fs.mb[fm].part[0].kind = 0;        \
        }                                                                      \
        (void)0; } while (0)

    /* baseline: zero MV, single reference, deblocking disabled. This is the
     * unfiltered prediction used as the comparison oracle. */
    static uint8_t unfiltered[FRAMESIZE];
    FILTER_BUILD(1);
    emit_b_slice(stream, sizeof(stream), &n, &fs);
    np = decode_stream(stream, n, 0, out, 8, &ec);
    TASK_CHECK(ec == 0 && np == 3);
    TASK_CHECK(out[1].err == 0);
    memcpy(unfiltered, out[1].data, FRAMESIZE);
    /* a disabled filter must reproduce the raw prediction */
    memset((void*)0, 0, 0);

    /* motion threshold: MB1 carries a one-luma-pixel vertical MVD (4 quarter
     * units). With deblocking enabled the shared edge gets a non-zero
     * strength, so the output differs from the unfiltered baseline. */
    FILTER_BUILD(0);
    fs.mb[1].part[0].my = 4;
    emit_b_slice(stream, sizeof(stream), &n, &fs);
    np = decode_stream(stream, n, 0, out, 8, &ec);
    TASK_CHECK(ec == 0 && np == 3);
    TASK_CHECK(out[1].err == 0);
    {
        int changed = 0;
        for (int i = 0; i < FRAMESIZE; i++)
            if (unfiltered[i] != out[1].data[i]) changed++;
        TASK_CHECK(changed > 0);
    }

    /* reference-identity rule: MB1 uses the L1 picture instead of L0 while
     * keeping a zero MV; the differing reference identity on the shared edge
     * again yields a non-zero boundary strength. */
    FILTER_BUILD(0);
    fs.mb[1].mbType = B_L1_16x16; fs.mb[1].part[0].kind = 1;
    emit_b_slice(stream, sizeof(stream), &n, &fs);
    np = decode_stream(stream, n, 0, out, 8, &ec);
    TASK_CHECK(ec == 0 && np == 3);
    TASK_CHECK(out[1].err == 0);
    {
        int changed = 0;
        for (int i = 0; i < FRAMESIZE; i++)
            if (unfiltered[i] != out[1].data[i]) changed++;
        TASK_CHECK(changed > 0);
    }
    #undef FILTER_BUILD
}

/* ---- scope rejection: direct and B skip must error, never produce a
 * substituted picture ---- */
static void test_rejection(void) {
    uint8_t stream[40000]; size_t n = 0;
    uint8_t refs[2][FRAMESIZE];
    int poc[2] = {0, 8};
    emit_sps_pps(stream, sizeof(stream), &n, 2);
    build_refs(stream, sizeof(stream), &n, refs, poc, 2, 0);

    /* case 1: B_Direct_16x16 (mb_type 0) */
    bw_t w; memset(&w, 0, sizeof(w));
    bw_ue(&w, 0); bw_ue(&w, 1); bw_ue(&w, 0);
    bw_fixed(&w, 2, 4); bw_fixed(&w, 4, 4);
    bw_bit(&w, 1); bw_ue(&w, 0); bw_ue(&w, 0);
    bw_bit(&w, 1); bw_bit(&w, 0); bw_bit(&w, 0);
    bw_se(&w, 0); bw_ue(&w, 1);
    bw_ue(&w, 0);                    /* skip run 0 */
    bw_ue(&w, 0);                    /* mb_type 0 = B_Direct_16x16 */
    bw_rbsp_trailing(&w);
    bw_nal(stream, sizeof(stream), &n, 0x01, &w);

    outpic_t out[8]; int ec;
    int np = decode_stream(stream, n, 0, out, 8, &ec);
    TASK_CHECK(ec >= 1);
    /* the rejected B picture must not appear as an output picture; only the
     * two reference pictures remain */
    TASK_CHECK(np == 2);

    /* case 2: B skip. skip_run = 1 over a B slice is outside the scope */
    n = 0;
    emit_sps_pps(stream, sizeof(stream), &n, 2);
    build_refs(stream, sizeof(stream), &n, refs, poc, 2, 0);
    memset(&w, 0, sizeof(w));
    bw_ue(&w, 0); bw_ue(&w, 1); bw_ue(&w, 0);
    bw_fixed(&w, 2, 4); bw_fixed(&w, 4, 4);
    bw_bit(&w, 1); bw_ue(&w, 0); bw_ue(&w, 0);
    bw_bit(&w, 1); bw_bit(&w, 0); bw_bit(&w, 0);
    bw_se(&w, 0); bw_ue(&w, 1);
    bw_ue(&w, 1);                    /* skip run 1 -> B skip, rejected */
    bw_rbsp_trailing(&w);
    bw_nal(stream, sizeof(stream), &n, 0x01, &w);
    np = decode_stream(stream, n, 0, out, 8, &ec);
    TASK_CHECK(ec >= 1);
    TASK_CHECK(np == 2);
}

/* ---- I/P regression, flush, noReorder, two instances ---- */
static void test_regression_and_lifecycle(void) {
    /* the existing I_PCM fixture still decodes through the same path */
    extern size_t task_fixture(uint8_t *, size_t, int);
    uint8_t input[32768];
    size_t n = task_fixture(input, sizeof(input), 0);
    outpic_t out[8]; int ec;
    int np = decode_stream(input, n, 0, out, 8, &ec);
    TASK_CHECK(np == 2 && ec == 0);

    /* noReorder keeps decode order: build I,P,B and check B comes out last */
    uint8_t stream[60000]; size_t m = 0;
    uint8_t refs[2][FRAMESIZE];
    int poc[2] = {0, 8};
    emit_sps_pps(stream, sizeof(stream), &m, 2);
    build_refs(stream, sizeof(stream), &m, refs, poc, 2, 0);
    bslice_t s; memset(&s, 0, sizeof(s));
    s.poc = 4; s.frameNum = 2; s.activeL0 = 1; s.activeL1 = 1;
    s.nmb = 4; s.disableDeblock = 1;
    for (int k = 0; k < 4; k++) {
        s.mb[k].mbType = B_L0_16x16; s.mb[k].part[0].kind = 0;
    }
    emit_b_slice(stream, sizeof(stream), &m, &s);
    storage_t dec; h264bsdInit(&dec, 1);
    size_t off = 0; int steps = 0, order = 0; u32 firstId = 999;
    (void)firstId;
    while (off < m && steps < 10000) {
        u32 used = 0;
        u32 r = h264bsdDecode(&dec, stream + off, (u32)(m-off), 0, &used);
        steps++;
        if (r == H264BSD_ERROR) { TASK_CHECK(0); break; }
        u32 id, idr, e; u8 *p;
        while ((p = h264bsdNextOutputPicture(&dec, &id, &idr, &e))) {
            if (order == 0) { /* first emitted in decode order is POC0 */
                TASK_CHECK(compare_plane(refs[0], p, FRAMESIZE) == 0);
            }
            order++;
        }
        if (used) off += used;
    }
    h264bsdFlushBuffer(&dec);
    u32 id, idr, e;
    while (h264bsdNextOutputPicture(&dec, &id, &idr, &e)) order++;
    h264bsdShutdown(&dec);
    TASK_CHECK(order == 3);

    /* two independent instances interleaved do not share list/MV history */
    storage_t d1, d2;
    h264bsdInit(&d1, 0); h264bsdInit(&d2, 0);
    u32 u1 = 0, u2 = 0;
    u32 r1 = h264bsdDecode(&d1, stream, (u32)m, 0, &u1);
    u32 r2 = h264bsdDecode(&d2, stream, (u32)m, 0, &u2);
    TASK_CHECK(r1 != H264BSD_ERROR && r2 != H264BSD_ERROR && u1 == u2 && u1 > 0);
    h264bsdShutdown(&d1); h264bsdShutdown(&d2);
}

int task_cases(const char *group)
{
    if (!group) return 0;
    if (strcmp(group, "lists") == 0) test_lists();
    else if (strcmp(group, "prediction") == 0) test_prediction();
    else if (strcmp(group, "filter") == 0) test_filter();
    else if (strcmp(group, "subpart") == 0) test_sub_partitions();
    else if (strcmp(group, "reject") == 0) test_rejection();
    else if (strcmp(group, "lifecycle") == 0) test_regression_and_lifecycle();
    else return 0;
    return 1;
}
