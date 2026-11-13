/*
    minimp3_push.c - push (streaming, non-seekable) MP3 decoder front-end.
    https://github.com/lieff/minimp3
    To the extent possible under law, the author(s) have dedicated all copyright and related and neighboring rights to this software to the public domain worldwide.
    This software is distributed without any warranty.
    See <http://creativecommons.org/publicdomain/zero/1.0/>.
*/
#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"
#include "minimp3_push.h"

#include <string.h>

/* Staging buffer: largest ordinary MPEG-1 frame is 1441 bytes, 2048 leaves
   room for header look-ahead and one re-sync point without ever growing. */
#define PUSH_IN_CAP         2048
#define PUSH_RING_SCALARS   MINIMP3_MAX_SAMPLES_PER_FRAME

enum
{
    PUSH_PHASE_START = 0,   /* before the first audio frame, ID3v2 may appear */
    PUSH_PHASE_ID3V2,       /* discarding an ID3v2 tag body */
    PUSH_PHASE_STREAM,      /* frame-by-frame decode */
    PUSH_PHASE_DONE         /* stream finished */
};

void mp3dec_push_init(mp3dec_push_t *dec)
{
    if (!dec)
        return;
    memset(dec, 0, sizeof(*dec));
    mp3dec_init(&dec->mp3d);
    dec->detected_samples = UINT64_MAX;
    dec->phase = PUSH_PHASE_START;
}

void mp3dec_push_close(mp3dec_push_t *dec)
{
    /* No heap memory is owned, a plain reset is enough. */
    mp3dec_push_init(dec);
}

/* ---- PCM ring ----------------------------------------------------------- */

static size_t push_ring_avail(const mp3dec_push_t *dec)
{
    return PUSH_RING_SCALARS - dec->pcm_fill;
}

static void push_ring_push(mp3dec_push_t *dec, const mp3d_sample_t *src, size_t n)
{
    size_t pos = (dec->pcm_pos + dec->pcm_fill) % PUSH_RING_SCALARS;
    size_t first = PUSH_RING_SCALARS - pos;
    if (first > n)
        first = n;
    memcpy(dec->pcm + pos, src, first*sizeof(mp3d_sample_t));
    if (n > first)
        memcpy(dec->pcm, src + first, (n - first)*sizeof(mp3d_sample_t));
    dec->pcm_fill += n;
}

/* Drains whole frames only: stereo output always keeps L/R pairs together. */
static size_t push_ring_drain(mp3dec_push_t *dec, mp3d_sample_t *dst, size_t cap)
{
    size_t n = dec->pcm_fill < cap ? dec->pcm_fill : cap;
    if (dec->channels > 1)
        n -= n % (size_t)dec->channels;
    if (!n)
        return 0;
    size_t first = PUSH_RING_SCALARS - dec->pcm_pos;
    if (first > n)
        first = n;
    memcpy(dst, dec->pcm + dec->pcm_pos, first*sizeof(mp3d_sample_t));
    if (n > first)
        memcpy(dst + first, dec->pcm, (n - first)*sizeof(mp3d_sample_t));
    dec->pcm_pos = (dec->pcm_pos + n) % PUSH_RING_SCALARS;
    dec->pcm_fill -= n;
    return n;
}

/* ---- tags --------------------------------------------------------------- */

/* Returns 1 and the full tag span in *total for an ID3v2.3/2.4 header,
   0 while fewer than 10 bytes are available, -1 when the bytes are no tag. */
static int push_id3v2_span(const uint8_t *p, size_t n, size_t *total)
{
    if (n < 10)
        return 0;
    if (memcmp(p, "ID3", 3))
        return -1;
    if (p[3] != 3 && p[3] != 4)
        return -1; /* only ID3v2.3 / 2.4 are supported */
    if ((p[5] & 15) || (p[6] & 0x80) || (p[7] & 0x80) || (p[8] & 0x80) || (p[9] & 0x80))
        return -1;
    size_t body = (size_t)(((p[6] & 0x7f) << 21) | ((p[7] & 0x7f) << 14) |
                           ((p[8] & 0x7f) << 7)  |  (p[9] & 0x7f));
    *total = 10 + body + (p[5] & 16 ? 10 : 0); /* optional footer */
    return 1;
}

