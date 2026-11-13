/*
 * Live bit-rate reconfiguration tests for the VisualOn AAC-LC encoder.
 *
 * The test drives the public VO API (VO_PID_AAC_LIVE_BITRATE through
 * SetParam/GetParam) and also inspects the encoder's internal state through
 * its real headers, so the quantization budget, padding phase, bit reservoir
 * and psychoacoustic history are checked directly instead of only reading
 * the parameter back.
 *
 * Every encoded output is parsed as standalone ADTS: sync word, frame
 * length, sample rate, channel configuration and the contained AAC elements
 * (SCE/CPE, fill, END) are validated.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "voAAC.h"
#include "cmnMemory.h"

#include "aacenc_core.h"
#include "qc_main.h"
#include "psy_main.h"

#define MAX_FRAMES        200
#define MAX_FRAME_BYTES   8192
#define FRAME_SAMPLES     1024

/* AAC sampling frequency index table, order matches the ADTS header. */
static const int adts_sample_rates[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025, 8000, 7350, 0, 0, 0
};

typedef struct {
    unsigned char data[MAX_FRAME_BYTES];
    int len;
} AdtsFrame;

typedef struct {
    AdtsFrame frame[MAX_FRAMES];
    int count;
} AdtsStream;

static int g_failures = 0;

/* Each stream is ~1.6 MB; several are live at once, so they live on the heap
 * to avoid overflowing the stack. */
static AdtsStream *new_stream(void)
{
    AdtsStream *s = (AdtsStream *) calloc(1, sizeof(AdtsStream));
    return s;
}

static void free_stream(AdtsStream *s)
{
    free(s);
}

#define CHECK(cond, ...) do {                                          \
    if (!(cond)) {                                                      \
        g_failures++;                                                   \
        fprintf(stderr, "CHECK failed at %s:%d: ", __FILE__, __LINE__);\
        fprintf(stderr, __VA_ARGS__);                                   \
        fputc('\n', stderr);                                            \
    }                                                                   \
} while (0)

/* ------------------------------------------------------------------ */
/* Encoder lifecycle helpers                                          */
/* ------------------------------------------------------------------ */

static VO_AUDIO_CODECAPI g_api;
static VO_MEM_OPERATOR   g_memop;

static void api_setup(void)
{
    voGetAACEncAPI(&g_api);
    g_memop.Alloc = cmnMemAlloc;
    g_memop.Copy  = cmnMemCopy;
    g_memop.Free  = cmnMemFree;
    g_memop.Set   = cmnMemSet;
    g_memop.Check = cmnMemCheck;
}

static VO_HANDLE enc_open(int sampleRate, int channels, int bitRate)
{
    VO_CODEC_INIT_USERDATA user_data;
    AACENC_PARAM params;
    VO_HANDLE h = NULL;

    memset(&user_data, 0, sizeof(user_data));
    user_data.memflag = VO_IMF_USERMEMOPERATOR;
    user_data.memData = &g_memop;

    if (g_api.Init(&h, VO_AUDIO_CodingAAC, &user_data) != VO_ERR_NONE)
        return NULL;

    memset(&params, 0, sizeof(params));
    params.sampleRate = sampleRate;
    params.bitRate    = bitRate;
    params.nChannels  = (short) channels;
    params.adtsUsed   = 1;
    if (g_api.SetParam(h, VO_PID_AAC_ENCPARAM, &params) != VO_ERR_NONE) {
        g_api.Uninit(h);
        return NULL;
    }
    return h;
}

static void enc_close(VO_HANDLE h)
{
    g_api.Uninit(h);
}

static VO_U32 enc_set_live(VO_HANDLE h, int bitRate)
{
    VO_S32 v = bitRate;
    return g_api.SetParam(h, VO_PID_AAC_LIVE_BITRATE, &v);
}

static int enc_get_live(VO_HANDLE h)
{
    VO_S32 v = 0;
    if (g_api.GetParam(h, VO_PID_AAC_LIVE_BITRATE, &v) != VO_ERR_NONE)
        return -1;
    return v;
}

/*
 * Feed PCM bytes and drain every ADTS frame that becomes available.
 * Returns the number of new frames appended to out.
 */
static int enc_feed_drain(VO_HANDLE h, const unsigned char *pcm, int bytes,
                          AdtsStream *out)
{
    VO_CODECBUFFER in, obuf;
    VO_AUDIO_OUTPUTINFO info;
    unsigned char outbuf[MAX_FRAME_BYTES];
    int before = out->count;

    if (bytes > 0) {
        memset(&in, 0, sizeof(in));
        in.Buffer = (VO_PBYTE) pcm;
        in.Length = bytes;
        if (g_api.SetInputData(h, &in) != VO_ERR_NONE)
            return -1;
    }

    for (;;) {
        VO_U32 ret;
        memset(&obuf, 0, sizeof(obuf));
        memset(&info, 0, sizeof(info));
        obuf.Buffer = outbuf;
        obuf.Length = sizeof(outbuf);
        ret = g_api.GetOutputData(h, &obuf, &info);
        if (ret == VO_ERR_INPUT_BUFFER_SMALL)
            break;
        if (ret != VO_ERR_NONE)
            return -1;
        if (out->count >= MAX_FRAMES)
            return -1;
        memcpy(out->frame[out->count].data, outbuf, obuf.Length);
        out->frame[out->count].len = (int) obuf.Length;
        out->count++;
    }
    return out->count - before;
}

/* Encode exactly one 1024-sample frame worth of interleaved PCM. */
static int enc_encode_one(VO_HANDLE h, const short *pcm, int channels,
                          AdtsStream *out)
{
    return enc_feed_drain(h, (const unsigned char *) pcm,
                          FRAME_SAMPLES * channels * (int) sizeof(short), out);
}

/* ------------------------------------------------------------------ */
/* Deterministic PCM generation                                       */
/* ------------------------------------------------------------------ */

