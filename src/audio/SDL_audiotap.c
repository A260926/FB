/*
  Simple DirectMedia Layer
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
#include "SDL_internal.h"

#include "SDL_sysaudio.h"
#include "SDL_audioqueue.h"
#include "SDL_audiotap.h"

SDL_AudioStream *SDL_CreateAudioStreamTapInternal(SDL_AudioStream *producer, const SDL_AudioSpec *dst_spec)
{
    SDL_LockMutex(producer->lock);

    if (producer->is_tap) {
        SDL_UnlockMutex(producer->lock);
        SDL_SetError("Cannot create a tap from another audio stream tap");
        return NULL;
    }

    SDL_AudioStream *tap = (SDL_AudioStream *)SDL_calloc(1, sizeof(*tap));
    if (!tap) {
        SDL_UnlockMutex(producer->lock);
        return NULL;
    }

    tap->freq_ratio = 1.0f;
    tap->gain = 1.0f;
    tap->queue = SDL_CreateAudioQueue(8192);
    if (!tap->queue) {
        SDL_free(tap);
        SDL_UnlockMutex(producer->lock);
        return NULL;
    }
    // Tap queues only ever hold shared-buffer tracks.
    SDL_SetAudioQueueShared(tap->queue);

    tap->lock = SDL_CreateMutex();
    if (!tap->lock) {
        SDL_DestroyAudioQueue(tap->queue);
        SDL_free(tap);
        SDL_UnlockMutex(producer->lock);
        return NULL;
    }

    // The tap consumes whatever the producer is currently configured to accept.
    SDL_copyp(&tap->src_spec, &producer->src_spec);
    if (producer->src_chmap) {
        tap->src_chmap = SDL_ChannelMapDup(producer->src_chmap, producer->src_spec.channels);
        if (!tap->src_chmap) {
            SDL_DestroyMutex(tap->lock);
            SDL_DestroyAudioQueue(tap->queue);
            SDL_free(tap);
            SDL_UnlockMutex(producer->lock);
            return NULL;
        }
    }
    SDL_copyp(&tap->dst_spec, dst_spec);

    tap->is_tap = true;
    tap->tap_producer = producer;

    OnAudioStreamCreated(tap);

    // From the first tap onward, the producer's queue only holds shared tracks.
    // Anything already queued stays put and drains through the regular code.
    producer->has_taps = true;
    SDL_SetAudioQueueShared(producer->queue);

    // Link the tap at the head of the producer's list.
    tap->tap_next = producer->tap_next;
    tap->tap_prev = NULL;
    if (producer->tap_next) {
        producer->tap_next->tap_prev = tap;
    }
    producer->tap_next = tap;

    SDL_UnlockMutex(producer->lock);

    return tap;
}

bool SDL_DestroyAudioStreamTap(SDL_AudioStream *tap)
{
    if (!tap->is_tap) {
        return false;
    }

    SDL_AudioStream *producer = tap->tap_producer;
    if (producer) {
        SDL_LockMutex(producer->lock);
        if (tap->tap_prev) {
            tap->tap_prev->tap_next = tap->tap_next;
        } else {
            producer->tap_next = tap->tap_next;
        }
        if (tap->tap_next) {
            tap->tap_next->tap_prev = tap->tap_prev;
        }
        producer->has_taps = (producer->tap_next != NULL);
        tap->tap_producer = NULL;
        tap->tap_next = NULL;
        tap->tap_prev = NULL;
        SDL_UnlockMutex(producer->lock);
    }

    // Wait for any in-flight Get on this tap to finish before tearing it down.
    SDL_LockMutex(tap->lock);
    OnAudioStreamDestroy(tap);

    SDL_free(tap->src_chmap);
    SDL_free(tap->dst_chmap);
    SDL_DestroyProperties(tap->props);
    SDL_DestroyAudioQueue(tap->queue);
    SDL_UnlockMutex(tap->lock);
    SDL_DestroyMutex(tap->lock);
    SDL_free(tap);

    return true;
}

bool SDL_FanOutTapPut(SDL_AudioStream *producer,
                      const SDL_AudioSpec *spec, const int *chmap,
                      const void *buf, int len,
                      SDL_ReleaseAudioBufferCallback callback, void *userdata,
                      bool *handled)
{
    *handled = false;

    if (!producer->has_taps) {
        return true;  // no taps: the caller takes the regular single-queue path
    }

    // One backing copy of the PCM for every consumer.
    SDL_SharedAudioBuffer *buffer;
    if (callback) {
        // NoCopy submission: borrow the caller's buffer; the release callback
        // is owned by the shared buffer and runs once, when the last
        // consumer drops its reference (even if the producer is gone).
        buffer = SDL_CreateSharedAudioBuffer((Uint8 *)(uintptr_t)buf, (size_t)len, (size_t)len, callback, userdata);
    } else {
        // Ordinary submission: copy the caller's data before returning, just
        // like a single-stream SDL_PutAudioStreamData would.
        buffer = SDL_CreateSharedAudioBufferCopy(buf, (size_t)len);
    }
    if (!buffer) {
        return false;
    }

    // Count the consumers: the producer itself and every live tap.
    int num_consumers = 1;
    for (SDL_AudioStream *tap = producer->tap_next; tap; tap = tap->tap_next) {
        ++num_consumers;
    }

    SDL_AudioQueue **queues = (SDL_AudioQueue **)SDL_malloc(sizeof(*queues) * (size_t)num_consumers);
    if (!queues) {
        SDL_DropUnusedSharedAudioBuffer(buffer);
        return false;
    }

    int num_queues = 0;
    queues[num_queues++] = producer->queue;
    for (SDL_AudioStream *tap = producer->tap_next; tap; tap = tap->tap_next) {
        queues[num_queues++] = tap->queue;
    }

    // The Get path of every stream holds that stream's own lock while
    // touching its queue. Producer control calls already hold the producer
    // lock, so take each tap lock in list order (fixed producer -> tap
    // ordering; nothing ever takes them in the reverse direction).
    for (SDL_AudioStream *tap = producer->tap_next; tap; tap = tap->tap_next) {
        SDL_LockMutex(tap->lock);
    }

    // Each consumer gets its own track (read cursor, metadata and history),
    // all referencing the single shared backing buffer. On failure none of
    // the queues is touched, so no consumer observes a partial submission.
    bool ok = SDL_FanOutSharedTrack(queues, num_queues, spec, chmap, buffer, NULL);

    for (SDL_AudioStream *tap = producer->tap_next; tap; tap = tap->tap_next) {
        SDL_UnlockMutex(tap->lock);
    }

    SDL_free(queues);

    if (!ok) {
        // The buffer never reached a queue; its data is handled by the owner.
        SDL_DropUnusedSharedAudioBuffer(buffer);
        return false;
    }

    // FanOutSharedTrack gave every track its own reference; drop ours.
    SDL_UnrefSharedAudioBuffer(buffer);
    *handled = true;
    return true;
}

void SDL_FanOutTapFlush(SDL_AudioStream *producer)
{
    for (SDL_AudioStream *tap = producer->tap_next; tap; tap = tap->tap_next) {
        SDL_LockMutex(tap->lock);
        SDL_FlushAudioQueue(tap->queue);
        SDL_UnlockMutex(tap->lock);
    }
}

void SDL_OrphanAudioStreamTaps(SDL_AudioStream *producer)
{
    if (!producer->has_taps) {
        return;
    }

    SDL_AudioStream *next;
    for (SDL_AudioStream *tap = producer->tap_next; tap; tap = next) {
        next = tap->tap_next;
        SDL_LockMutex(tap->lock);
        tap->tap_producer = NULL;
        tap->tap_next = NULL;
        tap->tap_prev = NULL;
        SDL_UnlockMutex(tap->lock);
    }
    producer->tap_next = NULL;
    producer->has_taps = false;
}
