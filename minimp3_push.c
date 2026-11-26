/*
    Push (streaming, forward-only, non-seekable) MP3 decoding for minimp3.
    https://github.com/lieff/minimp3
    To the extent possible under law, the author(s) have dedicated all copyright and related and neighboring rights to this software to the public domain worldwide.
    This software is distributed without any warranty.
    See <http://creativecommons.org/publicdomain/zero/1.0/>.
*/
#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"
#include "minimp3_push.h"

#include <string.h>

#define PUSH_PHASE_ID3_HDR  0  /* collecting the 10 byte ID3v2 header */
#define PUSH_PHASE_ID3_BODY 1  /* discarding the ID3v2 tag body */
#define PUSH_PHASE_AUDIO    2  /* locating and decoding audio frames */

int mp3dec_push_init(mp3dec_push_t *dec)
{
    if (!dec)
        return MP3D_PUSH_E_PARAM;
    memset(dec, 0, sizeof(*dec));
    mp3dec_init(&dec->mp3d);
    return MP3D_PUSH_OK;
}

void mp3dec_push_close(mp3dec_push_t *dec)
{
    if (dec)
        memset(dec, 0, sizeof(*dec));
}

/* hand as many pending PCM scalars as fit; stereo keeps whole L/R pairs */
static size_t push_drain_pcm(mp3dec_push_t *dec, mp3d_sample_t *output, size_t cap)
{
    size_t avail = dec->pcm_fill - dec->pcm_used;
    size_t n = cap < avail ? cap : avail;
    if (dec->channels == 2)
        n &= ~(size_t)1;
    if (n)
    {
        memcpy(output, dec->pcm + dec->pcm_used, n*sizeof(mp3d_sample_t));
        dec->pcm_used += n;
        if (dec->pcm_used == dec->pcm_fill)
            dec->pcm_fill = dec->pcm_used = 0;
    }
    return n;
}

static void push_discard_front(mp3dec_push_t *dec, size_t n)
{
    if (!n)
        return;
    dec->input_fill -= n;
    memmove(dec->input, dec->input + n, dec->input_fill);
}

/* mirror of mp3dec_skip_id3v1(), working on the buffered tail at eof */
static void push_strip_tail_tags(mp3dec_push_t *dec)
{
    size_t size = dec->input_fill;
    if (size >= 128 && !memcmp(dec->input + size - 128, "TAG", 3))
    {
        size -= 128;
        if (size >= 227 && !memcmp(dec->input + size - 227, "TAG+", 4))
            size -= 227;
    }
    if (size > 32 && !memcmp(dec->input + size - 32, "APETAGEX", 8))
    {
        const uint8_t *tag;
        uint32_t tag_size;
        size -= 32;
        tag = dec->input + size + 8 + 4;
        tag_size = (uint32_t)(tag[3] << 24) | (tag[2] << 16) | (tag[1] << 8) | tag[0];
        if (size >= tag_size)
            size -= tag_size;
    }
    dec->input_fill = size;
}

/*
   eof leftovers. A residual that still begins with (or contains) a frame header
   matching the locked stream is a cut audio frame and is reported as truncation;
   bytes that never form such a header are unconfirmable junk and are discarded,
   matching mp3dec_load_buf() which silently skips non-frame tail data.
*/
static int push_has_frame_sync(const mp3dec_push_t *dec)
{
    size_t i;
    for (i = 0; i + HDR_SIZE <= dec->input_fill; i++)
    {
        const uint8_t *h = dec->input + i;
        if (!hdr_valid(h))
            continue;
        if (!dec->format_locked)
            return 1;
        if ((4 - HDR_GET_LAYER(h)) == dec->layer &&
            hdr_sample_rate_hz(h) == (unsigned)dec->hz &&
            (HDR_IS_MONO(h) ? 1 : 2) == dec->channels &&
            !HDR_IS_FREE_FORMAT(h))
            return 1;
    }
    return 0;
}