static short pcm_sample(int frame, int idx, int channel)
{
    /* fixed integer generator: layered tones, no floating point */
    unsigned int s = (unsigned int) (frame * 1024 + idx);
    int v = (int) ((s * 263 + channel * 719) % 2001) - 1000;
    v += (int) ((s * 97 + channel * 53) % 401) - 200;
    return (short) v;
}

static void gen_quiet_frame(short *pcm, int channels)
{
    int i;
    for (i = 0; i < FRAME_SAMPLES * channels; i++)
        pcm[i] = (short) (i % 7);
}

static void gen_complex_frame(short *pcm, int frame, int channels)
{
    int i, ch;
    for (i = 0; i < FRAME_SAMPLES; i++)
        for (ch = 0; ch < channels; ch++)
            pcm[i * channels + ch] = pcm_sample(frame, i, ch);
}

/* ------------------------------------------------------------------ */
/* ADTS parsing                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    int valid;
    int sample_rate;
    int channels;
    int frame_len;
    int has_sce;
    int has_cpe;
    int has_end;
    int window_sequence;
} ParsedAdts;

static unsigned int get_bits(const unsigned char *p, int *bitpos, int n)
{
    unsigned int v = 0;
    int i;
    for (i = 0; i < n; i++) {
        int byte = (*bitpos) >> 3;
        int bit  = 7 - ((*bitpos) & 7);
        v = (v << 1) | ((p[byte] >> bit) & 1);
        (*bitpos)++;
    }
    return v;
}

static unsigned int get_bits_range(const unsigned char *p, int start, int n)
{
    int pos = start;
    return get_bits(p, &pos, n);
}

static unsigned int get_bits_at(const unsigned char *p, int index)
{
    return (p[index >> 3] >> (7 - (index & 7))) & 1;
}

/*
 * Validate the raw AAC payload. This encoder always writes one channel
 * element (SCE for mono, CPE for stereo), an optional fill element, then
 * ID_END followed by 0..7 zero alignment bits. The SCE/CPE body contains
 * arbitrary bit patterns, so rather than scan inside it for tags we verify
 * the lead element tag, locate ID_END from the tail (it is always the last
 * meaningful three bits), and parse any FIL immediately before it.
 */
static int parse_elements(const unsigned char *payload, int payload_bytes,
                          int expect_channels, ParsedAdts *out)
{
    int total = payload_bytes * 8;
    int bitpos = 0;
    unsigned int first_tag;
    int end_pos, pad;

    out->has_sce = out->has_cpe = out->has_end = 0;
    if (total < 8)
        return 0;

    /* lead channel element tag + instance tag */
    first_tag = get_bits(payload, &bitpos, 3);
    if (first_tag == 0) {                 /* SCE */
        get_bits(payload, &bitpos, 4);
        out->has_sce = 1;
    } else if (first_tag == 1) {          /* CPE */
        get_bits(payload, &bitpos, 5);
        out->has_cpe = 1;
    } else {
        return 0;
    }
    if ((expect_channels == 1 && !out->has_sce) ||
        (expect_channels == 2 && !out->has_cpe))
        return 0;

    /* find ID_END at the tail after the 0..7 zero alignment bits */
    pad = 0;
    while (pad < 8 && ((total - 1 - pad) >= 0) &&
           get_bits_at(payload, total - 1 - pad) == 0)
        pad++;
    end_pos = total - pad - 3;
    if (end_pos < bitpos || get_bits_range(payload, end_pos, 3) != 7)
        return 0;
    out->has_end = 1;

    /* if a fill element sits directly before END, its size must reach END */
    if (end_pos - bitpos >= 3 &&
        get_bits_range(payload, bitpos, 3) == 6) {
        unsigned int cnt;
        int p = bitpos + 3;
        cnt = get_bits_range(payload, p, 4);
        p += 4;
        if (cnt == 15) {
            cnt = 15 + get_bits_range(payload, p, 8) - 1;
            p += 8;
        }
        p += (int) cnt * 8;
        if (p != end_pos)
            return 0;
    }

    return 1;
}

static int parse_adts(const unsigned char *buf, int buflen, int expect_channels,
                      ParsedAdts *out)
{
    int sr_idx, chan_cfg, frame_len;

    memset(out, 0, sizeof(*out));
    if (buflen < 7)
        return 0;
    if (buf[0] != 0xFF || (buf[1] & 0xF0) != 0xF0)
        return 0;
    if ((buf[1] & 0x01) != 1)          /* protection_absent, 7 byte header */
        return 0;
    sr_idx   = (buf[2] >> 2) & 0x0F;
    chan_cfg = ((buf[2] & 0x01) << 2) | ((buf[3] >> 6) & 0x03);
    frame_len = ((buf[3] & 0x03) << 11) | (buf[4] << 3) | ((buf[5] >> 5) & 0x07);

    if (sr_idx >= 12 || chan_cfg < 1 || chan_cfg > 2)
        return 0;
    if (frame_len != buflen || frame_len <= 7)
        return 0;
    if (!parse_elements(buf + 7, frame_len - 7, expect_channels, out))
        return 0;

    out->valid = 1;
    out->sample_rate = adts_sample_rates[sr_idx];
    out->channels = chan_cfg;
    out->frame_len = frame_len;
    return 1;
}

static void check_stream(const AdtsStream *s, int sampleRate, int channels,
                         const char *group)
{
    int i;
    CHECK(s->count > 0, "[%s] no frames produced", group);
    for (i = 0; i < s->count; i++) {
        ParsedAdts p;
        if (!parse_adts(s->frame[i].data, s->frame[i].len, channels, &p)) {
            CHECK(0, "[%s] frame %d is not valid ADTS", group, i);
            return;
        }
        CHECK(p.sample_rate == sampleRate,
              "[%s] frame %d sample rate %d != %d", group, i, p.sample_rate, sampleRate);
        CHECK(p.channels == channels,
              "[%s] frame %d channels %d != %d", group, i, p.channels, channels);
        CHECK((channels == 2 && p.has_cpe) || (channels == 1 && p.has_sce),
              "[%s] frame %d missing expected AAC element", group, i);
    }
}

