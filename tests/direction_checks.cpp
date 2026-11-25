/*
Task checks for bidirectional WavStream playback.

The cases generate their own WAV files (PCM16 / float32, mono / stereo,
distinct non-periodic channels) and independently sample the material using
the same coordinate rules as the production code: the next output frame's
source coordinate x, 20-bit fixed point step h, sample first then advance by
direction*h, POINT / LINEAR interpolation with the last frame duplicated.
*/

#include "task_support/check.h"

#include "soloud.h"
#include "soloud_wav.h"
#include "soloud_wavstream.h"
#include "soloud_file.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	const unsigned FRAC_BITS = 20;
	const unsigned long long FRAC_ONE = 1ULL << FRAC_BITS;

	// Distinct, non-periodic, L != R source material, kept inside [-1, 1].
	float material(unsigned frame, unsigned channel)
	{
		double v = sin((double)(frame + 1) * 0.00731 + channel * 1.3) * 0.6
			+ sin((double)(frame + 1) * 0.00193) * 0.3
			+ (channel ? 0.08 : -0.08);
		return (float)v;
	}

	void put16(std::vector<unsigned char> &b, unsigned v)
	{
		b.push_back((unsigned char)(v & 255));
		b.push_back((unsigned char)((v >> 8) & 255));
	}

	void put32(std::vector<unsigned char> &b, unsigned v)
	{
		put16(b, v & 65535);
		put16(b, v >> 16);
	}

	void putF32(std::vector<unsigned char> &b, float v)
	{
		unsigned u;
		memcpy(&u, &v, sizeof(u));
		put32(b, u);
	}

	// Build a RIFF WAV: 16-bit PCM (format 1) or 32-bit float (format 3).
	std::vector<unsigned char> makeWav(unsigned frames, unsigned channels, unsigned rate, bool useFloat)
	{
		unsigned bytesPerSample = useFloat ? 4u : 2u;
		unsigned blockAlign = channels * bytesPerSample;
		std::vector<unsigned char> b;
		const char *riff = "RIFF"; b.insert(b.end(), riff, riff + 4);
		put32(b, 36u + frames * blockAlign);
		const char *wave = "WAVE"; b.insert(b.end(), wave, wave + 4);
		const char *fmt = "fmt "; b.insert(b.end(), fmt, fmt + 4);
		put32(b, 16);
		put16(b, useFloat ? 3u : 1u);
		put16(b, channels);
		put32(b, rate);
		put32(b, rate * blockAlign);
		put16(b, blockAlign);
		put16(b, bytesPerSample * 8);
		const char *data = "data"; b.insert(b.end(), data, data + 4);
		put32(b, frames * blockAlign);
		for (unsigned i = 0; i < frames; i++)
		{
			for (unsigned c = 0; c < channels; c++)
			{
				float v = material(i, c);
				if (useFloat)
				{
					putF32(b, v);
				}
				else
				{
					long s = lround((double)v * 32767.0);
					if (s > 32767) s = 32767;
					if (s < -32768) s = -32768;
					put16(b, (unsigned short)(unsigned long)s);
				}
			}
		}
		return b;
	}

	// Ground-truth sample of the generated material, matching dr_wav scaling.
	float truthSample(unsigned frame, unsigned channel, bool useFloat)
	{
		float v = material(frame, channel);
		if (useFloat)
			return v;
		long s = lround((double)v * 32767.0);
		if (s > 32767) s = 32767;
		if (s < -32768) s = -32768;
		return (float)s / 32768.0f;
	}

	unsigned stepFixed(unsigned srcRate, unsigned outRate, float speed)
	{
		double h = (double)srcRate * (double)speed / (double)outRate;
		return (unsigned)floor(h * (double)FRAC_ONE);
	}

	// Frame that production's seek lands on: floor(seconds * source rate).
	unsigned seekedFrame(double seconds, unsigned srcRate)
	{
		return (unsigned)floor(seconds * (double)srcRate);
	}

	// Independent reference player that owns x exactly like the production code.
	struct RefPlayer
	{
		unsigned long long xf;		// x in 20-bit fixed point
		int dir;
		unsigned step;
		unsigned frames;
		unsigned channels;
		bool useFloat;
		unsigned resampler;		// SoLoud::Soloud::RESAMPLER_*

		float at(unsigned frame, unsigned channel) const
		{
			return truthSample(frame, channel, useFloat);
		}

		// Produce n interleaved stereo output frames (mono sources are duplicated).
		std::vector<float> render(unsigned n)
		{
			std::vector<float> out((size_t)n * 2, 0.0f);
			const unsigned long long end = (unsigned long long)frames * FRAC_ONE;
			for (unsigned i = 0; i < n; i++)
			{
				float l = 0.0f, r = 0.0f;
				bool sounding = (xf >= 0 && xf < end);
				if (sounding)
				{
					unsigned lo = (unsigned)(xf >> FRAC_BITS);
					unsigned frac = (unsigned)(xf & (FRAC_ONE - 1));
					unsigned hi = lo + 1;
					if (resampler == SoLoud::Soloud::RESAMPLER_POINT)
					{
						l = at(lo, 0);
						r = channels == 2 ? at(lo, 1) : l;
					}
					else
					{
						float l0 = at(lo, 0), l1 = (hi < frames) ? at(hi, 0) : l0;
						l = l0 + (l1 - l0) * ((float)frac / (float)FRAC_ONE);
						if (channels == 2)
						{
							float r0 = at(lo, 1), r1 = (hi < frames) ? at(hi, 1) : r0;
							r = r0 + (r1 - r0) * ((float)frac / (float)FRAC_ONE);
						}
						else
						{
							r = l;
						}
					}
				}
				out[2 * i] = l;
				out[2 * i + 1] = r;
				xf += (unsigned long long)((long long)dir * (long long)step);
			}
			return out;
		}

		double positionSeconds(unsigned srcRate) const
		{
			const unsigned long long end = (unsigned long long)frames * FRAC_ONE;
			double x;
			if ((long long)xf <= 0) x = 0.0;
			else if (xf >= end) x = (double)frames;
			else x = (double)xf / (double)FRAC_ONE;
			return x / (double)srcRate;
		}
	};

	bool close(float a, float b)
	{
		return fabs((double)a - (double)b) <= 2e-6;
	}

	bool buffersMatch(const std::vector<float> &got, const std::vector<float> &want)
	{
		if (got.size() != want.size())
			return false;
		for (size_t i = 0; i < want.size(); i++)
		{
			// Silence outside the material range must be exact zero.
			if (want[i] == 0.0f)
			{
				if (got[i] != 0.0f)
					return false;
			}
			else if (!close(got[i], want[i]))
			{
				std::fprintf(stderr, "  sample %zu got=%.9g want=%.9g\n", i, got[i], want[i]);
				return false;
			}
		}
		return true;
	}

	SoLoud::Soloud *makeEngine(unsigned outRate)
	{
		SoLoud::Soloud *s = new SoLoud::Soloud;
		if (s->init(0, SoLoud::Soloud::NULLDRIVER, outRate, 512, 2) != SoLoud::SO_NO_ERROR)
		{
			delete s;
			return 0;
		}
		s->setGlobalVolume(1);
		s->setPostClipScaler(1);
		return s;
	}

	void setupVoice(SoLoud::Soloud &s, SoLoud::handle h)
	{
		s.setPanAbsolute(h, 1.0f, 1.0f);
		s.setVolume(h, 1.0f);
		s.setAutoStop(h, false);
	}

	// File implementation that counts every byte read and seek, to prove that
	// reverse playback is served from fixed windows rather than per-frame I/O.
	class CountFile : public SoLoud::File
	{
	public:
		std::vector<unsigned char> data;
		unsigned offset;
		unsigned readCalls;
		unsigned seekCalls;
		unsigned long long bytesRead;
		unsigned maxReadBytes;

		explicit CountFile(std::vector<unsigned char> bytes)
			: data(std::move(bytes)), offset(0), readCalls(0), seekCalls(0), bytesRead(0), maxReadBytes(0) {}

		void resetCounters() { readCalls = seekCalls = 0; bytesRead = 0; maxReadBytes = 0; }
		int eof() override { return offset >= (unsigned)data.size(); }
		unsigned read(unsigned char *dst, unsigned bytes) override
		{
			if (offset + bytes > (unsigned)data.size())
				bytes = (unsigned)data.size() - offset;
			if (bytes)
				memcpy(dst, data.data() + offset, bytes);
			offset += bytes;
			readCalls++;
			bytesRead += bytes;
			if (bytes > maxReadBytes) maxReadBytes = bytes;
			return bytes;
		}
		unsigned length() override { return (unsigned)data.size(); }
		void seek(int aOffset) override
		{
			seekCalls++;
			offset = (aOffset < 0) ? 0 : (aOffset > (int)data.size() ? (unsigned)data.size() : (unsigned)aOffset);
		}
		unsigned pos() override { return offset; }
	};

	// Minimal live filter; filter sources are not part of the build, so the
	// unsupported-filter case provides its own instance.
	class TestFilter;
	class TestFilterInstance : public SoLoud::FilterInstance
	{
	public:
		explicit TestFilterInstance(TestFilter *) {}
		void filterChannel(float *aBuffer, unsigned int aSamples, float, SoLoud::time, unsigned int, unsigned int) override
		{
			for (unsigned i = 0; i < aSamples; i++) aBuffer[i] = -aBuffer[i];
		}
	};

	class TestFilter : public SoLoud::Filter
	{
	public:
		SoLoud::FilterInstance *createInstance() override { return new TestFilterInstance(this); }
	};

	std::string tempPath(const char *name)
	{
		const char *tmp = getenv("TMPDIR");
		std::string p = tmp ? tmp : "/tmp";
		p += "/";
		p += name;
		return p;
	}

	void writeFile(const std::string &path, const std::vector<unsigned char> &bytes)
	{
		FILE *f = fopen(path.c_str(), "wb");
		fwrite(bytes.data(), 1, bytes.size(), f);
		fclose(f);
	}
}

