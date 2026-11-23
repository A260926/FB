/*
dr_mp3_stems.c - in-memory dual subband stem sessions.

Implementation notes:
  - Framing/reservoir/Huffman/stereo/reorder/antialias all happen inside the low
    level drmp3 decoder; this layer drives frames, owns the cut schedule, the per-stem
    continuation (held inside drmp3dec) and the gapless PCM clock.
  - The only persistent per-song data are the cut schedule copy and a compact frame
    index (byte offset + source granule). No whole-song PCM is stored and all work
    buffers hold at most one MP3 frame, so memory use is independent of song length.
  - Seeking restarts at most 64 physical audio frames before the target so the bit
    reservoir and both stems' overlap/synthesis histories are rebuilt, then discards
    up to the target. The cut schedule is always evaluated at absolute granules, never
    restarted at the seek point.
*/

#include "dr_mp3_stems.h"

#include <stdlib.h>
#include <string.h>

#define DRMP3_STEMS_MAX_LEAD_FRAMES   64
#define DRMP3_STEMS_SUBBANDS          32
#define DRMP3_STEMS_GRANULE_SAMPLES   576

typedef struct
{
    drmp3_uint64 byte_offset;   /* First byte of the frame in the source buffer. */
    drmp3_uint64 granule;       /* Absolute audio granule of the frame's granule 0. */
} drmp3_stems_frame_entry;

struct drmp3_stems
{
    const drmp3_uint8 *data;
    size_t             data_size;

    drmp3_stem_point  *points;
    size_t             point_count;

    drmp3_stems_frame_entry *entries;
    size_t                   entry_count;

    unsigned channels;
    unsigned sample_rate;

    drmp3_uint64 delay;             /* Effective gapless delay, raw PCM frames. */
    drmp3_uint64 padding;           /* Effective gapless padding, raw PCM frames. */
    drmp3_uint64 raw_total;         /* Raw PCM frames before gapless trimming. */
    drmp3_uint64 length;            /* Trimmed PCM frames per channel. */

    /* Mutable playback state. Backed up as a whole during a seek so a failed seek
       leaves the observable state untouched. */
    drmp3dec    decoder;
    size_t      next_entry;
    drmp3_uint64 delivered;         /* Trimmed frames handed to the caller. */
    float       frame_low[DRMP3_MAX_SAMPLES_PER_FRAME];
    float       frame_high[DRMP3_MAX_SAMPLES_PER_FRAME];
    float       low_buf[DRMP3_MAX_SAMPLES_PER_FRAME];
    float       high_buf[DRMP3_MAX_SAMPLES_PER_FRAME];
    size_t      buf_fill;           /* Trimmed frames currently buffered. */
    size_t      buf_pos;            /* Trimmed frames already consumed from buffer. */
};

/* Cut in effect at absolute audio granule g (binary search over the schedule). */
static unsigned stems_cut_at(const drmp3_stems *s, drmp3_uint64 g)
{
    size_t lo = 0, hi = s->point_count;
    while (hi - lo > 1)
    {
        size_t mid = lo + (hi - lo)/2;
        if (s->points[mid].granule <= g)
        {
            lo = mid;
        } else
        {
            hi = mid;
        }
    }
    return s->points[lo].low_subbands;
}

/* Index of the entry whose physical frame contains raw PCM frame target. */
static size_t stems_entry_for_raw(const drmp3_stems *s, drmp3_uint64 target)
{
    size_t lo = 0, hi = s->entry_count;
    while (hi - lo > 1)
    {
        size_t mid = lo + (hi - lo)/2;
        if (s->entries[mid].granule*DRMP3_STEMS_GRANULE_SAMPLES <= target)
        {
            lo = mid;
        } else
        {
            hi = mid;
        }
    }
    return lo;
}

/* Decode the next indexed frame into the internal frame buffers and advance
   next_entry. Returns the number of raw PCM frames produced (0 at end of stream). */
static drmp3_uint64 stems_decode_one(drmp3_stems *s)
{
    drmp3dec_frame_info info;
    drmp3_stems_frame_entry *e;
    const drmp3_uint8 *frame;
    size_t remain;
    unsigned cuts[2];
    int got;

    if (s->next_entry >= s->entry_count)
    {
        return 0;
    }

    e = &s->entries[s->next_entry];
    frame = s->data + e->byte_offset;
    remain = s->data_size - (size_t)e->byte_offset;

    cuts[0] = stems_cut_at(s, e->granule);
    cuts[1] = stems_cut_at(s, e->granule + 1);

    got = drmp3dec_decode_frame_stems(&s->decoder, frame, (int)remain,
        s->frame_low, s->frame_high, &info, cuts);
    s->next_entry++;

    return (got > 0) ? (drmp3_uint64)got : 0;
}

