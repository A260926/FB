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

#ifndef SDL_audioqueue_h_
#define SDL_audioqueue_h_

// Internal functions used by SDL_AudioStream for queueing audio.

typedef SDL_AudioStreamDataCompleteCallback SDL_ReleaseAudioBufferCallback;

typedef struct SDL_AudioQueue SDL_AudioQueue;
typedef struct SDL_AudioTrack SDL_AudioTrack;

// Create a new audio queue
extern SDL_AudioQueue *SDL_CreateAudioQueue(size_t chunk_size);

// Destroy an audio queue
extern void SDL_DestroyAudioQueue(SDL_AudioQueue *queue);

// Flip a queue to shared-buffer mode. From this point on, new tracks keep
// their PCM in SDL_SharedAudioBuffer objects that several queues can reference.
// Existing tracks are left untouched and consumed in the usual way.
extern void SDL_SetAudioQueueShared(SDL_AudioQueue *queue);
extern bool SDL_IsAudioQueueShared(const SDL_AudioQueue *queue);

// Completely clear the queue
extern void SDL_ClearAudioQueue(SDL_AudioQueue *queue);

// Mark the last track as flushed
extern void SDL_FlushAudioQueue(SDL_AudioQueue *queue);

// Pop the current head track
// REQUIRES: The head track must exist, and must have been flushed
extern void SDL_PopAudioQueueHead(SDL_AudioQueue *queue);

// Write data to the end of queue
// REQUIRES: If the spec has changed, the last track must have been flushed
extern bool SDL_WriteToAudioQueue(SDL_AudioQueue *queue, const SDL_AudioSpec *spec, const int *chmap, const Uint8 *data, size_t len);

// Create a track where the input data is owned by the caller
extern SDL_AudioTrack *SDL_CreateAudioTrack(SDL_AudioQueue *queue,
                                            const SDL_AudioSpec *spec, const int *chmap, Uint8 *data, size_t len, size_t capacity,
                                            SDL_ReleaseAudioBufferCallback callback, void *userdata);

// The data backing for one audio submission, shared between every stream
// (the producer and its taps) that is still feeding from it. The single copy
// of the PCM lives here until the last consumer has read past it.
typedef struct SDL_SharedAudioBuffer SDL_SharedAudioBuffer;

struct SDL_SharedAudioBuffer
{
    Uint8 *data;
    size_t len;
    size_t capacity;
    void *userdata;
    SDL_ReleaseAudioBufferCallback callback;
    SDL_AtomicInt refcount;
    bool owns_data;  // true for buffers that copied caller data and must free it themselves
};

// Create a shared buffer holding a single copy of submitted PCM.
// NoCopy submissions borrow the caller's buffer directly; the release
// callback fires once, when the last consumer drops its reference.
extern SDL_SharedAudioBuffer *SDL_CreateSharedAudioBuffer(Uint8 *data, size_t len, size_t capacity,
                                                          SDL_ReleaseAudioBufferCallback callback, void *userdata);

// Duplicate `data` into a new shared buffer released with the default allocator.
extern SDL_SharedAudioBuffer *SDL_CreateSharedAudioBufferCopy(const void *data, size_t len);

// Take or drop a reference to a shared buffer.
extern SDL_SharedAudioBuffer *SDL_RefSharedAudioBuffer(SDL_SharedAudioBuffer *buffer);
extern void SDL_UnrefSharedAudioBuffer(SDL_SharedAudioBuffer *buffer);

// Discard a buffer that no consumer ever took a reference to, without running
// its release callback. Used when a submission fails: borrowed/NoCopy data and
// large-put copies stay owned by the caller, which handles them itself.
extern void SDL_DropUnusedSharedAudioBuffer(SDL_SharedAudioBuffer *buffer);

// Create a track that reads from a shared buffer. `head` lets a late-joining
// consumer start at an arbitrary offset within the buffer.
extern SDL_AudioTrack *SDL_CreateSharedAudioTrack(SDL_AudioQueue *queue,
                                                   const SDL_AudioSpec *spec, const int *chmap,
                                                   SDL_SharedAudioBuffer *buffer, size_t head);

// Add one shared-buffer track to each of the given queues. Every queue gets
// its own track object (its own read cursor, metadata and flush state), but
// they all reference the same backing PCM. On failure all tracks created for
// the submission are rolled back, so no queue observes a partial submission.
extern bool SDL_FanOutSharedTrack(SDL_AudioQueue **queues, int num_queues,
                                  const SDL_AudioSpec *spec, const int *chmap,
                                  SDL_SharedAudioBuffer *buffer, const size_t *heads);

// Add a track to the end of the queue
// REQUIRES: `track != NULL`
extern void SDL_AddTrackToAudioQueue(SDL_AudioQueue *queue, SDL_AudioTrack *track);

// Iterate over the tracks in the queue
extern void *SDL_BeginAudioQueueIter(SDL_AudioQueue *queue);

// Query and update the track iterator
// REQUIRES: `*inout_iter != NULL` (a valid iterator)
extern size_t SDL_NextAudioQueueIter(SDL_AudioQueue *queue, void **inout_iter, SDL_AudioSpec *out_spec, int **out_chmap, bool *out_flushed);

extern const Uint8 *SDL_ReadFromAudioQueue(SDL_AudioQueue *queue,
                                           Uint8 *dst, SDL_AudioFormat dst_format, int dst_channels, const int *dst_map,
                                           int past_frames, int present_frames, int future_frames,
                                           Uint8 *scratch, float gain);

// Get the total number of bytes currently queued
extern size_t SDL_GetAudioQueueQueued(SDL_AudioQueue *queue);

extern bool SDL_ResetAudioQueueHistory(SDL_AudioQueue *queue, int num_frames);

#endif // SDL_audioqueue_h_
