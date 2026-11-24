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

/*
 * Output recording with rolling preroll.
 *
 * The recorder taps the mixer's final interleaved float32 PCM in
 * MixerCallback(), after the postmix callback and before the data is handed
 * to the output stream. The mixing thread is the sole producer; a reader
 * thread running MIX_ReadRecorder() is the sole consumer. The hot path never
 * allocates, does no file I/O, never waits on the reader, and never touches
 * slots already handed to the consumer.
 *
 * The ring stores `capacity` sample frames. Each slot carries the absolute
 * frame number it holds (one plus the frame index, zero meaning empty), so
 * frames dropped because the queue was full show up as gaps instead of
 * stale data. Producer/consumer positions are 64-bit single-writer values
 * published through a small sequence-lock built on SDL 32-bit atomics.
 */

#include "SDL_mixer_internal.h"

// Recorder lifecycle phases.
#define MIX_RECORDER_PREROLL  0  // no active session; rolling preroll, or stopped and fully drained.
#define MIX_RECORDER_ACTIVE   1  // recording session in progress.
#define MIX_RECORDER_DRAINING 2  // session stopped; queued frames remain readable until drained.

// Single-writer/single-reader 64-bit value. The writer toggles `seq` odd
// while updating `value`; readers retry until they observe a stable even
// sequence around their snapshot.
typedef struct MIX_RecU64
{
    SDL_AtomicInt seq;
    Uint64 value;
} MIX_RecU64;

static void RecU64_Store(MIX_RecU64 *slot, Uint64 value)
{
    SDL_SetAtomicInt(&slot->seq, 1);
    SDL_MemoryBarrierRelease();
    slot->value = value;
    SDL_MemoryBarrierRelease();
    SDL_SetAtomicInt(&slot->seq, 0);
}

static Uint64 RecU64_Load(const MIX_RecU64 *slot)
{
    for (;;) {
        const int seq_before = SDL_GetAtomicInt((SDL_AtomicInt *) &slot->seq);
        SDL_MemoryBarrierAcquire();
        if (seq_before & 1) {
            continue;  // a store is in progress.
        }
        const Uint64 value = slot->value;
        SDL_MemoryBarrierAcquire();
        if (SDL_GetAtomicInt((SDL_AtomicInt *) &slot->seq) == seq_before) {
            return value;
        }
    }
}

struct MIX_Recorder
{
    MIX_Mixer *mixer;
    int channels;           // interleaved floats per sample frame.
    Uint64 capacity;        // ring size, in sample frames.
    Uint64 preroll;         // frames of rolling history retained for a Start.
    float *data;            // capacity * channels floats of PCM.
    Uint64 *tags;           // absolute frame index + 1 for each slot; 0 == empty.

    MIX_RecU64 head;        // oldest queued absolute frame; advanced only by the reader.
    MIX_RecU64 tail;        // one past the last absolute frame seen; advanced only by the producer.
    SDL_AtomicInt phase;    // MIX_RECORDER_* value.

    Uint64 session_start;   // absolute frame of the current/last session, preroll included.
    Uint64 session_end;     // exclusive absolute tail captured at Stop.
    Uint64 dropped;         // frames dropped during the active session.
};

// Mixing-thread entry point: feed one postmix batch into the recorder.
// Called with the mixer lock held (from inside MixerCallback).
void MIX_RecordMixedFrames(MIX_Mixer *mixer, const float *pcm, int frames)
{
    MIX_Recorder *recorder = mixer->recorder;
    if (!recorder || (frames <= 0)) {
        return;
    }

    // Device hot-swapping can change the channel count; the recorder's ring
    // was sized for the spec at creation time, so leave the new format alone
    // rather than stride outside the allocation.
    if (recorder->channels != mixer->spec.channels) {
        return;
    }

    const int channels = recorder->channels;
    const Uint64 capacity = recorder->capacity;
    const int phase = SDL_GetAtomicInt(&recorder->phase);
    SDL_assert((phase >= MIX_RECORDER_PREROLL) && (phase <= MIX_RECORDER_DRAINING));

    for (int i = 0; i < frames; ++i) {
        const Uint64 absolute = RecU64_Load(&recorder->tail);
        const Uint64 slot_index = absolute % capacity;

        if (phase == MIX_RECORDER_ACTIVE) {
            // The reader owns `head`; a full queue means the oldest unread
            // frame still occupies every slot, so this frame is dropped and
            // its slot is left untouched (its stale/empty tag marks the gap).
            const Uint64 head = RecU64_Load(&recorder->head);
            if ((absolute - head) < capacity) {
                float *slot = recorder->data + (slot_index * (Uint64) channels);
                SDL_memcpy(slot, pcm + ((size_t) i * (size_t) channels), (size_t) channels * sizeof (float));
                SDL_MemoryBarrierRelease();
                recorder->tags[slot_index] = absolute + 1;  // publish after the PCM.
            } else {
                ++recorder->dropped;
            }
        } else if (phase == MIX_RECORDER_PREROLL) {
            // No session data is owed to a reader: keep the rolling history fresh.
            float *slot = recorder->data + (slot_index * (Uint64) channels);
            SDL_memcpy(slot, pcm + ((size_t) i * (size_t) channels), (size_t) channels * sizeof (float));
            SDL_MemoryBarrierRelease();
            recorder->tags[slot_index] = absolute + 1;
        }
        // In MIX_RECORDER_DRAINING, the stopped session still owns the ring;
        // this frame is counted (tail advances) but not stored.

        RecU64_Store(&recorder->tail, absolute + 1);
    }
}

