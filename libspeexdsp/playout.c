#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "speex/speex_playout.h"
#include "speex/speex_jitter.h"
#include "speex/speex_resampler.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef EXPORT
#define EXPORT
#endif

#define PO_QUANTUM_FRAMES 480u
#define PO_MIN_PACKET_FRAMES 120u
#define PO_MAX_PACKET_FRAMES 1920u
#define PO_MAX_ACTIVE_PACKETS 200u
#define PO_HORIZON_FRAMES 96000
#define PO_RATE_BASE 1000000u

typedef struct PoRange_ {
    uint32_t start;
    uint32_t frames;
    int used;
} PoRange;

struct SpeexPlayout_ {
    unsigned channels;
    unsigned target_frames;

    JitterBuffer *jitter;
    SpeexResamplerState *resampler;

    int started;
    uint32_t start;

    SpeexPlayoutStats stats;

    /* Timeline metadata only. All PCM payloads are owned by JitterBuffer. */
    PoRange ranges[PO_MAX_ACTIVE_PACKETS];

    /* Peek destination must hold one complete maximum-size float packet. */
    float peek_pcm[PO_MAX_PACKET_FRAMES * 2u];

    /* One complete device quantum, cached before any fragment is delivered. */
    float quantum[PO_QUANTUM_FRAMES * 2u];
    unsigned quantum_used;
};

static int32_t po_sdiff(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b);
}

static uint32_t po_media_timestamp(const SpeexPlayout *st)
{
    return (uint32_t)(st->start + st->stats.source_consumed);
}

static void po_drop_consumed_ranges(SpeexPlayout *st)
{
    uint32_t now = po_media_timestamp(st);
    unsigned i;
    for (i = 0; i < PO_MAX_ACTIVE_PACKETS; ++i) {
        if (st->ranges[i].used) {
            uint32_t end = st->ranges[i].start + st->ranges[i].frames;
            if (po_sdiff(end, now) <= 0)
                st->ranges[i].used = 0;
        }
    }
}

static int po_range_active(const PoRange *r, uint32_t now)
{
    uint32_t end;
    if (!r->used)
        return 0;
    end = r->start + r->frames;
    return po_sdiff(end, now) > 0;
}

EXPORT SpeexPlayout *speex_playout_init(unsigned channels, unsigned target_frames)
{
    SpeexPlayout *st;
    JitterBuffer *jb;

    if (channels != 1u && channels != 2u)
        return NULL;
    if (target_frames < PO_QUANTUM_FRAMES || target_frames > 9600u ||
        (target_frames % PO_QUANTUM_FRAMES) != 0u)
        return NULL;

    st = (SpeexPlayout *)calloc(1, sizeof(*st));
    if (!st)
        return NULL;

    jb = jitter_buffer_init((int)PO_QUANTUM_FRAMES);
    if (!jb) {
        free(st);
        return NULL;
    }

    st->channels = channels;
    st->target_frames = target_frames;
    st->jitter = jb;
    st->quantum_used = PO_QUANTUM_FRAMES;
    st->started = 0;
    return st;
}

EXPORT int speex_playout_reset(SpeexPlayout *st, uint32_t start)
{
    int err = 0;
    int rc;

    if (!st)
        return SPEEX_PLAYOUT_BAD_ARGUMENT;

    memset(&st->stats, 0, sizeof(st->stats));
    memset(st->ranges, 0, sizeof(st->ranges));
    st->start = start;
    st->started = 1;
    st->quantum_used = PO_QUANTUM_FRAMES;

    /* Reset the production buffer, then re-enter its explicit pull mode. */
    jitter_buffer_reset(st->jitter);
    rc = jitter_buffer_begin_pull(st->jitter, start);
    if (rc != JITTER_BUFFER_OK)
        return SPEEX_PLAYOUT_BAD_STATE;
    /* The explicit update-delay call also latches off arrival-based automatic
       adjustment; pull mode additionally guards the scheduler entry points. */
    jitter_buffer_update_delay(st->jitter, NULL, NULL);

    /* Reset must be observably equivalent to destroy+init+reset. */
    if (st->resampler) {
        speex_resampler_destroy(st->resampler);
        st->resampler = NULL;
    }
    st->resampler = speex_resampler_init_frac(st->channels,
                                              PO_RATE_BASE, PO_RATE_BASE,
                                              48000u, 48000u, 3, &err);
    if (!st->resampler || err != RESAMPLER_ERR_SUCCESS)
        return SPEEX_PLAYOUT_NO_MEMORY;
    speex_resampler_skip_zeros(st->resampler);
    return SPEEX_PLAYOUT_OK;
}

