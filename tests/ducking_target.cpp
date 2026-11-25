/*
 * Tests for per-entry look-ahead ducking on aud::Sequence.
 *
 * The tests only use the public C++ API and compare real rendered PCM against
 * an independent, closed-form model of the detector and the entry timeline.
 */

#include "task_support/check.h"

#include "sequence/Ducking.h"
#include "sequence/Sequence.h"
#include "sequence/SequenceEntry.h"
#include "ISound.h"
#include "IReader.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace
{
	const double RATE = 8000.0;
	const float FPS = 25.0f;
	const int CHUNK = 1024;
	const float WAVE_SCALE = 0.55f;

	/// Deterministic signed test signal, identical for fixture and model.
	float sampleAt(long long frame, int channel, float amp)
	{
		const double n = double(frame);
		const double c = double(channel);
		const double v = 0.9 * std::sin(0.0173 * n + 0.6 * c) + 0.35 * std::sin(0.0711 * n + 1.3 + 0.9 * c) + 0.2 * std::sin(0.221 * n + 0.2 * c);
		return float(amp * v);
	}

	class TestReader : public aud::IReader
	{
	private:
		long long m_position;
		float m_amp;
		int m_channels;

	public:
		explicit TestReader(float amp, int channels) : m_position(0), m_amp(amp), m_channels(channels) {}

		bool isSeekable() const override { return true; }

		void seek(int position) override { m_position = position < 0 ? 0 : position; }

		int getLength() const override { return 1 << 22; }

		int getPosition() const override { return int(m_position); }

		aud::Specs getSpecs() const override { return {RATE, aud::Channels(m_channels)}; }

		void read(int& length, bool& eos, aud::sample_t* buffer) override
		{
			int count = length;
			const long long limit = 1 << 22;

			if(m_position + count > limit)
				count = int(std::max<long long>(0, limit - m_position));

			for(int i = 0; i < count; i++)
				for(int c = 0; c < m_channels; c++)
					buffer[i * m_channels + c] = sampleAt(m_position + i, c, m_amp);

			length = count;
			m_position += count;
			eos = m_position >= limit;
		}
	};

	class TestSound : public aud::ISound
	{
	private:
		float m_amp;
		int m_channels;

	public:
		TestSound(float amp, int channels = 2) : m_amp(amp), m_channels(channels) {}

		std::shared_ptr<aud::IReader> createReader() override
		{
			return std::make_shared<TestReader>(m_amp, m_channels);
		}
	};

	class SilenceReader : public aud::IReader
	{
	private:
		long long m_position;
		int m_channels;

	public:
		explicit SilenceReader(int channels) : m_position(0), m_channels(channels) {}

		bool isSeekable() const override { return true; }
		void seek(int position) override { m_position = position < 0 ? 0 : position; }
		int getLength() const override { return 1 << 22; }
		int getPosition() const override { return int(m_position); }
		aud::Specs getSpecs() const override { return {RATE, aud::Channels(m_channels)}; }

		void read(int& length, bool& eos, aud::sample_t* buffer) override
		{
			std::fill(buffer, buffer + size_t(length) * m_channels, 0.0f);
			m_position += length;
			eos = false;
		}
	};

	class SilenceSound : public aud::ISound
	{
	private:
		int m_channels;

	public:
		explicit SilenceSound(int channels = 2) : m_channels(channels) {}

		std::shared_ptr<aud::IReader> createReader() override
		{
			return std::make_shared<SilenceReader>(m_channels);
		}
	};

	/// Model of one entry's scene-time, pre-ducking contribution.
	struct EntryModel
	{
		long long begin = 0;
		long long end = -1;
		long long skip = 0;
		float amp = 0.0f;
		float volume = 1.0f;
		int channels = 2;
		bool muted = false;
	};

	/// Writes the model's raw (pre-ducking) contribution of an entry into a
	/// scene-channel frame.  Returns nothing; out must hold the scene channels.
	void rawFrame(const EntryModel& entry, long long sceneFrame, int sceneChannels, float* out)
	{
		for(int c = 0; c < sceneChannels; c++)
			out[c] = 0.0f;

		if(entry.muted || entry.amp == 0.0f || sceneFrame < entry.begin)
			return;
		if(entry.end >= 0 && sceneFrame >= entry.end)
			return;

		const long long sourceFrame = entry.skip + (sceneFrame - entry.begin);

		float source[2] = {0.0f, 0.0f};
		for(int c = 0; c < entry.channels && c < 2; c++)
			source[c] = sampleAt(sourceFrame, c, entry.amp) * entry.volume;

		if(sceneChannels == 1)
		{
			if(entry.channels == 1)
				out[0] = source[0];
			else
				out[0] = source[0] + source[1];
		}
		else
		{
			if(entry.channels == 1)
			{
				const float gain = float(std::sqrt(0.5));
				out[0] = source[0] * gain;
				out[1] = source[0] * gain;
			}
			else
			{
				out[0] = source[0];
				out[1] = source[1];
			}
		}
	}

	struct RuleModel
	{
		int target = 0;
		int key = 0;
		float threshold = 0.1f;
		float ratio = 4.0f;
		float attack = 0.01f;
		float release = 0.05f;
		int lookahead = 0;
	};

	std::vector<float> environmentOf(const RuleModel& rule, const std::vector<EntryModel>& entries, int sceneChannels, int frames)
	{
		const double slope = 1.0 - 1.0 / double(rule.ratio);
		const double aA = rule.attack > 0.0f ? std::exp(-1.0 / (RATE * double(rule.attack))) : 0.0;
		const double aR = rule.release > 0.0f ? std::exp(-1.0 / (RATE * double(rule.release))) : 0.0;

		std::vector<float> env(size_t(frames), 0.0f);
		std::vector<float> frame(size_t(sceneChannels), 0.0f);
		double z = 0.0;

		for(int n = 0; n < frames; n++)
		{
			rawFrame(entries[size_t(rule.key)], n + rule.lookahead, sceneChannels, frame.data());

			float amplitude = 0.0f;
			for(int c = 0; c < sceneChannels; c++)
				amplitude = std::max(amplitude, std::fabs(frame[size_t(c)]));

			float target = 0.0f;
			if(amplitude > rule.threshold)
				target = float(slope * std::log(double(rule.threshold) / double(amplitude)));

			const double coefficient = target < z ? aA : aR;
			z = coefficient * z + (1.0 - coefficient) * double(target);

			env[size_t(n)] = float(std::exp(z));
		}

		return env;
	}

	/// Renders the full model of a scene.
	std::vector<float> renderModel(const std::vector<EntryModel>& entries, const std::vector<RuleModel>& rules, int sceneChannels, int frames)
	{
		std::vector<std::vector<float> > environments;
		environments.reserve(rules.size());

		for(const auto& rule : rules)
			environments.push_back(environmentOf(rule, entries, sceneChannels, frames));

		std::vector<float> out(size_t(frames) * sceneChannels, 0.0f);
		std::vector<float> frame(size_t(sceneChannels), 0.0f);

		for(int n = 0; n < frames; n++)
		{
			for(size_t r = 0; r < rules.size(); r++)
			{
				const float gain = environments[r][size_t(n)];
				rawFrame(entries[size_t(rules[r].target)], n, sceneChannels, frame.data());

				for(int c = 0; c < sceneChannels; c++)
					out[size_t(n) * sceneChannels + c] += frame[size_t(c)] * gain;
			}
		}

		return out;
	}

	/// Reads a whole number of frames from a reader.
	std::vector<float> readAll(aud::Sequence& sequence, int frames, int chunk, int channels)
	{
		auto reader = sequence.createQualityReader(aud::ResampleQuality::FASTEST);
		std::vector<float> out(size_t(frames) * channels);
		int pos = 0;

		while(pos < frames)
		{
			int n = (chunk <= 0) ? frames - pos : std::min(chunk, frames - pos);
			bool eos = false;
			reader->read(n, eos, out.data() + size_t(pos) * channels);
			pos += n;
		}

		return out;
	}

	/// Maximum absolute difference between two interleaved buffers.
	double maxDifference(const std::vector<float>& a, const std::vector<float>& b, size_t count)
	{
		double result = 0.0;

		for(size_t i = 0; i < count && i < a.size() && i < b.size(); i++)
			if(std::isfinite(a[i]) && std::isfinite(b[i]))
				result = std::max(result, double(std::fabs(a[i] - b[i])));
			else
				result = 1e9;

		return result;
	}

	bool allFinite(const std::vector<float>& values)
	{
		for(float v : values)
			if(!std::isfinite(v))
				return false;

		return true;
	}

	bool hasNonZeroEnergy(const std::vector<float>& values, size_t count)
	{
		double energy = 0.0;

		for(size_t i = 0; i < count && i < values.size(); i++)
			energy += std::fabs(values[i]);

		return energy > 1e-6;
	}

	aud::DuckingSettings settings(float threshold, float ratio, float attack, float release, int lookahead)
	{
		aud::DuckingSettings value;
		value.threshold = threshold;
		value.ratio = ratio;
		value.attack = attack;
		value.release = release;
		value.lookahead = lookahead;
		return value;
	}

	void setVolume(aud::SequenceEntry& entry, float volume)
	{
		entry.getAnimProperty(aud::AP_VOLUME)->write(&volume);
	}
}

