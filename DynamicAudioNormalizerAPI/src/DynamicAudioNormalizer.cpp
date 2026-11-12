//////////////////////////////////////////////////////////////////////////////////
// Dynamic Audio Normalizer - Audio Processing Library
// Copyright (c) 2014-2019 LoRd_MuldeR <mulder2@gmx.de>. Some rights reserved.
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
//
// http://www.gnu.org/licenses/lgpl-2.1.txt
//////////////////////////////////////////////////////////////////////////////////

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS 1
#endif

#include "DynamicAudioNormalizer.h"

#include "Common.h"
#include "FrameBuffer.h"
#include "GaussianFilter.h"
#include "Logging.h"
#include "Version.h"

#include <cmath>
#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <cfloat>
#include <inttypes.h>

///////////////////////////////////////////////////////////////////////////////
// Constants
///////////////////////////////////////////////////////////////////////////////

static const uint32_t CHANNELS_MIN =  1U;
static const uint32_t CHANNELS_MAX = 16U;

static const uint32_t SAMPLERATE_MIN =   8000U;
static const uint32_t SAMPLERATE_MAX = 192000U;

static const uint32_t FRAMELEN_MIN =      32U;
static const uint32_t FRAMELEN_MAX = 2097152U;

///////////////////////////////////////////////////////////////////////////////
// Static Initializer
///////////////////////////////////////////////////////////////////////////////

namespace DYNAUDNORM_NS
{
	class StaticInitializer
	{
	public:
		StaticInitializer(void)
		{
#if defined(_MSC_VER) && (_MSC_VER >= 1800) && (_MSC_VER < 1900) && defined(_M_X64) && (_M_X64 >= 100)
			// FMA3 support in the MSVC 2013 CRT is broken on Vista and Windows 7 RTM (fixed in SP1).
			// See https://connect.microsoft.com/VisualStudio/feedback/details/987093/x64-log-function-uses-vpsrlq-avx-instruction-without-regard-to-operating-system-so-it-crashes-on-vista-x64
			_set_FMA3_enable(0);
#endif
		}
	};
}

static const DYNAUDNORM_NS::StaticInitializer g_static_init;

///////////////////////////////////////////////////////////////////////////////
// Utility Functions
///////////////////////////////////////////////////////////////////////////////

#undef LOG_AMPFACTORS
#ifdef LOG_AMPFACTORS
static void LOG_VALUE(const double &value, const double &prev, const double &curr, const double &next, const uint32_t &channel, const uint32_t &pos)
{
	static FILE *g_myLogfile = FOPEN(TXT("ampFactors.log"), TXT("w"));
	if(g_myLogfile && (channel == 0) && (pos % 10 == 0))
	{
		fprintf(g_myLogfile, "%.8f\t%.8f %.8f %.8f\n", value, prev, curr, next);
	}
}
#else
#define LOG_VALUE(...) ((void)0)
#endif

template<typename T> static inline T LIMIT(const T &min, const T &val, const T &max)
{
	return std::min(max, std::max(min, val));
}

template<typename T> static inline void UPDATE_MAX(T &max, const T &val)
{
	if (val > max) { max = val; }
}

static inline uint32_t FRAME_SIZE(const uint32_t &sampleRate, const uint32_t &frameLenMsec)
{
	const uint32_t frameSize = static_cast<uint32_t>(round(double(sampleRate) * (double(frameLenMsec) / 1000.0)));
	return frameSize + (frameSize % 2);
}

static inline double UPDATE_VALUE(const double &NEW, const double &OLD, const double &aggressiveness)
{
	assert((aggressiveness >= 0.0) && (aggressiveness <= 1.0));
	return (aggressiveness * NEW) + ((1.0 - aggressiveness) * OLD);
}

static inline double FADE(const double &prev, const double &next, const uint32_t &pos, const double *const fadeFactors[2])
{
	return (fadeFactors[0][pos] * prev) + (fadeFactors[1][pos] * next);
}

static inline double BOUND(const double &threshold, const double &val)
{
	const double SQRT_PI = 0.8862269254527580136490837416705725913987747280611935; //sqrt(PI) / 2.0
	return erf(SQRT_PI * (val / threshold)) * threshold;
}

static inline double POW2(const double &value)
{
	return value * value;
}

#define BOOLIFY(X) ((X) ? "YES" : "NO")
#define POW2(X) ((X)*(X))

///////////////////////////////////////////////////////////////////////////////
// Fixed-size gain history storage
//
// All of these buffers are allocated by initialize() and never grow at run
// time. The capacities are derived from the channel count, the frame length
// and the filter size only.
///////////////////////////////////////////////////////////////////////////////

namespace
{
	//Simple fixed-capacity FIFO of doubles (stores the gain history windows)
	struct DoubleRing
	{
		double *data;
		uint32_t capacity;
		uint32_t head;
		uint32_t count;

		void init(double *const mem, const uint32_t &cap)
		{
			data = mem;
			capacity = cap;
			head = count = 0U;
		}

		void clear(void)
		{
			head = count = 0U;
		}

		inline bool empty(void) const { return count == 0U; }
		inline uint32_t size(void) const { return count; }
		inline const double &at(const uint32_t &i) const { return data[(head + i) % capacity]; }
		inline const double &front(void) const { return data[head]; }

		inline void push_back(const double &value)
		{
			assert(count < capacity);
			data[(head + count) % capacity] = value;
			++count;
		}

		inline void pop_front(void)
		{
			assert(count > 0U);
			head = (head + 1U) % capacity;
			--count;
		}
	};

	//Monotone deque used for the sliding-window minimum. Each element remembers
	//its absolute position so that the outgoing element can be dropped in O(1).
	struct MinSlidingWindow
	{
		double *values;
		uint64_t *positions;
		uint32_t capacity;
		uint32_t head;
		uint32_t count;

		void init(double *const valMem, uint64_t *const posMem, const uint32_t &cap)
		{
			values = valMem;
			positions = posMem;
			capacity = cap;
			head = count = 0U;
		}

		void clear(void)
		{
			head = count = 0U;
		}

		inline double backValue(void) const { return values[(head + count - 1U) % capacity]; }
		inline double frontValue(void) const { return values[head]; }
		inline uint64_t frontPosition(void) const { return positions[head]; }

		inline void push_back(const uint64_t &pos, const double &value)
		{
			assert(count < capacity);
			const uint32_t tail = (head + count) % capacity;
			values[tail] = value;
			positions[tail] = pos;
			++count;
		}

		inline void pop_back(void)
		{
			assert(count > 0U);
			--count;
		}

		inline void pop_front(void)
		{
			assert(count > 0U);
			head = (head + 1U) % capacity;
			--count;
		}

		inline void expireBefore(const uint64_t &minPosition)
		{
			while(count > 0U)
			{
				if(frontPosition() >= minPosition) break;
				pop_front();
			}
		}
	};

