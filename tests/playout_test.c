#include "task_support/check.h"
#include "speex/speex_playout.h"
#include "speex/speex_jitter.h"
#include "speex/speex_resampler.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI_F 3.14159265358979323846f

static float max_abs_diff(const float *a, const float *b, size_t n)
{
    float m = 0.0f;
    size_t i;
    for (i = 0; i < n; ++i) {
        float d = fabsf(a[i] - b[i]);
        if (d > m)
            m = d;
    }
    return m;
}

static void tone_packet(float *pcm, unsigned channels, uint32_t frames,
                        uint64_t first_frame, float amplitude)
{
    uint32_t i;
    for (i = 0; i < frames; ++i) {
        float v = amplitude * sinf(2.0f * PI_F * 80.0f *
                                   (float)(first_frame + i) / 48000.0f);
        if (channels == 1) {
            pcm[i] = v;
        } else {
            pcm[2u * i] = v;
            pcm[2u * i + 1u] = -v;
        }
    }
}

static int destroy_callback_count;
static void test_destroy_callback(void *p)
{
    (void)p;
    ++destroy_callback_count;
}

static int timeline_jitter_peek_consume_wrap(void)
{
    enum { N = 480 };
    JitterBuffer *j = jitter_buffer_init(480);
    float a[N], b[N], second[240], b2[240];
    JitterBufferPacket p, r;
    spx_int32_t off = 999;
    uint32_t start = 0xfffffe00u;
    int i;

    TASK_CHECK(j != NULL);
    TASK_CHECK(jitter_buffer_begin_pull(j, start) == 0);

    for (i = 0; i < N; ++i)
        a[i] = (float)(i + 1);
    memset(&p, 0, sizeof(p));
    p.data = (char *)a;
    p.len = sizeof(a);
    p.timestamp = start;
    p.span = N;
    jitter_buffer_put(j, &p);

    memset(&r, 0, sizeof(r));
    r.data = (char *)b;
    r.len = sizeof(b);
    TASK_CHECK(jitter_buffer_peek(j, &r, 480, &off) == JITTER_BUFFER_OK);
    TASK_CHECK(off == 0);
    TASK_CHECK(r.len == sizeof(a));
    TASK_CHECK(memcmp(a, b, sizeof(a)) == 0);
    TASK_CHECK((uint32_t)jitter_buffer_get_pointer_timestamp(j) == start);

    /* Peek does not advance, and a repeated peek copies the same payload. */
    off = 77;
    memset(b, 0, sizeof(b));
    TASK_CHECK(jitter_buffer_peek(j, &r, 480, &off) == JITTER_BUFFER_OK);
    TASK_CHECK(off == 0);
    TASK_CHECK(memcmp(a, b, sizeof(a)) == 0);
    TASK_CHECK((uint32_t)jitter_buffer_get_pointer_timestamp(j) == start);

    /* Invalid desired span is rejected without advancing. */
    TASK_CHECK(jitter_buffer_peek(j, &r, 0, &off) < 0);
    TASK_CHECK((uint32_t)jitter_buffer_get_pointer_timestamp(j) == start);

    TASK_CHECK(jitter_buffer_consume(j, 120) == 0);
    TASK_CHECK((uint32_t)jitter_buffer_get_pointer_timestamp(j) == start + 120u);
    TASK_CHECK(jitter_buffer_consume(j, 1) < 0); /* authorization was spent */

    r.len = sizeof(b);
    off = 999;
    TASK_CHECK(jitter_buffer_peek(j, &r, 360, &off) == JITTER_BUFFER_OK);
    TASK_CHECK(off == -120);
    TASK_CHECK(jitter_buffer_consume(j, 361) < 0); /* beyond authorized prefix */
    TASK_CHECK((uint32_t)jitter_buffer_get_pointer_timestamp(j) == start + 120u);
    TASK_CHECK(jitter_buffer_consume(j, 0) == 0);
    TASK_CHECK(jitter_buffer_consume(j, 360) == 0);
    TASK_CHECK((uint32_t)jitter_buffer_get_pointer_timestamp(j) == start + 480u);

    /* The 32-frame gap crosses the uint32 wrap and stops at the next start. */
    for (i = 0; i < 240; ++i)
        second[i] = -(float)(i + 1);
    memset(&p, 0, sizeof(p));
    p.data = (char *)second;
    p.len = sizeof(second);
    p.timestamp = 0u;
    p.span = 240;
    jitter_buffer_put(j, &p);

    memset(&r, 0, sizeof(r));
    r.data = (char *)b2;
    r.len = sizeof(b2);
    TASK_CHECK(jitter_buffer_peek(j, &r, 480, &off) == JITTER_BUFFER_MISSING);
    TASK_CHECK(r.len == 0);
    TASK_CHECK((int)r.span == 32);
    TASK_CHECK(jitter_buffer_consume(j, 32) == 0);
    TASK_CHECK((uint32_t)jitter_buffer_get_pointer_timestamp(j) == 0u);

    r.len = sizeof(b2);
    TASK_CHECK(jitter_buffer_peek(j, &r, 480, &off) == JITTER_BUFFER_OK);
    TASK_CHECK(off == 0);
    TASK_CHECK(memcmp(second, b2, sizeof(second)) == 0);
    TASK_CHECK(jitter_buffer_consume(j, 120) == 0);
    r.len = sizeof(b2);
    TASK_CHECK(jitter_buffer_peek(j, &r, 240, &off) == JITTER_BUFFER_OK);
    TASK_CHECK(off == -120);
    TASK_CHECK(jitter_buffer_consume(j, 120) == 0);

    /* The original reset operation leaves pull mode and restores legacy API. */
    jitter_buffer_reset(j);
    TASK_CHECK(jitter_buffer_consume(j, 1) < 0);
    memset(&r, 0, sizeof(r));
    TASK_CHECK(jitter_buffer_get(j, &r, 120, &off) == JITTER_BUFFER_MISSING);

    jitter_buffer_destroy(j);
    return 0;
}