// Detach and free any recorder attached to a mixer. Called from
// MIX_DestroyMixer(), with no concurrent control/reader access.
void MIX_DestroyMixerRecorder(MIX_Mixer *mixer)
{
    MIX_Recorder *recorder = mixer->recorder;
    if (!recorder) {
        return;
    }
    mixer->recorder = NULL;
    SDL_free(recorder->data);
    SDL_free(recorder->tags);
    SDL_free(recorder);
}

MIX_Recorder *MIX_CreateRecorder(MIX_Mixer *mixer, Uint64 preroll_frames, Uint64 capacity_frames)
{
    if (!mixer) {
        SDL_InvalidParamError("mixer");
        return NULL;
    } else if (capacity_frames == 0) {
        SDL_InvalidParamError("capacity_frames");
        return NULL;
    } else if (preroll_frames >= capacity_frames) {
        SDL_SetError("preroll_frames must be smaller than capacity_frames");
        return NULL;
    } else if (mixer->spec.channels <= 0) {
        SDL_SetError("Mixer has an invalid channel count");
        return NULL;
    }

    const int channels = mixer->spec.channels;

    // Keep every frame index usable in int-sized math, and reject overflows.
    if (capacity_frames > (Uint64) SDL_MAX_SINT32) {
        SDL_InvalidParamError("capacity_frames");
        return NULL;
    } else if (capacity_frames > (SIZE_MAX / (size_t) channels) / sizeof (float)) {
        SDL_OutOfMemory();
        return NULL;
    } else if (capacity_frames > (SIZE_MAX / sizeof (Uint64))) {
        SDL_OutOfMemory();
        return NULL;
    }

    MIX_Recorder *recorder = (MIX_Recorder *) SDL_calloc(1, sizeof (*recorder));
    if (!recorder) {
        return NULL;
    }

    const size_t pcm_bytes = (size_t) capacity_frames * (size_t) channels * sizeof (float);
    recorder->data = (float *) SDL_calloc(1, pcm_bytes);  // zeroed: dropped slots read back as silence.
    if (!recorder->data) {
        SDL_free(recorder);
        return NULL;
    }

    recorder->tags = (Uint64 *) SDL_calloc(1, (size_t) capacity_frames * sizeof (Uint64));
    if (!recorder->tags) {
        SDL_free(recorder->data);
        SDL_free(recorder);
        return NULL;
    }

    recorder->mixer = mixer;
    recorder->channels = channels;
    recorder->capacity = capacity_frames;
    recorder->preroll = preroll_frames;
    SDL_SetAtomicInt(&recorder->phase, MIX_RECORDER_PREROLL);

    // Attach under the mixer lock, at most one recorder per mixer.
    MIX_LockMixer(mixer);
    if (mixer->recorder) {
        MIX_UnlockMixer(mixer);
        SDL_SetError("Mixer already has a recorder attached");
        SDL_free(recorder->tags);
        SDL_free(recorder->data);
        SDL_free(recorder);
        return NULL;
    }
    mixer->recorder = recorder;
    MIX_UnlockMixer(mixer);

    return recorder;
}

bool MIX_StartRecorder(MIX_Recorder *recorder)
{
    if (!recorder) {
        return SDL_InvalidParamError("recorder");
    }

    MIX_Mixer *mixer = recorder->mixer;
    MIX_LockMixer(mixer);

    const int phase = SDL_GetAtomicInt(&recorder->phase);
    if (phase != MIX_RECORDER_PREROLL) {
        // Active, or a stopped session whose queue has not been drained yet.
        MIX_UnlockMixer(mixer);
        return SDL_SetError("Recorder is active or still holds unread frames");
    }

    const Uint64 tail = RecU64_Load(&recorder->tail);
    const Uint64 available = SDL_min(tail, recorder->preroll);
    const Uint64 start = tail - available;

    recorder->session_start = start;
    recorder->session_end = 0;
    recorder->dropped = 0;
    RecU64_Store(&recorder->head, start);
    SDL_SetAtomicInt(&recorder->phase, MIX_RECORDER_ACTIVE);

    MIX_UnlockMixer(mixer);
    return true;
}