// ---------------------------------------------------------------------------
// transport group
// ---------------------------------------------------------------------------

static int transport_enable_mid_stream_continues_delivered_position()
{
	const unsigned frames = 8192, srcRate = 32000, outRate = 48000;
	auto bytes = makeWav(frames, 2, srcRate, false);
	SoLoud::Soloud *s = makeEngine(outRate);
	TASK_CHECK(s != 0);
	SoLoud::WavStream w;
	TASK_CHECK(w.loadMem(bytes.data(), (unsigned)bytes.size(), true, false) == SoLoud::SO_NO_ERROR);
	auto h = s->play(w, 1, 0, true);
	setupVoice(*s, h);
	s->setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
	s->setPause(h, false);

	// First play through the legacy forward path; the decoder reads ahead, so
	// enabling afterwards must take over from the delivered playhead, not the
	// decoder position.
	RefPlayer ref{ 0, 1, stepFixed(srcRate, outRate, 1.0f), frames, 2, false, SoLoud::Soloud::RESAMPLER_POINT };
	const unsigned preFrames = 600;
	std::vector<float> pre(preFrames * 2);
	s->mix(pre.data(), preFrames);
	TASK_CHECK(buffersMatch(pre, ref.render(preFrames)));

	TASK_CHECK(s->setWavStreamDirection(h, +1) == SoLoud::SO_NO_ERROR);
	TASK_CHECK(fabs(s->getStreamPosition(h) - ref.positionSeconds(srcRate)) < 1e-9);
	// Continue forward through the same bidirectional coordinate.
	std::vector<float> after(257 * 2);
	s->mix(after.data(), 257);
	TASK_CHECK(buffersMatch(after, ref.render(257)));
	// Reverse from exactly the delivered coordinate.
	TASK_CHECK(s->setWavStreamDirection(h, -1) == SoLoud::SO_NO_ERROR);
	ref.dir = -1;
	TASK_CHECK(fabs(s->getStreamPosition(h) - ref.positionSeconds(srcRate)) < 1e-9);
	std::vector<float> rev(257 * 2);
	s->mix(rev.data(), 257);
	TASK_CHECK(buffersMatch(rev, ref.render(257)));
	s->deinit();
	delete s;
	return 0;
}