static int timeline_jitter_gap_variable_and_put_invalidation(void)
{
    JitterBuffer *j = jitter_buffer_init(480);
    float pcm120[120], pcm1920[1920], out[1920];
    JitterBufferPacket p, r;
    spx_int32_t off = 0;

    TASK_CHECK(j != NULL);
    TASK_CHECK(jitter_buffer_begin_pull(j, 1000) == 0);

    /* A future 120-frame packet leaves a 200-frame gap at cursor 1000. */
    memset(pcm120, 0, sizeof(pcm120));
    memset(&p, 0, sizeof(p));
    p.data = (char *)pcm120;
    p.len = sizeof(pcm120);
    p.timestamp = 1200;
    p.span = 120;
    jitter_buffer_put(j, &p);

    memset(&r, 0, sizeof(r));
    r.data = (char *)out;
    r.len = sizeof(out);
    TASK_CHECK(jitter_buffer_peek(j, &r, 480, &off) == JITTER_BUFFER_MISSING);
    TASK_CHECK(r.span == 200);
    TASK_CHECK(jitter_buffer_consume(j, 70) == 0);
    TASK_CHECK(jitter_buffer_get_pointer_timestamp(j) == 1070);

    /* A new successful put invalidates the next peek authorization only after
       it has been reissued; consuming the spent authorization fails. */
    TASK_CHECK(jitter_buffer_consume(j, 130) < 0);
    TASK_CHECK(jitter_buffer_peek(j, &r, 480, &off) == JITTER_BUFFER_MISSING);
    TASK_CHECK(r.span == 130);
    TASK_CHECK(jitter_buffer_consume(j, 130) == 0);

    r.len = sizeof(out);
    TASK_CHECK(jitter_buffer_peek(j, &r, 120, &off) == JITTER_BUFFER_OK);
    TASK_CHECK(off == 0);
    TASK_CHECK(jitter_buffer_consume(j, 120) == 0);

    /* Variable long packet: desired 480 authorizes only a 480 prefix. */
    memset(pcm1920, 0, sizeof(pcm1920));
    pcm1920[0] = 0.25f;
    memset(&p, 0, sizeof(p));
    p.data = (char *)pcm1920;
    p.len = sizeof(pcm1920);
    p.timestamp = 1320;
    p.span = 1920;
    jitter_buffer_put(j, &p);

    r.len = sizeof(out);
    TASK_CHECK(jitter_buffer_peek(j, &r, 480, &off) == JITTER_BUFFER_OK);
    TASK_CHECK(off == 0);
    TASK_CHECK(r.len == sizeof(pcm1920)); /* peek copies the complete packet */
    TASK_CHECK(jitter_buffer_consume(j, 480) == 0);
    TASK_CHECK(jitter_buffer_get_pointer_timestamp(j) == 1800);

    r.len = sizeof(out);
    TASK_CHECK(jitter_buffer_peek(j, &r, 1920, &off) == JITTER_BUFFER_OK);
    TASK_CHECK(off == -480);
    TASK_CHECK((int)(off + (spx_int32_t)r.span) == 1440);
    TASK_CHECK(jitter_buffer_consume(j, 1440) == 0);
    TASK_CHECK(jitter_buffer_get_pointer_timestamp(j) == 3240);

    jitter_buffer_destroy(j);
    return 0;
}

