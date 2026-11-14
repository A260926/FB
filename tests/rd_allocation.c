/*
 *  Rate-distortion allocation tests for TwoLAME.
 *
 *  This file has no main(): task_support/driver.c supplies one and runs
 *  the task_cases registered below. The whole file is only built with
 *  -DTWOLAME_RD_TESTING, which also enables the read-only observation
 *  points in libtwolame. Nothing here is part of the public ABI.
 *
 *  Three groups are registered:
 *    optimality    - small independent exhaustive searches vs the solver
 *    bitstream     - parse public-API MP2 frames and recompute everything
 *    compatibility - API contract, legality and byte-for-byte behaviour
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "twolame.h"
#include "common.h"
#include "encode.h"
#include "../task_support/check.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef TWOLAME_RD_TESTING
#error "rd_allocation tests require TWOLAME_RD_TESTING"
#endif

#define SF_BLOCK 12
#define FRAME_PCM 1152

/* ------------------------------------------------------------------ */
/* Small MSB-first bit reader over an encoded frame                   */
/* ------------------------------------------------------------------ */

struct bit_reader {
    const unsigned char *data;
    int pos;                    /* bit position */
};

static void br_init(struct bit_reader *br, const unsigned char *data)
{
    br->data = data;
    br->pos = 0;
}

