////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//                Copyright (c) 1998 - 2024 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// push_decoder.c

// This module implements a persistent, push-oriented WavPack decoder for a
// single-threaded receive loop. Two independent byte lanes (WV = 0, WVC = 1)
// are appended in arbitrary-size fragments. Shortage of bytes is never
// treated as end-of-file; complete physical blocks are assembled, verified
// and paired into block groups, and PCM is produced on demand through the
// existing decode core (unpack_init / WavpackUnpackSamples) with a single
// persistent WavpackContext. At most one unconsumed block group (plus a
// partial successor) is ever held per lane, so memory use is bounded by the
// configured group limit and does not grow with program length.

#include <stdlib.h>
#include <string.h>

#include "wavpack_local.h"

#define PUSH_HDR        32      // size of a WavPack physical block header
#define PUSH_MAX_BLOCKS 8       // up to 8 channels => at most 8 substreams
#define PUSH_MAX_CHANS  8

typedef struct {
    unsigned char *data;        // retained bytes (metadata blocks are dropped promptly)
    size_t len, cap;            // used / allocated bytes
    size_t meta_bytes;          // zero-sample metadata bytes counted toward the group budget
    size_t boff [PUSH_MAX_BLOCKS];
    WavpackHeader bhdr [PUSH_MAX_BLOCKS];
    int nb;                     // physical audio blocks currently retained
    int complete;               // retained group reached its FINAL_BLOCK
    int finished;               // lane permanently closed by WavpackPushFinish()
} PushLane;

struct WavpackPushDecoder {
    PushLane lanes [2];
    size_t max_group;
    int require_wvc;
    int sticky;
    char error [160];

    WavpackContext *wpc;        // persistent decode context (created lazily)
    int format_known;
    uint32_t channels, sample_rate, bits_per_sample;
    int64_t total_samples;      // -1 while unknown
    int64_t next_index;
    int have_next_index;
    uint64_t samples_returned;
    int group_samples;          // frames in the active/ready group
    int group_consumed;         // frames already decoded from it
    int group_active;           // the retained group is installed in the WavpackContext
};

// --------------------------------- errors --------------------------------

static int push_fail (WavpackPushDecoder *s, const char *msg)
{
    if (!s->sticky) {
        strncpy (s->error, msg, sizeof (s->error) - 1);
        s->error [sizeof (s->error) - 1] = 0;
        s->sticky = 1;
    }

    return -1;
}

// ------------------------- physical block parsing ------------------------

// Validate the 32-byte header in file (little-endian) form, mirroring the
// checks performed by read_next_header().

static int header_bytes_valid (const unsigned char *p)
{
    return !(p [4] & 1) && p [6] < 16 && !p [7] && (p [6] || p [5] || p [4] >= 24) &&
        p [9] == 4 && p [8] >= (MIN_STREAM_VERS & 0xff) && p [8] <= (MAX_STREAM_VERS & 0xff) &&
        p [18] < 3 && !p [19];
}

// Return values: 1 = full block at offset parsed, 0 = need more bytes, -1 = bad.

static int parse_block (const unsigned char *buf, size_t len, size_t off,
    WavpackHeader *out, size_t *total)
{
    if (len - off < 4 || memcmp (buf + off, "wvpk", 4))
        return (len - off < 4) ? 0 : -1;

    if (len - off < PUSH_HDR || !header_bytes_valid (buf + off))
        return (len - off < PUSH_HDR) ? 0 : -1;

    memcpy (out, buf + off, PUSH_HDR);
    WavpackLittleEndianToNative (out, WavpackHeaderFormat);

    if (out->ckSize < 24)
        return -1;

    *total = (size_t) out->ckSize + 8;

    if (*total < PUSH_HDR)
        return -1;

    return (len - off >= *total) ? 1 : 0;
}

// ----------------------------- lane ingestion ----------------------------

// Absorb everything that can currently be understood from a lane: zero-sample
// metadata blocks are verified and released immediately, audio blocks are
// verified and retained until the FINAL_BLOCK of the group. Returns 0 while
// more bytes may simply be needed, -1 on a sticky error.