/* Removes ID3v1 / APEv2 trailers from the buffered tail at end of input. */
static size_t push_strip_tail_tags(mp3dec_push_t *dec)
{
    size_t n = dec->in_fill, removed = 0;
    if (n >= 128 && !memcmp(dec->in + n - 128, "TAG", 3))
    {
        size_t r = 128;
        if (n >= 128 + 227 && !memcmp(dec->in + n - 128 - 227, "TAG+", 4))
            r += 227;
        n -= r;
        removed += r;
    }
    if (n > 32 && !memcmp(dec->in + n - 32, "APETAGEX", 8))
    {
        const uint8_t *tag = dec->in + (n - 32) + 8 + 4;
        uint32_t tag_size = (uint32_t)((tag[3] << 24) | (tag[2] << 16) | (tag[1] << 8) | tag[0]);
        n -= 32;
        if (n >= tag_size)
        {
            n -= tag_size;
            removed += 32 + tag_size;
        } else
        {
            n += 32; /* malformed APE tag, keep the bytes */
        }
    }
    dec->in_fill = n;
    return removed;
}

/* ---- frame processing --------------------------------------------------- */

static void push_fail(mp3dec_push_t *dec, int code)
{
    dec->last_error = code;
}

/* Pushes one decoded frame's PCM through gapless trimming into the ring.
   Returns the number of scalars accepted, 0 when the ring cannot hold it. */
static size_t push_trim_and_ring(mp3dec_push_t *dec, int samples, int channels)
{
    size_t sc = (size_t)samples*channels;
    size_t sk = dec->to_skip < sc ? dec->to_skip : sc;
    dec->to_skip -= sk;
    sc -= sk;
    if (dec->detected_samples != UINT64_MAX)
    {
        size_t room = dec->detected_samples > dec->produced ?
                      (size_t)(dec->detected_samples - dec->produced) : 0;
        if (sc > room)
            sc = room;
    }
    if (sc > push_ring_avail(dec))
        return 0;
    push_ring_push(dec, dec->frame + sk, sc);
    dec->produced += sc;
    return sc;
}

static int push_check_geometry(mp3dec_push_t *dec, const mp3dec_frame_info_t *info, const uint8_t *hdr)
{
    if (HDR_IS_FREE_FORMAT(hdr))
    {
        push_fail(dec, MP3D_PUSH_E_UNSUPPORTED);
        return -1;
    }
    if (info->layer != 3)
    {
        push_fail(dec, MP3D_PUSH_E_UNSUPPORTED);
        return -1;
    }
    if (!dec->hz)
    {
        dec->hz = info->hz;
        dec->channels = info->channels;
        dec->layer = info->layer;
    } else if (dec->hz != info->hz || dec->channels != info->channels || dec->layer != info->layer)
    {
        push_fail(dec, MP3D_PUSH_E_UNSUPPORTED);
        return -1;
    }
    return 0;
}

/* Decodes one complete frame already fully present in dec->in. Consumes
   frame_size bytes. Handles the first-frame Xing/Info tag. The caller
   guarantees the PCM ring is empty, so a decoded frame always fits.
   Returns 0 on progress, -1 on a sticky unsupported-format error. */
