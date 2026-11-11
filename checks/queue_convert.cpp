// Tests for Queue::setAutoConvert() - sample rate / channel conversion while
// queueing short audio sources.

#include "soloud.h"
#include "soloud_queue.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static bool ok = true;

#define CHECK(cond)                                                          \
	do                                                                       \
	{                                                                        \
		if (!(cond))                                                         \
		{                                                                    \
			std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,      \
				__LINE__, #cond);                                            \
			ok = false;                                                      \
			std::exit(1);                                                    \
		}                                                                    \
	} while (0)

struct Clip : public SoLoud::AudioSource
{
	unsigned int n;
	std::vector<float> data[2];
	unsigned int shortRead;   // 0 = unlimited
	int stallCall;            // call number which returns 0 while not ended
	unsigned int stallCount;

	Clip(unsigned int aCount, float aRate, unsigned int aChannels)
		: n(aCount), shortRead(0), stallCall(-1), stallCount(0)
	{
		mBaseSamplerate = aRate;
		mChannels = aChannels;
	}

	void setData(unsigned int aChannel, const std::vector<float> &aData)
	{
		data[aChannel] = aData;
		CHECK(aData.size() == n);
	}

	struct Instance : public SoLoud::AudioSourceInstance
	{
		Clip *p;
		unsigned int at;
		unsigned int calls;

		Instance(Clip *aParent) : p(aParent), at(0), calls(0) {}

		virtual unsigned int getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize)
		{
			if (p->stallCall >= 0 && (int)calls == p->stallCall && p->stallCount > 0)
			{
				p->stallCount--;
				calls++;
				return 0;
			}
			calls++;

			unsigned int available = p->n - at;
			unsigned int take = aSamplesToRead < available ? aSamplesToRead : available;
			if (p->shortRead && take > p->shortRead)
				take = p->shortRead;

			unsigned int c, i;
			for (c = 0; c < p->mChannels; c++)
				for (i = 0; i < take; i++)
					aBuffer[c * aBufferSize + i] = p->data[c][at + i];

			at += take;
			return take;
		}

		virtual bool hasEnded()
		{
			return at == p->n;
		}
	};

	virtual SoLoud::AudioSourceInstance *createInstance()
	{
		return new Instance(this);
	}
};

// Independent reference implementation of the conversion described by the
// Queue documentation.
static std::vector<float> reference(const Clip &aClip, unsigned int aDstRate, unsigned int aDstChannels)
{
	unsigned int srcRate = (unsigned int)aClip.mBaseSamplerate;
	unsigned int srcChannels = aClip.mChannels;
	unsigned int n = aClip.n;
	unsigned long long frames = n == 0 ? 0 :
		((unsigned long long)n * aDstRate + srcRate - 1) / srcRate;

	std::vector<float> out((unsigned int)frames * aDstChannels, 0.0f);

	unsigned long long k;
	for (k = 0; k < frames; k++)
	{
		unsigned long long position = k * (unsigned long long)srcRate;
		unsigned long long index = position / aDstRate;
		double frac = (double)(position % aDstRate) / (double)aDstRate;

		float value[2];
		unsigned int c;
		for (c = 0; c < srcChannels; c++)
		{
			float left = aClip.data[c][(unsigned int)index];
			unsigned long long right = index + 1;
			if (right >= n)
				right = n - 1; // clamp the right neighbor at the end
			float rightValue = aClip.data[c][(unsigned int)right];
			value[c] = (float)((double)left + ((double)rightValue - (double)left) * frac);
		}

		if (aDstChannels == 1)
		{
			out[(unsigned int)k] = (srcChannels == 2)
				? (value[0] + value[1]) * 0.5f
				: value[0];
		}
		else
		{
			out[(unsigned int)k] = value[0];
			out[(unsigned int)frames + (unsigned int)k] =
				(srcChannels == 2) ? value[1] : value[0];
		}
	}

	return out;
}

