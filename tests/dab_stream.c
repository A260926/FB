/*
 *  tests/dab_stream.c
 *
 *  Independent checks for DAB streaming mode (twolame_set_DAB_streaming).
 *
 *  Every expected ScF-CRC value is recomputed here from the transmitted
 *  bit allocation, scfsi and scalefactors parsed out of the real encoder
 *  output. The library's own CRC calculation and backfill helpers are never
 *  called to obtain expected values.
 */

#include "twolame.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPF TWOLAME_SAMPLES_PER_FRAME   /* 1152 */
#define MAXSAMPS (SPF * 8)
#define STREAM_CAP (1 << 18)
#define MAXFRAMES 32
#define ENC_CAP (7200 + 2 * MAXSAMPS)

static int g_asserts = 0;
static long g_crc_checks = 0;   /* number of ScF-CRC byte positions verified */

static void fail(int line, const char *expr)
{
    fprintf(stderr, "DAB_STREAM_FAIL line=%d expr=%s\n", line, expr);
    exit(1);
}
#define CHECK(x) do { g_asserts++; if (!(x)) fail(__LINE__, #x); } while (0)


/* ------------------------------------------------------------------ */
/* test material                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    int rate;
    int bitrate;
    int channels;
    int crc_len;
    int ancillary_bits;
    int error_protection;
    TWOLAME_Padding padding;
    int dab;                    /* enable the DAB extensions */
} cfg_t;

typedef struct {
    unsigned char buf[STREAM_CAP];
    int len;
} stream_t;

static void sput(stream_t *s, const unsigned char *p, int n)
{
    CHECK(s->len + n <= (int) sizeof(s->buf));
    memcpy(s->buf + s->len, p, n);
    s->len += n;
}

/* non-zero sound, different content on every frame */
static void gen_tone(short *l, short *r, int n, int rate, unsigned seed)
{
    uint32_t rng = seed ? seed : 1u;
    int i;
    for (i = 0; i < n; i++) {
        double t = (double) i / (double) rate;
        double f = 340.0 + 260.0 * (double) ((i / SPF) % 5);
        double v, w;
        rng = rng * 1664525u + 1013904223u;
        v = 8000.0 * sin(6.283185307179586 * f * t)
            + 2500.0 * sin(6.283185307179586 * 73.0 * t)
            + 3000.0 * ((double) (int) (rng >> 16) - 32768.0) / 32768.0;
        if (v > 32000)
            v = 32000;
        else if (v < -32000)
            v = -32000;
        l[i] = (short) v;
        if (r) {
            w = 0.7 * v + 2500.0 * sin(6.283185307179586 * 997.0 * t);
            if (w > 32000)
                w = 32000;
            else if (w < -32000)
                w = -32000;
            r[i] = (short) w;
        }
    }
}

static twolame_options *open_cfg(const cfg_t *c, int streaming)
{
    twolame_options *o = twolame_init();
    CHECK(o != NULL);
    CHECK(!twolame_set_num_channels(o, c->channels));
    CHECK(!twolame_set_in_samplerate(o, c->rate));
    CHECK(!twolame_set_out_samplerate(o, c->rate));
    CHECK(!twolame_set_bitrate(o, c->bitrate));
    CHECK(!twolame_set_mode(o, c->channels == 1 ? TWOLAME_MONO : TWOLAME_STEREO));
    CHECK(!twolame_set_verbosity(o, 0));
    CHECK(!twolame_set_padding(o, c->padding));
    CHECK(!twolame_set_error_protection(o, c->error_protection));
    CHECK(!twolame_set_DAB(o, c->dab));
    CHECK(!twolame_set_DAB_crc_length(o, c->crc_len));
    CHECK(!twolame_set_DAB_xpad_length(o, 0));
    CHECK(!twolame_set_num_ancillary_bits(o, c->ancillary_bits));
    CHECK(!twolame_set_DAB_streaming(o, streaming));
    CHECK(twolame_get_DAB_streaming(o) == streaming);
    CHECK(!twolame_init_params(o));
    return o;
}

/* Feed all pcm through a streaming encoder, then flush. pattern: 0 = single
   bulk call, 1 = 1152-sample calls, 2 = alternating 1/37/2049-sample calls.
   The same output buffer is reused (and dirtied) for every call; returned
   bytes are copied out immediately. */
static void run_streaming(const cfg_t *c, const short *l, const short *r, const short *inter,
                          int nsamp, int interleaved, int pattern, stream_t *out)
{
    static unsigned char buf[ENC_CAP];
    twolame_options *o = open_cfg(c, 1);
    int pos = 0, step = 0, n, k;
    out->len = 0;
    while (pos < nsamp) {
        if (pattern == 0)
            k = nsamp - pos;
        else if (pattern == 1)
            k = SPF;
        else
            k = (step % 3 == 0) ? 1 : ((step % 3 == 1) ? 37 : 2049);
        if (k > nsamp - pos)
            k = nsamp - pos;
        memset(buf, 0xAA, sizeof(buf));
        if (interleaved)
            n = twolame_encode_buffer_interleaved(o, inter + (size_t) pos * c->channels, k,
                                                  buf, sizeof(buf));
        else
            n = twolame_encode_buffer(o, l + pos, r + pos, k, buf, sizeof(buf));
        CHECK(n >= 0);
        CHECK(n <= 7200 + 2 * k);
        sput(out, buf, n);
        pos += k;
        step++;
    }
    memset(buf, 0xAA, sizeof(buf));
    n = twolame_encode_flush(o, buf, sizeof(buf));
    CHECK(n >= 0);
    CHECK(n <= 7200);
    sput(out, buf, n);
    twolame_close(&o);
}