static int push_consume_frame(mp3dec_push_t *dec, int frame_size, int first_frame)
{
    uint8_t *frame = dec->in;
    if (first_frame)
    {
        uint32_t vbr_frames = 0;
        int delay = 0, padding = 0;
        int vr = mp3dec_check_vbrtag(frame, frame_size, &vbr_frames, &delay, &padding);
        if (vr)
        {   /* Xing/Info frame: never audio. vr>0 carries gapless parameters. */
            if (vr > 0)
            {
                int ch = HDR_IS_MONO(frame) ? 1 : 2;
                dec->to_skip = (size_t)delay*ch;
                dec->detected_samples = (uint64_t)hdr_frame_samples(frame)*ch*vbr_frames;
                if (dec->detected_samples >= dec->to_skip)
                    dec->detected_samples -= dec->to_skip;
                size_t pad_scalars = (size_t)padding*ch;
                if (pad_scalars > 0 && dec->detected_samples >= pad_scalars)
                    dec->detected_samples -= pad_scalars;
                dec->channels = ch; /* stereo alignment for later frames */
            }
            memmove(dec->in, frame + frame_size, dec->in_fill - frame_size);
            dec->in_fill -= frame_size;
            dec->phase = PUSH_PHASE_STREAM;
            return 0;
        }
    }

    mp3dec_frame_info_t info;
    memset(&info, 0, sizeof(info));
    int samples = mp3dec_decode_frame(&dec->mp3d, frame, frame_size, dec->frame, &info);
    /* Mirror mp3dec_load_buf: a frame that yields no samples (e.g. corrupt
       side info that made the core reset) must not be used to judge stream
       geometry, so such silent frames are simply consumed. */
    if (samples > 0 && push_check_geometry(dec, &info, frame))
        return -1;
    memmove(dec->in, frame + frame_size, dec->in_fill - frame_size);
    dec->in_fill -= frame_size;
    dec->phase = PUSH_PHASE_STREAM;
    if (samples > 0)
        push_trim_and_ring(dec, samples, info.channels); /* ring is empty: always fits */
    return 0;
}

/* Makes one unit of progress on the compressed input.
   Returns 1 when something was consumed, 0 when more input is required,
   -1 on a sticky error. */
static int push_process(mp3dec_push_t *dec)
{
    if (dec->phase == PUSH_PHASE_ID3V2)
    {
        size_t take = dec->in_fill < dec->id3_remaining ? dec->in_fill : dec->id3_remaining;
        if (!take)
            return 0;
        memmove(dec->in, dec->in + take, dec->in_fill - take);
        dec->in_fill -= take;
        dec->id3_remaining -= take;
        if (!dec->id3_remaining)
            dec->phase = PUSH_PHASE_START;
        return 1;
    }

    if (dec->phase == PUSH_PHASE_START)
    {
        size_t total;
        int id3 = push_id3v2_span(dec->in, dec->in_fill, &total);
        if (id3 > 0)
        {
            dec->id3_remaining = total;
            dec->phase = PUSH_PHASE_ID3V2;
            return push_process(dec);
        }
        if (id3 == 0 && dec->in_fill < 10)
            return 0; /* header not fully received yet */

        if (dec->in_fill < 4)
            return 0;
        int free_format_bytes = 0, frame_size = 0;
        int off = mp3d_find_frame(dec->in, (int)dec->in_fill, &free_format_bytes, &frame_size);
        if (!frame_size)
        {
            /* No locatable frame yet. A free-format header at the head can
               never resolve inside the fixed staging buffer, reject it
               deterministically instead of stalling until truncation. */
            if (hdr_valid(dec->in) && (dec->in[2] & 0xF0) == 0)
            {
                push_fail(dec, MP3D_PUSH_E_UNSUPPORTED);
                return -1;
            }
            /* End-of-input fallback: one complete, non-free Layer III frame at
               the head is accepted even when no following frame confirms the
               sync; any residual bytes are classified by the finish logic. */
            if (dec->eof && hdr_valid(dec->in) && HDR_GET_LAYER(dec->in) == 1)
            {
                int single = hdr_frame_bytes(dec->in, 0) + hdr_padding(dec->in);
                if (single > HDR_SIZE && (size_t)single <= dec->in_fill)
                    return push_consume_frame(dec, single, 1) < 0 ? -1 : 1;
            }
            return 0; /* need more bytes for confident sync / end rule */
        }
        if (free_format_bytes || (dec->in[off + 2] & 0xF0) == 0)
        {
            push_fail(dec, MP3D_PUSH_E_UNSUPPORTED);
            return -1;
        }
        if (off)
        {   /* discard leading junk, mirroring mp3dec_load_buf scanning */
            memmove(dec->in, dec->in + off, dec->in_fill - off);
            dec->in_fill -= off;
            return 1;
        }
        if ((size_t)frame_size > dec->in_fill)
            return 0; /* complete frame not received yet */
        return push_consume_frame(dec, frame_size, 1) < 0 ? -1 : 1;
    }

    /* PUSH_PHASE_STREAM: frame geometry is known, walk frame by frame. */
    if (dec->in_fill < HDR_SIZE)
        return 0;
    if (!hdr_valid(dec->in))
        return 0; /* trailer tag or garbage: end-of-input logic strips/classifies it */
    if (HDR_IS_FREE_FORMAT(dec->in))
    {
        push_fail(dec, MP3D_PUSH_E_UNSUPPORTED);
        return -1;
    }
    int frame_size = hdr_frame_bytes(dec->in, 0) + hdr_padding(dec->in);
    if (frame_size <= HDR_SIZE || frame_size > PUSH_IN_CAP)
        return 0; /* cannot be a supported frame: classified at end of input */
    if ((size_t)frame_size > dec->in_fill)
        return 0; /* wait for the rest of this frame */
    return push_consume_frame(dec, frame_size, 0) < 0 ? -1 : 1;
}