static int lane_pump (WavpackPushDecoder *s, int lane)
{
    PushLane *l = s->lanes + lane;
    size_t off = 0;

    // parsing always restarts at the front of the retained buffer, so rebuild
    // the audio-block index from scratch each time
    l->nb = 0;
    l->complete = 0;

    while (off < l->len) {
        WavpackHeader h;
        size_t total;
        int st = parse_block (l->data, l->len, off, &h, &total);

        if (!st) {
            // a complete header already tells us whether this block can ever fit;
            // refusing to notice that would make Feed return 0 bytes forever

            if (l->len - off >= PUSH_HDR && !memcmp (l->data + off, "wvpk", 4) &&
                header_bytes_valid (l->data + off)) {
                WavpackHeader peek;
                size_t declared;

                memcpy (&peek, l->data + off, PUSH_HDR);
                WavpackLittleEndianToNative (&peek, WavpackHeaderFormat);
                declared = (size_t) peek.ckSize + 8;

                if (peek.ckSize >= 24 && (declared > s->max_group ||
                    l->meta_bytes + off + declared > s->max_group))
                    return push_fail (s, "block group exceeds max_group_bytes limit");
            }

            break;
        }
        if (st < 0)
            return push_fail (s, lane ? "invalid WVC block header" : "invalid WV block header");

        // a single physical block that can never fit is a resource error

        if (total > s->max_group)
            return push_fail (s, "block exceeds max_group_bytes limit");

        if (!h.block_samples) {
            if (!WavpackVerifySingleBlock (l->data + off, 1))
                return push_fail (s, lane ? "invalid WVC metadata block" : "invalid WV metadata block");

            l->meta_bytes += total;

            if (l->meta_bytes > s->max_group)
                return push_fail (s, "block group exceeds max_group_bytes limit");

            memmove (l->data + off, l->data + off + total, l->len - off - total);
            l->len -= total;
            continue;
        }

        // audio block: start or continue this lane's retained group

        if (!l->nb) {
            if (!(h.flags & INITIAL_BLOCK))
                return push_fail (s, "audio group does not start with INITIAL_BLOCK");
        }
        else {
            if (h.flags & INITIAL_BLOCK)
                return push_fail (s, "unexpected INITIAL_BLOCK inside audio group");

            if (GET_BLOCK_INDEX (h) != GET_BLOCK_INDEX (l->bhdr [0]) ||
                h.block_samples != l->bhdr [0].block_samples)
                return push_fail (s, "substream block index/sample mismatch inside group");
        }

        if (l->nb >= PUSH_MAX_BLOCKS)
            return push_fail (s, "too many substream blocks in group (max 8 channels)");

        // everything from the start of the retained group through this block counts,
        // including the zero-sample metadata blocks already accounted for

        if (l->meta_bytes + off + total > s->max_group)
            return push_fail (s, "block group exceeds max_group_bytes limit");

        if (!WavpackVerifySingleBlock (l->data + off, 1))
            return push_fail (s, lane ? "WVC block checksum/structure error" :
                                       "WV block checksum/structure error");

        l->boff [l->nb] = off;
        l->bhdr [l->nb] = h;
        l->nb++;
        off += total;

        if (h.flags & FINAL_BLOCK) {
            if (!(h.flags & INITIAL_BLOCK) && l->nb < 2)
                return push_fail (s, "malformed INITIAL/FINAL block sequence");

            l->complete = 1;
            break;
        }
    }

    // the budget is exhausted without a closed group: another block can never
    // fit, so fail instead of returning 0 accepted bytes forever

    if (!l->complete && l->meta_bytes + l->len >= s->max_group)
        return push_fail (s, "block group exceeds max_group_bytes limit");

    return 0;
}

// ----------------------------- group assembly ----------------------------

// Check the two retained lanes against each other and derive the group shape.
// Returns 1 when the group is fully available, 0 when bytes are still missing,
// and -1 on a sticky error (bad ordering, mismatch, or truncated finish).