/* Same configuration with DAB streaming off, fed frame by frame (the
   original single-frame mode). Produces the raw frames the streaming
   output is compared against. */
static void run_legacy_raw(const cfg_t *c, const short *l, const short *r, const short *inter,
                           int nsamp, int interleaved, int chunk, stream_t *out)
{
    static unsigned char buf[ENC_CAP];
    twolame_options *o = open_cfg(c, 0);
    int pos = 0, n, k;
    out->len = 0;
    while (pos < nsamp) {
        k = chunk;
        if (k > nsamp - pos)
            k = nsamp - pos;
        if (interleaved)
            n = twolame_encode_buffer_interleaved(o, inter + (size_t) pos * c->channels, k,
                                                  buf, sizeof(buf));
        else
            n = twolame_encode_buffer(o, l + pos, r + pos, k, buf, sizeof(buf));
        CHECK(n >= 0);
        sput(out, buf, n);
        pos += k;
    }
    n = twolame_encode_flush(o, buf, sizeof(buf));
    CHECK(n >= 0);
    sput(out, buf, n);
    twolame_close(&o);
}


/* ------------------------------------------------------------------ */
/* independent MP2 (MPEG-1/2 Layer II) frame parser                    */
/* ------------------------------------------------------------------ */

static const int br_tab[2][15] = {
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},     /* MPEG-2 */
    {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384} /* MPEG-1 */
};
static const int sr_tab[3] = { 44100, 48000, 32000 };   /* halved for MPEG-2 */

/* bit allocation table layout, mirroring the encoder's table definitions */
static const int tab_sblimit[5] = { 27, 30, 8, 12, 30 };
static const int tab_nbal[9] = { 4, 4, 3, 2, 4, 3, 4, 3, 2 };
static const int tab_line[5][32] = {
    {0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, -1, -1, -1,
     -1, -1},
    {0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3,
     -1, -1},
    {4, 4, 5, 5, 5, 5, 5, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
     -1, -1, -1, -1, -1, -1, -1},
    {4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
     -1, -1, -1, -1, -1, -1},
    {6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
     8, 8}
};

typedef struct {
    int version;                /* 1 = MPEG-1, 0 = MPEG-2 */
    int error_protection;
    int bitrate;                /* kbps */
    int samplerate;             /* Hz */
    int padding;
    int mode;                   /* 0 = stereo, 3 = mono */
    int nch;
    int frame_len;              /* bytes */
    int sblimit;
    unsigned bit_alloc[2][32];
    unsigned scfsi[2][32];
    unsigned scalar[2][3][32];
    unsigned crc[4];            /* independently recomputed ScF-CRC bytes */
} frame_info;

typedef struct {
    const unsigned char *p;
    long pos;
} bitrd;

