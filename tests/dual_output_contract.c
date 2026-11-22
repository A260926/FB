/* Regression tests for the optional dual-output Speex decoder.
 *
 * A dual-output decoder must behave exactly like two independent legacy
 * decoders (one permanently un-enhanced, one permanently enhanced) fed the
 * same bit-stream, while only reading/unfolding each frame once.
 */
#include <speex/speex.h>
#include <speex/speex_bits.h>
#include <speex/speex_callbacks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

/* Observation counters defined in libspeex (bits.c / cb_search.c) for the
 * test build.  They never affect decoded output. */
extern unsigned long speex_test_bits_unpack_count;
extern unsigned long speex_test_bits_advance_count;
extern unsigned long speex_test_codebook_unfold_count;

#define MAX_FRAME 640

/* ---------- deterministic test signal generation (real encoder) ---------- */

static unsigned int rng_state;
static void rng_seed(unsigned int s) { rng_state = s ? s : 1u; }
static unsigned int rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* Generates one frame of a continuous, non-trivial signal for a mode. */
static void gen_frame(short *out, int n, int frame, int mode, int active) {
    int i, rate = mode == 0 ? 8000 : (mode == 1 ? 16000 : 32000);
    double base = 3000.0;
    for (i = 0; i < n; i++) {
        double t = (double)(frame * n + i) / (double)rate;
        double v;
        if (!active) {
            /* Silence (for DTX entry). */
            v = 0.0;
        } else {
            v = base * sin(2.0 * M_PI * 440.0 * t +
                          1.7 * sin(2.0 * M_PI * 0.7 * t))
              + 1800.0 * sin(2.0 * M_PI * 1234.0 * t)
              + 900.0 * sin(2.0 * M_PI * 2600.0 * t)
              + (double)((int)(rng_next() % 401) - 200);
            base = 3400.0 - 400.0 * sin(2.0 * M_PI * t * 3.0);
            if (base < 300.0) base = 300.0;
            if (v > 32000.0) v = 32000.0;
            if (v < -32000.0) v = -32000.0;
        }
        out[i] = (short)(v >= 0 ? v + 0.5 : v - 0.5);
    }
}

/* ---------- bit capture ---------- */

typedef struct {
    char data[8192];
    int nbytes;
    int ret;              /* encoder return value */
    int lost;             /* was the packet deliberately dropped */
} Packet;

static void encode_packet(void *enc, short *frame, int n, Packet *p, int drop) {
    SpeexBits b;
    (void)n;
    speex_bits_init(&b);
    speex_bits_reset(&b);
    p->ret = speex_encode_int(enc, frame, &b);
    p->lost = drop || (p->ret == 0);
    p->nbytes = speex_bits_write(&b, p->data, sizeof p->data);
    speex_bits_destroy(&b);
}

/* ---------- reference rendering ---------- */

/* Two legacy decoders, one raw and one enhanced, advanced in lock step. */
typedef struct {
    void *raw;
    void *enh;
    int n;
    float raw_out[MAX_FRAME];
    float enh_out[MAX_FRAME];
} Ref;

static void ref_open(Ref *r, const SpeexMode *mode) {
    spx_int32_t en = 0, on = 1;
    memset(r, 0, sizeof *r);
    r->raw = speex_decoder_init(mode);
    r->enh = speex_decoder_init(mode);
    speex_decoder_ctl(r->raw, SPEEX_SET_ENH, &en);
    speex_decoder_ctl(r->enh, SPEEX_SET_ENH, &on);
    speex_decoder_ctl(r->raw, SPEEX_GET_FRAME_SIZE, &r->n);
}

static void ref_close(Ref *r) {
    speex_decoder_destroy(r->raw);
    speex_decoder_destroy(r->enh);
}

/* Advance the reference by one packet (lost => bits == NULL). */
static int ref_step(Ref *r, const Packet *p) {
    SpeexBits br, be;
    int a, b;
    speex_bits_init(&br);
    speex_bits_init(&be);
    if (p->lost) {
        a = speex_decode(r->raw, NULL, r->raw_out);
        b = speex_decode(r->enh, NULL, r->enh_out);
    } else {
        speex_bits_read_from(&br, p->data, p->nbytes);
        speex_bits_rewind(&br);
        speex_bits_read_from(&be, p->data, p->nbytes);
        speex_bits_rewind(&be);
        a = speex_decode(r->raw, &br, r->raw_out);
        b = speex_decode(r->enh, &be, r->enh_out);
    }
    speex_bits_destroy(&br);
    speex_bits_destroy(&be);
    return a == 0 && b == 0 ? 0 : -1;
}

/* ---------- comparison helpers ---------- */

static int float_close(float a, float b) {
    double tol = 1e-4 + 2e-6 * fabs((double)b);
    return fabs((double)a - (double)b) <= tol;
}

static int cmp_float(const float *got, const float *ref, int n, const char *what) {
    int i;
    for (i = 0; i < n; i++) {
        if (!float_close(got[i], ref[i])) {
            fprintf(stderr, "  %s[%d]: got %.9g want %.9g\n", what, i, got[i], ref[i]);
            return 0;
        }
    }
    return 1;
}

static int cmp_int(const spx_int16_t *got, const spx_int16_t *ref, int n, const char *what) {
    int i;
    for (i = 0; i < n; i++) {
        if (got[i] != ref[i]) {
            fprintf(stderr, "  %s[%d]: got %d want %d\n", what, i, got[i], ref[i]);
            return 0;
        }
    }
    return 1;
}

/* The float reference rendered to what speex_decode_int must yield. */
static void quantize_ref(const float *in, spx_int16_t *out, int n) {
    int i;
    for (i = 0; i < n; i++) {
        float v = in[i];
        if (v > 32767.f) out[i] = 32767;
        else if (v < -32768.f) out[i] = -32768;
        else out[i] = (spx_int16_t)floor(.5 + v);
    }
}

/* ---------- callback counting ---------- */

