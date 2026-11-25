/*******************************************************************************
 * Copyright 2026 Audaspace contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ******************************************************************************/

#pragma once

/**
 * @file Ducking.h
 * @ingroup sequence
 * Settings for look-ahead ducking between two sequence entries.
 */

#include "Audaspace.h"

AUD_NAMESPACE_BEGIN

/**
 * Settings of a look-ahead ducking rule.
 *
 * A rule links a target entry to a key entry.  The key is rendered in scene
 * time, after its own volume and muting but completely independently of any
 * ducking, and drives a per-output-frame gain envelope that is applied to the
 * target.
 *
 * For scene output frame @c n the detector samples the key at frame
 * <tt>n + lookahead</tt>, with the key's begin/end/skip window applied and
 * silence outside of it.  The envelope itself starts at scene frame zero with
 * <tt>z[-1] = 0</tt>:
 *
 * - <tt>A[n]</tt> is the largest absolute value over all channels of the key
 *   at the sampled frame.
 * - If <tt>A[n] &lt;= threshold</tt> the target log gain is zero, otherwise it
 *   is <tt>(1 - 1/ratio) * log(threshold / A[n])</tt>.
 * - The envelope moves towards that target with the attack coefficient when
 *   the target is below the previous value, and with the release coefficient
 *   otherwise.  For a time @c t the coefficient is <tt>a = 0</tt> when
 *   <tt>t == 0</tt>, and <tt>a = exp(-1 / (rate * t))</tt> otherwise, with
 *   <tt>z[n] = a * z[n-1] + (1 - a) * target</tt>.
 * - The target's own pre-ducking PCM is multiplied per frame by
 *   <tt>exp(z[n])</tt>, using the same gain for every channel, before it is
 *   summed into the scene.
 *
 * The look-ahead does not delay the finished scene and does not make the key
 * audible earlier: it only shifts which key frame drives the envelope.
 *
 * ## Seeking
 *
 * Seeking a reader is not the same as clearing the envelope.  The detector
 * history is rebuilt from the scene start up to the seek position, so that
 * <tt>seek(p)</tt> followed by a read yields exactly the same samples as
 * rendering from zero and discarding the first @c p frames.  Rebuilding walks
 * the key's timeline from frame zero, which costs time proportional to @c p;
 * the memory use is bounded by the look-ahead, a fixed work block and the
 * number of rules and does not grow with the playback position.
 *
 * ## Reader independence
 *
 * Every reader of a scene owns its own envelope state, so two readers do not
 * affect each other.  If an entry's key timeline is edited between two reads,
 * the reader keeps its output cursor and rebuilds the detector history up to
 * the current position from the edited timeline on the next read.
 *
 * ## Scope
 *
 * This version supports mono or stereo, seekable sources whose sample rate
 * equals the scene rate.  Participating entries are relative, use the default
 * panning, have a fixed pitch of one, have positions and skips on output
 * sample frame boundaries, and have a constant volume.  Entries without a rule
 * and scenes without rules keep their previous behaviour.
 */
struct DuckingSettings
{
	/// Detection threshold in linear amplitude. Must be finite and > 0.
	float threshold;

	/// Compression ratio. Must be finite and >= 1.
	float ratio;

	/// Attack time in seconds. Must be finite and >= 0.
	float attack;

	/// Release time in seconds. Must be finite and >= 0.
	float release;

	/// Look-ahead in output sample frames, in the range [0, 65536].
	int lookahead;
};

AUD_NAMESPACE_END