static unsigned getbits(bitrd *b, int n)
{
    unsigned v = 0;
    while (n-- > 0) {
        v = (v << 1) | ((b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1u);
        b->pos++;
    }
    return v;
}

/* allocation table selection, same decision rules as the encoder */
static int pick_table(int version, int samplerate, int bitrate, int nch)
{
    int br_per_ch = bitrate / nch;
    int sfrq = samplerate / 1000;
    if (version == 1) {
        if ((sfrq == 48 && br_per_ch >= 56) || (br_per_ch >= 56 && br_per_ch <= 80))
            return 0;
        if (sfrq != 48 && br_per_ch >= 96)
            return 1;
        if (sfrq != 32 && br_per_ch <= 48)
            return 2;
        return 3;
    }
    return 4;
}

static int parse_frame(const unsigned char *p, int avail, frame_info *fi)
{
    uint32_t h;
    int bri, sri, tabnum, sb, ch;
    bitrd b;

    if (avail < 4)
        return -1;
    h = ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
    if ((h >> 20) != 0xFFF)
        return -1;              /* syncword */
    fi->version = (h >> 19) & 1;
    if (((h >> 17) & 3) != 2)
        return -1;              /* layer II only */
    fi->error_protection = !((h >> 16) & 1);
    bri = (h >> 12) & 15;
    sri = (h >> 10) & 3;
    if (bri == 0 || bri == 15 || sri == 3)
        return -1;
    fi->padding = (h >> 9) & 1;
    fi->mode = (h >> 6) & 3;
    if (fi->mode != 0 && fi->mode != 3)
        return -1;              /* this parser covers mono/stereo only */
    fi->bitrate = br_tab[fi->version][bri];
    fi->samplerate = sr_tab[sri] >> (fi->version == 0 ? 1 : 0);
    fi->nch = (fi->mode == 3) ? 1 : 2;
    fi->frame_len = 144000 * fi->bitrate / fi->samplerate + fi->padding;
    if (fi->frame_len > avail)
        return -1;

    memset(fi->bit_alloc, 0, sizeof(fi->bit_alloc));
    memset(fi->scfsi, 0, sizeof(fi->scfsi));
    memset(fi->scalar, 0, sizeof(fi->scalar));

    tabnum = pick_table(fi->version, fi->samplerate, fi->bitrate, fi->nch);
    fi->sblimit = tab_sblimit[tabnum];

    b.p = p;
    b.pos = 32;
    if (fi->error_protection)
        getbits(&b, 16);        /* MPEG header CRC */

    for (sb = 0; sb < fi->sblimit; sb++)
        for (ch = 0; ch < fi->nch; ch++)
            fi->bit_alloc[ch][sb] = getbits(&b, tab_nbal[tab_line[tabnum][sb]]);

    for (sb = 0; sb < fi->sblimit; sb++)
        for (ch = 0; ch < fi->nch; ch++)
            if (fi->bit_alloc[ch][sb])
                fi->scfsi[ch][sb] = getbits(&b, 2);

    for (sb = 0; sb < fi->sblimit; sb++)
        for (ch = 0; ch < fi->nch; ch++)
            if (fi->bit_alloc[ch][sb]) {
                switch (fi->scfsi[ch][sb]) {
                case 0:
                    fi->scalar[ch][0][sb] = getbits(&b, 6);
                    fi->scalar[ch][1][sb] = getbits(&b, 6);
                    fi->scalar[ch][2][sb] = getbits(&b, 6);
                    break;
                case 1:
                case 3:
                    fi->scalar[ch][0][sb] = getbits(&b, 6);
                    fi->scalar[ch][2][sb] = getbits(&b, 6);
                    break;
                case 2:
                    fi->scalar[ch][0][sb] = getbits(&b, 6);
                    break;
                }
            }
    return 0;
}

/* CRC-8, polynomial 0x1D, MSB first - the DAB ScF-CRC algorithm */
static unsigned crc8(unsigned crc, unsigned data, int nbits)
{
    unsigned m;
    for (m = 1u << (nbits - 1); m; m >>= 1) {
        unsigned top = crc & 0x80u;
        crc = (crc << 1) & 0xFFu;
        if (!top ^ !(data & m))
            crc ^= 0x1Du;
    }
    return crc & 0xFFu;
}

/* Recompute the four packed ScF-CRC values of a parsed frame: subband
   ranges 0-3, 4-7, 8-15, 16-29 (clamped to sblimit), channels inner,
   scalefactors in transmission order, top 3 bits of each. */
static void crc_of(frame_info *fi)
{
    static const int f[5] = { 0, 4, 8, 16, 30 };
    int packed, i, k, j;
    for (packed = 0; packed < 4; packed++) {
        unsigned crc = 0;
        int first = f[packed];
        int last = f[packed + 1];
        if (last > fi->sblimit)
            last = fi->sblimit;
        for (i = first; i < last; i++)
            for (k = 0; k < fi->nch; k++)
                if (fi->bit_alloc[k][i]) {
                    switch (fi->scfsi[k][i]) {
                    case 0:
                        for (j = 0; j < 3; j++)
                            crc = crc8(crc, fi->scalar[k][j][i] >> 3, 3);
                        break;
                    case 1:
                    case 3:
                        crc = crc8(crc, fi->scalar[k][0][i] >> 3, 3);
                        crc = crc8(crc, fi->scalar[k][2][i] >> 3, 3);
                        break;
                    case 2:
                        crc = crc8(crc, fi->scalar[k][0][i] >> 3, 3);
                        break;
                    }
                }
        fi->crc[packed] = crc;
    }
}

/* walk a buffer of concatenated frames, parsing and recomputing CRCs */
static int scan_frames(const unsigned char *p, int n, frame_info *fi, int *offs, int maxf)
{
    int off = 0, cnt = 0;
    while (off < n) {
        CHECK(cnt < maxf);
        CHECK(parse_frame(p + off, n - off, &fi[cnt]) == 0);
        crc_of(&fi[cnt]);
        if (offs)
            offs[cnt] = off;
        off += fi[cnt].frame_len;
        cnt++;
    }
    return cnt;
}

static int frames_in(const unsigned char *p, int n)
{
    static frame_info fi[MAXFRAMES];
    return scan_frames(p, n, fi, NULL, MAXFRAMES);
}

/* Verify the ScF-CRC chain of a parsed stream: the tail of frame i holds
   the CRC of frame i+1 (first CRC byte at offset L-3, stored backwards,
   2 F-PAD bytes after it), the final frame carries zero CRC bytes, and
   any extra reserved ancillary bytes are left as encoded (zero). */
static void verify_chain(const unsigned char *stream, const int *offs, const frame_info *fi,
                         int nf, int crc_len, int anc_bytes)
{
    int i, j;
    for (i = 0; i < nf; i++) {
        int L = fi[i].frame_len;
        const unsigned char *fr = stream + offs[i];
        CHECK(fr[L - 2] == 0 && fr[L - 1] == 0);        /* F-PAD untouched */
        for (j = crc_len + 2; j < anc_bytes; j++)
            CHECK(fr[L - 1 - j] == 0);  /* extra reserved ancillary bytes untouched */
        if (i + 1 < nf) {
            for (j = 0; j < crc_len; j++) {
                CHECK(fr[L - 3 - j] == (unsigned char) fi[i + 1].crc[j]);
                g_crc_checks++;
            }
        } else {
            for (j = 0; j < crc_len; j++)
                CHECK(fr[L - 3 - j] == 0);
        }
    }
}

/* The streaming output must be byte-identical to the same encoder running
   in the original single-frame mode, except at the ScF-CRC positions. */
static void compare_to_legacy(const stream_t *s, const stream_t *leg, int crc_len)
{
    int o1 = 0, o2 = 0, i;
    frame_info f1, f2;
    CHECK(s->len == leg->len);
    while (o1 < s->len) {
        CHECK(parse_frame(s->buf + o1, s->len - o1, &f1) == 0);
        CHECK(parse_frame(leg->buf + o2, leg->len - o2, &f2) == 0);
        CHECK(f1.frame_len == f2.frame_len);
        for (i = 0; i < f1.frame_len; i++) {
            if (i >= f1.frame_len - crc_len - 2 && i <= f1.frame_len - 3)
                continue;       /* ScF-CRC positions */
            CHECK(s->buf[o1 + i] == leg->buf[o2 + i]);
        }
        o1 += f1.frame_len;
        o2 += f2.frame_len;
    }
}

static void expect_same_stream(const stream_t *a, const stream_t *b)
{
    CHECK(a->len == b->len);
    CHECK(!memcmp(a->buf, b->buf, a->len));
}


/* ------------------------------------------------------------------ */
/* case: crc                                                           */
/* ------------------------------------------------------------------ */

static void crc_round(const cfg_t *c, unsigned seed)
{
    static short l[MAXSAMPS], r[MAXSAMPS];
    int nsamp = 5 * SPF;
    stream_t s, leg;
    frame_info fi[MAXFRAMES];
    int offs[MAXFRAMES];
    int nf, i, j;

    gen_tone(l, r, nsamp, c->rate, seed);
    run_streaming(c, l, r, NULL, nsamp, 0, 0, &s);
    nf = scan_frames(s.buf, s.len, fi, offs, MAXFRAMES);
    CHECK(nf == 5);

    /* every frame's CRC tuple is actually non-zero, adjacent frames differ */
    for (i = 0; i < nf; i++) {
        unsigned acc = 0;
        for (j = 0; j < c->crc_len; j++)
            acc |= fi[i].crc[j];
        CHECK(acc != 0);
        CHECK(fi[i].crc[0] != 0);
        if (i)
            CHECK(memcmp(fi[i].crc, fi[i - 1].crc, sizeof(unsigned) * c->crc_len) != 0);
    }

    /* CRC of frame i+1 is on frame i (first byte at L-3, stored backwards),
       never on the frame that produced it; the last frame stays zero */
    verify_chain(s.buf, offs, fi, nf, c->crc_len, c->ancillary_bits / 8);
    for (i = 1; i < nf; i++) {
        int L = fi[i].frame_len;
        const unsigned char *fr = s.buf + offs[i];
        unsigned char own[4];
        for (j = 0; j < c->crc_len; j++)
            own[j] = (unsigned char) fi[i].crc[c->crc_len - 1 - j];
        CHECK(memcmp(fr + L - c->crc_len - 2, own, c->crc_len) != 0);
    }

    /* identical to the original single-frame mode except at CRC positions */
    run_legacy_raw(c, l, r, NULL, nsamp, 0, SPF, &leg);
    compare_to_legacy(&s, &leg, c->crc_len);
}

static void case_crc(void)
{
    int before = g_asserts;
    cfg_t c2 = { 48000, 96, 2, 2, 8 * (2 + 2), 0, TWOLAME_PAD_ALL, 1 };
    cfg_t c4 = { 44100, 192, 2, 4, 8 * (4 + 2), 0, TWOLAME_PAD_ALL, 1 };
    crc_round(&c2, 7);
    crc_round(&c4, 11);
    CHECK(g_asserts > before);
    printf("DAB_STREAM_CASE crc PASS\n");
}


/* ------------------------------------------------------------------ */
/* case: chunking                                                      */
/* ------------------------------------------------------------------ */

static void case_chunking(void)
{
    int before = g_asserts;
    cfg_t c = { 48000, 96, 2, 2, 8 * (2 + 2), 0, TWOLAME_PAD_ALL, 1 };
    static short l[MAXSAMPS], r[MAXSAMPS], inter[2 * MAXSAMPS];
    int nsamp = 3 * SPF + 173;
    stream_t ref, cur;
    int i, pos, step, n, k;
    twolame_options *o;
    static unsigned char buf[ENC_CAP];

    gen_tone(l, r, nsamp, c.rate, 5);
    for (i = 0; i < nsamp; i++) {
        inter[2 * i] = l[i];
        inter[2 * i + 1] = r[i];
    }

    /* planar: bulk, frame-aligned and odd-sized calls */
    run_streaming(&c, l, r, NULL, nsamp, 0, 0, &ref);
    run_streaming(&c, l, r, NULL, nsamp, 0, 1, &cur);
    expect_same_stream(&ref, &cur);
    run_streaming(&c, l, r, NULL, nsamp, 0, 2, &cur);
    expect_same_stream(&ref, &cur);

    /* interleaved: same three patterns, same final stream */
    run_streaming(&c, NULL, NULL, inter, nsamp, 1, 0, &cur);
    expect_same_stream(&ref, &cur);
    run_streaming(&c, NULL, NULL, inter, nsamp, 1, 1, &cur);
    expect_same_stream(&ref, &cur);
    run_streaming(&c, NULL, NULL, inter, nsamp, 1, 2, &cur);
    expect_same_stream(&ref, &cur);

    /* zero-length calls before the flush produce nothing and change nothing */
    o = open_cfg(&c, 1);
    cur.len = 0;
    pos = 0;
    step = 0;
    while (pos < nsamp) {
        n = twolame_encode_buffer(o, l, r, 0, buf, sizeof(buf));
        CHECK(n == 0);
        k = (step % 3 == 0) ? 1 : ((step % 3 == 1) ? 37 : 2049);
        if (k > nsamp - pos)
            k = nsamp - pos;
        n = twolame_encode_buffer(o, l + pos, r + pos, k, buf, sizeof(buf));
        CHECK(n >= 0);
        sput(&cur, buf, n);
        pos += k;
        step++;
    }
    n = twolame_encode_buffer(o, l, r, 0, buf, sizeof(buf));
    CHECK(n == 0);
    n = twolame_encode_flush(o, buf, sizeof(buf));
    CHECK(n >= 0);
    sput(&cur, buf, n);
    twolame_close(&o);
    expect_same_stream(&ref, &cur);

    CHECK(g_asserts > before);
    printf("DAB_STREAM_CASE chunking PASS\n");
}


/* ------------------------------------------------------------------ */
/* case: tail                                                          */
/* ------------------------------------------------------------------ */

static void tail_round(const cfg_t *c, const short *l, const short *r, int nsamp,
                       int exp_enc_frames, int exp_flush_frames)
{
    static unsigned char buf[ENC_CAP];
    stream_t s;
    frame_info fi[MAXFRAMES];
    int offs[MAXFRAMES];
    twolame_options *o = open_cfg(c, 1);
    int n, nf, total;

    s.len = 0;

    /* one encode call with the whole input */
    n = twolame_encode_buffer(o, l, r, nsamp, buf, sizeof(buf));
    CHECK(n >= 0);
    CHECK(frames_in(buf, n) == exp_enc_frames);
    sput(&s, buf, n);

    /* first flush may hand over up to two frames */
    n = twolame_encode_flush(o, buf, sizeof(buf));
    CHECK(n >= 0);
    CHECK(frames_in(buf, n) == exp_flush_frames);
    sput(&s, buf, n);

    /* total frame count is exactly ceil(N/1152) */
    nf = scan_frames(s.buf, s.len, fi, offs, MAXFRAMES);
    total = (nsamp + SPF - 1) / SPF;
    CHECK(nf == total);

    if (nf > 0) {
        /* chain holds across the whole stream, final frame CRC is zero */
        verify_chain(s.buf, offs, fi, nf, c->crc_len, c->ancillary_bits / 8);
        if (exp_flush_frames == 2) {
            /* the first of the two flush frames was really backfilled */
            const unsigned char *fr = s.buf + offs[nf - 2];
            int L = fi[nf - 2].frame_len;
            unsigned acc = 0;
            int j;
            for (j = 0; j < c->crc_len; j++)
                acc |= fr[L - 3 - j];
            CHECK(acc != 0);
        }
    }

    /* the stream is finished: repeated flush and zero-length encode give 0,
       fresh PCM is rejected without producing output */
    n = twolame_encode_flush(o, buf, sizeof(buf));
    CHECK(n == 0);
    n = twolame_encode_buffer(o, l, r, 0, buf, sizeof(buf));
    CHECK(n == 0);
    n = twolame_encode_buffer(o, l, r, 37, buf, sizeof(buf));
    CHECK(n == -1);
    n = twolame_encode_flush(o, buf, sizeof(buf));
    CHECK(n == 0);

    twolame_close(&o);
}

static void case_tail(void)
{
    int before = g_asserts;
    cfg_t c = { 48000, 96, 2, 2, 8 * (2 + 2), 0, TWOLAME_PAD_ALL, 1 };
    static short l[MAXSAMPS], r[MAXSAMPS];

    gen_tone(l, r, MAXSAMPS, c.rate, 9);

    tail_round(&c, l, r, 0, 0, 0);      /* empty stream: no frames at all */
    tail_round(&c, l, r, 500, 0, 1);    /* less than one frame */
    tail_round(&c, l, r, SPF, 0, 1);    /* exactly one frame */
    tail_round(&c, l, r, 3 * SPF, 2, 1);        /* exactly three frames */
    tail_round(&c, l, r, 3 * SPF + 173, 2, 2);  /* three frames plus tail */

    CHECK(g_asserts > before);
    printf("DAB_STREAM_CASE tail PASS\n");
}


/* ------------------------------------------------------------------ */
/* case: layout                                                        */
/* ------------------------------------------------------------------ */

static void layout_round(const cfg_t *c, int nsamp, unsigned seed, int expect_two_lengths,
                         int expect_same_lengths)
{
    static short l[MAXSAMPS], r[MAXSAMPS];
    stream_t s, leg;
    frame_info fi[MAXFRAMES];
    int offs[MAXFRAMES];
    int nf, i;

    gen_tone(l, c->channels == 2 ? r : NULL, nsamp, c->rate, seed);
    run_streaming(c, l, r, NULL, nsamp, 0, 0, &s);
    nf = scan_frames(s.buf, s.len, fi, offs, MAXFRAMES);
    CHECK(nf == (nsamp + SPF - 1) / SPF);
    CHECK(nf > 0);

    /* sanity: parsed stream matches the requested layout */
    for (i = 0; i < nf; i++) {
        CHECK(fi[i].version == (c->rate < 32000 ? 0 : 1));
        CHECK(fi[i].bitrate == c->bitrate);
        CHECK(fi[i].samplerate == c->rate);
        CHECK(fi[i].nch == c->channels);
        CHECK(fi[i].error_protection == c->error_protection);
        if (c->padding == TWOLAME_PAD_NO)
            CHECK(fi[i].padding == 0);
    }
    if (expect_two_lengths) {
        int seen = 0;
        for (i = 1; i < nf; i++)
            if (fi[i].frame_len != fi[0].frame_len)
                seen = 1;
        CHECK(seen);
    }
    if (expect_same_lengths)
        for (i = 1; i < nf; i++)
            CHECK(fi[i].frame_len == fi[0].frame_len);

    verify_chain(s.buf, offs, fi, nf, c->crc_len, c->ancillary_bits / 8);

    run_legacy_raw(c, l, r, NULL, nsamp, 0, SPF, &leg);
    compare_to_legacy(&s, &leg, c->crc_len);
}

static void case_layout(void)
{
    int before = g_asserts;
    cfg_t A = { 48000, 96, 2, 2, 8 * (2 + 2), 0, TWOLAME_PAD_ALL, 1 };
    cfg_t B = { 44100, 192, 2, 4, 8 * (4 + 2), 0, TWOLAME_PAD_ALL, 1 };
    cfg_t C = { 24000, 96, 1, 4, 8 * (4 + 2), 0, TWOLAME_PAD_ALL, 1 };
    cfg_t A2 = { 48000, 96, 2, 2, 8 * (2 + 2), 1, TWOLAME_PAD_ALL, 1 };        /* MPEG CRC on */
    cfg_t B2 = { 44100, 192, 2, 4, 8 * (4 + 2), 0, TWOLAME_PAD_NO, 1 };        /* padding off */
    cfg_t A3 = { 48000, 96, 2, 2, 8 * (2 + 2) + 16, 0, TWOLAME_PAD_ALL, 1 };   /* extra reserve */

    layout_round(&A, 5 * SPF + 231, 3, 0, 0);
    layout_round(&B, 5 * SPF + 231, 17, 1, 0);  /* 44.1kHz: two frame lengths */
    layout_round(&C, 5 * SPF + 231, 23, 0, 0);  /* MPEG-2 mono */
    layout_round(&A2, 4 * SPF, 29, 0, 0);
    layout_round(&B2, 4 * SPF, 31, 0, 1);
    layout_round(&A3, 4 * SPF + 7, 37, 0, 0);

    CHECK(g_asserts > before);
    printf("DAB_STREAM_CASE layout PASS\n");
}


/* ------------------------------------------------------------------ */
/* case: isolation                                                     */
/* ------------------------------------------------------------------ */

static void case_isolation(void)
{
    int before = g_asserts;
    cfg_t A = { 48000, 96, 2, 2, 8 * (2 + 2), 0, TWOLAME_PAD_ALL, 1 };
    cfg_t B = { 24000, 96, 1, 4, 8 * (4 + 2), 0, TWOLAME_PAD_ALL, 1 };
    static short la[MAXSAMPS], ra[MAXSAMPS], lb[MAXSAMPS];
    int na = 5 * SPF + 100, nb = 4 * SPF + 50;
    stream_t refA, refB, outA, outB;
    static unsigned char buf[ENC_CAP];
    twolame_options *oA, *oB;
    frame_info fi[MAXFRAMES];
    int offs[MAXFRAMES];
    int pa = 0, pb = 0, emA = 0, emB = 0, n, k, nfA, nfB;

    gen_tone(la, ra, na, A.rate, 41);
    gen_tone(lb, NULL, nb, B.rate, 43);

    /* solo references */
    run_streaming(&A, la, ra, NULL, na, 0, 0, &refA);
    run_streaming(&B, lb, NULL, NULL, nb, 0, 0, &refB);

    /* interleaved feeding */
    oA = open_cfg(&A, 1);
    oB = open_cfg(&B, 1);
    outA.len = 0;
    outB.len = 0;
    while (pa < na || pb < nb) {
        if (pa < na) {
            k = 1000;
            if (k > na - pa)
                k = na - pa;
            n = twolame_encode_buffer(oA, la + pa, ra + pa, k, buf, sizeof(buf));
            CHECK(n >= 0);
            emA += frames_in(buf, n);
            sput(&outA, buf, n);
            pa += k;
            /* exactly one frame of latency, never more */
            if (pa / SPF > 0)
                CHECK(emA == pa / SPF - 1);
            else
                CHECK(emA == 0);
        }
        if (pb < nb) {
            k = 700;
            if (k > nb - pb)
                k = nb - pb;
            n = twolame_encode_buffer(oB, lb + pb, NULL, k, buf, sizeof(buf));
            CHECK(n >= 0);
            emB += frames_in(buf, n);
            sput(&outB, buf, n);
            pb += k;
            if (pb / SPF > 0)
                CHECK(emB == pb / SPF - 1);
            else
                CHECK(emB == 0);
        }
    }

    /* interleaved shutdown */
    n = twolame_encode_flush(oA, buf, sizeof(buf));
    CHECK(n >= 0);
    sput(&outA, buf, n);
    n = twolame_encode_flush(oB, buf, sizeof(buf));
    CHECK(n >= 0);
    sput(&outB, buf, n);
    n = twolame_encode_flush(oA, buf, sizeof(buf));
    CHECK(n == 0);
    n = twolame_encode_flush(oB, buf, sizeof(buf));
    CHECK(n == 0);

    twolame_close(&oA);
    twolame_close(&oB);

    /* identical to each instance running on its own */
    expect_same_stream(&outA, &refA);
    expect_same_stream(&outB, &refB);

    /* and both streams pass the independent CRC chain check */
    nfA = scan_frames(outA.buf, outA.len, fi, offs, MAXFRAMES);
    verify_chain(outA.buf, offs, fi, nfA, A.crc_len, A.ancillary_bits / 8);
    nfB = scan_frames(outB.buf, outB.len, fi, offs, MAXFRAMES);
    verify_chain(outB.buf, offs, fi, nfB, B.crc_len, B.ancillary_bits / 8);

    CHECK(g_crc_checks > 0);
    CHECK(g_asserts > before);
    printf("DAB_STREAM_ISOLATION crc_frames=%ld asserts=%d\n", g_crc_checks, g_asserts);
    printf("DAB_STREAM_CASE isolation PASS\n");
}


/* ------------------------------------------------------------------ */
/* case: legacy                                                        */
/* ------------------------------------------------------------------ */

/* build a valid streaming configuration, then break one thing */
static int init_with_break(int brk)
{
    twolame_options *o = twolame_init();
    int rc;
    if (!o)
        return -2;
    twolame_set_num_channels(o, 2);
    twolame_set_in_samplerate(o, 48000);
    twolame_set_out_samplerate(o, 48000);
    twolame_set_bitrate(o, 96);
    twolame_set_mode(o, TWOLAME_STEREO);
    twolame_set_verbosity(o, 0);
    twolame_set_padding(o, TWOLAME_PAD_ALL);
    twolame_set_DAB(o, 1);
    twolame_set_DAB_crc_length(o, 2);
    twolame_set_num_ancillary_bits(o, 8 * (2 + 2));
    twolame_set_DAB_streaming(o, 1);
    switch (brk) {
    case 0:
        break;                  /* valid control */
    case 1:
        twolame_set_DAB(o, 0);
        break;
    case 2:
        twolame_set_DAB_crc_length(o, 3);
        break;
    case 3:
        twolame_set_DAB_crc_length(o, 0);
        break;
    case 4:
        twolame_set_DAB_xpad_length(o, 2);
        break;
    case 5:
        twolame_set_VBR(o, 1);
        break;
    case 6:
        twolame_set_freeformat(o, 1);
        break;
    case 7:
        twolame_set_energy_levels(o, 1);
        break;
    case 8:
        twolame_set_mode(o, TWOLAME_JOINT_STEREO);
        break;
    case 9:
        twolame_set_mode(o, TWOLAME_DUAL_CHANNEL);
        break;
    case 10:
        twolame_set_num_channels(o, 1);
        break;                  /* stereo mode but 1 channel in */
    case 11:
        twolame_set_mode(o, TWOLAME_MONO);
        break;                  /* mono mode but 2 channels in */
    case 12:
        twolame_set_out_samplerate(o, 44100);
        break;
    case 13:
        twolame_set_num_ancillary_bits(o, 8 * (2 + 2) - 8);
        break;                  /* too small */
    case 14:
        twolame_set_num_ancillary_bits(o, 8 * (2 + 2) + 4);
        break;                  /* not a multiple of 8 */
    case 15:
        twolame_set_num_ancillary_bits(o, 0);
        break;                  /* nothing reserved */
    }
    rc = twolame_init_params(o);
    twolame_close(&o);
    return rc;
}

static void case_legacy(void)
{
    int before = g_asserts;
    cfg_t plain = { 48000, 192, 2, 2, 0, 0, TWOLAME_PAD_ALL, 0 };
    cfg_t dab = { 48000, 96, 2, 2, 8 * (2 + 2), 0, TWOLAME_PAD_ALL, 1 };
    static short l[MAXSAMPS], r[MAXSAMPS], inter[2 * MAXSAMPS];
    static float fl[MAXSAMPS], fr[MAXSAMPS];
    static unsigned char buf[ENC_CAP];
    static unsigned char frames[3][ENC_CAP / 4], raw[3][ENC_CAP / 4], bulk[ENC_CAP];
    unsigned char snap[3][4];
    stream_t r1, r2, r3, s, ref;
    int nsamp = 3 * SPF + 173;
    int i, n, len[3];
    twolame_options *o, *o2;

    gen_tone(l, r, nsamp, 48000, 51);
    for (i = 0; i < nsamp; i++) {
        inter[2 * i] = l[i];
        inter[2 * i + 1] = r[i];
        fl[i] = l[i] / 32768.0f;
        fr[i] = r[i] / 32768.0f;
    }

    /* 1. normal encoding with the new mode off: planar/interleaved/chunked
       all produce the same bytes */
    run_legacy_raw(&plain, l, r, NULL, nsamp, 0, SPF, &r1);
    run_legacy_raw(&plain, l, r, inter, nsamp, 1, SPF, &r2);
    run_legacy_raw(&plain, l, r, NULL, nsamp, 0, nsamp, &r3);
    expect_same_stream(&r1, &r2);
    expect_same_stream(&r1, &r3);

    /* 2. the original manual DAB interface is untouched */
    o = open_cfg(&dab, 0);
    for (i = 0; i < 3; i++) {
        unsigned char probe[16];
        len[i] = twolame_encode_buffer(o, l + i * SPF, r + i * SPF, SPF, frames[i],
                                       sizeof(frames[i]));
        CHECK(len[i] > 0);
        memcpy(raw[i], frames[i], len[i]);
        /* backfill the previous frame with this frame's CRC (manual flow) */
        if (i > 0)
            CHECK(!twolame_set_DAB_scf_crc(o, frames[i - 1], len[i - 1]));
        /* snapshot this frame's CRC through the original API */
        memset(probe, 0xA5, sizeof(probe));
        CHECK(!twolame_set_DAB_scf_crc(o, probe, sizeof(probe)));
        memcpy(snap[i], probe + 14 - dab.crc_len, dab.crc_len);
    }
    /* the backfilled bytes are exactly the successor's snapshot */
    for (i = 0; i + 1 < 3; i++) {
        CHECK(!memcmp(frames[i] + len[i] - dab.crc_len - 2, snap[i + 1], dab.crc_len));
        CHECK(frames[i][len[i] - 2] == 0 && frames[i][len[i] - 1] == 0);
    }
    /* bulk encoding still equals the concatenation of single frames */
    o2 = open_cfg(&dab, 0);
    n = twolame_encode_buffer(o2, l, r, 3 * SPF, bulk, sizeof(bulk));
    CHECK(n == len[0] + len[1] + len[2]);
    {
        int off = 0;
        for (i = 0; i < 3; i++) {
            CHECK(!memcmp(raw[i], bulk + off, len[i]));
            off += len[i];
        }
    }
    CHECK(twolame_encode_flush(o, buf, sizeof(buf)) == 0);
    CHECK(twolame_encode_flush(o2, buf, sizeof(buf)) == 0);
    twolame_close(&o);
    twolame_close(&o2);

    /* 3. initialisation restrictions */
    CHECK(init_with_break(0) == 0);
    for (i = 1; i <= 15; i++)
        CHECK(init_with_break(i) == -1);

    /* 4. setter restrictions: only 0/1, locked after successful init */
    o = twolame_init();
    CHECK(o != NULL);
    CHECK(twolame_get_DAB_streaming(o) == 0);
    CHECK(twolame_set_DAB_streaming(o, 2) == -1);
    CHECK(twolame_get_DAB_streaming(o) == 0);
    CHECK(twolame_set_DAB_streaming(o, -1) == -1);
    CHECK(twolame_get_DAB_streaming(o) == 0);
    CHECK(twolame_set_DAB_streaming(o, 1) == 0);
    CHECK(twolame_get_DAB_streaming(o) == 1);
    CHECK(twolame_set_DAB_streaming(o, 0) == 0);
    CHECK(twolame_get_DAB_streaming(o) == 0);
    CHECK(twolame_set_DAB_streaming(o, 1) == 0);
    CHECK(twolame_get_DAB_streaming(o) == 1);
    CHECK(!twolame_set_num_channels(o, 2));
    CHECK(!twolame_set_in_samplerate(o, 48000));
    CHECK(!twolame_set_out_samplerate(o, 48000));
    CHECK(!twolame_set_bitrate(o, 96));
    CHECK(!twolame_set_mode(o, TWOLAME_STEREO));
    CHECK(!twolame_set_verbosity(o, 0));
    CHECK(!twolame_set_DAB(o, 1));
    CHECK(!twolame_set_DAB_crc_length(o, 2));
    CHECK(!twolame_set_num_ancillary_bits(o, 8 * (2 + 2)));
    CHECK(!twolame_init_params(o));
    CHECK(twolame_set_DAB_streaming(o, 0) == -1);
    CHECK(twolame_get_DAB_streaming(o) == 1);
    CHECK(twolame_set_DAB_streaming(o, 1) == -1);
    CHECK(twolame_get_DAB_streaming(o) == 1);
    CHECK(twolame_set_DAB_streaming(o, 2) == -1);
    CHECK(twolame_get_DAB_streaming(o) == 1);
    twolame_close(&o);

    /* 5. float32 entry points are rejected in streaming mode and leave the
       encoder state untouched: the normal output is unchanged afterwards */
    run_streaming(&dab, l, r, NULL, nsamp, 0, 0, &ref);
    o = open_cfg(&dab, 1);
    s.len = 0;
    n = twolame_encode_buffer_float32(o, fl, fr, 100, buf, sizeof(buf));
    CHECK(n == -1);
    n = twolame_encode_buffer_float32_interleaved(o, fl, 100, buf, sizeof(buf));
    CHECK(n == -1);
    n = twolame_encode_buffer(o, l, r, SPF, buf, sizeof(buf));
    CHECK(n == 0);              /* first frame is held back */
    n = twolame_encode_buffer_float32(o, fl, fr, 100, buf, sizeof(buf));
    CHECK(n == -1);
    n = twolame_encode_buffer(o, l + SPF, r + SPF, nsamp - SPF, buf, sizeof(buf));
    CHECK(n >= 0);
    sput(&s, buf, n);
    n = twolame_encode_flush(o, buf, sizeof(buf));
    CHECK(n >= 0);
    sput(&s, buf, n);
    twolame_close(&o);
    expect_same_stream(&s, &ref);

    /* float32 encoding itself still works with the mode off */
    o = open_cfg(&dab, 0);
    n = twolame_encode_buffer_float32(o, fl, fr, SPF, buf, sizeof(buf));
    CHECK(n > 0);
    twolame_close(&o);

    CHECK(g_asserts > before);
    printf("DAB_STREAM_CASE legacy PASS\n");
}


int main(void)
{
    case_crc();
    case_chunking();
    case_tail();
    case_layout();
    case_isolation();
    case_legacy();
    printf("DAB_STREAM_OK cases=6\n");
    return 0;
}