static int g_callback_count;
static int count_user_callback(SpeexBits *bits, void *state, void *data) {
    int req_size;
    (void)state; (void)data;
    g_callback_count++;
    /* Same payload layout as the default user handler, so the surrounding
     * frame still parses exactly as it does without a registered handler. */
    req_size = speex_bits_unpack_unsigned(bits, 4);
    speex_bits_advance(bits, 5 + 8 * req_size);
    return 0;
}

/* Builds a packet containing one user in-band request in the narrowband block,
 * before the wideband layer(s), without byte-aligning the control message.
 * Returns the number of bytes written.
 *
 * The narrowband block is: 0 (NB marker), then optional in-band requests,
 * then the 4-bit NB sub-mode and its payload.  A wideband mode then appends
 * "1" + 3-bit SB sub-mode + payload per layer.  We reuse the encoder's own
 * frame for the narrowband/sub-mode layout and splice the request in after
 * the NB marker.  `mode` selects 0/1/2 so the correct number of WB layers is
 * appended for modes the encoder is not a WB/UWB encoder for. */
static int build_inband_packet(void *enc, short *frame, char *out, int maxlen,
                               int *enc_ret, int mode) {
    SpeexBits inner, outer;
    int nb_bits, i, layer;
    int wb_layers = (mode == 0) ? 0 : (mode == 1 ? 1 : 2);
    unsigned char frame_bits[8192];

    speex_bits_init(&inner);
    speex_bits_reset(&inner);
    *enc_ret = speex_encode_int(enc, frame, &inner);
    speex_bits_rewind(&inner);
    nb_bits = inner.nbBits;
    /* Snapshot the encoder frame so it can be replayed verbatim. */
    for (i = 0; i < nb_bits; i++)
        frame_bits[i] = speex_bits_unpack_unsigned(&inner, 1);

    speex_bits_init(&outer);
    speex_bits_reset(&outer);
    /* Re-emit the encoder frame, splicing the user request in right after
       the leading narrowband marker.  The request is m=13 followed by the
       default user-handler layout of 4-bit size + payload, all unaligned. */
    speex_bits_pack(&outer, frame_bits[0], 1); /* NB marker */
    speex_bits_pack(&outer, 13, 4);
    speex_bits_pack(&outer, 2, 4);       /* request size = 2 bytes */
    /* The default user handler, after reading the 4-bit size, skips
       5 + 8*size bits.  Supply exactly that many (the leading 5 are the
       fixed preamble of the user request), keeping the message unaligned. */
    speex_bits_pack(&outer, 0x15, 5);
    speex_bits_pack(&outer, 0x5a, 8);
    speex_bits_pack(&outer, 0xa5, 8);
    /* Each decoder iteration reads a narrowband marker bit followed by a
       4-bit mode.  After the in-band request the decoder loops and expects
       exactly that again, so the encoder frame is copied in full (from its
       own marker) right after the control message. */
    for (i = 0; i < nb_bits; i++)
        speex_bits_pack(&outer, frame_bits[i], 1);

    /* For a narrowband encoder, add synthetic wideband layers so that the WB
     * and UWB decoders have something to consume.  Sub-mode 1 carries 36
     * bits per wideband frame. */
    for (layer = 0; layer < wb_layers; layer++) {
        int b;
        speex_bits_pack(&outer, 1, 1);
        speex_bits_pack(&outer, 1, 3);
        for (b = 0; b < 36 - 4; b++) speex_bits_pack(&outer, 0, 1);
    }

    speex_bits_rewind(&outer);
    {
        int n = speex_bits_write(&outer, out, maxlen);
        speex_bits_destroy(&outer);
        speex_bits_destroy(&inner);
        return n;
    }
}

/* ---------- shared fixture ---------- */

/* Builds a realistic packet stream for a mode: speech with per-frame quality
 * changes, an interleaved instance, DTX entry, and single/continuous loss. */
#define STREAM_FRAMES 40
typedef struct {
    Packet pk[STREAM_FRAMES];
    int quality[STREAM_FRAMES];
    int nframes;
} Stream;

static void build_stream(Stream *s, int mode) {
    const SpeexMode *sm = speex_lib_get_mode(mode);
    void *enc = speex_encoder_init(sm);
    void *enc2 = speex_encoder_init(sm);
    short frame[MAX_FRAME];
    int n = 0, f;
    spx_int32_t dtx_on = 1, vbr_on = 1, q;
    speex_encoder_ctl(enc, SPEEX_GET_FRAME_SIZE, &n);
    speex_encoder_ctl(enc, SPEEX_SET_VBR, &vbr_on);
    speex_encoder_ctl(enc, SPEEX_SET_DTX, &dtx_on);
    speex_encoder_ctl(enc2, SPEEX_GET_FRAME_SIZE, &n);
    speex_encoder_ctl(enc2, SPEEX_SET_VBR, &vbr_on);
    speex_encoder_ctl(enc2, SPEEX_SET_DTX, &dtx_on);
    rng_seed(0x1234567u + (unsigned)mode * 7919u);
    memset(s, 0, sizeof *s);
    for (f = 0; f < STREAM_FRAMES; f++) {
        int active = f < 26;             /* frames 26+ are silence -> DTX */
        int drop = (f == 30 || f == 31 || f == 32); /* continuous loss */
        if (f == 20) drop = 1;           /* single loss */
        gen_frame(frame, n, f, mode, active);
        /* Per-frame quality changes: alternate between two encoders (also
         * exercises interleaved instances via enc2 on some frames). */
        s->quality[f] = 2 + (f * 3 + mode) % 9;
        if (f % 5 == 3) {
            q = s->quality[f];
            speex_encoder_ctl(enc2, SPEEX_SET_QUALITY, &q);
            encode_packet(enc2, frame, n, &s->pk[f], drop);
        } else {
            q = s->quality[f];
            speex_encoder_ctl(enc, SPEEX_SET_QUALITY, &q);
            encode_packet(enc, frame, n, &s->pk[f], drop);
        }
        if (f == 27) {
            /* Switch the primary encoder to a different quality band. */
            q = 8; speex_encoder_ctl(enc, SPEEX_SET_QUALITY, &q);
        }
    }
    s->nframes = STREAM_FRAMES;
    speex_encoder_destroy(enc);
    speex_encoder_destroy(enc2);
}

