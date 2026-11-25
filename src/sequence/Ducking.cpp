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

#include "DuckingInternal.h"
#include "sequence/Ducking.h"
#include "sequence/SequenceData.h"
#include "sequence/SequenceEntry.h"
#include "Exception.h"
#include "IReader.h"
#include "ISound.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

AUD_NAMESPACE_BEGIN

namespace
{
	/// A source frame buffer big enough for mono and stereo input.
	const int SOURCE_FRAME_SAMPLES = 2;

	/// Applies the default panning of a mono source to the scene channels.
	void mapMonoToScene(float sample, int sceneChannels, sample_t* target)
	{
		if(sceneChannels == CHANNELS_MONO)
		{
			target[0] = sample;
		}
		else if(sceneChannels == CHANNELS_STEREO)
		{
			// Default panning of zero places the mono source in the centre.
			const float gain = std::sqrt(0.5f);
			target[0] = sample * gain;
			target[1] = sample * gain;
		}
		else
		{
			target[0] = sample;
			for(int c = 1; c < sceneChannels; c++)
				target[c] = 0;
		}
	}
}

float duckingTargetGain(float amplitude, const DuckingRuleParams& params)
{
	if(amplitude <= params.threshold)
		return 0.0f;

	return params.slope * float(std::log(double(params.threshold) / double(amplitude)));
}

double duckingStep(double previous, float target, const DuckingRuleParams& params)
{
	const double a = target < previous ? params.attackCoefficient : params.releaseCoefficient;
	return a * previous + (1.0 - a) * double(target);
}

bool sameRuleSettings(const DuckingRuleParams& a, const DuckingRuleParams& b)
{
	return a.threshold == b.threshold && a.slope == b.slope && a.attack == b.attack && a.release == b.release && a.lookahead == b.lookahead;
}

DuckingEntryRenderer::DuckingEntryRenderer(std::shared_ptr<SequenceEntry> entry, Specs scene, float fps) :
	m_entry(entry),
	m_scene(scene),
	m_fps(fps),
	m_sourceChannels(CHANNELS_INVALID),
	m_sourceRate(scene.rate),
	m_sourcePosition(0),
	m_sourcePositionValid(false)
{
	refreshSound();
}

void DuckingEntryRenderer::refreshSound()
{
	std::shared_ptr<ISound> sound;

	if(m_entry)
		sound = m_entry->getSound();

	if(sound == m_sound && m_reader)
		return;

	m_sound = sound;
	m_reader.reset();
	m_sourcePositionValid = false;
	m_sourceChannels = CHANNELS_INVALID;
	m_sourceRate = m_scene.rate;

	if(!sound)
		return;

	try
	{
		m_reader = sound->createReader();
	}
	catch(Exception&)
	{
		m_reader.reset();
	}

	if(m_reader)
	{
		Specs specs = m_reader->getSpecs();
		m_sourceChannels = specs.channels;
		m_sourceRate = specs.rate;
	}
}

void DuckingEntryRenderer::reset()
{
	m_sourcePositionValid = false;
	refreshSound();
}

void DuckingEntryRenderer::seekSource(long long frame)
{
	if(!m_reader)
		return;
	if(m_sourcePositionValid && frame == m_sourcePosition)
		return;

	m_reader->seek(int(frame));
	m_sourcePosition = frame;
	m_sourcePositionValid = true;
}

void DuckingEntryRenderer::mapFrame(const sample_t* source, sample_t* target) const
{
	if(!source || m_sourceChannels == CHANNELS_INVALID)
	{
		for(int c = 0; c < m_scene.channels; c++)
			target[c] = 0;
		return;
	}

	if(m_scene.channels == CHANNELS_MONO)
	{
		if(m_sourceChannels == CHANNELS_MONO)
			target[0] = source[0];
		else
			// Matches ChannelMapperReader: both source channels fold into mono.
			target[0] = source[0] + source[1];
		return;
	}

	if(m_scene.channels == CHANNELS_STEREO)
	{
		if(m_sourceChannels == CHANNELS_MONO)
			mapMonoToScene(source[0], m_scene.channels, target);
		else
		{
			target[0] = source[0];
			target[1] = source[1];
		}
		return;
	}

	for(int c = 0; c < m_scene.channels; c++)
		target[c] = c < m_sourceChannels ? source[c] : 0;
}