/* ------------------------------------------------------------------ */
/* Independent 64 bit padding phase reference                         */
/* ------------------------------------------------------------------ */

typedef struct {
    int64_t R;
    int sampleRate;
} PhaseRef;

static int64_t phase_step(PhaseRef *ref, int bitRate, int *padding_out)
{
    int64_t prod = (int64_t) 128 * bitRate;
    int64_t q = prod / ref->sampleRate;
    int64_t r = prod % ref->sampleRate;
    int padding;

    ref->R -= r;
    padding = (ref->R <= 0);
    if (padding)
        ref->R += ref->sampleRate;
    *padding_out = padding;
    return (int64_t) 8 * (q + padding);
}

/* ------------------------------------------------------------------ */
/* Group 1: phase                                                     */
/* ------------------------------------------------------------------ */

static void case_phase(void)
{
    static const int rates[3] = { 96000, 64000, 128000 };
    static const int srates[2] = { 44100, 48000 };
    int si, ri;

    for (si = 0; si < 2; si++) {
        for (ri = 0; ri < 3; ri++) {
            int sr = srates[si];
            int br = rates[ri];
            VO_HANDLE h = enc_open(sr, 2, br);
            PhaseRef ref;
            AdtsStream *out;
            int f, schedule_idx = ri; /* encoder opens at rates[ri] */
            short pcm[FRAME_SAMPLES * 2];

            CHECK(h != NULL, "phase: open failed %d/%d", sr, br);
            if (!h) continue;
            out = new_stream();
            CHECK(out != NULL, "phase: stream alloc failed");
            if (!out) { enc_close(h); continue; }
            ref.R = sr; /* encoder initializes paddingRest to sampleRate */
            ref.sampleRate = sr;

            for (f = 0; f < 60; f++) {
                int next_br;
                int want_pad;
                int64_t want_avg;

                /* switch every 7 frames, cycling through all three rates */
                if (f > 0 && (f % 7) == 0) {
                    schedule_idx = (schedule_idx + 1) % 3;
                    CHECK(enc_set_live(h, rates[schedule_idx]) == VO_ERR_NONE,
                          "phase: set %d failed", rates[schedule_idx]);
                }
                next_br = rates[schedule_idx];
                gen_complex_frame(pcm, f, 2);
                CHECK(enc_encode_one(h, pcm, 2, out) == 1,
                      "phase: frame %d not produced", f);

                want_avg = phase_step(&ref, next_br, &want_pad);
                {
                    AAC_ENCODER *e = (AAC_ENCODER *) h;
                    CHECK(e->qcKernel.averageBitsTot == want_avg,
                          "phase: sr=%d br=%d frame=%d avgBits %d != %lld",
                          sr, next_br, f, e->qcKernel.averageBitsTot,
                          (long long) want_avg);
                    CHECK(e->qcKernel.padding.paddingRest == ref.R,
                          "phase: sr=%d br=%d frame=%d remainder %lld != %lld",
                          sr, next_br, f,
                          (long long) e->qcKernel.padding.paddingRest,
                          (long long) ref.R);
                    CHECK(ref.R >= 1 && ref.R <= sr,
                          "phase: remainder out of range: %lld", (long long) ref.R);
                }
            }
            check_stream(out, sr, 2, "phase");
            free_stream(out);
            enc_close(h);
        }
    }

    /* Explicit 44.1 kHz / 64 kbit/s overflow regression: r = 33500. */
    {
        int64_t prod = (int64_t) 128 * 64000;
        CHECK(prod % 44100 == 33500, "phase: test premise wrong, r=%lld",
              (long long) (prod % 44100));
        CHECK((short) 33500 < 0, "phase: 33500 no longer overflows Word16");
    }

    if (g_failures == 0)
        printf("AAC_LIVE_CASE phase PASS\n");
}

/* ------------------------------------------------------------------ */
/* Group 2: history                                                   */
/* ------------------------------------------------------------------ */

#define HIST_FRAMES 24
#define HIST_ONSET 8      /* frame carrying the sharp attack */

static void gen_hist_pcm(short *pcm, int frame)
{
    int i, ch;
    if (frame == HIST_ONSET) {
        /* silence up to sample 128, then full-scale alternating material:
         * reliably forces START -> SHORT -> STOP across the next frames */
        for (i = 0; i < FRAME_SAMPLES; i++) {
            int amp = (i >= 128) ? 30000 : 0;
            for (ch = 0; ch < 2; ch++)
                pcm[i * 2 + ch] = amp ? (((i + ch) & 1) ? amp : -amp) : 0;
        }
    } else {
        gen_quiet_frame(pcm, 2);
    }
}

static int window_of(VO_HANDLE h, int ch)
{
    AAC_ENCODER *e = (AAC_ENCODER *) h;
    return e->psyOut.psyOutChannel[ch].windowSequence;
}

static void check_block_history_equal(VO_HANDLE ha, VO_HANDLE hb, int frame,
                                      const char *tag)
{
    AAC_ENCODER *a = (AAC_ENCODER *) ha;
    AAC_ENCODER *b = (AAC_ENCODER *) hb;
    int ch;
    for (ch = 0; ch < 2; ch++) {
        BLOCK_SWITCHING_CONTROL *x = &a->psyKernel.psyData[ch].blockSwitchingControl;
        BLOCK_SWITCHING_CONTROL *y = &b->psyKernel.psyData[ch].blockSwitchingControl;
        CHECK(x->windowSequence == y->windowSequence &&
              x->nextwindowSequence == y->nextwindowSequence &&
              x->attack == y->attack && x->lastattack == y->lastattack &&
              x->attackIndex == y->attackIndex &&
              x->lastAttackIndex == y->lastAttackIndex &&
              x->noOfGroups == y->noOfGroups &&
              x->maxWindowNrg == y->maxWindowNrg &&
              x->accWindowNrg == y->accWindowNrg,
              "history: %s frame %d ch %d block state diverged", tag, frame, ch);
        CHECK(memcmp(x->windowNrg, y->windowNrg, sizeof(x->windowNrg)) == 0 &&
              memcmp(x->windowNrgF, y->windowNrgF, sizeof(x->windowNrgF)) == 0 &&
              memcmp(x->iirStates, y->iirStates, sizeof(x->iirStates)) == 0 &&
              memcmp(x->groupLen, y->groupLen, sizeof(x->groupLen)) == 0,
              "history: %s frame %d ch %d block energy history diverged",
              tag, frame, ch);
        CHECK(memcmp(a->psyKernel.psyData[ch].mdctDelayBuffer,
                     b->psyKernel.psyData[ch].mdctDelayBuffer,
                     BLOCK_SWITCHING_OFFSET * sizeof(Word16)) == 0,
              "history: %s frame %d ch %d MDCT overlap diverged", tag, frame, ch);
        CHECK(memcmp(a->psyKernel.psyData[ch].sfbThresholdnm1,
                     b->psyKernel.psyData[ch].sfbThresholdnm1,
                     MAX_SFB * sizeof(Word32)) == 0,
              "history: %s frame %d ch %d pre-echo threshold diverged",
              tag, frame, ch);
    }
}