/* Runs the reference decoders over the whole stream, recording per-frame
 * outputs.  Caller supplies storage [STREAM_FRAMES][MAX_FRAME]. */
static void reference_run(Ref *r, const Stream *s, float *raw_hist, float *enh_hist) {
    int f;
    for (f = 0; f < s->nframes; f++) {
        ref_step(r, &s->pk[f]);
        memcpy(raw_hist + (size_t)f * MAX_FRAME, r->raw_out, MAX_FRAME * sizeof(float));
        memcpy(enh_hist + (size_t)f * MAX_FRAME, r->enh_out, MAX_FRAME * sizeof(float));
    }
}

/* =====================  DualPCM cases  ===================== */

/* Two full decoders, no losses: both outputs equal their references. */
static int case_dual_full_stack(void) {
    int mode;
    for (mode = 0; mode < 3; mode++) {
        const SpeexMode *sm = speex_lib_get_mode(mode);
        Stream s;
        void *dec = speex_decoder_init(sm);
        Ref ref;
        int f, n = 0, differs = 0;
        spx_int32_t on = 1;
        float raw[MAX_FRAME], enh[MAX_FRAME];

        TASK_CHECK(speex_decoder_ctl(dec, SPEEX_SET_DUAL_OUTPUT, &on) == 0);
        TASK_CHECK(speex_decoder_ctl(dec, SPEEX_GET_DUAL_OUTPUT, &f) == 0 && f == 1);
        speex_decoder_ctl(dec, SPEEX_GET_FRAME_SIZE, &n);
        build_stream(&s, mode);
        ref_open(&ref, sm);
        for (f = 0; f < s.nframes; f++) {
            SpeexBits b;
            int ret;
            speex_bits_init(&b);
            speex_bits_read_from(&b, s.pk[f].data, s.pk[f].nbytes);
            speex_bits_rewind(&b);
            if (s.pk[f].lost) {
                ref_step(&ref, &s.pk[f]);
                ret = speex_decode_dual(dec, NULL, raw, enh);
            } else {
                SpeexBits braw, benh;
                speex_bits_init(&braw); speex_bits_init(&benh);
                speex_bits_read_from(&braw, s.pk[f].data, s.pk[f].nbytes); speex_bits_rewind(&braw);
                speex_bits_read_from(&benh, s.pk[f].data, s.pk[f].nbytes); speex_bits_rewind(&benh);
                speex_decode(ref.raw, &braw, ref.raw_out);
                speex_decode(ref.enh, &benh, ref.enh_out);
                speex_bits_destroy(&braw); speex_bits_destroy(&benh);
                ret = speex_decode_dual(dec, &b, raw, enh);
            }
            TASK_CHECK(ret == 0);
            TASK_CHECK(cmp_float(raw, ref.raw_out, n, "raw"));
            TASK_CHECK(cmp_float(enh, ref.enh_out, n, "enh"));
            {
                int i;
                for (i = 0; i < n; i++)
                    if (fabs((double)raw[i] - (double)enh[i]) > 1.0) differs = 1;
            }
            speex_bits_destroy(&b);
        }
        /* The two renders must genuinely differ somewhere, otherwise the
         * comparisons above would not prove much. */
        TASK_CHECK(differs);
        ref_close(&ref);
        speex_decoder_destroy(dec);
    }
    return 0;
}

/* Bit cursor parity and single-pass reads. */
static int case_dual_single_pass(void) {
    int mode;
    for (mode = 0; mode < 3; mode++) {
        const SpeexMode *sm = speex_lib_get_mode(mode);
        Stream s;
        void *dec = speex_decoder_init(sm);
        void *legacy = speex_decoder_init(sm);
        int f, n = 0;
        spx_int32_t on = 1, zero = 0;
        float raw[MAX_FRAME], enh[MAX_FRAME], dummy[MAX_FRAME];

        speex_decoder_ctl(dec, SPEEX_SET_DUAL_OUTPUT, &on);
        speex_decoder_ctl(legacy, SPEEX_SET_DUAL_OUTPUT, &zero);
        speex_decoder_ctl(dec, SPEEX_GET_FRAME_SIZE, &n);
        build_stream(&s, mode);
        for (f = 0; f < s.nframes; f++) {
            SpeexBits bd, bl;
            unsigned long u0, a0, c0, u1, a1, c1, ul, al, cl;
            int rd;
            if (s.pk[f].lost) continue;
            speex_bits_init(&bd); speex_bits_init(&bl);
            speex_bits_read_from(&bd, s.pk[f].data, s.pk[f].nbytes); speex_bits_rewind(&bd);
            speex_bits_read_from(&bl, s.pk[f].data, s.pk[f].nbytes); speex_bits_rewind(&bl);

            u0 = speex_test_bits_unpack_count; a0 = speex_test_bits_advance_count; c0 = speex_test_codebook_unfold_count;
            rd = speex_decode_dual(dec, &bd, raw, enh);
            u1 = speex_test_bits_unpack_count; a1 = speex_test_bits_advance_count; c1 = speex_test_codebook_unfold_count;
            TASK_CHECK(rd == 0);

            ul = speex_test_bits_unpack_count; al = speex_test_bits_advance_count; cl = speex_test_codebook_unfold_count;
            speex_decode(legacy, &bl, dummy);
            ul = speex_test_bits_unpack_count - ul;
            al = speex_test_bits_advance_count - al;
            cl = speex_test_codebook_unfold_count - cl;

            /* Dual must not read or unfold more than the legacy decoder. */
            TASK_CHECK(u1 - u0 == ul);
            TASK_CHECK(a1 - a0 == al);
            TASK_CHECK(c1 - c0 == cl);
            /* and the bit cursor must end at the same place. */
            TASK_CHECK(bd.charPtr == bl.charPtr && bd.bitPtr == bl.bitPtr && bd.overflow == bl.overflow);
            speex_bits_destroy(&bd); speex_bits_destroy(&bl);
        }
        speex_decoder_destroy(dec);
        speex_decoder_destroy(legacy);
    }
    return 0;
}