static void expectSame(const std::vector<float> &aGot, const std::vector<float> &aExpected,
	unsigned int aCount, const char *aWhat, unsigned int aChunk, bool aChunked)
{
	CHECK(aGot.size() == aExpected.size());
	unsigned int i;
	for (i = 0; i < aCount && i < aGot.size(); i++)
	{
		float diff = std::fabs(aGot[i] - aExpected[i]);
		if (diff > 1e-4f * (1.0f + std::fabs(aExpected[i])))
		{
			std::fprintf(stderr,
				"%s (%s, chunk=%u): sample %u got %f expected %f (diff %f)\n",
				aWhat, aChunked ? "chunked" : "whole", aChunk, i,
				aGot[i], aExpected[i], diff);
			CHECK(false);
		}
	}
}

static std::vector<float> readWhole(SoLoud::Queue &aQueue, unsigned int aFrames, unsigned int aChannels)
{
	std::vector<float> out(aFrames * aChannels, 0.0f);
	unsigned int got = aQueue.mInstance->getAudio(out.data(), aFrames, aFrames);
	CHECK(got == aFrames);
	return out;
}

static std::vector<float> readChunked(SoLoud::Queue &aQueue, unsigned int aFrames,
	unsigned int aChannels, unsigned int aChunk)
{
	std::vector<float> out(aFrames * aChannels, 0.0f);
	std::vector<float> tmp(aChunk * aChannels, 0.0f);
	unsigned int offset = 0;
	while (offset < aFrames)
	{
		unsigned int want = aChunk < aFrames - offset ? aChunk : aFrames - offset;
		unsigned int got = aQueue.mInstance->getAudio(tmp.data(), want, want);
		CHECK(got == want);
		unsigned int c, i;
		for (c = 0; c < aChannels; c++)
			for (i = 0; i < got; i++)
				out[c * aFrames + offset + i] = tmp[c * want + i];
		offset += got;
	}
	return out;
}

static void setupQueue(SoLoud::Soloud &aS, SoLoud::Queue &aQ, unsigned int aRate,
	unsigned int aChannels, SoLoud::handle &aHandle)
{
	CHECK(aQ.setParams((float)aRate, aChannels) == SoLoud::SO_NO_ERROR);
	CHECK(aQ.setAutoConvert(true) == SoLoud::SO_NO_ERROR);
	aHandle = aS.play(aQ);
	CHECK(aHandle != 0);
}