/* Append the gapless-visible frames of one decoded frame to the trimmed buffers. */
static void stems_append_trimmed(drmp3_stems *s, drmp3_uint64 base_raw,
    drmp3_uint64 frame_count)
{
    unsigned ch = s->channels;
    drmp3_uint64 visible_end = s->raw_total - s->padding;
    drmp3_uint64 i;

    for (i = 0; i < frame_count; i++)
    {
        drmp3_uint64 raw = base_raw + i;
        if (raw < s->delay || raw >= visible_end)
        {
            continue;
        }
        memcpy(s->low_buf + s->buf_fill*ch,  s->frame_low  + i*ch, ch*sizeof(float));
        memcpy(s->high_buf + s->buf_fill*ch, s->frame_high + i*ch, ch*sizeof(float));
        s->buf_fill++;
    }
}

/* Ensure at least one trimmed frame is buffered, or reach EOF. */
static int stems_fill(drmp3_stems *s)
{
    if (s->buf_pos < s->buf_fill)
    {
        return 1;
    }

    /* Buffer is drained; compact it. */
    s->buf_pos = 0;
    s->buf_fill = 0;

    while (s->buf_fill == 0 && s->next_entry < s->entry_count)
    {
        drmp3_uint64 base_raw = s->entries[s->next_entry].granule*DRMP3_STEMS_GRANULE_SAMPLES;
        drmp3_uint64 got = stems_decode_one(s);
        if (got == 0)
        {
            break;
        }
        stems_append_trimmed(s, base_raw, got);
    }

    return s->buf_fill > 0;
}

drmp3_stems *drmp3_stems_open_memory(const void *data, size_t data_size,
                                     const drmp3_stem_point *points, size_t point_count)
{
    drmp3_stems *s;
    drmp3 probe;
    drmp3dec scanner;
    drmp3dec_frame_info info;
    size_t cap = 0, count = 0, pos;
    drmp3_uint64 granule = 0;
    size_t i;

    if (data == NULL || data_size == 0 || points == NULL || point_count == 0)
    {
        return NULL;
    }
    if (points[0].granule != 0)
    {
        return NULL;
    }
    for (i = 0; i < point_count; i++)
    {
        if (points[i].low_subbands > DRMP3_STEMS_SUBBANDS)
        {
            return NULL;
        }
        if (i > 0 && points[i].granule <= points[i - 1].granule)
        {
            return NULL;
        }
    }

    /* A native memory probe gives the authoritative gapless interpretation, audio
       start offset (ID3/Xing already skipped) and stream format. */
    if (!drmp3_init_memory(&probe, data, data_size, NULL))
    {
        return NULL;
    }
    if (probe.channels < 1 || probe.channels > 2)
    {
        drmp3_uninit(&probe);
        return NULL;
    }

    s = (drmp3_stems*)calloc(1, sizeof(*s));
    if (s == NULL)
    {
        drmp3_uninit(&probe);
        return NULL;
    }
    s->data = (const drmp3_uint8*)data;
    /* In memory mode dr_mp3 trims trailing tags off memory.dataSize and records the
       first audio byte in streamStartOffset (non-zero after an ID3/Xing prefix). */
    s->data_size = probe.memory.dataSize;
    s->channels = probe.channels;
    s->sample_rate = probe.sampleRate;
    s->delay = probe.delayInPCMFrames;
    s->padding = probe.paddingInPCMFrames;
    pos = (size_t)probe.streamStartOffset;
    drmp3_uninit(&probe);

    s->points = (drmp3_stem_point*)malloc(point_count*sizeof(*s->points));
    if (s->points == NULL)
    {
        free(s);
        return NULL;
    }
    memcpy(s->points, points, point_count*sizeof(*s->points));
    s->point_count = point_count;

    cap = 64;
    s->entries = (drmp3_stems_frame_entry*)malloc(cap*sizeof(*s->entries));
    if (s->entries == NULL)
    {
        free(s->points);
        free(s);
        return NULL;
    }

    /* Scan physical frames with an output-less low level decoder. Only frames that
       produce PCM are indexed; tags and reservoir-incomplete frames are consumed but
       are not audio granules. */
    drmp3dec_init(&scanner);
    while (pos < s->data_size)
    {
        int got = drmp3dec_decode_frame(&scanner, s->data + pos, (int)(s->data_size - pos), NULL, &info);
        if (info.frame_bytes <= 0)
        {
            break;
        }
        if (got > 0)
        {
            if (count == cap)
            {
                drmp3_stems_frame_entry *grown;
                cap *= 2;
                grown = (drmp3_stems_frame_entry*)realloc(s->entries, cap*sizeof(*s->entries));
                if (grown == NULL)
                {
                    free(s->entries);
                    free(s->points);
                    free(s);
                    return NULL;
                }
                s->entries = grown;
            }
            s->entries[count].byte_offset = pos;
            s->entries[count].granule = granule;
            count++;
            granule += 2;  /* MPEG-1 Layer III: two granules of 576 raw frames each. */
        }
        pos += (size_t)info.frame_bytes;
    }

    s->entry_count = count;
    if (count == 0)
    {
        free(s->entries);
        free(s->points);
        free(s);
        return NULL;
    }

    s->raw_total = granule*DRMP3_STEMS_GRANULE_SAMPLES;

    /* Mirror dr_mp3's independent gapless clamps: an out-of-range value is ignored. */
    if (s->delay >= s->raw_total)
    {
        s->delay = 0;
    }
    if (s->padding >= s->raw_total)
    {
        s->padding = 0;
    }
    s->length = s->raw_total - s->delay - s->padding;

    drmp3dec_init(&s->decoder);
    s->next_entry = 0;
    s->delivered = 0;
    s->buf_fill = 0;
    s->buf_pos = 0;
    return s;
}

