/**
 * SDL_sound; An abstract sound format decoding API.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 * Finite, native-frame loop region support. This header is self-contained:
 * the build can force-include it into every translation unit, so it only
 * depends on the public SDL_sound API, never on the internal header.
 */

#ifndef SDL_SOUND_LOOP_H_
#define SDL_SOUND_LOOP_H_

#include <SDL3/SDL.h>
#include <SDL3_sound/SDL_sound.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * All times in this structure are complete sample frames in the sample's
 * native (actual) format. The expanded logical timeline looks like:
 *
 *   [0, end), then `repeats` extra copies of [start, end), then [end, N).
 *
 * Only the region bounds and the repeat count are remembered; the expanded
 * waveform is never materialized.
 */
typedef struct Sound_LoopState
{
    bool configured;        /* a loop configuration is active */
    bool finished;          /* the logical end was reached */

    Uint64 source_frames;   /* N, total frames of the original material */
    Uint64 start;           /* first frame of the loop region */
    Uint64 end;             /* one past the last frame of the region */
    Uint32 repeats;         /* configured additional repeats */
    Uint64 loop_len;        /* L = end - start */
    Uint64 repeat_span;     /* repeats * L, in logical frames */
    Uint64 total;           /* logical length, N + repeats * L */

    Uint64 cursor;          /* logical frame the next decoder output starts at */
    Uint64 decoder_frame;   /* source frame the underlying decoder sits on */

    /* delivery clock for the current playback generation */
    Uint64 origin;          /* q0: logical frame the generation started at */
    Uint64 clock_pos;       /* native frames accounted from delivered output */
    Uint64 clock_rem;       /* integer remainder of the clock division */
    Uint64 delivered;       /* desired-format frames delivered this generation */
} Sound_LoopState;


/* life cycle ... */

void Sound_LoopInitState(Sound_LoopState *loop);

/* Validate a region against a source of N frames and, on success, install it
   without touching the decoder. Returns nonzero on success, zero after setting
   an SDL_sound error; on failure the previous state is left untouched. */
int Sound_LoopConfigure(Sound_LoopState *loop, Uint64 source_frames,
                        Uint64 start, Uint64 end, Uint32 additional_repeats);

/* Reset every playback position for a decode generation beginning at logical
   frame `origin`, discarding the old delivery clock. */
void Sound_LoopBeginGeneration(Sound_LoopState *loop, Uint64 origin);


/* timeline math ... */

/* Map a logical frame to the matching frame in the original source. */
Uint64 Sound_LoopMapSource(const Sound_LoopState *loop, Uint64 logical_frame);

/* Extra loop passes still ahead when sitting on logical frame q. */
Uint32 Sound_LoopRemainingRepeats(const Sound_LoopState *loop, Uint64 q);

/* Account for frames handed to the application in the desired format. The
   conversion to native frame units uses an integer quotient/remainder, so it
   cannot drift or overflow across a long playback. */
void Sound_LoopClockAdvance(Sound_Sample *sample, Uint32 desired_frames);


/* decoding ... */

/* Decoder-facing read that implements the loop jumps. Stands in for the
   decoder's read() method while a loop is configured; frame-exact, and never
   lets samples past a region boundary reach the audio stream. */
Uint32 Sound_LoopRead(Sound_Sample *sample);


/* WAV backend hooks (implemented in SDL_sound_wav.c) ... */

/* Nonzero only for seekable mono/stereo 16-bit PCM or Microsoft ADPCM WAVs. */
int Sound_WAVLoopSupports(Sound_Sample *sample);

/* Exact frame count the backend can decode. */
Uint64 Sound_WAVLoopTotalFrames(Sound_Sample *sample);

/* Reposition the real WAV decoder on an exact source frame, including frames
   in the middle of an ADPCM block. Returns nonzero on success. */
int Sound_WAVLoopSeekFrame(Sound_Sample *sample, Uint64 source_frame);

#ifdef __cplusplus
}
#endif

#endif /* SDL_SOUND_LOOP_H_ */

/* end of SDL_sound_loop.h ... */