static void testRateAndChannels(SoLoud::Soloud &aS)
{
	// 24 kHz mono -> 48 kHz mono, whole and chunked reads, using a sequence
	// which is independent of the stereo sequences below.
	{
		SoLoud::Queue q;
		SoLoud::handle h;
		setupQueue(aS, q, 48000, 1, h);

		Clip clip(7, 24000, 1);
		clip.setData(0, std::vector<float>{0.0f, 4.0f, -2.0f, 8.0f, 3.0f, -6.0f, 5.0f});
		CHECK(q.play(clip) == SoLoud::SO_NO_ERROR);
		CHECK(q.play(clip) == SoLoud::SO_NO_ERROR);

		std::vector<float> expected = reference(clip, 48000, 1);
		CHECK(expected.size() == 14);

		std::vector<float> whole = readWhole(q, 14, 1);
		expectSame(whole, expected, 14, "24->48 mono", 14, false);

		std::vector<float> chunked = readChunked(q, 14, 1, 3);
		expectSame(chunked, expected, 14, "24->48 mono", 3, true);

		CHECK(q.getQueueCount() == 0);
		aS.stop(h);
	}

	// 48 kHz mono -> 32 kHz mono (non-integer ratio).
	{
		SoLoud::Queue q;
		SoLoud::handle h;
		setupQueue(aS, q, 32000, 1, h);

		Clip clip(7, 48000, 1);
		clip.setData(0, std::vector<float>{1.0f, -3.0f, 6.0f, 2.0f, -8.0f, 4.0f, 9.0f});
		CHECK(q.play(clip) == SoLoud::SO_NO_ERROR);
		CHECK(q.play(clip) == SoLoud::SO_NO_ERROR);

		std::vector<float> expected = reference(clip, 32000, 1);
		CHECK(expected.size() == 5);

		std::vector<float> whole = readWhole(q, 5, 1);
		expectSame(whole, expected, 5, "48->32 mono", 5, false);

		std::vector<float> chunked = readChunked(q, 5, 1, 2);
		expectSame(chunked, expected, 5, "48->32 mono", 2, true);

		CHECK(q.getQueueCount() == 0);
		aS.stop(h);
	}

	// Stereo -> mono: interpolate each source channel first, then average.
	{
		SoLoud::Queue q;
		SoLoud::handle h;
		setupQueue(aS, q, 48000, 1, h);

		Clip clip(5, 24000, 2);
		clip.setData(0, std::vector<float>{0.0f, 10.0f, 20.0f, 30.0f, 40.0f});
		clip.setData(1, std::vector<float>{5.0f, 15.0f, 25.0f, 35.0f, 45.0f});
		CHECK(q.play(clip) == SoLoud::SO_NO_ERROR);
		CHECK(q.play(clip) == SoLoud::SO_NO_ERROR);

		std::vector<float> expected = reference(clip, 48000, 1);
		CHECK(expected.size() == 10);

		std::vector<float> whole = readWhole(q, 10, 1);
		expectSame(whole, expected, 10, "stereo 24->48 mono", 10, false);

		std::vector<float> chunked = readChunked(q, 10, 1, 4);
		expectSame(chunked, expected, 10, "stereo 24->48 mono", 4, true);

		aS.stop(h);
	}

	// Mono -> stereo: duplicate, with a different source sequence.
	{
		SoLoud::Queue q;
		SoLoud::handle h;
		setupQueue(aS, q, 32000, 2, h);

		Clip clip(5, 48000, 1);
		clip.setData(0, std::vector<float>{-7.0f, 2.0f, 11.0f, -4.0f, 13.0f});
		CHECK(q.play(clip) == SoLoud::SO_NO_ERROR);
		CHECK(q.play(clip) == SoLoud::SO_NO_ERROR);

		std::vector<float> expected = reference(clip, 32000, 2);
		CHECK(expected.size() == 8); // ceil(5 * 32 / 48) * 2

		std::vector<float> whole = readWhole(q, 4, 2);
		expectSame(whole, expected, 8, "mono 48->32 stereo", 4, false);

		std::vector<float> chunked = readChunked(q, 4, 2, 3);
		expectSame(chunked, expected, 8, "mono 48->32 stereo", 3, true);

		aS.stop(h);
	}

	// Stereo -> stereo, both channels interpolated independently.
	{
		SoLoud::Queue q;
		SoLoud::handle h;
		setupQueue(aS, q, 48000, 2, h);

		Clip clip(6, 32000, 2);
		clip.setData(0, std::vector<float>{-1.0f, 3.0f, -5.0f, 7.0f, -9.0f, 11.0f});
		clip.setData(1, std::vector<float>{2.0f, -4.0f, 6.0f, -8.0f, 10.0f, -12.0f});
		CHECK(q.play(clip) == SoLoud::SO_NO_ERROR);
		CHECK(q.play(clip) == SoLoud::SO_NO_ERROR);

		std::vector<float> expected = reference(clip, 48000, 2);
		CHECK(expected.size() == 18); // ceil(6 * 48 / 32) * 2

		std::vector<float> whole = readWhole(q, 9, 2);
		expectSame(whole, expected, 18, "stereo 32->48 stereo", 9, false);

		std::vector<float> chunked = readChunked(q, 9, 2, 5);
		expectSame(chunked, expected, 18, "stereo 32->48 stereo", 5, true);

		aS.stop(h);
	}
}