static int timeline_jitter_destroy_callback_rejected(void)
{
    JitterBuffer *j = jitter_buffer_init(480);
    static float payload[120];
    JitterBufferPacket p;
    JitterBufferPacket got;
    spx_int32_t off = 0;

    TASK_CHECK(j != NULL);
    destroy_callback_count = 0;
    TASK_CHECK(jitter_buffer_ctl(j, JITTER_BUFFER_SET_DESTROY_CALLBACK,
                                 (void *)(uintptr_t)test_destroy_callback) == 0);

    memset(&p, 0, sizeof(p));
    p.data = (char *)payload;
    p.len = sizeof(payload);
    p.timestamp = 50;
    p.span = 120;
    jitter_buffer_put(j, &p);

    TASK_CHECK(jitter_buffer_begin_pull(j, 77) < 0);
    TASK_CHECK(jitter_buffer_peek(j, &got, 120, &off) < 0);

    /* The rejected begin did not clear the old scheduler state. */
    memset(&got, 0, sizeof(got));
    got.data = NULL;
    got.len = 0;
    TASK_CHECK(jitter_buffer_get(j, &got, 120, &off) == JITTER_BUFFER_OK);
    TASK_CHECK(off == 0);
    TASK_CHECK(got.data == (char *)payload);

    jitter_buffer_destroy(j);
    return 0;
}

static int timeline_playout_overlap_duplicates_wrap_limit(void)
{
    SpeexPlayout *st = speex_playout_init(1, 960);
    float pcm[1920];
    float duplicate_out[480];
    SpeexPlayoutStats stats;
    uint32_t start = 0xfffff000u;
    int i;
    double mean = 0.0;

    TASK_CHECK(st != NULL);
    TASK_CHECK(speex_playout_reset(st, start) == 0);
    for (i = 0; i < 1920; ++i)
        pcm[i] = 0.25f;

    /* Out-of-order adjacent packets are accepted. */
    TASK_CHECK(speex_playout_put(st, start + 480u, pcm, 480) == 0);
    TASK_CHECK(speex_playout_put(st, start, pcm, 480) == 0);

    /* Changed retransmission of the exact interval reports success. */
    for (i = 0; i < 480; ++i)
        pcm[i] = -0.5f;
    TASK_CHECK(speex_playout_put(st, start, pcm, 480) == 0);

    /* Non-identical overlaps are rejected. */
    TASK_CHECK(speex_playout_put(st, start + 120u, pcm, 480) < 0);
    TASK_CHECK(speex_playout_put(st, start, pcm, 240) < 0);

    /* Adjacent after the second packet is allowed. */
    TASK_CHECK(speex_playout_put(st, start + 960u, pcm, 120) == 0);

    /* Signed wrap comparisons: start-1 is in the past. */
    TASK_CHECK(speex_playout_put(st, start - 1u, pcm, 120) < 0);
    TASK_CHECK(speex_playout_put(st, start + 96000u, pcm, 120) < 0);

    TASK_CHECK(speex_playout_get_stats(st, &stats) == 0);
    TASK_CHECK(stats.delivered == 0);
    TASK_CHECK(stats.quanta == 0);

    /* The ignored -0.5 retransmission must not replace the original +0.25. */
    TASK_CHECK(speex_playout_pull(st, duplicate_out, 480) == 0);
    for (i = 0; i < 480; ++i)
        mean += duplicate_out[i];
    TASK_CHECK(mean / 480.0 > 0.15);

    speex_playout_destroy(st);

    /* Exactly 200 active packets fit; the 201st is rejected. */
    st = speex_playout_init(1, 9600);
    TASK_CHECK(st != NULL);
    TASK_CHECK(speex_playout_reset(st, 0x12340000u) == 0);
    for (i = 0; i < 200; ++i)
        TASK_CHECK(speex_playout_put(st, 0x12340000u + (uint32_t)i * 120u, pcm, 120) == 0);
    TASK_CHECK(speex_playout_put(st, 0x12340000u + 200u * 120u, pcm, 120) < 0);
    speex_playout_destroy(st);
    return 0;
}