	struct GainChannel
	{
		DoubleRing original;
		DoubleRing minimum;
		DoubleRing smoothed;
		MinSlidingWindow minWindow;

		uint64_t pushes; //total number of values ever inserted into "original"

		//amplifyFrame() consumes "smoothed" values without regard to the output
		//delay, so the ring must hold every value produced but not yet played
		//out. The bound is (2 * filterSize - 1) plus the frames that can sit in
		//the internal frame buffer; the extra headroom is a safety margin only.

#ifdef DAN_TEST_INSTRUMENT
		uint64_t advances;  //number of gain advances (original insertions)
		uint64_t compares;  //number of actual gain-value comparisons (total)
		uint64_t windowOps; //subset of the above spent on gain comparisons in the minimum stage
#endif

		void clear(void)
		{
			original.clear();
			minimum.clear();
			smoothed.clear();
			minWindow.clear();
			pushes = 0U;
#ifdef DAN_TEST_INSTRUMENT
			advances = compares = windowOps = 0U;
#endif
		}
	};

	struct LogChannel
	{
		DoubleRing original;
		DoubleRing minimum;
		DoubleRing smoothed;

		void clear(void)
		{
			original.clear();
			minimum.clear();
			smoothed.clear();
		}
	};
}

#ifdef DAN_TEST_INSTRUMENT
//Both counters are bumped exactly where a gain-value comparison is performed.
//"compares" covers all of them, "windowOps" only those of the sliding minimum.
#define COUNT_GAIN_COMPARE(STATE) do { ++(STATE).compares; } while(0)
#define COUNT_WINDOW_COMPARE(STATE) do { ++(STATE).compares; ++(STATE).windowOps; } while(0)
#else
#define COUNT_GAIN_COMPARE(STATE) ((void)0)
#define COUNT_WINDOW_COMPARE(STATE) ((void)0)
#endif

///////////////////////////////////////////////////////////////////////////////
// MDynamicAudioNormalizer_PrivateData
///////////////////////////////////////////////////////////////////////////////

class MDynamicAudioNormalizer_PrivateData
{
public:
	MDynamicAudioNormalizer_PrivateData(const uint32_t channels, const uint32_t sampleRate, const uint32_t frameLenMsec, const uint32_t filterSize, const double peakValue, const double maxAmplification, const double targetRms, const double compressThresh, const bool channelsCoupled, const bool enableDCCorrection, const bool altBoundaryMode, FILE *const logFile);
	~MDynamicAudioNormalizer_PrivateData(void);

	bool initialize(void);
	bool process(const double *const *const samplesIn, double *const *const samplesOut, const int64_t inputSize, int64_t &outputSize, const bool &bFlush);
	bool flushBuffer(double *const *const samplesOut, const int64_t bufferSize, int64_t &outputSize);
	bool reset(void);
	bool getConfiguration(uint32_t &channels, uint32_t &sampleRate, uint32_t &frameLen, uint32_t &filterSize);
	bool getInternalDelay(int64_t &delayInSamples);

private:
	const uint32_t m_channels;
	const uint32_t m_sampleRate;
	const uint32_t m_frameLen;
	const uint32_t m_filterSize;
	const uint32_t m_prefillLen;
	const uint32_t m_delay;

	const double m_peakValue;
	const double m_maxAmplification;
	const double m_targetRms;
	const double m_compressFactor;

	const bool m_channelsCoupled;
	const bool m_enableDCCorrection;
	const bool m_altBoundaryMode;

	FILE *const m_logFile;

	bool m_initialized;
	bool m_flushBuffer;

	bool m_loggingCallbackSet;
	bool m_loggingEnabled;

	DYNAUDNORM_NS::FrameFIFO *m_buffSrc;
	DYNAUDNORM_NS::FrameFIFO *m_buffOut;
	
	int64_t m_delayedSamples;

	uint64_t m_sampleCounterTotal;
	uint64_t m_sampleCounterClips;
	uint64_t m_sampleCounterCompr;

	DYNAUDNORM_NS::FrameBuffer *m_frameBuffer;

	GainChannel *m_gainHistory;
	LogChannel *m_loggingData;

	//Backing memory for the fixed-size histories; allocated in initialize()
	uint32_t m_smoothCapacity;
	uint32_t m_minWindowCapacity;

	double *m_gainBuffer;          // m_channels * (3 * filterSize + smoothedCapacity)
	uint64_t *m_gainPositions;     // m_channels * minimumWindowCapacity
	double *m_loggingBuffer;       // m_channels * (3 * logCapacity) (only if logging is enabled)

	DYNAUDNORM_NS::GaussianFilter *m_gaussianFilter;

	double *m_prevAmplificationFactor;
	double *m_dcCorrectionValue;
	double *m_compressThreshold;

	double *m_fadeFactors[2];

#ifdef DAN_TEST_INSTRUMENT
	//Test-only accessors: pure instrumentation, not part of the public interface.
	friend bool dan_test_stats(const MDynamicAudioNormalizer_PrivateData *const, const uint32_t, uint64_t &, uint64_t &, uint64_t &);
	friend bool dan_test_process(MDynamicAudioNormalizer_PrivateData *const, const double *const *const, double *const *const, const int64_t, int64_t &);
	friend bool dan_test_processInplace(MDynamicAudioNormalizer_PrivateData *const, double *const *const, const int64_t, int64_t &);
	friend bool dan_test_flush(MDynamicAudioNormalizer_PrivateData *const, double *const *const, const int64_t, int64_t &);
	friend bool dan_test_reset(MDynamicAudioNormalizer_PrivateData *const);
	friend void dan_test_destroy(MDynamicAudioNormalizer_PrivateData *const);
#endif

	//test-only instrumentation hooks (see the DAN_TEST_INSTRUMENT block below)
	friend MDynamicAudioNormalizer_PrivateData *dan_test_create(const uint32_t, const uint32_t, const uint32_t, const uint32_t, const double, const double, const double, const double, const bool, const bool, const bool, FILE *const);
	void analyzeFrame(DYNAUDNORM_NS::FrameData *const frame);
	void amplifyFrame(DYNAUDNORM_NS::FrameData *const frame);
	
	double getMaxLocalGain(DYNAUDNORM_NS::FrameData *const frame, const uint32_t channel = UINT32_MAX);
	double findPeakMagnitude(DYNAUDNORM_NS::FrameData *const frame, const uint32_t channel = UINT32_MAX);
	double computeFrameRMS(const DYNAUDNORM_NS::FrameData *const frame, const uint32_t channel = UINT32_MAX);
	double computeFrameStdDev(const DYNAUDNORM_NS::FrameData *const frame, const uint32_t channel = UINT32_MAX);
	void updateGainHistory(const uint32_t &channel, const double &currentGainFactor);
	void prefillGainHistory(const uint32_t &channel, const double &currentGainFactor);
	void perfromDCCorrection(DYNAUDNORM_NS::FrameData *const frame, const bool &isFirstFrame);
	void perfromCompression(DYNAUDNORM_NS::FrameData *const frame, const bool &isFirstFrame);
	void writeLogFile(void);
	void printParameters(void);