static void testSmallAndEmpty(SoLoud::Soloud &aS)
{
	// One-frame clip: ceil(1 * 48 / 24) == 2 frames, both the same sample.
	{
		SoLoud::Queue q;
		SoLoud::handle h;
		setupQueue(aS, q, 48000, 1, h);

		Clip clip(1, 24000, 1);
		clip.setData(0, std::vector<float>{0.75f});
		CHECK(q.play(clip) == SoLoud::SO_NO_ERROR);
		CHECK(q.play(clip) == SoLoud::SO_NO_ERROR);

		std::vector<float> expected = reference(clip, 48000, 1);
		CHECK(expected.size() == 2);
		CHECK(expected[0] == 0.75f && expected[1] == 0.75f);

		std::vector<float> whole = readWhole(q, 2, 1);
		expectSame(whole, expected, 2, "one frame 24->48", 2, false);

		std::vector<float> chunked = readChunked(q, 2, 1, 1);
		expectSame(chunked, expected, 2, "one frame 24->48", 1, true);

		aS.stop(h);
	}

	// One frame at 48 -> 32 produces a single frame.
	{
		SoLoud::Queue q;
		SoLoud::handle h;
		setupQueue(aS, q, 32000, 1, h);

		Clip clip(1, 48000, 1);
		clip.setData(0, std::vector<float>{-0.5f});
		CHECK(q.play(clip) == SoLoud::SO_NO_ERROR);

		std::vector<float> expected = reference(clip, 32000, 1);
		CHECK(expected.size() == 1);
		std::vector<float> whole = readWhole(q, 1, 1);
		expectSame(whole, expected, 1, "one frame 48->32", 1, false);

		aS.stop(h);
	}

	// Empty clips are skipped without producing frames, and do not stop the
	// queue from playing the following clip.
	{
		SoLoud::Queue q;
		SoLoud::handle h;
		setupQueue(aS, q, 48000, 1, h);

		Clip empty(0, 24000, 1);
		Clip a(3, 24000, 1);
		a.setData(0, std::vector<float>{1.0f, 2.0f, 3.0f});
		Clip b(2, 48000, 1);
		b.setData(0, std::vector<float>{7.0f, 8.0f});
		CHECK(q.play(empty) == SoLoud::SO_NO_ERROR);
		CHECK(q.play(a) == SoLoud::SO_NO_ERROR);
		CHECK(q.play(empty) == SoLoud::SO_NO_ERROR);
		CHECK(q.play(b) == SoLoud::SO_NO_ERROR);
		CHECK(q.getQueueCount() == 4);

		std::vector<float> expectedA = reference(a, 48000, 1);
		std::vector<float> expectedB = reference(b, 48000, 1);
		std::vector<float> expected;
		expected.insert(expected.end(), expectedA.begin(), expectedA.end());
		expected.insert(expected.end(), expectedB.begin(), expectedB.end());

		CHECK(expected.size() == 8);
		std::vector<float> out = readWhole(q, 8, 1);
		expectSame(out, expected, 8, "empty clips skipped", 8, false);
		CHECK(q.getQueueCount() == 0);

		aS.stop(h);
	}
}

static void testShortReadsAndStalls(SoLoud::Soloud &aS)
{
	// A source returning at most two frames per call is a short read, not EOF.
	{
		SoLoud::Queue q;
		SoLoud::handle h;
		setupQueue(aS, q, 32000, 1, h);

		Clip clip(9, 48000, 1);
		clip.shortRead = 2;
		clip.setData(0, std::vector<float>{0.0f, 1.0f, -2.0f, 3.0f, -4.0f,
			5.0f, -6.0f, 7.0f, -8.0f});
		CHECK(q.play(clip) == SoLoud::SO_NO_ERROR);

		std::vector<float> expected = reference(clip, 32000, 1);
		CHECK(expected.size() == 6);

		std::vector<float> out = readWhole(q, 6, 1);
		expectSame(out, expected, 6, "short read", 6, false);
		CHECK(q.getQueueCount() == 0);

		aS.stop(h);
	}

	// A source which returns zero frames once while not ended must not spin.
	// The call may return output produced so far; the next call resumes.
	{
		SoLoud::Queue q;
		SoLoud::handle h;
		setupQueue(aS, q, 48000, 1, h);

		Clip clip(4, 24000, 1);
		clip.shortRead = 1;
		clip.stallCall = 1; // second getAudio call
		clip.stallCount = 1;
		clip.setData(0, std::vector<float>{10.0f, 20.0f, 30.0f, 40.0f});
		CHECK(q.play(clip) == SoLoud::SO_NO_ERROR);

		std::vector<float> expected = reference(clip, 48000, 1);
		CHECK(expected.size() == 8);

		unsigned int totalGot = 0;
		unsigned int zeroReturns = 0;
		unsigned int call;
		for (call = 0; call < 20 && totalGot < expected.size(); call++)
		{
			unsigned int want = expected.size() - totalGot;
			if (want > 3)
				want = 3;
			std::vector<float> tmp(want, 0.0f);
			unsigned int got = q.mInstance->getAudio(tmp.data(), want, want);
			if (got == 0)
				zeroReturns++;
			CHECK(got <= want);
			unsigned int i;
			for (i = 0; i < got; i++)
			{
				float diff = std::fabs(tmp[i] - expected[totalGot + i]);
				CHECK(diff <= 1e-4f * (1.0f + std::fabs(expected[totalGot + i])));
			}
			totalGot += got;
		}
		CHECK(totalGot == expected.size());
		// The stalled call returned promptly rather than spinning.
		CHECK(zeroReturns <= 1);
		CHECK(call < 20);

		aS.stop(h);
	}
}