static int elastic_fragment_matches_whole_and_reference(void)
{
    enum { Q = 6, CH = 1 };
    SpeexPlayout *whole = speex_playout_init(CH, 960);
    SpeexPlayout *frag = speex_playout_init(CH, 960);
    SpeexResamplerState *ref;
    SpeexPlayoutStats ws, fs;
    float packets[Q][480];
    float whole_out[Q * 480];
    float frag_out[Q * 480];
    float ref_source[Q * 480 + 4096];
    float ref_out[Q * 480];
    int us[Q];
    spx_uint32_t q, p;
    int err;
    static const uint32_t pieces[] = {1, 7, 113, 359};

    TASK_CHECK(whole != NULL && frag != NULL);
    TASK_CHECK(speex_playout_reset(whole, 1234) == 0);
    TASK_CHECK(speex_playout_reset(frag, 1234) == 0);

    memset(ref_source, 0, sizeof(ref_source));
    for (q = 0; q < Q; ++q) {
        tone_packet(packets[q], CH, 480, (uint64_t)q * 480u, 0.45f);
        memcpy(ref_source + q * 480u, packets[q], sizeof(packets[q]));
        TASK_CHECK(speex_playout_put(whole, 1234u + q * 480u, packets[q], 480) == 0);
        TASK_CHECK(speex_playout_put(frag, 1234u + q * 480u, packets[q], 480) == 0);
    }

    for (q = 0; q < Q; ++q) {
        TASK_CHECK(speex_playout_pull(whole, whole_out + q * 480u, 480) == 0);
        TASK_CHECK(speex_playout_get_stats(whole, &ws) == 0);
        us[q] = ws.correction_ppm;

        p = 0;
        TASK_CHECK(speex_playout_pull(frag, frag_out + q * 480u + p, pieces[0]) == 0);
        p += pieces[0];
        TASK_CHECK(speex_playout_pull(frag, frag_out + q * 480u + p, pieces[1]) == 0);
        p += pieces[1];
        TASK_CHECK(speex_playout_pull(frag, frag_out + q * 480u + p, pieces[2]) == 0);
        p += pieces[2];
        TASK_CHECK(p + pieces[3] == 480);
        TASK_CHECK(speex_playout_pull(frag, frag_out + q * 480u + p, pieces[3]) == 0);

        TASK_CHECK(speex_playout_get_stats(frag, &fs) == 0);
        TASK_CHECK(fs.correction_ppm == ws.correction_ppm);
        TASK_CHECK(fs.source_consumed == ws.source_consumed);
        TASK_CHECK(fs.concealed == ws.concealed);
        TASK_CHECK(fs.quanta == ws.quanta);
        TASK_CHECK(fs.delivered == ws.delivered);
        TASK_CHECK(fs.media_timestamp == ws.media_timestamp);
    }

    TASK_CHECK(max_abs_diff(whole_out, frag_out, Q * 480u) <= 2.0e-6f);

    ref = speex_resampler_init(CH, 48000, 48000, 3, &err);
    TASK_CHECK(ref != NULL && err == 0);
    TASK_CHECK(speex_resampler_skip_zeros(ref) == 0);
    {
        spx_uint32_t pos = 0;
        for (q = 0; q < Q; ++q) {
            spx_uint32_t in_len = (spx_uint32_t)(Q * 480u + 4096u - pos);
            spx_uint32_t out_len = 480;
            TASK_CHECK(speex_resampler_set_rate_frac(
                            ref, 1000000u + (spx_uint32_t)us[q], 1000000u,
                            48000u, 48000u) == 0);
            TASK_CHECK(speex_resampler_process_interleaved_float(
                            ref, ref_source + pos, &in_len,
                            ref_out + q * 480u, &out_len) == 0);
            TASK_CHECK(out_len == 480);
            pos += in_len;
        }
    }
    TASK_CHECK(max_abs_diff(whole_out, ref_out, Q * 480u) <= 2.0e-6f);

    speex_resampler_destroy(ref);
    speex_playout_destroy(whole);
    speex_playout_destroy(frag);
    return 0;
}

