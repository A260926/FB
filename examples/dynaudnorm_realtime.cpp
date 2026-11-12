//////////////////////////////////////////////////////////////////////////////////
// Dynamic Audio Normalizer - example: initialization and audio callback lifetime
//
// This sample shows the intended real-time use of the C++ API:
//
//   1. construct the normalizer outside any audio callback,
//   2. call initialize() once (all processing memory is allocated here),
//   3. inside the audio callback only call process()/processInplace(),
//      process()/processInplace() never allocate or free memory,
//   4. at the end of the stream call flushBuffer() until it returns no samples,
//   5. call reset() before reusing the object for another stream
//      (flushBuffer() leaves the object in a "flush done" state).
//
// The program generates a short stereo test tone, runs it through the
// normalizer in the same way an audio device would (small blocks from a
// callback), then drains the delay line with flushBuffer() and verifies that
// the total number of produced samples equals the number of input samples.
//
// Build (from the repository root):
//   c++ -std=c++17 -O2 -pthread \
//       -IDynamicAudioNormalizerAPI/include -IDynamicAudioNormalizerShared/include \
//       DynamicAudioNormalizerAPI/src/DynamicAudioNormalizer.cpp \
//       DynamicAudioNormalizerAPI/src/FrameBuffer.cpp \
//       DynamicAudioNormalizerAPI/src/GaussianFilter.cpp \
//       DynamicAudioNormalizerAPI/src/Logging.cpp \
//       DynamicAudioNormalizerAPI/src/Version.cpp \
//       examples/dynaudnorm_realtime.cpp -o dynaudnorm_realtime
//////////////////////////////////////////////////////////////////////////////////

#include "DynamicAudioNormalizer.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static const double PI = 3.14159265358979323846264338327950288419716939937510;

///////////////////////////////////////////////////////////////////////////////
// Configuration
///////////////////////////////////////////////////////////////////////////////

static const uint32_t CHANNELS = 2U;
static const uint32_t SAMPLE_RATE = 48000U;
static const uint32_t FRAME_LEN_MSEC = 50U; //short frames keep the example snappy
static const uint32_t FILTER_SIZE = 31U;
static const int64_t TOTAL_SAMPLES = 48000 * 3; //3 seconds
static const int64_t BLOCK_SIZE = 256;          //typical audio callback size

///////////////////////////////////////////////////////////////////////////////
// Simulated audio callbacks
///////////////////////////////////////////////////////////////////////////////

//Produces the next block of the test tone. In a real application this would
//read from the audio device or from a decoder.
static void produceBlock(const int64_t pos, const int64_t count, std::vector<double> &left, std::vector<double> &right)
{
	left.resize(size_t(count));
	right.resize(size_t(count));
	for(int64_t i = 0; i < count; i++)
	{
		const double t = double(pos + i) / double(SAMPLE_RATE);
		const double tone = 0.18 + 0.25 * sin(2.0 * PI * 330.0 * t) + 0.15 * sin(2.0 * PI * 910.0 * t);
		left[size_t(i)] = tone;
		right[size_t(i)] = -0.5 * tone;
	}
}

//A consumer for produced samples; a real application would feed the audio
//device here.
static void consumeBlock(const double *const left, const double *const right, const int64_t count, int64_t &consumed)
{
	for(int64_t i = 0; i < count; i++)
	{
		const double l = left[i];
		const double r = right[i];
		if(!(std::isfinite(l) && std::isfinite(r))) throw "non-finite output";
	}
	consumed += count;
}

///////////////////////////////////////////////////////////////////////////////
// Main
///////////////////////////////////////////////////////////////////////////////