	static void precalculateFadeFactors(double *const fadeFactors[2], const uint32_t frameLen);
	static double setupCompressThresh(const double &dThreshold);

	inline bool gainHistoryEmpty(void) const { return m_gainHistory[0].original.empty(); }
	inline bool loggingEnabled(void) const { return m_loggingEnabled; }
	inline uint32_t minimumWindowCapacity(void) const { return m_filterSize + m_prefillLen + 1U; }
	inline uint32_t smoothedCapacity(void) const { return (3U * m_filterSize) + 64U; }
	inline uint32_t logCapacity(void) const
	{
		//The logging triplets must survive until the frame is amplified, i.e.
		//the same delay applies as for the smoothed ring.
		return (3U * m_filterSize) + 64U;
	}
};

///////////////////////////////////////////////////////////////////////////////
// Constructor & Destructor
///////////////////////////////////////////////////////////////////////////////

MDynamicAudioNormalizer::MDynamicAudioNormalizer(const uint32_t channels, const uint32_t sampleRate, const uint32_t frameLenMsec, const uint32_t filterSize, const double peakValue, const double maxAmplification, const double targetRms, const double compressThresh, const bool channelsCoupled, const bool enableDCCorrection, const bool altBoundaryMode, FILE *const logFile)
:
	p(new MDynamicAudioNormalizer_PrivateData(channels, sampleRate, frameLenMsec, filterSize, peakValue, maxAmplification, targetRms, compressThresh, channelsCoupled, enableDCCorrection, altBoundaryMode,logFile))
{
	/*nothing to do here*/
}

MDynamicAudioNormalizer_PrivateData::MDynamicAudioNormalizer_PrivateData(const uint32_t channels, const uint32_t sampleRate, const uint32_t frameLenMsec, const uint32_t filterSize, const double peakValue, const double maxAmplification, const double targetRms, const double compressFactor, const bool channelsCoupled, const bool enableDCCorrection, const bool altBoundaryMode, FILE *const logFile)
:
	m_channels(channels),
	m_sampleRate(sampleRate),
	m_frameLen(FRAME_SIZE(sampleRate, frameLenMsec)),
	m_filterSize(LIMIT(3u, filterSize, 301u)),
	m_prefillLen(m_filterSize / 2u),
	m_delay(m_frameLen * m_filterSize),
	m_peakValue(LIMIT(0.01, peakValue, 1.0)),
	m_maxAmplification(LIMIT(1.0, maxAmplification, 100.0)),
	m_targetRms(LIMIT(0.0, targetRms, 1.0)),
	m_compressFactor(compressFactor ? LIMIT(1.0, compressFactor, 30.0) : 0.0),
	m_channelsCoupled(channelsCoupled),
	m_enableDCCorrection(enableDCCorrection),
	m_altBoundaryMode(altBoundaryMode),
	m_logFile(logFile)
{
	// LOG2_DBG("channels: %u",           channels);
	// LOG2_DBG("sampleRate: %u",         sampleRate);
	// LOG2_DBG("frameLenMsec: %u",       frameLenMsec);
	// LOG2_DBG("filterSize: %u",         filterSize);
	// LOG2_DBG("peakValue: %.4f",        peakValue);
	// LOG2_DBG("maxAmplification: %.4f", maxAmplification);
	// LOG2_DBG("targetRms: %.4f",        targetRms);
	// LOG2_DBG("compressFactor: %.4f",   compressFactor);
	// LOG2_DBG("channelsCoupled: %s",    channelsCoupled    ? "YES" : "NO");
	// LOG2_DBG("enableDCCorrection: %s", enableDCCorrection ? "YES" : "NO");
	// LOG2_DBG("altBoundaryMode: %s",    altBoundaryMode    ? "YES" : "NO");
	// LOG2_DBG("logFile: %p",            logFile);
	
	m_initialized = false;
	m_flushBuffer = false;

	m_loggingCallbackSet = false;
	m_loggingEnabled = false;

	m_smoothCapacity = m_minWindowCapacity = 0U;

	m_buffSrc = NULL;
	m_buffOut = NULL;

	m_delayedSamples = 0;
	m_sampleCounterClips = m_sampleCounterTotal = 0;

	m_frameBuffer = NULL;
	m_gaussianFilter = NULL;

	m_gainHistory = NULL;
	m_loggingData = NULL;

	m_gainBuffer = NULL;
	m_gainPositions = NULL;
	m_loggingBuffer = NULL;

	m_prevAmplificationFactor = NULL;
	m_dcCorrectionValue = NULL;
	m_compressThreshold = NULL;

	m_fadeFactors[0] = m_fadeFactors[1] = NULL;
}

MDynamicAudioNormalizer::~MDynamicAudioNormalizer(void)
{
	delete p;
}

MDynamicAudioNormalizer_PrivateData::~MDynamicAudioNormalizer_PrivateData(void)
{
	LOG2_DBG("Processed %" PRIu64 " samples total, clipped %" PRIu64 " samples (%.2f%%).",
		m_sampleCounterTotal,
		m_sampleCounterClips,
		m_sampleCounterTotal ? (double(m_sampleCounterClips) / double(m_sampleCounterTotal) * 100.0) : 0.0
	);

	MY_DELETE(m_buffSrc);
	MY_DELETE(m_buffOut);

	MY_DELETE(m_frameBuffer);
	MY_DELETE(m_gaussianFilter);

	MY_DELETE_ARRAY(m_gainHistory);
	MY_DELETE_ARRAY(m_loggingData);
	MY_DELETE_ARRAY(m_gainBuffer);
	MY_DELETE_ARRAY(m_gainPositions);
	MY_DELETE_ARRAY(m_loggingBuffer);

	MY_DELETE_ARRAY(m_prevAmplificationFactor);
	MY_DELETE_ARRAY(m_dcCorrectionValue);
	MY_DELETE_ARRAY(m_compressThreshold);

	MY_DELETE_ARRAY(m_fadeFactors[0]);
	MY_DELETE_ARRAY(m_fadeFactors[1]);
}

///////////////////////////////////////////////////////////////////////////////
// Public API
///////////////////////////////////////////////////////////////////////////////

bool MDynamicAudioNormalizer::initialize(void)
{
	try
	{
		return p->initialize();
	}
	catch(std::exception &e)
	{
		LOG1_ERR(e.what());
		return false;
	}
}