/* Dual output disabled: -2, no bits consumed, no output written. */
static int case_dual_disabled(void) {
    int mode;
    for (mode = 0; mode < 3; mode++) {
        const SpeexMode *sm = speex_lib_get_mode(mode);
        void *dec = speex_decoder_init(sm);
        Stream s;
        int f, n = 0;
        float raw[MAX_FRAME], enh[MAX_FRAME];
        float sentinel = 12345.5f;
        spx_int32_t on = 1, zero = 0;

        speex_decoder_ctl(dec, SPEEX_GET_FRAME_SIZE, &n);
        build_stream(&s, mode);
        for (f = 0; f < 3; f++) {
            SpeexBits b;
            int i, ret;
            speex_bits_init(&b);
            if (!s.pk[f].lost)
                speex_bits_read_from(&b, s.pk[f].data, s.pk[f].nbytes);
            else
                speex_bits_reset(&b);
            speex_bits_rewind(&b);
            for (i = 0; i < n; i++) { raw[i] = sentinel; enh[i] = sentinel; }
            {
                int cp = b.charPtr, bp = b.bitPtr, ov = b.overflow, nb = b.nbBits;
                unsigned long u = speex_test_bits_unpack_count;
                ret = speex_decode_dual(dec, s.pk[f].lost ? NULL : &b, raw, enh);
                TASK_CHECK(ret == -2);
                TASK_CHECK(speex_test_bits_unpack_count == u);
                TASK_CHECK(b.charPtr == cp && b.bitPtr == bp && b.overflow == ov && b.nbBits == nb);
            }
            for (i = 0; i < n; i++) {
                TASK_CHECK(raw[i] == sentinel);
                TASK_CHECK(enh[i] == sentinel);
            }
            speex_bits_destroy(&b);
        }

        /* Enable later: still allowed before the first successful decode. */
        TASK_CHECK(speex_decoder_ctl(dec, SPEEX_SET_DUAL_OUTPUT, &on) == 0);
        {
            SpeexBits b;
            int ret;
            speex_bits_init(&b);
            if (!s.pk[0].lost) speex_bits_read_from(&b, s.pk[0].data, s.pk[0].nbytes);
            speex_bits_rewind(&b);
            ret = speex_decode_dual(dec, s.pk[0].lost ? NULL : &b, raw, enh);
            TASK_CHECK(ret == 0);
            speex_bits_destroy(&b);
        }
        /* Now locked. */
        {
            spx_int32_t r;
            TASK_CHECK(speex_decoder_ctl(dec, SPEEX_SET_DUAL_OUTPUT, &zero) == -1);
            speex_decoder_ctl(dec, SPEEX_GET_DUAL_OUTPUT, &r);
            TASK_CHECK(r == 1);
            TASK_CHECK(speex_decoder_ctl(dec, SPEEX_SET_DUAL_OUTPUT, &on) == 0);
        }
        speex_decoder_destroy(dec);
    }
    return 0;
}

/* =====================  LossDTX cases  ===================== */

/* Single and continuous loss, plus recovery, compared to references. */
static int case_loss_recovery(void) {
    int mode;
    for (mode = 0; mode < 3; mode++) {
        const SpeexMode *sm = speex_lib_get_mode(mode);
        Stream s;
        void *dec = speex_decoder_init(sm);
        Ref ref;
        int f, n = 0;
        spx_int32_t on = 1;
        float raw[MAX_FRAME], enh[MAX_FRAME];

        speex_decoder_ctl(dec, SPEEX_SET_DUAL_OUTPUT, &on);
        speex_decoder_ctl(dec, SPEEX_GET_FRAME_SIZE, &n);
        build_stream(&s, mode);
        ref_open(&ref, sm);
        for (f = 0; f < s.nframes; f++) {
            SpeexBits b;
            int ret;
            if (s.pk[f].lost) {
                ref_step(&ref, &s.pk[f]);
                ret = speex_decode_dual(dec, NULL, raw, enh);
            } else {
                SpeexBits br, be;
                speex_bits_init(&br); speex_bits_init(&be);
                speex_bits_read_from(&br, s.pk[f].data, s.pk[f].nbytes); speex_bits_rewind(&br);
                speex_bits_read_from(&be, s.pk[f].data, s.pk[f].nbytes); speex_bits_rewind(&be);
                speex_decode(ref.raw, &br, ref.raw_out);
                speex_decode(ref.enh, &be, ref.enh_out);
                speex_bits_destroy(&br); speex_bits_destroy(&be);
                speex_bits_init(&b);
                speex_bits_read_from(&b, s.pk[f].data, s.pk[f].nbytes);
                speex_bits_rewind(&b);
                ret = speex_decode_dual(dec, &b, raw, enh);
                speex_bits_destroy(&b);
            }
            TASK_CHECK(ret == 0);
            TASK_CHECK(cmp_float(raw, ref.raw_out, n, "raw"));
            TASK_CHECK(cmp_float(enh, ref.enh_out, n, "enh"));
        }
        ref_close(&ref);
        speex_decoder_destroy(dec);
    }
    return 0;
}