static int transport_reverse_after_forward()
{
	const unsigned frames = 8192, srcRate = 32000, outRate = 48000;
	auto bytes = makeWav(frames, 2, srcRate, false);
	SoLoud::Soloud *s = makeEngine(outRate);
	TASK_CHECK(s != 0);
	SoLoud::WavStream w;
	TASK_CHECK(w.loadMem(bytes.data(), (unsigned)bytes.size(), true, false) == SoLoud::SO_NO_ERROR);
	auto h = s->play(w, 1, 0, true);
	setupVoice(*s, h);
	s->setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
	s->setPause(h, false);
	TASK_CHECK(s->setWavStreamDirection(h, +1) == SoLoud::SO_NO_ERROR);

	RefPlayer ref{ 0, 1, stepFixed(srcRate, outRate, 1.0f), frames, 2, false, SoLoud::Soloud::RESAMPLER_POINT };
	const unsigned chunks[4] = { 257, 257, 7, 257 };
	std::vector<float> got;
	for (unsigned k = 0; k < 4; k++)
	{
		std::vector<float> buf(chunks[k] * 2);
		s->mix(buf.data(), chunks[k]);
		auto want = ref.render(chunks[k]);
		TASK_CHECK(buffersMatch(buf, want));
		got.insert(got.end(), buf.begin(), buf.end());
	}
	// Flip to reverse; x (including its fractional part) must carry over.
	TASK_CHECK(s->setWavStreamDirection(h, -1) == SoLoud::SO_NO_ERROR);
	ref.dir = -1;
	TASK_CHECK(fabs(s->getStreamPosition(h) - ref.positionSeconds(srcRate)) < 1e-9);
	// Setting the same direction again changes nothing.
	double posBeforeRepeat = s->getStreamPosition(h);
	TASK_CHECK(s->setWavStreamDirection(h, -1) == SoLoud::SO_NO_ERROR);
	TASK_CHECK(s->getStreamPosition(h) == posBeforeRepeat);
	// Stream time keeps accumulating the output time even while reversing.
	double tBefore = s->getStreamTime(h);
	for (unsigned k = 0; k < 4; k++)
	{
		std::vector<float> buf(chunks[k] * 2);
		s->mix(buf.data(), chunks[k]);
		auto want = ref.render(chunks[k]);
		TASK_CHECK(buffersMatch(buf, want));
	}
	double reverseElapsed = 0;
	for (unsigned c2 = 0; c2 < 4; c2++) reverseElapsed += chunks[c2];
	TASK_CHECK(fabs(s->getStreamTime(h) - (tBefore + reverseElapsed / outRate)) < 1e-9);
	// Release: forward playback resumes from the same coordinate.
	TASK_CHECK(s->setWavStreamDirection(h, +1) == SoLoud::SO_NO_ERROR);
	ref.dir = 1;
	TASK_CHECK(fabs(s->getStreamPosition(h) - ref.positionSeconds(srcRate)) < 1e-9);
	for (unsigned k = 0; k < 4; k++)
	{
		std::vector<float> buf(chunks[k] * 2);
		s->mix(buf.data(), chunks[k]);
		auto want = ref.render(chunks[k]);
		TASK_CHECK(buffersMatch(buf, want));
	}
	s->deinit();
	delete s;
	return 0;
}