bool MDynamicAudioNormalizer_PrivateData::initialize(void)
{
	if(m_initialized)
	{
		LOG1_WRN("Already initialized -> ignoring!");
		return true;
	}

	if((m_channels < CHANNELS_MIN) || (m_channels > CHANNELS_MAX))
	{
		LOG2_ERR("Invalid or unsupported channel count. Should be in the %u to %u range!", CHANNELS_MIN, CHANNELS_MAX);
		return false;
	}
	if((m_sampleRate < SAMPLERATE_MIN) || (m_sampleRate > SAMPLERATE_MAX))
	{
		LOG2_ERR("Invalid or unsupported sampling rate. Should be in the %u to %u range!", SAMPLERATE_MIN, SAMPLERATE_MAX);
		return false;
	}
	if((m_frameLen < FRAMELEN_MIN) || (m_frameLen > FRAMELEN_MAX))
	{
		LOG2_ERR("Invalid or unsupported frame size. Should be in the %u to %u range!", FRAMELEN_MIN, FRAMELEN_MAX);
		return false;
	}
	if(m_logFile && (ferror(m_logFile) != 0))
	{
		LOG1_WRN("Specified log file has the error indicator set, no logging information will be created!");
	}

	m_buffSrc = new DYNAUDNORM_NS::FrameFIFO(m_channels, m_frameLen);
	m_buffOut = new DYNAUDNORM_NS::FrameFIFO(m_channels, m_frameLen);

	m_frameBuffer = new DYNAUDNORM_NS::FrameBuffer(m_channels, m_frameLen, m_filterSize + 1);

	//Allocate all run-time scratch space up-front. Sizes depend only on the
	//channel count, the frame length and the filter size.
	const uint32_t minWindowCapacity = minimumWindowCapacity();
	m_smoothCapacity = smoothedCapacity();
	m_minWindowCapacity = minWindowCapacity;
	const uint32_t gainSlotsPerChannel = (3U * m_filterSize) + m_smoothCapacity;

	m_gainHistory = new GainChannel[m_channels];
	m_gainBuffer  = new double[m_channels * gainSlotsPerChannel];
	m_gainPositions = new uint64_t[m_channels * minWindowCapacity];

	memset(m_gainBuffer, 0, sizeof(double) * m_channels * gainSlotsPerChannel);
	memset(m_gainPositions, 0, sizeof(uint64_t) * m_channels * minWindowCapacity);

	for(uint32_t c = 0; c < m_channels; c++)
	{
		double *const base = &m_gainBuffer[c * gainSlotsPerChannel];
		m_gainHistory[c].original.init(&base[0U], m_filterSize);
		m_gainHistory[c].minimum .init(&base[m_filterSize], m_filterSize);
		m_gainHistory[c].smoothed.init(&base[2U * m_filterSize], m_smoothCapacity);
		m_gainHistory[c].minWindow.init(&base[3U * m_filterSize], &m_gainPositions[c * m_minWindowCapacity], m_minWindowCapacity);
		m_gainHistory[c].clear();
	}

	//Logging queues are only set up if a log file or a user log callback exists.
	m_loggingCallbackSet = DYNAUDNORM_NS::hasLoggingHandler();
	m_loggingEnabled = (m_logFile != NULL) || m_loggingCallbackSet;

	m_loggingData = new LogChannel[m_channels];
	m_loggingBuffer = NULL;
	if(m_loggingEnabled)
	{
		const uint32_t logSlotsPerChannel = 3U * logCapacity();
		m_loggingBuffer = new double[m_channels * logSlotsPerChannel];
		memset(m_loggingBuffer, 0, sizeof(double) * m_channels * logSlotsPerChannel);
		for(uint32_t c = 0; c < m_channels; c++)
		{
			double *const base = &m_loggingBuffer[c * logSlotsPerChannel];
			m_loggingData[c].original.init(&base[0U], logCapacity());
			m_loggingData[c].minimum .init(&base[logCapacity()], logCapacity());
			m_loggingData[c].smoothed.init(&base[2U * logCapacity()], logCapacity());
			m_loggingData[c].clear();
		}
	}

	const double sigma = (((double(m_filterSize) / 2.0) - 1.0) / 3.0) + (1.0 / 3.0);
	m_gaussianFilter = new DYNAUDNORM_NS::GaussianFilter(m_filterSize, sigma);

	m_dcCorrectionValue       = new double[m_channels];
	m_prevAmplificationFactor = new double[m_channels];
	m_compressThreshold       = new double[m_channels];

	m_fadeFactors[0] = new double[m_frameLen];
	m_fadeFactors[1] = new double[m_frameLen];
	
	precalculateFadeFactors(m_fadeFactors, m_frameLen);
	printParameters();

	if(m_logFile)
	{
		fprintf(m_logFile, "DynamicAudioNormalizer Logfile v%u.%02u-%u\n", DYNAUDNORM_NS::VERSION_MAJOR, DYNAUDNORM_NS::VERSION_MINOR, DYNAUDNORM_NS::VERSION_PATCH);
		fprintf(m_logFile, "CHANNEL_COUNT:%u\n\n", m_channels);
	}

	m_initialized = true;
	reset();

	return true;
}

#ifdef DAN_TEST_INSTRUMENT
	//Test-only instance creation: exposes the internal state to checks without
	//extending the public interface.
	MDynamicAudioNormalizer_PrivateData *dan_test_create(const uint32_t channels, const uint32_t sampleRate, const uint32_t frameLenMsec, const uint32_t filterSize, const double peakValue, const double maxAmplification, const double targetRms, const double compressFactor, const bool channelsCoupled, const bool enableDCCorrection, const bool altBoundaryMode, FILE *const logFile)
	{
		MDynamicAudioNormalizer_PrivateData *const instance = new MDynamicAudioNormalizer_PrivateData(channels, sampleRate, frameLenMsec, filterSize, peakValue, maxAmplification, targetRms, compressFactor, channelsCoupled, enableDCCorrection, altBoundaryMode, logFile);
		return instance->initialize() ? instance : NULL;
	}

	bool dan_test_stats(const MDynamicAudioNormalizer_PrivateData *const instance, const uint32_t channel, uint64_t &advances, uint64_t &compares, uint64_t &windowOps)
	{
		if((instance == NULL) || (channel >= instance->m_channels)) return false;
		advances  = instance->m_gainHistory[channel].advances;
		compares  = instance->m_gainHistory[channel].compares;
		windowOps = instance->m_gainHistory[channel].windowOps;
		return true;
	}

	bool dan_test_process(MDynamicAudioNormalizer_PrivateData *const instance, const double *const *const samplesIn, double *const *const samplesOut, const int64_t inputSize, int64_t &outputSize)
	{
		if(instance == NULL) return false;
		return instance->process(samplesIn, samplesOut, inputSize, outputSize, false);
	}

	bool dan_test_processInplace(MDynamicAudioNormalizer_PrivateData *const instance, double *const *const samplesInOut, const int64_t inputSize, int64_t &outputSize)
	{
		if(instance == NULL) return false;
		return instance->process(samplesInOut, samplesInOut, inputSize, outputSize, false);
	}

	bool dan_test_flush(MDynamicAudioNormalizer_PrivateData *const instance, double *const *const samplesOut, const int64_t bufferSize, int64_t &outputSize)
	{
		if(instance == NULL) return false;
		return instance->flushBuffer(samplesOut, bufferSize, outputSize);
	}

	bool dan_test_reset(MDynamicAudioNormalizer_PrivateData *const instance)
	{
		if(instance == NULL) return false;
		return instance->reset();
	}

	void dan_test_destroy(MDynamicAudioNormalizer_PrivateData *const instance)
	{
		delete instance;
	}
