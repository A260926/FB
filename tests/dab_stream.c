/*
 * Tests for library-managed DAB ScF-CRC streaming.
 *
 * The frame parser and ScF-CRC calculation below are written from the table
 * definitions in the MPEG Layer II specification (the same tables the encoder
 * uses).  Expected CRCs are never obtained by calling the library's own DAB
 * routines; they are recomputed from the encoded bit stream.
 */

#include "twolame.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S 1152
#define MAXBUF (1 << 21)
#define FMAX 128

static int g_checks;
static int g_crc_frames;

static void fail(const char *file, int line, const char *expr)
{
    fprintf(stderr, "DAB_STREAM_FAIL %s:%d: %s\n", file, line, expr);
    exit(2);
}

#define CHECK(x) do { g_checks++; if (!(x)) fail(__FILE__, __LINE__, #x); } while (0)

/* ------------------------------------------------------------------ */
/* Frame parsing                                                       */

static const int bitrate_table[2][15] = {
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
    {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384}
};

static const int mpeg1_rates[4] = { 44100, 48000, 32000, 0 };
static const int mpeg2_rates[4] = { 22050, 24000, 16000, 0 };

/* Layer II allocation tables (ISO 11172 / 13818), copied to let the test
   decode bit allocation without touching encoder internals. */
static const int nbal[9] = { 4, 4, 3, 2, 4, 3, 4, 3, 2 };