static void testRingWrap(SoLoud::Soloud &aS)
{
	// Fill the ring, drain it completely, then enqueue again so both read and
	// write indices have wrapped.
	SoLoud::Queue q;
	SoLoud::handle h;
	setupQueue(aS, q, 48000, 1, h);

	std::vector<Clip> clips;
	clips.reserve(SOLOUD_QUEUE_MAX);
	unsigned int i;
	for (i = 0; i < SOLOUD_QUEUE_MAX; i++)
	{
		Clip c(1, 48000, 1);
		c.setData(0, std::vector<float>{(float)i});
		clips.push_back(c);
		CHECK(q.play(clips.back()) == SoLoud::SO_NO_ERROR);
	}
	CHECK(q.getQueueCount() == SOLOUD_QUEUE_MAX);

	std::vector<float> first((unsigned int)SOLOUD_QUEUE_MAX, 0.0f);
	unsigned int got = q.mInstance->getAudio(first.data(), SOLOUD_QUEUE_MAX, SOLOUD_QUEUE_MAX);
	CHECK(got == SOLOUD_QUEUE_MAX);
	for (i = 0; i < SOLOUD_QUEUE_MAX; i++)
		CHECK(first[i] == (float)i);
	CHECK(q.getQueueCount() == 0);

	// New items after wrap must not inherit any old data.
	Clip x(2, 24000, 1);
	x.setData(0, std::vector<float>{100.0f, 102.0f});
	Clip y(1, 32000, 1);
	y.setData(0, std::vector<float>{200.0f});
	CHECK(q.play(x) == SoLoud::SO_NO_ERROR);
	CHECK(q.play(y) == SoLoud::SO_NO_ERROR);
	CHECK(q.getQueueCount() == 2);

	std::vector<float> expectedX = reference(x, 48000, 1);
	std::vector<float> expectedY = reference(y, 48000, 1);
	std::vector<float> expected;
	expected.insert(expected.end(), expectedX.begin(), expectedX.end());
	expected.insert(expected.end(), expectedY.begin(), expectedY.end());
	CHECK(expected.size() == 6);

	std::vector<float> second = readWhole(q, 6, 1);
	expectSame(second, expected, 6, "ring wrap", 6, false);
	CHECK(q.getQueueCount() == 0);

	aS.stop(h);
}

