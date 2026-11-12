//////////////////////////////////////////////////////////////////////////////////
// Target checks for the real-time gain-control rework.
//
// Groups: equivalence, window, allocation (see checks/realtime-history.md).
//
// The "equivalence" group compares the optimized engine sample-by-sample
// against checks/legacy_engine.cpp, a verbatim copy of the pre-optimization
// engine. That reference keeps the original std::deque/std::queue gain history
// and the original O(filterSize) minimum scan; it never calls into the new
// fixed-size containers.
//
// The "allocation" group installs counting replacements for the global
// allocator entry points, then counts every allocation that happens inside the
// API after initialize() returned, on the no-log code path.
//////////////////////////////////////////////////////////////////////////////////

#include "suite.h"
#include "DynamicAudioNormalizer.h"
#include "legacy_engine.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>
#include <stdexcept>

//Test instrumentation of the optimized engine (compiled only when
//DAN_TEST_INSTRUMENT is defined; see DynamicAudioNormalizer.cpp).
#ifdef DAN_TEST_INSTRUMENT
class MDynamicAudioNormalizer_PrivateData;
extern MDynamicAudioNormalizer_PrivateData *dan_test_create(const uint32_t, const uint32_t, const uint32_t, const uint32_t, const double, const double, const double, const double, const bool, const bool, const bool, FILE *);
extern bool dan_test_stats(const MDynamicAudioNormalizer_PrivateData *, const uint32_t, uint64_t &, uint64_t &, uint64_t &);
extern bool dan_test_process(MDynamicAudioNormalizer_PrivateData *, const double *const *const, double *const *const, const int64_t, int64_t &);
extern bool dan_test_processInplace(MDynamicAudioNormalizer_PrivateData *, double *const *const, const int64_t, int64_t &);
extern bool dan_test_flush(MDynamicAudioNormalizer_PrivateData *, double *const *const, const int64_t, int64_t &);
extern bool dan_test_reset(MDynamicAudioNormalizer_PrivateData *);
extern void dan_test_destroy(MDynamicAudioNormalizer_PrivateData *);
#endif

typedef std::vector<double> Samples;
typedef std::vector<Samples> Multichannel;

static const double PI = 3.14159265358979323846264338327950288419716939937510;

///////////////////////////////////////////////////////////////////////////////
// Deterministic test signals
///////////////////////////////////////////////////////////////////////////////

enum SignalKind
{
	SIG_SILENCE = 0,
	SIG_DUALTONE,
	SIG_STEP,
	SIG_PLATEAU,
	SIG_IMPULSES,
	SIG_DRIFT,
	SIG_NOISE
};

struct InputSignal
{
	Multichannel channels;
	uint32_t channels_;
	uint32_t sampleRate;

	double *ptr(const uint32_t c) { return channels[c].data(); }
};

static InputSignal makeSignal(const uint32_t channels, const uint32_t sampleRate, const uint32_t length, const SignalKind kind, const uint32_t seed = 1U)
{
	InputSignal sig;
	sig.channels_ = channels;
	sig.sampleRate = sampleRate;
	sig.channels.resize(channels);
	for(uint32_t c = 0; c < channels; c++)
	{
		sig.channels[c].assign(length, 0.0);
	}

	uint32_t rng = seed * 2654435761U + 12345U;
	for(uint32_t i = 0; i < length; i++)
	{
		rng ^= rng << 13U; rng ^= rng >> 17U; rng ^= rng << 5U;
		const double noise = (double(rng & 0xFFFFU) / 32768.0) - 1.0;
		const double t = double(i) / double(sampleRate);

		for(uint32_t c = 0; c < channels; c++)
		{
			double value = 0.0;
			switch(kind)
			{
			case SIG_SILENCE:
				value = 0.0;
				break;
			case SIG_DUALTONE:
				value = 0.30 * sin(2.0 * PI * 440.0 * t) + 0.20 * sin(2.0 * PI * 1330.0 * t);
				break;
			case SIG_STEP:
				value = ((i % 4000U) < 2000U) ? 0.20 : 0.70;
				break;
			case SIG_PLATEAU:
				value = ((i % 500U) < 250U) ? 0.10 : (((i % 500U) < 400U) ? 0.60 : 0.30);
				break;
			case SIG_IMPULSES:
				value = 0.05 * sin(2.0 * PI * 220.0 * t);
				if(i == 37U) value += 0.90;
				if((length > 50U) && (i == length - 50U)) value += 0.80;
				break;
			case SIG_DRIFT:
				value = noise * 0.10 + (((i / 1000U) % 2U) ? 0.40 : -0.20);
				break;
			case SIG_NOISE:
			default:
				value = noise * 0.35;
				break;
			}
			const double scale = (c == 0U) ? 1.0 : (0.6 + 0.2 * double(c)) * ((c % 2U) ? -1.0 : 1.0);
			sig.channels[c][i] = value * scale;
		}
	}

	return sig;
}

///////////////////////////////////////////////////////////////////////////////
// End-to-end driver
///////////////////////////////////////////////////////////////////////////////

struct Config
{
	uint32_t channels;
	uint32_t sampleRate;
	uint32_t frameLenMsec;
	uint32_t filterSize;
	double peakValue;
	double maxAmplification;
	double targetRms;
	double compressFactor;
	bool channelsCoupled;
	bool enableDCCorrection;
	bool altBoundaryMode;
};

struct RunResult
{
	Multichannel output;
	int64_t totalProduced;
	bool ok;
};

static const uint32_t OUT_CAP = 8192U;     //local output buffers in the checks
static const uint32_t ENGINE_OUT_CAP = 65536U; //whole-block calls can emit that many samples
static const uint32_t MAX_CHANNELS = 16U;

//Static output pool for the generic drivers: no allocation happens at run
//time (important for the allocation group which counts global allocations).
static double g_engineOutput[MAX_CHANNELS][ENGINE_OUT_CAP];

static void readOutBlock(double *const out[MAX_CHANNELS], const uint32_t channels, const int64_t count, Multichannel &dest, const uint32_t destOffset)
{
	for(uint32_t c = 0; c < channels; c++)
	{
		dest[c].resize(destOffset + size_t(count));
		memcpy(&dest[c][destOffset], out[c], sizeof(double) * size_t(count));
	}
}

template<typename ENGINE>
static bool runEngineInternal(ENGINE &engine, const InputSignal &sig, const std::vector<uint32_t> &chunkSizes, const uint32_t flushCapacity, RunResult &result)
{
	const uint32_t ch = sig.channels_;
	const uint32_t length = static_cast<uint32_t>(sig.channels[0].size());
	result.output.assign(ch, Samples());
	result.totalProduced = 0;
	result.ok = false;

	InputSignal input = sig; //working copy; process() never writes to the input
	double *out[MAX_CHANNELS];
	for(uint32_t c = 0; c < ch; c++) out[c] = g_engineOutput[c];

	uint32_t pos = 0U;
	uint32_t chunkIndex = 0U;
	while(pos < length)
	{
		uint32_t len = chunkSizes.empty() ? (length - pos) : chunkSizes[chunkIndex % chunkSizes.size()];
		if(len < 1U) len = 1U;
		if(pos + len > length) len = length - pos;

		double *in[MAX_CHANNELS];
		for(uint32_t c = 0; c < ch; c++) in[c] = &input.channels[c][pos];

		int64_t got = 0;
		if(!engine.process(in, out, int64_t(len), got))
		{
			return false;
		}
		readOutBlock(out, ch, got, result.output, static_cast<uint32_t>(result.totalProduced));
		result.totalProduced += got;
		pos += len;
		chunkIndex++;
	}

	int64_t flushTotal = 0;
	for(;;)
	{
		int64_t got = 0;
		if(!engine.flushBuffer(out, int64_t(flushCapacity), got))
		{
			return false;
		}
		if(got <= 0) break;
		readOutBlock(out, ch, got, result.output, static_cast<uint32_t>(result.totalProduced));
		result.totalProduced += got;
		flushTotal += got;
		if(flushTotal > 4000000)
		{
			return false; //runaway flush
		}
	}

	result.ok = true;
	return true;
}