static const int line_table[5][32] = {
    { 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3,
      -1, -1, -1, -1, -1 },
    { 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3,
      3, 3, -1, -1 },
    { 4, 4, 5, 5, 5, 5, 5, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    { 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1 },
    { 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
      8, 8, 8, 8 }
};

static const int table_sblimit[5] = { 27, 30, 8, 12, 30 };
static const int js_bound_table[4] = { 4, 8, 12, 16 };

typedef struct {
    int off;
    int frame_len;
    int nch;
    int version;
    int mode;
    int mode_ext;
    int prot;                   /* 16-bit header CRC present */
    int crc_end_bit;            /* bit offset of header CRC coverage end */
    int br_index;
    int sr_index;
    int padding;
    int bitrate;
    int rate;
    int tablenum;
    int sblimit;
    int jsbound;
    unsigned int ba[2][32];
    unsigned int scfsi[2][32];
    unsigned int sf[2][3][32];
    int header_crc_ok;          /* -1 when no header CRC */
} PARSED;

typedef struct {
    const unsigned char *p;
    int bitpos;
} BR;

static unsigned br_get(BR * br, int n)
{
    unsigned v = 0;
    while (n-- > 0) {
        v = (v << 1) | ((br->p[br->bitpos >> 3] >> (7 - (br->bitpos & 7))) & 1);
        br->bitpos++;
    }
    return v;
}

static unsigned crc16_update(unsigned value, unsigned crc, unsigned nbits)
{
    unsigned i;
    value <<= 8;
    for (i = 0; i < nbits; i++) {
        value <<= 1;
        crc <<= 1;
        if ((crc ^ value) & 0x10000)
            crc ^= 0x8005;
    }
    return crc;
}

/* Pick the allocation table exactly like the encoder does. */
static int pick_tablenum(int version, int rate, int bitrate, int nch, int freeformat)
{
    int br_per_ch = bitrate / nch;
    int sfrq = rate / 1000;

    if (version == TWOLAME_MPEG1) {
        if (freeformat) {
            return (sfrq == 48) ? 0 : 1;
        }
        if ((sfrq == 48 && br_per_ch >= 56) || (br_per_ch >= 56 && br_per_ch <= 80))
            return 0;
        else if (sfrq != 48 && br_per_ch >= 96)
            return 1;
        else if (sfrq != 32 && br_per_ch <= 48)
            return 2;
        else
            return 3;
    }
    return 4;
}

/* Parse one complete MP2 frame.  Returns 0 on success. */
static int parse_frame(const unsigned char *buf, int avail, PARSED * p)
{
    BR br;
    int sb, ch, gr, hdr_end, nch_alloc;

    memset(p, 0, sizeof(*p));
    if (avail < 4)
        return -1;
    if (buf[0] != 0xff || (buf[1] & 0xe0) != 0xe0)
        return -1;

    p->version = (buf[1] >> 3) & 1;
    if (((buf[1] >> 1) & 3) != 2)       /* Layer II */
        return -1;
    p->prot = ((buf[1] & 1) == 0);
    p->br_index = (buf[2] >> 4) & 15;
    p->sr_index = (buf[2] >> 2) & 3;
    p->padding = (buf[2] >> 1) & 1;
    p->mode = (buf[3] >> 6) & 3;
    p->mode_ext = (buf[3] >> 4) & 3;

    if (p->br_index == 0 || p->br_index == 15)
        return -1;
    if (p->sr_index == 3)
        return -1;
    p->bitrate = bitrate_table[p->version][p->br_index];
    p->rate = (p->version == TWOLAME_MPEG1) ? mpeg1_rates[p->sr_index]
             : mpeg2_rates[p->sr_index];
    if (p->rate == 0)
        return -1;
    p->frame_len = 144 * p->bitrate * 1000 / p->rate + p->padding;
    p->nch = (p->mode == TWOLAME_MONO) ? 1 : 2;
    p->tablenum = pick_tablenum(p->version, p->rate, p->bitrate, p->nch, 0);
    p->sblimit = table_sblimit[p->tablenum];
    p->jsbound = (p->mode == TWOLAME_JOINT_STEREO) ? js_bound_table[p->mode_ext]
                 : p->sblimit;

    hdr_end = 4 + (p->prot ? 2 : 0);
    if (p->frame_len > avail || p->frame_len < hdr_end)
        return -1;

    br.p = buf;
    br.bitpos = hdr_end * 8;

    /* bit allocation */
    for (sb = 0; sb < p->sblimit; sb++) {
        int nb = nbal[line_table[p->tablenum][sb]];
        nch_alloc = (sb < p->jsbound) ? p->nch : 1;
        for (ch = 0; ch < nch_alloc; ch++)
            p->ba[ch][sb] = br_get(&br, nb);
        for (ch = nch_alloc; ch < p->nch; ch++)
            p->ba[ch][sb] = p->ba[0][sb];
    }

    /* scfsi, then transmitted scalefactors */
    for (sb = 0; sb < p->sblimit; sb++)
        for (ch = 0; ch < p->nch; ch++)
            if (p->ba[ch][sb])
                p->scfsi[ch][sb] = br_get(&br, 2);

    /* The MPEG header CRC (when present) protects the header and
       everything up to and including the scfsi bits. */
    {
        p->crc_end_bit = br.bitpos;
    }

    for (sb = 0; sb < p->sblimit; sb++)
        for (ch = 0; ch < p->nch; ch++)
            if (p->ba[ch][sb]) {
                switch (p->scfsi[ch][sb]) {
                case 0:
                    for (gr = 0; gr < 3; gr++)
                        p->sf[ch][gr][sb] = br_get(&br, 6);
                    break;
                case 1:
                case 3:
                    p->sf[ch][0][sb] = br_get(&br, 6);
                    p->sf[ch][2][sb] = br_get(&br, 6);
                    p->sf[ch][1][sb] = p->sf[ch][0][sb];
                    break;
                case 2:
                default:
                    p->sf[ch][0][sb] = p->sf[ch][1][sb] = p->sf[ch][2][sb] =
                        br_get(&br, 6);
                    break;
                }
            }

    /* independent MPEG header CRC check */
    if (p->prot) {
        int total_bits = p->crc_end_bit - 48;
        int whole = total_bits >> 3;
        int rem = total_bits & 7;
        unsigned crc = 0xffff;
        int byte;

        crc = crc16_update(buf[2], crc, 8);
        crc = crc16_update(buf[3], crc, 8);
        for (byte = 6; byte < 6 + whole; byte++)
            crc = crc16_update(buf[byte], crc, 8);
        if (rem)
            crc = crc16_update(buf[6 + whole], crc, (unsigned) rem);
        crc &= 0xffff;
        p->header_crc_ok = ((buf[4] == (crc >> 8)) && (buf[5] == (crc & 0xff)));
    } else {
        p->header_crc_ok = -1;
    }

    return 0;
}

static int split_frames(const unsigned char *s, int total, PARSED * ps, int max)
{
    int off = 0, nf = 0;

    while (off < total) {
        CHECK(nf < max);
        CHECK(parse_frame(s + off, total - off, &ps[nf]) == 0);
        ps[nf].off = off;
        off += ps[nf].frame_len;
        nf++;
    }
    CHECK(off == total);
    return nf;
}

/* ------------------------------------------------------------------ */
/* Independent DAB ScF-CRC                                             */

static void dab_update(unsigned int data, unsigned int length, unsigned int *crc)
{
    unsigned int masking, carry;

    masking = 1u << length;
    while ((masking >>= 1) != 0) {
        carry = *crc & 0x80u;
        *crc <<= 1;
        if ((!carry) ^ (!(data & masking)))
            *crc ^= 0x1Du;
    }
    *crc &= 0xffu;
}

static unsigned int dab_band_crc(const PARSED * p, int band)
{
    static const int f[5] = { 0, 4, 8, 16, 30 };
    int first = f[band];
    int last = f[band + 1];
    int i, k, j;
    unsigned int crc = 0;

    if (last > p->sblimit)
        last = p->sblimit;
    for (i = first; i < last; i++)
        for (k = 0; k < p->nch; k++)
            if (p->ba[k][i])
                switch (p->scfsi[k][i]) {
                case 0:
                    for (j = 0; j < 3; j++)
                        dab_update(p->sf[k][j][i] >> 3, 3, &crc);
                    break;
                case 1:
                case 3:
                    dab_update(p->sf[k][0][i] >> 3, 3, &crc);
                    dab_update(p->sf[k][2][i] >> 3, 3, &crc);
                    break;
                case 2:
                    dab_update(p->sf[k][0][i] >> 3, 3, &crc);
                    break;
                }
    return crc;
}

static void dab_crc_bytes(const PARSED * p, int crc_len, unsigned char out[4])
{
    int i;

    for (i = crc_len - 1; i >= 0; i--)
        out[i] = (unsigned char) (dab_band_crc(p, i) & 0xff);
}

/* Verify that every frame carries its successor's CRC in its own tail, in
   reverse byte order, and that the final frame's CRC field is zero. */
static int verify_chain(const unsigned char *s, int total, int crc_len)
{
    static PARSED ps[FMAX];
    int nf = split_frames(s, total, ps, FMAX);
    int i, k;
    int self_hit = 0;
    int order_hit = 0;
    unsigned char last[4] = { 0, 0, 0, 0 };

    for (i = 0; i < nf - 1; i++) {
        unsigned char exp[4];
        unsigned char own[4];
        int len = ps[i].frame_len;
        int nonzero = 0;

        dab_crc_bytes(&ps[i + 1], crc_len, exp);
        for (k = 0; k < crc_len; k++) {
            CHECK(s[ps[i].off + len - 3 - k] == exp[k]);
            nonzero |= exp[k];
        }
        CHECK(nonzero != 0);
        if (i > 0)
            CHECK(memcmp(exp, last, (size_t) crc_len) != 0);
        memcpy(last, exp, (size_t) crc_len);

        /* the tail two bytes are the untouched F-PAD */
        CHECK(s[ps[i].off + len - 2] == 0);
        CHECK(s[ps[i].off + len - 1] == 0);

        /* it must be the successor's CRC, not the frame's own */
        dab_crc_bytes(&ps[i], crc_len, own);
        if (memcmp(own, exp, (size_t) crc_len) != 0)
            self_hit++;

        /* prove the byte order is distinguishable for the 4-byte field */
        for (k = 1; k < crc_len; k++)
            if (exp[k] != exp[0])
                order_hit++;

        g_crc_frames++;
    }

    if (nf >= 1) {
        int len = ps[nf - 1].frame_len;
        for (k = 0; k < crc_len; k++)
            CHECK(s[ps[nf - 1].off + len - 3 - k] == 0);
        CHECK(s[ps[nf - 1].off + len - 2] == 0);
        CHECK(s[ps[nf - 1].off + len - 1] == 0);
    }
    if (nf >= 2)
        CHECK(self_hit > 0);
    if (nf >= 3 && crc_len >= 2)
        CHECK(order_hit > 0);
    return nf;
}

/* ------------------------------------------------------------------ */
/* Encoder helpers                                                     */

typedef struct {
    int rate;
    int bitrate;
    int channels;               /* 1 or 2 */
    int mode;                   /* TWOLAME_MONO / TWOLAME_STEREO */
    int crc_len;
    int padding;                /* TWOLAME_Padding */
    int protection;
    int extra_bytes;            /* extra reserved ancillary bytes */
} CFG;

static void gen_pcm(int seed, int n, short *l, short *r)
{
    uint32_t x = (uint32_t) seed * 2654435761u + 1u;
    int i;

    for (i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        l[i] = (short) ((int) ((x >> 13) & 0x3ffff) - 131072) / 4;
        x = x * 1664525u + 1013904223u;
        r[i] = (short) ((int) ((x >> 13) & 0x3ffff) - 131072) / 6;
    }
}

static twolame_options *open_enc(const CFG * c, int streaming, int *ok)
{
    twolame_options *o = twolame_init();
    int rc;

    *ok = 0;
    CHECK(o != NULL);
    CHECK(twolame_set_num_channels(o, c->channels) == 0);
    CHECK(twolame_set_in_samplerate(o, c->rate) == 0);
    CHECK(twolame_set_out_samplerate(o, c->rate) == 0);
    CHECK(twolame_set_bitrate(o, c->bitrate) == 0);
    CHECK(twolame_set_mode(o, c->mode) == 0);
    CHECK(twolame_set_verbosity(o, 0) == 0);
    CHECK(twolame_set_padding(o, c->padding) == 0);
    CHECK(twolame_set_error_protection(o, c->protection) == 0);
    CHECK(twolame_set_DAB(o, 1) == 0);
    CHECK(twolame_set_DAB_crc_length(o, c->crc_len) == 0);
    CHECK(twolame_set_num_ancillary_bits(o, 8 * (c->crc_len + 2 + c->extra_bytes)) == 0);
    CHECK(twolame_set_DAB_streaming(o, streaming) == 0);
    rc = twolame_init_params(o);
    if (rc != 0) {
        twolame_close(&o);
        return NULL;
    }
    *ok = 1;
    return o;
}

/* Run the streaming encoder over N samples with a chunking plan:
   0 = one block, 1 = 1152 at a time, 2 = alternating 1/37/2049. */
static int run_stream(const CFG * c, const short *l, const short *r, int N,
                      int interleaved, int plan, unsigned char *out, int cap)
{
    static const int pattern[3] = { 1, 37, 2049 };
    short *inter = NULL;
    unsigned char *scratch;
    size_t scrcap;
    twolame_options *o;
    int ok, pos = 0, total = 0, pi = 0;

    o = open_enc(c, 1, &ok);
    CHECK(ok);
    if (interleaved) {
        int i;
        inter = (short *) malloc(sizeof(short) * 2 * (size_t) N);
        CHECK(inter != NULL);
        for (i = 0; i < N; i++) {
            inter[2 * i] = l[i];
            inter[2 * i + 1] = r[i];
        }
    }

    /* one reusable output buffer: the caller copies each result out and then
       clobbers the buffer before the next call */
    scrcap = 7200 + 2 * (size_t) N;
    scratch = (unsigned char *) malloc(scrcap);
    CHECK(scratch != NULL);

    while (pos < N) {
        int k;
        int ret;
        size_t mcap;

        if (plan == 0)
            k = N - pos;
        else if (plan == 1)
            k = (N - pos < S) ? N - pos : S;
        else
            k = pattern[pi++ % 3];
        if (k > N - pos)
            k = N - pos;

        mcap = 7200 + 2 * (size_t) k;
        CHECK(mcap <= scrcap);
        memset(scratch, 0xA5, mcap);
        if (interleaved)
            ret = twolame_encode_buffer_interleaved(o, inter + 2 * pos, k, scratch,
                                                    (int) mcap);
        else
            ret = twolame_encode_buffer(o, l + pos, r + pos, k, scratch, (int) mcap);
        CHECK(ret >= 0);
        CHECK((size_t) ret <= mcap);
        CHECK(total + ret <= cap);
        memcpy(out + total, scratch, (size_t) ret);
        total += ret;
        memset(scratch, 0x5A, mcap);    /* caller is free to reuse its buffer */
        pos += k;
    }

    {
        int ret;
        unsigned char ftmp[7200];

        memset(ftmp, 0xA5, sizeof(ftmp));
        ret = twolame_encode_flush(o, ftmp, sizeof(ftmp));
        CHECK(ret >= 0);
        CHECK(ret <= (int) sizeof(ftmp));
        CHECK(total + ret <= cap);
        memcpy(out + total, ftmp, (size_t) ret);
        total += ret;
        memset(ftmp, 0x5A, sizeof(ftmp));
    }
    {
        unsigned char ftmp[7200];
        memset(ftmp, 0xA5, sizeof(ftmp));
        CHECK(twolame_encode_flush(o, ftmp, sizeof(ftmp)) == 0);
    }

    twolame_close(&o);
    free(inter);
    free(scratch);
    return total;
}

/* Per-1152 legacy encoder with the documented manual ScF-CRC patching. */
static int build_legacy(const CFG * c, const short *l, const short *r, int N,
                        unsigned char *out, int cap)
{
    twolame_options *o;
    int ok, pos = 0, total = 0, prev_off = -1, prev_len = 0;

    o = open_enc(c, 0, &ok);
    CHECK(ok);
    while (pos < N) {
        int k = (N - pos < S) ? N - pos : S;
        int n = twolame_encode_buffer(o, l + pos, r + pos, k, out + total, cap - total);

        CHECK(n >= 0);
        if (prev_off >= 0)
            CHECK(twolame_set_DAB_scf_crc(o, out + prev_off, prev_len) == 0);
        prev_off = total;
        prev_len = n;
        total += n;
        pos += k;
    }
    {
        int n = twolame_encode_flush(o, out + total, cap - total);

        CHECK(n >= 0);
        if (n > 0) {
            if (prev_off >= 0)
                CHECK(twolame_set_DAB_scf_crc(o, out + prev_off, prev_len) == 0);
            total += n;
        }
    }
    twolame_close(&o);
    return total;
}

/* Frame-by-frame byte comparison.  With the new mode disabled and the same
   parameters the two streams must be identical except in the ScF-CRC slots
   that the streaming mode is responsible for filling in. */
static void compare_except_crc(const unsigned char *a, int alen,
                               const unsigned char *b, int blen, int crc_len)
{
    static PARSED pa[FMAX], pb[FMAX];
    int na = split_frames(a, alen, pa, FMAX);
    int nb = split_frames(b, blen, pb, FMAX);
    int i, k, byte;

    CHECK(na == nb);
    for (i = 0; i < na; i++) {
        int len = pa[i].frame_len;

        CHECK(pb[i].frame_len == len);
        for (byte = 0; byte < len; byte++) {
            int in_crc = 0;
            for (k = 0; k < crc_len; k++)
                if (byte == len - 3 - k)
                    in_crc = 1;
            if (!in_crc)
                CHECK(a[pa[i].off + byte] == b[pb[i].off + byte]);
        }
    }
}

/* Exact comparison, used against a reference that performed the same manual
   ScF-CRC patching the old API documents. */
static void compare_exact(const unsigned char *a, int alen,
                          const unsigned char *b, int blen)
{
    static PARSED pa[FMAX], pb[FMAX];
    int na = split_frames(a, alen, pa, FMAX);
    int nb = split_frames(b, blen, pb, FMAX);
    int i, byte;

    CHECK(na == nb);
    for (i = 0; i < na; i++) {
        int len = pa[i].frame_len;

        CHECK(pb[i].frame_len == len);
        for (byte = 0; byte < len; byte++)
            CHECK(a[pa[i].off + byte] == b[pb[i].off + byte]);
    }
}

/* ------------------------------------------------------------------ */
/* Cases                                                               */

static void test_crc(void)
{
    static const CFG cfgs[] = {
        {48000, 96, 2, TWOLAME_STEREO, 2, TWOLAME_PAD_ALL, 0, 0},
        {44100, 192, 2, TWOLAME_STEREO, 4, TWOLAME_PAD_ALL, 1, 0},
    };
    short *l = (short *) malloc(sizeof(short) * 6 * S);
    short *r = (short *) malloc(sizeof(short) * 6 * S);
    unsigned char *stream = (unsigned char *) malloc(MAXBUF);
    int ci, i, total, nf;

    CHECK(l && r && stream);
    for (ci = 0; ci < (int) (sizeof(cfgs) / sizeof(cfgs[0])); ci++) {
        int N = 6 * S;
        static PARSED ps[FMAX];

        gen_pcm(0x100 + ci, N, l, r);
        total = run_stream(&cfgs[ci], l, r, N, 0, 0, stream, MAXBUF);
        g_crc_frames = 0;
        nf = verify_chain(stream, total, cfgs[ci].crc_len);
        CHECK(nf == 6);
        CHECK(g_crc_frames == 5);
        CHECK(split_frames(stream, total, ps, FMAX) == nf);

        /* every frame's tail must be distinct non-zero CRC data */
        for (i = 0; i < nf - 1; i++) {
            int k, nonzero = 0;
            unsigned char exp[4];

            dab_crc_bytes(&ps[i + 1], cfgs[ci].crc_len, exp);
            for (k = 0; k < cfgs[ci].crc_len; k++)
                nonzero |= exp[k];
            CHECK(nonzero != 0);
        }
    }
    /* a 2-byte and a 4-byte configuration were both exercised */
    CHECK(g_crc_frames > 0);
    free(l);
    free(r);
    free(stream);
    printf("DAB_STREAM_CASE crc PASS\n");
}

static void test_chunking(void)
{
    const CFG cfg = { 48000, 96, 2, TWOLAME_STEREO, 2, TWOLAME_PAD_ALL, 0, 0 };
    int N = 3 * S + 500;
    short *l = (short *) malloc(sizeof(short) * N);
    short *r = (short *) malloc(sizeof(short) * N);
    unsigned char *ref = (unsigned char *) malloc(MAXBUF);
    unsigned char *tmp = (unsigned char *) malloc(MAXBUF);
    int reflen, i, j;

    CHECK(l && r && ref && tmp);
    gen_pcm(0x200, N, l, r);
    reflen = run_stream(&cfg, l, r, N, 0, 0, ref, MAXBUF);
    CHECK(verify_chain(ref, reflen, cfg.crc_len) == 4);

    for (i = 0; i < 2; i++) {   /* interleaved = 0 then 1 */
        for (j = 0; j < 3; j++) {       /* whole / 1152 / alternating */
            int len = run_stream(&cfg, l, r, N, i, j, tmp, MAXBUF);

            CHECK(len == reflen);
            CHECK(memcmp(ref, tmp, (size_t) reflen) == 0);
        }
    }
    free(l);
    free(r);
    free(ref);
    free(tmp);
    printf("DAB_STREAM_CASE chunking PASS\n");
}

/* Count how many complete frames the parse yields. */
static int count_frames(const unsigned char *s, int total)
{
    static PARSED ps[FMAX];
    return split_frames(s, total, ps, FMAX);
}

static void test_tail(void)
{
    const CFG cfg = { 24000, 96, 1, TWOLAME_MONO, 4, TWOLAME_PAD_NO, 0, 0 };
    int NMAX = 4 * S + 173;
    short *l = (short *) malloc(sizeof(short) * NMAX);
    short *r = (short *) malloc(sizeof(short) * NMAX);
    unsigned char *a = (unsigned char *) malloc(MAXBUF);
    unsigned char *b = (unsigned char *) malloc(MAXBUF);
    twolame_options *o;
    int ok, n, total;

    CHECK(l && r && a && b);
    gen_pcm(0x300, NMAX, l, r);

    /* empty stream: no frames at all, and repeatable end */
    o = open_enc(&cfg, 1, &ok);
    CHECK(ok);
    CHECK(twolame_encode_buffer(o, l, r, 0, a, MAXBUF) == 0);
    CHECK(twolame_encode_buffer(o, l, r, 0, a, MAXBUF) == 0);
    CHECK(twolame_encode_flush(o, a, MAXBUF) == 0);
    CHECK(twolame_encode_flush(o, a, MAXBUF) == 0);
    CHECK(twolame_encode_buffer(o, l, r, 0, a, MAXBUF) == 0);
    CHECK(twolame_encode_buffer(o, l, r, S, a, MAXBUF) == -1);
    CHECK(twolame_encode_buffer_interleaved(o, l, S, a, MAXBUF) == -1);
    twolame_close(&o);

    /* less than one frame: one zero-padded frame at flush, zero CRC */
    o = open_enc(&cfg, 1, &ok);
    CHECK(ok);
    n = twolame_encode_buffer(o, l, r, 700, a, MAXBUF);
    CHECK(n == 0);
    total = n;
    n = twolame_encode_flush(o, a + total, MAXBUF - total);
    CHECK(n > 0);
    total += n;
    CHECK(count_frames(a, total) == 1);
    {
        PARSED p;
        int k;

        CHECK(parse_frame(a, total, &p) == 0);
        for (k = 0; k < cfg.crc_len; k++)
            CHECK(a[p.frame_len - 3 - k] == 0);
    }
    CHECK(twolame_encode_flush(o, b, MAXBUF) == 0);
    twolame_close(&o);

    /* exactly one frame: held, then emitted patched=zero by flush */
    o = open_enc(&cfg, 1, &ok);
    CHECK(ok);
    n = twolame_encode_buffer(o, l, r, S, a, MAXBUF);
    CHECK(n == 0);
    n = twolame_encode_flush(o, a, MAXBUF);
    CHECK(n > 0);
    CHECK(count_frames(a, n) == 1);
    twolame_close(&o);

    /* exactly three frames: two emitted during encode, one at flush */
    o = open_enc(&cfg, 1, &ok);
    CHECK(ok);
    n = twolame_encode_buffer(o, l, r, 3 * S, a, MAXBUF);
    CHECK(n > 0);
    CHECK(count_frames(a, n) == 2);
    total = n;
    n = twolame_encode_flush(o, a + total, MAXBUF - total);
    CHECK(n > 0);
    total += n;
    CHECK(count_frames(a, total) == 3);
    twolame_close(&o);

    /* three frames + 173: flush hands back two frames at once */
    o = open_enc(&cfg, 1, &ok);
    CHECK(ok);
    n = twolame_encode_buffer(o, l, r, 3 * S + 173, a, MAXBUF);
    CHECK(n > 0);
    CHECK(count_frames(a, n) == 2);
    total = n;
    n = twolame_encode_flush(o, a + total, MAXBUF - total);
    CHECK(n > 0);
    CHECK(count_frames(a + total, n) == 2);
    total += n;
    CHECK(count_frames(a, total) == 4);
    CHECK(total == 4 * 576);    /* zero-padded tail frame is a real frame */
    verify_chain(a, total, cfg.crc_len);
    CHECK(twolame_encode_flush(o, b, MAXBUF) == 0);
    CHECK(twolame_encode_buffer(o, l, r, 0, b, MAXBUF) == 0);
    CHECK(twolame_encode_buffer(o, l, r, 100, b, MAXBUF) == -1);
    twolame_close(&o);

    /* zero-length encodes before the end must not advance anything */
    {
        int totalA, totalB;

        o = open_enc(&cfg, 1, &ok);
        CHECK(ok);
        totalA = twolame_encode_buffer(o, l, r, S, a, MAXBUF);
        CHECK(twolame_encode_buffer(o, l, r, 0, a + totalA, MAXBUF) == 0);
        totalA += twolame_encode_buffer(o, l + S, r + S, S, a + totalA, MAXBUF - totalA);
        totalA += twolame_encode_buffer(o, l + 2 * S, r + 2 * S, S, a + totalA,
                                        MAXBUF - totalA);
        totalA += twolame_encode_flush(o, a + totalA, MAXBUF - totalA);
        twolame_close(&o);

        totalB = run_stream(&cfg, l, r, 3 * S, 0, 1, b, MAXBUF);
        CHECK(totalA == totalB);
        CHECK(memcmp(a, b, (size_t) totalB) == 0);
    }

    free(l);
    free(r);
    free(a);
    free(b);
    printf("DAB_STREAM_CASE tail PASS\n");
}

static void test_layout(void)
{
    static const CFG cfgs[] = {
        {48000, 96, 2, TWOLAME_STEREO, 2, TWOLAME_PAD_ALL, 0, 0},
        {48000, 96, 2, TWOLAME_STEREO, 2, TWOLAME_PAD_ALL, 1, 0},
        {44100, 192, 2, TWOLAME_STEREO, 4, TWOLAME_PAD_ALL, 1, 0},
        {44100, 192, 2, TWOLAME_STEREO, 4, TWOLAME_PAD_NO, 0, 0},
        {24000, 96, 1, TWOLAME_MONO, 4, TWOLAME_PAD_NO, 0, 0},
        {48000, 96, 2, TWOLAME_STEREO, 2, TWOLAME_PAD_ALL, 0, 3},
    };
    int N = 5 * S + 300;
    short *l = (short *) malloc(sizeof(short) * N);
    short *r = (short *) malloc(sizeof(short) * N);
    unsigned char *stream = (unsigned char *) malloc(MAXBUF);
    unsigned char *legacy = (unsigned char *) malloc(MAXBUF);
    int ci;

    CHECK(l && r && stream && legacy);
    gen_pcm(0x400, N, l, r);

    for (ci = 0; ci < (int) (sizeof(cfgs) / sizeof(cfgs[0])); ci++) {
        const CFG *c = &cfgs[ci];
        static PARSED ps[FMAX];
        int total, nf, i, k;
        int saw_626 = 0, saw_627 = 0, saw_576 = 0;

        total = run_stream(c, l, r, N, 0, 0, stream, MAXBUF);
        nf = verify_chain(stream, total, c->crc_len);
        CHECK(nf == 6);
        CHECK(split_frames(stream, total, ps, FMAX) == nf);

        for (i = 0; i < nf; i++) {
            int base = 144 * ps[i].bitrate * 1000 / ps[i].rate;

            CHECK(ps[i].frame_len == base + ps[i].padding);
            if (c->protection)
                CHECK(ps[i].header_crc_ok == 1);
            else
                CHECK(ps[i].header_crc_ok == -1);
            if (c->padding == TWOLAME_PAD_NO)
                CHECK(ps[i].padding == 0);
            if (ps[i].frame_len == 626)
                saw_626 = 1;
            if (ps[i].frame_len == 627)
                saw_627 = 1;
            if (ps[i].frame_len == 576)
                saw_576 = 1;

            if (c->extra_bytes) {
                /* reserved ancillary bytes in front of the CRC stay zero */
                int start = ps[i].frame_len - 3 - c->crc_len + 1 - c->extra_bytes;
                for (k = 0; k < c->extra_bytes; k++)
                    CHECK(stream[ps[i].off + start + k] == 0);
            }
        }
        if (c->rate == 44100 && c->bitrate == 192 && c->padding == TWOLAME_PAD_ALL)
            CHECK(saw_626 && saw_627);
        if (c->rate == 24000)
            CHECK(saw_576);

        /* same configuration with streaming off must match except CRC bytes */
        {
            int llen = build_legacy(c, l, r, N, legacy, MAXBUF);

            compare_except_crc(stream, total, legacy, llen, c->crc_len);
            /* The manual flow must have written the CRC we compute ourselves
               into each preceding frame; this anchors the "except CRC" byte
               comparison so it cannot hide a wrong CRC. */
            {
                static PARSED lp[FMAX];
                int lnf = split_frames(legacy, llen, lp, FMAX);

                CHECK(lnf == nf);
                for (i = 0; i < lnf - 1; i++) {
                    unsigned char exp[4];
                    int k;

                    dab_crc_bytes(&lp[i + 1], c->crc_len, exp);
                    for (k = 0; k < c->crc_len; k++)
                        CHECK(legacy[lp[i].off + lp[i].frame_len - 3 - k] == exp[k]);
                }
                for (k = 0; k < c->crc_len; k++)
                    CHECK(legacy[lp[lnf - 1].off + lp[lnf - 1].frame_len - 3 - k] == 0);
            }
        }
    }
    free(l);
    free(r);
    free(stream);
    free(legacy);
    printf("DAB_STREAM_CASE layout PASS\n");
}

static void test_isolation(void)
{
    const CFG ca = { 44100, 192, 2, TWOLAME_STEREO, 4, TWOLAME_PAD_ALL, 1, 0 };
    const CFG cb = { 24000, 96, 1, TWOLAME_MONO, 4, TWOLAME_PAD_NO, 0, 0 };
    int checks_at_entry = g_checks;

    g_crc_frames = 0;
    int na = 3 * S + 400;
    int nb = 2 * S + 700;
    short *la = (short *) malloc(sizeof(short) * na);
    short *ra = (short *) malloc(sizeof(short) * na);
    short *lb = (short *) malloc(sizeof(short) * nb);
    short *rb = (short *) malloc(sizeof(short) * nb);
    short *ib = (short *) malloc(sizeof(short) * nb);   /* mono interleaved: stride 1 */
    unsigned char *sola = (unsigned char *) malloc(MAXBUF);
    unsigned char *solb = (unsigned char *) malloc(MAXBUF);
    unsigned char *mix_a = (unsigned char *) malloc(MAXBUF);
    unsigned char *mix_b = (unsigned char *) malloc(MAXBUF);
    unsigned char tmp[7200 + 2 * MAXBUF / 4];
    twolame_options *oa, *ob;
    int oka, okb;
    int pa = 0, pb = 0, ta = 0, tb = 0;
    int emitted_a, emitted_b;
    static const int plan_a[] = { 500, 2000, 1152, 300, 777 };
    static const int plan_b[] = { 700, 1500, 1152, 1999, 1152 };
    int i;

    CHECK(la && ra && lb && rb && ib && sola && solb && mix_a && mix_b);
    gen_pcm(0x500, na, la, ra);
    gen_pcm(0x600, nb, lb, rb);
    for (i = 0; i < nb; i++)
        ib[i] = lb[i];

    /* solo runs */
    {
        int la_len = run_stream(&ca, la, ra, na, 0, 1, sola, MAXBUF);
        int lb_len = run_stream(&cb, lb, rb, nb, 0, 1, solb, MAXBUF);

        CHECK(verify_chain(sola, la_len, ca.crc_len) == 4);
        CHECK(verify_chain(solb, lb_len, cb.crc_len) == 3);
    }

    /* interleaved runs with differently shaped chunk plans */
    oa = open_enc(&ca, 1, &oka);
    ob = open_enc(&cb, 1, &okb);
    CHECK(oka && okb);
    i = 0;
    while (pa < na || pb < nb) {
        if (pa < na) {
            int k = plan_a[i % 5];
            int ret, cap;

            if (k > na - pa)
                k = na - pa;
            cap = 7200 + 2 * k;
            CHECK((size_t) cap <= sizeof(tmp));
            memset(tmp, 0xA5, (size_t) cap);
            ret = twolame_encode_buffer(oa, la + pa, ra + pa, k, tmp, cap);
            CHECK(ret >= 0);
            memcpy(mix_a + ta, tmp, (size_t) ret);
            ta += ret;
            pa += k;
        }
        if (pb < nb) {
            int k = plan_b[i % 5];
            int ret, cap;

            if (k > nb - pb)
                k = nb - pb;
            cap = 7200 + 2 * k;
            CHECK((size_t) cap <= sizeof(tmp));
            memset(tmp, 0xA5, (size_t) cap);
            ret = twolame_encode_buffer_interleaved(ob, ib + pb, k, tmp, cap);
            CHECK(ret >= 0);
            memcpy(mix_b + tb, tmp, (size_t) ret);
            tb += ret;
            pb += k;
        }
        i++;
    }

    emitted_a = count_frames(mix_a, ta);
    emitted_b = count_frames(mix_b, tb);
    /* During feeding only complete input frames can produce audio: one of
       those is still held back per instance, and the partial tail is not a
       frame until flush pads it. */
    CHECK(emitted_a == na / S - 1);
    CHECK(emitted_b == nb / S - 1);

    /* finish in an interleaved fashion */
    {
        int ret = twolame_encode_flush(oa, tmp, 7200);

        CHECK(ret >= 0);
        memcpy(mix_a + ta, tmp, (size_t) ret);
        ta += ret;
        ret = twolame_encode_flush(ob, tmp, 7200);
        CHECK(ret >= 0);
        memcpy(mix_b + tb, tmp, (size_t) ret);
        tb += ret;
        CHECK(twolame_encode_flush(oa, tmp, 7200) == 0);
        CHECK(twolame_encode_flush(ob, tmp, 7200) == 0);
    }
    twolame_close(&oa);
    twolame_close(&ob);

    {
        unsigned char *refa = (unsigned char *) malloc(MAXBUF);
        unsigned char *refb = (unsigned char *) malloc(MAXBUF);
        int refla, reflb;

        CHECK(refa && refb);
        /* re-run each instance on its own */
        refla = run_stream(&ca, la, ra, na, 0, 1, refa, MAXBUF);
        reflb = run_stream(&cb, lb, rb, nb, 0, 1, refb, MAXBUF);
        CHECK(refla == ta);
        CHECK(reflb == tb);
        CHECK(memcmp(refa, mix_a, (size_t) ta) == 0);
        CHECK(memcmp(refb, mix_b, (size_t) tb) == 0);
        CHECK(verify_chain(mix_a, ta, ca.crc_len) == 4);
        CHECK(verify_chain(mix_b, tb, cb.crc_len) == 3);
        free(refa);
        free(refb);
    }

    /* there must be real assertions and real independently checked frames;
       zero assertions can never pass */
    CHECK(g_checks - checks_at_entry > 100);
    CHECK(g_crc_frames > 0);
    printf("DAB_STREAM_CASE isolation PASS checked_frames=%d assertions=%d\n",
           g_crc_frames, g_checks - checks_at_entry);

    free(la);
    free(ra);
    free(lb);
    free(rb);
    free(ib);
    free(sola);
    free(solb);
    free(mix_a);
    free(mix_b);
}

/* Build an encoder with arbitrary (possibly invalid) settings. */
static twolame_options *build_custom(int dab, int crc_len, int xpad, int vbr,
                                     int freeformat, int energy, int ancillary,
                                     int channels, int mode, int in_rate, int out_rate,
                                     int padding, int protection)
{
    twolame_options *o = twolame_init();

    CHECK(o != NULL);
    CHECK(twolame_set_num_channels(o, channels) == 0);
    CHECK(twolame_set_in_samplerate(o, in_rate) == 0);
    CHECK(twolame_set_out_samplerate(o, out_rate) == 0);
    CHECK(twolame_set_bitrate(o, 96) == 0);
    CHECK(twolame_set_mode(o, mode) == 0);
    CHECK(twolame_set_verbosity(o, 0) == 0);
    CHECK(twolame_set_padding(o, padding) == 0);
    CHECK(twolame_set_error_protection(o, protection) == 0);
    CHECK(twolame_set_DAB(o, dab) == 0);
    CHECK(twolame_set_DAB_crc_length(o, crc_len) == 0);
    CHECK(twolame_set_DAB_xpad_length(o, xpad) == 0);
    CHECK(twolame_set_VBR(o, vbr) == 0);
    CHECK(twolame_set_freeformat(o, freeformat) == 0);
    CHECK(twolame_set_energy_levels(o, energy) == 0);
    CHECK(twolame_set_num_ancillary_bits(o, ancillary) == 0);
    CHECK(twolame_set_DAB_streaming(o, 1) == 0);
    return o;
}

static void expect_init_fail(twolame_options * o)
{
    CHECK(twolame_init_params(o) != 0);
}

static void test_legacy(void)
{
    const CFG cfg = { 48000, 96, 2, TWOLAME_STEREO, 2, TWOLAME_PAD_ALL, 0, 0 };
    int N = 4 * S + 211;
    short *l = (short *) malloc(sizeof(short) * N);
    short *r = (short *) malloc(sizeof(short) * N);
    short *inter = (short *) malloc(sizeof(short) * 2 * N);
    float *fl = (float *) malloc(sizeof(float) * N);
    float *fr = (float *) malloc(sizeof(float) * N);
    unsigned char *a = (unsigned char *) malloc(MAXBUF);
    unsigned char *b = (unsigned char *) malloc(MAXBUF);
    unsigned char *stream = (unsigned char *) malloc(MAXBUF);
    twolame_options *o;
    int ok, i, total;

    CHECK(l && r && inter && fl && fr && a && b && stream);
    gen_pcm(0x700, N, l, r);
    for (i = 0; i < N; i++) {
        inter[2 * i] = l[i];
        inter[2 * i + 1] = r[i];
        fl[i] = (float) l[i] / 32768.0f;
        fr[i] = (float) r[i] / 32768.0f;
    }

    /* setter restrictions, including after a successful init */
    o = twolame_init();
    CHECK(o != NULL);
    CHECK(twolame_get_DAB_streaming(o) == 0);
    CHECK(twolame_set_DAB_streaming(o, 2) == -1);
    CHECK(twolame_set_DAB_streaming(o, -1) == -1);
    CHECK(twolame_get_DAB_streaming(o) == 0);
    CHECK(twolame_set_DAB_streaming(o, 1) == 0);
    CHECK(twolame_get_DAB_streaming(o) == 1);
    CHECK(twolame_set_DAB_streaming(o, 0) == 0);
    CHECK(twolame_get_DAB_streaming(o) == 0);
    twolame_close(&o);

    o = open_enc(&cfg, 1, &ok);
    CHECK(ok);
    CHECK(twolame_get_DAB_streaming(o) == 1);
    CHECK(twolame_set_DAB_streaming(o, 0) == -1);
    CHECK(twolame_set_DAB_streaming(o, 1) == -1);
    CHECK(twolame_get_DAB_streaming(o) == 1);
    twolame_close(&o);

    /* parameter restrictions; each must fail init without silent coercion */
    o = build_custom(0, 2, 0, 0, 0, 0, 32, 2, TWOLAME_STEREO, 48000, 48000,
                     TWOLAME_PAD_ALL, 0);
    expect_init_fail(o);
    twolame_close(&o);

    o = build_custom(1, 3, 0, 0, 0, 0, 40, 2, TWOLAME_STEREO, 48000, 48000,
                     TWOLAME_PAD_ALL, 0);
    expect_init_fail(o);
    twolame_close(&o);

    o = build_custom(1, 2, 1, 0, 0, 0, 32, 2, TWOLAME_STEREO, 48000, 48000,
                     TWOLAME_PAD_ALL, 0);
    expect_init_fail(o);
    twolame_close(&o);

    o = build_custom(1, 2, 0, 1, 0, 0, 32, 2, TWOLAME_STEREO, 48000, 48000,
                     TWOLAME_PAD_ALL, 0);
    expect_init_fail(o);
    twolame_close(&o);

    o = build_custom(1, 2, 0, 0, 1, 0, 32, 2, TWOLAME_STEREO, 48000, 48000,
                     TWOLAME_PAD_ALL, 0);
    expect_init_fail(o);
    twolame_close(&o);

    o = build_custom(1, 2, 0, 0, 0, 1, 32, 2, TWOLAME_STEREO, 48000, 48000,
                     TWOLAME_PAD_ALL, 0);
    expect_init_fail(o);
    twolame_close(&o);

    /* ancillary bits must be a whole number of bytes */
    o = build_custom(1, 2, 0, 0, 0, 0, 20, 2, TWOLAME_STEREO, 48000, 48000,
                     TWOLAME_PAD_ALL, 0);
    expect_init_fail(o);
    twolame_close(&o);

    /* ancillary bits must fit the CRC plus the 2-byte F-PAD */
    o = build_custom(1, 2, 0, 0, 0, 0, 24, 2, TWOLAME_STEREO, 48000, 48000,
                     TWOLAME_PAD_ALL, 0);
    expect_init_fail(o);
    twolame_close(&o);

    /* only mono / stereo with matching channel counts */
    o = build_custom(1, 2, 0, 0, 0, 0, 32, 2, TWOLAME_JOINT_STEREO, 48000, 48000,
                     TWOLAME_PAD_ALL, 0);
    CHECK(twolame_get_mode(o) == TWOLAME_JOINT_STEREO);
    expect_init_fail(o);
    CHECK(twolame_get_mode(o) == TWOLAME_JOINT_STEREO);
    twolame_close(&o);

    o = build_custom(1, 2, 0, 0, 0, 0, 32, 2, TWOLAME_DUAL_CHANNEL, 48000, 48000,
                     TWOLAME_PAD_ALL, 0);
    expect_init_fail(o);
    CHECK(twolame_get_mode(o) == TWOLAME_DUAL_CHANNEL);
    twolame_close(&o);

    o = build_custom(1, 2, 0, 0, 0, 0, 32, 1, TWOLAME_STEREO, 48000, 48000,
                     TWOLAME_PAD_ALL, 0);
    expect_init_fail(o);
    twolame_close(&o);

    o = build_custom(1, 2, 0, 0, 0, 0, 32, 2, TWOLAME_STEREO, 44100, 48000,
                     TWOLAME_PAD_ALL, 0);
    expect_init_fail(o);
    twolame_close(&o);

    /* streaming off: the old normal encoder path is unchanged */
    {
        int na, nb, nc;

        o = open_enc(&cfg, 0, &ok);
        CHECK(ok);
        na = twolame_encode_buffer_interleaved(o, inter, N, a, MAXBUF);
        {
            int fl2 = twolame_encode_flush(o, a + na, MAXBUF - na);
            CHECK(fl2 >= 0);
            na += fl2;
        }
        twolame_close(&o);

        o = open_enc(&cfg, 0, &ok);
        CHECK(ok);
        nb = twolame_encode_buffer(o, l, r, N, b, MAXBUF);
        {
            int fl2 = twolame_encode_flush(o, b + nb, MAXBUF - nb);
            CHECK(fl2 >= 0);
            nb += fl2;
        }
        twolame_close(&o);
        CHECK(na == nb);
        CHECK(memcmp(a, b, (size_t) na) == 0);
        CHECK(count_frames(a, na) == 5);

        /* chunked feeding matches a single block in legacy mode too */
        o = open_enc(&cfg, 0, &ok);
        CHECK(ok);
        nc = 0;
        for (i = 0; i < N; i += 100) {
            int k = (N - i < 100) ? N - i : 100;
            int ret = twolame_encode_buffer(o, l + i, r + i, k, b + nc, MAXBUF - nc);

            CHECK(ret >= 0);
            nc += ret;
        }
        {
            int fl2 = twolame_encode_flush(o, b + nc, MAXBUF - nc);

            CHECK(fl2 >= 0);
            nc += fl2;
        }
        twolame_close(&o);
        CHECK(nc == na);
        CHECK(memcmp(a, b, (size_t) na) == 0);

        /* the original manual DAB interface still works and its stream is
           byte-identical apart from the patched CRC fields */
        {
            int legacy_len = build_legacy(&cfg, l, r, N, b, MAXBUF);
            static PARSED ps[FMAX];
            int nf = split_frames(b, legacy_len, ps, FMAX);
            int j;

            CHECK(nf == 5);
            for (j = 0; j < nf; j++)
                CHECK(ps[j].frame_len > 0);

            /* manual patching wrote our independently computed successor
               CRCs into all but the final frame */
            for (j = 0; j < nf - 1; j++) {
                unsigned char exp[4];
                int k;

                dab_crc_bytes(&ps[j + 1], cfg.crc_len, exp);
                for (k = 0; k < cfg.crc_len; k++)
                    CHECK(b[ps[j].off + ps[j].frame_len - 3 - k] == exp[k]);
            }

            total = run_stream(&cfg, l, r, N, 0, 0, stream, MAXBUF);
            compare_except_crc(stream, total, b, legacy_len, cfg.crc_len);
            compare_exact(stream, total, b, legacy_len);
        }

        /* manual DAB interface: invalid CRC length is still rejected */
        o = twolame_init();
        CHECK(o != NULL);
        CHECK(twolame_set_num_channels(o, 2) == 0);
        CHECK(twolame_set_in_samplerate(o, 48000) == 0);
        CHECK(twolame_set_out_samplerate(o, 48000) == 0);
        CHECK(twolame_set_bitrate(o, 96) == 0);
        CHECK(twolame_set_mode(o, TWOLAME_STEREO) == 0);
        CHECK(twolame_set_verbosity(o, 0) == 0);
        CHECK(twolame_set_DAB(o, 1) == 0);
        CHECK(twolame_set_DAB_crc_length(o, 3) == 0);
        CHECK(twolame_set_num_ancillary_bits(o, 8 * 5) == 0);
        CHECK(twolame_init_params(o) == 0);
        CHECK(twolame_set_DAB_scf_crc(o, a, 100) == -1);
        twolame_close(&o);
    }

    /* float32 inputs are refused in streaming mode without side effects */
    o = open_enc(&cfg, 1, &ok);
    CHECK(ok);
    CHECK(twolame_encode_buffer_float32(o, fl, fr, S, a, MAXBUF) == -1);
    CHECK(twolame_encode_buffer_float32_interleaved(o, fl, S, a, MAXBUF) == -1);
    /* these must not have consumed PCM or touched the output state */
    total = twolame_encode_buffer(o, l, r, S, a, MAXBUF);
    CHECK(total == 0);
    total += twolame_encode_flush(o, a + total, MAXBUF - total);
    twolame_close(&o);
    {
        int ref = run_stream(&cfg, l, r, S, 0, 1, b, MAXBUF);
        CHECK(total == ref);
        CHECK(memcmp(a, b, (size_t) ref) == 0);
        verify_chain(a, total, cfg.crc_len);
    }

    /* a rejected setup does not spoil a freshly initialised encoder */
    {
        int ref = run_stream(&cfg, l, r, N, 0, 1, a, MAXBUF);
        int len = run_stream(&cfg, l, r, N, 0, 1, b, MAXBUF);

        CHECK(ref == len);
        CHECK(memcmp(a, b, (size_t) ref) == 0);
        verify_chain(a, ref, cfg.crc_len);
    }

    free(l);
    free(r);
    free(inter);
    free(fl);
    free(fr);
    free(a);
    free(b);
    free(stream);
    printf("DAB_STREAM_CASE legacy PASS\n");
}

int main(void)
{
    test_crc();
    test_chunking();
    test_tail();
    test_layout();
    test_isolation();
    test_legacy();
    printf("DAB_STREAM_OK cases=6\n");
    return 0;
}