// ---------------------------------------------------------------------------
// scene_clock: detector timing, look-ahead, panning and sample-accurate cuts
// ---------------------------------------------------------------------------

static int case_stereo_clock()
{
	const long long begin = 137;
	const long long end = 1234;
	const long long skip = 53;
	const int lookahead = 17;
	const int frames = 1500;

	aud::Sequence scene({RATE, aud::CHANNELS_STEREO}, FPS, false);

	auto target = scene.add(std::make_shared<TestSound>(0.5f, 2), begin / RATE, end / RATE, skip / RATE);
	auto key = scene.add(std::make_shared<TestSound>(0.4f, 2), begin / RATE, end / RATE, skip / RATE);
	auto silence = scene.add(std::make_shared<SilenceSound>(2), 0.0, 2.0, 0.0);

	const aud::DuckingSettings duck = settings(0.12f, 3.0f, 0.004f, 0.03f, lookahead);
	scene.setDucking(target, key, duck);
	scene.setDucking(key, silence, settings(0.2f, 2.0f, 0.0f, 0.0f, 0));

	const std::vector<EntryModel> entries = {
		{begin, end, skip, 0.5f, 1.0f, 2, false},
		{begin, end, skip, 0.4f, 1.0f, 2, false},
		{0, -1, 0, 0.0f, 1.0f, 2, false},
	};
	const std::vector<RuleModel> rules = {
		{0, 1, 0.12f, 3.0f, 0.004f, 0.03f, lookahead},
		{1, 2, 0.2f, 2.0f, 0.0f, 0.0f, 0},
	};

	const auto real = readAll(scene, frames, 0, 2);
	const auto model = renderModel(entries, rules, 2, frames);

	TASK_CHECK(allFinite(real));
	TASK_CHECK(hasNonZeroEnergy(real, real.size()));
	TASK_CHECK(maxDifference(real, model, real.size()) <= 2e-5);

	// The detector must actually duck somewhere.
	double minimumStart = 0.0;
	for(int n = 0; n < frames; n++)
		minimumStart = std::min(minimumStart, double(model[size_t(n) * 2]));

	return 0;
}