/*
   Walk consecutive frames from a candidate header and count how many keep
   agreeing via hdr_compare(). A short buffer of main data can accidentally
   satisfy a single exact-size frame and even one successor header; a chain of
   corroborated frames is what reliably distinguishes a real stream.
*/
#define PUSH_MIN_SYNC_MATCHES 4
static int push_frame_chain_ok(const uint8_t *buf, size_t fill, size_t off, int frame_size)
{
    int matches = 1;
    size_t pos = off;
    if (frame_size <= 0)
        return 0;
    while (matches < PUSH_MIN_SYNC_MATCHES)
    {
        int next_size;
        size_t next = pos + (size_t)frame_size;
        if (next + HDR_SIZE > fill)
            return 0;
        if (!hdr_valid(buf + next) || !hdr_compare(buf + pos, buf + next))
            return 0;
        next_size = hdr_frame_bytes(buf + next, 0) + hdr_padding(buf + next);
        if (next_size <= 0)
            next_size = frame_size; /* free-format: constant frame size */
        frame_size = next_size;
        pos = next;
        matches++;
    }
    return 1;
}

int mp3dec_push_decode(mp3dec_push_t *dec, const uint8_t *input, size_t *bytes,
                       mp3d_sample_t *output, size_t *samples, int eof)
{
    size_t in_cap, out_cap, in_used = 0, out_used = 0;

    if (!dec || !bytes || !samples || (output == NULL && *samples != 0) ||
        (input == NULL && *bytes != 0))
    {
        if (bytes)
            *bytes = 0;
        if (samples)
            *samples = 0;
        return MP3D_PUSH_E_PARAM;
    }
    if (dec->error)
    {
        *bytes = *samples = 0;
        return dec->error;
    }
    if (dec->finished)
    {
        *bytes = *samples = 0;
        return MP3D_PUSH_DONE;
    }

    in_cap = *bytes;
    out_cap = *samples;
    *bytes = *samples = 0;
    if (eof)
        dec->eof_seen = 1;

    for (;;)
    {
        size_t frag_left, take, n;
        int frame_size = 0, free_format_bytes = 0;

        /* first hand over any sound already produced */
        if (out_used < out_cap)
            out_used += push_drain_pcm(dec, output ? output + out_used : NULL, out_cap - out_used);
        if (dec->pcm_fill)
        {
            /* undelivered PCM must survive; still accept any input offered so
               the caller can keep advancing, then wait for drain space */
            frag_left = in_cap - in_used;
            take = MINIMP3_MIN(frag_left, sizeof(dec->input) - dec->input_fill);
            if (take)
            {
                memcpy(dec->input + dec->input_fill, input + in_used, take);
                dec->input_fill += take;
                in_used += take;
            }
            break;
        }

        frag_left = in_cap - in_used;

        if (dec->phase == PUSH_PHASE_ID3_HDR)
        {
            /* buffer at most the fixed 10 byte tag header, never the tag body */
            take = MINIMP3_MIN(frag_left, (size_t)MINIMP3_ID3_DETECT_SIZE - dec->input_fill);
            if (take)
            {
                memcpy(dec->input + dec->input_fill, input + in_used, take);
                dec->input_fill += take;
                in_used += take;
                frag_left -= take;
            }
            if (dec->input_fill < MINIMP3_ID3_DETECT_SIZE)
            {
                if (dec->eof_seen)
                {
                    if (dec->input_fill == 0)
                        break; /* nothing ever arrived */
                    if (!memcmp(dec->input, "ID3", 3))
                    {
                        dec->error = MP3D_PUSH_E_TRUNC; /* unfinished tag header */
                        break;
                    }
                    dec->phase = PUSH_PHASE_AUDIO; /* short non-tag prefix */
                } else
                    break;
            }
            if (dec->input_fill >= MINIMP3_ID3_DETECT_SIZE)
            {
                size_t id3_size = mp3dec_skip_id3v2(dec->input, dec->input_fill);
                if (id3_size)
                {
                    dec->id3_left = id3_size - dec->input_fill; /* header bytes buffered */
                    dec->input_fill = 0;
                    dec->phase = PUSH_PHASE_ID3_BODY;
                } else
                {
                    dec->phase = PUSH_PHASE_AUDIO; /* not an ID3v2 tag */
                }
            }
            continue;
        }

        if (dec->phase == PUSH_PHASE_ID3_BODY)
        {
            /* discard buffered header bytes first (already counted in id3_left) */
            take = MINIMP3_MIN(dec->input_fill, dec->id3_left);
            push_discard_front(dec, take);
            dec->id3_left -= take;
            /* discard the rest straight from the fragment, without caching it */
            take = MINIMP3_MIN(frag_left, dec->id3_left);
            in_used += take;
            dec->id3_left -= take;
            if (dec->id3_left)
            {
                if (dec->eof_seen)
                    dec->error = MP3D_PUSH_E_TRUNC; /* unfinished tag body */
                break;
            }
            dec->phase = PUSH_PHASE_AUDIO;
            continue;
        }

        /* PUSH_PHASE_AUDIO */
        take = MINIMP3_MIN(frag_left, sizeof(dec->input) - dec->input_fill);
        if (take)
        {
            memcpy(dec->input + dec->input_fill, input + in_used, take);
            dec->input_fill += take;
            in_used += take;
        }

        /* strip trailing tags only once the complete tail has arrived */
        if (dec->eof_seen && !dec->id3v1_checked)
        {
            push_strip_tail_tags(dec);
            dec->id3v1_checked = 1;
        }

        if (!dec->input_fill)
            break; /* waiting for more bytes */

        frame_size = 0;
        free_format_bytes = 0;
        n = (size_t)mp3d_find_frame(dec->input, (int)dec->input_fill, &free_format_bytes, &frame_size);

        if (!frame_size)
        {
            if (dec->eof_seen)
            {
                if (push_has_frame_sync(dec) ||
                    (dec->format_locked && dec->input_fill > 0))
                    dec->error = MP3D_PUSH_E_TRUNC; /* incomplete frame / stream bytes */
                else
                    dec->input_fill = 0;            /* unconfirmable junk at the tail */
                break;
            }
            if (dec->input_fill >= sizeof(dec->input))
            {
                dec->error = MP3D_PUSH_E_FORMAT; /* never syncs: unsupported stream */
                break;
            }
            break; /* need more bytes to confirm the next frame */
        }

        /* A confirmed free-format stream (find_frame sets free_format_bytes) is
           unsupported. Only reject the stream-start case at offset 0; a free
           header found mid-buffer is a false sync handled by the chain gate. */
        if (free_format_bytes && n == 0)
        {
            dec->error = MP3D_PUSH_E_FORMAT;
            break;
        }

        /*
           While more data may still arrive, only decode a frame that is safely
           corroborated. mp3d_find_frame() otherwise accepts a buffer that
           exactly equals one computed frame size, which a short buffer of main
           data can satisfy by coincidence; decoding such a false frame would
           poison the bit reservoir and synthesis history.
             * Before the format is locked, a junk prefix may precede the stream,
               so accept the find_frame() candidate only when a frame chain backs
               it (false syncs form only short chains).
             * After lock, frames are consumed one by one so the next genuine
               header always sits at offset 0; require that alignment too.
           At eof the exact-size acceptance is the only way to confirm the final
           frame, so these checks are skipped.
        */
        if (!dec->eof_seen)
        {
            if (!push_frame_chain_ok(dec->input, dec->input_fill, n, frame_size))
                break; /* wait for more corroborating frame headers */
            if (dec->format_locked && n != 0)
                break; /* genuine frames are contiguous after lock */
        }

        /* n bytes of junk/skipped data sit before the confirmed frame */
        push_discard_front(dec, n);

        {
            const uint8_t *hdr = dec->input;
            int layer = 4 - HDR_GET_LAYER(hdr);
            int hz = hdr_sample_rate_hz(hdr);
            int channels = HDR_IS_MONO(hdr) ? 1 : 2;

            /*
               A Xing/Info tag only ever sits on an ordinary Layer III frame.
               False syncs inside main data can briefly look like free-format or
               other layers; those decode to zero samples and are skipped below,
               so the tag check is limited to plausible Layer III candidates.
            */
            if (!dec->vbr_checked && layer == 3 && !HDR_IS_FREE_FORMAT(hdr))
            {
                uint32_t tag_frames = 0;
                int delay = 0, padding = 0, tag_ret;
                dec->vbr_checked = 1;
                tag_ret = mp3dec_check_vbrtag(hdr, frame_size, &tag_frames, &delay, &padding);
                if (tag_ret)
                {
                    /* Xing/Info frame carries no sound and must not touch the decoder */
                    push_discard_front(dec, (size_t)frame_size);
                    if (tag_ret > 0)
                    {
                        int pad_scalars = padding*channels;
                        dec->have_gapless = 1;
                        dec->to_skip = delay*channels;
                        dec->detected_samples =
                            (unsigned long long)hdr_frame_samples(hdr)*channels*tag_frames;
                        if (dec->detected_samples >= (unsigned long long)dec->to_skip)
                            dec->detected_samples -= dec->to_skip;
                        if (pad_scalars > 0 &&
                            dec->detected_samples >= (unsigned long long)pad_scalars)
                            dec->detected_samples -= pad_scalars;
                    }
                    continue;
                }
            }

            {
                mp3dec_frame_info_t info;
                int frame_samples;
                size_t scalar_count;
                memset(&info, 0, sizeof(info));
                /* the frame is complete and confirmed: safe to update reservoir/history */
                frame_samples = mp3dec_decode_frame(&dec->mp3d, dec->input,
                                                    (int)dec->input_fill, dec->pcm, &info);
                push_discard_front(dec, (size_t)info.frame_bytes);
                if (!frame_samples)
                    continue; /* false sync / cold reservoir: skip, no format decision yet */

                /* only a frame that actually produced sound defines the format */
                if (HDR_IS_FREE_FORMAT(hdr) || layer != 3)
                {
                    dec->error = MP3D_PUSH_E_FORMAT;
                    break;
                }
                if (dec->format_locked && (dec->hz != hz || dec->channels != channels))
                {
                    dec->error = MP3D_PUSH_E_FORMAT;
                    break;
                }
                if (!dec->format_locked)
                {
                    dec->format_locked = 1;
                    dec->hz = hz;
                    dec->channels = channels;
                    dec->layer = layer;
                }

                scalar_count = (size_t)frame_samples*info.channels;

                /* leading gapless trim */
                if (dec->to_skip > 0)
                {
                    size_t skip = MINIMP3_MIN(scalar_count, (size_t)dec->to_skip);
                    memmove(dec->pcm, dec->pcm + skip, (scalar_count - skip)*sizeof(mp3d_sample_t));
                    dec->to_skip -= (int)skip;
                    scalar_count -= skip;
                }
                /* trailing gapless trim */
                if (dec->have_gapless &&
                    dec->produced_samples + scalar_count > dec->detected_samples)
                {
                    if (dec->detected_samples > dec->produced_samples)
                        scalar_count = (size_t)(dec->detected_samples - dec->produced_samples);
                    else
                        scalar_count = 0;
                }

                dec->pcm_fill = scalar_count;
                dec->pcm_used = 0;
                dec->produced_samples += scalar_count;
            }
        }
    }

    /* report everything accepted or delivered in this call, even on error */
    *bytes = in_used;
    *samples = out_used;

    if (dec->error)
        return dec->error;
    if (dec->eof_seen && !dec->input_fill && !dec->pcm_fill)
    {
        dec->finished = 1;
        return MP3D_PUSH_DONE;
    }
    return MP3D_PUSH_OK;
}