static bool runEngine(MDynamicAudioNormalizer &engine, const InputSignal &sig, const std::vector<uint32_t> &chunkSizes, const uint32_t flushCapacity, RunResult &result)
{
	return runEngineInternal(engine, sig, chunkSizes, flushCapacity, result);
}

static bool runEngine(LegacyEngine &engine, const InputSignal &sig, const std::vector<uint32_t> &chunkSizes, const uint32_t flushCapacity, RunResult &result)
{
	return runEngineInternal(engine, sig, chunkSizes, flushCapacity, result);
}

static Config defaultConfig(const uint32_t channels, const uint32_t filterSize)
{
	Config cfg;
	cfg.channels = channels;
	cfg.sampleRate = 16000U;
	cfg.frameLenMsec = 500U;
	cfg.filterSize = filterSize;
	cfg.peakValue = 0.95;
	cfg.maxAmplification = 10.0;
	cfg.targetRms = 0.0;
	cfg.compressFactor = 0.0;
	cfg.channelsCoupled = true;
	cfg.enableDCCorrection = false;
	cfg.altBoundaryMode = false;
	return cfg;
}

#ifdef DAN_TEST_INSTRUMENT
//Same driver, but talking to the instrumented private engine of the test build.
static RunResult runHook(const Config &cfg, const InputSignal &sig, const std::vector<uint32_t> &chunks, const uint32_t flushCapacity)
{
	MDynamicAudioNormalizer_PrivateData *engine = dan_test_create(cfg.channels, cfg.sampleRate, cfg.frameLenMsec, cfg.filterSize, cfg.peakValue, cfg.maxAmplification, cfg.targetRms, cfg.compressFactor, cfg.channelsCoupled, cfg.enableDCCorrection, cfg.altBoundaryMode, NULL);
	if(engine == NULL) throw std::runtime_error("dan_test_create() failed");

	const uint32_t ch = sig.channels_;
	const uint32_t length = uint32_t(sig.channels[0].size());
	InputSignal input = sig;
	double *out[MAX_CHANNELS];
	for(uint32_t c = 0; c < ch; c++) out[c] = g_engineOutput[c];

	RunResult result;
	result.output.assign(ch, Samples());
	result.totalProduced = 0;

	uint32_t pos = 0U, chunkIndex = 0U;
	while(pos < length)
	{
		uint32_t len = chunks.empty() ? (length - pos) : chunks[chunkIndex % chunks.size()];
		if(len < 1U) len = 1U;
		if(pos + len > length) len = length - pos;
		double *in[MAX_CHANNELS];
		for(uint32_t c = 0; c < ch; c++) in[c] = &input.channels[c][pos];
		int64_t got = 0;
		if(!dan_test_process(engine, in, out, int64_t(len), got)) throw std::runtime_error("dan_test_process failed");
		readOutBlock(out, ch, got, result.output, uint32_t(result.totalProduced));
		result.totalProduced += got;
		pos += len; chunkIndex++;
	}
	int64_t flushed = 0;
	for(;;)
	{
		int64_t got = 0;
		if(!dan_test_flush(engine, out, int64_t(flushCapacity), got)) throw std::runtime_error("dan_test_flush failed");
		if(got <= 0) break;
		readOutBlock(out, ch, got, result.output, uint32_t(result.totalProduced));
		result.totalProduced += got;
		flushed += got;
		if(flushed > 4000000) throw std::runtime_error("flush runaway");
	}
	result.ok = true;
	dan_test_destroy(engine);
	return result;
}
#endif

static RunResult runNew(const Config &cfg, const InputSignal &sig, const std::vector<uint32_t> &chunks, const uint32_t flushCapacity)
{
	MDynamicAudioNormalizer engine(cfg.channels, cfg.sampleRate, cfg.frameLenMsec, cfg.filterSize, cfg.peakValue, cfg.maxAmplification, cfg.targetRms, cfg.compressFactor, cfg.channelsCoupled, cfg.enableDCCorrection, cfg.altBoundaryMode, NULL);
	if(!engine.initialize()) throw std::runtime_error("initialize() failed");
	RunResult r;
	runEngine(engine, sig, chunks, flushCapacity, r);
	if(!r.ok) throw std::runtime_error("process/flush failed");
	if(!engine.reset()) throw std::runtime_error("reset() failed");
	return r;
}

static RunResult runLegacy(const Config &cfg, const InputSignal &sig, const std::vector<uint32_t> &chunks, const uint32_t flushCapacity)
{
	LegacyEngine engine(cfg.channels, cfg.sampleRate, cfg.frameLenMsec, cfg.filterSize, cfg.peakValue, cfg.maxAmplification, cfg.targetRms, cfg.compressFactor, cfg.channelsCoupled, cfg.enableDCCorrection, cfg.altBoundaryMode, NULL);
	if(!engine.initialize()) throw std::runtime_error("legacy initialize() failed");
	RunResult r;
	runEngine(engine, sig, chunks, flushCapacity, r);
	if(!r.ok) throw std::runtime_error("legacy process/flush failed");
	if(!engine.reset()) throw std::runtime_error("legacy reset() failed");
	return r;
}

static double maxAbsDiff(const Multichannel &a, const Multichannel &b, int64_t &atSample, uint32_t &atChannel)
{
	atSample = -1;
	atChannel = 0U;
	if(a.size() != b.size()) return 1e300;
	double worst = 0.0;
	for(size_t c = 0; c < a.size(); c++)
	{
		if(a[c].size() != b[c].size()) return 1e300;
		for(size_t i = 0; i < a[c].size(); i++)
		{
			const double d = fabs(a[c][i] - b[c][i]);
			if(d > worst)
			{
				worst = d;
				atSample = int64_t(i);
				atChannel = uint32_t(c);
			}
		}
	}
	return worst;
}

static void requireEqual(const RunResult &newR, const RunResult &oldR, const std::string &label, const double tolerance = 1e-12)
{
	if(!newR.ok || !oldR.ok) throw std::runtime_error(label + ": engine call failed");
	if(newR.totalProduced != oldR.totalProduced)
	{
		throw std::runtime_error(label + ": total length differs: new=" + std::to_string(newR.totalProduced) + " legacy=" + std::to_string(oldR.totalProduced));
	}
	int64_t at = -1; uint32_t ch = 0U;
	const double diff = maxAbsDiff(newR.output, oldR.output, at, ch);
	if(!(diff <= tolerance))
	{
		std::string extra;
		for(size_t i = 0; i < 4U && i < newR.output[0].size(); i++)
		{
			extra += " new=" + std::to_string(newR.output[0][i]) + " old=" + std::to_string(oldR.output[0][i]);
		}
		throw std::runtime_error(label + ": max |diff|=" + std::to_string(diff) + " at channel " + std::to_string(ch) + " sample " + std::to_string(at) + extra);
	}
}