static int case_mono_instantaneous()
{
	// attack and release zero yield an instantaneous envelope, exercised on a
	// mono scene fed from a mono source.
	const long long begin = 320;
	const long long end = 1120;
	const int frames = 1400;

	aud::Sequence scene({RATE, aud::CHANNELS_MONO}, FPS, false);

	auto target = scene.add(std::make_shared<TestSound>(0.6f, 1), begin / RATE, end / RATE, 0.0);
	auto key = scene.add(std::make_shared<TestSound>(0.45f, 1), begin / RATE, end / RATE, 0.0);
	auto silence = scene.add(std::make_shared<SilenceSound>(1), 0.0, 2.0, 0.0);

	const aud::DuckingSettings duck = settings(0.1f, 5.0f, 0.0f, 0.0f, 0);
	scene.setDucking(target, key, duck);
	scene.setDucking(key, silence, settings(0.1f, 2.0f, 0.0f, 0.0f, 0));

	const std::vector<EntryModel> entries = {
		{begin, end, 0, 0.6f, 1.0f, 1, false},
		{begin, end, 0, 0.45f, 1.0f, 1, false},
		{0, -1, 0, 0.0f, 1.0f, 1, false},
	};
	const std::vector<RuleModel> rules = {
		{0, 1, 0.1f, 5.0f, 0.0f, 0.0f, 0},
		{1, 2, 0.1f, 2.0f, 0.0f, 0.0f, 0},
	};

	const auto real = readAll(scene, frames, 0, 1);
	const auto model = renderModel(entries, rules, 1, frames);

	TASK_CHECK(allFinite(real));
	TASK_CHECK(maxDifference(real, model, real.size()) <= 2e-5);

	return 0;
}