/* Empty (null) mode: a frame with no transmission. */
static int case_null_mode(void) {
    int mode;
    for (mode = 0; mode < 3; mode++) {
        const SpeexMode *sm = speex_lib_get_mode(mode);
        void *dec = speex_decoder_init(sm);
        Ref ref;
        int n = 0, i;
        spx_int32_t on = 1, zero = 0;
        float raw[MAX_FRAME], enh[MAX_FRAME];
        char nullpkt[64];
        int nullbytes;
        SpeexBits b;
        int ret;

        speex_decoder_ctl(dec, SPEEX_SET_DUAL_OUTPUT, &on);
        speex_decoder_ctl(dec, SPEEX_GET_FRAME_SIZE, &n);
        ref_open(&ref, sm);

        /* The "null mode" (sub-mode with no transmission) is decoded by a
         * packet that carries only a narrowband submode of 0: one zero bit
         * (narrowband marker) and a 4-bit zero submode, no payload. */
        speex_bits_init(&b);
        speex_bits_reset(&b);
        speex_bits_pack(&b, 0, 1);
        speex_bits_pack(&b, 0, 4);
        speex_bits_rewind(&b);
        nullbytes = speex_bits_write(&b, nullpkt, sizeof nullpkt);
        speex_bits_destroy(&b);

        speex_bits_init(&b);
        speex_bits_read_from(&b, nullpkt, nullbytes);
        speex_bits_rewind(&b);
        ret = speex_decode_dual(dec, &b, raw, enh);
        speex_bits_destroy(&b);

        {
            SpeexBits br, be;
            int rr, re;
            speex_bits_init(&br); speex_bits_init(&be);
            speex_bits_read_from(&br, nullpkt, nullbytes); speex_bits_rewind(&br);
            speex_bits_read_from(&be, nullpkt, nullbytes); speex_bits_rewind(&be);
            rr = speex_decode(ref.raw, &br, ref.raw_out);
            re = speex_decode(ref.enh, &be, ref.enh_out);
            speex_bits_destroy(&br); speex_bits_destroy(&be);
            TASK_CHECK(rr == 0 && re == 0);
        }
        (void)zero;
        TASK_CHECK(ret == 0);
        TASK_CHECK(cmp_float(raw, ref.raw_out, n, "raw"));
        TASK_CHECK(cmp_float(enh, ref.enh_out, n, "enh"));

        /* A couple of follow-up frames to prove continuity after null mode. */
        for (i = 0; i < 3; i++) {
            SpeexBits bb, br, be;
            SpeexBits make;
            short frame[MAX_FRAME];
            char data[8192];
            int nb;
            void *enc = speex_encoder_init(sm);
            spx_int32_t q = 4;
            speex_encoder_ctl(enc, SPEEX_SET_QUALITY, &q);
            rng_seed(99u + (unsigned)i);
            gen_frame(frame, n, i, mode, 1);
            speex_bits_init(&make);
            speex_encode_int(enc, frame, &make);
            speex_bits_rewind(&make);
            nb = speex_bits_write(&make, data, sizeof data);
            speex_bits_destroy(&make);
            speex_encoder_destroy(enc);

            speex_bits_init(&bb);
            speex_bits_read_from(&bb, data, nb); speex_bits_rewind(&bb);
            TASK_CHECK(speex_decode_dual(dec, &bb, raw, enh) == 0);
            speex_bits_destroy(&bb);

            speex_bits_init(&br); speex_bits_init(&be);
            speex_bits_read_from(&br, data, nb); speex_bits_rewind(&br);
            speex_bits_read_from(&be, data, nb); speex_bits_rewind(&be);
            speex_decode(ref.raw, &br, ref.raw_out);
            speex_decode(ref.enh, &be, ref.enh_out);
            speex_bits_destroy(&br); speex_bits_destroy(&be);
            TASK_CHECK(cmp_float(raw, ref.raw_out, n, "raw-post-null"));
            TASK_CHECK(cmp_float(enh, ref.enh_out, n, "enh-post-null"));
        }
        ref_close(&ref);
        speex_decoder_destroy(dec);
    }
    return 0;
}

/* DTX: silence enters DTX; the decoder must stay continuous in both paths. */
static int case_dtx_entry(void) {
    int mode;
    for (mode = 0; mode < 3; mode++) {
        const SpeexMode *sm = speex_lib_get_mode(mode);
        void *enc = speex_encoder_init(sm);
        void *dec = speex_decoder_init(sm);
        Ref ref;
        int n = 0, f, dtx_seen = 0;
        spx_int32_t on = 1, vbr_on = 1, dtx_on = 1, q = 6;
        short frame[MAX_FRAME];
        float raw[MAX_FRAME], enh[MAX_FRAME];

        speex_encoder_ctl(enc, SPEEX_GET_FRAME_SIZE, &n);
        speex_encoder_ctl(enc, SPEEX_SET_VBR, &vbr_on);
        speex_encoder_ctl(enc, SPEEX_SET_DTX, &dtx_on);
        speex_encoder_ctl(enc, SPEEX_SET_QUALITY, &q);
        speex_decoder_ctl(dec, SPEEX_SET_DUAL_OUTPUT, &on);
        ref_open(&ref, sm);
        rng_seed(424242u + (unsigned)mode);

        for (f = 0; f < 30; f++) {
            Packet p;
            int active = f < 12;
            gen_frame(frame, n, f, mode, active);
            encode_packet(enc, frame, n, &p, 0);
            if (!active && p.ret == 0) dtx_seen = 1;
            if (p.lost) {
                ref_step(&ref, &p);
                TASK_CHECK(speex_decode_dual(dec, NULL, raw, enh) == 0);
            } else {
                SpeexBits b, br, be;
                int ret;
                speex_bits_init(&b);
                speex_bits_read_from(&b, p.data, p.nbytes); speex_bits_rewind(&b);
                ret = speex_decode_dual(dec, &b, raw, enh);
                speex_bits_destroy(&b);
                TASK_CHECK(ret == 0);
                speex_bits_init(&br); speex_bits_init(&be);
                speex_bits_read_from(&br, p.data, p.nbytes); speex_bits_rewind(&br);
                speex_bits_read_from(&be, p.data, p.nbytes); speex_bits_rewind(&be);
                speex_decode(ref.raw, &br, ref.raw_out);
                speex_decode(ref.enh, &be, ref.enh_out);
                speex_bits_destroy(&br); speex_bits_destroy(&be);
            }
            TASK_CHECK(cmp_float(raw, ref.raw_out, n, "raw"));
            TASK_CHECK(cmp_float(enh, ref.enh_out, n, "enh"));
        }
        TASK_CHECK(dtx_seen);
        ref_close(&ref);
        speex_decoder_destroy(dec);
        speex_encoder_destroy(enc);
    }
    return 0;
}