///////////////////////////////////////////////////////////////////////////////
// Equivalence group
///////////////////////////////////////////////////////////////////////////////

UNIT_TEST("equivalence", configMatrix)
{
	const uint32_t channelCounts[] = { 1U, 2U, 6U };
	const uint32_t filterSizes[] = { 3U, 31U, 301U };
	const uint32_t lengths[] = { 150U, 800U, 8000U, 40000U };
	const SignalKind kinds[] = { SIG_SILENCE, SIG_DUALTONE, SIG_STEP, SIG_PLATEAU, SIG_IMPULSES, SIG_DRIFT, SIG_NOISE };

	struct Variant { bool coupled; bool dc; bool alt; double compress; };
	const Variant variants[] =
	{
		{ true,  false, false, 0.0 },
		{ false, false, false, 0.0 },
		{ true,  true,  false, 0.0 },
		{ true,  false, true,  0.0 },
		{ true,  true,  true,  0.0 },
		{ true,  false, false, 2.5 },
		{ false, true,  false, 2.5 },
		{ false, false, true,  2.5 },
	};

	int64_t combinations = 0;

	for(size_t vi = 0; vi < sizeof(variants) / sizeof(variants[0]); vi++)
	{
		for(size_t ci = 0; ci < sizeof(channelCounts) / sizeof(channelCounts[0]); ci++)
		{
			for(size_t fi = 0; fi < sizeof(filterSizes) / sizeof(filterSizes[0]); fi++)
			{
				for(size_t li = 0; li < sizeof(lengths) / sizeof(lengths[0]); li++)
				{
					for(size_t ki = 0; ki < sizeof(kinds) / sizeof(kinds[0]); ki++)
					{
						Config cfg = defaultConfig(channelCounts[ci], filterSizes[fi]);
						cfg.channelsCoupled = variants[vi].coupled;
						cfg.enableDCCorrection = variants[vi].dc;
						cfg.altBoundaryMode = variants[vi].alt;
						cfg.compressFactor = variants[vi].compress;
						const InputSignal sig = makeSignal(cfg.channels, cfg.sampleRate, lengths[li], kinds[ki], uint32_t(vi * 97U + ki * 7U + 3U));

						const std::vector<uint32_t> whole;                    //whole block at once
						const std::vector<uint32_t> alternate = { 1U, 7U, 113U };
						const uint32_t flushCapacity = 700U + uint32_t(fi) * 97U + uint32_t(vi) * 13U + uint32_t(ci) * 5U + uint32_t(li) * 3U + uint32_t(ki);

						const std::string tag = "vi=" + std::to_string(vi) + " ci=" + std::to_string(ci) + " fi=" + std::to_string(fi) + " li=" + std::to_string(li) + " ki=" + std::to_string(ki) + " cap=" + std::to_string(flushCapacity);
						const RunResult a = runNew(cfg, sig, whole, flushCapacity);
						const RunResult b = runLegacy(cfg, sig, whole, flushCapacity);
						requireEqual(a, b, "whole " + tag);
						const RunResult c = runNew(cfg, sig, alternate, flushCapacity);
						const RunResult d = runLegacy(cfg, sig, alternate, flushCapacity);
						requireEqual(c, d, "alternate");
						combinations += 2;
					}
				}
			}
		}
	}

	UNIT_CHECK(combinations > 0);
}

//The same block must produce the same total length and the same sample values
//whether it is processed in one call, in inplace mode, or in mixed chunks.
UNIT_TEST("equivalence", chunkingAndInplace)
{
	const uint32_t channelCounts[] = { 1U, 2U, 6U };
	const uint32_t filterSizes[] = { 3U, 31U, 301U };
	const uint32_t lengths[] = { 800U, 8000U };
	for(size_t ci = 0; ci < 3U; ci++)
	{
		for(size_t fi = 0; fi < 3U; fi++)
		{
			for(size_t li = 0; li < 2U; li++)
			{
				Config cfg = defaultConfig(channelCounts[ci], filterSizes[fi]);
				cfg.channelsCoupled = (fi != 1U);
				cfg.enableDCCorrection = (li != 0U);
				cfg.altBoundaryMode = (ci == 2U);
				const InputSignal sig = makeSignal(cfg.channels, cfg.sampleRate, lengths[li], SIG_PLATEAU, uint32_t(11U + ci));

				//reference: the legacy engine with the one-call scheme
				const std::vector<uint32_t> whole;
				const RunResult reference = runLegacy(cfg, sig, whole, 4096U);

				const std::vector<uint32_t> oneCall;
				requireEqual(runNew(cfg, sig, oneCall, 4096U), reference, "one-call");
				const std::vector<uint32_t> alternating = { 1U, 7U, 113U };
				requireEqual(runNew(cfg, sig, alternating, 4096U), reference, "alternating");
			}
		}
	}

	//inplace: the C++ entry point must produce the very same stream
	{
		Config cfg = defaultConfig(2U, 31U);
		cfg.channelsCoupled = false;
		cfg.enableDCCorrection = true;
		const InputSignal sig = makeSignal(2U, cfg.sampleRate, 8000U, SIG_DUALTONE, 5U);
		MDynamicAudioNormalizer inplaceEngine(cfg.channels, cfg.sampleRate, cfg.frameLenMsec, cfg.filterSize, cfg.peakValue, cfg.maxAmplification, cfg.targetRms, cfg.compressFactor, cfg.channelsCoupled, cfg.enableDCCorrection, cfg.altBoundaryMode, NULL);
		UNIT_CHECK(inplaceEngine.initialize());

		InputSignal work = sig;
		Multichannel inplaceOut(2U);
		double *io[MAX_CHANNELS];
		for(uint32_t c = 0; c < 2U; c++) io[c] = work.channels[c].data();
		const uint32_t chunk = 113U;
		uint32_t pos = 0U;
		int64_t produced = 0;
		while(pos < 8000U)
		{
			const uint32_t len = std::min(chunk, 8000U - pos);
			double *ptr[MAX_CHANNELS];
			for(uint32_t c = 0; c < 2U; c++) ptr[c] = &work.channels[c][pos];
			int64_t got = 0;
			UNIT_CHECK(inplaceEngine.processInplace(ptr, int64_t(len), got));
			UNIT_CHECK(got >= 0);
			//in-place output starts at the position just written
			for(uint32_t c = 0; c < 2U; c++)
			{
				inplaceOut[c].resize(size_t(produced + got));
				memcpy(&inplaceOut[c][size_t(produced)], ptr[c], sizeof(double) * size_t(got));
			}
			produced += got;
			pos += len;
		}
		//flush
		double *out[MAX_CHANNELS];
		for(uint32_t c = 0; c < 2U; c++) out[c] = new double[OUT_CAP];
		for(;;)
		{
			int64_t got = 0;
			UNIT_CHECK(inplaceEngine.flushBuffer(out, 700, got));
			if(got <= 0) break;
			for(uint32_t c = 0; c < 2U; c++)
			{
				inplaceOut[c].resize(size_t(produced + got));
				memcpy(&inplaceOut[c][size_t(produced)], out[c], sizeof(double) * size_t(got));
			}
			produced += got;
		}
		for(uint32_t c = 0; c < 2U; c++) delete [] out[c];

		const std::vector<uint32_t> whole;
		const RunResult reference = runLegacy(cfg, sig, whole, 700U);
		RunResult mine; mine.output = inplaceOut; mine.totalProduced = produced; mine.ok = true;
		requireEqual(mine, reference, "inplace");
	}
}