void DuckingEntryRenderer::render(long long startFrame, int frames, sample_t* buffer)
{
	const int channels = m_scene.channels;
	std::memset(buffer, 0, size_t(frames) * channels * sizeof(sample_t));

	if(frames <= 0 || !m_entry || m_fps <= 0 || m_scene.rate <= 0)
		return;

	refreshSound();

	if(!m_reader)
		return;

	// Snapshot the entry timing and volume.  The ducking detector only supports
	// positional entries with constant volume, which is exactly the case in
	// which these values are well defined.
	const double begin = m_entry->getBegin();
	const double end = m_entry->getEnd();
	const double skip = m_entry->getSkip();
	const float volume = m_entry->getAnimProperty(AP_VOLUME)->readSingle(0.0f);

	if(m_entry->isMuted() || volume == 0.0f)
		return;
	if(end >= 0.0 && !(begin < end))
		return;

	const long long beginFrame = (long long)std::llround(begin * m_scene.rate);
	long long endFrame = -1;
	if(end >= 0.0)
		endFrame = (long long)std::llround(end * m_scene.rate);

	const long long skipFrames = (long long)std::llround(skip * m_sourceRate);

	sample_t source[SOURCE_FRAME_SAMPLES];
	sample_t mapped[8];

	for(int i = 0; i < frames; i++)
	{
		const long long sceneFrame = startFrame + i;

		if(sceneFrame < beginFrame || (endFrame >= 0 && sceneFrame >= endFrame))
			continue;

		const long long sourceFrame = skipFrames + (sceneFrame - beginFrame);

		seekSource(sourceFrame);

		int len = 1;
		bool eos = false;
		std::memset(source, 0, sizeof(source));
		m_reader->read(len, eos, source);

		if(len < 1)
			// Past the end of the source, emit silence but keep the cursor so
			// that a later scene frame does not skip source data.
			continue;

		m_sourcePosition = sourceFrame + 1;
		m_sourcePositionValid = true;

		mapFrame(source, mapped);

		sample_t* out = buffer + size_t(i) * channels;
		for(int c = 0; c < channels; c++)
			out[c] = mapped[c] * volume;
	}
}

std::vector<DuckingRuleSnapshot> captureDuckingRules(const SequenceData& data, SampleRate rate)
{
	std::vector<DuckingRuleSnapshot> snapshots;

	for(const auto& rule : data.m_ducking)
	{
		if(!rule.target || !rule.key)
			continue;

		const DuckingSettings& settings = rule.settings;

		DuckingRuleParams params;
		params.threshold = settings.threshold;
		params.slope = 1.0f - 1.0f / settings.ratio;
		params.attack = settings.attack;
		params.release = settings.release;
		params.lookahead = settings.lookahead;
		params.attackCoefficient = settings.attack > 0.0f ? std::exp(-1.0 / (double(rate) * double(settings.attack))) : 0.0;
		params.releaseCoefficient = settings.release > 0.0f ? std::exp(-1.0 / (double(rate) * double(settings.release))) : 0.0;

		DuckingRuleSnapshot snapshot;
		snapshot.target = rule.target;
		snapshot.key = rule.key;
		snapshot.params = params;
		snapshots.push_back(snapshot);
	}

	return snapshots;
}