static int transport_reverse_seek_then_forward()
{
	const unsigned frames = 6000, srcRate = 24000, outRate = 48000;
	auto bytes = makeWav(frames, 1, srcRate, true);
	SoLoud::Soloud *s = makeEngine(outRate);
	TASK_CHECK(s != 0);
	SoLoud::WavStream w;
	TASK_CHECK(w.loadMem(bytes.data(), (unsigned)bytes.size(), true, false) == SoLoud::SO_NO_ERROR);
	auto h = s->play(w, 1, 0, true);
	setupVoice(*s, h);
	s->setMainResampler(SoLoud::Soloud::RESAMPLER_LINEAR);
	s->setPause(h, false);
	TASK_CHECK(s->setWavStreamDirection(h, -1) == SoLoud::SO_NO_ERROR);

	double startSec = (double)(frames - 100) / srcRate;
	unsigned startFrame = seekedFrame(startSec, srcRate);
	RefPlayer ref{ (unsigned long long)startFrame * FRAC_ONE, -1, stepFixed(srcRate, outRate, 1.0f),
		frames, 1, true, SoLoud::Soloud::RESAMPLER_LINEAR };
	// Begin near the end so reverse has room to walk.
	TASK_CHECK(s->seek(h, startSec) == SoLoud::SO_NO_ERROR);
	double timeBeforeSeek = s->getStreamTime(h);
	{
		std::vector<float> buf(301 * 2);
		s->mix(buf.data(), 301);
		auto want = ref.render(301);
		TASK_CHECK(buffersMatch(buf, want));
	}
	// Seek while reversing: x jumps, direction and accumulated time survive.
	const double seekSec = 0.05;
	TASK_CHECK(s->seek(h, seekSec) == SoLoud::SO_NO_ERROR);
	unsigned seekFrame = (unsigned)floor(seekSec * srcRate);
	ref.xf = (unsigned long long)seekFrame * FRAC_ONE;
	TASK_CHECK(fabs(s->getStreamPosition(h) - (double)seekFrame / srcRate) < 1e-9);
	TASK_CHECK(s->getStreamTime(h) >= timeBeforeSeek);
	{
		std::vector<float> buf(123 * 2);
		s->mix(buf.data(), 123);
		auto want = ref.render(123);
		TASK_CHECK(buffersMatch(buf, want));
	}
	// Switch back to forward from the seeked coordinate.
	TASK_CHECK(s->setWavStreamDirection(h, +1) == SoLoud::SO_NO_ERROR);
	ref.dir = 1;
	TASK_CHECK(fabs(s->getStreamPosition(h) - ref.positionSeconds(srcRate)) < 1e-9);
	{
		std::vector<float> buf(257 * 2);
		s->mix(buf.data(), 257);
		auto want = ref.render(257);
		TASK_CHECK(buffersMatch(buf, want));
	}
	// Illegal seek leaves every bit of state untouched.
	double posBefore = s->getStreamPosition(h);
	double timeBefore = s->getStreamTime(h);
	TASK_CHECK(s->seek(h, w.getLength()) == SoLoud::INVALID_PARAMETER);
	TASK_CHECK(s->seek(h, -0.01) == SoLoud::INVALID_PARAMETER);
	TASK_CHECK(fabs(s->getStreamPosition(h) - posBefore) < 1e-12);
	TASK_CHECK(fabs(s->getStreamTime(h) - timeBefore) < 1e-12);
	s->deinit();
	delete s;
	return 0;
}

static int transport_endpoints_and_autostop()
{
	const unsigned frames = 256, srcRate = 16000, outRate = 16000;
	auto bytes = makeWav(frames, 2, srcRate, false);
	SoLoud::Soloud *s = makeEngine(outRate);
	TASK_CHECK(s != 0);
	SoLoud::WavStream w;
	TASK_CHECK(w.loadMem(bytes.data(), (unsigned)bytes.size(), true, false) == SoLoud::SO_NO_ERROR);

	// Reverse into the start: remaining output is silence, position clamps to 0.
	auto h = s->play(w, 1, 0, true);
	setupVoice(*s, h);
	s->setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
	s->setPause(h, false);
	TASK_CHECK(s->setWavStreamDirection(h, -1) == SoLoud::SO_NO_ERROR);
	TASK_CHECK(s->seek(h, 0.005) == SoLoud::SO_NO_ERROR);	// frame 80
	std::vector<float> buf(400 * 2, 0.0f);
	s->mix(buf.data(), 400);
	unsigned zeros = 0;
	for (unsigned i = 0; i < 400; i++) if (buf[2 * i] == 0.0f && buf[2 * i + 1] == 0.0f) zeros++;
	TASK_CHECK(zeros > 300);
	TASK_CHECK(s->getStreamPosition(h) == 0.0);
	TASK_CHECK(s->isValidVoiceHandle(h));	// auto-stop disabled, voice survives

	// Flipping direction while parked outside the start walks x back into range
	// and resumes sounding from frame 0 (the mouse-release-to-forward scenario).
	RefPlayer reenter{ (unsigned long long)80 * FRAC_ONE, -1, stepFixed(srcRate, srcRate, 1.0f),
		frames, 2, false, SoLoud::Soloud::RESAMPLER_POINT };
	reenter.render(400);	// match the reverse walk that parked x below zero
	TASK_CHECK(s->setWavStreamDirection(h, +1) == SoLoud::SO_NO_ERROR);
	reenter.dir = 1;
	std::vector<float> rebuf(400 * 2, 0.0f);
	s->mix(rebuf.data(), 400);
	TASK_CHECK(buffersMatch(rebuf, reenter.render(400)));
	unsigned reSounding = 0;
	for (unsigned i = 0; i < 400; i++) if (rebuf[2 * i] != 0.0f) reSounding++;
	TASK_CHECK(reSounding > 0);

	// Forward past the end with auto-stop enabled removes the voice.
	auto h2 = s->play(w, 1, 0, true);
	setupVoice(*s, h2);
	s->setAutoStop(h2, true);
	s->setPause(h2, false);
	TASK_CHECK(s->setWavStreamDirection(h2, +1) == SoLoud::SO_NO_ERROR);
	TASK_CHECK(s->seek(h2, (double)(frames - 10) / srcRate) == SoLoud::SO_NO_ERROR);
	std::vector<float> buf2(512 * 2, 0.0f);
	s->mix(buf2.data(), 512);
	TASK_CHECK(!s->isValidVoiceHandle(h2));
	s->deinit();
	delete s;
	return 0;
}