//C entry point must actually process audio and keep the established error
//contract (flush -> process needs a reset()).
UNIT_TEST("equivalence", cBinding)
{
	const uint32_t ch = 2U;
	const int64_t inLength = 300000; //longer than the 248000-sample internal delay
	InputSignal sig = makeSignal(ch, 16000U, uint32_t(inLength), SIG_DUALTONE, 9U);

	MDynamicAudioNormalizer_Handle *handle = MDYNAMICAUDIONORMALIZER_FUNCTION(createInstance)(ch, 16000U, 500U, 31U, 0.95, 10.0, 0.0, 0.0, 1, 0, 0, NULL);
	UNIT_CHECK(handle != NULL);
	uint32_t cfgChannels = 0U, cfgRate = 0U, cfgFrame = 0U, cfgFilter = 0U;
	UNIT_CHECK(MDYNAMICAUDIONORMALIZER_FUNCTION(getConfiguration)(handle, &cfgChannels, &cfgRate, &cfgFrame, &cfgFilter) == 1);
	UNIT_CHECK(cfgChannels == ch);
	UNIT_CHECK(cfgRate == 16000U);
	UNIT_CHECK(cfgFrame == 8000U);
	UNIT_CHECK(cfgFilter == 31U);
	(void)cfgFilter;

	int64_t delay = 0;
	UNIT_CHECK(MDYNAMICAUDIONORMALIZER_FUNCTION(getInternalDelay)(handle, &delay) == 1);
	UNIT_CHECK(delay == 8000 * 31);

	std::vector<double> left(static_cast<size_t>(inLength)), right(static_cast<size_t>(inLength));
	for(int64_t i = 0; i < inLength; i++)
	{
		left[size_t(i)] = sig.channels[0][size_t(i)];
		right[size_t(i)] = sig.channels[1][size_t(i)];
	}
	const double *in[2] = { left.data(), right.data() };
	double outL[4096], outR[4096];
	double *out[2] = { outL, outR };

	//feed everything, collect the produced samples
	Multichannel produced(2U);
	int64_t pos = 0;
	int64_t total = 0;
	while(pos < inLength)
	{
		const int64_t len = std::min(int64_t(4096), inLength - pos);
		const double *inChunk[2] = { left.data() + pos, right.data() + pos };
		int64_t got = 0;
		UNIT_CHECK(MDYNAMICAUDIONORMALIZER_FUNCTION(process)(handle, inChunk, out, len, &got) == 1);
		UNIT_CHECK(got >= 0 && got <= len);
		for(uint32_t c = 0; c < 2U; c++)
		{
			produced[c].resize(size_t(total + got));
			memcpy(&produced[c][size_t(total)], out[c], sizeof(double) * size_t(got));
		}
		total += got;
		pos += len;
	}
	UNIT_CHECK(total > 0);

	//flush drains the delay line
	int64_t flushed = 0;
	for(;;)
	{
		int64_t got = 0;
		UNIT_CHECK(MDYNAMICAUDIONORMALIZER_FUNCTION(flushBuffer)(handle, out, 500, &got) == 1);
		if(got <= 0) break;
		for(uint32_t c = 0; c < 2U; c++)
		{
			produced[c].resize(size_t(total + got));
			memcpy(&produced[c][size_t(total)], out[c], sizeof(double) * size_t(got));
		}
		total += got;
		flushed += got;
	}
	UNIT_CHECK(flushed > 0);
	UNIT_CHECK(total == inLength);

	//the C and C++ entry points must agree sample by sample
	Config cfg = defaultConfig(2U, 31U);
	const RunResult reference = runLegacy(cfg, sig, std::vector<uint32_t>(), 500U);
	RunResult mine; mine.output = produced; mine.totalProduced = total; mine.ok = true;
	requireEqual(mine, reference, "cbinding");

	//after a flush, process() must be rejected until reset()
	int64_t got = 0;
	UNIT_CHECK(MDYNAMICAUDIONORMALIZER_FUNCTION(process)(handle, in, out, 32, &got) == 0);
	UNIT_CHECK(MDYNAMICAUDIONORMALIZER_FUNCTION(reset)(handle) == 1);
	UNIT_CHECK(MDYNAMICAUDIONORMALIZER_FUNCTION(process)(handle, in, out, 32, &got) == 1);

	MDYNAMICAUDIONORMALIZER_FUNCTION(destroyInstance)(&handle);
	UNIT_CHECK(handle == NULL);
}

//A stream cut into differently sized calls must yield the same total length and
//the same samples at the same positions (for a fixed flush call sequence).
UNIT_TEST("equivalence", streamAlignment)
{
	const uint32_t lengths[] = { 150U, 800U, 8000U };
	for(size_t li = 0; li < 3U; li++)
	{
		Config cfg = defaultConfig(2U, 31U);
		cfg.channelsCoupled = false;
		const InputSignal sig = makeSignal(2U, cfg.sampleRate, lengths[li], SIG_IMPULSES, uint32_t(31U + li));

		const std::vector<uint32_t> whole;
		const std::vector<uint32_t> fine = { 1U, 7U, 113U };
		const RunResult a = runNew(cfg, sig, whole, 777U);
		const RunResult b = runNew(cfg, sig, fine, 777U);
		UNIT_CHECK(a.totalProduced == b.totalProduced);
		UNIT_CHECK(a.totalProduced == int64_t(lengths[li]));
		int64_t at = -1; uint32_t ch = 0U;
		UNIT_CHECK(maxAbsDiff(a.output, b.output, at, ch) <= 1e-12);
	}
}

///////////////////////////////////////////////////////////////////////////////
// Window group
///////////////////////////////////////////////////////////////////////////////

#ifdef DAN_TEST_INSTRUMENT
struct GainStats
{
	uint64_t advances[MAX_CHANNELS];
	uint64_t compares[MAX_CHANNELS];
	uint64_t windowOps[MAX_CHANNELS];
};

static void snapshotStats(MDynamicAudioNormalizer_PrivateData *const engine, const uint32_t channels, GainStats &out)
{
	for(uint32_t c = 0; c < channels; c++)
	{
		out.advances[c] = out.compares[c] = out.windowOps[c] = 0U;
		UNIT_CHECK(dan_test_stats(engine, c, out.advances[c], out.compares[c], out.windowOps[c]));
	}
}

static void checkCompareBound(const GainStats &stats, const uint32_t channels, const uint32_t filterSize, const int64_t minAdvances)
{
	for(uint32_t c = 0; c < channels; c++)
	{
		const uint64_t M = stats.advances[c];
		UNIT_CHECK(M >= uint64_t(minAdvances));
		//4M + O(filterSize), with the constant kept small
		UNIT_CHECK(stats.compares[c] <= (4U * M) + uint64_t(filterSize) + 8U);
		UNIT_CHECK(stats.windowOps[c] <= stats.compares[c]);
	}
}
#endif

