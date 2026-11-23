/*
  SDL_mixer:  An audio mixer library based on the SDL library
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

// Recording support: a device-independent tap on a mixer's final output,
// with a rolling preroll history, so apps can offer "record what just
// played". The mixing thread only ever appends to a preallocated ring; the
// reader thread drains it without taking the mixer lock.

#include "SDL_mixer_internal.h"

struct MIX_Recorder
{
    MIX_Mixer *mixer;
    int channels;          // copied from mixer->spec at creation; recorded data is interleaved float32 in this channel order.
    int preroll_frames;    // frames of rolling history kept at all times. May be zero.
    int capacity_frames;   // total queue capacity in frames, preroll included.
    float *preroll;        // preroll_frames * channels floats; absolute frame F lives at slot (F % preroll_frames).
    float *queue;          // capacity_frames * channels floats; single-producer/single-consumer ring.
    Uint64 *queue_frames;  // absolute frame number of the PCM in each queue slot.
    SDL_AtomicU32 queue_head;  // total frames consumed. Written by the reader thread, read by the mixing thread.
    SDL_AtomicU32 queue_tail;  // total frames stored. Written by the mixing thread, read by the reader thread.
    Uint64 frames_seen;    // all output frames mixed since creation, silence included. Only touched with the mixer locked.
    Uint64 start_frame;    // absolute number of the first frame of the current/last recording, preroll included.
    Uint64 end_frame;      // absolute number one past the last frame the current/last recording has seen.
    Uint64 dropped_frames; // frames dropped because the queue was full during the current/last recording.
    bool active;           // true while recording.
};

// The mixer lock is the audio stream lock; holding it serializes against
// MixerCallback (and thus MIX_RecorderMix) on any thread.
static void LockRecorder(MIX_Recorder *recorder)
{
    SDL_LockAudioStream(recorder->mixer->output_stream);
}

static void UnlockRecorder(MIX_Recorder *recorder)
{
    SDL_UnlockAudioStream(recorder->mixer->output_stream);
}

// Copy `frames` frames of interleaved PCM into a ring, starting at absolute
// frame `first_frame`. The ring maps absolute frame F to slot (F % ring_frames).
static void WriteFrameRing(float *ring, int ring_frames, int channels, Uint64 first_frame, const float *src, int frames)
{
    int done = 0;
    while (done < frames) {
        const Uint64 slot = (first_frame + (Uint64) done) % (Uint64) ring_frames;
        Uint64 chunk = (Uint64) ring_frames - slot;
        if (chunk > (Uint64) (frames - done)) {
            chunk = (Uint64) (frames - done);
        }
        SDL_memcpy(ring + ((size_t) slot * (size_t) channels), src + ((size_t) done * (size_t) channels), (size_t) chunk * (size_t) channels * sizeof (float));
        done += (int) chunk;
    }
}

// Called from MixerCallback with the mixer locked, after the app's postmix
// callback and before the data goes to the output stream. This runs on the
// mixing thread: it must not allocate, do I/O, or wait on the reader.
void MIX_RecorderMix(MIX_Recorder *recorder, const float *pcm, int frames)
{
    if (frames <= 0) {
        return;
    }

    const int channels = recorder->channels;
    const Uint64 first = recorder->frames_seen;

    // The rolling preroll is always kept up to date, recording or not.
    if (recorder->preroll_frames > 0) {
        WriteFrameRing(recorder->preroll, recorder->preroll_frames, channels, first, pcm, frames);
    }

    if (recorder->active) {
        const Uint32 capacity = (Uint32) recorder->capacity_frames;
        const Uint32 head = SDL_GetAtomicU32(&recorder->queue_head);
        Uint32 tail = SDL_GetAtomicU32(&recorder->queue_tail);
        Uint32 space = capacity - (tail - head);
        Uint64 dropped = 0;
        Uint64 frame = first;
        const float *src = pcm;

        for (int i = 0; i < frames; i++, frame++, src += channels) {
            if (space > 0) {
                // Never touches slots the reader hasn't released yet.
                const Uint32 slot = tail % capacity;
                SDL_memcpy(recorder->queue + ((size_t) slot * (size_t) channels), src, (size_t) channels * sizeof (float));
                recorder->queue_frames[slot] = frame;
                tail++;
                space--;
            } else {
                dropped++;  // queue is full: drop new frames, never overwrite unread data, never pause the mix.
            }
        }

        if (dropped > 0) {
            recorder->dropped_frames += dropped;
        }
        SDL_SetAtomicU32(&recorder->queue_tail, tail);  // publishes the new frames to the reader.
        recorder->end_frame = first + (Uint64) frames;
    }

    recorder->frames_seen = first + (Uint64) frames;
}

MIX_Recorder *MIX_CreateRecorder(MIX_Mixer *mixer, int preroll_frames, int capacity_frames)
{
    if (!mixer) {
        SDL_InvalidParamError("mixer");
        return NULL;
    } else if (preroll_frames < 0) {
        SDL_InvalidParamError("preroll_frames");
        return NULL;
    } else if (capacity_frames <= preroll_frames) {
        SDL_InvalidParamError("capacity_frames");
        return NULL;
    }

    const int channels = mixer->spec.channels;

    // all frame counts are ints, but the byte sizes still have to fit in a size_t.
    if (((Uint64) capacity_frames * (Uint64) channels) > ((Uint64) SDL_SIZE_MAX / sizeof (float))) {
        SDL_SetError("Recorder capacity is too large");
        return NULL;
    } else if ((Uint64) capacity_frames > ((Uint64) SDL_SIZE_MAX / sizeof (Uint64))) {
        SDL_SetError("Recorder capacity is too large");
        return NULL;
    }

    MIX_Recorder *recorder = (MIX_Recorder *) SDL_calloc(1, sizeof (*recorder));
    if (!recorder) {
        return NULL;
    }

    // everything the recorder will ever need is allocated up front.
    recorder->queue = (float *) SDL_calloc((size_t) capacity_frames, (size_t) channels * sizeof (float));
    recorder->queue_frames = (Uint64 *) SDL_calloc((size_t) capacity_frames, sizeof (Uint64));
    recorder->preroll = (preroll_frames > 0) ? (float *) SDL_calloc((size_t) preroll_frames, (size_t) channels * sizeof (float)) : NULL;
    if (!recorder->queue || !recorder->queue_frames || ((preroll_frames > 0) && !recorder->preroll)) {
        SDL_free(recorder->preroll);
        SDL_free(recorder->queue_frames);
        SDL_free(recorder->queue);
        SDL_free(recorder);
        return NULL;
    }

    recorder->mixer = mixer;
    recorder->channels = channels;
    recorder->preroll_frames = preroll_frames;
    recorder->capacity_frames = capacity_frames;

    LockRecorder(recorder);
    if (mixer->recorder) {  // at most one recorder per mixer.
        UnlockRecorder(recorder);
        SDL_free(recorder->preroll);
        SDL_free(recorder->queue_frames);
        SDL_free(recorder->queue);
        SDL_free(recorder);
        SDL_SetError("Mixer already has a recorder attached");
        return NULL;
    }
    mixer->recorder = recorder;
    UnlockRecorder(recorder);

    return recorder;
}

bool MIX_StartRecorder(MIX_Recorder *recorder)
{
    if (!recorder) {
        return SDL_InvalidParamError("recorder");
    }

    bool retval = false;
    LockRecorder(recorder);

    const Uint32 head = SDL_GetAtomicU32(&recorder->queue_head);
    const Uint32 tail = SDL_GetAtomicU32(&recorder->queue_tail);
    if (recorder->active) {
        SDL_SetError("Recorder is already recording");
    } else if (head != tail) {
        SDL_SetError("Recorder queue has not been drained");
    } else {
        // The recording starts at the tail of what's already been produced; the
        // most recent preroll frames that actually exist become its first frames.
        const Uint64 now = recorder->frames_seen;
        const Uint64 avail = (recorder->preroll_frames > 0) ? SDL_min((Uint64) recorder->preroll_frames, now) : 0;
        const int channels = recorder->channels;

        SDL_SetAtomicU32(&recorder->queue_head, 0);
        for (Uint64 i = 0; i < avail; i++) {
            const Uint64 frame = (now - avail) + i;
            const Uint64 pslot = frame % (Uint64) recorder->preroll_frames;
            SDL_memcpy(recorder->queue + ((size_t) i * (size_t) channels), recorder->preroll + ((size_t) pslot * (size_t) channels), (size_t) channels * sizeof (float));
            recorder->queue_frames[i] = frame;
        }

        recorder->start_frame = now - avail;
        recorder->end_frame = now;
        recorder->dropped_frames = 0;
        recorder->active = true;
        SDL_SetAtomicU32(&recorder->queue_tail, (Uint32) avail);  // publishes the preroll to the reader.
        retval = true;
    }

    UnlockRecorder(recorder);
    return retval;
}

bool MIX_StopRecorder(MIX_Recorder *recorder)
{
    if (!recorder) {
        return SDL_InvalidParamError("recorder");
    }

    LockRecorder(recorder);
    if (recorder->active) {
        recorder->active = false;
        recorder->end_frame = recorder->frames_seen;  // frozen here; the queue stays readable until drained.
    }
    UnlockRecorder(recorder);
    return true;
}

bool MIX_GetRecorderState(MIX_Recorder *recorder, MIX_RecorderState *state)
{
    if (!recorder) {
        return SDL_InvalidParamError("recorder");
    } else if (!state) {
        return SDL_InvalidParamError("state");
    }

    LockRecorder(recorder);
    state->active = recorder->active;
    state->start_frame = recorder->start_frame;
    state->end_frame = recorder->end_frame;
    state->dropped_frames = recorder->dropped_frames;
    UnlockRecorder(recorder);
    return true;
}

int MIX_ReadRecorder(MIX_Recorder *recorder, float *dst, int max_frames, Uint64 *first_frame)
{
    if (!recorder) {
        SDL_InvalidParamError("recorder");
        return -1;
    } else if (!dst) {
        SDL_InvalidParamError("dst");
        return -1;
    } else if (!first_frame) {
        SDL_InvalidParamError("first_frame");
        return -1;
    } else if (max_frames <= 0) {
        SDL_InvalidParamError("max_frames");
        return -1;
    }

    // This runs on the reader thread without the mixer lock; queue_head is ours,
    // queue_tail is the mixing thread's. The atomics keep both sides in sync.
    const int channels = recorder->channels;
    const Uint32 capacity = (Uint32) recorder->capacity_frames;
    const Uint32 head = SDL_GetAtomicU32(&recorder->queue_head);
    const Uint32 tail = SDL_GetAtomicU32(&recorder->queue_tail);
    const Uint32 avail = tail - head;
    if (avail == 0) {
        return 0;  // nothing recorded right now.
    }

    // only return the contiguous run at the front; never stitch across a drop gap.
    const Uint64 first = recorder->queue_frames[head % capacity];
    Uint32 run = 1;
    while ((run < avail) && (recorder->queue_frames[(head + run) % capacity] == (first + run))) {
        run++;
    }

    Uint32 amount = (Uint32) max_frames;
    if (amount > run) {
        amount = run;
    }

    for (Uint32 i = 0; i < amount; i++) {
        SDL_memcpy(dst + ((size_t) i * (size_t) channels), recorder->queue + ((size_t) ((head + i) % capacity) * (size_t) channels), (size_t) channels * sizeof (float));
    }

    *first_frame = first;
    SDL_SetAtomicU32(&recorder->queue_head, head + amount);  // releases the slots to the mixing thread.
    return (int) amount;
}

void MIX_DestroyRecorder(MIX_Recorder *recorder)
{
    if (!recorder) {
        return;  // harmless no-op.
    }

    MIX_Mixer *mixer = recorder->mixer;
    LockRecorder(recorder);
    if (mixer->recorder == recorder) {
        mixer->recorder = NULL;
    }
    UnlockRecorder(recorder);

    SDL_free(recorder->preroll);
    SDL_free(recorder->queue_frames);
    SDL_free(recorder->queue);
    SDL_free(recorder);
}
