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

#ifndef SDL_audiotap_h_
#define SDL_audiotap_h_

// Internal implementation of SDL_CreateAudioStreamTap ().

struct SDL_AudioStream;

// Create a tap on `producer`. The producer must be an ordinary stream (not a
// tap itself); validation of `dst_spec` happens in the public wrapper.
extern struct SDL_AudioStream *SDL_CreateAudioStreamTapInternal(struct SDL_AudioStream *producer, const SDL_AudioSpec *dst_spec);

// Tap-specific teardown; unlinks the tap from its producer and frees the
// pieces that a tap owns separately. Returns true if `stream` was a tap.
extern bool SDL_DestroyAudioStreamTap(struct SDL_AudioStream *stream);

// Fan out a producer submission (ordinary or NoCopy) to the producer's taps.
// On return `*handled` is true when the producer currently has taps: the
// submission has then been committed (shared) to every consumer queue and the
// caller must skip its regular single-queue path. A false result with
// `*handled` true means allocation failure and no queue saw the submission.
// REQUIRES: producer->lock is held.
extern bool SDL_FanOutTapPut(struct SDL_AudioStream *producer,
                             const SDL_AudioSpec *spec, const int *chmap,
                             const void *buf, int len,
                             SDL_ReleaseAudioBufferCallback callback, void *userdata,
                             bool *handled);

// Fan out a producer flush to every tap.
// REQUIRES: producer->lock is held.
extern void SDL_FanOutTapFlush(struct SDL_AudioStream *producer);

// Detach every tap when its producer is destroyed. The taps keep draining
// the audio they already accepted and are destroyed individually later.
// REQUIRES: producer->lock is held.
extern void SDL_OrphanAudioStreamTaps(struct SDL_AudioStream *producer);

#endif // SDL_audiotap_h_