static void case_history(void)
{
    /*
     * ref   : constant 64k, never reconfigured
     * live  : 64k -> 96k immediately after the transient
     * twin  : identical PCM and identical schedule as live, so its retained
     *         history must match live frame by frame
     * fresh : freshly opened encoders used only to read reference config and
     *         the initial (post-AacEncOpen) threshold state
     */
    VO_HANDLE ref = enc_open(44100, 2, 64000);
    VO_HANDLE live = enc_open(44100, 2, 64000);
    VO_HANDLE twin = enc_open(44100, 2, 64000);
    VO_HANDLE fresh64 = enc_open(44100, 2, 64000);
    VO_HANDLE fresh96 = enc_open(44100, 2, 96000);
    AdtsStream *ref_out = NULL, *live_out = NULL, *twin_out = NULL;
    short pcm[FRAME_SAMPLES * 2];
    int f, ch, switched = 0, applied = 0, saw_start = 0, saw_short = 0, saw_stop = 0;
    Word16 init_pe_min, init_pe_max;

    CHECK(ref && live && twin && fresh64 && fresh96, "history: open failed");
    if (!(ref && live && twin && fresh64 && fresh96)) goto done;
    ref_out = new_stream();
    live_out = new_stream();
    twin_out = new_stream();
    CHECK(ref_out && live_out && twin_out, "history: alloc failed");
    if (!(ref_out && live_out && twin_out)) goto done;

    init_pe_min = ((AAC_ENCODER *) fresh64)->qcKernel.adjThr.adjThrStateElem.peMin;
    init_pe_max = ((AAC_ENCODER *) fresh64)->qcKernel.adjThr.adjThrStateElem.peMax;

    for (f = 0; f < HIST_FRAMES; f++) {
        AAC_ENCODER *el = (AAC_ENCODER *) live;
        gen_hist_pcm(pcm, f);
        CHECK(enc_encode_one(ref, pcm, 2, ref_out) == 1, "history ref frame");
        CHECK(enc_encode_one(live, pcm, 2, live_out) == 1, "history live frame");
        CHECK(enc_encode_one(twin, pcm, 2, twin_out) == 1, "history twin frame");

        if (window_of(live, 0) == START_WINDOW) saw_start = 1;
        if (window_of(live, 0) == SHORT_WINDOW) saw_short = 1;
        if (window_of(live, 0) == STOP_WINDOW) saw_stop = 1;

        /* switch live and twin immediately after the onset leaves LONG_WINDOW */
        if (!switched && window_of(live, 0) != LONG_WINDOW) {
            Word64 rest = el->qcKernel.padding.paddingRest;
            Word16 res = el->qcKernel.elementBits.bitResLevel;
            ATS_ELEMENT before;

            check_block_history_equal(live, ref, f, "pre-switch live/ref");
            check_block_history_equal(live, twin, f, "pre-switch live/twin");

            before = el->qcKernel.adjThr.adjThrStateElem;
            CHECK(enc_get_live(live) == 64000, "history: pre-switch bitrate");
            CHECK(enc_set_live(live, 96000) == VO_ERR_NONE, "history: live set");
            CHECK(enc_set_live(twin, 96000) == VO_ERR_NONE, "history: twin set");

            /* the request is only stored: no history or budget may move yet */
            CHECK(el->liveBitratePending == 96000 && el->config.bitRate == 64000,
                  "history: request applied before a complete frame");
            CHECK(el->qcKernel.padding.paddingRest == rest &&
                  el->qcKernel.elementBits.bitResLevel == res,
                  "history: request altered reservoir/phase early");
            {
                ATS_ELEMENT after = el->qcKernel.adjThr.adjThrStateElem;
                CHECK(after.peMin == before.peMin && after.peMax == before.peMax &&
                      after.peLast == before.peLast &&
                      after.dynBitsLast == before.dynBitsLast &&
                      after.peCorrectionFactor == before.peCorrectionFactor,
                      "history: request altered threshold history early");
            }
            check_block_history_equal(live, ref, f, "queued live/ref");
            switched = 1;
        }

        /* window selection is signal driven and must stay identical even
         * though live/twin changed bitrate */
        for (ch = 0; ch < 2; ch++) {
            CHECK(window_of(live, ch) == window_of(ref, ch),
                  "history: frame %d ch %d window live %d != ref %d",
                  f, ch, window_of(live, ch), window_of(ref, ch));
            CHECK(window_of(twin, ch) == window_of(ref, ch),
                  "history: frame %d ch %d window twin %d != ref %d",
                  f, ch, window_of(twin, ch), window_of(ref, ch));
        }

        if (switched) {
            ATS_ELEMENT *a = &el->qcKernel.adjThr.adjThrStateElem;

            /* The request is queued in frame f and applied at the start of
             * frame f+1. Validate that transition once. */
            if (!applied && enc_get_live(live) == 96000) {
                applied = 1;
                CHECK(el->liveBitratePending == 0,
                      "history: pending flag not cleared after apply");

                /* threshold history is retained, not re-initialized */
                CHECK(!(a->peMin == init_pe_min && a->peMax == init_pe_max),
                      "history: peMin/peMax were reset to open-time values");
                CHECK(a->peCorrectionFactor >= 85 && a->peCorrectionFactor <= 115,
                      "history: peCorrectionFactor out of range");

                /* psychoacoustic/QC config now matches a fresh 96k instance */
                {
                    AAC_ENCODER *n = (AAC_ENCODER *) fresh96;
                    CHECK(el->psyKernel.psyConfLong.lowpassLine ==
                              n->psyKernel.psyConfLong.lowpassLine &&
                          el->psyKernel.psyConfLong.sfbActive ==
                              n->psyKernel.psyConfLong.sfbActive,
                          "history: long psy config not updated to new bitrate");
                    CHECK(el->psyKernel.psyConfShort.sfbActive ==
                              n->psyKernel.psyConfShort.sfbActive,
                          "history: short psy config not updated to new bitrate");
                    CHECK(el->psyKernel.psyData[0].blockSwitchingControl.invAttackRatio ==
                              n->psyKernel.psyData[0].blockSwitchingControl.invAttackRatio,
                          "history: block-switch attack ratio not updated");
                    CHECK(el->qcKernel.elementBits.chBitrate ==
                              n->qcKernel.elementBits.chBitrate &&
                          el->qcKernel.maxBitFac == n->qcKernel.maxBitFac &&
                          el->qcKernel.elementBits.maxBitResBits ==
                              n->qcKernel.elementBits.maxBitResBits,
                          "history: QC constants not updated to new bitrate");
                }
            } else if (applied) {
                /* retained measurement history keeps evolving, never resets */
                CHECK(a->peMin != init_pe_min || a->peMax != init_pe_max,
                      "history: peMin/peMax reset to open-time values at frame %d", f);
                CHECK(a->peCorrectionFactor >= 85 && a->peCorrectionFactor <= 115,
                      "history: peCorrectionFactor out of range at frame %d", f);
            }

            /* two identically scheduled encoders keep identical history */
            check_block_history_equal(live, twin, f, "post-switch live/twin");
        }
    }

    CHECK(saw_start && saw_short && saw_stop,
          "history: expected START+SHORT+STOP, got %d/%d/%d",
          saw_start, saw_short, saw_stop);
    CHECK(switched, "history: switch never happened");
    CHECK(applied, "history: queued bitrate was never applied");

    /* adaptation history must have evolved away from the open-time state */
    {
        AAC_ENCODER *el = (AAC_ENCODER *) live;
        ATS_ELEMENT *a = &el->qcKernel.adjThr.adjThrStateElem;
        CHECK(a->peMin != init_pe_min || a->peMax != init_pe_max,
              "history: peMin/peMax never evolved from open-time values");
        CHECK(a->peCorrectionFactor >= 85 && a->peCorrectionFactor <= 115,
              "history: final peCorrectionFactor out of range");
    }

    check_stream(ref_out, 44100, 2, "history");
    check_stream(live_out, 44100, 2, "history");
    check_stream(twin_out, 44100, 2, "history");

    /* block-switching bitrate parameter updates across the mono threshold */
    {
        VO_HANDLE mono = enc_open(44100, 1, 16000);
        AAC_ENCODER *m;
        CHECK(mono != NULL, "history: mono open failed");
        if (mono) {
            m = (AAC_ENCODER *) mono;
            CHECK(AacEncSetLiveBitrate(m, 16000) == 0, "history: mono 16k");
            AacEncApplyLiveBitrate(m);
            CHECK(m->psyKernel.psyData[0].blockSwitchingControl.invAttackRatio ==
                      0x072b020c, "history: low-rate attack ratio not selected");
            CHECK(AacEncSetLiveBitrate(m, 32000) == 0, "history: mono 32k");
            AacEncApplyLiveBitrate(m);
            CHECK(m->psyKernel.psyData[0].blockSwitchingControl.invAttackRatio ==
                      0x0ccccccd, "history: high-rate attack ratio not selected");
            enc_close(mono);
        }
    }

done:
    free_stream(ref_out);
    free_stream(live_out);
    free_stream(twin_out);
    enc_close(ref);
    enc_close(live);
    enc_close(twin);
    enc_close(fresh64);
    enc_close(fresh96);
    if (g_failures == 0)
        printf("AAC_LIVE_CASE history PASS\n");
}