#endif

bool MDynamicAudioNormalizer::reset(void)
{
	try
	{
		return p->reset();
	}
	catch(std::exception &e)
	{
		LOG1_ERR(e.what());
		return false;
	}
}

bool MDynamicAudioNormalizer_PrivateData::reset(void)
{
	//Check audio normalizer status
	if(!m_initialized)
	{
		LOG1_ERR("Not initialized yet. Must call initialize() first!");
		return false;
	}

	m_delayedSamples = 0;
	m_flushBuffer = false;

	m_buffSrc->reset();
	m_buffOut->reset();

	m_frameBuffer->reset();

	for(uint32_t c = 0; c < m_channels; c++)
	{
		m_gainHistory[c].clear();
		m_loggingData[c].clear();
		m_dcCorrectionValue      [c] = 0.0;
		m_prevAmplificationFactor[c] = 1.0;
		m_compressThreshold      [c] = 0.0;
	}

	return true;
}

bool MDynamicAudioNormalizer::getConfiguration(uint32_t &channels, uint32_t &sampleRate, uint32_t &frameLen, uint32_t &filterSize)
{
	try
	{
		return p->getConfiguration(channels, sampleRate, frameLen, filterSize);
	}
	catch(std::exception &e)
	{
		LOG1_ERR(e.what());
		return false;
	}
}

bool MDynamicAudioNormalizer::getInternalDelay(int64_t &delayInSamples)
{
	try
	{
		return p->getInternalDelay(delayInSamples);
	}
	catch(std::exception &e)
	{
		LOG1_ERR(e.what());
		return false;
	}
}

bool MDynamicAudioNormalizer_PrivateData::getConfiguration(uint32_t &channels, uint32_t &sampleRate, uint32_t &frameLen, uint32_t &filterSize)
{
	//Check audio normalizer status
	if(!m_initialized)
	{
		LOG1_ERR("Not initialized yet. Must call initialize() first!");
		return false;
	}

	channels = m_channels;
	sampleRate = m_sampleRate;
	frameLen = m_frameLen;
	filterSize = m_filterSize;

	return true;
}

bool MDynamicAudioNormalizer_PrivateData::getInternalDelay(int64_t &delayInSamples)
{
	//Check audio normalizer status
	if(!m_initialized)
	{
		LOG1_ERR("Not initialized yet. Must call initialize() first!");
		return false;
	}

	delayInSamples = m_delay; //m_frameLen * m_filterSize;
	return true;
}

bool MDynamicAudioNormalizer::process(const double *const *const samplesIn, double *const *const samplesOut, const int64_t inputSize, int64_t &outputSize)
{
	try
	{
		return p->process(samplesIn, samplesOut, inputSize, outputSize, false);
	}
	catch (std::exception &e)
	{
		LOG1_ERR(e.what());
		return false;
	}
}

bool MDynamicAudioNormalizer::processInplace(double *const *const samplesInOut, const int64_t inputSize, int64_t &outputSize)
{
	try
	{
		return p->process(samplesInOut, samplesInOut, inputSize, outputSize, false);
	}
	catch(std::exception &e)
	{
		LOG1_ERR(e.what());
		return false;
	}
}

bool MDynamicAudioNormalizer_PrivateData::process(const double *const *const samplesIn, double *const *const samplesOut, const int64_t inputSize, int64_t &outputSize, const bool &bFlush)
{
	outputSize = 0;

	//Check audio normalizer status
	if(!m_initialized)
	{
		LOG1_ERR("Not initialized yet. Must call initialize() first!");
		return false;
	}
	if(m_flushBuffer && (!bFlush))
	{
		LOG1_ERR("Must not call processInplace() after flushBuffer(). Call reset() first!");
		return false;
	}

	bool bStop = false;

	uint32_t inputPos = 0;
	uint32_t inputSamplesLeft = static_cast<uint32_t>(LIMIT(int64_t(0), inputSize, int64_t(UINT32_MAX)));
	
	uint32_t outputPos = 0;
	uint32_t outputBufferLeft = 0;

	while(!bStop)
	{
		bStop = true;

		//Read as many input samples as possible
		while((inputSamplesLeft > 0) && (m_buffSrc->samplesLeftPut() > 0))
		{
			bStop = false;
			
			const uint32_t copyLen = std::min(inputSamplesLeft, m_buffSrc->samplesLeftPut());
			m_buffSrc->putSamples(samplesIn, inputPos, copyLen);

			inputPos         += copyLen;
			inputSamplesLeft -= copyLen;
			outputBufferLeft += copyLen;

			if(!bFlush)
			{
				m_delayedSamples += copyLen;
			}
		}

		//Analyze next input frame, if we have enough input
		if(m_buffSrc->samplesLeftGet() >= m_frameLen)
		{
			bStop = false;
			analyzeFrame(m_buffSrc->data());
			
			if(!m_frameBuffer->putFrame(m_buffSrc))
			{
				LOG1_ERR("Failed to append current input frame to internal buffer!");
				return false;
			}

			m_buffSrc->reset();
		}

		//Amplify next output frame, if we have enough output
		if((m_buffOut->samplesLeftPut() >= m_frameLen) && (m_frameBuffer->framesUsed() > 0) && (!m_gainHistory[0].smoothed.empty()))
		{
			bStop = false;
			
			if(!m_frameBuffer->getFrame(m_buffOut))
			{
				LOG1_ERR("Failed to retrieve next output frame from internal buffer!");
				return false;
			}

			amplifyFrame(m_buffOut->data());
		}

		//Write as many output samples as possible
		while((outputBufferLeft > 0) && (m_buffOut->samplesLeftGet() > 0) && (bFlush || (m_delayedSamples > m_delay)))
		{
			bStop = false;

			const uint32_t pending = bFlush ? UINT32_MAX : uint32_t(m_delayedSamples - m_delay);
			const uint32_t copyLen = std::min(std::min(outputBufferLeft, m_buffOut->samplesLeftGet()), pending);
			m_buffOut->getSamples(samplesOut, outputPos, copyLen);

			outputPos        += copyLen;
			outputBufferLeft -= copyLen;
			m_delayedSamples -= copyLen;

			if((m_buffOut->samplesLeftGet() < 1) && (m_buffOut->samplesLeftPut() < 1))
			{
				m_buffOut->reset();
			}
		}
	}

	outputSize = int64_t(outputPos);

	if(inputSamplesLeft > 0)
	{
		LOG1_WRN("No all input samples could be processed -> discarding pending input!");
		return false;
	}

	return true;
}

