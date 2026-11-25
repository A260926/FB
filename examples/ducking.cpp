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

/**
 * @file ducking.cpp
 * @ingroup demos
 * A sound-card free demonstration of per-entry look-ahead ducking.
 *
 * The program renders a short stereo scene in memory: a music bed and a
 * narration entry.  The narration is used as the ducking key, so the music is
 * attenuated with a look-ahead while the narration is active.  The rendered
 * PCM is written to the single path given on the command line as interleaved
 * 32 bit float raw samples.
 */

#include "sequence/Ducking.h"
#include "sequence/Sequence.h"
#include "sequence/SequenceEntry.h"
#include "ISound.h"
#include "IReader.h"
#include "respec/Specification.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

using namespace aud;

namespace
{
	const SampleRate RATE = 8000;
	const float FPS = 25.0f;

	/// A small synthesised source with a configurable waveform.
	class ToneReader : public IReader
	{
	private:
		long long m_position;
		float m_amplitude;
		double m_frequency;
		double m_phase;
		int m_channels;

	public:
		ToneReader(float amplitude, double frequency, double phase, int channels) :
			m_position(0), m_amplitude(amplitude), m_frequency(frequency), m_phase(phase), m_channels(channels)
		{
		}

		bool isSeekable() const override { return true; }
		void seek(int position) override { m_position = position < 0 ? 0 : position; }
		int getLength() const override { return 1 << 20; }
		int getPosition() const override { return int(m_position); }
		Specs getSpecs() const override { return {RATE, Channels(m_channels)}; }

		void read(int& length, bool& eos, sample_t* buffer) override
		{
			for(int i = 0; i < length; i++)
			{
				const double t = double(m_position + i) / double(RATE);

				for(int c = 0; c < m_channels; c++)
				{
					const double value = std::sin(2.0 * M_PI * m_frequency * t + m_phase + 0.4 * c);
					buffer[size_t(i) * m_channels + c] = m_amplitude * float(value);
				}
			}

			m_position += length;
			eos = false;
		}
	};

	class ToneSound : public ISound
	{
	private:
		float m_amplitude;
		double m_frequency;
		double m_phase;
		int m_channels;

	public:
		ToneSound(float amplitude, double frequency, double phase, int channels) :
			m_amplitude(amplitude), m_frequency(frequency), m_phase(phase), m_channels(channels)
		{
		}

		std::shared_ptr<IReader> createReader() override
		{
			return std::make_shared<ToneReader>(m_amplitude, m_frequency, m_phase, m_channels);
		}
	};
}

int main(int argc, char* argv[])
{
	if(argc != 2)
	{
		std::cerr << "Usage: " << argv[0] << " <output.raw>" << std::endl;
		return 1;
	}

	const int frames = 2048;

	// The scene has a stereo output at the same rate as both sources.
	Sequence scene({RATE, CHANNELS_STEREO}, FPS, false);

	// A music bed lasting the whole scene, and a narration in the middle.
	auto music = scene.add(std::make_shared<ToneSound>(0.30f, 110.0, 0.0, 2), 0.0, 0.5, 0.0);
	auto narration = scene.add(std::make_shared<ToneSound>(0.22f, 440.0, 0.7, 1), 0.04, 0.28, 0.0);
	auto silent = scene.add(std::make_shared<ToneSound>(0.0f, 1.0, 0.0, 2), 0.0, 0.5, 0.0);

	// The narration ducks the music.  The look-ahead lets the envelope fall
	// before the narration itself becomes audible.
	DuckingSettings ducking;
	ducking.threshold = 0.05f;
	ducking.ratio = 4.0f;
	ducking.attack = 0.004f;
	ducking.release = 0.06f;
	ducking.lookahead = 64;
	scene.setDucking(music, narration, ducking);

	// Keep the narration itself un-ducked; the silent entry is only there to
	// show that rules can be laid over entries that produce no audible signal.
	scene.setDucking(narration, silent, ducking);

	auto reader = scene.createReader();

	std::vector<float> pcm(size_t(frames) * 2);
	int remaining = frames;
	bool eos = false;
	int offset = 0;

	while(remaining > 0)
	{
		int n = remaining;
		reader->read(n, eos, pcm.data() + size_t(offset) * 2);
		offset += n;
		remaining -= n;
	}

	std::ofstream file(argv[1], std::ios::binary);

	if(!file)
	{
		std::cerr << "Could not open " << argv[1] << " for writing." << std::endl;
		return 2;
	}

	file.write(reinterpret_cast<const char*>(pcm.data()), std::streamsize(pcm.size() * sizeof(float)));

	if(!file)
	{
		std::cerr << "Could not write the output file." << std::endl;
		return 3;
	}

	std::cout << "Wrote " << frames << " stereo frames to " << argv[1] << "." << std::endl;

	return 0;
}