/* ------------------------------------------------------------------ */
/* Group 3: reservoir                                                 */
/* ------------------------------------------------------------------ */

static void case_reservoir(void)
{
    static const int br_list[4] = { 56000, 64000, 96000, 128000 };
    VO_HANDLE h = enc_open(48000, 2, 64000);
    AdtsStream *sink = NULL;
    short pcm[FRAME_SAMPLES * 2];
    int f, i;
    int16_t level_quiet, level_loud;

    CHECK(h != NULL, "reservoir: open failed");
    if (!h) goto done;
    sink = new_stream();
    CHECK(sink != NULL, "reservoir: stream alloc failed");
    if (!sink) goto done;

    /* quiet material accumulates reserve */
    for (f = 0; f < 20; f++) {
        gen_quiet_frame(pcm, 2);
        CHECK(enc_encode_one(h, pcm, 2, sink) == 1, "reservoir quiet frame");
    }
    {
        AAC_ENCODER *e = (AAC_ENCODER *) h;
        level_quiet = e->qcKernel.elementBits.bitResLevel;
        CHECK(level_quiet > 0, "reservoir: quiet section did not save bits");
        CHECK(level_quiet <= e->qcKernel.elementBits.maxBitResBits,
              "reservoir: fill above capacity");
    }

    /* white-box capacity/budget checks across rates, fill must be retained */
    for (i = 0; i < 4; i++) {
        AAC_ENCODER *e = (AAC_ENCODER *) h;
        int16_t before = e->qcKernel.elementBits.bitResLevel;
        int64_t avg = (int64_t) br_list[i] * 1024 / 48000;
        int64_t C = 8 * ((MAXBITS_COEF * 2 - avg) / 8);
        int16_t expected;

        CHECK(AacEncSetLiveBitrate(e, br_list[i]) == 0, "reservoir: set");
        AacEncApplyLiveBitrate(e);
        CHECK(e->config.bitRate == br_list[i], "reservoir: config bitrate");
        CHECK(e->qcKernel.elementBits.maxBitResBits == C,
              "reservoir: br=%d capacity %d != %lld", br_list[i],
              e->qcKernel.elementBits.maxBitResBits, (long long) C);
        CHECK(e->qcKernel.maxBitsTot == MAXBITS_COEF * 2,
              "reservoir: maxBits wrong");
        CHECK(e->qcKernel.averageBitsTot == avg,
              "reservoir: averageBits %d != %lld",
              e->qcKernel.averageBitsTot, (long long) avg);
        CHECK(e->qcKernel.elementBits.chBitrate == br_list[i] / 2,
              "reservoir: per-channel bitrate wrong");
        expected = before;
        if (expected > C)
            expected = (int16_t) C;
        CHECK(e->qcKernel.elementBits.bitResLevel == expected,
              "reservoir: fill not retained/clipped correctly");
    }

    /* complex material spends the reserve */
    for (f = 0; f < 20; f++) {
        gen_complex_frame(pcm, 100 + f, 2);
        CHECK(enc_encode_one(h, pcm, 2, sink) == 1, "reservoir loud frame");
    }
    {
        AAC_ENCODER *e = (AAC_ENCODER *) h;
        level_loud = e->qcKernel.elementBits.bitResLevel;
        CHECK(level_loud < level_quiet,
              "reservoir: complex section did not spend reserve (%d >= %d)",
              level_loud, level_quiet);
    }
    check_stream(sink, 48000, 2, "reservoir");

    /* actual quantization budget follows the bitrate on identical PCM */
    {
        VO_HANDLE lo = enc_open(44100, 2, 48000);
        VO_HANDLE hi = enc_open(44100, 2, 128000);
        AdtsStream *lo_out, *hi_out;
        int total_lo = 0, total_hi = 0;
        CHECK(lo && hi, "reservoir: comparison open failed");
        if (lo && hi) {
            lo_out = new_stream();
            hi_out = new_stream();
            CHECK(lo_out && hi_out, "reservoir: compare alloc failed");
            if (lo_out && hi_out) {
                for (f = 0; f < 24; f++) {
                    gen_complex_frame(pcm, f, 2);
                    CHECK(enc_encode_one(lo, pcm, 2, lo_out) == 1, "lo frame");
                    CHECK(enc_encode_one(hi, pcm, 2, hi_out) == 1, "hi frame");
                }
                for (f = 0; f < lo_out->count; f++)
                    total_lo += lo_out->frame[f].len;
                for (f = 0; f < hi_out->count; f++)
                    total_hi += hi_out->frame[f].len;
                CHECK(total_hi > total_lo,
                      "reservoir: high bitrate used no more bytes (%d vs %d)",
                      total_hi, total_lo);
                check_stream(lo_out, 44100, 2, "reservoir");
                check_stream(hi_out, 44100, 2, "reservoir");
            }
            free_stream(lo_out);
            free_stream(hi_out);
        }
        enc_close(lo);
        enc_close(hi);
    }

done:
    free_stream(sink);
    enc_close(h);
    if (g_failures == 0)
        printf("AAC_LIVE_CASE reservoir PASS\n");
}