bool MDynamicAudioNormalizer::flushBuffer(double *const *const samplesOut, const int64_t bufferSize, int64_t &outputSize)
{
	try
	{
		return p->flushBuffer(samplesOut, bufferSize, outputSize);
	}
	catch(std::exception &e)
	{
		LOG1_ERR(e.what());
		return false;
	}
}

bool MDynamicAudioNormalizer_PrivateData::flushBuffer(double *const *const samplesOut, const int64_t bufferSize, int64_t &outputSize)
{
	outputSize = 0;

	//Check audio normalizer status
	if(!m_initialized)
	{
		LOG1_ERR("Not initialized yet. Must call initialize() first!");
		return false;
	}

	m_flushBuffer = true;
	const uint32_t pendingSamples = static_cast<uint32_t>(LIMIT(int64_t(0), std::min(m_delayedSamples, bufferSize), int64_t(UINT32_MAX)));

	if(pendingSamples < 1)
	{
		return true; /*no pending samples left*/
	}

	bool success = false;
	do
	{
		for(uint32_t c = 0; c < m_channels; c++)
		{
			for(uint32_t i = 0; i < pendingSamples; i++)
			{
				samplesOut[c][i] = m_altBoundaryMode ? DBL_EPSILON : ((m_targetRms > DBL_EPSILON) ? std::min(m_peakValue, m_targetRms) : m_peakValue);
				if(m_enableDCCorrection)
				{
					samplesOut[c][i] *= ((i % 2) == 1) ? (-1) : 1;
					samplesOut[c][i] += m_dcCorrectionValue[c];
				}
			}
		}

		success = process(samplesOut, samplesOut, pendingSamples, outputSize, true);
	}
	while(success && (outputSize <= 0));
	
	return success;
}

///////////////////////////////////////////////////////////////////////////////
// Public Static Functions
///////////////////////////////////////////////////////////////////////////////

void MDynamicAudioNormalizer::getVersionInfo(uint32_t &major, uint32_t &minor,uint32_t &patch)
{
	major = DYNAUDNORM_NS::VERSION_MAJOR;
	minor = DYNAUDNORM_NS::VERSION_MINOR;
	patch = DYNAUDNORM_NS::VERSION_PATCH;
}

void MDynamicAudioNormalizer::getBuildInfo(const char **const date, const char **const time, const char **const compiler, const char **const arch, bool &debug)
{
	*date     = DYNAUDNORM_NS::BUILD_DATE;
	*time     = DYNAUDNORM_NS::BUILD_TIME;
	*compiler = DYNAUDNORM_NS::BUILD_COMPILER;
	*arch     = DYNAUDNORM_NS::BUILD_ARCH;
	debug     = bool(DYNAUDNORM_DEBUG);
}

MDynamicAudioNormalizer::LogFunction *MDynamicAudioNormalizer::setLogFunction(MDynamicAudioNormalizer::LogFunction *const logFunction)
{
	return DYNAUDNORM_NS::setLoggingHandler(logFunction);
}

///////////////////////////////////////////////////////////////////////////////
// Procesing Functions
///////////////////////////////////////////////////////////////////////////////

void MDynamicAudioNormalizer_PrivateData::analyzeFrame(DYNAUDNORM_NS::FrameData *const frame)
{
	//Perform DC Correction (optional)
	if(m_enableDCCorrection)
	{
		perfromDCCorrection(frame, gainHistoryEmpty());
	}

	//Perform compression (optional)
	if(m_compressFactor > DBL_EPSILON)
	{
		perfromCompression(frame, gainHistoryEmpty());
	}

	//Find the frame's peak sample value
	if(m_channelsCoupled)
	{
		const double currentGainFactor = getMaxLocalGain(frame);
		for(uint32_t c = 0; c < m_channels; c++)
		{
			updateGainHistory(c, currentGainFactor);
		}
	}
	else
	{
		for(uint32_t c = 0; c < m_channels; c++)
		{
			updateGainHistory(c, getMaxLocalGain(frame, c));
		}
	}

	//Write data to the log file (no-op when neither a file nor a callback exists)
	writeLogFile();
}

void MDynamicAudioNormalizer_PrivateData::amplifyFrame(DYNAUDNORM_NS::FrameData *const frame)
{
	for(uint32_t c = 0; c < m_channels; c++)
	{
		if(m_gainHistory[c].smoothed.empty())
		{
			LOG1_WRN("There are no information available for the current frame!");
			break;
		}

		double *const dataPtr = frame->data(c);
		const double currAmplificationFactor = m_gainHistory[c].smoothed.front();
		m_gainHistory[c].smoothed.pop_front();

		for(uint32_t i = 0; i < m_frameLen; i++)
		{
			const double amplificationFactor = FADE(m_prevAmplificationFactor[c], currAmplificationFactor, i, m_fadeFactors);
			dataPtr[i] *= amplificationFactor;
			LOG_VALUE(amplificationFactor, m_prevAmplificationFactor[c], currAmplificationFactor, nextAmplificationFactor, c, i);
			if(fabs(dataPtr[i]) > m_peakValue)
			{
				m_sampleCounterClips++;
				dataPtr[i] = copysign(m_peakValue, dataPtr[i]); /*fix rare clipping*/
			}
		}
		
		m_prevAmplificationFactor[c] = currAmplificationFactor;
		m_sampleCounterTotal += m_frameLen;
	}
}

///////////////////////////////////////////////////////////////////////////////
// Helper Functions
///////////////////////////////////////////////////////////////////////////////

double MDynamicAudioNormalizer_PrivateData::getMaxLocalGain(DYNAUDNORM_NS::FrameData *const frame, const uint32_t channel)
{
	const double maximumGain = m_peakValue / findPeakMagnitude(frame, channel);
	const double rmsGain = (m_targetRms > DBL_EPSILON) ? (m_targetRms / computeFrameRMS(frame, channel)) : DBL_MAX;
	return BOUND(m_maxAmplification, std::min(maximumGain, rmsGain));
}

double MDynamicAudioNormalizer_PrivateData::findPeakMagnitude(DYNAUDNORM_NS::FrameData *const frame, const uint32_t channel)
{
	double dMax = DBL_EPSILON;

	if (channel == UINT32_MAX)
	{
		for (uint32_t c = 0; c < m_channels; c++)
		{
			double *const dataPtr = frame->data(c);
			for (uint32_t i = 0; i < m_frameLen; i++)
			{
				UPDATE_MAX(dMax, fabs(dataPtr[i]));
			}
		}
	}
	else
	{
		double *const dataPtr = frame->data(channel);
		for (uint32_t i = 0; i < m_frameLen; i++)
		{
			UPDATE_MAX(dMax, fabs(dataPtr[i]));
		}
	}

	return dMax;
}