static int group_available (WavpackPushDecoder *s)
{
    PushLane *wv = s->lanes, *wvc = s->lanes + 1;

    if (!wv->nb || !wv->complete) {
        if (wv->finished && wv->len)
            return push_fail (s, "WV stream ended inside an incomplete block group");

        return 0;
    }

    if (s->require_wvc) {
        if (!wvc->nb || !wvc->complete) {
            if (wvc->finished)
                return push_fail (s, wvc->len ?
                    "WVC stream ended inside an incomplete block group" :
                    "missing WVC correction partner for WV block group");
            return 0;
        }

        if (wvc->nb != wv->nb ||
            GET_BLOCK_INDEX (wvc->bhdr [0]) != GET_BLOCK_INDEX (wv->bhdr [0]) ||
            wvc->bhdr [0].block_samples != wv->bhdr [0].block_samples)
            return push_fail (s, "WVC block group does not match WV block group");

        for (int i = 0; i < wv->nb; ++i)
            if (wvc->bhdr [i].flags != wv->bhdr [i].flags ||
                GET_BLOCK_INDEX (wvc->bhdr [i]) != GET_BLOCK_INDEX (wv->bhdr [i]) ||
                wvc->bhdr [i].block_samples != wv->bhdr [i].block_samples)
                return push_fail (s, "WVC substream header does not match its WV partner");
    }
    else if (wvc->len)
        return push_fail (s, "unexpected correction data on a lossless decoder");

    return 1;
}

// Lazily create the persistent decode context. No reader callbacks are needed
// because block buffers are installed directly, so no short read can occur.

static int ensure_context (WavpackPushDecoder *s)
{
    WavpackContext *wpc;

    if (s->wpc)
        return 0;

    wpc = (WavpackContext *) calloc (1, sizeof (WavpackContext));

    if (!wpc)
        return push_fail (s, "can't allocate decoder context");

    wpc->total_samples = -1;
    wpc->max_streams = OLD_MAX_STREAMS;
    wpc->open_flags = 0;
    wpc->streams = (WavpackStream **) calloc (1, sizeof (WavpackStream *));

    if (!wpc->streams) {
        free (wpc);
        return push_fail (s, "can't allocate decoder streams");
    }

    wpc->num_streams = 1;
    wpc->streams [0] = (WavpackStream *) calloc (1, sizeof (WavpackStream));

    if (!wpc->streams [0]) {
        free (wpc->streams);
        free (wpc);
        return push_fail (s, "can't allocate decoder stream");
    }

    wpc->streams [0]->wpc = wpc;
    s->wpc = wpc;
    return 0;
}

// Make sure the context has one WavpackStream per physical substream block.

static int ensure_streams (WavpackPushDecoder *s, int n)
{
    WavpackContext *wpc = s->wpc;

    if (n > wpc->num_streams) {
        WavpackStream **more = (WavpackStream **) realloc (wpc->streams, n * sizeof (more [0]));

        if (!more)
            return push_fail (s, "can't allocate decoder streams");

        wpc->streams = more;

        while (wpc->num_streams < n) {
            WavpackStream *wps = (WavpackStream *) calloc (1, sizeof (WavpackStream));

            if (!wps)
                return push_fail (s, "can't allocate decoder stream");

            wps->wpc = wpc;
            wps->stream_index = wpc->num_streams;
            wpc->streams [wpc->num_streams++] = wps;
        }
    }

    return 0;
}

// Install the ready group into the persistent context and run the library's
// own metadata/entropy initialization. Block indexes are the original 40-bit
// values; OPEN_STREAMING is never used.

