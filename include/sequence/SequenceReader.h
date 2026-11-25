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

#pragma once

/**
 * @file SequenceReader.h
 * @ingroup sequence
 * The SequenceReader class.
 */

#include "IReader.h"
#include "devices/ReadDevice.h"
#include "respec/Specification.h"

#include <memory>
#include <vector>

AUD_NAMESPACE_BEGIN

class SequenceHandle;
class SequenceData;
class SequenceEntry;
struct SequenceDuckingRuntime;

/**
 * This reader plays back sequenced entries.
 */
class AUD_API SequenceReader : public IReader
{
private:
	/**
	 * The current position.
	 */
	int m_position;

	/**
	 * The read device used to mix the sounds correctly.
	 */
	ReadDevice m_device;

	/**
	 * Saves the sequence the reader belongs to.
	 */
	std::shared_ptr<SequenceData> m_sequence;

	/**
	 * The list of playback handles for the entries.
	 */
	std::list<std::shared_ptr<SequenceHandle> > m_handles;

	/**
	 * Last status read from the sequence.
	 */
	int m_status;

	/**
	 * Last entry status read from the sequence.
	 */
	int m_entry_status;

	/**
	 * Last ducking status read from the sequence.
	 */
	int m_ducking_status;

	/**
	 * Independent ducking runtime, one per rule (and therefore per target).
	 */
	std::vector<std::shared_ptr<SequenceDuckingRuntime> > m_ducking;

	/**
	 * The scene specification the ducking runtime was built for.
	 */
	Specs m_ducking_specs;

	/**
	 * The target entries of the current runtime, in runtime order, used to
	 * detect target set changes.
	 */
	std::vector<std::shared_ptr<SequenceEntry> > m_ducking_target_ids;

	/**
	 * Scratch buffers reused between reads.
	 */
	std::vector<sample_t> m_ducking_key_buffer;
	std::vector<sample_t> m_ducking_target_buffer;

	/**
	 * Rebuilds the playback handles, leaving out ducking targets whose
	 * contribution is mixed separately.
	 */
	void rebuildHandles();

	/**
	 * Returns whether an entry is the target of a ducking rule.
	 */
	bool isDuckingTarget(const std::shared_ptr<SequenceEntry>& entry) const;

	/**
	 * Returns whether the set of ducking target entries changed.
	 */
	bool duckingTargetsChanged() const;

	/**
	 * Updates the stored ducking target set.
	 */
	void refreshDuckingTargetIds();

	/**
	 * Synchronizes the ducking runtime with the current rules.
	 */
	void updateDucking();

	/**
	 * Rebuilds one runtime's envelope from the scene start up to a frame.
	 */
	void rebuildDucking(SequenceDuckingRuntime& runtime, long long frame);

	/**
	 * Advances all runtimes through a scene block and adds the ducked target
	 * contributions.
	 */
	void mixDucking(long long sceneStart, int length, float volume, sample_t* buffer);

	// delete copy constructor and operator=
	SequenceReader(const SequenceReader&) = delete;
	SequenceReader& operator=(const SequenceReader&) = delete;

public:
	/**
	 * Creates a resampling reader.
	 * \param sequence The sequence data.
	 * \param quality Resampling quality vs performance option.
	 */
	SequenceReader(std::shared_ptr<SequenceData> sequence, ResampleQuality quality = ResampleQuality::FASTEST);

	/**
	 * Destroys the reader.
	 */
	~SequenceReader();

	virtual bool isSeekable() const;
	virtual void seek(int position);
	virtual int getLength() const;
	virtual int getPosition() const;
	virtual Specs getSpecs() const;
	virtual void read(int& length, bool& eos, sample_t* buffer);
};

AUD_NAMESPACE_END