static void testStopAndReuse(SoLoud::Soloud &aS)
{
	// Read part of the converted output, then stop. The source has already
	// been fully read into the conversion cache; stop must release it and the
	// queue must be empty when played again.
	SoLoud::Queue q;
	SoLoud::handle h;
	setupQueue(aS, q, 48000, 1, h);

	Clip oldClip(4, 24000, 1);
	oldClip.setData(0, std::vector<float>{50.0f, 60.0f, 70.0f, 80.0f});
	CHECK(q.play(oldClip) == SoLoud::SO_NO_ERROR);
	CHECK(q.isCurrentlyPlaying(oldClip));

	std::vector<float> partial(3, 0.0f);
	unsigned int got = q.mInstance->getAudio(partial.data(), 3, 3);
	CHECK(got == 3);
	CHECK(q.isCurrentlyPlaying(oldClip));

	aS.stop(h);
	CHECK(q.getQueueCount() == 0);

	// Reuse with the same output format and fresh material.
	h = aS.play(q);
	CHECK(h != 0);

	Clip fresh(2, 24000, 1);
	fresh.setData(0, std::vector<float>{-3.0f, 9.0f});
	Clip fresh2(1, 48000, 1);
	fresh2.setData(0, std::vector<float>{4.0f});
	CHECK(q.play(fresh) == SoLoud::SO_NO_ERROR);
	CHECK(q.play(fresh2) == SoLoud::SO_NO_ERROR);
	CHECK(q.getQueueCount() == 2);

	std::vector<float> expectedFresh = reference(fresh, 48000, 1);
	std::vector<float> expectedFresh2 = reference(fresh2, 48000, 1);
	std::vector<float> expected;
	expected.insert(expected.end(), expectedFresh.begin(), expectedFresh.end());
	expected.insert(expected.end(), expectedFresh2.begin(), expectedFresh2.end());
	CHECK(expected.size() == 5);

	std::vector<float> out = readWhole(q, 5, 1);
	expectSame(out, expected, 5, "stop/reuse", 5, false);
	CHECK(q.getQueueCount() == 0);

	aS.stop(h);
}

static void testCurrentlyPlayingBoundary(SoLoud::Soloud &aS)
{
	// While the last converted frames of an item are still pending it is the
	// current item; once they have all been delivered it is not.
	SoLoud::Queue q;
	SoLoud::handle h;
	setupQueue(aS, q, 48000, 1, h);

	Clip a(1, 24000, 1);
	a.setData(0, std::vector<float>{11.0f});
	Clip b(1, 48000, 1);
	b.setData(0, std::vector<float>{22.0f});
	CHECK(q.play(a) == SoLoud::SO_NO_ERROR);
	CHECK(q.play(b) == SoLoud::SO_NO_ERROR);

	CHECK(q.isCurrentlyPlaying(a));
	CHECK(!q.isCurrentlyPlaying(b));

	float first[1];
	CHECK(q.mInstance->getAudio(first, 1, 1) == 1);
	CHECK(first[0] == 11.0f);
	CHECK(q.isCurrentlyPlaying(a));
	CHECK(!q.isCurrentlyPlaying(b));

	float second[1];
	CHECK(q.mInstance->getAudio(second, 1, 1) == 1);
	CHECK(second[0] == 11.0f);
	CHECK(!q.isCurrentlyPlaying(a));
	CHECK(q.isCurrentlyPlaying(b));

	float third[1];
	CHECK(q.mInstance->getAudio(third, 1, 1) == 1);
	CHECK(third[0] == 22.0f);
	CHECK(q.getQueueCount() == 0);

	aS.stop(h);
}