static int elastic_stereo_antiphase_reference(void)
{
    enum { Q = 8, CH = 2 };
    SpeexPlayout *po = speex_playout_init(CH, 1920);
    SpeexResamplerState *ref;
    float packets[Q][480 * 2];
    float out[Q * 480 * 2];
    float source[(Q * 480 + 4096) * 2];
    float reference[Q * 480 * 2];
    int us[Q];
    spx_uint32_t q;
    int err;
    double left_energy = 0.0, right_energy = 0.0, anti_energy = 0.0;

    TASK_CHECK(po != NULL);
    TASK_CHECK(speex_playout_reset(po, 700000u) == 0);
    memset(source, 0, sizeof(source));
    for (q = 0; q < Q; ++q) {
        tone_packet(packets[q], CH, 480, (uint64_t)q * 480u, 0.5f);
        memcpy(source + q * 480u * 2u, packets[q], sizeof(packets[q]));
        TASK_CHECK(speex_playout_put(po, 700000u + q * 480u, packets[q], 480) == 0);
    }
    for (q = 0; q < Q; ++q) {
        SpeexPlayoutStats st;
        TASK_CHECK(speex_playout_pull(po, out + q * 480u * 2u, 480) == 0);
        TASK_CHECK(speex_playout_get_stats(po, &st) == 0);
        us[q] = st.correction_ppm;
    }

    for (q = 0; q < Q * 480u; ++q) {
        float l = out[2u * q], r = out[2u * q + 1u];
        TASK_CHECK(isfinite(l) && isfinite(r));
        left_energy += (double)l * l;
        right_energy += (double)r * r;
        anti_energy += (double)(l + r) * (l + r);
    }
    TASK_CHECK(left_energy > 20.0);
    TASK_CHECK(right_energy > 20.0);
    TASK_CHECK(anti_energy < 1.0e-3);

    ref = speex_resampler_init(CH, 48000, 48000, 3, &err);
    TASK_CHECK(ref != NULL && err == 0);
    TASK_CHECK(speex_resampler_skip_zeros(ref) == 0);
    {
        spx_uint32_t pos = 0;
        for (q = 0; q < Q; ++q) {
            spx_uint32_t in_frames = Q * 480u + 4096u - pos;
            spx_uint32_t out_frames = 480;
            TASK_CHECK(speex_resampler_set_rate_frac(
                            ref, 1000000u + (spx_uint32_t)us[q], 1000000u,
                            48000u, 48000u) == 0);
            TASK_CHECK(speex_resampler_process_interleaved_float(
                            ref, source + pos * 2u, &in_frames,
                            reference + q * 480u * 2u, &out_frames) == 0);
            TASK_CHECK(out_frames == 480);
            pos += in_frames;
        }
    }
    TASK_CHECK(max_abs_diff(out, reference, Q * 480u * 2u) <= 2.0e-6f);

    speex_resampler_destroy(ref);
    speex_playout_destroy(po);
    return 0;
}