SequenceDuckingRuntime::SequenceDuckingRuntime(const DuckingRuleSnapshot& rule, Specs scene, float fps) :
	rule(rule),
	key(rule.key, scene, fps),
	target(rule.target, scene, fps),
	envelope(0.0),
	nextFrame(0),
	keyBegin(0),
	keyEnd(-1),
	keySkip(0),
	keyVolume(1.0f),
	keyMuted(false)
{
	updateKeySnapshot();
}

bool SequenceDuckingRuntime::keyChanged() const
{
	if(!rule.key)
		return false;

	if(rule.key->getBegin() != keyBegin || rule.key->getEnd() != keyEnd || rule.key->getSkip() != keySkip)
		return true;

	if(rule.key->isMuted() != keyMuted)
		return true;

	if(rule.key->getAnimProperty(AP_VOLUME)->readSingle(0.0f) != keyVolume)
		return true;

	if(rule.key->getSound() != keySound)
		return true;

	return rule.key->getAnimProperty(AP_VOLUME)->isAnimated();
}

void SequenceDuckingRuntime::updateKeySnapshot()
{
	if(!rule.key)
		return;

	keyBegin = rule.key->getBegin();
	keyEnd = rule.key->getEnd();
	keySkip = rule.key->getSkip();
	keyVolume = rule.key->getAnimProperty(AP_VOLUME)->readSingle(0.0f);
	keyMuted = rule.key->isMuted();
	keySound = rule.key->getSound();
}

namespace
{
	/// Validates the scalar settings of a ducking rule.
	void validateDuckingSettings(const DuckingSettings& settings)
	{
		if(!std::isfinite(settings.threshold) || !(settings.threshold > 0.0f))
			throw std::invalid_argument("Ducking threshold must be finite and greater than zero.");
		if(!std::isfinite(settings.ratio) || !(settings.ratio >= 1.0f))
			throw std::invalid_argument("Ducking ratio must be finite and at least one.");
		if(!std::isfinite(settings.attack) || !(settings.attack >= 0.0f))
			throw std::invalid_argument("Ducking attack must be finite and non-negative.");
		if(!std::isfinite(settings.release) || !(settings.release >= 0.0f))
			throw std::invalid_argument("Ducking release must be finite and non-negative.");
		if(settings.lookahead < 0 || settings.lookahead > 65536)
			throw std::invalid_argument("Ducking lookahead must be in the range [0, 65536].");
	}

	/// Compares two settings exactly.
	bool sameSettings(const DuckingSettings& a, const DuckingSettings& b)
	{
		return a.threshold == b.threshold && a.ratio == b.ratio && a.attack == b.attack && a.release == b.release && a.lookahead == b.lookahead;
	}
}

void SequenceData::setDucking(std::shared_ptr<SequenceEntry> target, std::shared_ptr<SequenceEntry> key, const DuckingSettings& settings)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);

	if(!target || !key)
		throw std::invalid_argument("Ducking entries must not be null.");
	if(target == key)
		throw std::invalid_argument("Ducking target and key must be different entries.");

	if(!isEntry(target) || !isEntry(key))
		throw std::invalid_argument("Ducking entries must be members of this sequence.");

	validateDuckingSettings(settings);

	for(auto& rule : m_ducking)
	{
		if(rule.target == target)
		{
			// Replacing a rule with an identical one leaves the reader state
			// untouched, so the envelope is not needlessly reset.
			if(rule.key != key || !sameSettings(rule.settings, settings))
			{
				rule.key = key;
				rule.settings = settings;
				m_ducking_status++;
			}
			return;
		}
	}

	SequenceDuckingRule rule;
	rule.target = target;
	rule.key = key;
	rule.settings = settings;
	m_ducking.push_back(rule);
	m_ducking_status++;
}

void SequenceData::clearDucking(std::shared_ptr<SequenceEntry> target)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);

	for(auto it = m_ducking.begin(); it != m_ducking.end(); ++it)
	{
		if(it->target == target)
		{
			m_ducking.erase(it);
			m_ducking_status++;
			return;
		}
	}
}

AUD_NAMESPACE_END