EXPORT void speex_playout_destroy(SpeexPlayout *st)
{
    if (!st)
        return;
    if (st->resampler)
        speex_resampler_destroy(st->resampler);
    if (st->jitter)
        jitter_buffer_destroy(st->jitter);
    free(st);
}

EXPORT int speex_playout_put(SpeexPlayout *st, uint32_t timestamp,
                      const float *pcm, uint32_t frames)
{
    uint32_t now;
    int32_t start_delta;
    int32_t end_delta;
    unsigned i;
    unsigned active_count = 0;
    int free_range = -1;
    JitterBufferPacket packet;

    if (!st || !pcm)
        return SPEEX_PLAYOUT_BAD_ARGUMENT;
    if (frames < PO_MIN_PACKET_FRAMES || frames > PO_MAX_PACKET_FRAMES)
        return SPEEX_PLAYOUT_BAD_ARGUMENT;
    if (!st->started || !st->resampler)
        return SPEEX_PLAYOUT_BAD_STATE;
    /* Callers deliver packet events only at control-quantum boundaries. */
    if ((st->stats.delivered % PO_QUANTUM_FRAMES) != 0u)
        return SPEEX_PLAYOUT_BAD_STATE;

    now = po_media_timestamp(st);

    /* An exact start/length match is a retransmission of the first copy while
       that copy remains queued, including after partial source consumption. */
    for (i = 0; i < PO_MAX_ACTIVE_PACKETS; ++i) {
        if (po_range_active(&st->ranges[i], now) &&
            st->ranges[i].start == timestamp &&
            st->ranges[i].frames == frames)
            return SPEEX_PLAYOUT_OK;
    }

    for (i = 0; i < PO_MAX_ACTIVE_PACKETS; ++i) {
        if (po_range_active(&st->ranges[i], now))
            ++active_count;
        if ((!st->ranges[i].used || !po_range_active(&st->ranges[i], now)) &&
            free_range < 0)
            free_range = (int)i;
    }
    if (active_count >= PO_MAX_ACTIVE_PACKETS || free_range < 0)
        return SPEEX_PLAYOUT_REJECTED;

    start_delta = po_sdiff(timestamp, now);
    if (start_delta < 0 || (uint32_t)start_delta > PO_HORIZON_FRAMES)
        return SPEEX_PLAYOUT_REJECTED;

    end_delta = po_sdiff(timestamp + frames, now);
    if (end_delta < 0 || end_delta > PO_HORIZON_FRAMES)
        return SPEEX_PLAYOUT_REJECTED;

    /* Half-open interval overlap. Adjacent end == next start is allowed. */
    for (i = 0; i < PO_MAX_ACTIVE_PACKETS; ++i) {
        const PoRange *r = &st->ranges[i];
        int32_t old_start, old_end, new_start, new_end;
        if (!po_range_active(r, now))
            continue;
        old_start = po_sdiff(r->start, now);
        old_end = old_start + (int32_t)r->frames;
        new_start = start_delta;
        new_end = start_delta + (int32_t)frames;
        if (new_start < old_end && old_start < new_end)
            return SPEEX_PLAYOUT_REJECTED;
    }

    memset(&packet, 0, sizeof(packet));
    packet.data = (char *)(void *)pcm;
    packet.len = frames * st->channels * (uint32_t)sizeof(float);
    packet.timestamp = timestamp;
    packet.span = frames;
    jitter_buffer_put(st->jitter, &packet);

    st->ranges[free_range].used = 1;
    st->ranges[free_range].start = timestamp;
    st->ranges[free_range].frames = frames;

    /* A direct put invalidates any old peek authorization. Playout does not
       retain an authorization across calls to this function. */
    return SPEEX_PLAYOUT_OK;
}