double MDynamicAudioNormalizer_PrivateData::computeFrameRMS(const DYNAUDNORM_NS::FrameData *const frame, const uint32_t channel)
{
	double rmsValue = 0.0;
	
	if(channel == UINT32_MAX)
	{
		for(uint32_t c = 0; c < m_channels; c++)
		{
			const double *dataPtr = frame->data(c);
			for(uint32_t i = 0; i < m_frameLen; i++)
			{
				rmsValue += POW2(dataPtr[i]);
			}
		}
		rmsValue /= double(m_frameLen * m_channels);
	}
	else
	{
		const double *dataPtr = frame->data(channel);
		for(uint32_t i = 0; i < m_frameLen; i++)
		{
			rmsValue += POW2(dataPtr[i]);
		}
		rmsValue /= double(m_frameLen);
	}

	return std::max(sqrt(rmsValue), DBL_EPSILON);
}

double MDynamicAudioNormalizer_PrivateData::computeFrameStdDev(const DYNAUDNORM_NS::FrameData *const frame, const uint32_t channel)
{
	double variance = 0.0;

	if(channel == UINT32_MAX)
	{
		for(uint32_t c = 0; c < m_channels; c++)
		{
			const double *dataPtr = frame->data(c);
			for(uint32_t i = 0; i < m_frameLen; i++)
			{
				variance += POW2(dataPtr[i]);	//Assume that MEAN is *zero*
			}
		}
		variance /= double((m_channels * m_frameLen) - 1);
	}
	else
	{
		const double *dataPtr = frame->data(channel);
		for(uint32_t i = 0; i < m_frameLen; i++)
		{
			variance += POW2(dataPtr[i]);	//Assume that MEAN is *zero*
		}
		variance /= double(m_frameLen - 1);
	}


	return std::max(sqrt(variance), DBL_EPSILON);
}

void MDynamicAudioNormalizer_PrivateData::prefillGainHistory(const uint32_t &channel, const double &currentGainFactor)
{
	//Pre-fill the gain history
	if(m_gainHistory[channel].original.empty() || m_gainHistory[channel].minimum.empty())
	{
		const double initial_value = m_altBoundaryMode ? currentGainFactor : 1.0;
		m_prevAmplificationFactor[channel] = initial_value;
		while(m_gainHistory[channel].original.size() < m_prefillLen)
		{
			m_gainHistory[channel].original.push_back(initial_value);
		}
	}
}

void MDynamicAudioNormalizer_PrivateData::updateGainHistory(const uint32_t &channel, const double &currentGainFactor)
{
	GainChannel &state = m_gainHistory[channel];
	const uint32_t filterSize = m_filterSize;
	const uint32_t prefillLen = m_prefillLen;

	//Pre-fill the gain history
	prefillGainHistory(channel, currentGainFactor);

	//Insert current gain factor
	const uint64_t pushPosition = state.pushes++;
	//Absolute position: the pre-filled elements occupy the positions before the
	//first real push. Keeping this offset lets the sliding window expire values
	//purely by comparing positions.
	const uint64_t absolutePosition = uint64_t(prefillLen) + pushPosition;
	state.original.push_back(currentGainFactor);
	if(m_loggingEnabled)
	{
		m_loggingData[channel].original.push_back(currentGainFactor);
	}
#ifdef DAN_TEST_INSTRUMENT
	state.advances++;
#endif

	//Apply the minimum filter
	if(state.original.size() >= filterSize)
	{
		assert(state.original.size() == filterSize);
		const uint64_t windowStart = absolutePosition - uint64_t(filterSize - 1U);

		if(state.minimum.empty())
		{
			//First formation of the minimum history. In the legacy code the
			//"minimum" exists neither before the original history has caught up
			//with the pre-fill nor while it is still waiting for the first
			//minimum window. The partial history scans the first window from
			//the second post-prefill element onward.
			if(state.pushes <= uint64_t(prefillLen))
			{
				return;
			}

			double initial_value = m_altBoundaryMode ? state.original.front() : 1.0;
			for(uint32_t i = prefillLen + 1U; i < filterSize; i++)
			{
				COUNT_WINDOW_COMPARE(state);
				initial_value = std::min(initial_value, state.original.at(i));
				state.minimum.push_back(initial_value);
			}

			//Prime the monotone deque with the first window, preserving the
			//order in which the values appear in the original history.
			state.minWindow.clear();
			for(uint32_t i = 0U; i < filterSize; i++)
			{
				const double value = state.original.at(i);
				while(state.minWindow.count > 0U)
				{
					COUNT_WINDOW_COMPARE(state);
					if(state.minWindow.backValue() < value) break;
					state.minWindow.pop_back();
				}
				state.minWindow.push_back(windowStart + uint64_t(i), value);
			}
		}
		else
		{
			//Amortized O(1): drop candidates that have left the window, then
			//insert the new value (equal or larger candidates can never be the
			//minimum again because the new value expires later).
			state.minWindow.expireBefore(windowStart);
			while(state.minWindow.count > 0U)
			{
				COUNT_WINDOW_COMPARE(state);
				if(state.minWindow.backValue() < currentGainFactor) break;
				state.minWindow.pop_back();
			}
			state.minWindow.push_back(absolutePosition, currentGainFactor);
		}

		const double minimum = state.minWindow.frontValue();
		state.original.pop_front();
		state.minimum.push_back(minimum);
		if(m_loggingEnabled)
		{
			m_loggingData[channel].minimum.push_back(minimum);
		}
	}

	//Apply the Gaussian filter
	while(state.minimum.size() >= filterSize)
	{
		assert(state.minimum.size() == filterSize);
		double smoothed = 0.0;
		for(uint32_t w = 0U; w < filterSize; w++)
		{
			smoothed += state.minimum.at(w) * m_gaussianFilter->weight(w);
		}
		state.minimum.pop_front();
		state.smoothed.push_back(smoothed);
		if(m_loggingEnabled)
		{
			m_loggingData[channel].smoothed.push_back(smoothed);
		}
	}
}

void MDynamicAudioNormalizer_PrivateData::perfromDCCorrection(DYNAUDNORM_NS::FrameData *const frame, const bool &isFirstFrame)
{
	const double diff = 1.0 / double(m_frameLen);

	for(uint32_t c = 0; c < m_channels; c++)
	{
		double *const dataPtr = frame->data(c);
		double currentAverageValue = 0.0;

		for(uint32_t i = 0; i < m_frameLen; i++)
		{
			currentAverageValue += (dataPtr[i] * diff);
		}

		const double prevValue = isFirstFrame ? currentAverageValue : m_dcCorrectionValue[c];
		m_dcCorrectionValue[c] = isFirstFrame ? currentAverageValue : UPDATE_VALUE(currentAverageValue, m_dcCorrectionValue[c], 0.1);

		for(uint32_t i = 0; i < m_frameLen; i++)
		{
			dataPtr[i] -= FADE(prevValue, m_dcCorrectionValue[c], i, m_fadeFactors);
		}
	}
}

