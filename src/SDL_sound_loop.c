/**
 * SDL_sound; An abstract sound format decoding API.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 * Finite, native-frame loop regions: logical timeline, delivery clock and
 * the frame-exact decode wrapper that performs the loop jumps.
 */

#define __SDL_SOUND_INTERNAL__
#include "SDL_sound_internal.h"
#include "SDL_sound_loop.h"

/* Error strings used by the loop layer. */
#define ERR_LOOP_BAD_REGION    "Loop region is invalid for this sample"
#define ERR_LOOP_OVERFLOW      "Loop length exceeds the frame counter range"
#define ERR_LOOP_UNSUPPORTED   "Looping requires a seekable mono/stereo 16-bit PCM or Microsoft ADPCM WAV"
#define ERR_LOOP_NOT_CONFIGURED "No loop region configured"
#define ERR_LOOP_ACTIVE         "Loop region cannot be changed after decoding started"
#define ERR_LOOP_RANGE          "Loop seek position is past the logical end"


void Sound_LoopInitState(Sound_LoopState *loop)
{
    SDL_zerop(loop);
} /* Sound_LoopInitState */


int Sound_LoopConfigure(Sound_LoopState *loop, Uint64 source_frames,
                        Uint64 start, Uint64 end, Uint32 additional_repeats)
{
    Uint64 region_len, span, total;

    if ((source_frames == 0) || (start >= end) || (end > source_frames))
        BAIL_MACRO(ERR_LOOP_BAD_REGION, 0);

    region_len = end - start;
    if ((additional_repeats != 0) &&
        (region_len > (Uint64) SDL_MAX_UINT64 / (Uint64) additional_repeats))
        BAIL_MACRO(ERR_LOOP_OVERFLOW, 0);

    span = region_len * (Uint64) additional_repeats;
    if (span > (Uint64) SDL_MAX_UINT64 - source_frames)
        BAIL_MACRO(ERR_LOOP_OVERFLOW, 0);

    total = source_frames + span;

    loop->source_frames = source_frames;
    loop->start = start;
    loop->end = end;
    loop->repeats = additional_repeats;
    loop->loop_len = region_len;
    loop->repeat_span = span;
    loop->total = total;
    loop->configured = true;
    loop->finished = false;
    Sound_LoopBeginGeneration(loop, 0);
    return 1;
} /* Sound_LoopConfigure */


void Sound_LoopBeginGeneration(Sound_LoopState *loop, Uint64 origin)
{
    loop->origin = origin;
    loop->cursor = origin;
    loop->decoder_frame = Sound_LoopMapSource(loop, origin);
    loop->clock_pos = 0;
    loop->clock_rem = 0;
    loop->delivered = 0;
    loop->finished = false;
} /* Sound_LoopBeginGeneration */


Uint64 Sound_LoopMapSource(const Sound_LoopState *loop, Uint64 q)
{
    if (q < loop->end)
        return q;

    if (q < loop->end + loop->repeat_span)
    {
        const Uint64 into = q - loop->end;
        return loop->start + (into % loop->loop_len);
    }

    /* Tail region; the logical end itself maps onto the source end N. */
    return q - loop->repeat_span;
} /* Sound_LoopMapSource */


Uint32 Sound_LoopRemainingRepeats(const Sound_LoopState *loop, Uint64 q)
{
    if (q < loop->end)
        return loop->repeats;

    if (q < loop->end + loop->repeat_span)
    {
        const Uint64 completed = (q - loop->end) / loop->loop_len;
        return loop->repeats - (Uint32) completed;
    }

    return 0;
} /* Sound_LoopRemainingRepeats */


void Sound_LoopClockAdvance(Sound_Sample *sample, Uint32 desired_frames)
{
    Sound_SampleInternal *internal = (Sound_SampleInternal *) sample->opaque;
    Sound_LoopState *loop = internal->loop;

    if ((loop == NULL) || !loop->configured || desired_frames == 0)
        return;

    /* q = min(total, q0 + floor(m * actual.freq / desired.freq)), evaluated
       with an exact integer quotient/remainder. The carried remainder keeps
       the clock exact over arbitrarily long playback without per-block
       rounding, and the running sum cannot overflow the Uint64 timeline. */
    {
        const Uint64 denom = (Uint64) sample->desired.freq;
        const Uint64 num = (Uint64) desired_frames * (Uint64) sample->actual.freq;
        Uint64 add = num / denom;
        const Uint64 rem = num % denom;

        loop->clock_rem += rem;
        if (loop->clock_rem >= denom)
        {
            loop->clock_rem -= denom;
            add++;
        }

        loop->delivered += desired_frames;
        loop->clock_pos += add;

        if ((loop->origin + loop->clock_pos) > loop->total)
            loop->clock_pos = loop->total - loop->origin;
    }
} /* Sound_LoopClockAdvance */


#if SOUND_SUPPORTS_WAV

/*
 * Frame-exact read wrapper standing in for the WAV decoder's read() method.
 * The underlying decoder only ever sees ordinary source material: a loop
 * jump is a real backend seek, so no sample past a region end is fed to the
 * SDL_AudioStream, while the stream itself is never flushed, cleared or
 * recreated on a natural jump, keeping resampling/filter history continuous.
 */