static int activate_group (WavpackPushDecoder *s)
{
    PushLane *wv = s->lanes, *wvc = s->lanes + 1;
    WavpackContext *wpc;
    int n = wv->nb, channels = 0;

    if (ensure_context (s) || ensure_streams (s, n))
        return -1;

    wpc = s->wpc;
    wpc->wvc_flag = s->require_wvc;
    wpc->open_flags = 0;
    wpc->crc_errors = 0;

    for (int i = 0; i < n; ++i) {
        WavpackStream *wps = wpc->streams [i];
        size_t wv_total = (size_t) wv->bhdr [i].ckSize + 8;

        if (wps->blockbuff || wps->block2buff)
            return push_fail (s, "internal: stale block buffers retained");

        // structural sequence check: INITIAL first, FINAL last, plain middle blocks

        if (i == 0 && !(wv->bhdr [i].flags & INITIAL_BLOCK))
            return push_fail (s, "audio group does not start with INITIAL_BLOCK");

        if (i == n - 1 && !(wv->bhdr [i].flags & FINAL_BLOCK))
            return push_fail (s, "audio group does not end with FINAL_BLOCK");

        if (i && (wv->bhdr [i].flags & INITIAL_BLOCK))
            return push_fail (s, "unexpected INITIAL_BLOCK inside audio group");

        if (i < n - 1 && (wv->bhdr [i].flags & FINAL_BLOCK))
            return push_fail (s, "unexpected FINAL_BLOCK inside audio group");

        channels += (wv->bhdr [i].flags & MONO_FLAG) ? 1 : 2;

        wps->blockbuff = (unsigned char *) malloc (wv_total);

        if (!wps->blockbuff)
            return push_fail (s, "can't allocate WV block buffer");

        memcpy (wps->blockbuff, wv->data + wv->boff [i], wv_total);
        wps->wphdr = wv->bhdr [i];       // native-endian working header

        if (s->require_wvc) {
            size_t wvc_total = (size_t) wvc->bhdr [i].ckSize + 8;

            wps->block2buff = (unsigned char *) malloc (wvc_total);

            if (!wps->block2buff)
                return push_fail (s, "can't allocate WVC block buffer");

            memcpy (wps->block2buff, wvc->data + wvc->boff [i], wvc_total);

            // the hybrid-lossless CRC checked at end-of-block lives in the WVC header,
            // exactly as the standard open path arranges

            wps->wphdr = wvc->bhdr [i];
        }

        wps->init_done = FALSE;

        if (!unpack_init (wpc, i)) {
            push_fail (s, wpc->error_message [0] ? wpc->error_message :
                "incompatible WavPack block metadata");
            return -1;
        }

        wps->init_done = TRUE;
    }

    // Derive the audio format exactly as WavpackOpenFileInputEx64() does after
    // its header loop, since unpack_init() alone only gathers metadata.

    {
        WavpackStream *last = wpc->streams [n - 1];

        wpc->config.flags &= ~0xff;
        wpc->config.flags |= last->wphdr.flags & 0xff;

        if (!wpc->config.num_channels) {
            wpc->config.num_channels = (last->wphdr.flags & MONO_FLAG) ? 1 : 2;
            wpc->config.channel_mask = 0x5 - wpc->config.num_channels;
        }

        wpc->config.bytes_per_sample = (last->wphdr.flags & BYTES_STORED) + 1;
        wpc->config.bits_per_sample = (wpc->config.bytes_per_sample * 8) -
            ((last->wphdr.flags & SHIFT_MASK) >> SHIFT_LSB);

        if (!wpc->config.sample_rate) {
            if (!last->wphdr.block_samples || (last->wphdr.flags & SRATE_MASK) == SRATE_MASK)
                wpc->config.sample_rate = 44100;
            else
                wpc->config.sample_rate = sample_rates [(last->wphdr.flags & SRATE_MASK) >> SRATE_LSB];
        }
    }

    if (channels < 1 || channels > PUSH_MAX_CHANS || channels != wpc->config.num_channels)
        return push_fail (s, "unsupported or inconsistent channel count (1-8 supported)");

    if (wpc->config.sample_rate <= 0)
        return push_fail (s, "invalid sample rate in WavPack block");

    if (wpc->config.bits_per_sample != 16 && wpc->config.bits_per_sample != 24)
        return push_fail (s, "unsupported bit depth (16 or 24-bit integer PCM supported)");

    if (wv->bhdr [0].flags & (FLOAT_DATA | DSD_FLAG))
        return push_fail (s, "float/DSD data is not supported by the push decoder");

    if (!s->format_known) {
        s->format_known = 1;
        s->channels = (uint32_t) wpc->config.num_channels;
        s->sample_rate = (uint32_t) wpc->config.sample_rate;
        s->bits_per_sample = (uint32_t) wpc->config.bits_per_sample;
        s->total_samples = GET_TOTAL_SAMPLES (wv->bhdr [0]);
    }
    else if ((uint32_t) wpc->config.num_channels != s->channels ||
             (uint32_t) wpc->config.sample_rate != s->sample_rate ||
             (uint32_t) wpc->config.bits_per_sample != s->bits_per_sample)
        return push_fail (s, "format changed mid-stream");

    if (s->have_next_index && GET_BLOCK_INDEX (wv->bhdr [0]) != s->next_index)
        return push_fail (s, "block group out of sequence (gap, duplicate or reorder)");

    if (!s->have_next_index) {
        s->have_next_index = 1;
        s->next_index = GET_BLOCK_INDEX (wv->bhdr [0]);
    }

    s->group_samples = (int) wv->bhdr [0].block_samples;
    s->group_consumed = 0;
    s->group_active = 1;
    return 0;
}