void MDynamicAudioNormalizer_PrivateData::perfromCompression(DYNAUDNORM_NS::FrameData *const frame, const bool &isFirstFrame)
{
	if(m_channelsCoupled)
	{
		const double standardDeviation = computeFrameStdDev(frame);
		const double currentThreshold  = std::min(1.0, m_compressFactor * standardDeviation);

		const double prevValue = isFirstFrame ? currentThreshold : m_compressThreshold[0];
		m_compressThreshold[0] = isFirstFrame ? currentThreshold : UPDATE_VALUE(currentThreshold, m_compressThreshold[0], (1.0/3.0));

		const double prevActualThresh = setupCompressThresh(prevValue);
		const double currActualThresh = setupCompressThresh(m_compressThreshold[0]);

		for(uint32_t c = 0; c < m_channels; c++)
		{
			double *const dataPtr = frame->data(c);
			for(uint32_t i = 0; i < m_frameLen; i++)
			{
				const double localThresh = FADE(prevActualThresh, currActualThresh, i, m_fadeFactors);
				dataPtr[i] = copysign(BOUND(localThresh, fabs(dataPtr[i])), dataPtr[i]);
			}
		}
	}
	else
	{
		for(uint32_t c = 0; c < m_channels; c++)
		{
			const double standardDeviation = computeFrameStdDev(frame, c);
			const double currentThreshold  = setupCompressThresh(std::min(1.0,m_compressFactor * standardDeviation));

			const double prevValue = isFirstFrame ? currentThreshold : m_compressThreshold[c];
			m_compressThreshold[c] = isFirstFrame ? currentThreshold : UPDATE_VALUE(currentThreshold, m_compressThreshold[c], (1.0/3.0));

			const double prevActualThresh = setupCompressThresh(prevValue);
			const double currActualThresh = setupCompressThresh(m_compressThreshold[c]);

			double *const dataPtr = frame->data(c);
			for(uint32_t i = 0; i < m_frameLen; i++)
			{
				const double localThresh = FADE(prevActualThresh, currActualThresh, i, m_fadeFactors);
				dataPtr[i] = copysign(BOUND(localThresh, fabs(dataPtr[i])), dataPtr[i]);
			}
		}
	}
}

void MDynamicAudioNormalizer_PrivateData::writeLogFile(void)
{
	if(!m_loggingEnabled)
	{
		return; /*no log file and no callback -> nothing to record*/
	}

	bool bIsEmpty = false;
	for (uint32_t c = 0; c < m_channels; c++)
	{
		bIsEmpty = bIsEmpty || m_loggingData[c].original.empty() || m_loggingData[c].minimum.empty() || m_loggingData[c].smoothed.empty();
	}

	while (!bIsEmpty)
	{
		for (uint32_t c = 0; c < m_channels; c++)
		{
			if (m_logFile && (ferror(m_logFile) == 0))
			{
				if (c > 0)
				{
					fprintf(m_logFile, "\t\t");
				}
				fprintf(m_logFile, "%.5f\t%.5f\t%.5f", m_loggingData[c].original.front(), m_loggingData[c].minimum.front(), m_loggingData[c].smoothed.front());
			}
			m_loggingData[c].original.pop_front();
			m_loggingData[c].minimum.pop_front();
			m_loggingData[c].smoothed.pop_front();
			bIsEmpty = bIsEmpty || m_loggingData[c].original.empty() || m_loggingData[c].minimum.empty() || m_loggingData[c].smoothed.empty();
		}
		if (m_logFile && (ferror(m_logFile) == 0))
		{
			fprintf(m_logFile, "\n");
		}
	}

	if(m_logFile && (ferror(m_logFile) != 0))
	{
		if(ferror(m_logFile) != 0)
		{
			LOG1_WRN("Error while writing to log file. No further logging output will be created.");
		}
	}
}

void MDynamicAudioNormalizer_PrivateData::printParameters(void)
{
	LOG1_DBG("------- DynamicAudioNormalizer -------");
	LOG2_DBG("Lib Version          : %u.%02u-%u", DYNAUDNORM_NS::VERSION_MAJOR, DYNAUDNORM_NS::VERSION_MINOR , DYNAUDNORM_NS::VERSION_PATCH);
	LOG2_DBG("m_channels           : %u",         m_channels);
	LOG2_DBG("m_sampleRate         : %u",         m_sampleRate);
	LOG2_DBG("m_frameLen           : %u",         m_frameLen);
	LOG2_DBG("m_filterSize         : %u",         m_filterSize);
	LOG2_DBG("m_peakValue          : %.4f",       m_peakValue);
	LOG2_DBG("m_maxAmplification   : %.4f",       m_maxAmplification);
	LOG2_DBG("m_targetRms          : %.4f",       m_targetRms);
	LOG2_DBG("m_compressFactor     : %.4f",       m_compressFactor);
	LOG2_DBG("m_channelsCoupled    : %s",         BOOLIFY(m_channelsCoupled));
	LOG2_DBG("m_enableDCCorrection : %s",         BOOLIFY(m_enableDCCorrection));
	LOG2_DBG("m_altBoundaryMode    : %s",         BOOLIFY(m_altBoundaryMode));
	LOG1_DBG("------- DynamicAudioNormalizer -------");
}

///////////////////////////////////////////////////////////////////////////////
// Static Utility Functions
///////////////////////////////////////////////////////////////////////////////

void MDynamicAudioNormalizer_PrivateData::precalculateFadeFactors(double *const fadeFactors[2], const uint32_t frameLen)
{
	assert((frameLen > 0) && ((frameLen % 2) == 0));
	const double dStepSize = 1.0 / double(frameLen);

	for(uint32_t pos = 0; pos < frameLen; pos++)
	{
		fadeFactors[0][pos] = 1.0 - (dStepSize * double(pos + 1U));
		fadeFactors[1][pos] = 1.0 - fadeFactors[0][pos];
	}

	//for(uint32_t pos = 0; pos < frameLen; pos++)
	//{
	//	LOG2_DBG("%.8f %.8f %.8f", fadeFactors[0][pos], fadeFactors[1][pos], fadeFactors[0][pos] + fadeFactors[1][pos]);
	//}
}

double MDynamicAudioNormalizer_PrivateData::setupCompressThresh(const double &dThreshold)
{
	if((dThreshold > DBL_EPSILON) && (dThreshold < (1.0 - DBL_EPSILON)))
	{
		double dCurrentThreshold = dThreshold;
		double dStepSize = 1.0;
		while(dStepSize > DBL_EPSILON)
		{
			while((dCurrentThreshold + dStepSize > dCurrentThreshold) && (BOUND(dCurrentThreshold + dStepSize, 1.0) <= dThreshold))
			{
				dCurrentThreshold += dStepSize;
			}
			dStepSize /= 2.0;
		}
		return dCurrentThreshold;
	}
	else
	{
		return dThreshold;
	}
}