static int transport_pause_freezes_coordinate()
{
	const unsigned frames = 4096, srcRate = 32000, outRate = 44100;
	auto bytes = makeWav(frames, 2, srcRate, false);
	SoLoud::Soloud *s = makeEngine(outRate);
	TASK_CHECK(s != 0);
	SoLoud::WavStream w;
	TASK_CHECK(w.loadMem(bytes.data(), (unsigned)bytes.size(), true, false) == SoLoud::SO_NO_ERROR);
	auto h = s->play(w, 1, 0, true);
	setupVoice(*s, h);
	s->setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
	s->setPause(h, false);
	TASK_CHECK(s->setWavStreamDirection(h, -1) == SoLoud::SO_NO_ERROR);
	TASK_CHECK(s->seek(h, 0.1) == SoLoud::SO_NO_ERROR);
	std::vector<float> buf(128 * 2);
	s->mix(buf.data(), 128);
	double pos = s->getStreamPosition(h);
	double elapsed = s->getStreamTime(h);
	s->setPause(h, true);
	std::vector<float> quiet(64 * 2, 1.0f);
	s->mix(quiet.data(), 64);
	TASK_CHECK(s->getStreamPosition(h) == pos);
	TASK_CHECK(s->getStreamTime(h) == elapsed);
	for (float v : quiet) TASK_CHECK(v == 0.0f);
	s->setPause(h, false);
	s->mix(buf.data(), 128);
	TASK_CHECK(s->getStreamPosition(h) != pos);
	s->deinit();
	delete s;
	return 0;
}

static int transport_invalid_calls_keep_state()
{
	const unsigned frames = 2048, rate = 32000;
	auto bytes = makeWav(frames, 2, rate, false);
	SoLoud::Soloud *s = makeEngine(rate);
	TASK_CHECK(s != 0);
	SoLoud::WavStream w;
	TASK_CHECK(w.loadMem(bytes.data(), (unsigned)bytes.size(), true, false) == SoLoud::SO_NO_ERROR);
	auto h = s->play(w, 1, 0, true);
	setupVoice(*s, h);
	s->setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
	s->setPause(h, false);

	// Bad directions before enablement.
	TASK_CHECK(s->setWavStreamDirection(h, 0) == SoLoud::INVALID_PARAMETER);
	TASK_CHECK(s->setWavStreamDirection(h, -2) == SoLoud::INVALID_PARAMETER);
	TASK_CHECK(s->setWavStreamDirection(h, 2) == SoLoud::INVALID_PARAMETER);
	// Invalid handle.
	TASK_CHECK(s->setWavStreamDirection(0, +1) == SoLoud::INVALID_PARAMETER);
	TASK_CHECK(s->setWavStreamDirection(0xfffffffu, +1) == SoLoud::INVALID_PARAMETER);
	// Voice group handle.
	SoLoud::handle group = s->createVoiceGroup();
	TASK_CHECK(group != 0);
	TASK_CHECK(s->addVoiceToGroup(group, h) == SoLoud::SO_NO_ERROR);
	TASK_CHECK(s->setWavStreamDirection(group, -1) == SoLoud::INVALID_PARAMETER);
	TASK_CHECK(!s->isValidVoiceHandle(group));

	// Looping, filter, bus and 3D configurations are rejected.
	SoLoud::WavStream loopW;
	TASK_CHECK(loopW.loadMem(bytes.data(), (unsigned)bytes.size(), true, false) == SoLoud::SO_NO_ERROR);
	loopW.setLooping(true);
	auto lh = s->play(loopW);
	TASK_CHECK(s->setWavStreamDirection(lh, -1) == SoLoud::INVALID_PARAMETER);

	SoLoud::WavStream filtW;
	TASK_CHECK(filtW.loadMem(bytes.data(), (unsigned)bytes.size(), true, false) == SoLoud::SO_NO_ERROR);
	TestFilter tf;
	filtW.setFilter(0, &tf);
	auto fh = s->play(filtW);
	TASK_CHECK(s->setWavStreamDirection(fh, -1) == SoLoud::INVALID_PARAMETER);

	SoLoud::Bus bus;
	auto bh = s->play(bus);
	TASK_CHECK(s->setWavStreamDirection(bh, -1) == SoLoud::INVALID_PARAMETER);	// bus voice itself
	SoLoud::WavStream busW;
	TASK_CHECK(busW.loadMem(bytes.data(), (unsigned)bytes.size(), true, false) == SoLoud::SO_NO_ERROR);
	auto busVoice = bus.play(busW);
	TASK_CHECK(s->setWavStreamDirection(busVoice, -1) == SoLoud::INVALID_PARAMETER);	// child on a bus

	SoLoud::WavStream threeW;
	TASK_CHECK(threeW.loadMem(bytes.data(), (unsigned)bytes.size(), true, false) == SoLoud::SO_NO_ERROR);
	auto th = s->play3d(threeW, 0, 0, 0);
	TASK_CHECK(s->setWavStreamDirection(th, -1) == SoLoud::INVALID_PARAMETER);

	// A non-streaming Wav source is rejected.
	SoLoud::Wav memWav;
	TASK_CHECK(memWav.loadMem(bytes.data(), (unsigned)bytes.size(), true, false) == SoLoud::SO_NO_ERROR);
	auto wh = s->play(memWav);
	TASK_CHECK(s->setWavStreamDirection(wh, -1) == SoLoud::INVALID_PARAMETER);

	// Nothing was enabled: the original voice is still live on the legacy forward path.
	TASK_CHECK(s->isValidVoiceHandle(h));
	double posBefore = s->getStreamPosition(h);
	std::vector<float> buf(257 * 2);
	s->mix(buf.data(), 257);
	TASK_CHECK(s->getStreamPosition(h) > posBefore);
	TASK_CHECK(s->setWavStreamDirection(h, +1) == SoLoud::SO_NO_ERROR);	// still enableable
	s->deinit();
	delete s;
	return 0;
}