// Release the retained group on both lanes and detach its blocks from the
// persistent context (the context itself and the format info stay alive).

static void release_group (WavpackPushDecoder *s)
{
    WavpackContext *wpc = s->wpc;

    if (wpc)
        free_streams (wpc);

    for (int lane = 0; lane < 2; ++lane) {
        PushLane *l = s->lanes + lane;

        if (l->nb) {
            size_t tail = (size_t) l->boff [l->nb - 1] + (size_t) l->bhdr [l->nb - 1].ckSize + 8;

            if (tail < l->len)
                memmove (l->data, l->data + tail, l->len - tail);

            l->len -= tail;
        }

        l->nb = 0;
        l->complete = 0;
        l->meta_bytes = 0;
    }

    s->group_active = 0;

    // parse the successor bytes left in each lane (metadata blocks or next group)

    if (lane_pump (s, 0))
        return;

    if (s->require_wvc)
        lane_pump (s, 1);
}

// --------------------------------- API -----------------------------------

WavpackPushDecoder *WavpackPushCreate (size_t max_group_bytes, int require_wvc)
{
    WavpackPushDecoder *s;

    if (max_group_bytes < 32 || (require_wvc & ~1))
        return NULL;

    s = (WavpackPushDecoder *) calloc (1, sizeof (*s));

    if (!s)
        return NULL;

    s->max_group = max_group_bytes;
    s->require_wvc = require_wvc;
    s->total_samples = -1;
    return s;
}

int WavpackPushFeed (WavpackPushDecoder *s, int lane, const void *data, size_t size, size_t *accepted)
{
    PushLane *l;
    size_t room, take;

    if (accepted)
        *accepted = 0;

    if (!s || !accepted || lane < 0 || lane > 1 || (size && !data))
        return -1;

    if (s->sticky)
        return -1;

    if (!s->require_wvc && lane == 1 && size)
        return -1;                      // correction lane rejected, object stays usable

    if (!size)
        return 0;

    l = s->lanes + lane;

    if (l->complete)
        return 0;                       // backpressure: group not consumed yet

    room = s->max_group - l->len - l->meta_bytes;
    take = size < room ? size : room;

    if (take && l->len + take > l->cap) {
        size_t ncap = l->cap ? l->cap : 256;
        unsigned char *ndata;

        while (ncap < l->len + take)
            ncap *= 2;

        if (ncap > s->max_group)
            ncap = s->max_group;

        ndata = (unsigned char *) realloc (l->data, ncap);

        if (!ndata)
            return push_fail (s, "can't grow lane buffer");

        l->data = ndata;
        l->cap = ncap;
    }

    if (take)
        memcpy (l->data + l->len, data, take);

    l->len += take;
    *accepted = take;

    if (lane_pump (s, lane))
        return -1;

    return s->sticky ? -1 : 0;
}

int WavpackPushFinish (WavpackPushDecoder *s, int lane)
{
    if (!s || lane < 0 || lane > 1)
        return -1;

    if (s->sticky)
        return -1;

    s->lanes [lane].finished = 1;
    return 0;
}