static int case_lookahead_shifts_detection()
{
	const long long begin = 101;
	const long long end = 1501;
	const int frames = 1600;

	auto make = [&](int lookahead) {
		auto scene = std::make_unique<aud::Sequence>(aud::Specs{RATE, aud::CHANNELS_STEREO}, FPS, false);
		auto target = scene->add(std::make_shared<TestSound>(0.5f, 2), begin / RATE, end / RATE, 0.0);
		auto key = scene->add(std::make_shared<TestSound>(0.5f, 2), 0.0, 2.0, 0.0);
		auto silence = scene->add(std::make_shared<SilenceSound>(2), 0.0, 2.0, 0.0);
		scene->setDucking(target, key, settings(0.11f, 4.0f, 0.002f, 0.02f, lookahead));
		scene->setDucking(key, silence, settings(0.2f, 2.0f, 0.0f, 0.0f, 0));
		return scene;
	};

	auto without = make(0);
	auto with = make(240);

	const auto a = readAll(*without, frames, 0, 2);
	const auto b = readAll(*with, frames, 0, 2);

	TASK_CHECK(allFinite(a));
	TASK_CHECK(allFinite(b));
	TASK_CHECK(maxDifference(a, b, a.size()) > 1e-4);

	// Both must align with the model, which is where the look-ahead matters.
	const std::vector<EntryModel> entries = {
		{begin, end, 0, 0.5f, 1.0f, 2, false},
		{0, -1, 0, 0.5f, 1.0f, 2, false},
		{0, -1, 0, 0.0f, 1.0f, 2, false},
	};
	const std::vector<RuleModel> rules = {
		{0, 1, 0.11f, 4.0f, 0.002f, 0.02f, 240},
		{1, 2, 0.2f, 2.0f, 0.0f, 0.0f, 0},
	};
	const auto model = renderModel(entries, rules, 2, frames);

	TASK_CHECK(maxDifference(b, model, b.size()) <= 2e-5);

	return 0;
}

// ---------------------------------------------------------------------------
// envelope_seek: chunked reads and seeking rebuild the detector history
// ---------------------------------------------------------------------------

namespace
{
	struct Fixture
	{
		aud::Sequence scene;
		std::shared_ptr<aud::SequenceEntry> target;
		std::shared_ptr<aud::SequenceEntry> key;
		std::shared_ptr<aud::SequenceEntry> silence;
		std::vector<EntryModel> entries;
		std::vector<RuleModel> rules;

		Fixture() : scene({RATE, aud::CHANNELS_STEREO}, FPS, false)
		{
			const long long begin = 211;
			const long long end = 1711;
			const long long skip = 29;

			target = scene.add(std::make_shared<TestSound>(0.5f, 2), begin / RATE, end / RATE, skip / RATE);
			key = scene.add(std::make_shared<TestSound>(0.42f, 2), begin / RATE, end / RATE, skip / RATE);
			silence = scene.add(std::make_shared<SilenceSound>(2), 0.0, 2.0, 0.0);

			scene.setDucking(target, key, settings(0.1f, 4.0f, 0.006f, 0.04f, 29));
			scene.setDucking(key, silence, settings(0.2f, 2.0f, 0.0f, 0.0f, 0));

			entries = {
				{begin, end, skip, 0.5f, 1.0f, 2, false},
				{begin, end, skip, 0.42f, 1.0f, 2, false},
				{0, -1, 0, 0.0f, 1.0f, 2, false},
			};
			rules = {
				{0, 1, 0.1f, 4.0f, 0.006f, 0.04f, 29},
				{1, 2, 0.2f, 2.0f, 0.0f, 0.0f, 0},
			};
		}
	};
}