void drmp3_stems_close(drmp3_stems *s)
{
    if (s == NULL)
    {
        return;
    }
    free(s->entries);
    free(s->points);
    free(s);
}

drmp3_uint64 drmp3_stems_read(drmp3_stems *s, drmp3_uint64 frames, float *low, float *high)
{
    drmp3_uint64 want = frames;
    drmp3_uint64 done = 0;
    unsigned ch;

    if (s == NULL || frames == 0 || low == NULL || high == NULL)
    {
        return 0;
    }
    ch = s->channels;

    while (done < want)
    {
        size_t avail, take;

        if (s->buf_pos == s->buf_fill && !stems_fill(s))
        {
            break;  /* EOF. */
        }

        avail = s->buf_fill - s->buf_pos;
        take = (size_t)((want - done) < avail ? (want - done) : avail);
        memcpy(low  + done*ch, s->low_buf  + s->buf_pos*ch, take*ch*sizeof(float));
        memcpy(high + done*ch, s->high_buf + s->buf_pos*ch, take*ch*sizeof(float));
        s->buf_pos += take;
        done += take;
        s->delivered += take;
    }

    return done;
}

int drmp3_stems_seek(drmp3_stems *s, drmp3_uint64 frame)
{
    drmp3_stems backup;
    drmp3_uint64 raw_target, base_raw, first_visible;
    size_t target_entry, recovery, e;
    drmp3_uint64 got;
    int ok = 1;

    if (s == NULL || frame > s->length)
    {
        return 0;
    }

    /* Snapshot the whole mutable state; a failed seek restores it byte for byte. */
    backup = *s;

    raw_target = frame + s->delay;
    target_entry = stems_entry_for_raw(s, raw_target);
    recovery = (target_entry >= DRMP3_STEMS_MAX_LEAD_FRAMES)
        ? target_entry - (DRMP3_STEMS_MAX_LEAD_FRAMES - 1) : 0;

    drmp3dec_init(&s->decoder);
    s->next_entry = recovery;
    s->buf_pos = 0;
    s->buf_fill = 0;
    s->delivered = frame;

    /* Rebuild reservoir and both stems' histories through the frame before the target
       frame; the schedule is evaluated at absolute granules the whole way. A lead frame
       whose back reference is not yet satisfiable produces no audio but still consumes
       and warms the reservoir; it self-heals within a frame or two, far inside the
       bounded pre-roll window, so it is not a seek failure. */
    for (e = recovery; e < target_entry; e++)
    {
        stems_decode_one(s);
    }

    if (ok)
    {
        base_raw = s->entries[target_entry].granule*DRMP3_STEMS_GRANULE_SAMPLES;
        got = stems_decode_one(s);
        if (got == 0)
        {
            ok = 0;
        } else
        {
            stems_append_trimmed(s, base_raw, got);
            /* Buffered frame 0 is the first visible (post-delay) raw frame of this
               physical frame; skip the visible samples preceding raw_target. */
            first_visible = (base_raw > s->delay) ? base_raw : s->delay;
            if (raw_target < first_visible || (raw_target - first_visible) > s->buf_fill)
            {
                ok = 0;
            } else
            {
                s->buf_pos = (size_t)(raw_target - first_visible);
            }
        }
    }

    if (!ok)
    {
        *s = backup;  /* Entire state, including decoder history and buffered PCM. */
        return 0;
    }

    return 1;
}

drmp3_uint64 drmp3_stems_tell(const drmp3_stems *s)
{
    return (s != NULL) ? s->delivered : 0;
}

drmp3_uint64 drmp3_stems_length(const drmp3_stems *s)
{
    return (s != NULL) ? s->length : 0;
}

unsigned drmp3_stems_channels(const drmp3_stems *s)
{
    return (s != NULL) ? s->channels : 0;
}

unsigned drmp3_stems_sample_rate(const drmp3_stems *s)
{
    return (s != NULL) ? s->sample_rate : 0;
}
