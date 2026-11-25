/*******************************************************************************
 * Copyright 2009-2016 Jörg Müller
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

#include "sequence/SequenceReader.h"
#include "sequence/SequenceData.h"
#include "DuckingInternal.h"
#include "Exception.h"
#include "SequenceHandle.h"

#include <algorithm>
#include <mutex>
#include <cmath>
#include <vector>

AUD_NAMESPACE_BEGIN

namespace
{
	/// The block size used while replaying detector history.
	const long long DUCKING_REPLAY_BLOCK = 1024;
}

SequenceReader::SequenceReader(std::shared_ptr<SequenceData> sequence, ResampleQuality quality) :
	m_position(0), m_device(sequence->m_specs), m_sequence(sequence), m_status(0), m_entry_status(0),
	m_ducking_status(0), m_ducking_specs(sequence->m_specs)
{
	m_device.setQuality(quality);
}

SequenceReader::~SequenceReader()
{
}

bool SequenceReader::isSeekable() const
{
	return true;
}

void SequenceReader::seek(int position)
{
	if(position < 0)
		return;

	std::lock_guard<ILockable> lock(*m_sequence);

	m_position = position;

	for(auto& handle : m_handles)
	{
		handle->seek(position / (double)m_sequence->m_specs.rate);
	}

	// Ducking runtimes rebuild their envelope from the scene start on the next
	// read; a seek must not merely clear the state.
	for(auto& runtime : m_ducking)
	{
		runtime->envelope = 0.0;
		runtime->nextFrame = 0;
		runtime->key.reset();
		runtime->target.reset();
	}
}

int SequenceReader::getLength() const
{
	return -1;
}

int SequenceReader::getPosition() const
{
	return m_position;
}

Specs SequenceReader::getSpecs() const
{
	return m_sequence->m_specs;
}

void SequenceReader::read(int& length, bool& eos, sample_t* buffer)
{
	std::lock_guard<ILockable> lock(*m_sequence);

	if(m_sequence->m_status != m_status)
	{
		m_device.changeSpecs(m_sequence->m_specs);
		m_device.setSpeedOfSound(m_sequence->m_speed_of_sound);
		m_device.setDistanceModel(m_sequence->m_distance_model);
		m_device.setDopplerFactor(m_sequence->m_doppler_factor);

		m_status = m_sequence->m_status;
	}

	if(m_sequence->m_entry_status != m_entry_status)
	{
		rebuildHandles();
		m_entry_status = m_sequence->m_entry_status;
	}

	updateDucking();

	// A rule change can add or remove target entries, which are excluded from
	// the device mix and rendered separately.  Rebuild the handles when the
	// target set changed.
	if(duckingTargetsChanged())
	{
		refreshDuckingTargetIds();
		rebuildHandles();
	}

	Specs specs = m_sequence->m_specs;
	int pos = 0;
	double time = double(m_position) / double(specs.rate);
	float volume, frame;
	int len, cfra;
	Vector3 v, v2;
	Quaternion q;

	while(pos < length)
	{
		frame = time * m_sequence->m_fps;
		cfra = int(std::floor(frame));

		len = int(std::ceil((cfra + 1) / m_sequence->m_fps * specs.rate)) - m_position - pos;
		len = std::min(length - pos, len);
		len = std::max(len, 1);

		for(auto& handle : m_handles)
		{
			handle->update(time, frame, m_sequence->m_fps);
		}

		m_sequence->m_volume.read(frame, &volume);
		if(m_sequence->m_muted)
			volume = 0.0f;
		m_device.setVolume(volume);

		m_sequence->m_orientation.read(frame, q.get());
		m_device.setListenerOrientation(q);
		m_sequence->m_location.read(frame, v.get());
		m_device.setListenerLocation(v);
		m_sequence->m_location.read(frame + 1, v2.get());
		v2 -= v;
		m_device.setListenerVelocity(v2 * m_sequence->m_fps);

		m_device.read(reinterpret_cast<data_t*>(buffer + specs.channels * pos), len);

		// The scene volume is constant within a device block and already
		// applied to the device mix; the separately mixed ducked target
		// contribution uses the same value.
		if(!m_ducking.empty())
			mixDucking(m_position + pos, len, volume, buffer + specs.channels * pos);

		pos += len;
		time += double(len) / double(specs.rate);
	}

	m_position += length;

	eos = false;
}

void SequenceReader::rebuildHandles()
{
	std::list<std::shared_ptr<SequenceHandle> > handles;

	auto hit = m_handles.begin();
	auto eit = m_sequence->m_entries.begin();

	int result;
	std::shared_ptr<SequenceHandle> handle;

	while(hit != m_handles.end() && eit != m_sequence->m_entries.end())
	{
		handle = *hit;
		std::shared_ptr<SequenceEntry> entry = *eit;

		result = handle->compare(entry);

		if(result < 0)
		{
			if(!isDuckingTarget(entry))
			{
				try
				{
					handle = std::shared_ptr<SequenceHandle>(new SequenceHandle(entry, m_device));
					handles.push_back(handle);
				}
				catch(Exception&)
				{
				}
			}
			eit++;
		}
		else if(result == 0)
		{
			if(isDuckingTarget(entry))
				handle->stop();
			else
				handles.push_back(handle);
			hit++;
			eit++;
		}
		else
		{
			handle->stop();
			hit++;
		}
	}

	while(hit != m_handles.end())
	{
		(*hit)->stop();
		hit++;
	}

	while(eit != m_sequence->m_entries.end())
	{
		if(!isDuckingTarget(*eit))
		{
			try
			{
				handle = std::shared_ptr<SequenceHandle>(new SequenceHandle(*eit, m_device));
				handles.push_back(handle);
			}
			catch(Exception&)
			{
			}
		}
		eit++;
	}

	m_handles = handles;
}

bool SequenceReader::isDuckingTarget(const std::shared_ptr<SequenceEntry>& entry) const
{
	for(const auto& runtime : m_ducking)
	{
		if(runtime->rule.target == entry)
			return true;
	}

	return false;
}

bool SequenceReader::duckingTargetsChanged() const
{
	if(m_ducking_target_ids.size() != m_ducking.size())
		return true;

	for(size_t i = 0; i < m_ducking.size(); i++)
		if(m_ducking_target_ids[i] != m_ducking[i]->rule.target)
			return true;

	return false;
}

void SequenceReader::refreshDuckingTargetIds()
{
	m_ducking_target_ids.clear();

	for(const auto& runtime : m_ducking)
		m_ducking_target_ids.push_back(runtime->rule.target);
}

void SequenceReader::updateDucking()
{
	Specs specs = m_sequence->m_specs;

	bool specsChanged = !AUD_COMPARE_SPECS(specs, m_ducking_specs);

	if(!specsChanged && m_sequence->m_ducking_status == m_ducking_status)
	{
		// The rules did not change, but key timelines may have been edited
		// between two reads.  Detect that lazily in mixDucking.
		return;
	}

	std::vector<DuckingRuleSnapshot> rules = captureDuckingRules(*m_sequence, specs.rate);

	std::vector<std::shared_ptr<SequenceDuckingRuntime> > runtimes;
	runtimes.reserve(rules.size());

	for(auto& snapshot : rules)
	{
		std::shared_ptr<SequenceDuckingRuntime> existing;

		if(!specsChanged)
		{
			for(auto& runtime : m_ducking)
			{
				if(runtime->rule.target == snapshot.target)
				{
					existing = runtime;
					break;
				}
			}
		}

		if(existing && existing->rule.key == snapshot.key && sameRuleSettings(existing->rule.params, snapshot.params))
		{
			runtimes.push_back(existing);
		}
		else
		{
			runtimes.push_back(std::make_shared<SequenceDuckingRuntime>(snapshot, specs, m_sequence->m_fps));
		}
	}

	m_ducking = runtimes;
	m_ducking_specs = specs;
	m_ducking_status = m_sequence->m_ducking_status;
}

void SequenceReader::rebuildDucking(SequenceDuckingRuntime& runtime, long long frame)
{
	const int channels = m_ducking_specs.channels;
	const int lookahead = runtime.rule.params.lookahead;

	runtime.envelope = 0.0;
	runtime.nextFrame = 0;
	runtime.key.reset();
	runtime.target.reset();

	const long long window = (long long)DUCKING_REPLAY_BLOCK * channels;
	if((long long)m_ducking_key_buffer.size() < window)
		m_ducking_key_buffer.resize(window);

	long long position = 0;

	while(position < frame)
	{
		const int block = int(std::min<long long>(DUCKING_REPLAY_BLOCK, frame - position));

		runtime.key.render(position + lookahead, block, m_ducking_key_buffer.data());

		for(int i = 0; i < block; i++)
		{
			float amplitude = 0.0f;
			const sample_t* frameData = m_ducking_key_buffer.data() + size_t(i) * channels;

			for(int c = 0; c < channels; c++)
				amplitude = std::max(amplitude, std::fabs(frameData[c]));

			runtime.envelope = duckingStep(runtime.envelope, duckingTargetGain(amplitude, runtime.rule.params), runtime.rule.params);
		}

		position += block;
	}

	runtime.nextFrame = frame;
}

void SequenceReader::mixDucking(long long sceneStart, int length, float volume, sample_t* buffer)
{
	if(length <= 0)
		return;

	const int channels = m_ducking_specs.channels;
	const long long needed = (long long)std::max(length, (int)DUCKING_REPLAY_BLOCK) * channels;

	if((long long)m_ducking_key_buffer.size() < needed)
		m_ducking_key_buffer.resize(needed);
	if((long long)m_ducking_target_buffer.size() < needed)
		m_ducking_target_buffer.resize(needed);

	for(auto& runtime : m_ducking)
	{
		const DuckingRuleParams& params = runtime->rule.params;

		if(runtime->keyChanged())
		{
			runtime->updateKeySnapshot();
			rebuildDucking(*runtime, sceneStart);
		}
		else if(runtime->nextFrame != sceneStart)
		{
			rebuildDucking(*runtime, sceneStart);
		}

		runtime->key.render(sceneStart + params.lookahead, length, m_ducking_key_buffer.data());
		runtime->target.render(sceneStart, length, m_ducking_target_buffer.data());

		sample_t* out = buffer;

		for(int i = 0; i < length; i++)
		{
			const sample_t* keyFrame = m_ducking_key_buffer.data() + size_t(i) * channels;
			const sample_t* targetFrame = m_ducking_target_buffer.data() + size_t(i) * channels;

			float amplitude = 0.0f;
			for(int c = 0; c < channels; c++)
				amplitude = std::max(amplitude, std::fabs(keyFrame[c]));

			runtime->envelope = duckingStep(runtime->envelope, duckingTargetGain(amplitude, params), params);
			runtime->nextFrame = sceneStart + i + 1;

			const sample_t gain = sample_t(std::exp(runtime->envelope)) * volume;

			for(int c = 0; c < channels; c++)
				out[size_t(i) * channels + c] += targetFrame[c] * gain;
		}
	}
}

AUD_NAMESPACE_END