/* =====================  Recursive cases  ===================== */

/* Two interleaved dual decoders over the same stream (independent histories). */
static int case_interleaved_instances(void) {
    int mode;
    for (mode = 0; mode < 3; mode++) {
        const SpeexMode *sm = speex_lib_get_mode(mode);
        Stream s;
        void *a = speex_decoder_init(sm);
        void *b = speex_decoder_init(sm);
        Ref ra, rb;
        int f, n = 0;
        spx_int32_t on = 1;
        float rawa[MAX_FRAME], enha[MAX_FRAME], rawb[MAX_FRAME], enhb[MAX_FRAME];

        speex_decoder_ctl(a, SPEEX_SET_DUAL_OUTPUT, &on);
        speex_decoder_ctl(b, SPEEX_SET_DUAL_OUTPUT, &on);
        speex_decoder_ctl(a, SPEEX_GET_FRAME_SIZE, &n);
        build_stream(&s, mode);
        ref_open(&ra, sm);
        ref_open(&rb, sm);
        for (f = 0; f < s.nframes; f++) {
            Packet *p = &s.pk[f];
            if (p->lost) {
                ref_step(&ra, p); ref_step(&rb, p);
                TASK_CHECK(speex_decode_dual(a, NULL, rawa, enha) == 0);
                TASK_CHECK(speex_decode_dual(b, NULL, rawb, enhb) == 0);
            } else {
                SpeexBits ba, bb_, x, y;
                speex_bits_init(&ba); speex_bits_init(&bb_);
                speex_bits_read_from(&ba, p->data, p->nbytes); speex_bits_rewind(&ba);
                speex_bits_read_from(&bb_, p->data, p->nbytes); speex_bits_rewind(&bb_);
                TASK_CHECK(speex_decode_dual(a, &ba, rawa, enha) == 0);
                TASK_CHECK(speex_decode_dual(b, &bb_, rawb, enhb) == 0);
                speex_bits_destroy(&ba); speex_bits_destroy(&bb_);
                speex_bits_init(&x); speex_bits_init(&y);
                speex_bits_read_from(&x, p->data, p->nbytes); speex_bits_rewind(&x);
                speex_bits_read_from(&y, p->data, p->nbytes); speex_bits_rewind(&y);
                speex_decode(ra.raw, &x, ra.raw_out);
                speex_decode(ra.enh, &y, ra.enh_out);
                speex_bits_destroy(&x); speex_bits_destroy(&y);
                speex_bits_init(&x); speex_bits_init(&y);
                speex_bits_read_from(&x, p->data, p->nbytes); speex_bits_rewind(&x);
                speex_bits_read_from(&y, p->data, p->nbytes); speex_bits_rewind(&y);
                speex_decode(rb.raw, &x, rb.raw_out);
                speex_decode(rb.enh, &y, rb.enh_out);
                speex_bits_destroy(&x); speex_bits_destroy(&y);
            }
            TASK_CHECK(cmp_float(rawa, ra.raw_out, n, "rawA"));
            TASK_CHECK(cmp_float(enha, ra.enh_out, n, "enhA"));
            TASK_CHECK(cmp_float(rawb, rb.raw_out, n, "rawB"));
            TASK_CHECK(cmp_float(enhb, rb.enh_out, n, "enhB"));
        }
        ref_close(&ra); ref_close(&rb);
        speex_decoder_destroy(a); speex_decoder_destroy(b);
    }
    return 0;
}

/* Mixed legacy float/int entry points once dual output is enabled. */
static int case_mixed_entries(void) {
    int mode;
    for (mode = 0; mode < 3; mode++) {
        const SpeexMode *sm = speex_lib_get_mode(mode);
        Stream s;
        void *dec = speex_decoder_init(sm);
        Ref ref;
        int f, n = 0;
        spx_int32_t on = 1, zero = 0;
        float raw[MAX_FRAME], enh[MAX_FRAME];
        float *hist_raw, *hist_enh;
        spx_int16_t iraw[MAX_FRAME], ienh[MAX_FRAME], iq[MAX_FRAME];

        hist_raw = (float*)malloc(sizeof(float) * MAX_FRAME * STREAM_FRAMES);
        hist_enh = (float*)malloc(sizeof(float) * MAX_FRAME * STREAM_FRAMES);
        TASK_CHECK(hist_raw && hist_enh);

        speex_decoder_ctl(dec, SPEEX_SET_DUAL_OUTPUT, &on);
        speex_decoder_ctl(dec, SPEEX_GET_FRAME_SIZE, &n);
        build_stream(&s, mode);
        ref_open(&ref, sm);
        reference_run(&ref, &s, hist_raw, hist_enh);
        ref_close(&ref);

        /* Replay, alternating the entry points.  The legacy entry point still
         * advances both histories; ENH selects which one it returns. */
        for (f = 0; f < s.nframes; f++) {
            const float *rr = hist_raw + (size_t)f * MAX_FRAME;
            const float *re = hist_enh + (size_t)f * MAX_FRAME;
            SpeexBits b;
            int use_dual = (f % 3 != 0);
            int use_int = (f % 2 == 0);
            speex_bits_init(&b);
            if (!s.pk[f].lost) {
                speex_bits_read_from(&b, s.pk[f].data, s.pk[f].nbytes);
                speex_bits_rewind(&b);
            }
            if (use_dual) {
                int ret;
                if (use_int)
                    ret = speex_decode_dual_int(dec, s.pk[f].lost ? NULL : &b, iraw, ienh);
                else
                    ret = speex_decode_dual(dec, s.pk[f].lost ? NULL : &b, raw, enh);
                TASK_CHECK(ret == 0);
                if (s.pk[f].lost) {
                    /* A lost frame yields a fresh independent rendering; both
                     * tracks are simply the ones the same legacy call would
                     * produce.  (A lost frame's randomness makes quantitative
                     * comparison meaningless, so only basic sanity is done.) */
                    int i;
                    for (i = 0; i < n; i++) TASK_CHECK(isfinite(iraw[i]) && isfinite(ienh[i]));
                } else if (!use_int) {
                    TASK_CHECK(cmp_float(raw, rr, n, "raw"));
                    TASK_CHECK(cmp_float(enh, re, n, "enh"));
                } else {
                    quantize_ref(rr, iq, n);
                    TASK_CHECK(cmp_int(iraw, iq, n, "iraw"));
                    quantize_ref(re, iq, n);
                    TASK_CHECK(cmp_int(ienh, iq, n, "ienh"));
                }
            } else {
                int ret, sel = (f % 2) == 1;
                speex_decoder_ctl(dec, SPEEX_SET_ENH, sel ? &on : &zero);
                if (use_int) {
                    ret = speex_decode_int(dec, s.pk[f].lost ? NULL : &b, iraw);
                    TASK_CHECK(ret == 0);
                    if (s.pk[f].lost) { int i; for (i = 0; i < n; i++) TASK_CHECK(isfinite(iraw[i])); }
                    else if (sel) { quantize_ref(re, iq, n); TASK_CHECK(cmp_int(iraw, iq, n, "single-int-enh")); }
                    else { quantize_ref(rr, iq, n); TASK_CHECK(cmp_int(iraw, iq, n, "single-int-raw")); }
                } else {
                    ret = speex_decode(dec, s.pk[f].lost ? NULL : &b, raw);
                    TASK_CHECK(ret == 0);
                    if (s.pk[f].lost) { int i; for (i = 0; i < n; i++) TASK_CHECK(isfinite(raw[i])); }
                    else if (sel) TASK_CHECK(cmp_float(raw, re, n, "single-float-enh"));
                    else TASK_CHECK(cmp_float(raw, rr, n, "single-float-raw"));
                }
            }
            speex_bits_destroy(&b);
        }
        free(hist_raw); free(hist_enh);
        speex_decoder_destroy(dec);
    }
    return 0;
}