Uint32 Sound_LoopRead(Sound_Sample *sample)
{
    Sound_SampleInternal *internal = (Sound_SampleInternal *) sample->opaque;
    Sound_LoopState *loop = internal->loop;
    const Uint32 frame_size = (Uint32) SDL_AUDIO_FRAMESIZE(sample->actual);
    const Uint32 real_buffer_size = internal->buffer_size;
    void *const real_buffer = internal->buffer;
    Uint32 bw = 0;

    if (loop->finished)
    {
        sample->flags |= SOUND_SAMPLEFLAG_EOF;
        return 0;
    }

    while (bw < real_buffer_size)
    {
        Uint64 source_pos, logical_limit, limit, frames, want_bytes;
        Uint32 got, got_frames;

        source_pos = Sound_LoopMapSource(loop, loop->cursor);

        /* Nearest cut point ahead: the next region end while repeats
           remain, otherwise the logical end of the whole expanded stream. */
        if (loop->cursor < loop->end)
            logical_limit = (loop->repeats > 0) ? loop->end : loop->total;
        else if (loop->cursor < loop->end + loop->repeat_span)
        {
            const Uint64 into = loop->cursor - loop->end;
            const Uint64 block = into / loop->loop_len;
            logical_limit = loop->end + (block + 1) * loop->loop_len;
        }
        else
            logical_limit = loop->total;

        if (loop->cursor >= logical_limit)
        {
            if (logical_limit >= loop->total)
            {
                loop->finished = true;
                sample->flags |= SOUND_SAMPLEFLAG_EOF;
                return bw;
            }

            /* Natural loop jump. The audio stream keeps running untouched. */
            if (!Sound_WAVLoopSeekFrame(sample, loop->start))
            {
                sample->flags |= SOUND_SAMPLEFLAG_ERROR;
                return bw;
            }
            loop->decoder_frame = loop->start;
            continue;
        }

        limit = logical_limit - loop->cursor;
        frames = ((Uint64) real_buffer_size - bw) / frame_size;
        if (frames > limit)
            frames = limit;
        if (frames == 0)
            return bw;

        /* Stay aligned with the source frame the cursor requires; a straight
           continuation needs no seek at all. */
        if (loop->decoder_frame != source_pos)
        {
            if (!Sound_WAVLoopSeekFrame(sample, source_pos))
            {
                sample->flags |= SOUND_SAMPLEFLAG_ERROR;
                return bw;
            }
            loop->decoder_frame = source_pos;
        }

        /* Decoders always write from the start of internal->buffer, so aim
           each segment at its own slice and cap it to the remaining cut. */
        want_bytes = frames * frame_size;
        internal->buffer = (Uint8 *) real_buffer + bw;
        internal->buffer_size = (Uint32) want_bytes;
        sample->flags &= ~(SOUND_SAMPLEFLAG_EOF | SOUND_SAMPLEFLAG_ERROR);
        got = internal->funcs->read(sample);
        internal->buffer = real_buffer;
        internal->buffer_size = real_buffer_size;

        if (got == 0)
        {
            if (sample->flags & SOUND_SAMPLEFLAG_ERROR)
                return bw;

            /* Backend EOF with nothing decoded: legitimate only at the end of
               the whole logical timeline; otherwise the source is shorter
               than the configured region. */
            if ((sample->flags & SOUND_SAMPLEFLAG_EOF) &&
                (loop->cursor < loop->total))
            {
                sample->flags |= SOUND_SAMPLEFLAG_ERROR;
                return bw;
            }

            loop->finished = true;
            sample->flags |= SOUND_SAMPLEFLAG_EOF;
            return bw;
        }

        got_frames = got / frame_size;
        bw += got;
        loop->cursor += got_frames;
        loop->decoder_frame += got_frames;

        if (sample->flags & SOUND_SAMPLEFLAG_ERROR)
            return bw;

        if (sample->flags & SOUND_SAMPLEFLAG_EOF)
        {
            /* Physical end of the source reached. That is only legal at the
               final logical cut; at an earlier cut the next iteration seeks
               the decoder back into the region, so the flag must not leak to
               the stream layer (which would treat it as the end of input). */
            if (loop->cursor >= loop->total)
            {
                loop->finished = true;
                return bw;
            }
            if (loop->cursor >= logical_limit)
            {
                sample->flags &= ~SOUND_SAMPLEFLAG_EOF;
                continue;  /* top of the loop performs the jump */
            }
            sample->flags |= SOUND_SAMPLEFLAG_ERROR;
            return bw;
        }

        if (got < want_bytes)
            return bw;  /* short read (EAGAIN): hand back what we have */
    }

    return bw;
} /* Sound_LoopRead */

#else  /* !SOUND_SUPPORTS_WAV */

/* Without the WAV decoder the public API still links, but every attempt to
   configure a loop is rejected through Sound_WAVLoopSupports() == 0. */

Uint32 Sound_LoopRead(Sound_Sample *sample)
{
    Sound_SampleInternal *internal = (Sound_SampleInternal *) sample->opaque;
    return internal->funcs->read(sample);
} /* Sound_LoopRead */

int Sound_WAVLoopSupports(Sound_Sample *sample)
{
    (void) sample;
    return 0;
} /* Sound_WAVLoopSupports */

Uint64 Sound_WAVLoopTotalFrames(Sound_Sample *sample)
{
    (void) sample;
    return 0;
} /* Sound_WAVLoopTotalFrames */

int Sound_WAVLoopSeekFrame(Sound_Sample *sample, Uint64 source_frame)
{
    (void) sample;
    (void) source_frame;
    return 0;
} /* Sound_WAVLoopSeekFrame */

#endif /* SOUND_SUPPORTS_WAV */

/* end of SDL_sound_loop.c ... */
