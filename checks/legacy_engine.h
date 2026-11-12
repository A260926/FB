//////////////////////////////////////////////////////////////////////////////////
// Reference (pre-optimization) gain engine, kept for compatibility checks only.
//
// This is a verbatim copy of DynamicAudioNormalizerAPI/src/DynamicAudioNormalizer.cpp
// as it was before the real-time allocation rework (commit c152491). Only the
// class names were prefixed with "Legacy" so that both engines can be linked
// into the same test binary. It must never be used by the shipping library;
// checks/target.cpp compares its sample-by-sample output against the new engine.
//
// The copy deliberately keeps the original std::deque/std::queue based gain
// history and the O(filterSize) minimum scan, exactly as it was.
//////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdio>
#include <stdint.h>

class LegacyEngine_PrivateData;

class LegacyEngine
{
public:
	LegacyEngine(const uint32_t channels, const uint32_t sampleRate, const uint32_t frameLenMsec = 500, const uint32_t filterSize = 31, const double peakValue = 0.95, const double maxAmplification = 10.0, const double targetRms = 0.0, const double compressFactor = 0.0, const bool channelsCoupled = true, const bool enableDCCorrection = false, const bool altBoundaryMode = false, FILE *const logFile = NULL);
	virtual ~LegacyEngine(void);

	bool initialize(void);
	bool process(const double *const *const samplesIn, double *const *const samplesOut, const int64_t inputSize, int64_t &outputSize);
	bool processInplace(double *const *const samplesInOut, const int64_t inputSize, int64_t &outputSize);
	bool flushBuffer(double *const *const samplesOut, const int64_t bufferSize, int64_t &outputSize);
	bool reset(void);
	bool getConfiguration(uint32_t &channels, uint32_t &sampleRate, uint32_t &frameLen, uint32_t &filterSize);
	bool getInternalDelay(int64_t &delayInSamples);

	typedef void (LogFunction)(const int logLevel, const char *const message);
	static LogFunction *setLogFunction(LogFunction *const logFunction);
	static void getVersionInfo(uint32_t &major, uint32_t &minor, uint32_t &patch);
	static void getBuildInfo(const char **const date, const char **const time, const char **const compiler, const char **const arch, bool &debug);

private:
	LegacyEngine(const LegacyEngine&) : p(NULL) { throw "unsupported"; }
	LegacyEngine &operator=(const LegacyEngine&) { throw "unsupported"; }

	LegacyEngine_PrivateData *const p;
};