/* In-band control messages: the callback fires once per packet, both outputs
 * match legacy decoders advanced over the same packet, and the bit cursor
 * ends at the same position. */
static int case_inband_once(void) {
    int mode;
    for (mode = 0; mode < 3; mode++) {
        const SpeexMode *sm = speex_lib_get_mode(mode);
        void *enc = speex_encoder_init(sm);
        void *dec = speex_decoder_init(sm);
        void *legacy = speex_decoder_init(sm);
        void *refraw = speex_decoder_init(sm);
        void *refenh = speex_decoder_init(sm);
        SpeexCallback cb;
        short frame[MAX_FRAME];
        char packet[8192];
        int n = 0, nb, enc_ret, f, i;
        spx_int32_t on = 1, zero = 0;

        speex_encoder_ctl(enc, SPEEX_GET_FRAME_SIZE, &n);
        speex_decoder_ctl(dec, SPEEX_SET_DUAL_OUTPUT, &on);
        speex_decoder_ctl(legacy, SPEEX_SET_DUAL_OUTPUT, &zero);
        speex_decoder_ctl(refraw, SPEEX_SET_DUAL_OUTPUT, &zero);
        speex_decoder_ctl(refenh, SPEEX_SET_DUAL_OUTPUT, &zero);
        speex_decoder_ctl(refraw, SPEEX_SET_ENH, &zero);
        speex_decoder_ctl(refenh, SPEEX_SET_ENH, &on);
        cb.callback_id = SPEEX_INBAND_CHAR;
        cb.func = count_user_callback;
        cb.data = NULL;
        cb.reserved1 = NULL;
        cb.reserved2 = 0;
        speex_decoder_ctl(dec, SPEEX_SET_USER_HANDLER, &cb);
        speex_decoder_ctl(legacy, SPEEX_SET_USER_HANDLER, &cb);
        speex_decoder_ctl(refraw, SPEEX_SET_USER_HANDLER, &cb);
        speex_decoder_ctl(refenh, SPEEX_SET_USER_HANDLER, &cb);
        rng_seed(777u + (unsigned)mode);

        for (f = 0; f < 6; f++) {
            float raw[MAX_FRAME], enh[MAX_FRAME], dummy[MAX_FRAME];
            float rraw[MAX_FRAME], renh[MAX_FRAME];
            SpeexBits bd, bl, x, y;
            int endc, endb;

            gen_frame(frame, n, f, mode, 1);
            nb = build_inband_packet(enc, frame, packet, sizeof packet, &enc_ret, mode);
            TASK_CHECK(nb > 0 && enc_ret > 0);

            /* Dual decode. */
            g_callback_count = 0;
            speex_bits_init(&bd);
            speex_bits_read_from(&bd, packet, nb); speex_bits_rewind(&bd);
            TASK_CHECK(speex_decode_dual(dec, &bd, raw, enh) == 0);
            TASK_CHECK(g_callback_count == 1);
            endc = bd.charPtr; endb = bd.bitPtr;
            speex_bits_destroy(&bd);

            /* Legacy single decode of the same packet. */
            g_callback_count = 0;
            speex_bits_init(&bl);
            speex_bits_read_from(&bl, packet, nb); speex_bits_rewind(&bl);
            TASK_CHECK(speex_decode(legacy, &bl, dummy) == 0);
            TASK_CHECK(g_callback_count == 1);
            TASK_CHECK(bl.charPtr == endc && bl.bitPtr == endb);
            speex_bits_destroy(&bl);

            /* Reference pair from two fresh legacy decoders. */
            speex_bits_init(&x); speex_bits_init(&y);
            speex_bits_read_from(&x, packet, nb); speex_bits_rewind(&x);
            speex_bits_read_from(&y, packet, nb); speex_bits_rewind(&y);
            TASK_CHECK(speex_decode(refraw, &x, rraw) == 0);
            TASK_CHECK(speex_decode(refenh, &y, renh) == 0);
            speex_bits_destroy(&x); speex_bits_destroy(&y);

            TASK_CHECK(cmp_float(raw, rraw, n, "raw-inband"));
            TASK_CHECK(cmp_float(enh, renh, n, "enh-inband"));
            for (i = 0; i < n; i++) TASK_CHECK(isfinite(raw[i]) && isfinite(enh[i]));
        }
        speex_decoder_destroy(dec);
        speex_decoder_destroy(legacy);
        speex_decoder_destroy(refraw);
        speex_decoder_destroy(refenh);
        speex_encoder_destroy(enc);
    }
    return 0;
}