static int elastic_occupancy_zero_capacity_and_partial(void)
{
    SpeexPlayout *st = speex_playout_init(1, 1920);
    SpeexPlayoutStats stats;
    float out[480], pcm[1920];
    int i;

    TASK_CHECK(st != NULL);
    TASK_CHECK(speex_playout_reset(st, 0) == 0);
    TASK_CHECK(speex_playout_pull(st, out, 1) == 0);
    TASK_CHECK(speex_playout_get_stats(st, &stats) == 0);
    TASK_CHECK(stats.quanta == 1);
    TASK_CHECK(stats.rendered == 480);
    TASK_CHECK(stats.delivered == 1);
    TASK_CHECK(stats.correction_ppm == -40);
    TASK_CHECK(stats.concealed == stats.source_consumed);
    TASK_CHECK(stats.source_consumed > 400);
    TASK_CHECK(speex_playout_pull(st, out + 1, 479) == 0);
    for (i = 0; i < 480; ++i)
        TASK_CHECK(out[i] == 0.0f);

    /* High occupancy reaches the positive clamp. */
    speex_playout_destroy(st);
    st = speex_playout_init(1, 480);
    TASK_CHECK(st != NULL);
    TASK_CHECK(speex_playout_reset(st, 0) == 0);
    memset(pcm, 0, sizeof(pcm));
    for (i = 0; i < 50; ++i)
        TASK_CHECK(speex_playout_put(st, (uint32_t)i * 1920u, pcm, 1920) == 0);
    TASK_CHECK(speex_playout_pull(st, out, 1) == 0);
    TASK_CHECK(speex_playout_get_stats(st, &stats) == 0);
    TASK_CHECK(stats.correction_ppm == 1000);

    /* A long packet remains queued after partial FIR consumption; an exact
       duplicate is accepted while that is true. */
    speex_playout_destroy(st);
    st = speex_playout_init(1, 480);
    TASK_CHECK(st != NULL);
    TASK_CHECK(speex_playout_reset(st, 500) == 0);
    for (i = 0; i < 1920; ++i)
        pcm[i] = 0.3f * sinf(i * 0.03f);
    TASK_CHECK(speex_playout_put(st, 500, pcm, 1920) == 0);
    TASK_CHECK(speex_playout_pull(st, out, 480) == 0);
    TASK_CHECK(speex_playout_get_stats(st, &stats) == 0);
    TASK_CHECK(stats.source_consumed < 1920);
    pcm[0] = 9.0f; /* must not replace the first copy */
    TASK_CHECK(speex_playout_put(st, 500, pcm, 1920) == 0);
    TASK_CHECK(speex_playout_pull(st, out, 480) == 0);
    TASK_CHECK(speex_playout_get_stats(st, &stats) == 0);
    TASK_CHECK(stats.source_consumed < 1920);

    speex_playout_destroy(st);
    return 0;
}

static int lifecycle_arguments_and_reject_recovery(void)
{
    SpeexPlayout *st;
    SpeexPlayoutStats stats;
    float pcm[480], out[480];
    int i;

    TASK_CHECK(speex_playout_init(0, 480) == NULL);
    TASK_CHECK(speex_playout_init(3, 480) == NULL);
    TASK_CHECK(speex_playout_init(1, 0) == NULL);
    TASK_CHECK(speex_playout_init(1, 479) == NULL);
    TASK_CHECK(speex_playout_init(1, 9601) == NULL);
    TASK_CHECK(speex_playout_init(1, 1000) == NULL); /* not multiple of 480 */
    SpeexPlayout *valid_two;
    TASK_CHECK((valid_two = speex_playout_init(2, 9600)) != NULL);
    speex_playout_destroy(valid_two);

    TASK_CHECK(speex_playout_reset(NULL, 0) < 0);
    TASK_CHECK(speex_playout_put(NULL, 0, pcm, 480) < 0);
    TASK_CHECK(speex_playout_pull(NULL, out, 480) < 0);
    TASK_CHECK(speex_playout_get_stats(NULL, &stats) < 0);

    st = speex_playout_init(1, 480);
    TASK_CHECK(st != NULL);
    TASK_CHECK(speex_playout_pull(st, out, 480) < 0); /* reset required */
    TASK_CHECK(speex_playout_put(st, 0, pcm, 480) < 0);
    TASK_CHECK(speex_playout_pull(st, NULL, 0) == 0);
    TASK_CHECK(speex_playout_get_stats(st, &stats) == 0);
    TASK_CHECK(stats.quanta == 0 && stats.delivered == 0);

    TASK_CHECK(speex_playout_reset(st, 0) == 0);
    TASK_CHECK(speex_playout_put(st, 0, NULL, 0) < 0);
    TASK_CHECK(speex_playout_put(st, 0, pcm, 119) < 0);
    TASK_CHECK(speex_playout_put(st, 0, pcm, 1921) < 0);
    TASK_CHECK(speex_playout_pull(st, NULL, 480) < 0);
    TASK_CHECK(speex_playout_pull(st, NULL, 0) == 0);

    for (i = 0; i < 480; ++i)
        pcm[i] = 0.4f * sinf(i * 0.08f);
    TASK_CHECK(speex_playout_put(st, 0, pcm, 480) == 0);
    TASK_CHECK(speex_playout_pull(st, NULL, 480) < 0); /* null output rejected */
    TASK_CHECK(speex_playout_get_stats(st, &stats) == 0);
    TASK_CHECK(stats.delivered == 0);
    TASK_CHECK(speex_playout_pull(st, out, 480) == 0);
    {
        double energy = 0.0;
        for (i = 0; i < 480; ++i)
            energy += (double)out[i] * out[i];
        TASK_CHECK(energy > 1.0);
    }
    TASK_CHECK(speex_playout_pull(st, out, 1) == 0);
    TASK_CHECK(speex_playout_put(st, 9000, pcm, 480) < 0); /* not a 480 boundary */
    TASK_CHECK(speex_playout_pull(st, out + 1, 479) == 0);
    TASK_CHECK(speex_playout_get_stats(st, NULL) < 0);

    speex_playout_destroy(st);
    speex_playout_destroy(NULL);
    return 0;
}