/* Final classification once eof is set and no frame can be progressed. */
static int push_finish(mp3dec_push_t *dec)
{
    if (dec->phase == PUSH_PHASE_DONE)
        return MP3D_PUSH_DONE;
    if (dec->pcm_fill)
        return MP3D_PUSH_OK; /* drain remaining PCM first */

    if (dec->in_fill)
    {
        size_t stripped = push_strip_tail_tags(dec);
        if (stripped)
            return MP3D_PUSH_OK; /* tags removed, re-run processing */
        /* unfinished ID3v2 header/body or a residual partial frame */
        push_fail(dec, MP3D_PUSH_E_TRUNCATED);
        return MP3D_PUSH_E_TRUNCATED;
    }
    if (dec->phase == PUSH_PHASE_ID3V2 && dec->id3_remaining)
    {
        push_fail(dec, MP3D_PUSH_E_TRUNCATED);
        return MP3D_PUSH_E_TRUNCATED;
    }
    dec->phase = PUSH_PHASE_DONE;
    return MP3D_PUSH_DONE;
}

int mp3dec_push_decode(mp3dec_push_t *dec, const uint8_t *data, size_t *bytes,
                       mp3d_sample_t *pcm, size_t *samples, int eof)
{
    if (!dec || !bytes || !samples)
        return MP3D_PUSH_E_PARAM;
    if (*bytes && !data)
        return MP3D_PUSH_E_PARAM;
    if (*samples && !pcm)
        return MP3D_PUSH_E_PARAM;
    if (dec->last_error)
    {
        *bytes = 0;
        *samples = 0;
        return dec->last_error;
    }

    size_t in_left = *bytes;
    size_t out_cap = *samples;
    *bytes = 0;
    *samples = 0;
    if (eof)
        dec->eof = 1;

    for (;;)
    {
        /* deliver everything that fits (whole L/R frames for stereo) */
        if (out_cap)
        {
            size_t n = push_ring_drain(dec, pcm, out_cap);
            if (n)
            {
                pcm += n;
                out_cap -= n;
                *samples += n;
            }
        }
        if (dec->phase == PUSH_PHASE_DONE && !dec->pcm_fill)
            return MP3D_PUSH_DONE;

        /* A decoded frame fills the whole ring; wait until it is drained so
           no frame is ever decoded on top of undelivered PCM. */
        if (dec->pcm_fill)
            return MP3D_PUSH_OK;
        if (!out_cap && (*samples > 0))
            return MP3D_PUSH_OK; /* output buffer full, come back after emptying it */

        /* accept as much compressed input as the staging buffer holds */
        if (in_left)
        {
            size_t room = PUSH_IN_CAP - dec->in_fill;
            if (room)
            {
                size_t take = in_left < room ? in_left : room;
                memcpy(dec->in + dec->in_fill, data, take);
                dec->in_fill += take;
                data += take;
                in_left -= take;
                *bytes += take;
            }
        }

        int pr = push_process(dec);
        if (pr < 0)
            return dec->last_error; /* PCM already drained above is kept */

        if (!pr)
        {   /* buffered data cannot progress without more bytes */
            if (dec->eof)
            {
                int rc = push_finish(dec);
                if (rc == MP3D_PUSH_OK)
                    continue; /* tail tags were stripped, retry */
                return rc;
            }
            /* No room for more input and nothing progressed: the caller must
               first drain PCM (already handled above) or provide more bytes. */
            return MP3D_PUSH_OK;
        }
    }
}