// ---------------------------------------------------------------------------
// fraction group
// ---------------------------------------------------------------------------

static int fraction_fractional_step_keeps_x()
{
	const unsigned frames = 8192, srcRate = 22050, outRate = 48000;
	auto bytes = makeWav(frames, 2, srcRate, true);
	SoLoud::Soloud *s = makeEngine(outRate);
	TASK_CHECK(s != 0);
	SoLoud::WavStream w;
	TASK_CHECK(w.loadMem(bytes.data(), (unsigned)bytes.size(), true, false) == SoLoud::SO_NO_ERROR);
	auto h = s->play(w, 1, 0, true);
	setupVoice(*s, h);
	s->setMainResampler(SoLoud::Soloud::RESAMPLER_LINEAR);
	s->setPause(h, false);
	TASK_CHECK(s->setWavStreamDirection(h, +1) == SoLoud::SO_NO_ERROR);

	unsigned hstep = stepFixed(srcRate, outRate, 1.0f);
	TASK_CHECK(hstep < FRAC_ONE);	// genuinely sub-frame step
	RefPlayer ref{ 0, 1, hstep, frames, 2, true, SoLoud::Soloud::RESAMPLER_LINEAR };
	std::vector<float> buf(333 * 2);
	s->mix(buf.data(), 333);
	auto want = ref.render(333);
	TASK_CHECK(buffersMatch(buf, want));

	// Reverse must not flip the fractional part of x.
	double posAtSwitch = s->getStreamPosition(h);
	TASK_CHECK(s->setWavStreamDirection(h, -1) == SoLoud::SO_NO_ERROR);
	ref.dir = -1;
	TASK_CHECK(fabs(s->getStreamPosition(h) - posAtSwitch) < 1e-12);
	TASK_CHECK(fabs(s->getStreamPosition(h) - ref.positionSeconds(srcRate)) < 1e-9);
	s->mix(buf.data(), 333);
	want = ref.render(333);
	TASK_CHECK(buffersMatch(buf, want));
	s->deinit();
	delete s;
	return 0;
}

static int fraction_speed_change_keeps_coordinate()
{
	const unsigned frames = 8192, srcRate = 32000, outRate = 32000;
	auto bytes = makeWav(frames, 1, srcRate, false);
	SoLoud::Soloud *s = makeEngine(outRate);
	TASK_CHECK(s != 0);
	SoLoud::WavStream w;
	TASK_CHECK(w.loadMem(bytes.data(), (unsigned)bytes.size(), true, false) == SoLoud::SO_NO_ERROR);
	auto h = s->play(w, 1, 0, true);
	setupVoice(*s, h);
	s->setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
	s->setPause(h, false);
	TASK_CHECK(s->setWavStreamDirection(h, -1) == SoLoud::SO_NO_ERROR);
	TASK_CHECK(s->seek(h, 0.1) == SoLoud::SO_NO_ERROR);
	RefPlayer ref{ (unsigned long long)floor(0.1 * srcRate) * FRAC_ONE, -1,
		stepFixed(srcRate, outRate, 1.0f), frames, 1, false, SoLoud::Soloud::RESAMPLER_POINT };
	std::vector<float> buf(100 * 2);
	s->mix(buf.data(), 100);
	TASK_CHECK(buffersMatch(buf, ref.render(100)));

	double pos = s->getStreamPosition(h);
	TASK_CHECK(s->setRelativePlaySpeed(h, 2.0f) == SoLoud::SO_NO_ERROR);
	TASK_CHECK(fabs(s->getStreamPosition(h) - pos) < 1e-12);	// x unchanged
	ref.step = stepFixed(srcRate, outRate, 2.0f);
	TASK_CHECK(ref.step == 2 * stepFixed(srcRate, outRate, 1.0f));
	s->mix(buf.data(), 100);
	TASK_CHECK(buffersMatch(buf, ref.render(100)));
	// Negative absolute speed stays rejected.
	TASK_CHECK(s->setRelativePlaySpeed(h, -1.0f) == SoLoud::INVALID_PARAMETER);
	s->deinit();
	delete s;
	return 0;
}