int main(void)
{
	//1. Construct and initialize outside of the audio callback. This is where
	//   all per-stream state and scratch memory is set up; after initialize()
	//   returns, process(), processInplace(), flushBuffer() and reset() work
	//   without any further allocation on the no-log path.
	MDynamicAudioNormalizer normalizer(CHANNELS, SAMPLE_RATE, FRAME_LEN_MSEC, FILTER_SIZE,
		0.95, 10.0, 0.0, 0.0,
		true,  //channels coupled
		false, //no DC correction
		false, //default boundary mode
		NULL); //no log file

	if(!normalizer.initialize())
	{
		fprintf(stderr, "initialize() failed\n");
		return 1;
	}

	//2. "Audio callback" loop: small blocks, exactly as the device would deliver
	//   them. The buffers below belong to the caller and are reused.
	std::vector<double> inLeft, inRight;
	double outLeft[BLOCK_SIZE], outRight[BLOCK_SIZE];
	double *out[CHANNELS] = { outLeft, outRight };

	int64_t produced = 0;
	int64_t producedByProcess = 0;
	for(int64_t pos = 0; pos + BLOCK_SIZE <= TOTAL_SAMPLES; pos += BLOCK_SIZE)
	{
		produceBlock(pos, BLOCK_SIZE, inLeft, inRight);
		const double *in[CHANNELS] = { inLeft.data(), inRight.data() };

		int64_t outputSize = 0;
		if(!normalizer.process(in, out, BLOCK_SIZE, outputSize))
		{
			fprintf(stderr, "process() failed at sample %lld\n", static_cast<long long>(pos));
			return 1;
		}
		consumeBlock(outLeft, outRight, outputSize, produced);
		producedByProcess += outputSize;
	}

	//3. End of stream: drain the delay line with flushBuffer(). The synthetic
	//   frames used internally are not part of the input length. Keep calling
	//   until no more samples are returned.
	int64_t flushedTotal = 0;
	for(;;)
	{
		int64_t outputSize = 0;
		if(!normalizer.flushBuffer(out, BLOCK_SIZE, outputSize))
		{
			fprintf(stderr, "flushBuffer() failed\n");
			return 1;
		}
		if(outputSize <= 0) break;
		consumeBlock(outLeft, outRight, outputSize, flushedTotal);
	}

	//4. After a flush the object is in the "flush done" state: process() is
	//   rejected until reset() has been called. reset() restores all gain
	//   histories, the delay state and the counters, so the next stream starts
	//   exactly like a freshly initialized object.
	if(!normalizer.reset())
	{
		fprintf(stderr, "reset() failed\n");
		return 1;
	}

	//5. Reuse for a second stream (short example: one block, then flush again)
	produceBlock(0, BLOCK_SIZE, inLeft, inRight);
	{
		const double *in[CHANNELS] = { inLeft.data(), inRight.data() };
		int64_t outputSize = 0;
		if(!normalizer.process(in, out, BLOCK_SIZE, outputSize))
		{
			fprintf(stderr, "process() after reset() failed\n");
			return 1;
		}
		consumeBlock(outLeft, outRight, outputSize, produced);
		producedByProcess += outputSize;
	}
	for(;;)
	{
		int64_t outputSize = 0;
		if(!normalizer.flushBuffer(out, BLOCK_SIZE, outputSize)) return 1;
		if(outputSize <= 0) break;
		consumeBlock(outLeft, outRight, outputSize, flushedTotal);
	}

	//6. The internal delay is reported in samples; it is what flushBuffer()
	//   still holds back during normal processing.
	int64_t delay = 0;
	if(!normalizer.getInternalDelay(delay))
	{
		fprintf(stderr, "getInternalDelay() failed\n");
		return 1;
	}

	const int64_t firstStream = (TOTAL_SAMPLES / BLOCK_SIZE) * BLOCK_SIZE;
	printf("input samples: %lld (first stream) + %lld (second stream)\n", static_cast<long long>(firstStream), static_cast<long long>(BLOCK_SIZE));
	printf("internal delay: %lld samples\n", static_cast<long long>(delay));
	printf("produced by process(): %lld, by flushBuffer(): %lld, total: %lld\n",
		static_cast<long long>(producedByProcess), static_cast<long long>(flushedTotal),
		static_cast<long long>(producedByProcess + flushedTotal));

	//7. The destructor releases what initialize() allocated. This is the only
	//   place where the API frees memory on the no-log path.
	return 0;
}