static void testErrors(SoLoud::Soloud &aS)
{
	// Conversion may only be toggled while there are no queued items and no
	// active QueueInstance.
	{
		SoLoud::Queue q;
		CHECK(q.setAutoConvert(true) == SoLoud::SO_NO_ERROR);

		SoLoud::handle h = aS.play(q);
		CHECK(h != 0);
		// Active instance, even with an empty queue.
		CHECK(q.setAutoConvert(false) == SoLoud::INVALID_PARAMETER);
		aS.stop(h);
		CHECK(q.setAutoConvert(false) == SoLoud::SO_NO_ERROR);
	}

	{
		SoLoud::Queue q;
		SoLoud::handle h;
		setupQueue(aS, q, 48000, 1, h);
		Clip c(1, 48000, 1);
		c.setData(0, std::vector<float>{0.0f});
		CHECK(q.play(c) == SoLoud::SO_NO_ERROR);
		CHECK(q.setAutoConvert(false) == SoLoud::INVALID_PARAMETER);
		aS.stop(h);
	}

	// Unsupported formats are rejected explicitly in conversion mode.
	{
		SoLoud::Queue q;
		SoLoud::handle h;
		setupQueue(aS, q, 48000, 1, h);

		Clip fractional(2, 22050.5f, 1);
		fractional.setData(0, std::vector<float>{1.0f, 2.0f});
		Clip threeCh(2, 48000, 3);
		CHECK(q.play(fractional) == SoLoud::INVALID_PARAMETER);
		CHECK(q.play(threeCh) == SoLoud::INVALID_PARAMETER);
		CHECK(q.getQueueCount() == 0);

		aS.stop(h);
	}

	// The target queue format must also be a positive integer rate with one or
	// two channels, and may not change once playback has begun.
	{
		SoLoud::Queue q;
		CHECK(q.setAutoConvert(true) == SoLoud::SO_NO_ERROR);
		CHECK(q.setParams(44100.5f, 1) == SoLoud::INVALID_PARAMETER);
		CHECK(q.setParams(48000.0f, 3) == SoLoud::INVALID_PARAMETER);
		CHECK(q.setParams(48000.0f, 1) == SoLoud::SO_NO_ERROR);

		SoLoud::handle h = aS.play(q);
		CHECK(h != 0);
		CHECK(q.setParams(24000.0f, 1) == SoLoud::INVALID_PARAMETER);
		CHECK(q.setParamsFromAudioSource(q) == SoLoud::INVALID_PARAMETER);
		aS.stop(h);
	}

	// Disabled mode keeps the old behavior: params may still be changed.
	{
		SoLoud::Queue q;
		CHECK(q.setParams(48000, 1) == SoLoud::SO_NO_ERROR);
		SoLoud::handle h = aS.play(q);
		CHECK(h != 0);
		CHECK(q.setParams(24000, 1) == SoLoud::SO_NO_ERROR);
		aS.stop(h);
	}
}

static void testMixPath(SoLoud::Soloud &aS)
{
	// Exercise the conversion through Soloud's mixer (NULLDRIVER) too.
	SoLoud::Queue q;
	CHECK(q.setParams(48000, 1) == SoLoud::SO_NO_ERROR);
	CHECK(q.setAutoConvert(true) == SoLoud::SO_NO_ERROR);
	// Compensate the mono pan law so the mixed output can be compared against
	// the converted frames directly.
	SoLoud::handle h = aS.play(q, 1.0f / 0.70710678f);
	CHECK(h != 0);
	aS.setPostClipScaler(1.0f);

	Clip clip(3, 24000, 1);
	clip.setData(0, std::vector<float>{0.5f, -0.25f, 0.75f});
	CHECK(q.play(clip) == SoLoud::SO_NO_ERROR);

	std::vector<float> expected = reference(clip, 48000, 1);
	CHECK(expected.size() == 6);

	// The default linear resampler introduces a one sample delay at the start
	// of the stream, so the converted frames appear from mixed[1] onwards.
	float mixed[7] = {0, 0, 0, 0, 0, 0, 0};
	aS.mix(mixed, 7);

	CHECK(std::fabs(mixed[0]) <= 1e-4f);
	unsigned int i;
	for (i = 0; i < 6; i++)
	{
		float diff = std::fabs(mixed[i + 1] - expected[i]);
		if (diff > 1e-4f * (1.0f + std::fabs(expected[i])))
		{
			std::fprintf(stderr, "mix path sample %u got %f expected %f\n", i + 1, mixed[i + 1], expected[i]);
			CHECK(false);
		}
	}

	CHECK(q.getQueueCount() == 0);
	aS.stop(h);
}

int main()
{
	SoLoud::Soloud s;
	CHECK(s.init(0, SoLoud::Soloud::NULLDRIVER, 48000, 512, 1) == SoLoud::SO_NO_ERROR);

	testRateAndChannels(s);
	testSmallAndEmpty(s);
	testShortReadsAndStalls(s);
	testRingWrap(s);
	testStopAndReuse(s);
	testCurrentlyPlayingBoundary(s);
	testErrors(s);
	testMixPath(s);

	CHECK(ok);
	std::puts("QUEUE_CONVERT_OK");
	s.deinit();
	return 0;
}