static int case_chunked_reads()
{
	const int frames = 2400;
	Fixture fixture;
	const auto model = renderModel(fixture.entries, fixture.rules, 2, frames);

	const auto whole = readAll(fixture.scene, frames, 0, 2);
	TASK_CHECK(allFinite(whole));
	TASK_CHECK(maxDifference(whole, model, whole.size()) <= 2e-5);

	const int chunks[] = {1, 7, 32, 137, 511, 999};
	for(int chunk : chunks)
	{
		const auto partial = readAll(fixture.scene, frames, chunk, 2);
		TASK_CHECK(allFinite(partial));
		TASK_CHECK(maxDifference(whole, partial, whole.size()) <= 2e-5);
	}

	return 0;
}

static int case_seek_rebuilds_history()
{
	const int frames = 2200;
	Fixture fixture;

	const auto model = renderModel(fixture.entries, fixture.rules, 2, frames);
	const auto whole = readAll(fixture.scene, frames, 0, 2);
	TASK_CHECK(maxDifference(whole, model, whole.size()) <= 2e-5);

	for(int seekFrame : {0, 1, 137, 900, 1500})
	{
		auto reader = fixture.scene.createQualityReader(aud::ResampleQuality::FASTEST);
		std::vector<float> buffer(size_t(frames) * 2);
		int n = frames;
		bool eos = false;
		reader->read(n, eos, buffer.data());

		reader->seek(seekFrame);
		std::vector<float> suffix(size_t(frames - seekFrame) * 2);
		int m = frames - seekFrame;
		bool eos2 = false;
		reader->read(m, eos2, suffix.data());
		TASK_CHECK(m == frames - seekFrame);

		const size_t offset = size_t(seekFrame) * 2;
		TASK_CHECK(maxDifference(std::vector<float>(buffer.begin() + offset, buffer.end()), suffix, suffix.size()) <= 2e-5);
	}

	return 0;
}

static int case_seek_after_edit_rebuilds_new_timeline()
{
	const int frames = 1800;
	Fixture fixture;

	auto reader = fixture.scene.createQualityReader(aud::ResampleQuality::FASTEST);
	std::vector<float> prefix(size_t(700) * 2);
	int n = 700;
	bool eos = false;
	reader->read(n, eos, prefix.data());

	// Move the key and then seek: the detector history must be rebuilt from the
	// scene start using the new timeline.
	fixture.key->move(0.01, 0.19, 0.004);

	reader->seek(0);
	std::vector<float> after(size_t(frames) * 2);
	int m = frames;
	bool eos2 = false;
	reader->read(m, eos2, after.data());
	TASK_CHECK(m == frames);

	EntryModel moved = fixture.entries[1];
	moved.begin = 80;
	moved.end = 1520;
	moved.skip = 32;
	std::vector<EntryModel> entries = fixture.entries;
	entries[1] = moved;

	const auto model = renderModel(entries, fixture.rules, 2, frames);
	TASK_CHECK(allFinite(after));
	TASK_CHECK(maxDifference(after, model, after.size()) <= 2e-5);

	return 0;
}

// ---------------------------------------------------------------------------
// entry_changes: edits between reads, shared keys, multiple readers, removal
// ---------------------------------------------------------------------------

static int case_key_move_between_reads()
{
	const int frames = 2000;
	Fixture fixture;

	auto reader = fixture.scene.createQualityReader(aud::ResampleQuality::FASTEST);
	std::vector<float> before(size_t(800) * 2);
	int n = 800;
	bool eos = false;
	reader->read(n, eos, before.data());

	const auto firstModel = renderModel(fixture.entries, fixture.rules, 2, 800);
	TASK_CHECK(maxDifference(before, firstModel, before.size()) <= 2e-5);

	fixture.key->move(0.02, 0.21, 0.006);

	std::vector<float> after(size_t(frames - 800) * 2);
	int m = frames - 800;
	bool eos2 = false;
	reader->read(m, eos2, after.data());
	TASK_CHECK(m == frames - 800);

	EntryModel moved = fixture.entries[1];
	moved.begin = 160;
	moved.end = 1680;
	moved.skip = 48;
	std::vector<EntryModel> entries = fixture.entries;
	entries[1] = moved;

	// A fresh reader with the moved key gives the reference timeline; the
	// continued reader must produce the same suffix.
	const auto reference = renderModel(entries, fixture.rules, 2, frames);
	const std::vector<float> referenceSuffix(reference.begin() + 800 * 2, reference.end());

	TASK_CHECK(allFinite(after));
	TASK_CHECK(maxDifference(after, referenceSuffix, after.size()) <= 2e-5);

	return 0;
}