bool MIX_StopRecorder(MIX_Recorder *recorder)
{
    if (!recorder) {
        return SDL_InvalidParamError("recorder");
    }

    MIX_Mixer *mixer = recorder->mixer;
    MIX_LockMixer(mixer);

    if (SDL_GetAtomicInt(&recorder->phase) == MIX_RECORDER_ACTIVE) {
        recorder->session_end = RecU64_Load(&recorder->tail);
        SDL_SetAtomicInt(&recorder->phase, MIX_RECORDER_DRAINING);
    }
    // Calling Stop before Start, or after an earlier Stop, leaves state intact.

    MIX_UnlockMixer(mixer);
    return true;
}

bool MIX_GetRecorderState(MIX_Recorder *recorder, MIX_RecorderState *state)
{
    if (!recorder) {
        return SDL_InvalidParamError("recorder");
    } else if (!state) {
        return SDL_InvalidParamError("state");
    }

    MIX_Mixer *mixer = recorder->mixer;
    MIX_LockMixer(mixer);

    const int phase = SDL_GetAtomicInt(&recorder->phase);
    state->active = (phase == MIX_RECORDER_ACTIVE);
    state->start_frame = recorder->session_start;
    state->end_frame = (phase == MIX_RECORDER_ACTIVE) ? RecU64_Load(&recorder->tail) : recorder->session_end;
    state->dropped_frames = recorder->dropped;

    MIX_UnlockMixer(mixer);
    return true;
}

// Find the next absolute frame at or after `from` whose slot holds real,
// current data, bounded by `limit`. Stale tags from a ring revolution ago
// point behind `from` and are treated as "nothing here yet".
static Uint64 NextReadableFrame(const MIX_Recorder *recorder, Uint64 from, Uint64 limit)
{
    for (Uint64 absolute = from; absolute < limit; ++absolute) {
        const Uint64 tag = recorder->tags[absolute % recorder->capacity];
        if (tag == (absolute + 1)) {
            return absolute;
        }
    }
    return limit;
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

    const int phase = SDL_GetAtomicInt(&recorder->phase);
    const Uint64 tail = RecU64_Load(&recorder->tail);
    Uint64 head = RecU64_Load(&recorder->head);

    // While draining, frames counted past the Stop boundary are not part of
    // the session and were never stored. In preroll, nothing is owed yet.
    const Uint64 limit = (phase == MIX_RECORDER_ACTIVE) ? tail : recorder->session_end;

    if (head >= limit) {
        if (phase == MIX_RECORDER_DRAINING) {
            // Queue fully drained: hand ring ownership back to rolling preroll.
            SDL_CompareAndSwapAtomicInt(&recorder->phase, MIX_RECORDER_DRAINING, MIX_RECORDER_PREROLL);
        }
        return 0;
    }

    // A missing or stale slot means frames were dropped here: jump to the
    // next real frame without stitching PCM across the gap.
    if (recorder->tags[head % recorder->capacity] != (head + 1)) {
        head = NextReadableFrame(recorder, head + 1, limit);
        RecU64_Store(&recorder->head, head);
        if (head >= limit) {
            if (phase == MIX_RECORDER_DRAINING) {
                SDL_CompareAndSwapAtomicInt(&recorder->phase, MIX_RECORDER_DRAINING, MIX_RECORDER_PREROLL);
            }
            return 0;  // the gap (possibly a dropped tail) carries no PCM.
        }
    }

    // From `head`, take the longest contiguous run that fits the request,
    // the readable window, and a single ring wrap.
    Uint64 count = SDL_min((Uint64) max_frames, limit - head);
    count = SDL_min(count, recorder->capacity - (head % recorder->capacity));

    // Trim at any further gap so one read never spans discontinuous frames.
    Uint64 run = 0;
    while (run < count) {
        const Uint64 absolute = head + run;
        if (recorder->tags[absolute % recorder->capacity] != (absolute + 1)) {
            break;
        }
        ++run;
    }
    count = run;
    if (count == 0) {
        return 0;  // producer has not published this slot yet; try again later.
    }

    const size_t frame_size = (size_t) recorder->channels * sizeof (float);
    const Uint64 slot_index = head % recorder->capacity;
    SDL_MemoryBarrierAcquire();  // pair with the producer's release before the tag publish.
    SDL_memcpy(dst, recorder->data + (slot_index * (Uint64) recorder->channels), (size_t) count * frame_size);

    *first_frame = head;
    RecU64_Store(&recorder->head, head + count);
    return (int) count;
}

void MIX_DestroyRecorder(MIX_Recorder *recorder)
{
    if (!recorder) {
        return;
    }

    MIX_Mixer *mixer = recorder->mixer;
    MIX_LockMixer(mixer);
    if (mixer->recorder == recorder) {
        mixer->recorder = NULL;
    }
    MIX_UnlockMixer(mixer);

    SDL_free(recorder->data);
    SDL_free(recorder->tags);
    SDL_free(recorder);
}