/* SPEEX_RESET_STATE keeps dual output on and resets both histories. */
static int case_reset_state(void) {
    int mode;
    for (mode = 0; mode < 3; mode++) {
        const SpeexMode *sm = speex_lib_get_mode(mode);
        Stream s;
        void *dec = speex_decoder_init(sm);
        Ref dec_ref;
        int pass, f, n = 0;
        spx_int32_t on = 1, got = 0;
        float raw[MAX_FRAME], enh[MAX_FRAME];

        speex_decoder_ctl(dec, SPEEX_SET_DUAL_OUTPUT, &on);
        speex_decoder_ctl(dec, SPEEX_GET_FRAME_SIZE, &n);
        build_stream(&s, mode);

        /* A reset decoder is not the same as a freshly created one (the legacy
         * reset intentionally leaves some state alone), so the references get
         * the very same reset request at the same point. */
        ref_open(&dec_ref, sm);
        for (pass = 0; pass < 2; pass++) {
            if (pass == 1) {
                TASK_CHECK(speex_decoder_ctl(dec, SPEEX_RESET_STATE, NULL) == 0);
                speex_decoder_ctl(dec, SPEEX_GET_DUAL_OUTPUT, &got);
                TASK_CHECK(got == 1);
                TASK_CHECK(speex_decoder_ctl(dec_ref.raw, SPEEX_RESET_STATE, NULL) == 0);
                TASK_CHECK(speex_decoder_ctl(dec_ref.enh, SPEEX_RESET_STATE, NULL) == 0);
            }
            for (f = 0; f < 8; f++) {
                Packet *p = &s.pk[f];
                if (p->lost) {
                    ref_step(&dec_ref, p);
                    TASK_CHECK(speex_decode_dual(dec, NULL, raw, enh) == 0);
                } else {
                    SpeexBits b, br, be;
                    speex_bits_init(&b);
                    speex_bits_read_from(&b, p->data, p->nbytes); speex_bits_rewind(&b);
                    TASK_CHECK(speex_decode_dual(dec, &b, raw, enh) == 0);
                    speex_bits_destroy(&b);
                    speex_bits_init(&br); speex_bits_init(&be);
                    speex_bits_read_from(&br, p->data, p->nbytes); speex_bits_rewind(&br);
                    speex_bits_read_from(&be, p->data, p->nbytes); speex_bits_rewind(&be);
                    speex_decode(dec_ref.raw, &br, dec_ref.raw_out);
                    speex_decode(dec_ref.enh, &be, dec_ref.enh_out);
                    speex_bits_destroy(&br); speex_bits_destroy(&be);
                }
                TASK_CHECK(cmp_float(raw, dec_ref.raw_out, n, "raw"));
                TASK_CHECK(cmp_float(enh, dec_ref.enh_out, n, "enh"));
            }
        }
        ref_close(&dec_ref);
        speex_decoder_destroy(dec);
    }
    return 0;
}

/* Non-success returns must not write either dual buffer. */
static int case_terminator_no_write(void) {
    int mode;
    for (mode = 0; mode < 3; mode++) {
        const SpeexMode *sm = speex_lib_get_mode(mode);
        void *dec = speex_decoder_init(sm);
        SpeexBits b;
        float raw[MAX_FRAME], enh[MAX_FRAME];
        float sentinel = -9999.25f;
        int n = 0, i, ret;
        spx_int32_t on = 1;

        speex_decoder_ctl(dec, SPEEX_SET_DUAL_OUTPUT, &on);
        speex_decoder_ctl(dec, SPEEX_GET_FRAME_SIZE, &n);
        speex_bits_init(&b);
        speex_bits_reset(&b);
        /* Enough leading zero bits that the narrowband decoder reaches its
         * 4-bit sub-mode field and finds the terminator (15).  The legacy
         * decoder leaves the output buffer untouched on this return. */
        speex_bits_pack(&b, 0, 1);
        speex_bits_pack(&b, 15, 4);
        speex_bits_rewind(&b);
        for (i = 0; i < n; i++) { raw[i] = sentinel; enh[i] = sentinel; }
        ret = speex_decode_dual(dec, &b, raw, enh);
        TASK_CHECK(ret == -1);
        /* The legacy decoder leaves its output untouched when it returns -1;
         * the dual-output decoder must do the same for BOTH buffers. */
        for (i = 0; i < n; i++) {
            TASK_CHECK(raw[i] == sentinel);
            TASK_CHECK(enh[i] == sentinel);
        }
        speex_bits_destroy(&b);
        speex_decoder_destroy(dec);
    }
    return 0;
}

/* =====================  registration  ===================== */

const struct task_case task_cases[] = {
    {"DualPCM",   "dual_full_stack",       case_dual_full_stack},
    {"DualPCM",   "dual_single_pass",      case_dual_single_pass},
    {"DualPCM",   "dual_disabled",         case_dual_disabled},
    {"DualPCM",   "terminator_no_write",   case_terminator_no_write},
    {"LossDTX",   "loss_recovery",         case_loss_recovery},
    {"LossDTX",   "null_mode",             case_null_mode},
    {"LossDTX",   "dtx_entry",             case_dtx_entry},
    {"Recursive", "interleaved_instances", case_interleaved_instances},
    {"Recursive", "mixed_entries",         case_mixed_entries},
    {"Recursive", "inband_once",           case_inband_once},
    {"Recursive", "reset_state",           case_reset_state},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