//Repeated minimum: all gains are equal, so the monotone deque collapses to a
//single candidate. Real gain comparisons must stay at one per advance (plus
//the prefill), enumerating them per channel.
UNIT_TEST("window", repeatedMinimum)
{
#ifdef DAN_TEST_INSTRUMENT
	const uint32_t filterSize = 31U;
	const uint32_t frames = 10U;
	Config cfg = defaultConfig(2U, filterSize);
	cfg.channelsCoupled = false;
	MDynamicAudioNormalizer_PrivateData *engine = dan_test_create(cfg.channels, cfg.sampleRate, cfg.frameLenMsec, cfg.filterSize, cfg.peakValue, cfg.maxAmplification, cfg.targetRms, cfg.compressFactor, cfg.channelsCoupled, cfg.enableDCCorrection, cfg.altBoundaryMode, NULL);
	UNIT_CHECK(engine != NULL);

	InputSignal sig;
	sig.channels.push_back(Samples(80000U, 0.25));
	sig.channels.push_back(Samples(80000U, -0.125));
	sig.channels_ = 2U;
	sig.sampleRate = 16000U;

	RunResult result = runHook(cfg, sig, std::vector<uint32_t>(), 4096U);
	(void)result;
	//runHook destroyed its own instance; use the one created above instead
	{
		double out[2][OUT_CAP];
		double *outPtr[2] = { out[0], out[1] };
		int64_t got = 0;
		const uint32_t n = uint32_t(sig.channels[0].size());
		for(uint32_t pos = 0U; pos < n; pos += 4096U)
		{
			const uint32_t len = std::min(4096U, n - pos);
			double *in[2] = { sig.channels[0].data() + pos, sig.channels[1].data() + pos };
			UNIT_CHECK(dan_test_process(engine, in, outPtr, int64_t(len), got));
		}
		for(;;)
		{
			UNIT_CHECK(dan_test_flush(engine, outPtr, 4096, got));
			if(got <= 0) break;
		}
	}
	GainStats stats; snapshotStats(engine, 2U, stats);
	UNIT_CHECK(stats.advances[0] == stats.advances[1]);
	for(uint32_t c = 0; c < 2U; c++)
	{
		//one comparison per advance plus the one-time O(filterSize) first pass
		UNIT_CHECK(stats.compares[c] <= stats.advances[c] + 2U * filterSize + 8U);
		UNIT_CHECK(stats.windowOps[c] <= stats.advances[c] + 2U * filterSize + 8U);
	}
	checkCompareBound(stats, 2U, filterSize, int64_t(frames));
	dan_test_destroy(engine);
#else
	throw std::runtime_error("window tests need a DAN_TEST_INSTRUMENT build");
#endif
}

//Monotone rise: the window minimum is always the oldest element, so the
//candidate deque grows by one and expires one element per advance. Only the
//prefill scan may cost O(filterSize) extra comparisons.
UNIT_TEST("window", monotoneRamp)
{
#ifdef DAN_TEST_INSTRUMENT
	const uint32_t filterSize = 301U;
	const uint32_t frames = 15U;
	Config cfg = defaultConfig(1U, filterSize);
	Samples data(120000U);
	for(uint32_t i = 0; i < data.size(); i++)
	{
		//short bursts of decreasing amplitude: the gain rises between frames
		const double burst = ((i % 8000U) < 4000U) ? 1.0 : 0.55;
		data[i] = 0.9 * exp(-0.00003 * double(i)) * burst;
	}
	InputSignal sig; sig.channels.push_back(data); sig.channels_ = 1U; sig.sampleRate = 16000U;

	MDynamicAudioNormalizer_PrivateData *engine = dan_test_create(cfg.channels, cfg.sampleRate, cfg.frameLenMsec, cfg.filterSize, cfg.peakValue, cfg.maxAmplification, cfg.targetRms, cfg.compressFactor, cfg.channelsCoupled, cfg.enableDCCorrection, cfg.altBoundaryMode, NULL);
	UNIT_CHECK(engine != NULL);
	{
		double out[OUT_CAP]; double *outPtr[1] = { out };
		int64_t got = 0;
		for(uint32_t pos = 0U; pos < 120000U; pos += 4096U)
		{
			const uint32_t len = std::min(4096U, 120000U - pos);
			double *in[1] = { sig.channels[0].data() + pos };
			UNIT_CHECK(dan_test_process(engine, in, outPtr, int64_t(len), got));
		}
		for(;;) { UNIT_CHECK(dan_test_flush(engine, outPtr, 4096, got)); if(got <= 0) break; }
	}
	GainStats stats; snapshotStats(engine, 1U, stats);
	UNIT_CHECK(stats.advances[0] >= frames);
	checkCompareBound(stats, 1U, filterSize, int64_t(frames));
	dan_test_destroy(engine);
#else
	throw std::runtime_error("window tests need a DAN_TEST_INSTRUMENT build");
#endif
}

//Descending / noisy gains exercise the opposite deque behaviour and the
//wrap-around of the fixed-size rings after many frames.
UNIT_TEST("window", boundaryWrap)
{
#ifdef DAN_TEST_INSTRUMENT
	const uint32_t filterSize = 3U;
	const uint32_t frames = 5U; //40000 samples / 8000 per frame (8 kHz, 500 ms)
	for(uint32_t channels = 1U; channels <= 2U; channels++)
	{
		Config cfg = defaultConfig(channels, filterSize);
		cfg.sampleRate = 8000U;
		cfg.channelsCoupled = false;
		cfg.enableDCCorrection = true;
		MDynamicAudioNormalizer_PrivateData *engine = dan_test_create(cfg.channels, cfg.sampleRate, cfg.frameLenMsec, cfg.filterSize, cfg.peakValue, cfg.maxAmplification, cfg.targetRms, cfg.compressFactor, cfg.channelsCoupled, cfg.enableDCCorrection, cfg.altBoundaryMode, NULL);
		UNIT_CHECK(engine != NULL);
		InputSignal sig = makeSignal(channels, 8000U, 40000U, SIG_NOISE, 77U + channels);
		{
			double out[2][OUT_CAP]; double *outPtr[2] = { out[0], out[1] };
			int64_t got = 0;
			for(uint32_t pos = 0U; pos < 40000U; pos += 4096U)
			{
				const uint32_t len = std::min(4096U, 40000U - pos);
				double *in[2] = { sig.channels[0].data() + pos, sig.channels[1].data() + pos };
				UNIT_CHECK(dan_test_process(engine, in, outPtr, int64_t(len), got));
			}
			for(;;) { UNIT_CHECK(dan_test_flush(engine, outPtr, 4096, got)); if(got <= 0) break; }
		}
		GainStats stats; snapshotStats(engine, channels, stats);
		for(uint32_t c = 0; c < channels; c++) UNIT_CHECK(stats.advances[c] >= frames);
		checkCompareBound(stats, channels, filterSize, int64_t(frames));
		dan_test_destroy(engine);
	}
#else
	throw std::runtime_error("window tests need a DAN_TEST_INSTRUMENT build");
#endif
}