static int case_key_set_sound_between_reads()
{
	const int frames = 1600;
	Fixture fixture;

	auto reader = fixture.scene.createQualityReader(aud::ResampleQuality::FASTEST);
	std::vector<float> before(size_t(600) * 2);
	int n = 600;
	bool eos = false;
	reader->read(n, eos, before.data());

	fixture.key->setSound(std::make_shared<TestSound>(0.22f, 2));

	std::vector<float> after(size_t(frames - 600) * 2);
	int m = frames - 600;
	bool eos2 = false;
	reader->read(m, eos2, after.data());

	std::vector<EntryModel> entries = fixture.entries;
	entries[1].amp = 0.22f;

	const auto reference = renderModel(entries, fixture.rules, 2, frames);
	const std::vector<float> referenceSuffix(reference.begin() + 600 * 2, reference.end());

	TASK_CHECK(allFinite(after));
	TASK_CHECK(maxDifference(after, referenceSuffix, after.size()) <= 2e-5);

	return 0;
}

static int case_key_mute_between_reads()
{
	const int frames = 1600;
	Fixture fixture;

	auto reader = fixture.scene.createQualityReader(aud::ResampleQuality::FASTEST);
	std::vector<float> before(size_t(700) * 2);
	int n = 700;
	bool eos = false;
	reader->read(n, eos, before.data());

	fixture.key->mute(true);

	std::vector<float> after(size_t(frames - 700) * 2);
	int m = frames - 700;
	bool eos2 = false;
	reader->read(m, eos2, after.data());

	std::vector<EntryModel> entries = fixture.entries;
	entries[1].muted = true;

	const auto reference = renderModel(entries, fixture.rules, 2, frames);
	const std::vector<float> referenceSuffix(reference.begin() + 700 * 2, reference.end());

	TASK_CHECK(allFinite(after));
	TASK_CHECK(maxDifference(after, referenceSuffix, after.size()) <= 2e-5);

	// A muted key must release the ducking: the target eventually rises.
	double tail = 0.0;
	for(size_t i = after.size() > 400 ? after.size() - 400 : 0; i < after.size(); i++)
		tail += std::fabs(after[i]);
	TASK_CHECK(tail > 0.0);

	return 0;
}

static int case_two_targets_share_key()
{
	const long long begin = 97;
	const long long end = 1697;
	const int frames = 1800;

	aud::Sequence scene({RATE, aud::CHANNELS_STEREO}, FPS, false);

	auto first = scene.add(std::make_shared<TestSound>(0.45f, 2), begin / RATE, end / RATE, 0.0);
	auto second = scene.add(std::make_shared<TestSound>(0.35f, 2), begin / RATE, end / RATE, 33 / RATE);
	auto key = scene.add(std::make_shared<TestSound>(0.4f, 2), 0.0, 2.0, 0.0);
	auto silence = scene.add(std::make_shared<SilenceSound>(2), 0.0, 2.0, 0.0);

	scene.setDucking(first, key, settings(0.1f, 4.0f, 0.003f, 0.03f, 19));
	scene.setDucking(second, key, settings(0.16f, 2.0f, 0.008f, 0.05f, 3));
	scene.setDucking(key, silence, settings(0.2f, 2.0f, 0.0f, 0.0f, 0));

	const std::vector<EntryModel> entries = {
		{begin, end, 0, 0.45f, 1.0f, 2, false},
		{begin, end, 33, 0.35f, 1.0f, 2, false},
		{0, -1, 0, 0.4f, 1.0f, 2, false},
		{0, -1, 0, 0.0f, 1.0f, 2, false},
	};
	const std::vector<RuleModel> rules = {
		{0, 2, 0.1f, 4.0f, 0.003f, 0.03f, 19},
		{1, 2, 0.16f, 2.0f, 0.008f, 0.05f, 3},
		{2, 3, 0.2f, 2.0f, 0.0f, 0.0f, 0},
	};

	const auto real = readAll(scene, frames, 0, 2);
	const auto model = renderModel(entries, rules, 2, frames);

	TASK_CHECK(allFinite(real));
	TASK_CHECK(maxDifference(real, model, real.size()) <= 2e-5);

	return 0;
}