static int fraction_linear_last_frame_clamps()
{
	const unsigned frames = 128, srcRate = 16000, outRate = 24000;
	auto bytes = makeWav(frames, 2, srcRate, true);
	SoLoud::Soloud *s = makeEngine(outRate);
	TASK_CHECK(s != 0);
	SoLoud::WavStream w;
	TASK_CHECK(w.loadMem(bytes.data(), (unsigned)bytes.size(), true, false) == SoLoud::SO_NO_ERROR);
	auto h = s->play(w, 1, 0, true);
	setupVoice(*s, h);
	s->setMainResampler(SoLoud::Soloud::RESAMPLER_LINEAR);
	s->setPause(h, false);
	TASK_CHECK(s->setWavStreamDirection(h, +1) == SoLoud::SO_NO_ERROR);
	// A sub-frame step walks a fractional x onto the final frame; its right
	// neighbour must be copied, and the next steps leave the range and go silent.
	double startSec = (double)(frames - 4) / srcRate;
	unsigned startFrame = seekedFrame(startSec, srcRate);
	TASK_CHECK(s->seek(h, startSec) == SoLoud::SO_NO_ERROR);
	RefPlayer ref{ (unsigned long long)startFrame * FRAC_ONE, 1,
		stepFixed(srcRate, outRate, 1.0f), frames, 2, true, SoLoud::Soloud::RESAMPLER_LINEAR };
	TASK_CHECK(ref.step < FRAC_ONE);	// genuinely fractional steps
	std::vector<float> buf(12 * 2);
	s->mix(buf.data(), 12);
	TASK_CHECK(buffersMatch(buf, ref.render(12)));
	TASK_CHECK(fabs(s->getStreamPosition(h) - ref.positionSeconds(srcRate)) < 1e-9);
	// The coordinate walked out of range and clamps at the end endpoint.
	TASK_CHECK(s->getStreamPosition(h) >= (double)(frames - 1) / srcRate);
	s->deinit();
	delete s;
	return 0;
}

// ---------------------------------------------------------------------------
// storage group
// ---------------------------------------------------------------------------

static int storage_chunked_mono_stereo_formats()
{
	const unsigned chunks[3] = { 1, 7, 257 };
	const bool floatModes[2] = { false, true };
	const unsigned channelModes[2] = { 1, 2 };
	for (unsigned ci = 0; ci < 3; ci++)
	{
		for (unsigned fi = 0; fi < 2; fi++)
		{
			for (unsigned mi = 0; mi < 2; mi++)
			{
				const unsigned frames = 3000, srcRate = 24000, outRate = 32000;
				auto bytes = makeWav(frames, channelModes[mi], srcRate, floatModes[fi]);
				SoLoud::Soloud *s = makeEngine(outRate);
				TASK_CHECK(s != 0);
				SoLoud::WavStream w;
				TASK_CHECK(w.loadMem(bytes.data(), (unsigned)bytes.size(), true, false) == SoLoud::SO_NO_ERROR);
				auto h = s->play(w, 1, 0, true);
				setupVoice(*s, h);
				s->setMainResampler(SoLoud::Soloud::RESAMPLER_LINEAR);
				s->setPause(h, false);
				TASK_CHECK(s->setWavStreamDirection(h, -1) == SoLoud::SO_NO_ERROR);
				double startSec = (double)(frames - 50) / srcRate;
				TASK_CHECK(s->seek(h, startSec) == SoLoud::SO_NO_ERROR);
				RefPlayer ref{ (unsigned long long)seekedFrame(startSec, srcRate) * FRAC_ONE, -1,
					stepFixed(srcRate, outRate, 1.0f), frames, channelModes[mi], floatModes[fi],
					SoLoud::Soloud::RESAMPLER_LINEAR };
				unsigned chunk = chunks[ci];
				std::vector<float> buf(chunk * 2);
				s->mix(buf.data(), chunk);
				TASK_CHECK(buffersMatch(buf, ref.render(chunk)));
				s->deinit();
				delete s;
			}
		}
	}
	return 0;
}