//reset() must restore the histories, the minimum candidates, the counter
//generation and the delay state; the next stream must equal a fresh object.
UNIT_TEST("window", resetAndRestart)
{
#ifdef DAN_TEST_INSTRUMENT
	const uint32_t filterSize = 31U;
	Config cfg = defaultConfig(2U, filterSize);
	cfg.altBoundaryMode = true;
	MDynamicAudioNormalizer_PrivateData *engine = dan_test_create(cfg.channels, cfg.sampleRate, cfg.frameLenMsec, cfg.filterSize, cfg.peakValue, cfg.maxAmplification, cfg.targetRms, cfg.compressFactor, cfg.channelsCoupled, cfg.enableDCCorrection, cfg.altBoundaryMode, NULL);
	UNIT_CHECK(engine != NULL);

	InputSignal first = makeSignal(2U, 16000U, 8000U, SIG_STEP, 3U);
	InputSignal second = makeSignal(2U, 16000U, 8000U, SIG_IMPULSES, 4U);
	double out[2][OUT_CAP]; double *outPtr[2] = { out[0], out[1] };
	int64_t got = 0;

	//first stream: process, then flush (which also needs a reset afterwards)
	for(uint32_t pos = 0U; pos < 8000U; pos += 4096U)
	{
		const uint32_t len = std::min(4096U, 8000U - pos);
		double *in[2] = { first.channels[0].data() + pos, first.channels[1].data() + pos };
		UNIT_CHECK(dan_test_process(engine, in, outPtr, int64_t(len), got));
	}
	for(;;) { UNIT_CHECK(dan_test_flush(engine, outPtr, 4096, got)); if(got <= 0) break; }

	UNIT_CHECK(dan_test_reset(engine));
	GainStats zeroStats; snapshotStats(engine, 2U, zeroStats);
	for(uint32_t c = 0; c < 2U; c++)
	{
		UNIT_CHECK(zeroStats.advances[c] == 0U);
		UNIT_CHECK(zeroStats.compares[c] == 0U);
		UNIT_CHECK(zeroStats.windowOps[c] == 0U);
	}

	//second stream after reset -> same samples as a fresh object
	RunResult r2;
	{
		RunResult tmp; tmp.output.assign(2U, Samples()); tmp.totalProduced = 0; tmp.ok = true;
		uint32_t pos = 0U;
		while(pos < 8000U)
		{
			const uint32_t len = std::min(uint32_t(1U), 8000U - pos); //fine chunks
			double *in[2] = { second.channels[0].data() + pos, second.channels[1].data() + pos };
			UNIT_CHECK(dan_test_process(engine, in, outPtr, int64_t(len), got));
			for(uint32_t c = 0; c < 2U; c++)
			{
				tmp.output[c].resize(size_t(tmp.totalProduced + got));
				memcpy(&tmp.output[c][size_t(tmp.totalProduced)], outPtr[c], sizeof(double) * size_t(got));
			}
			tmp.totalProduced += got;
			pos += len;
		}
		for(;;)
		{
			UNIT_CHECK(dan_test_flush(engine, outPtr, 4096, got));
			if(got <= 0) break;
			for(uint32_t c = 0; c < 2U; c++)
			{
				tmp.output[c].resize(size_t(tmp.totalProduced + got));
				memcpy(&tmp.output[c][size_t(tmp.totalProduced)], outPtr[c], sizeof(double) * size_t(got));
			}
			tmp.totalProduced += got;
		}
		RunResult fresh = runNew(cfg, second, std::vector<uint32_t>(1, 1U), 4096U);
		requireEqual(tmp, fresh, "reset restarts the gain history");
		r2 = tmp;
	}
	GainStats restartStats; snapshotStats(engine, 2U, restartStats);
	UNIT_CHECK(restartStats.advances[0] > 0U && restartStats.advances[0] == restartStats.advances[1]);
	(void)restartStats;
	(void)r2;
	dan_test_destroy(engine);
#else
	throw std::runtime_error("window tests need a DAN_TEST_INSTRUMENT build");
#endif
}

///////////////////////////////////////////////////////////////////////////////
// Allocation group
///////////////////////////////////////////////////////////////////////////////
//
// Counting is done with replacements for the global allocation functions.
// Every counter input, expected value and report buffer is created before
// counting starts; the measured calls below only receive raw pointers.
//
// The counting flag is global, so no unrelated allocation may happen while it
// is set: all containers used by the measured section are built up front.

#include <new>

static volatile bool g_trackAllocations = false;
static volatile unsigned long long g_allocCount = 0U;
static volatile unsigned long long g_freeCount = 0U;

#ifdef __APPLE__
#include <malloc/malloc.h>
static void *counting_malloc(const size_t size)
{
	if(g_trackAllocations) ++g_allocCount;
	return malloc_zone_malloc(malloc_default_zone(), size ? size : 1U);
}

static void counting_free(void *const ptr)
{
	if(g_trackAllocations && (ptr != NULL)) ++g_freeCount;
	if(ptr != NULL) malloc_zone_free(malloc_default_zone(), ptr);
}

static void *counting_realloc(void *const ptr, const size_t size)
{
	if(g_trackAllocations) { ++g_allocCount; if(ptr != NULL) ++g_freeCount; }
	return malloc_zone_realloc(malloc_default_zone(), ptr, size ? size : 1U);
}
#else
static void *counting_malloc(const size_t size)
{
	if(g_trackAllocations) ++g_allocCount;
	if(void *const ptr = std::malloc(size ? size : 1U)) return ptr;
	throw std::bad_alloc();
}

static void counting_free(void *const ptr)
{
	if(g_trackAllocations && (ptr != NULL)) ++g_freeCount;
	std::free(ptr);
}

static void *counting_realloc(void *const ptr, const size_t size)
{
	if(g_trackAllocations) { ++g_allocCount; if(ptr != NULL) ++g_freeCount; }
	return std::realloc(ptr, size ? size : 1U);
}
#endif

void *operator new(std::size_t size) { return counting_malloc(size); }
void *operator new[](std::size_t size) { return counting_malloc(size); }
void *operator new(std::size_t size, const std::nothrow_t &) noexcept { try { return counting_malloc(size); } catch(...) { return NULL; } }
void *operator new[](std::size_t size, const std::nothrow_t &) noexcept { try { return counting_malloc(size); } catch(...) { return NULL; } }
void operator delete(void *ptr) noexcept { counting_free(ptr); }
void operator delete[](void *ptr) noexcept { counting_free(ptr); }
void operator delete(void *ptr, std::size_t) noexcept { counting_free(ptr); }
void operator delete[](void *ptr, std::size_t) noexcept { counting_free(ptr); }
void operator delete(void *ptr, const std::nothrow_t &) noexcept { counting_free(ptr); }
void operator delete[](void *ptr, const std::nothrow_t &) noexcept { counting_free(ptr); }

extern "C" void *malloc(size_t size) { return counting_malloc(size); }
extern "C" void *calloc(size_t count, size_t size) { return counting_malloc(count * size); }
extern "C" void free(void *ptr) { counting_free(ptr); }
extern "C" void *realloc(void *ptr, size_t size) { return counting_realloc(ptr, size); }

struct AllocationSnapshot
{
	unsigned long long allocs;
	unsigned long long frees;
	std::string label; //report buffer, filled before counting starts
};

static void beginMeasurement(AllocationSnapshot &snap, const char *const label)
{
	snap.allocs = snap.frees = 0U;
	snap.label.assign(label);
	g_allocCount = g_freeCount = 0U;
	g_trackAllocations = true;
}

static AllocationSnapshot endMeasurement(void)
{
	g_trackAllocations = false;
	AllocationSnapshot snap;
	snap.allocs = g_allocCount;
	snap.frees = g_freeCount;
	return snap;
}

static std::string snapshotText(const AllocationSnapshot &s)
{
	return s.label + ": allocs=" + std::to_string(s.allocs) + " frees=" + std::to_string(s.frees);
}