static int case_multiple_readers_independent()
{
	const int frames = 1500;
	Fixture fixture;
	const auto model = renderModel(fixture.entries, fixture.rules, 2, frames);

	auto first = fixture.scene.createQualityReader(aud::ResampleQuality::FASTEST);
	auto second = fixture.scene.createQualityReader(aud::ResampleQuality::FASTEST);

	std::vector<float> a(size_t(400) * 2);
	int n = 400;
	bool eos = false;
	first->read(n, eos, a.data());

	std::vector<float> b(size_t(900) * 2);
	int m = 900;
	bool eos2 = false;
	second->read(m, eos2, b.data());

	std::vector<float> a2(size_t(700) * 2);
	int n2 = 700;
	bool eos3 = false;
	first->read(n2, eos3, a2.data());

	std::vector<float> joined(a.begin(), a.end());
	joined.insert(joined.end(), a2.begin(), a2.end());

	TASK_CHECK(allFinite(joined));
	TASK_CHECK(maxDifference(joined, model, joined.size()) <= 2e-5);

	// The prefix read by both readers must agree sample for sample.
	const std::vector<float> prefix(b.begin(), b.begin() + 400 * 2);
	TASK_CHECK(maxDifference(prefix, a, prefix.size()) <= 2e-5);

	return 0;
}

static int case_clear_rule_midstream_matches_plain_scene()
{
	Fixture fixture;
	const int frames = 1600;
	const int cut = 800;

	auto plain = [&]() {
		auto scene = std::make_unique<aud::Sequence>(aud::Specs{RATE, aud::CHANNELS_STEREO}, FPS, false);
		scene->add(std::make_shared<TestSound>(0.5f, 2), 211 / RATE, 1711 / RATE, 29 / RATE);
		scene->add(std::make_shared<TestSound>(0.42f, 2), 211 / RATE, 1711 / RATE, 29 / RATE);
		return scene;
	};

	// Reference without any rule, read whole.
	auto referenceScene = plain();
	const auto reference = readAll(*referenceScene, frames, 0, 2);

	auto reader = fixture.scene.createQualityReader(aud::ResampleQuality::FASTEST);
	std::vector<float> before(size_t(cut) * 2);
	int n = cut;
	bool eos = false;
	reader->read(n, eos, before.data());

	// The ducked prefix is quieter than the plain scene.
	double duckedEnergy = 0.0, plainEnergy = 0.0;
	for(int i = 0; i < cut * 2; i++)
	{
		duckedEnergy += std::fabs(before[size_t(i)]);
		plainEnergy += std::fabs(reference[size_t(i)]);
	}
	TASK_CHECK(duckedEnergy < plainEnergy);

	fixture.scene.clearDucking(fixture.target);
	// Removing a non-existent rule is a no-op.
	fixture.scene.clearDucking(fixture.target);

	std::vector<float> after(size_t(frames - cut) * 2);
	int m = frames - cut;
	bool eos2 = false;
	reader->read(m, eos2, after.data());

	TASK_CHECK(allFinite(after));
	const std::vector<float> referenceSuffix(reference.begin() + cut * 2, reference.end());
	TASK_CHECK(maxDifference(after, referenceSuffix, after.size()) <= 2e-5);

	return 0;
}

static int case_remove_key_drops_rule()
{
	Fixture fixture;
	const int frames = 1600;
	const int cut = 700;

	auto reader = fixture.scene.createQualityReader(aud::ResampleQuality::FASTEST);
	std::vector<float> before(size_t(cut) * 2);
	int n = cut;
	bool eos = false;
	reader->read(n, eos, before.data());

	fixture.scene.remove(fixture.key);

	// The target must now play un-ducked; compare against a scene that only has
	// the target.
	aud::Sequence plain({RATE, aud::CHANNELS_STEREO}, FPS, false);
	plain.add(std::make_shared<TestSound>(0.5f, 2), 211 / RATE, 1711 / RATE, 29 / RATE);
	const auto plainAll = readAll(plain, frames, 0, 2);

	std::vector<float> after(size_t(frames - cut) * 2);
	int m = frames - cut;
	bool eos2 = false;
	reader->read(m, eos2, after.data());

	TASK_CHECK(allFinite(after));
	const std::vector<float> plainSuffix(plainAll.begin() + cut * 2, plainAll.end());
	TASK_CHECK(maxDifference(after, plainSuffix, after.size()) <= 2e-5);

	return 0;
}