/* ------------------------------------------------------------------ */
/* Group 4: streaming                                                 */
/* ------------------------------------------------------------------ */

static void case_streaming(void)
{
    static const int total_frames = 96;
    static const int irregular[6] = { 1024, 512, 2048, 768, 1536, 256 };
    static const int schedule[4] = { 24, 48, 72, 88 };
    VO_HANDLE whole = enc_open(44100, 2, 64000);
    VO_HANDLE half = enc_open(44100, 2, 64000);
    VO_HANDLE jag1 = enc_open(44100, 2, 64000);
    VO_HANDLE jag2 = enc_open(44100, 2, 64000);
    AdtsStream *s_whole = NULL, *s_half = NULL, *s_jag1 = NULL, *s_jag2 = NULL;
    short *pcm;
    int total_bytes, off, f, k, chunk_idx = 0;
    int scheduled[4] = { 0, 0, 0, 0 };

    CHECK(whole && half && jag1 && jag2, "streaming: open failed");
    if (!(whole && half && jag1 && jag2)) goto done;
    s_whole = new_stream();
    s_half = new_stream();
    s_jag1 = new_stream();
    s_jag2 = new_stream();
    CHECK(s_whole && s_half && s_jag1 && s_jag2, "streaming: alloc failed");
    if (!(s_whole && s_half && s_jag1 && s_jag2)) goto done;

    total_bytes = total_frames * FRAME_SAMPLES * 2 * (int) sizeof(short);
    pcm = (short *) malloc((size_t) total_bytes);
    CHECK(pcm != NULL, "streaming: malloc failed");
    if (!pcm) goto done;
    for (f = 0; f < total_frames; f++)
        gen_complex_frame(pcm + f * FRAME_SAMPLES * 2, f, 2);

    /* instance A: whole 1024-sample frames, requests between frames */
    for (f = 0; f < total_frames; f++) {
        for (k = 0; k < 4; k++)
            if (f == schedule[k])
                CHECK(enc_set_live(whole, k % 2 ? 64000 : 96000) == VO_ERR_NONE,
                      "streaming: whole set");
        CHECK(enc_encode_one(whole, pcm + f * FRAME_SAMPLES * 2, 2,
                             s_whole) == 1, "streaming: whole frame");
    }

    /* instance B: half frames, request issued while half a frame is buffered */
    off = 0;
    for (f = 0; f < total_frames; f++) {
        int half_bytes = FRAME_SAMPLES * (int) sizeof(short); /* per half */
        for (k = 0; k < 4; k++)
            if (f == schedule[k]) {
                /* before the completing half: must not apply early */
                CHECK(enc_set_live(half, k % 2 ? 64000 : 96000) == VO_ERR_NONE,
                      "streaming: half set");
            }
        CHECK(enc_feed_drain(half, (unsigned char *) pcm + off,
                             half_bytes, s_half) >= 0, "streaming half a");
        off += half_bytes;
        CHECK(enc_feed_drain(half, (unsigned char *) pcm + off,
                             half_bytes, s_half) >= 0, "streaming half b");
        off += half_bytes;
    }

    /*
     * Two more instances driven with irregular even-byte chunks; requests
     * are issued as soon as the corresponding frame count is complete, so
     * the next encoded frame uses the new budget. The two receive identical
     * chunks and requests and must stay identical.
     */
    off = 0;
    while (off < total_bytes) {
        int chunk = irregular[chunk_idx % 6];
        int before1, j;
        chunk_idx++;
        if (off + chunk > total_bytes)
            chunk = total_bytes - off;
        before1 = s_jag1->count;
        CHECK(enc_feed_drain(jag1, (unsigned char *) pcm + off, chunk,
                             s_jag1) >= 0, "streaming jag1");
        CHECK(enc_feed_drain(jag2, (unsigned char *) pcm + off, chunk,
                             s_jag2) >= 0, "streaming jag2");
        off += chunk;
        for (j = 0; j < 4; j++) {
            if (!scheduled[j] && s_jag1->count >= schedule[j] &&
                s_jag2->count >= schedule[j] && before1 < schedule[j]) {
                CHECK(enc_set_live(jag1, j % 2 ? 64000 : 96000) == VO_ERR_NONE,
                      "streaming: jag1 set");
                CHECK(enc_set_live(jag2, j % 2 ? 64000 : 96000) == VO_ERR_NONE,
                      "streaming: jag2 set");
                scheduled[j] = 1;
            }
        }
    }

    CHECK(s_whole->count == total_frames, "streaming: whole count %d",
          s_whole->count);
    CHECK(s_half->count == total_frames, "streaming: half count %d", s_half->count);
    CHECK(s_jag1->count == total_frames, "streaming: jag1 count %d", s_jag1->count);
    CHECK(s_jag2->count == total_frames, "streaming: jag2 count %d", s_jag2->count);

    for (f = 0; f < total_frames; f++) {
        CHECK(s_half->frame[f].len == s_whole->frame[f].len,
              "streaming: frame %d half length %d != %d", f,
              s_half->frame[f].len, s_whole->frame[f].len);
        CHECK(s_jag1->frame[f].len == s_whole->frame[f].len,
              "streaming: frame %d jag1 length %d != %d", f,
              s_jag1->frame[f].len, s_whole->frame[f].len);
        CHECK(s_jag2->frame[f].len == s_whole->frame[f].len,
              "streaming: frame %d jag2 length %d != %d", f,
              s_jag2->frame[f].len, s_whole->frame[f].len);
        CHECK(memcmp(s_half->frame[f].data, s_whole->frame[f].data,
                     (size_t) s_whole->frame[f].len) == 0,
              "streaming: frame %d half bytes differ", f);
        CHECK(memcmp(s_jag1->frame[f].data, s_whole->frame[f].data,
                     (size_t) s_whole->frame[f].len) == 0,
              "streaming: frame %d jag1 bytes differ", f);
        CHECK(memcmp(s_jag2->frame[f].data, s_jag1->frame[f].data,
                     (size_t) s_jag1->frame[f].len) == 0,
              "streaming: frame %d jag2 bytes differ", f);
    }
    check_stream(s_whole, 44100, 2, "streaming");
    check_stream(s_half, 44100, 2, "streaming");
    check_stream(s_jag1, 44100, 2, "streaming");
    check_stream(s_jag2, 44100, 2, "streaming");

    free(pcm);
done:
    free_stream(s_whole);
    free_stream(s_half);
    free_stream(s_jag1);
    free_stream(s_jag2);
    enc_close(whole);
    enc_close(half);
    enc_close(jag1);
    enc_close(jag2);
    if (g_failures == 0)
        printf("AAC_LIVE_CASE streaming PASS\n");
}

