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
 * @file DuckingInternal.h
 * @ingroup sequence
 * Internal helpers shared between the ducking rule storage and the sequence
 * reader.  This header is private to the library.
 */

#include "Audaspace.h"
#include "respec/Specification.h"
#include "sequence/Ducking.h"
#include "sequence/SequenceData.h"
#include "sequence/SequenceEntry.h"

#include <memory>
#include <vector>

AUD_NAMESPACE_BEGIN

class IReader;

/**
 * The detector parameters of a validated ducking rule, precomputed for a
 * specific output sample rate.
 */
struct DuckingRuleParams
{
	/// Detection threshold, linear.
	float threshold;

	/// Compression slope, <tt>1 - 1/ratio</tt>.
	float slope;

	/// Attack time in seconds.
	float attack;

	/// Release time in seconds.
	float release;

	/// Look-ahead in output sample frames.
	int lookahead;

	/// One-pole coefficient used while the target gain decreases.
	double attackCoefficient;

	/// One-pole coefficient used while the target gain increases.
	double releaseCoefficient;
};

/**
 * Returns the target log gain of the detector for a given key amplitude.
 */
float duckingTargetGain(float amplitude, const DuckingRuleParams& params);

/**
 * Applies one envelope step.
 * \return The new envelope value.
 */
double duckingStep(double previous, float target, const DuckingRuleParams& params);

/**
 * Compares two detector parameter sets for equality.
 */
bool sameRuleSettings(const DuckingRuleParams& a, const DuckingRuleParams& b);

/**
 * Renders one entry's pre-ducking PCM in scene time.
 *
 * The rendered signal is the entry's source after its own constant volume and
 * its begin/skip/end window have been applied, mapped to the scene channel
 * layout with the entry's default panning, and before the scene volume and any
 * ducking have been applied.  Samples outside <tt>[begin, end)</tt> are zero.
 *
 * The renderer owns its own source reader; two renderers for the same entry
 * therefore have independent cursors.  It is seekable in output sample frames
 * and uses a fixed amount of memory regardless of the rendered position.
 */
class DuckingEntryRenderer
{
private:
	/// The entry to render.
	std::shared_ptr<SequenceEntry> m_entry;

	/// Scene specification.
	Specs m_scene;

	/// The animation frame rate of the scene.
	float m_fps;

	/// The source reader owned by this renderer.
	std::shared_ptr<IReader> m_reader;

	/// The sound the reader was created from.
	std::shared_ptr<ISound> m_sound;

	/// Source channels.
	Channels m_sourceChannels;

	/// Source sample rate.
	SampleRate m_sourceRate;

	/// Next source frame to read.
	long long m_sourcePosition;

	/// Whether the source position is valid.
	bool m_sourcePositionValid;

	// delete copy constructor and operator=
	DuckingEntryRenderer(const DuckingEntryRenderer&) = delete;
	DuckingEntryRenderer& operator=(const DuckingEntryRenderer&) = delete;

	/// Recreates the source reader if the entry's sound changed.
	void refreshSound();

	/// Seeks the source reader to a source frame.
	void seekSource(long long frame);

	/// Maps one source frame to the scene channel layout.
	void mapFrame(const sample_t* source, sample_t* target) const;

public:
	/**
	 * Creates a renderer for an entry.
	 * \param entry The entry to render.
	 * \param scene The scene output specification.
	 * \param fps The scene frame rate.
	 */
	DuckingEntryRenderer(std::shared_ptr<SequenceEntry> entry, Specs scene, float fps);

	/**
	 * Re-reads the entry's current sound and invalidates the source cursor.
	 * The next render resumes at its requested scene frame.
	 */
	void reset();

	/**
	 * Renders a block of scene frames.
	 * \param startFrame The first scene output frame.
	 * \param frames The number of frames to render.
	 * \param buffer The target buffer, interleaved scene channels.  It must
	 *        hold <tt>frames * scene.channels</tt> samples.
	 */
	void render(long long startFrame, int frames, sample_t* buffer);
};

/**
 * A snapshot of a ducking rule with detector values precomputed for one
 * output sample rate.
 */
struct DuckingRuleSnapshot
{
	/// The target entry.
	std::shared_ptr<SequenceEntry> target;

	/// The key entry.
	std::shared_ptr<SequenceEntry> key;

	/// Precomputed detector parameters.
	DuckingRuleParams params;
};

/**
 * Copies the current ducking rules as a stable snapshot.
 * \param data The sequence data, which must be locked by the caller.
 * \param rate The output sample rate.
 */
std::vector<DuckingRuleSnapshot> captureDuckingRules(const SequenceData& data, SampleRate rate);

/**
 * Per-reader state of a single ducking rule.
 *
 * The runtime owns its own key renderer, its own target renderer and the
 * envelope value for the last output frame it produced.  Two readers of the
 * same scene therefore never share envelope state.
 */
struct SequenceDuckingRuntime
{
	/// The rule this runtime belongs to.
	DuckingRuleSnapshot rule;

	/// The renderer of the key's pre-ducking signal.
	DuckingEntryRenderer key;

	/// The renderer of the target's pre-ducking signal.
	DuckingEntryRenderer target;

	/// Envelope value of the last processed output frame.
	double envelope;

	/// The next output frame the runtime has not processed yet.
	long long nextFrame;

	/// Key timeline snapshot used to detect edits between two reads.
	double keyBegin;
	double keyEnd;
	double keySkip;
	float keyVolume;
	bool keyMuted;
	std::shared_ptr<ISound> keySound;

	/**
	 * Creates a runtime for a rule.
	 */
	SequenceDuckingRuntime(const DuckingRuleSnapshot& rule, Specs scene, float fps);

	/**
	 * Compares the runtime's key snapshot with the entry's current state.
	 * \return Whether the key timeline differs from the snapshot.
	 */
	bool keyChanged() const;

	/**
	 * Updates the key snapshot from the entry's current state.
	 */
	void updateKeySnapshot();
};

AUD_NAMESPACE_END