static int po_generate_quantum(SpeexPlayout *st)
{
    uint32_t now = po_media_timestamp(st);
    int32_t L = 0;
    int32_t raw_u;
    int32_t u;
    uint32_t ratio_num;
    uint32_t produced = 0;
    unsigned guard = 0;
    unsigned i;
    int rc;

    for (i = 0; i < PO_MAX_ACTIVE_PACKETS; ++i) {
        const PoRange *r = &st->ranges[i];
        int32_t end_delta;
        if (!po_range_active(r, now))
            continue;
        end_delta = po_sdiff(r->start + r->frames, now);
        if (end_delta > L)
            L = end_delta;
    }

    /* C99 signed division truncates toward zero, as required. */
    raw_u = (int32_t)(((int64_t)L - (int64_t)st->target_frames) / 48);
    u = raw_u;
    if (u < -1000)
        u = -1000;
    if (u > 1000)
        u = 1000;
    st->stats.correction_ppm = u;
    ratio_num = (uint32_t)((int32_t)PO_RATE_BASE + u);

    rc = speex_resampler_set_rate_frac(st->resampler, ratio_num,
                                       PO_RATE_BASE, 48000u, 48000u);
    if (rc != RESAMPLER_ERR_SUCCESS)
        return SPEEX_PLAYOUT_BAD_STATE;

    /* Report the real queued media horizon; pull mode makes this a timestamp
       bookkeeping call rather than an automatic delay decision. */
    jitter_buffer_remaining_span(st->jitter, (spx_uint32_t)(L > 0 ? L : 0));

    ++st->stats.quanta;

    while (produced < PO_QUANTUM_FRAMES) {
        JitterBufferPacket peek_packet;
        spx_int32_t offset = 0;
        spx_uint32_t in_len;
        spx_uint32_t out_len;
        spx_uint32_t consumed;
        const float *source;
        int gap;
        int rv;

        memset(&peek_packet, 0, sizeof(peek_packet));
        peek_packet.data = (char *)(void *)st->peek_pcm;
        peek_packet.len = (spx_uint32_t)sizeof(st->peek_pcm);
        rv = jitter_buffer_peek(st->jitter, &peek_packet,
                                (spx_int32_t)PO_MAX_PACKET_FRAMES, &offset);

        if (rv == JITTER_BUFFER_OK) {
            int32_t available_i;
            uint32_t skip;
            if (offset > 0 || peek_packet.span == 0)
                return SPEEX_PLAYOUT_BAD_STATE;
            available_i = offset + (int32_t)peek_packet.span;
            if (available_i <= 0)
                return SPEEX_PLAYOUT_BAD_STATE;
            skip = (uint32_t)(-offset);
            in_len = (uint32_t)available_i;
            source = st->peek_pcm + (size_t)skip * st->channels;
            gap = 0;
        } else if (rv == JITTER_BUFFER_MISSING) {
            if (peek_packet.span == 0)
                return SPEEX_PLAYOUT_BAD_STATE;
            in_len = peek_packet.span;
            source = NULL;
            gap = 1;
        } else {
            return SPEEX_PLAYOUT_BAD_STATE;
        }

        consumed = in_len;
        out_len = PO_QUANTUM_FRAMES - produced;
        rc = speex_resampler_process_interleaved_float(
            st->resampler, source, &consumed,
            st->quantum + (size_t)produced * st->channels, &out_len);
        if (rc != RESAMPLER_ERR_SUCCESS)
            return SPEEX_PLAYOUT_BAD_STATE;

        if (consumed > 0) {
            rc = jitter_buffer_consume(st->jitter, consumed);
            if (rc != JITTER_BUFFER_OK)
                return SPEEX_PLAYOUT_BAD_STATE;
            st->stats.source_consumed += consumed;
            if (gap)
                st->stats.concealed += consumed;
            po_drop_consumed_ranges(st);
        }

        produced += out_len;

        /* Positive consumed or produced makes progress. The guard only turns a
           theoretically impossible zero/zero resampler condition into error. */
        if (consumed == 0 && out_len == 0)
            return SPEEX_PLAYOUT_BAD_STATE;
        if (++guard > 100000u)
            return SPEEX_PLAYOUT_BAD_STATE;
    }

    st->stats.rendered += PO_QUANTUM_FRAMES;
    st->quantum_used = 0;
    return SPEEX_PLAYOUT_OK;
}

EXPORT int speex_playout_pull(SpeexPlayout *st, float *pcm, uint32_t frames)
{
    uint32_t done = 0;

    if (!st)
        return SPEEX_PLAYOUT_BAD_ARGUMENT;
    if (frames == 0)
        return SPEEX_PLAYOUT_OK;
    if (!pcm)
        return SPEEX_PLAYOUT_BAD_ARGUMENT;
    if (!st->started || !st->resampler)
        return SPEEX_PLAYOUT_BAD_STATE;

    while (done < frames) {
        uint32_t take;
        int rc;

        if (st->quantum_used >= PO_QUANTUM_FRAMES) {
            rc = po_generate_quantum(st);
            if (rc != SPEEX_PLAYOUT_OK)
                return rc;
        }

        take = PO_QUANTUM_FRAMES - st->quantum_used;
        if (take > frames - done)
            take = frames - done;

        memcpy(pcm + (size_t)done * st->channels,
               st->quantum + (size_t)st->quantum_used * st->channels,
               (size_t)take * st->channels * sizeof(float));

        st->quantum_used += take;
        done += take;
        st->stats.delivered += take;
    }

    return SPEEX_PLAYOUT_OK;
}

EXPORT int speex_playout_get_stats(SpeexPlayout *st, SpeexPlayoutStats *out)
{
    if (!st || !out)
        return SPEEX_PLAYOUT_BAD_ARGUMENT;
    st->stats.media_timestamp = po_media_timestamp(st);
    *out = st->stats;
    return SPEEX_PLAYOUT_OK;
}