static void requireNoAllocations(const AllocationSnapshot &s, const char *const stage)
{
	if((s.allocs != 0U) || (s.frees != 0U))
	{
		throw std::runtime_error(std::string("allocation during ") + stage + " -> " + snapshotText(s));
	}
}

//Post-initialize hot path: process, processInplace, flushBuffer and reset must
//not allocate or free anything on the no-log path.
UNIT_TEST("allocation", hotPath)
{
#ifdef DAN_TEST_INSTRUMENT
	const uint32_t channels = 2U;
	const uint32_t filterSize = 31U;

	//All buffers for the measured calls are prepared *before* counting starts.
	MDynamicAudioNormalizer_PrivateData *const engine = dan_test_create(channels, 16000U, 500U, filterSize, 0.95, 10.0, 0.0, 0.0, true, false, false, NULL);
	UNIT_CHECK(engine != NULL);

	InputSignal sig = makeSignal(channels, 16000U, 40000U, SIG_DUALTONE, 12U);
	InputSignal work = sig;
	const uint32_t n = uint32_t(sig.channels[0].size());

	double out[2][OUT_CAP];
	double *outPtr[2] = { out[0], out[1] };
	int64_t got = 0;

	AllocationSnapshot postInit;
	beginMeasurement(postInit, "initialize->process");
	for(uint32_t pos = 0U; pos < n; pos += 4096U)
	{
		const uint32_t len = std::min(4096U, n - pos);
		double *inChunk[2] = { sig.channels[0].data() + pos, sig.channels[1].data() + pos };
		UNIT_CHECK(dan_test_process(engine, inChunk, outPtr, int64_t(len), got));
		UNIT_CHECK(got >= 0);
	}
	const AllocationSnapshot afterProcess = endMeasurement();
	postInit.label.clear(); //no allocation while the flag is clear
	requireNoAllocations(postInit, "initialize->process");
	requireNoAllocations(afterProcess, "process");

	//flushBuffer: synthetic frames keep the delay line moving
	beginMeasurement(postInit, "flush");
	int64_t flushTotal = 0;
	for(;;)
	{
		UNIT_CHECK(dan_test_flush(engine, outPtr, 2048, got));
		if(got <= 0) break;
		flushTotal += got;
		if(flushTotal > 4000000) throw std::runtime_error("flush runaway");
	}
	const AllocationSnapshot afterFlush = endMeasurement();
	postInit.label.clear();
	requireNoAllocations(afterFlush, "flushBuffer");
	UNIT_CHECK(flushTotal > 0);

	//reset: all histories and the delay state are restored
	beginMeasurement(postInit, "reset");
	UNIT_CHECK(dan_test_reset(engine));
	const AllocationSnapshot afterReset = endMeasurement();
	postInit.label.clear();
	requireNoAllocations(afterReset, "reset");

	//processInplace on the same preallocated buffers
	InputSignal work2 = work;
	beginMeasurement(postInit, "processInplace");
	uint32_t pos2 = 0U;
	int64_t producedInplace = 0;
	while(pos2 < n)
	{
		const uint32_t len = std::min(4096U, n - pos2);
		double *io[2] = { work2.channels[0].data() + pos2, work2.channels[1].data() + pos2 };
		UNIT_CHECK(dan_test_processInplace(engine, io, int64_t(len), got));
		UNIT_CHECK(got >= 0);
		producedInplace += got;
		pos2 += len;
	}
	const AllocationSnapshot afterInplace = endMeasurement();
	postInit.label.clear();
	requireNoAllocations(afterInplace, "processInplace");
	UNIT_CHECK(producedInplace >= 0);

	dan_test_destroy(engine);
#else
	throw std::runtime_error("allocation tests need a DAN_TEST_INSTRUMENT build");
#endif
}

//Many ring wrap-arounds: a long stream processed in alternating chunks, with a
//flush, a reset and a second stream afterwards. Every stage is reported.
UNIT_TEST("allocation", ringWrap)
{
#ifdef DAN_TEST_INSTRUMENT
	const uint32_t channels = 2U;
	const uint32_t filterSize = 31U;
	const uint32_t frameLen = 8000U;

	MDynamicAudioNormalizer_PrivateData *const engine = dan_test_create(channels, 16000U, 500U, filterSize, 0.95, 10.0, 0.0, 0.0, true, false, false, NULL);
	UNIT_CHECK(engine != NULL);

	//long enough that the fixed rings wrap many times
	InputSignal sig = makeSignal(channels, 16000U, 24U * frameLen, SIG_DRIFT, 21U);
	InputSignal work = sig;
	const uint32_t n = uint32_t(sig.channels[0].size());
	const uint32_t chunkPattern[3] = { 1U, 7U, 113U };

	double out[2][OUT_CAP];
	double *outPtr[2] = { out[0], out[1] };
	int64_t got = 0;

	AllocationSnapshot snap;
	beginMeasurement(snap, "long stream (chunked)");
	uint32_t pos = 0U;
	uint32_t chunkIndex = 0U;
	while(pos < n)
	{
		const uint32_t len = std::min(chunkPattern[chunkIndex % 3U], n - pos);
		double *inChunk[2] = { sig.channels[0].data() + pos, sig.channels[1].data() + pos };
		UNIT_CHECK(dan_test_process(engine, inChunk, outPtr, int64_t(len), got));
		UNIT_CHECK(got >= 0);
		pos += len;
		chunkIndex++;
	}
	const AllocationSnapshot afterLong = endMeasurement();
	snap.label.clear();
	requireNoAllocations(afterLong, "chunked stream");

	beginMeasurement(snap, "flush after stream");
	int64_t flushed = 0;
	for(;;)
	{
		UNIT_CHECK(dan_test_flush(engine, outPtr, 512, got));
		if(got <= 0) break;
		flushed += got;
		if(flushed > 4000000) throw std::runtime_error("flush runaway");
	}
	const AllocationSnapshot afterFlush = endMeasurement();
	snap.label.clear();
	requireNoAllocations(afterFlush, "flush");
	UNIT_CHECK(flushed > 0);

	beginMeasurement(snap, "reset after flush");
	UNIT_CHECK(dan_test_reset(engine));
	const AllocationSnapshot afterReset = endMeasurement();
	snap.label.clear();
	requireNoAllocations(afterReset, "reset");

	beginMeasurement(snap, "second stream after reset");
	pos = 0U; chunkIndex = 0U;
	while(pos < n)
	{
		const uint32_t len = std::min(chunkPattern[chunkIndex % 3U], n - pos);
		double *inChunk[2] = { work.channels[0].data() + pos, work.channels[1].data() + pos };
		UNIT_CHECK(dan_test_process(engine, inChunk, outPtr, int64_t(len), got));
		UNIT_CHECK(got >= 0);
		pos += len;
		chunkIndex++;
	}
	const AllocationSnapshot afterRestart = endMeasurement();
	snap.label.clear();
	requireNoAllocations(afterRestart, "second stream");

	dan_test_destroy(engine);
#else
	throw std::runtime_error("allocation tests need a DAN_TEST_INSTRUMENT build");
#endif
}