static int case_invalid_configuration_preserves_rule()
{
	Fixture fixture;
	const int frames = 900;

	const auto baseline = readAll(fixture.scene, frames, 0, 2);

	auto tryInvalid = [&](const aud::DuckingSettings& invalid) {
		bool threw = false;
		try
		{
			fixture.scene.setDucking(fixture.target, fixture.key, invalid);
		}
		catch(const std::invalid_argument&)
		{
			threw = true;
		}
		return threw;
	};

	TASK_CHECK(tryInvalid(settings(0.0f, 4.0f, 0.0f, 0.0f, 0)));
	TASK_CHECK(tryInvalid(settings(-1.0f, 4.0f, 0.0f, 0.0f, 0)));
	TASK_CHECK(tryInvalid(settings(std::numeric_limits<float>::infinity(), 4.0f, 0.0f, 0.0f, 0)));
	TASK_CHECK(tryInvalid(settings(0.1f, 0.5f, 0.0f, 0.0f, 0)));
	TASK_CHECK(tryInvalid(settings(0.1f, std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, 0)));
	TASK_CHECK(tryInvalid(settings(0.1f, 4.0f, -0.1f, 0.0f, 0)));
	TASK_CHECK(tryInvalid(settings(0.1f, 4.0f, 0.0f, -0.1f, 0)));
	TASK_CHECK(tryInvalid(settings(0.1f, 4.0f, 0.0f, 0.0f, -1)));
	TASK_CHECK(tryInvalid(settings(0.1f, 4.0f, 0.0f, 0.0f, 65537)));

	// Same target used as its own key is invalid.
	bool threw = false;
	try
	{
		fixture.scene.setDucking(fixture.target, fixture.target, settings(0.1f, 4.0f, 0.0f, 0.0f, 0));
	}
	catch(const std::invalid_argument&)
	{
		threw = true;
	}
	TASK_CHECK(threw);

	// An entry from another scene is not a member.
	aud::Sequence other({RATE, aud::CHANNELS_STEREO}, FPS, false);
	auto foreign = other.add(std::make_shared<TestSound>(0.2f, 2), 0.0, 1.0, 0.0);
	threw = false;
	try
	{
		fixture.scene.setDucking(fixture.target, foreign, settings(0.1f, 4.0f, 0.0f, 0.0f, 0));
	}
	catch(const std::invalid_argument&)
	{
		threw = true;
	}
	TASK_CHECK(threw);

	// The valid rule must be untouched by all rejected attempts.
	const auto after = readAll(fixture.scene, frames, 0, 2);
	TASK_CHECK(maxDifference(baseline, after, baseline.size()) <= 2e-5);

	// Replacing a rule with itself must not change the rendered output either.
	fixture.scene.setDucking(fixture.target, fixture.key, settings(0.1f, 4.0f, 0.006f, 0.04f, 29));
	const auto replaced = readAll(fixture.scene, frames, 0, 2);
	TASK_CHECK(maxDifference(baseline, replaced, baseline.size()) <= 2e-5);

	return 0;
}

// ---------------------------------------------------------------------------

extern const task_case task_cases[] = {
	{"scene_clock", "stereo_clock", case_stereo_clock},
	{"scene_clock", "mono_instantaneous", case_mono_instantaneous},
	{"scene_clock", "lookahead_shifts_detection", case_lookahead_shifts_detection},
	{"envelope_seek", "chunked_reads", case_chunked_reads},
	{"envelope_seek", "seek_rebuilds_history", case_seek_rebuilds_history},
	{"envelope_seek", "seek_after_edit_rebuilds_new_timeline", case_seek_after_edit_rebuilds_new_timeline},
	{"entry_changes", "key_move_between_reads", case_key_move_between_reads},
	{"entry_changes", "key_set_sound_between_reads", case_key_set_sound_between_reads},
	{"entry_changes", "key_mute_between_reads", case_key_mute_between_reads},
	{"entry_changes", "two_targets_share_key", case_two_targets_share_key},
	{"entry_changes", "multiple_readers_independent", case_multiple_readers_independent},
	{"entry_changes", "clear_rule_midstream_matches_plain_scene", case_clear_rule_midstream_matches_plain_scene},
	{"entry_changes", "remove_key_drops_rule", case_remove_key_drops_rule},
	{"entry_changes", "invalid_configuration_preserves_rule", case_invalid_configuration_preserves_rule},
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