int WavpackPushRead (WavpackPushDecoder *s, int32_t *pcm, uint32_t capacity, uint32_t *written)
{
    PushLane *wv = s->lanes, *wvc = s->lanes + 1;
    int both_finished, ready;

    if (written)
        *written = 0;

    if (!s || !written || (capacity && !pcm))
        return -1;

    if (s->sticky)
        return WV_PUSH_ERROR;

    ready = group_available (s);

    if (ready < 0)
        return WV_PUSH_ERROR;

    both_finished = wv->finished && (!s->require_wvc || wvc->finished);

    if (!ready) {
        // no complete group remains, so any retained bytes are a truncated
        // group or an unmatched trailing correction group
        if (both_finished) {
            if (wv->len || (s->require_wvc && wvc->len)) {
                push_fail (s, "stream finished with an unclosed block group");
                return WV_PUSH_ERROR;
            }

            if (s->total_samples >= 0 && (int64_t) s->samples_returned != s->total_samples) {
                push_fail (s, "delivered sample count does not match declared total_samples");
                return WV_PUSH_ERROR;
            }

            return WV_PUSH_DONE;
        }

        if (!capacity)
            return WV_PUSH_NEED_OUTPUT;

        // WV is reported first: until its current group is complete (or WV is
        // finished and drained) it is the gating lane; afterwards only the
        // correction lane can unblock decoding

        if (!(wv->complete || (wv->finished && !wv->len)))
            return WV_PUSH_NEED_WV;

        return WV_PUSH_NEED_WVC;
    }

    if (!capacity)
        return WV_PUSH_NEED_OUTPUT;     // do not advance the decoder without room

    if (!s->group_active && activate_group (s))
        return WV_PUSH_ERROR;

    {
        WavpackContext *wpc = s->wpc;
        uint32_t want = (uint32_t) (s->group_samples - s->group_consumed);
        uint32_t got, errors_before = wpc->crc_errors;

        if (want > capacity)
            want = capacity;

        got = WavpackUnpackSamples (wpc, pcm, want);

        if (got != want || wpc->crc_errors != errors_before) {
            push_fail (s, wpc->error_message [0] ? wpc->error_message :
                "WavPack decoding or CRC error");
            return WV_PUSH_ERROR;
        }

        *written = got;
        s->samples_returned += got;
        s->group_consumed += (int) got;

        if (s->group_consumed == s->group_samples) {
            s->next_index = GET_BLOCK_INDEX (wv->bhdr [0]) + (int64_t) s->group_samples;
            release_group (s);
        }
    }

    return WV_PUSH_PCM;
}

int WavpackPushGetInfo (WavpackPushDecoder *s, WavpackPushInfo *info)
{
    if (!s || !info)
        return -1;

    info->channels = s->format_known ? s->channels : 0;
    info->sample_rate = s->format_known ? s->sample_rate : 0;
    info->bits_per_sample = s->format_known ? s->bits_per_sample : 0;
    info->samples_returned = s->samples_returned;
    info->buffered_wv = s->lanes [0].len;
    info->buffered_wvc = s->lanes [1].len;
    return s->format_known ? 1 : 0;
}

const char *WavpackPushGetError (WavpackPushDecoder *s)
{
    return (s && s->sticky) ? s->error : NULL;
}

void WavpackPushDestroy (WavpackPushDecoder *s)
{
    if (!s)
        return;

    if (s->wpc) {
        WavpackContext *wpc = s->wpc;

        free_streams (wpc);

        if (wpc->streams) {
            for (int i = 0; i < wpc->num_streams; ++i)
                free (wpc->streams [i]);

            free (wpc->streams);
        }

        if (wpc->wrapper_data)
            free (wpc->wrapper_data);

        if (wpc->metadata) {
            for (int i = 0; i < wpc->metacount; ++i)
                free (wpc->metadata [i].data);

            free (wpc->metadata);
        }

        if (wpc->channel_identities)
            free (wpc->channel_identities);

        if (wpc->channel_reordering)
            free (wpc->channel_reordering);

        free (wpc);
    }

    free (s->lanes [0].data);
    free (s->lanes [1].data);
    free (s);
}