static int storage_disk_load_and_load_to_mem()
{
	const unsigned frames = 4096, srcRate = 32000, outRate = 48000;
	auto bytes = makeWav(frames, 2, srcRate, false);
	std::string p1 = tempPath("soloud_dir_load.wav");
	std::string p2 = tempPath("soloud_dir_loadtomem.wav");
	writeFile(p1, bytes);
	writeFile(p2, bytes);

	for (int mode = 0; mode < 2; mode++)
	{
		SoLoud::Soloud *s = makeEngine(outRate);
		TASK_CHECK(s != 0);
		SoLoud::WavStream w;
		TASK_CHECK(mode == 0 ? w.load(p1.c_str()) == SoLoud::SO_NO_ERROR
			: w.loadToMem(p2.c_str()) == SoLoud::SO_NO_ERROR);
		auto h = s->play(w, 1, 0, true);
		setupVoice(*s, h);
		s->setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
		s->setPause(h, false);
		TASK_CHECK(s->setWavStreamDirection(h, -1) == SoLoud::SO_NO_ERROR);
		double startSec = (double)(frames - 300) / srcRate;
		TASK_CHECK(s->seek(h, startSec) == SoLoud::SO_NO_ERROR);
		RefPlayer ref{ (unsigned long long)seekedFrame(startSec, srcRate) * FRAC_ONE, -1,
			stepFixed(srcRate, outRate, 1.0f), frames, 2, false, SoLoud::Soloud::RESAMPLER_POINT };
		std::vector<float> buf(257 * 2);
		s->mix(buf.data(), 257);
		TASK_CHECK(buffersMatch(buf, ref.render(257)));
		s->deinit();
		delete s;
	}
	remove(p1.c_str());
	remove(p2.c_str());
	return 0;
}

static int storage_reverse_uses_windowed_reads()
{
	const unsigned frames = 16384, rate = 32000;
	auto bytes = makeWav(frames, 2, rate, false);
	CountFile *file = new CountFile(bytes);
	SoLoud::Soloud *s = makeEngine(rate);
	TASK_CHECK(s != 0);
	SoLoud::WavStream w;
	// Stream straight from the counting file; its instance reuses this object.
	TASK_CHECK(w.loadFile(file) == SoLoud::SO_NO_ERROR);
	auto h = s->play(w, 1, 0, true);
	setupVoice(*s, h);
	s->setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
	s->setPause(h, false);
	TASK_CHECK(s->setWavStreamDirection(h, -1) == SoLoud::SO_NO_ERROR);
	TASK_CHECK(s->seek(h, (double)(frames - 100) / rate) == SoLoud::SO_NO_ERROR);
	file->resetCounters();

	const unsigned outFrames = 4096;
	std::vector<float> buf(outFrames * 2);
	// Feed through several chunk sizes, including the 512-frame read boundary.
	unsigned done = 0;
	const unsigned sizes[4] = { 1, 7, 257, 512 };
	while (done < outFrames)
	{
		unsigned n = sizes[done % 4];
		if (n > outFrames - done) n = outFrames - done;
		std::vector<float> part(n * 2);
		s->mix(part.data(), n);
		memcpy(buf.data() + done * 2, part.data(), n * 2 * sizeof(float));
		done += n;
	}

	// 4096 reverse output frames across 16k-frame material must cost a handful of
	// window-sized reads, never one read or seek per frame, regardless of file length.
	TASK_CHECK(file->readCalls >= 1);
	TASK_CHECK(file->readCalls <= 4);
	TASK_CHECK(file->seekCalls <= 10);
	TASK_CHECK(file->maxReadBytes <= 2048u * 2u * sizeof(short));
	// Every refill is a single contiguous read; no frame-granular seeking.
	TASK_CHECK(file->bytesRead <= (unsigned long long)file->maxReadBytes * file->readCalls);

	// Reversing while staying inside one window must not touch the file at all.
	TASK_CHECK(s->seek(h, (double)(frames - 3000) / rate) == SoLoud::SO_NO_ERROR);
	std::vector<float> prime(1 * 2);
	s->mix(prime.data(), 1);	// prime the window for the new coordinate
	file->resetCounters();
	std::vector<float> inside(500 * 2);
	s->mix(inside.data(), 500);
	TASK_CHECK(file->readCalls == 0);
	TASK_CHECK(file->seekCalls == 0);

	unsigned winStart = seekedFrame((double)(frames - 100) / rate, rate);
	RefPlayer ref{ (unsigned long long)winStart * FRAC_ONE, -1,
		stepFixed(rate, rate, 1.0f), frames, 2, false, SoLoud::Soloud::RESAMPLER_POINT };
	TASK_CHECK(buffersMatch(buf, ref.render(outFrames)));
	s->deinit();
	delete s;
	delete file;
	return 0;
}

// ---------------------------------------------------------------------------
// registration
// ---------------------------------------------------------------------------

const task_case task_cases[] =
{
	{ "transport", "enable_mid_stream_continues_delivered_position", transport_enable_mid_stream_continues_delivered_position },
	{ "transport", "reverse_after_forward", transport_reverse_after_forward },
	{ "transport", "reverse_seek_then_forward", transport_reverse_seek_then_forward },
	{ "transport", "endpoints_and_autostop", transport_endpoints_and_autostop },
	{ "transport", "pause_freezes_coordinate", transport_pause_freezes_coordinate },
	{ "transport", "invalid_calls_keep_state", transport_invalid_calls_keep_state },
	{ "fraction", "fractional_step_keeps_x", fraction_fractional_step_keeps_x },
	{ "fraction", "speed_change_keeps_coordinate", fraction_speed_change_keeps_coordinate },
	{ "fraction", "linear_last_frame_clamps", fraction_linear_last_frame_clamps },
	{ "storage", "chunked_mono_stereo_formats", storage_chunked_mono_stereo_formats },
	{ "storage", "disk_load_and_load_to_mem", storage_disk_load_and_load_to_mem },
	{ "storage", "reverse_uses_windowed_reads", storage_reverse_uses_windowed_reads },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