/* ------------------------------------------------------------------ */
/* Group 5: compatibility                                             */
/* ------------------------------------------------------------------ */

static void case_compatibility(void)
{
    VO_HANDLE ref = enc_open(44100, 2, 64000);
    VO_HANDLE same = enc_open(44100, 2, 64000);
    VO_HANDLE old = enc_open(44100, 2, 64000);
    AdtsStream *ref_out = NULL, *same_out = NULL, *old_out = NULL, *one = NULL;
    short pcm[FRAME_SAMPLES * 2];
    int f;
    static const int bad_rates[7] = { 0, -1, 7999, 330000, 529201, 2147483647,
                                      4000 };

    CHECK(ref && same && old, "compat: open failed");
    if (!ref || !same || !old) goto done;
    ref_out = new_stream();
    same_out = new_stream();
    old_out = new_stream();
    CHECK(ref_out && same_out && old_out, "compat: alloc failed");
    if (!(ref_out && same_out && old_out)) goto done;

    for (f = 0; f < 96; f++) {
        gen_complex_frame(pcm, f, 2);
        CHECK(enc_encode_one(ref, pcm, 2, ref_out) == 1, "compat ref frame");
        CHECK(enc_encode_one(same, pcm, 2, same_out) == 1, "compat same frame");
        CHECK(enc_encode_one(old, pcm, 2, old_out) == 1, "compat old frame");

        if (f == 40) {
            /* same-value live update must be a no-op, also cancels pending */
            CHECK(enc_set_live(same, 96000) == VO_ERR_NONE, "compat: queue 96k");
            CHECK(enc_set_live(same, 64000) == VO_ERR_NONE, "compat: same value");
            {
                AAC_ENCODER *e = (AAC_ENCODER *) same;
                CHECK(e->liveBitratePending == 0,
                      "compat: same value did not cancel pending request");
            }

            /* old interface reinitializes: history and phase are reset */
            {
                AACENC_PARAM p;
                AAC_ENCODER *e = (AAC_ENCODER *) old;
                memset(&p, 0, sizeof(p));
                p.sampleRate = 44100; p.bitRate = 64000; p.nChannels = 2;
                p.adtsUsed = 1;
                CHECK(g_api.SetParam(old, VO_PID_AAC_ENCPARAM, &p) == VO_ERR_NONE,
                      "compat: old ENCPARAM failed");
                CHECK(e->qcKernel.padding.paddingRest == 44100,
                      "compat: old interface did not reset padding phase");
                CHECK(e->qcKernel.adjThr.adjThrStateElem.peLast == 0 &&
                      e->qcKernel.adjThr.adjThrStateElem.dynBitsLast == 0 &&
                      e->qcKernel.adjThr.adjThrStateElem.peCorrectionFactor == 100,
                      "compat: old interface did not reset threshold history");
                CHECK(e->liveBitratePending == 0,
                      "compat: reopen left a pending live request");
            }
        }
    }

    /* same-value live run must be bit exact against the untouched run */
    CHECK(ref_out->count == same_out->count, "compat: frame counts differ");
    for (f = 0; f < ref_out->count; f++) {
        CHECK(same_out->frame[f].len == ref_out->frame[f].len &&
              memcmp(same_out->frame[f].data, ref_out->frame[f].data,
                     (size_t) ref_out->frame[f].len) == 0,
              "compat: same-value update changed frame %d", f);
    }
    check_stream(old_out, 44100, 2, "compat");

    /* invalid arguments */
    for (f = 0; f < 7; f++) {
        CHECK(enc_set_live(ref, bad_rates[f]) == VO_ERR_INVALID_ARG,
              "compat: bad rate %d accepted", bad_rates[f]);
    }
    {
        VO_S32 legal = 64000;
        VO_S32 zero = 0;
        CHECK(g_api.SetParam(ref, VO_PID_AAC_LIVE_BITRATE, NULL) ==
                  VO_ERR_INVALID_ARG, "compat: null pointer accepted");
        CHECK(g_api.SetParam(NULL, VO_PID_AAC_LIVE_BITRATE, &legal) ==
                  VO_ERR_INVALID_ARG, "compat: null handle accepted");
        CHECK(g_api.GetParam(NULL, VO_PID_AAC_LIVE_BITRATE, &zero) ==
                  VO_ERR_INVALID_ARG, "compat: null handle get accepted");
        CHECK(g_api.GetParam(ref, VO_PID_AAC_LIVE_BITRATE, NULL) ==
                  VO_ERR_INVALID_ARG, "compat: null get pointer accepted");
    }

    /* invalid request must not overwrite a queued legal value */
    CHECK(enc_set_live(ref, 96000) == VO_ERR_NONE, "compat: queue legal");
    CHECK(enc_set_live(ref, 1) == VO_ERR_INVALID_ARG, "compat: illegal accepted");
    /* several legal requests in a row: the last one must win */
    CHECK(enc_set_live(ref, 64000) == VO_ERR_NONE, "compat: queue 64k");
    CHECK(enc_set_live(ref, 128000) == VO_ERR_NONE, "compat: queue 128k");
    one = new_stream();
    CHECK(one != NULL, "compat: one alloc failed");
    if (one) {
        AAC_ENCODER *e = (AAC_ENCODER *) ref;
        gen_complex_frame(pcm, 200, 2);
        CHECK(enc_encode_one(ref, pcm, 2, one) == 1, "compat: apply frame");
        CHECK(e->config.bitRate == 128000,
              "compat: last queued value did not win (got %d)", e->config.bitRate);
        CHECK(enc_get_live(ref) == 128000, "compat: effective bitrate wrong");
        free_stream(one);
    }

    /* partial input must never apply a pending change or lose the half frame */
    {
        VO_HANDLE partial = enc_open(44100, 2, 64000);
        AdtsStream *pout = new_stream();
        AAC_ENCODER *e = (AAC_ENCODER *) partial;
        short half[FRAME_SAMPLES]; /* one channel-stride worth of samples */
        int got;
        CHECK(partial && pout, "compat: partial alloc failed");
        if (partial && pout) {
            gen_complex_frame(pcm, 300, 2);
            memcpy(half, pcm, sizeof(half));
            CHECK(enc_set_live(partial, 96000) == VO_ERR_NONE,
                  "compat: partial set failed");
            /* feed only half a frame's samples: must report buffer small */
            got = enc_feed_drain(partial, (unsigned char *) half, sizeof(half), pout);
            CHECK(got == 0 && pout->count == 0,
                  "compat: half input produced a frame");
            CHECK(e->config.bitRate == 64000 && e->liveBitratePending == 96000,
                  "compat: pending rate applied on partial input");
            CHECK(e->intlen > 0, "compat: half frame was not buffered");
            /* completing the frame applies the new budget and yields output */
            memcpy(half, pcm + FRAME_SAMPLES, sizeof(half));
            got = enc_feed_drain(partial, (unsigned char *) half, sizeof(half), pout);
            CHECK(got == 1 && pout->count == 1,
                  "compat: completing half did not produce one frame");
            CHECK(enc_get_live(partial) == 96000 && e->liveBitratePending == 0,
                  "compat: pending rate not applied on the completed frame");
            check_stream(pout, 44100, 2, "compat");
            free_stream(pout);
        }
        enc_close(partial);
    }

done:
    free_stream(ref_out);
    free_stream(same_out);
    free_stream(old_out);
    enc_close(ref);
    enc_close(same);
    enc_close(old);
    if (g_failures == 0)
        printf("AAC_LIVE_CASE compatibility PASS\n");
}

int main(void)
{
    api_setup();

    case_phase();
    case_history();
    case_reservoir();
    case_streaming();
    case_compatibility();

    if (g_failures) {
        fprintf(stderr, "AAC_LIVE: %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("AAC_LIVE: all groups passed\n");
    return 0;
}