static int lifecycle_reset_matches_rebuild_and_discards(void)
{
    SpeexPlayout *reset_st = speex_playout_init(1, 960);
    SpeexPlayout *fresh = speex_playout_init(1, 960);
    float loud[480], quiet[480], a[960], b[960], zero[16];
    SpeexPlayoutStats sa, sb;
    int i;

    TASK_CHECK(reset_st != NULL && fresh != NULL);
    TASK_CHECK(speex_playout_reset(reset_st, 100) == 0);
    for (i = 0; i < 480; ++i)
        loud[i] = 1.0f;
    TASK_CHECK(speex_playout_put(reset_st, 100, loud, 480) == 0);
    TASK_CHECK(speex_playout_pull(reset_st, a, 480) == 0);

    /* Reset to a different start must discard the old queue and FIR. */
    TASK_CHECK(speex_playout_reset(reset_st, 777) == 0);
    TASK_CHECK(speex_playout_pull(reset_st, zero, 16) == 0);
    for (i = 0; i < 16; ++i)
        TASK_CHECK(fabsf(zero[i]) < 1.0e-7f);

    TASK_CHECK(speex_playout_reset(reset_st, 4242) == 0);
    TASK_CHECK(speex_playout_reset(fresh, 4242) == 0);
    for (i = 0; i < 480; ++i)
        quiet[i] = 0.35f * sinf(i * 0.04f);
    TASK_CHECK(speex_playout_put(reset_st, 4242 + 480, quiet, 480) == 0);
    TASK_CHECK(speex_playout_put(fresh, 4242 + 480, quiet, 480) == 0);
    TASK_CHECK(speex_playout_put(reset_st, 4242, quiet, 480) == 0);
    TASK_CHECK(speex_playout_put(fresh, 4242, quiet, 480) == 0);

    TASK_CHECK(speex_playout_pull(reset_st, a, 960) == 0);
    TASK_CHECK(speex_playout_pull(fresh, b, 7) == 0);
    TASK_CHECK(speex_playout_pull(fresh, b + 7, 953) == 0);
    TASK_CHECK(max_abs_diff(a, b, 960) <= 2.0e-6f);

    TASK_CHECK(speex_playout_get_stats(reset_st, &sa) == 0);
    TASK_CHECK(speex_playout_get_stats(fresh, &sb) == 0);
    TASK_CHECK(sa.source_consumed == sb.source_consumed);
    TASK_CHECK(sa.rendered == sb.rendered);
    TASK_CHECK(sa.delivered == sb.delivered);
    TASK_CHECK(sa.concealed == sb.concealed);
    TASK_CHECK(sa.quanta == sb.quanta);
    TASK_CHECK(sa.media_timestamp == sb.media_timestamp);
    TASK_CHECK(sa.correction_ppm == sb.correction_ppm);

    speex_playout_destroy(reset_st);
    speex_playout_destroy(fresh);
    return 0;
}

const struct task_case task_cases[] = {
    {"timeline", "jitter_peek_consume_wrap", timeline_jitter_peek_consume_wrap},
    {"timeline", "jitter_gap_variable", timeline_jitter_gap_variable_and_put_invalidation},
    {"timeline", "jitter_destroy_rejected", timeline_jitter_destroy_callback_rejected},
    {"timeline", "playout_overlap_duplicates_limit", timeline_playout_overlap_duplicates_wrap_limit},
    {"elastic", "fragment_reference", elastic_fragment_matches_whole_and_reference},
    {"elastic", "stereo_antiphase_reference", elastic_stereo_antiphase_reference},
    {"elastic", "occupancy_partial_zero", elastic_occupancy_zero_capacity_and_partial},
    {"lifecycle", "arguments_reject_recovery", lifecycle_arguments_and_reject_recovery},
    {"lifecycle", "reset_rebuild", lifecycle_reset_matches_rebuild_and_discards},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