static unsigned int br_get(struct bit_reader *br, int n)
{
    unsigned int v = 0;
    int i;
    for (i = 0; i < n; i++) {
        int byte = br->pos >> 3;
        int bit = 7 - (br->pos & 7);
        v = (v << 1) | ((br->data[byte] >> bit) & 1u);
        br->pos++;
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* Deterministic non-zero PCM material                                */
/* ------------------------------------------------------------------ */

static short gen_sample(int ch, int n)
{
    double v = 0.0;
    v += 0.45 * sin(2.0 * M_PI * (ch + 1.0) * 137.0 * n / 44100.0);
    v += 0.30 * sin(2.0 * M_PI * (ch + 2.0) * 913.0 * n / 44100.0);
    v += 0.18 * sin(2.0 * M_PI * (ch + 3.0) * 4407.0 * n / 44100.0);
    v += 0.10 * sin(2.0 * M_PI * (ch + 5.0) * 12347.0 * n / 44100.0);
    /* deterministic low level decorrelation between channels */
    v += 0.05 * sin(2.0 * M_PI * (17.0 + ch * 31.0) * n / 97.0);
    if (v > 1.0)
        v = 1.0;
    if (v < -1.0)
        v = -1.0;
    return (short) lrint(v * 28000.0);
}

static void fill_pcm_interleaved(short *pcm, int frames, int nch)
{
    int n;
    for (n = 0; n < frames; n++) {
        short l = gen_sample(0, n);
        short r = gen_sample(1, n);
        if (nch == 2) {
            pcm[2 * n] = l;
            pcm[2 * n + 1] = r;
        } else {
            pcm[n] = (short) ((l + r) / 2);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Minimal PCM16 WAV reader for the bundled public samples            */
/* ------------------------------------------------------------------ */

struct wav_data {
    short *pcm;                 /* interleaved */
    int frames;
    int channels;
    int samplerate;
};

static int wav_load(const char *path, struct wav_data *w)
{
    FILE *f = fopen(path, "rb");
    unsigned char hdr[44];
    unsigned int data_len = 0;
    size_t got;

    memset(w, 0, sizeof(*w));
    if (f == NULL)
        return -1;
    got = fread(hdr, 1, sizeof(hdr), f);
    if (got < sizeof(hdr) || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fclose(f);
        return -1;
    }
    if (hdr[20] != 1 || hdr[34] != 16) {     /* PCM, 16 bit */
        fclose(f);
        return -1;
    }
    w->channels = hdr[22] | (hdr[23] << 8);
    w->samplerate = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24);
    if (memcmp(hdr + 36, "data", 4) == 0)
        data_len = hdr[40] | (hdr[41] << 8) | (hdr[42] << 16) | ((unsigned int) hdr[43] << 24);
    if (data_len == 0 || w->channels < 1 || w->channels > 2) {
        fclose(f);
        return -1;
    }
    w->frames = (int) (data_len / (2u * (unsigned int) w->channels));
    w->pcm = (short *) malloc(data_len);
    if (w->pcm == NULL) {
        fclose(f);
        return -1;
    }
    if (fread(w->pcm, 1, data_len, f) != data_len) {
        free(w->pcm);
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Encoder factory                                                    */
/* ------------------------------------------------------------------ */

struct enc_cfg {
    int rd;
    int mono;
    int samplerate;
    int bitrate;
    int crc;
    int ancillary;
    int padding;
};

static twolame_options *make_encoder(const struct enc_cfg *cfg)
{
    twolame_options *o = twolame_init();
    if (o == NULL)
        return NULL;
    twolame_set_verbosity(o, 0);
    twolame_set_num_channels(o, cfg->mono ? 1 : 2);
    twolame_set_in_samplerate(o, cfg->samplerate);
    twolame_set_out_samplerate(o, cfg->samplerate);
    twolame_set_mode(o, cfg->mono ? TWOLAME_MONO : TWOLAME_STEREO);
    twolame_set_bitrate(o, cfg->bitrate);
    twolame_set_error_protection(o, cfg->crc ? 1 : 0);
    twolame_set_num_ancillary_bits(o, cfg->ancillary);
    twolame_set_padding(o, cfg->padding ? TWOLAME_PAD_ALL : TWOLAME_PAD_NO);
    if (cfg->rd) {
        if (twolame_set_RD_optimization(o, 1) != 0) {
            twolame_close(&o);
            return NULL;
        }
    }
    if (twolame_init_params(o) != 0) {
        twolame_close(&o);
        return NULL;
    }
    return o;
}

/* ================================================================== */
/* optimality group                                                   */
/*                                                                     */
/* Independent reference implementation. The quantization rule, the   */
/* distortion accumulation and the exhaustive search are all written  */
/* here (only the standard constant tables are borrowed from the      */
/* library), so production cost/selection code is never used as an    */
/* expectation.                                                        */
/* ================================================================== */

struct t_cand {
    int ba;
    int scfsi;
    int nbits;                 /* bits beyond the unit's nbal field */
    double dist;
    unsigned int sf[3];
};

struct t_unit {
    int ch;
    int sb;
    int max_alloc;
    int ncand;
    struct t_cand cand[64];
};

static int t_quant(double x, double s, int L)
{
    double q = floor((x / s + 1.0) * (double) L * 0.5);
    if (q < 0.0)
        q = 0.0;
    if (q > (double) (L - 1))
        q = (double) (L - 1);
    return (int) q;
}

static void t_pattern_sf(const unsigned int m[3], int scfsi, unsigned int sf[3])
{
    unsigned int u, v;
    switch (scfsi) {
    case 0:
        sf[0] = m[0]; sf[1] = m[1]; sf[2] = m[2];
        break;
    case 1:
        u = m[0] < m[1] ? m[0] : m[1];
        sf[0] = sf[1] = u; sf[2] = m[2];
        break;
    case 2:
        u = m[0];
        if (m[1] < u) u = m[1];
        if (m[2] < u) u = m[2];
        sf[0] = sf[1] = sf[2] = u;
        break;
    default:
        v = m[1] < m[2] ? m[1] : m[2];
        sf[0] = m[0]; sf[1] = sf[2] = v;
        break;
    }
}

static double t_unit_dist(twolame_options *o, int ch, int sb,
                          const unsigned int sf[3], int L)
{
    int gr, j;
    double dist = 0.0;
    for (gr = 0; gr < 3; gr++) {
        double s = (double) twolame_rd_scalefactor((int) sf[gr]);
        for (j = 0; j < SF_BLOCK; j++) {
            double x = (double) (*o->sb_sample)[ch][gr][j][sb];
            int q = t_quant(x, s, L);
            double y = s * (double) (2 * q + 1 - L) / (double) L;
            double d = x - y;
            dist += d * d;
        }
    }
    return dist;
}

static double t_zero_dist(twolame_options *o, int ch, int sb)
{
    int gr, j;
    double dist = 0.0;
    for (gr = 0; gr < 3; gr++)
        for (j = 0; j < SF_BLOCK; j++) {
            double x = (double) (*o->sb_sample)[ch][gr][j][sb];
            dist += x * x;
        }
    return dist;
}

static void t_build_unit(twolame_options *o, int ch, int sb, struct t_unit *u)
{
    unsigned int m[3];
    int ba, scfsi;
    static const int nsfs[4] = { 3, 2, 1, 2 };

    u->ch = ch;
    u->sb = sb;
    u->max_alloc = twolame_rd_max_alloc_for(o, sb);
    u->ncand = 0;
    m[0] = o->scalar[ch][0][sb];
    m[1] = o->scalar[ch][1][sb];
    m[2] = o->scalar[ch][2][sb];

    /* candidate 0: zero allocation */
    {
        struct t_cand *c = &u->cand[u->ncand++];
        c->ba = 0; c->scfsi = 0; c->nbits = 0;
        c->sf[0] = m[0]; c->sf[1] = m[1]; c->sf[2] = m[2];
        c->dist = t_zero_dist(o, ch, sb);
    }
    for (ba = 1; ba <= u->max_alloc; ba++) {
        int stepidx = twolame_rd_step_index_for(o, sb, ba);
        int L = twolame_rd_steps(stepidx);
        int sample_bits = SF_BLOCK * twolame_rd_group(stepidx) * twolame_rd_bits(stepidx);
        for (scfsi = 0; scfsi < 4; scfsi++) {
            struct t_cand *c = &u->cand[u->ncand++];
            c->ba = ba;
            c->scfsi = scfsi;
            c->nbits = 2 + 6 * nsfs[scfsi] + sample_bits;
            t_pattern_sf(m, scfsi, c->sf);
            c->dist = t_unit_dist(o, ch, sb, c->sf, L);
        }
    }
}

/* Exhaustive enumeration state */
struct t_enum {
    int nunits;
    int budget;
    const struct t_unit *units;
    int sel[8];
    int best_sel[8];
    double best_dist;
    long best_bits;
};

static void t_enumerate(struct t_enum *e, int idx, long used, double dist)
{
    int k;
    if (idx == e->nunits) {
        if (dist < e->best_dist || (dist == e->best_dist && used < e->best_bits)) {
            e->best_dist = dist;
            e->best_bits = used;
            for (k = 0; k < e->nunits; k++)
                e->best_sel[k] = e->sel[k];
        }
        return;
    }
    for (k = 0; k < e->units[idx].ncand; k++) {
        const struct t_cand *c = &e->units[idx].cand[k];
        long nu = used + c->nbits;
        if (nu > (long) e->budget)
            continue;
        e->sel[idx] = k;
        t_enumerate(e, idx + 1, nu, dist + c->dist);
    }
}

/* Distortion of the selection actually committed by the library. */
static double t_committed_dist(twolame_options *o, const struct t_unit *u)
{
    int ba = (int) o->bit_alloc[u->ch][u->sb];
    int scfsi = (int) o->scfsi[u->ch][u->sb];
    unsigned int sf[3];
    int k;

    sf[0] = o->scalar[u->ch][0][u->sb];
    sf[1] = o->scalar[u->ch][1][u->sb];
    sf[2] = o->scalar[u->ch][2][u->sb];
    if (ba == 0)
        return t_zero_dist(o, u->ch, u->sb);
    for (k = 0; k < u->ncand; k++) {
        const struct t_cand *c = &u->cand[k];
        if (c->ba == ba && c->scfsi == scfsi &&
            c->sf[0] == sf[0] && c->sf[1] == sf[1] && c->sf[2] == sf[2])
            return c->dist;
    }
    /* Committed selection is not even in the legal candidate domain */
    return -1.0;
}

static int t_committed_bits(twolame_options *o, const struct t_unit *u)
{
    int ba = (int) o->bit_alloc[u->ch][u->sb];
    int scfsi = (int) o->scfsi[u->ch][u->sb];
    int k;
    for (k = 0; k < u->ncand; k++) {
        const struct t_cand *c = &u->cand[k];
        if (c->ba == ba && (ba == 0 || c->scfsi == scfsi))
            return c->nbits;
    }
    return -1;
}

/* Inject fixed, non-zero material into a few units; all other units
   stay exactly silent (their optimum is provably zero allocation). */
static void inject_material(twolame_options *o, const int sbs[], int nsbs)
{
    static const double amp[3][3] = {
        {0.50, 0.495, 0.10},     /* groups 0/1 nearly equal: sharing */
        {0.30, 0.050, 0.020},
        {0.80, 0.600, 0.400}
    };
    int u, gr, j, sb;

    memset(*o->sb_sample, 0, sizeof(sb_sample_t));
    for (u = 0; u < nsbs; u++) {
        sb = sbs[u];
        for (gr = 0; gr < 3; gr++)
            for (j = 0; j < SF_BLOCK; j++)
                (*o->sb_sample)[0][gr][j][sb] =
                    (FLOAT) (amp[u][gr] * (0.82 + 0.03 * j));
    }
    twolame_scalefactor_calc(*o->sb_sample, o->scalar, 1, o->sblimit);
}

static int case_opt_small_search(void)
{
    /* MPEG-1 32 kHz mono, 80 kbps -> allocation table 3, sblimit 12,
       lines mixing grouped (3/5/9) and non-grouped quantizers. */
    struct enc_cfg cfg = { 1, 1, 32000, 80, 0, 0, 0 };
    twolame_options *o = make_encoder(&cfg);
    const int sbs[3] = { 0, 2, 5 };
    const int nsbs = 3;
    struct t_unit units[3];
    unsigned int raw_scalar[1][3][SBLIMIT];
    int syntax, u, bi;
    int min_nonzero = 1 << 20;
    int max_total = 0;
    int shared = 0;

    TASK_CHECK(o != NULL);
    inject_material(o, sbs, nsbs);
    memcpy(raw_scalar, o->scalar, sizeof(raw_scalar));

    for (u = 0; u < nsbs; u++) {
        int k;
        int mb = 0;
        t_build_unit(o, 0, sbs[u], &units[u]);
        for (k = 1; k < units[u].ncand; k++) {
            if (units[u].cand[k].nbits < min_nonzero)
                min_nonzero = units[u].cand[k].nbits;
            if (units[u].cand[k].nbits > mb)
                mb = units[u].cand[k].nbits;
        }
        max_total += mb;
    }
    syntax = twolame_rd_fixed_overhead(o);

    /* budgets: below the cheapest non-zero choice, two middling ones,
       and one loose enough to take every maximum level. */
    {
        int budgets[4];
        budgets[0] = min_nonzero - 1;
        budgets[1] = min_nonzero + 20;
        budgets[2] = min_nonzero * 3 + 40;
        budgets[3] = max_total;
        for (bi = 0; bi < 4; bi++) {
            struct t_enum e;
            int adb, used;
            double committed = 0.0;
            long committed_bits = 0;

            /* restore raw scalefactors before each solver run */
            memcpy(o->scalar, raw_scalar, sizeof(raw_scalar));
            memset(o->bit_alloc, 0, sizeof(o->bit_alloc));
            memset(o->scfsi, 0, sizeof(o->scfsi));

            e.nunits = nsbs;
            e.budget = budgets[bi];
            e.units = units;
            e.best_dist = 1e300;
            e.best_bits = (long) 1 << 60;
            t_enumerate(&e, 0, 0, 0.0);

            adb = syntax + budgets[bi];
            TASK_CHECK(twolame_rd_bit_allocation(o, &adb) == 0);
            used = budgets[bi] - adb;
            TASK_CHECK(used >= 0 && used <= budgets[bi]);

            for (u = 0; u < nsbs; u++) {
                double d = t_committed_dist(o, &units[u]);
                int b = t_committed_bits(o, &units[u]);
                TASK_CHECK(d >= 0.0);
                TASK_CHECK(b >= 0);
                committed += d;
                committed_bits += b;
            }
            TASK_CHECK((long) used == committed_bits);
            TASK_CHECK(fabs(committed - e.best_dist) <=
                       1e-9 * (1.0 + fabs(e.best_dist)));
            if (committed == e.best_dist) {
                /* exact cost tie: the fewer-bit solution is mandatory */
                TASK_CHECK(committed_bits == e.best_bits);
            }
            /* otherwise this is a sub-ULP near tie between two
               independent double accumulations; both costs agree to
               far below 1e-9 relative, so either solution is optimal */

            /* SCFSI patterns must match the raw indices exactly */
            for (u = 0; u < nsbs; u++) {
                int sb = sbs[u];
                int ba = (int) o->bit_alloc[0][sb];
                if (ba) {
                    unsigned int m[3], sf[3], got[3];
                    int scfsi = (int) o->scfsi[0][sb];
                    m[0] = raw_scalar[0][0][sb];
                    m[1] = raw_scalar[0][1][sb];
                    m[2] = raw_scalar[0][2][sb];
                    t_pattern_sf(m, scfsi, sf);
                    got[0] = o->scalar[0][0][sb];
                    got[1] = o->scalar[0][1][sb];
                    got[2] = o->scalar[0][2][sb];
                    TASK_CHECK(got[0] == sf[0] && got[1] == sf[1] && got[2] == sf[2]);
                    if (scfsi != 0)
                        shared = 1;
                } else {
                    TASK_CHECK(o->scfsi[0][sb] == 0);
                }
            }

            /* tightest budget: everything must be zero allocation */
            if (bi == 0) {
                for (u = 0; u < nsbs; u++)
                    TASK_CHECK(o->bit_alloc[0][sbs[u]] == 0);
                TASK_CHECK(e.best_bits == 0);
            }
            /* loosest budget: every unit reaches its maximum level */
            if (bi == 3) {
                for (u = 0; u < nsbs; u++)
                    TASK_CHECK((int) o->bit_alloc[0][sbs[u]] == units[u].max_alloc);
            }
        }
    }

    /* Explicit scalefactor-sharing witness: an optimum on one of the
       bounded budgets must use a non-zero SCFSI. */
    TASK_CHECK(shared);

    twolame_close(&o);
    return 0;
}

/* Distortion of the selection currently committed in an encoder,
   evaluated with the single RD quantization rule. */
static double eval_committed(twolame_options *o)
{
    int nch = o->num_channels_out;
    int sblimit = o->sblimit;
    int sb, ch, gr, j;
    double dist = 0.0;

    for (sb = 0; sb < sblimit; sb++)
        for (ch = 0; ch < nch; ch++) {
            int ba = (int) o->bit_alloc[ch][sb];
            if (ba == 0) {
                for (gr = 0; gr < 3; gr++)
                    for (j = 0; j < SF_BLOCK; j++) {
                        double x = (double) (*o->sb_sample)[ch][gr][j][sb];
                        dist += x * x;
                    }
                continue;
            }
            {
                int stepidx = twolame_rd_step_index_for(o, sb, ba);
                int L = twolame_rd_steps(stepidx);
                for (gr = 0; gr < 3; gr++) {
                    double s = (double) twolame_rd_scalefactor((int) o->scalar[ch][gr][sb]);
                    for (j = 0; j < SF_BLOCK; j++) {
                        double x = (double) (*o->sb_sample)[ch][gr][j][sb];
                        int q = t_quant(x, s, L);
                        double y = s * (double) (2 * q + 1 - L) / (double) L;
                        double d = x - y;
                        dist += d * d;
                    }
                }
            }
        }
    return dist;
}

static int case_opt_legacy_comparison(void)
{
    /* Tight MPEG-1 mono budget where the greedy allocator provably
       leaves error on the table. Both encoders get the identical PCM
       frame and therefore identical analysis subband samples. */
    struct enc_cfg cfg_leg = { 0, 1, 32000, 48, 0, 0, 0 };
    struct enc_cfg cfg_rd = { 1, 1, 32000, 48, 0, 0, 0 };
    twolame_options *leg = make_encoder(&cfg_leg);
    twolame_options *rd = make_encoder(&cfg_rd);
    unsigned char buf_leg[4096], buf_rd[4096];
    short pcm[FRAME_PCM];
    int n_leg, n_rd;
    double d_leg, d_rd;

    TASK_CHECK(leg != NULL && rd != NULL);
    fill_pcm_interleaved(pcm, FRAME_PCM, 1);
    n_leg = twolame_encode_buffer_interleaved(leg, pcm, FRAME_PCM, buf_leg, sizeof(buf_leg));
    n_rd = twolame_encode_buffer_interleaved(rd, pcm, FRAME_PCM, buf_rd, sizeof(buf_rd));
    TASK_CHECK(n_leg > 0 && n_rd > 0 && n_leg == n_rd);

    /* The analysis samples must be identical: filters run once per
       frame in both paths and share the same (fresh) history. */
    TASK_CHECK(memcmp(*twolame_rd_test_get_samples(leg),
                      *twolame_rd_test_get_samples(rd),
                      sizeof(sb_sample_t)) == 0);

    d_leg = eval_committed(leg);
    d_rd = eval_committed(rd);
    printf("OPT fixed-material frame distortion: legacy=%.6f rd=%.6f ratio=%.6f\n",
           d_leg, d_rd, d_rd / d_leg);
    TASK_CHECK(d_leg > 0.0 && d_rd > 0.0);
    TASK_CHECK(d_rd < d_leg);
    TASK_CHECK(d_rd <= d_leg * (1.0 + 1e-12));

    twolame_close(&leg);
    twolame_close(&rd);
    return 0;
}

/* ================================================================== */
/* bitstream group                                                    */
/* ================================================================== */

struct frame_parse {
    int frame_bytes;
    int padding;
    int crc;
    int alloc_bits;
    int scfsi_bits;
    int scale_bits;
    int sample_bits;
    int zero_bits;
    double distortion;
    int nonzero_units;
    unsigned int grouped_mask;   /* bits set for each grouped level L seen */
    int nongrouped_units;
};

/* Parse one frame straight from the byte stream and recompute every
   field, the bit accounting and the squared reconstruction error. */
static int parse_frame(twolame_options *o, const unsigned char *f, int frame_bytes,
                       struct frame_parse *p)
{
    struct bit_reader br;
    int nch = o->num_channels_out;
    int sblimit = o->sblimit;
    int anc = twolame_get_num_ancillary_bits(o);
    int sb, ch, gr, j, k;
    unsigned int ba[2][SBLIMIT], scfsi[2][SBLIMIT], sf[2][3][SBLIMIT];

    memset(p, 0, sizeof(*p));
    memset(ba, 0, sizeof(ba));
    memset(scfsi, 0, sizeof(scfsi));
    memset(sf, 0, sizeof(sf));
    br_init(&br, f);
    p->frame_bytes = frame_bytes;

    /* header */
    TASK_CHECK(br_get(&br, 12) == 0xfff);
    br_get(&br, 1);             /* ID */
    br_get(&br, 2);             /* layer */
    p->crc = (br_get(&br, 1) == 0);
    br_get(&br, 4);             /* bitrate index */
    br_get(&br, 2);             /* samplerate index */
    p->padding = (int) br_get(&br, 1);
    br_get(&br, 1);             /* private */
    br_get(&br, 2);             /* mode */
    br_get(&br, 2);             /* mode ext */
    br_get(&br, 2);             /* copyright/original */
    br_get(&br, 2);             /* emphasis */
    TASK_CHECK(p->crc == (o->header.error_protection != 0));
    if (p->crc)
        br_get(&br, 16);

    /* bit allocation */
    for (sb = 0; sb < sblimit; sb++)
        for (ch = 0; ch < nch; ch++) {
            int n = twolame_rd_nbal_for(o, sb);
            ba[ch][sb] = br_get(&br, n);
            p->alloc_bits += n;
        }

    /* SCFSI */
    for (sb = 0; sb < sblimit; sb++)
        for (ch = 0; ch < nch; ch++)
            if (ba[ch][sb]) {
                scfsi[ch][sb] = br_get(&br, 2);
                p->scfsi_bits += 2;
            }

    /* scalefactors */
    for (sb = 0; sb < sblimit; sb++)
        for (ch = 0; ch < nch; ch++)
            if (ba[ch][sb]) {
                switch (scfsi[ch][sb]) {
                case 0:
                    sf[ch][0][sb] = br_get(&br, 6);
                    sf[ch][1][sb] = br_get(&br, 6);
                    sf[ch][2][sb] = br_get(&br, 6);
                    p->scale_bits += 18;
                    break;
                case 1:
                case 3:
                    sf[ch][0][sb] = br_get(&br, 6);
                    sf[ch][2][sb] = br_get(&br, 6);
                    sf[ch][1][sb] = (scfsi[ch][sb] == 1) ? sf[ch][0][sb] : sf[ch][2][sb];
                    p->scale_bits += 12;
                    break;
                default:
                    sf[ch][0][sb] = sf[ch][1][sb] = sf[ch][2][sb] = br_get(&br, 6);
                    p->scale_bits += 6;
                    break;
                }
            }

    /* sample codes + reconstruction error against the analysis samples */
    for (gr = 0; gr < 3; gr++)
        for (j = 0; j < SF_BLOCK; j += 3)
            for (sb = 0; sb < sblimit; sb++)
                for (ch = 0; ch < nch; ch++) {
                    int bav = (int) ba[ch][sb];
                    int qs[3];
                    if (bav == 0) {
                        for (k = 0; k < 3; k++) {
                            double x = (double) (*o->sb_sample)[ch][gr][j + k][sb];
                            p->distortion += x * x;
                        }
                        continue;
                    }
                    p->nonzero_units++;
                    {
                        int stepidx = twolame_rd_step_index_for(o, sb, bav);
                        int L = twolame_rd_steps(stepidx);
                        int nb = twolame_rd_bits(stepidx);
                        if (twolame_rd_group(stepidx) == 1) {
                            if (L <= (int) sizeof(unsigned int) * 8)
                                p->grouped_mask |= 1u << L;
                        } else {
                            p->nongrouped_units++;
                        }
                        if (twolame_rd_group(stepidx) == 3) {
                            for (k = 0; k < 3; k++) {
                                qs[k] = (int) br_get(&br, nb);
                                p->sample_bits += nb;
                            }
                        } else {
                            unsigned int V = br_get(&br, nb);
                            p->sample_bits += nb;
                            qs[0] = (int) (V % (unsigned int) L);
                            qs[1] = (int) ((V / (unsigned int) L) % (unsigned int) L);
                            qs[2] = (int) (V / ((unsigned int) L * (unsigned int) L));
                        }
                        for (k = 0; k < 3; k++) {
                            double x = (double) (*o->sb_sample)[ch][gr][j + k][sb];
                            double s = (double) twolame_rd_scalefactor((int) sf[ch][gr][sb]);
                            int qwant = t_quant(x, s, L);
                            double y;
                            TASK_CHECK(qs[k] >= 0 && qs[k] < L);
                            TASK_CHECK(qs[k] == qwant);
                            TASK_CHECK(qs[k] == (int) (*o->subband)[ch][gr][j + k][sb]);
                            y = s * (double) (2 * qs[k] + 1 - L) / (double) L;
                            p->distortion += (x - y) * (x - y);
                        }
                    }
                }

    /* cross-check parsed control fields against encoder observation */
    for (sb = 0; sb < sblimit; sb++)
        for (ch = 0; ch < nch; ch++) {
            TASK_CHECK(ba[ch][sb] == o->bit_alloc[ch][sb]);
            if (ba[ch][sb]) {
                TASK_CHECK(scfsi[ch][sb] == o->scfsi[ch][sb]);
                for (gr = 0; gr < 3; gr++)
                    TASK_CHECK(sf[ch][gr][sb] == o->scalar[ch][gr][sb]);
            }
        }

    /* everything between the last sample and the padding/ancillary
       area must be the zero fill of the unused budget */
    p->zero_bits = frame_bytes * 8 - br.pos
        - (p->padding ? 8 : 0) - anc;
    TASK_CHECK(p->zero_bits >= 0);
    for (k = 0; k < p->zero_bits; k++)
        TASK_CHECK(br_get(&br, 1) == 0);
    if (p->padding)
        br_get(&br, 8);
    for (k = 0; k < anc; k++)
        TASK_CHECK(br_get(&br, 1) == 0);
    TASK_CHECK(br.pos == frame_bytes * 8);
    return 0;
}

/* Encode frames one at a time and parse each immediately, while the
   read-only observation points still hold that frame's analysis
   samples. */
static int run_parse_cases(const struct enc_cfg *cfg, int frames_wanted,
                           int want_both_pad_lengths, struct wav_data *wav,
                           unsigned int *grouped_out, int *nongrouped_out)
{
    twolame_options *o = make_encoder(cfg);
    unsigned char out[32768];
    int nch = cfg->mono ? 1 : 2;
    int f;
    int saw_pad = 0, saw_nopad = 0;
    double first_dist = -1.0;
    int total = 0, pos = 0, produced = 0;
    unsigned int grouped_mask = 0;
    int nongrouped = 0;

    if (grouped_out)
        *grouped_out = 0;
    if (nongrouped_out)
        *nongrouped_out = 0;
    TASK_CHECK(o != NULL);
    for (f = 0; f < frames_wanted; f++) {
        short pcm[FRAME_PCM * 2];
        int n, len;
        struct frame_parse p;

        if (wav != NULL) {
            int base = f * FRAME_PCM;
            if (base + FRAME_PCM > wav->frames)
                break;
            if (wav->channels == 2)
                n = twolame_encode_buffer_interleaved(o, wav->pcm + base * 2,
                                                      FRAME_PCM, out + total,
                                                      sizeof(out) - total);
            else {
                /* planarize the (mono) wav into separate channel buffers */
                int i;
                static short left[FRAME_PCM], right[FRAME_PCM];
                for (i = 0; i < FRAME_PCM; i++)
                    left[i] = right[i] = wav->pcm[base + i];
                n = twolame_encode_buffer(o, left, right, FRAME_PCM,
                                          out + total, sizeof(out) - total);
            }
        } else {
            fill_pcm_interleaved(pcm, FRAME_PCM, nch);
            n = twolame_encode_buffer_interleaved(o, pcm, FRAME_PCM,
                                                  out + total, sizeof(out) - total);
        }
        TASK_CHECK(n > 0);
        total += n;
        produced++;

        /* one frame per call: parse it against the live observation */
        /* 12-bit sync word; version/layer differences live in byte 1 */
        TASK_CHECK(out[pos] == 0xff && (out[pos + 1] & 0xf0) == 0xf0);
        /* padding is the top bit of header byte 2 (after bitrate/sr) */
        len = 144 * cfg->bitrate * 1000 / cfg->samplerate
            + ((out[pos + 2] >> 1) & 1);
        TASK_CHECK(pos + len == total);
        TASK_CHECK(parse_frame(o, out + pos, len, &p) == 0);
        TASK_CHECK(p.nonzero_units > 0);
        TASK_CHECK(p.distortion > 0.0);
        grouped_mask |= p.grouped_mask;
        nongrouped += p.nongrouped_units;
        if (p.padding)
            saw_pad = 1;
        else
            saw_nopad = 1;
        if (first_dist < 0.0)
            first_dist = p.distortion;
        pos += len;
    }
    TASK_CHECK(produced >= 1 && pos == total);
    if (want_both_pad_lengths) {
        TASK_CHECK(saw_pad && saw_nopad);
    } else {
        TASK_CHECK(!saw_pad);
    }
    TASK_CHECK(first_dist > 0.0);
    if (grouped_out)
        *grouped_out = grouped_mask;
    if (nongrouped_out)
        *nongrouped_out = nongrouped;
    twolame_close(&o);
    return 0;
}

static int case_bs_mpeg1_stereo_pad(void)
{
    /* 44.1 kHz stereo, CRC, ancillary, padding: both real frame lengths.
       The high bitrate reaches non-grouped power-of-two quantizers. */
    struct enc_cfg cfg = { 1, 0, 44100, 192, 1, 24, 1 };
    unsigned int grouped;
    int nongrouped;
    int rc = run_parse_cases(&cfg, 6, 1, NULL, &grouped, &nongrouped);
    if (rc)
        return rc;
    TASK_CHECK(nongrouped > 0);
    return 0;
}

static int case_bs_mpeg2_mono(void)
{
    /* MPEG-2 LSF mono, table 4 with 4/3/2-bit nbal lines, ancillary */
    struct enc_cfg cfg = { 1, 1, 22050, 48, 0, 16, 0 };
    return run_parse_cases(&cfg, 3, 0, NULL, NULL, NULL);
}

static int case_bs_grouped_levels(void)
{
    /* 44.1 kHz mono at 56 kbps reaches all grouped 3, 5 and 9 level
       codewords, which pack three samples into one transmitted word. */
    struct enc_cfg cfg = { 1, 1, 44100, 56, 1, 0, 0 };
    unsigned int grouped;
    int nongrouped;
    int rc = run_parse_cases(&cfg, 4, 0, NULL, &grouped, &nongrouped);
    if (rc)
        return rc;
    TASK_CHECK((grouped & (1u << 3)) != 0);   /* 3 levels */
    TASK_CHECK((grouped & (1u << 5)) != 0);   /* 5 levels */
    TASK_CHECK((grouped & (1u << 9)) != 0);   /* 9 levels */
    return 0;
}

static int case_bs_wav_sample(void)
{
    /* Public 44.1 kHz stereo WAV through the public API, RD enabled */
    struct wav_data wav;
    struct enc_cfg cfg = { 1, 0, 44100, 192, 0, 0, 0 };
    int rc;
    TASK_CHECK(wav_load("tests/testcase-44100.wav", &wav) == 0);
    TASK_CHECK(wav.samplerate == 44100 && wav.channels == 2);
    rc = run_parse_cases(&cfg, 4, 0, &wav, NULL, NULL);
    free(wav.pcm);
    return rc;
}

/* ================================================================== */
/* compatibility group                                                */
/* ================================================================== */

static int case_comp_api(void)
{
    twolame_options *o;
    struct enc_cfg base = { 0, 1, 44100, 96, 0, 0, 0 };

    /* NULL and illegal requests */
    TASK_CHECK(twolame_set_RD_optimization(NULL, 1) == -1);
    TASK_CHECK(twolame_get_RD_optimization(NULL) == -1);

    o = twolame_init();
    TASK_CHECK(o != NULL);
    TASK_CHECK(twolame_get_RD_optimization(o) == 0);   /* default off */
    TASK_CHECK(twolame_set_RD_optimization(o, 2) == -1);
    TASK_CHECK(twolame_set_RD_optimization(o, -1) == -1);
    TASK_CHECK(twolame_get_RD_optimization(o) == 0);   /* retained */
    TASK_CHECK(twolame_set_RD_optimization(o, 1) == 0);
    TASK_CHECK(twolame_get_RD_optimization(o) == 1);
    TASK_CHECK(twolame_set_RD_optimization(o, 0) == 0);
    TASK_CHECK(twolame_get_RD_optimization(o) == 0);
    twolame_close(&o);

    /* locked after successful initialisation, setting retained */
    o = make_encoder(&(struct enc_cfg) { 1, 1, 44100, 96, 0, 0, 0 });
    TASK_CHECK(o != NULL);
    TASK_CHECK(twolame_set_RD_optimization(o, 0) == -1);
    TASK_CHECK(twolame_set_RD_optimization(o, 1) == -1);
    TASK_CHECK(twolame_get_RD_optimization(o) == 1);
    twolame_close(&o);

    /* unsupported configurations are refused, never silently changed */
    {
        twolame_options *bad;

        bad = twolame_init();
        twolame_set_verbosity(bad, 0);
        twolame_set_num_channels(bad, 1);
        twolame_set_in_samplerate(bad, 44100);
        twolame_set_out_samplerate(bad, 44100);
        twolame_set_mode(bad, TWOLAME_MONO);
        twolame_set_bitrate(bad, 96);
        twolame_set_VBR(bad, 1);
        twolame_set_RD_optimization(bad, 1);
        TASK_CHECK(twolame_init_params(bad) != 0);
        twolame_close(&bad);

        bad = twolame_init();
        twolame_set_verbosity(bad, 0);
        twolame_set_num_channels(bad, 2);
        twolame_set_in_samplerate(bad, 44100);
        twolame_set_out_samplerate(bad, 44100);
        twolame_set_mode(bad, TWOLAME_STEREO);
        twolame_set_bitrate(bad, 192);
        twolame_set_freeformat(bad, 1);
        twolame_set_RD_optimization(bad, 1);
        TASK_CHECK(twolame_init_params(bad) != 0);
        twolame_close(&bad);

        bad = twolame_init();
        twolame_set_verbosity(bad, 0);
        twolame_set_num_channels(bad, 2);
        twolame_set_in_samplerate(bad, 44100);
        twolame_set_out_samplerate(bad, 44100);
        twolame_set_mode(bad, TWOLAME_JOINT_STEREO);
        twolame_set_bitrate(bad, 192);
        twolame_set_RD_optimization(bad, 1);
        TASK_CHECK(twolame_init_params(bad) != 0);
        TASK_CHECK(twolame_get_mode(bad) == TWOLAME_JOINT_STEREO);
        twolame_close(&bad);

        bad = twolame_init();
        twolame_set_verbosity(bad, 0);
        twolame_set_num_channels(bad, 2);
        twolame_set_in_samplerate(bad, 44100);
        twolame_set_out_samplerate(bad, 44100);
        twolame_set_mode(bad, TWOLAME_DUAL_CHANNEL);
        twolame_set_bitrate(bad, 192);
        twolame_set_RD_optimization(bad, 1);
        TASK_CHECK(twolame_init_params(bad) != 0);
        twolame_close(&bad);

        /* ancillary reserve larger than the no-padding frame capacity */
        bad = twolame_init();
        twolame_set_verbosity(bad, 0);
        twolame_set_num_channels(bad, 1);
        twolame_set_in_samplerate(bad, 44100);
        twolame_set_out_samplerate(bad, 44100);
        twolame_set_mode(bad, TWOLAME_MONO);
        twolame_set_bitrate(bad, 96);
        twolame_set_num_ancillary_bits(bad, 3000);
        twolame_set_RD_optimization(bad, 1);
        TASK_CHECK(twolame_init_params(bad) != 0);
        twolame_close(&bad);

        /* resampling not supported either */
        bad = twolame_init();
        twolame_set_verbosity(bad, 0);
        twolame_set_num_channels(bad, 1);
        twolame_set_in_samplerate(bad, 44100);
        twolame_set_out_samplerate(bad, 32000);
        twolame_set_mode(bad, TWOLAME_MONO);
        twolame_set_bitrate(bad, 64);
        twolame_set_RD_optimization(bad, 1);
        TASK_CHECK(twolame_init_params(bad) != 0);
        twolame_close(&bad);
    }

    /* RD disabled (the default) is byte-identical on two independent
       encoders built from the same configuration */
    {
        unsigned char a[4096], b[4096];
        short pcm[FRAME_PCM * 2];
        int na, nb;
        twolame_options *ea = make_encoder(&base);
        twolame_options *eb = make_encoder(&base);
        TASK_CHECK(ea != NULL && eb != NULL);
        TASK_CHECK(twolame_get_RD_optimization(ea) == 0);
        TASK_CHECK(twolame_get_RD_optimization(eb) == 0);
        fill_pcm_interleaved(pcm, FRAME_PCM, 1);
        na = twolame_encode_buffer_interleaved(ea, pcm, FRAME_PCM, a, sizeof(a));
        nb = twolame_encode_buffer_interleaved(eb, pcm, FRAME_PCM, b, sizeof(b));
        TASK_CHECK(na > 0 && na == nb && memcmp(a, b, (size_t) na) == 0);
        twolame_close(&ea);
        twolame_close(&eb);
    }
    return 0;
}

static int case_comp_silent_tie(void)
{
    struct enc_cfg cfg = { 1, 1, 44100, 96, 0, 0, 0 };
    twolame_options *a = make_encoder(&cfg);
    twolame_options *b = make_encoder(&cfg);
    short pcm[FRAME_PCM];
    unsigned char fa[4096], fb[4096];
    int na, nb, sb;

    TASK_CHECK(a != NULL && b != NULL);
    memset(pcm, 0, sizeof(pcm));
    na = twolame_encode_buffer_interleaved(a, pcm, FRAME_PCM, fa, sizeof(fa));
    nb = twolame_encode_buffer_interleaved(b, pcm, FRAME_PCM, fb, sizeof(fb));
    TASK_CHECK(na > 0 && na == nb && memcmp(fa, fb, (size_t) na) == 0);

    /* silence: every unit ties at zero allocation, deterministic */
    for (sb = 0; sb < a->sblimit; sb++)
        TASK_CHECK(a->bit_alloc[0][sb] == 0);
    return 0;
}

static int case_comp_dual_instance(void)
{
    struct enc_cfg cfg = { 1, 0, 44100, 192, 0, 0, 0 };
    twolame_options *a = make_encoder(&cfg);
    twolame_options *b = make_encoder(&cfg);
    twolame_options *ar = make_encoder(&cfg);
    twolame_options *br = make_encoder(&cfg);
    unsigned char ia[8192], ib[8192], ra[8192], rb[8192];
    int nia = 0, nib = 0, nra = 0, nrb = 0;
    int f;

    TASK_CHECK(a && b && ar && br);
    for (f = 0; f < 4; f++) {
        short pa[FRAME_PCM * 2], pb[FRAME_PCM * 2];
        int n;
        /* distinct material for the two instances */
        fill_pcm_interleaved(pa, FRAME_PCM, 2);
        fill_pcm_interleaved(pb, FRAME_PCM, 2);
        {
            int i;
            for (i = 0; i < FRAME_PCM * 2; i++)
                pb[i] = (short) (pb[i] ^ 0x1357);
        }
        /* interleave calls between the two live instances */
        n = twolame_encode_buffer_interleaved(a, pa, FRAME_PCM, ia + nia, sizeof(ia) - nia);
        TASK_CHECK(n > 0); nia += n;
        n = twolame_encode_buffer_interleaved(b, pb, FRAME_PCM, ib + nib, sizeof(ib) - nib);
        TASK_CHECK(n > 0); nib += n;
        /* reference: single instance per stream */
        n = twolame_encode_buffer_interleaved(ar, pa, FRAME_PCM, ra + nra, sizeof(ra) - nra);
        TASK_CHECK(n > 0); nra += n;
        n = twolame_encode_buffer_interleaved(br, pb, FRAME_PCM, rb + nrb, sizeof(rb) - nrb);
        TASK_CHECK(n > 0); nrb += n;
    }
    TASK_CHECK(nia == nra && memcmp(ia, ra, (size_t) nia) == 0);
    TASK_CHECK(nib == nrb && memcmp(ib, rb, (size_t) nib) == 0);
    twolame_close(&a); twolame_close(&b);
    twolame_close(&ar); twolame_close(&br);
    return 0;
}

static int case_comp_planar_frag(void)
{
    struct enc_cfg cfg = { 1, 0, 44100, 192, 0, 0, 0 };
    twolame_options *whole = make_encoder(&cfg);
    twolame_options *frag = make_encoder(&cfg);
    twolame_options *planar = make_encoder(&cfg);
    twolame_options *planar_frag = make_encoder(&cfg);
    short inter[FRAME_PCM * 2], left[FRAME_PCM], right[FRAME_PCM];
    unsigned char bw[4096], bf[4096], bp[4096], bpf[4096];
    int nw, nf, np, npf, pos;

    TASK_CHECK(whole && frag && planar && planar_frag);
    fill_pcm_interleaved(inter, FRAME_PCM, 2);
    {
        int i;
        for (i = 0; i < FRAME_PCM; i++) {
            left[i] = inter[2 * i];
            right[i] = inter[2 * i + 1];
        }
    }

    nw = twolame_encode_buffer_interleaved(whole, inter, FRAME_PCM, bw, sizeof(bw));
    np = twolame_encode_buffer(planar, left, right, FRAME_PCM, bp, sizeof(bp));

    nf = 0;
    for (pos = 0; pos < FRAME_PCM;) {
        int chunk = 137;
        int n;
        if (chunk > FRAME_PCM - pos)
            chunk = FRAME_PCM - pos;
        n = twolame_encode_buffer_interleaved(frag, inter + pos * 2, chunk,
                                              bf + nf, sizeof(bf) - nf);
        TASK_CHECK(n >= 0);
        nf += n;
        pos += chunk;
    }

    npf = 0;
    for (pos = 0; pos < FRAME_PCM;) {
        int chunk = 211;
        int n;
        if (chunk > FRAME_PCM - pos)
            chunk = FRAME_PCM - pos;
        n = twolame_encode_buffer(planar_frag, left + pos, right + pos, chunk,
                                  bpf + npf, sizeof(bpf) - npf);
        TASK_CHECK(n >= 0);
        npf += n;
        pos += chunk;
    }

    TASK_CHECK(nw > 0);
    TASK_CHECK(nw == nf && memcmp(bw, bf, (size_t) nw) == 0);
    TASK_CHECK(nw == np && memcmp(bw, bp, (size_t) nw) == 0);
    TASK_CHECK(nw == npf && memcmp(bw, bpf, (size_t) nw) == 0);
    twolame_close(&whole); twolame_close(&frag);
    twolame_close(&planar); twolame_close(&planar_frag);
    return 0;
}

static int case_comp_flush(void)
{
    /* A partial final buffer is zero-padded and flushed as one frame,
       exactly like the legacy path. Two encoders fed the same partial
       input must emit identical bytes, and a second flush is empty. */
    struct enc_cfg cfg = { 1, 0, 44100, 192, 0, 0, 0 };
    twolame_options *a = make_encoder(&cfg);
    twolame_options *b = make_encoder(&cfg);
    short pcm[700 * 2];
    unsigned char fa[4096], fb[4096];
    int na, nb, again;

    TASK_CHECK(a && b);
    fill_pcm_interleaved(pcm, 700, 2);
    na = twolame_encode_buffer_interleaved(a, pcm, 700, fa, sizeof(fa));
    nb = twolame_encode_buffer_interleaved(b, pcm, 700, fb, sizeof(fb));
    TASK_CHECK(na == 0 && nb == 0);       /* not enough for a full frame */
    na = twolame_encode_flush(a, fa, sizeof(fa));
    nb = twolame_encode_flush(b, fb, sizeof(fb));
    TASK_CHECK(na > 0 && na == nb && memcmp(fa, fb, (size_t) na) == 0);
    TASK_CHECK(fa[0] == 0xff && (fa[1] & 0xf0) == 0xf0);
    again = twolame_encode_flush(a, fa, sizeof(fa));
    TASK_CHECK(again == 0);               /* nothing left to flush */
    twolame_close(&a);
    twolame_close(&b);
    return 0;
}

const struct task_case task_cases[] = {
    { "optimality", "small_exhaustive_search", case_opt_small_search },
    { "optimality", "fixed_material_beats_legacy", case_opt_legacy_comparison },
    { "bitstream", "mpeg1_stereo_padding_crc_ancillary", case_bs_mpeg1_stereo_pad },
    { "bitstream", "mpeg2_lsf_mono", case_bs_mpeg2_mono },
    { "bitstream", "grouped_quantization", case_bs_grouped_levels },
    { "bitstream", "public_wav_sample", case_bs_wav_sample },
    { "compatibility", "api_and_illegal_configs", case_comp_api },
    { "compatibility", "silence_tie", case_comp_silent_tie },
    { "compatibility", "flush_partial_frame", case_comp_flush },
    { "compatibility", "dual_instance_interleaving", case_comp_dual_instance },
    { "compatibility", "planar_interleaved_fragmented", case_comp_planar_frag },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