//The public C++ entry points (constructor/initialize/process/flush/reset) are
//exercised through the real class, not the test hook. Allocation counting
//starts as soon as initialize() returns.
UNIT_TEST("allocation", publicApi)
{
	const uint32_t channels = 6U;
	const uint32_t filterSize = 301U;
	MDynamicAudioNormalizer engine(channels, 16000U, 250U, filterSize, 0.95, 10.0, 0.0, 2.0, false, true, true, NULL);
	UNIT_CHECK(engine.initialize());

	InputSignal sig = makeSignal(channels, 16000U, 40000U, SIG_PLATEAU, 33U);
	const uint32_t n = uint32_t(sig.channels[0].size());
	double out[16][OUT_CAP];
	double *outPtr[16];
	for(uint32_t c = 0; c < channels; c++) outPtr[c] = out[c];
	int64_t got = 0;

	AllocationSnapshot snap;
	beginMeasurement(snap, "public API processing");
	uint32_t pos = 0U;
	while(pos < n)
	{
		const uint32_t len = std::min(4096U, n - pos);
		double *inChunk[16];
		for(uint32_t c = 0; c < channels; c++) inChunk[c] = sig.channels[c].data() + pos;
		UNIT_CHECK(engine.process(inChunk, outPtr, int64_t(len), got));
		UNIT_CHECK(got >= 0);
		pos += len;
	}
	const AllocationSnapshot afterProcess = endMeasurement();
	snap.label.clear();
	requireNoAllocations(afterProcess, "public process");

	beginMeasurement(snap, "public flush");
	int64_t flushed = 0;
	for(;;)
	{
		UNIT_CHECK(engine.flushBuffer(outPtr, 1000, got));
		if(got <= 0) break;
		flushed += got;
		if(flushed > 4000000) throw std::runtime_error("flush runaway");
	}
	const AllocationSnapshot afterFlush = endMeasurement();
	snap.label.clear();
	requireNoAllocations(afterFlush, "public flushBuffer");

	beginMeasurement(snap, "public reset");
	UNIT_CHECK(engine.reset());
	const AllocationSnapshot afterReset = endMeasurement();
	snap.label.clear();
	requireNoAllocations(afterReset, "public reset");
}

//Logging enabled through the global user callback: the hot path must stay
//allocation-free too (the log triplets are recorded in preallocated rings).
UNIT_TEST("allocation", withLogCallback)
{
#ifdef DAN_TEST_INSTRUMENT
	static int s_logMessages = 0;
	struct CallbackHolder
	{
		static void callback(const int, const char *const) { ++s_logMessages; }
	};
	MDynamicAudioNormalizer::setLogFunction(&CallbackHolder::callback);

	const uint32_t channels = 2U;
	const uint32_t filterSize = 3U;
	MDynamicAudioNormalizer_PrivateData *const engine = dan_test_create(channels, 16000U, 500U, filterSize, 0.95, 10.0, 0.0, 0.0, true, false, false, NULL);
	UNIT_CHECK(engine != NULL);

	InputSignal sig = makeSignal(channels, 16000U, 40000U, SIG_STEP, 44U);
	const uint32_t n = uint32_t(sig.channels[0].size());
	double out[2][OUT_CAP];
	double *outPtr[2] = { out[0], out[1] };
	int64_t got = 0;

	AllocationSnapshot snap;
	beginMeasurement(snap, "log callback processing");
	uint32_t pos = 0U;
	while(pos < n)
	{
		const uint32_t len = std::min(337U, n - pos);
		double *inChunk[2] = { sig.channels[0].data() + pos, sig.channels[1].data() + pos };
		UNIT_CHECK(dan_test_process(engine, inChunk, outPtr, int64_t(len), got));
		UNIT_CHECK(got >= 0);
		pos += len;
	}
	const AllocationSnapshot afterProcess = endMeasurement();
	snap.label.clear();
	requireNoAllocations(afterProcess, "log callback process");

	beginMeasurement(snap, "log callback flush");
	int64_t flushed = 0;
	for(;;)
	{
		UNIT_CHECK(dan_test_flush(engine, outPtr, 900, got));
		if(got <= 0) break;
		flushed += got;
		if(flushed > 4000000) throw std::runtime_error("flush runaway");
	}
	const AllocationSnapshot afterFlush = endMeasurement();
	snap.label.clear();
	requireNoAllocations(afterFlush, "log callback flush");

	MDynamicAudioNormalizer::setLogFunction(NULL);
	UNIT_CHECK(s_logMessages > 0);
	dan_test_destroy(engine);
#else
	throw std::runtime_error("allocation tests need a DAN_TEST_INSTRUMENT build");
#endif
}

//File logging must keep working: with a log file the same triplets
//(original/minimum/smoothed) at the same analysis instants are written. The
//header lines differ only in that both engines write the same header; compare
//the data lines byte for byte.
UNIT_TEST("equivalence", logFileTriplets)
{
	const std::string pathNew = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") + "/dan_log_new.txt";
	const std::string pathOld = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") + "/dan_log_old.txt";

	const uint32_t ch = 2U;
	const uint32_t filterSize = 31U;
	InputSignal sig = makeSignal(ch, 16000U, 40000U, SIG_PLATEAU, 55U);

	{
		FILE *const f = fopen(pathNew.c_str(), "w");
		UNIT_CHECK(f != NULL);
		MDynamicAudioNormalizer engine(ch, 16000U, 500U, filterSize, 0.95, 10.0, 0.0, 0.0, true, false, false, f);
		UNIT_CHECK(engine.initialize());
		double out[2][OUT_CAP]; double *op[2] = { out[0], out[1] };
		for(uint32_t pos = 0U; pos < 40000U; pos += 4096U)
		{
			const uint32_t len = std::min(4096U, 40000U - pos);
			double *ip[2] = { sig.channels[0].data() + pos, sig.channels[1].data() + pos };
			int64_t got = 0;
			UNIT_CHECK(engine.process(ip, op, int64_t(len), got));
		}
		for(;;) { int64_t got = 0; UNIT_CHECK(engine.flushBuffer(op, 4096, got)); if(got <= 0) break; }
		fclose(f);
	}
	{
		FILE *const f = fopen(pathOld.c_str(), "w");
		UNIT_CHECK(f != NULL);
		LegacyEngine engine(ch, 16000U, 500U, filterSize, 0.95, 10.0, 0.0, 0.0, true, false, false, f);
		UNIT_CHECK(engine.initialize());
		double out[2][OUT_CAP]; double *op[2] = { out[0], out[1] };
		for(uint32_t pos = 0U; pos < 40000U; pos += 4096U)
		{
			const uint32_t len = std::min(4096U, 40000U - pos);
			double *ip[2] = { sig.channels[0].data() + pos, sig.channels[1].data() + pos };
			int64_t got = 0;
			UNIT_CHECK(engine.process(ip, op, int64_t(len), got));
		}
		for(;;) { int64_t got = 0; UNIT_CHECK(engine.flushBuffer(op, 4096, got)); if(got <= 0) break; }
		fclose(f);
	}

	FILE *const fn = fopen(pathNew.c_str(), "r");
	FILE *const fo = fopen(pathOld.c_str(), "r");
	UNIT_CHECK(fn != NULL && fo != NULL);
	{
		char lineNew[4096], lineOld[4096];
		int dataLines = 0;
		while(fgets(lineNew, sizeof(lineNew), fn) && fgets(lineOld, sizeof(lineOld), fo))
		{
			if((lineNew[0] == 'D') || (lineNew[0] == 'C')) continue; //header lines
			dataLines++;
			UNIT_CHECK(strcmp(lineNew, lineOld) == 0);
		}
		UNIT_CHECK(dataLines > 0);
	}
	fclose(fn); fclose(fo);
	remove(pathNew.c_str());
	remove(pathOld.c_str());
}
