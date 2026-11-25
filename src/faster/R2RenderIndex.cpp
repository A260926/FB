/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */

/*
    Rubber Band Library
    An audio time-stretching and pitch-shifting library.
    Copyright 2007-2024 Particular Programs Ltd.

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.  See the file
    COPYING included with this distribution for more information.

    Alternatively, if you have a valid commercial licence for the
    Rubber Band Library obtained by agreement with the copyright
    holders, you may redistribute and/or modify it under the terms
    described in that licence.
*/

#include "../../rubberband/R2RenderIndex.h"

#include "../common/Allocators.h"
#include "../common/FFT.h"
#include "../common/Log.h"
#include "../common/RingBuffer.h"
#include "../common/StretchCalculator.h"
#include "../common/VectorOps.h"
#include "../common/Window.h"
#include "../common/mathmisc.h"
#include "CompoundAudioCurve.h"
#include "SilentAudioCurve.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace RubberBand
{

using std::vector;

namespace
{

// Fixed configuration, see R2RenderIndex.h:
// 48 kHz, R2, standard window, smoothing off, pitch scale 1.
const size_t SAMPLE_RATE   = 48000;
const size_t BASE_FFT_SIZE = 2048; // R2Stretcher::m_defaultFftSize at 48k
const size_t FFT_SIZE      = 2048; // window stays base size for ratios in
const size_t WINDOW_SIZE   = 2048; // [0.5, 2] with the standard window
const size_t HALF_WINDOW   = WINDOW_SIZE / 2;
const size_t BIN_COUNT     = FFT_SIZE / 2 + 1;
const size_t START_SKIP    = WINDOW_SIZE / 2; // offline start trim
const size_t WARMUP_MARGIN = 2 * WINDOW_SIZE; // overlap trust distance

// Replica of the non-realtime branch of
// R2Stretcher::calculateSizes() for pitch scale 1 and ratios in
// [0.5, 2].  The FFT and window sizes remain the base size; only the
// input (analysis) hop increment varies with the ratio and duration.
size_t
calculateIncrement(double timeRatio, size_t inputFrames)
{
    const size_t windowSize = BASE_FFT_SIZE;
    size_t inputIncrement;
    double r = timeRatio;

    if (r < 1.0) {
        inputIncrement = windowSize / 4;
        while (inputIncrement >= 512) inputIncrement /= 2;
        size_t outputIncrement = size_t(floor(inputIncrement * r));
        if (outputIncrement < 1) {
            outputIncrement = 1;
            inputIncrement = size_t(roundUp(lrint(ceil(outputIncrement / r))));
        }
    } else {
        size_t outputIncrement = windowSize / 6;
        inputIncrement = size_t(outputIncrement / r);
        while (inputIncrement < 1) {
            outputIncrement *= 2;
            inputIncrement = size_t(outputIncrement / r);
        }
    }

    while (inputIncrement * 4 > inputFrames && inputIncrement > 1) {
        inputIncrement /= 2;
    }
    return inputIncrement;
}

const uint32_t FORMAT_MAGIC   = 0x52325249; // "R2RI"
const uint32_t FORMAT_VERSION = 1;

Log
silentLog()
{
    auto noop0 = [](const char *) { };
    auto noop1 = [](const char *, double) { };
    auto noop2 = [](const char *, double, double) { };
    return Log(noop0, noop1, noop2);
}

// Phase-domain state of one channel at a chunk boundary, i.e. the
// state entering that chunk.  Nothing here derives from synthesis:
// it is fully determined by forward analysis and the phase
// modification math, so it can be captured without any inverse FFT.
struct BoundaryState
{
    vector<double> prevPhase;
    vector<double> prevError;
    vector<double> unwrapped;
    double unityResetLow;
    bool unchanged;

    BoundaryState() :
        prevPhase(BIN_COUNT, 0.0),
        prevError(BIN_COUNT, 0.0),
        unwrapped(BIN_COUNT, 0.0),
        unityResetLow(16000.0),
        unchanged(true)
    { }
};

struct ChannelBoundaries
{
    vector<BoundaryState> states;
};

struct IndexData
{
    size_t sampleRate = 0;
    size_t channels = 0;
    size_t inputFrames = 0;
    size_t outputFrames = 0;
    size_t stride = 0;
    size_t increment = 0;
    double timeRatio = 1.0;

    // Complete hop schedule: one signed phase increment per chunk.
    // A negative value encodes a phase reset on that chunk.
    vector<int32_t> increments;

    // Boundary chunk indices and the engine outCount (R2's
    // pre-start-trim output counter) reached when entering them.
    vector<uint64_t> boundaryChunk;
    vector<uint64_t> boundaryOutCount;
    vector<ChannelBoundaries> channelState;
};

typedef R2RenderIndex::ReadAt ReadAt;

// Fetch the (possibly partly out-of-range) window starting at source
// position `origin` (may be negative) of length `want`, zero-padding
// the parts outside [0, inputFrames).  One reader call per channel.
bool
fetchWindow(ReadAt reader, void *context,
            size_t channels, size_t inputFrames,
            long long origin, size_t want,
            float *const *planes, size_t &framesRead)
{
    long long lo = std::max(origin, 0LL);
    long long hi = std::min(origin + (long long)want,
                            (long long)inputFrames);
    size_t count = (hi > lo) ? size_t(hi - lo) : 0;
    for (size_t c = 0; c < channels; ++c) {
        v_zero(planes[c], want);
    }
    if (count > 0) {
        // The reader fills every channel, so hand it one destination
        // plane per channel rather than a single-element array.
        vector<float *> dst(channels);
        for (size_t c = 0; c < channels; ++c) {
            dst[c] = planes[c] + (lo - origin);
        }
        if (!reader(context, (size_t)lo, count, dst.data())) return false;
        framesRead += count * channels;
    }
    return true;
}

// Exact replica of R2Stretcher::modifyChunk for the fixed
// configuration (OptionPhaseIndependent, no formant shift, pitch
// scale 1, crisp transients, standard window).
void
modifyPhase(double *phase,
            const double *prevPhaseIn,
            double *prevPhaseOut,
            const double *prevErrorIn,
            double *prevErrorOut,
            const double *unwrappedIn,
            double *unwrappedOut,
            size_t sampleRate,
            size_t increment,
            size_t fftSize,
            double timeRatio,
            size_t outputIncrement,
            bool phaseReset,
            double &unityResetLow,
            bool &unchanged)
{
    const double rate = double(sampleRate);
    const int count = int(fftSize / 2);

    bool wasUnchanged = unchanged && (outputIncrement == increment);
    bool fullReset = phaseReset;
    const bool laminar = false; // OptionPhaseIndependent
    bool bandlimited = false;
    int bandlow = 0;
    int bandhigh = 0;

    double r = timeRatio;

    bool unity = (fabs(r - 1.0) < 1.e-6);
    if (unity) {
        if (!phaseReset) {
            phaseReset = true;
            bandlimited = true;
            bandlow = lrint((unityResetLow * fftSize) / rate);
            bandhigh = count;
        }
        unityResetLow *= 0.9;
    } else {
        unityResetLow = 16000.0;
    }

    const float freq0 = 600.f;
    const float freq1 = 1200.f;
    const float freq2 = 12000.f;

    int limit0 = lrint((freq0 * fftSize) / rate);
    int limit1 = lrint((freq1 * fftSize) / rate);
    int limit2 = lrint((freq2 * fftSize) / rate);

    if (limit1 < limit0) limit1 = limit0;
    if (limit2 < limit1) limit2 = limit1;

    const double maxdist = 8.0;
    (void)maxdist;
    (void)laminar;
    (void)prevErrorIn; // only consumed by the laminar inheritance path

    for (int i = count; i >= 0; --i) {

        bool resetThis = phaseReset;

        if (bandlimited) {
            if (resetThis) {
                if (i > bandlow && i < bandhigh) {
                    resetThis = false;
                    fullReset = false;
                }
            }
        }

        double p = phase[i];
        double perr = 0.0;
        double outphase = p;

        if (!resetThis) {

            double omega = (2 * M_PI * increment * i) / fftSize;

            double pp = prevPhaseIn[i];
            double ep = pp + omega;
            perr = princarg(p - ep);

            double advance = outputIncrement * ((omega + perr) / increment);

            // PhaseIndependent: inheritance never happens, so each
            // bin is advanced from its own unwrapped phase.
            outphase = unwrappedIn[i] + advance;
        }

        prevErrorOut[i] = perr;
        prevPhaseOut[i] = p;
        phase[i] = outphase;
        unwrappedOut[i] = outphase;
    }

    if (fullReset) wasUnchanged = true;
    unchanged = wasUnchanged;
}

// Exact replica of the synthesis half of
// R2Stretcher::synthesiseChunk for the fixed configuration (window
// size == FFT size, no time-domain smoothing/interpolator).
void
synthesiseWindow(FFT &fft,
                 double *mag,
                 double *phase,
                 double *dblbuf,
                 float *fltbuf,
                 float *accumulator,
                 float *windowAccumulator,
                 size_t &accumulatorFill,
                 const Window<float> &sWindow,
                 float analysisWindowArea,
                 size_t fftSize,
                 size_t windowSize,
                 bool unchanged)
{
    const int hs = int(fftSize / 2);

    if (!unchanged) {

        float factor = 1.f / fftSize;
        v_scale(mag, factor, hs + 1);

        fft.inversePolar(mag, phase, dblbuf);

        v_convert(fltbuf, dblbuf + hs, hs);
        v_convert(fltbuf + hs, dblbuf, hs);
    }

    sWindow.cut(fltbuf);
    v_add(accumulator, fltbuf, int(windowSize));
    accumulatorFill = std::max(accumulatorFill, windowSize);

    sWindow.add(windowAccumulator, analysisWindowArea * 1.5f);
}

// Replica of R2Stretcher::writeChunk + writeOutput (pitch scale 1,
// no resampler, offline start trim, theoretical end trim).  Copies
// only frames whose absolute output index lies in [first, first+count)
// into dst.
void
emitChunk(float *accumulator,
          float *windowAccumulator,
          size_t &accumulatorFill,
          size_t shiftIncrement,
          size_t &engineOutCount,
          size_t theoreticalOut,
          size_t first,
          size_t count,
          float *dst)
{
    v_divide(accumulator, windowAccumulator, int(shiftIncrement));

    size_t qty = shiftIncrement;

    if (engineOutCount > START_SKIP) {

        if (theoreticalOut > 0 &&
            engineOutCount - START_SKIP <= theoreticalOut &&
            engineOutCount - START_SKIP + qty > theoreticalOut) {
            qty = theoreticalOut - (engineOutCount - START_SKIP);
        }

        size_t absStart = engineOutCount - START_SKIP;
        for (size_t i = 0; i < qty; ++i) {
            size_t absIndex = absStart + i;
            if (absIndex >= first && absIndex < first + count) {
                dst[absIndex - first] = accumulator[i];
            }
        }

        engineOutCount += qty;

    } else if (engineOutCount + qty <= START_SKIP) {

        engineOutCount += qty;

    } else {

        size_t off = START_SKIP - engineOutCount;
        for (size_t i = off; i < qty; ++i) {
            size_t absIndex = i - off;
            if (absIndex >= first && absIndex < first + count) {
                dst[absIndex - first] = accumulator[i];
            }
        }
        engineOutCount += qty;
    }

    size_t sz = accumulatorFill;
    size_t si = shiftIncrement;

    v_move(accumulator, accumulator + si, sz - si);
    v_zero(accumulator + sz - si, si);

    v_move(windowAccumulator, windowAccumulator + si, sz - si);
    v_zero(windowAccumulator + sz - si, si);

    if (accumulatorFill > si) {
        accumulatorFill -= si;
    } else {
        accumulatorFill = 0;
    }
}

class R2RenderIndexImpl : public R2RenderIndex
{
public:
    explicit R2RenderIndexImpl(std::shared_ptr<const IndexData> d) :
        m_data(d)
    { }

    size_t getInputFrames() const override {
        return m_data->inputFrames;
    }

    size_t getOutputFrames() const override {
        return m_data->outputFrames;
    }

    void save(std::ostream &os) const override;
    bool render(size_t first, size_t count,
                ReadAt reader, void *context,
                float *const *output) const override;

private:
    std::shared_ptr<const IndexData> m_data;
};

// -------------------------------------------------------------------
// Analysis (build)
// -------------------------------------------------------------------

bool
validateKeyFrames(size_t inputFrames, double timeRatio,
                  const std::map<size_t, size_t> &keyFrames)
{
    if (keyFrames.empty()) return false;
    if (keyFrames.begin()->first != 0 || keyFrames.begin()->second != 0) {
        return false;
    }
    if (keyFrames.rbegin()->first != inputFrames) return false;
    size_t expectedOut = (size_t)llround((double)inputFrames * timeRatio);
    if (keyFrames.rbegin()->second != expectedOut) return false;

    size_t prevIn = 0, prevOut = 0;
    bool first = true;
    for (const auto &kv : keyFrames) {
        if (!first) {
            if (kv.first <= prevIn || kv.second <= prevOut) return false;
            double slope = double(kv.second - prevOut) /
                           double(kv.first - prevIn);
            if (slope < 0.5 - 1e-12 || slope > 2.0 + 1e-12) return false;
        }
        prevIn = kv.first;
        prevOut = kv.second;
        first = false;
    }
    return true;
}

// Source positioning of study/analysis chunks, matching
// R2Stretcher::study() with its half-window prefill.  Each entry is
// the source-space origin of the window (may be negative); the final
// entry may be shorter than the window.
struct ChunkWindow
{
    long long origin;
    size_t want;
};

vector<ChunkWindow>
planChunks(size_t inputFrames, size_t increment)
{
    vector<ChunkWindow> plan;
    const size_t buffered = HALF_WINDOW + inputFrames;
    size_t readPointer = 0; // virtual ring-buffer read position
    while (true) {
        size_t ready = buffered - readPointer;
        bool full = (ready >= WINDOW_SIZE);
        bool finalPartial = (ready >= HALF_WINDOW);
        if (!full && !finalPartial) break;
        size_t want = std::min(ready, WINDOW_SIZE);
        plan.push_back({ (long long)readPointer - (long long)HALF_WINDOW,
                         want });
        readPointer += std::min(increment, ready);
    }
    return plan;
}

bool
runBuild(size_t sampleRate,
         size_t channels,
         size_t inputFrames,
         double timeRatio,
         size_t stride,
         const std::map<size_t, size_t> &keyFrames,
         ReadAt reader,
         void *context,
         IndexData &data)
{
    const size_t increment = calculateIncrement(timeRatio, inputFrames);
    const vector<ChunkWindow> plan = planChunks(inputFrames, increment);
    const size_t chunkCount = plan.size();
    if (chunkCount == 0) return false;

    vector<float *> planes(channels);
    for (size_t c = 0; c < channels; ++c) {
        planes[c] = allocate_and_zero<float>(WINDOW_SIZE);
    }
    const size_t ringCapacity = WINDOW_SIZE * 2;
    vector<vector<float>> fetch(channels);
    for (size_t c = 0; c < channels; ++c) fetch[c].resize(ringCapacity);
    vector<float> block(ringCapacity, 0.f);
    vector<float> studyAcc(WINDOW_SIZE, 0.f);
    vector<float> studyMag(BIN_COUNT, 0.f);
    vector<double> dblbuf(FFT_SIZE, 0.0);
    vector<double> mag(BIN_COUNT, 0.0);
    vector<double> phase(BIN_COUNT, 0.0);

    FFT fft((int)FFT_SIZE);
    fft.initDouble();
    FFT studyFft((int)FFT_SIZE);
    studyFft.initFloat();
    Window<float> aWindow(HannWindow, (int)WINDOW_SIZE);

    CompoundAudioCurve resetCurve(
        CompoundAudioCurve::Parameters((int)sampleRate, (int)FFT_SIZE));
    resetCurve.setType(CompoundAudioCurve::CompoundDetector);
    SilentAudioCurve silentCurve(
        SilentAudioCurve::Parameters((int)sampleRate, (int)FFT_SIZE));

    vector<float> phaseResetDf;
    vector<bool> silence;
    phaseResetDf.reserve(chunkCount);
    silence.reserve(chunkCount);

    // ---- Study pass on the channel mixdown (forward FFTs only) ----
    // Reproduces R2Stretcher::study() exactly: one 4096-capacity ring
    // for the mixdown, prefilled with half a window of silence, all
    // source delivered with final=true.  A short peek only overwrites
    // the available prefix of the persistent analysis buffer, so its
    // tail keeps the previous chunk's window-cut content.
    RingBuffer<float> studyRing((int)WINDOW_SIZE * 2);
    studyRing.zero((int)HALF_WINDOW);
    size_t sourcePos = 0;
    while (sourcePos < inputFrames) {
        int writable = studyRing.getWriteSpace();
        size_t toWrite = std::min(size_t(writable),
                                  inputFrames - sourcePos);
        if (toWrite == 0) break;

        vector<float *> dst(channels);
        for (size_t c = 0; c < channels; ++c) dst[c] = fetch[c].data();
        if (!reader(context, sourcePos, toWrite, dst.data())) {
            for (size_t c = 0; c < channels; ++c) deallocate(planes[c]);
            return false;
        }
        for (size_t i = 0; i < toWrite; ++i) {
            float sum = 0.f;
            for (size_t c = 0; c < channels; ++c) sum += fetch[c][i];
            block[i] = sum / float(channels);
        }
        studyRing.write(block.data(), (int)toWrite);
        sourcePos += toWrite;

        // The whole source is available, exactly like a single
        // R2Stretcher::study(..., final=true) call, so final stays
        // true for every refill and half-window partials are drained
        // as soon as they appear.
        const bool final = true;
        while (studyRing.getReadSpace() >= (int)WINDOW_SIZE ||
               (final && studyRing.getReadSpace() >= (int)HALF_WINDOW)) {
            int ready = studyRing.getReadSpace();
            int n = std::min(ready, (int)WINDOW_SIZE);
            studyRing.peek(studyAcc.data(), n);
            aWindow.cut(studyAcc.data());
            studyFft.forwardMagnitude(studyAcc.data(), studyMag.data());

            phaseResetDf.push_back(
                resetCurve.processFloat(studyMag.data(), int(increment)));
            silence.push_back(
                silentCurve.processFloat(studyMag.data(), int(increment))
                > 0.f);

            studyRing.skip((int)std::min(increment, size_t(ready)));
        }
    }
    if (phaseResetDf.size() != chunkCount) return false;

    // ---- Hop schedule, identical to R2Stretcher::calculateStretch ----
    StretchCalculator calculator(sampleRate, increment,
                                 true /* hard peaks, crisp transients */,
                                 silentLog());
    calculator.setKeyFrameMap(keyFrames);
    vector<int> increments =
        calculator.calculate(timeRatio, inputFrames, phaseResetDf);

    int silentHistory = 0;
    for (size_t i = 0; i < increments.size(); ++i) {
        if (i >= silence.size()) break;
        if (silence[i]) ++silentHistory;
        else silentHistory = 0;
        if (silentHistory >= int(WINDOW_SIZE / increment) &&
            increments[i] >= 0) {
            increments[i] = -increments[i];
        }
    }
    if (increments.size() != chunkCount) return false;

    // Engine shift for chunk k is the (absolute) phase increment of
    // chunk min(k+1, n-1).  Overlong shifts are split into
    // quarter-window pieces in R2, but their total is the same, so
    // the outCount prefix only needs the summed shifts.
    vector<uint64_t> outCountBefore(chunkCount + 1, 0);
    {
        uint64_t total = 0;
        for (size_t k = 0; k < chunkCount; ++k) {
            outCountBefore[k] = total;
            size_t shiftIndex = std::min(k + 1, chunkCount - 1);
            total += (uint64_t)std::abs(increments[shiftIndex]);
        }
        outCountBefore[chunkCount] = total;
    }

    vector<size_t> boundaryChunks;
    boundaryChunks.push_back(0);
    for (size_t k = 1; k < chunkCount; ++k) {
        if (outCountBefore[k] -
            outCountBefore[boundaryChunks.back()] >= stride) {
            boundaryChunks.push_back(k);
        }
    }
    if (boundaryChunks.back() != chunkCount) {
        boundaryChunks.push_back(chunkCount);
    }

    data.sampleRate  = sampleRate;
    data.channels    = channels;
    data.inputFrames = inputFrames;
    data.stride      = stride;
    data.increment   = increment;
    data.timeRatio   = timeRatio;
    data.outputFrames = (size_t)llround(double(inputFrames) * timeRatio);
    data.increments.resize(chunkCount);
    for (size_t k = 0; k < chunkCount; ++k) {
        data.increments[k] = increments[k];
    }
    data.boundaryChunk.reserve(boundaryChunks.size());
    data.boundaryOutCount.reserve(boundaryChunks.size());
    for (size_t k : boundaryChunks) {
        data.boundaryChunk.push_back((uint64_t)k);
        data.boundaryOutCount.push_back(outCountBefore[k]);
    }
    data.channelState.resize(channels);

    // ---- Per-channel phase state capture (forward FFTs only) ----
    for (size_t c = 0; c < channels; ++c) {

        vector<double> prevPhase(BIN_COUNT, 0.0);
        vector<double> prevError(BIN_COUNT, 0.0);
        vector<double> unwrapped(BIN_COUNT, 0.0);
        vector<double> nextPrevPhase(BIN_COUNT, 0.0);
        vector<double> nextPrevError(BIN_COUNT, 0.0);
        vector<double> nextUnwrapped(BIN_COUNT, 0.0);

        double unityResetLow = 16000.0;
        bool unchanged = true;

        auto &states = data.channelState[c].states;
        states.reserve(boundaryChunks.size());

        size_t nextBoundary = 0;
        size_t fetchedChunk = size_t(-1);

        for (size_t k = 0; k <= chunkCount; ++k) {

            while (nextBoundary < boundaryChunks.size() &&
                   boundaryChunks[nextBoundary] < k) {
                ++nextBoundary;
            }
            if (nextBoundary < boundaryChunks.size() &&
                boundaryChunks[nextBoundary] == k) {
                BoundaryState s;
                s.prevPhase = prevPhase;
                s.prevError = prevError;
                s.unwrapped = unwrapped;
                s.unityResetLow = unityResetLow;
                s.unchanged = unchanged;
                states.push_back(std::move(s));
                ++nextBoundary;
            }
            if (k == chunkCount) break;

            // Fetch every channel once per chunk; channel passes then
            // reuse the same buffered windows.
            if (fetchedChunk != k) {
                size_t framesRead = 0;
                if (!fetchWindow(reader, context, channels, inputFrames,
                                 plan[k].origin, plan[k].want,
                                 planes.data(), framesRead)) {
                    for (size_t cc = 0; cc < channels; ++cc) {
                        deallocate(planes[cc]);
                    }
                    return false;
                }
                fetchedChunk = k;
            }

            // analyseChunk replica (window size == FFT size): the
            // analysis window is applied in place and the FFT input
            // is the fft-shifted buffer, exactly like
            // R2Stretcher::cutShiftAndFold.
            aWindow.cut(planes[c]);
            v_convert(dblbuf.data(), planes[c] + HALF_WINDOW, HALF_WINDOW);
            v_convert(dblbuf.data() + HALF_WINDOW, planes[c], HALF_WINDOW);
            fft.forwardPolar(dblbuf.data(), mag.data(), phase.data());

            int signedIncr = increments[k];
            bool phaseReset = (signedIncr < 0);
            size_t phaseIncrement = (size_t)std::abs(signedIncr);
            if (k == 0) phaseReset = true; // getIncrements() rule

            modifyPhase(phase.data(),
                        prevPhase.data(), nextPrevPhase.data(),
                        prevError.data(), nextPrevError.data(),
                        unwrapped.data(), nextUnwrapped.data(),
                        sampleRate, increment, FFT_SIZE, timeRatio,
                        phaseIncrement, phaseReset,
                        unityResetLow, unchanged);

            prevPhase.swap(nextPrevPhase);
            prevError.swap(nextPrevError);
            unwrapped.swap(nextUnwrapped);
        }

        if (states.size() != boundaryChunks.size()) return false;
    }

    for (size_t c = 0; c < channels; ++c) deallocate(planes[c]);
    return true;
}

// -------------------------------------------------------------------
// Serialisation
// -------------------------------------------------------------------

class Hash64
{
public:
    Hash64() : m_h(0xcbf29ce484222325ULL) { }

    void add(const void *p, size_t n) {
        const unsigned char *b = static_cast<const unsigned char *>(p);
        for (size_t i = 0; i < n; ++i) {
            m_h ^= b[i];
            m_h *= 0x100000001b3ULL;
        }
    }

    uint64_t get() const { return m_h; }

private:
    uint64_t m_h;
};

void
putBytes(std::ostream &os, const void *p, size_t n, Hash64 &hash)
{
    os.write(static_cast<const char *>(p), std::streamsize(n));
    if (!os) throw std::runtime_error("R2RenderIndex: write failed");
    hash.add(p, n);
}

template <typename T>
void
putPod(std::ostream &os, const T &value, Hash64 &hash)
{
    putBytes(os, &value, sizeof(T), hash);
}

bool
getBytes(std::istream &is, void *p, size_t n, Hash64 &hash)
{
    is.read(static_cast<char *>(p), std::streamsize(n));
    if (!is || is.gcount() != std::streamsize(n)) return false;
    hash.add(p, n);
    return true;
}

template <typename T>
bool
getPod(std::istream &is, T &value, Hash64 &hash)
{
    return getBytes(is, &value, sizeof(T), hash);
}

void
R2RenderIndexImpl::save(std::ostream &os) const
{
    const IndexData &d = *m_data;
    Hash64 hash;

    putPod(os, FORMAT_MAGIC, hash);
    putPod(os, FORMAT_VERSION, hash);
    putPod(os, uint64_t(d.sampleRate), hash);
    putPod(os, uint64_t(d.channels), hash);
    putPod(os, uint64_t(d.inputFrames), hash);
    putPod(os, uint64_t(d.outputFrames), hash);
    putPod(os, uint64_t(d.stride), hash);
    putPod(os, uint64_t(d.increment), hash);
    putPod(os, d.timeRatio, hash);
    putPod(os, uint64_t(d.increments.size()), hash);
    putPod(os, uint64_t(d.boundaryChunk.size()), hash);

    if (!d.increments.empty()) {
        putBytes(os, d.increments.data(),
                 d.increments.size() * sizeof(int32_t), hash);
    }
    if (!d.boundaryChunk.empty()) {
        putBytes(os, d.boundaryChunk.data(),
                 d.boundaryChunk.size() * sizeof(uint64_t), hash);
        putBytes(os, d.boundaryOutCount.data(),
                 d.boundaryOutCount.size() * sizeof(uint64_t), hash);
    }

    for (size_t c = 0; c < d.channels; ++c) {
        const auto &states = d.channelState[c].states;
        if (states.size() != d.boundaryChunk.size()) {
            throw std::runtime_error("R2RenderIndex: inconsistent state");
        }
        for (const BoundaryState &s : states) {
            putBytes(os, s.prevPhase.data(),
                     s.prevPhase.size() * sizeof(double), hash);
            putBytes(os, s.prevError.data(),
                     s.prevError.size() * sizeof(double), hash);
            putBytes(os, s.unwrapped.data(),
                     s.unwrapped.size() * sizeof(double), hash);
            putPod(os, s.unityResetLow, hash);
            putPod(os, uint8_t(s.unchanged ? 1 : 0), hash);
        }
    }

    uint64_t trailer = hash.get(); // written after, not hashed itself
    os.write(reinterpret_cast<const char *>(&trailer), sizeof(trailer));
    if (!os) throw std::runtime_error("R2RenderIndex: write failed");
    os.flush();
    if (!os) throw std::runtime_error("R2RenderIndex: flush failed");
}

std::shared_ptr<const R2RenderIndex>
loadIndex(std::istream &is)
{
    auto data = std::make_shared<IndexData>();
    Hash64 hash;

    uint32_t magic = 0, version = 0;
    uint64_t sampleRate = 0, channels = 0, inputFrames = 0;
    uint64_t outputFrames = 0, stride = 0, increment = 0;
    double timeRatio = 0.0;
    uint64_t hopCount = 0, boundaryCount = 0;

    if (!getPod(is, magic, hash) || !getPod(is, version, hash)) {
        return nullptr;
    }
    if (magic != FORMAT_MAGIC || version != FORMAT_VERSION) return nullptr;
    if (!getPod(is, sampleRate, hash) || !getPod(is, channels, hash) ||
        !getPod(is, inputFrames, hash) || !getPod(is, outputFrames, hash) ||
        !getPod(is, stride, hash) || !getPod(is, increment, hash) ||
        !getPod(is, timeRatio, hash) ||
        !getPod(is, hopCount, hash) || !getPod(is, boundaryCount, hash)) {
        return nullptr;
    }

    if (sampleRate != SAMPLE_RATE || channels < 1 || channels > 2 ||
        inputFrames == 0 || outputFrames == 0 ||
        stride < 4096 || stride > 65536 ||
        increment == 0 || increment > WINDOW_SIZE ||
        !std::isfinite(timeRatio) || timeRatio <= 0.0 ||
        hopCount == 0 || boundaryCount == 0) {
        return nullptr;
    }
    if (outputFrames != (size_t)llround(double(inputFrames) * timeRatio)) {
        return nullptr;
    }
    if (increment != calculateIncrement(timeRatio, size_t(inputFrames))) {
        return nullptr;
    }

    data->sampleRate = size_t(sampleRate);
    data->channels = size_t(channels);
    data->inputFrames = size_t(inputFrames);
    data->outputFrames = size_t(outputFrames);
    data->stride = size_t(stride);
    data->increment = size_t(increment);
    data->timeRatio = timeRatio;

    data->increments.resize(hopCount);
    if (!getBytes(is, data->increments.data(),
                  hopCount * sizeof(int32_t), hash)) return nullptr;
    for (int32_t v : data->increments) {
        if (v == 0) return nullptr;
    }

    data->boundaryChunk.resize(boundaryCount);
    data->boundaryOutCount.resize(boundaryCount);
    if (!getBytes(is, data->boundaryChunk.data(),
                  boundaryCount * sizeof(uint64_t), hash) ||
        !getBytes(is, data->boundaryOutCount.data(),
                  boundaryCount * sizeof(uint64_t), hash)) {
        return nullptr;
    }
    if (data->boundaryChunk[0] != 0 ||
        data->boundaryOutCount[0] != 0 ||
        data->boundaryChunk.back() != hopCount) {
        return nullptr;
    }
    for (uint64_t i = 1; i < boundaryCount; ++i) {
        if (data->boundaryChunk[i] <= data->boundaryChunk[i-1] ||
            data->boundaryOutCount[i] < data->boundaryOutCount[i-1]) {
            return nullptr;
        }
    }

    data->channelState.resize(size_t(channels));
    for (uint64_t c = 0; c < channels; ++c) {
        auto &states = data->channelState[size_t(c)].states;
        states.resize(boundaryCount);
        for (uint64_t b = 0; b < boundaryCount; ++b) {
            BoundaryState &s = states[size_t(b)];
            if (!getBytes(is, s.prevPhase.data(),
                          BIN_COUNT * sizeof(double), hash) ||
                !getBytes(is, s.prevError.data(),
                          BIN_COUNT * sizeof(double), hash) ||
                !getBytes(is, s.unwrapped.data(),
                          BIN_COUNT * sizeof(double), hash) ||
                !getPod(is, s.unityResetLow, hash) ||
                !std::isfinite(s.unityResetLow)) {
                return nullptr;
            }
            uint8_t unchanged = 0;
            if (!getPod(is, unchanged, hash)) return nullptr;
            if (unchanged > 1) return nullptr;
            s.unchanged = (unchanged != 0);
        }
    }

    // The trailer is not itself covered by the running hash.
    uint64_t expected = hash.get();
    uint64_t trailer = 0;
    {
        char *p = reinterpret_cast<char *>(&trailer);
        is.read(p, std::streamsize(sizeof(trailer)));
        if (!is || is.gcount() != std::streamsize(sizeof(trailer))) {
            return nullptr;
        }
    }
    if (trailer != expected) return nullptr;
    if (is.peek() == std::char_traits<char>::eof()) {
        // exactly consumed, good
    } else {
        return nullptr; // trailing garbage
    }

    return std::shared_ptr<const R2RenderIndex>(
        new R2RenderIndexImpl(data));
}

// -------------------------------------------------------------------
// Rendering
// -------------------------------------------------------------------

bool
R2RenderIndexImpl::render(size_t first,
                          size_t count,
                          ReadAt reader,
                          void *context,
                          float *const *output) const
{
    if (count == 0) return true;
    if (!reader) return false;
    const IndexData &d = *m_data;
    if (first > d.outputFrames || count > d.outputFrames - first) {
        return false;
    }

    const size_t chunkCount = d.increments.size();
    const size_t theoreticalOut = d.outputFrames;

    // Pick the latest boundary whose synthetic output begins at
    // least WARMUP_MARGIN absolute frames before the requested range.
    // A warmup rebuild converges exactly with sequential R2 state
    // once a full window length of overlap has been produced.
    size_t boundary = 0;
    for (size_t b = 0; b < d.boundaryChunk.size(); ++b) {
        long long absStart =
            (long long)d.boundaryOutCount[b] - (long long)START_SKIP;
        if (absStart + (long long)WARMUP_MARGIN <= (long long)first) {
            boundary = b;
        }
    }

    const size_t startChunk = size_t(d.boundaryChunk[boundary]);

    struct ChannelScratch {
        float *fltbuf;
        double *dblbuf;
        double *mag;
        double *phase;
        double *prevPhase;
        double *prevError;
        double *unwrapped;
        double *nextPrevPhase;
        double *nextPrevError;
        double *nextUnwrapped;
        float *accumulator;
        float *windowAccumulator;
        float *temp;
        size_t accumulatorFill;
        size_t engineOutCount;
        double unityResetLow;
        bool unchanged;
        bool draining;
    };

    vector<ChannelScratch> cs(d.channels);
    bool ok = true;

    for (size_t c = 0; c < d.channels; ++c) {
        ChannelScratch &s = cs[c];
        s.fltbuf = allocate_and_zero<float>(WINDOW_SIZE);
        s.dblbuf = allocate_and_zero<double>(FFT_SIZE);
        s.mag = allocate_and_zero<double>(BIN_COUNT);
        s.phase = allocate_and_zero<double>(BIN_COUNT);
        s.prevPhase = allocate_and_zero<double>(BIN_COUNT);
        s.prevError = allocate_and_zero<double>(BIN_COUNT);
        s.unwrapped = allocate_and_zero<double>(BIN_COUNT);
        s.nextPrevPhase = allocate_and_zero<double>(BIN_COUNT);
        s.nextPrevError = allocate_and_zero<double>(BIN_COUNT);
        s.nextUnwrapped = allocate_and_zero<double>(BIN_COUNT);
        s.accumulator = allocate_and_zero<float>(WINDOW_SIZE);
        s.windowAccumulator = allocate_and_zero<float>(WINDOW_SIZE);
        s.windowAccumulator[0] = 1.f; // matches ChannelData::reset()
        s.temp = allocate_and_zero<float>(count);
        s.accumulatorFill = 0;
        s.engineOutCount = size_t(d.boundaryOutCount[boundary]);
        s.draining = false;

        const BoundaryState &bs = d.channelState[c].states[boundary];
        v_copy(s.prevPhase, bs.prevPhase.data(), BIN_COUNT);
        v_copy(s.prevError, bs.prevError.data(), BIN_COUNT);
        v_copy(s.unwrapped, bs.unwrapped.data(), BIN_COUNT);
        s.unityResetLow = bs.unityResetLow;
        s.unchanged = bs.unchanged;
    }

    FFT fft((int)FFT_SIZE);
    fft.initDouble();
    Window<float> aWindow(HannWindow, (int)WINDOW_SIZE);
    Window<float> sWindow(HannWindow, (int)WINDOW_SIZE);
    const float windowArea = aWindow.getArea();

    size_t chunkIndex = startChunk;

    // Virtual input-buffer read pointer, with the half-window
    // prefill of offline R2.
    const size_t bufferedTotal = HALF_WINDOW + d.inputFrames;
    size_t readPointer = startChunk * d.increment;

    while (ok) {

        bool allDone = true;
        for (size_t c = 0; c < d.channels; ++c) {
            if (!cs[c].draining || cs[c].accumulatorFill > 0) {
                allDone = false;
                break;
            }
        }
        if (allDone) break;

        size_t ready = bufferedTotal > readPointer ?
                       bufferedTotal - readPointer : 0;

        // All channels consume the same input positions and share the
        // schedule, so draining and the read window are global.
        bool inputDraining = false;
        if (ready == 0 || ready < HALF_WINDOW) inputDraining = true;
        for (size_t c = 0; c < d.channels; ++c) {
            if (inputDraining) cs[c].draining = true;
        }

        size_t scheduleIndex = std::min(chunkIndex, chunkCount - 1);
        int signedIncr = d.increments[scheduleIndex];
        bool resetFlag = (signedIncr < 0);
        size_t phaseIncrement = (size_t)std::abs(signedIncr);
        size_t shiftIndex = std::min(chunkIndex + 1, chunkCount - 1);
        size_t shiftIncrement =
            (size_t)std::abs(d.increments[shiftIndex]);

        // One multi-plane reader call for every channel per chunk.
        if (!inputDraining) {
            size_t want = std::min(ready, WINDOW_SIZE);
            long long origin =
                (long long)readPointer - (long long)HALF_WINDOW;
            long long lo = std::max(origin, 0LL);
            long long hi = std::min(origin + (long long)want,
                                    (long long)d.inputFrames);
            size_t rcount = (hi > lo) ? size_t(hi - lo) : 0;

            vector<float *> planes(d.channels);
            for (size_t c = 0; c < d.channels; ++c) {
                // Only the ready prefix is refreshed; the tail keeps
                // the previous chunk's content, exactly like R2's
                // analysis buffer after a short ring-buffer peek.
                v_zero(cs[c].fltbuf, want);
                planes[c] = cs[c].fltbuf + (lo - origin);
            }
            if (rcount > 0 && !reader(context, (size_t)lo, rcount,
                                      planes.data())) {
                ok = false;
                break;
            }
        }

        for (size_t c = 0; c < d.channels; ++c) {
            ChannelScratch &s = cs[c];
            bool phaseReset = resetFlag;
            if (chunkIndex == 0) phaseReset = true;

            if (!s.draining) {

                // analyseChunk replica: window cut then fft shift.
                aWindow.cut(s.fltbuf);
                v_convert(s.dblbuf, s.fltbuf + HALF_WINDOW, HALF_WINDOW);
                v_convert(s.dblbuf + HALF_WINDOW, s.fltbuf, HALF_WINDOW);
                fft.forwardPolar(s.dblbuf, s.mag, s.phase);

                if (shiftIncrement <= WINDOW_SIZE) {

                    modifyPhase(s.phase,
                                s.prevPhase, s.nextPrevPhase,
                                s.prevError, s.nextPrevError,
                                s.unwrapped, s.nextUnwrapped,
                                d.sampleRate, d.increment, FFT_SIZE,
                                d.timeRatio,
                                phaseIncrement, phaseReset,
                                s.unityResetLow, s.unchanged);
                    std::swap(s.prevPhase, s.nextPrevPhase);
                    std::swap(s.prevError, s.nextPrevError);
                    std::swap(s.unwrapped, s.nextUnwrapped);

                    synthesiseWindow(fft, s.mag, s.phase, s.dblbuf,
                                     s.fltbuf, s.accumulator,
                                     s.windowAccumulator,
                                     s.accumulatorFill, sWindow,
                                     windowArea, FFT_SIZE, WINDOW_SIZE,
                                     s.unchanged);
                    emitChunk(s.accumulator, s.windowAccumulator,
                              s.accumulatorFill, shiftIncrement,
                              s.engineOutCount, theoreticalOut,
                              first, count, s.temp);
                } else {
                    // Defensive replica of the overlong-shift split in
                    // processChunks(); unreachable for ratios in
                    // [0.5, 2] but kept identical to R2 semantics.
                    size_t bit = WINDOW_SIZE / 4;
                    vector<float> tmp(s.fltbuf, s.fltbuf + WINDOW_SIZE);
                    bool firstPiece = true;
                    for (size_t i = 0; i < shiftIncrement; i += bit) {
                        v_copy(s.fltbuf, tmp.data(), WINDOW_SIZE);
                        size_t thisIncrement = bit;
                        if (i + thisIncrement > shiftIncrement) {
                            thisIncrement = shiftIncrement - i;
                        }
                        modifyPhase(s.phase,
                                    s.prevPhase, s.nextPrevPhase,
                                    s.prevError, s.nextPrevError,
                                    s.unwrapped, s.nextUnwrapped,
                                    d.sampleRate, d.increment, FFT_SIZE,
                                    d.timeRatio,
                                    phaseIncrement + i,
                                    phaseReset && firstPiece,
                                    s.unityResetLow, s.unchanged);
                        std::swap(s.prevPhase, s.nextPrevPhase);
                        std::swap(s.prevError, s.nextPrevError);
                        std::swap(s.unwrapped, s.nextUnwrapped);
                        synthesiseWindow(fft, s.mag, s.phase, s.dblbuf,
                                         s.fltbuf, s.accumulator,
                                         s.windowAccumulator,
                                         s.accumulatorFill, sWindow,
                                         windowArea, FFT_SIZE,
                                         WINDOW_SIZE, s.unchanged);
                        emitChunk(s.accumulator, s.windowAccumulator,
                                  s.accumulatorFill, thisIncrement,
                                  s.engineOutCount, theoreticalOut,
                                  first, count, s.temp);
                        firstPiece = false;
                    }
                }
            } else {
                // Draining: no new analysis, emit from the existing
                // accumulator using the tail shift increment.
                if (s.accumulatorFill == 0) {
                    continue;
                }
                size_t drainShift = shiftIncrement;
                if (drainShift == 0) drainShift = d.increment;
                if (s.accumulatorFill <= drainShift) {
                    drainShift = s.accumulatorFill;
                }
                emitChunk(s.accumulator, s.windowAccumulator,
                          s.accumulatorFill, drainShift,
                          s.engineOutCount, theoreticalOut,
                          first, count, s.temp);
            }
        }

        if (!ok) break;

        // Stop as soon as every channel has emitted the final frame
        // of the requested absolute range; nothing later can
        // contribute to it.
        bool covered = true;
        for (size_t c = 0; c < d.channels; ++c) {
            size_t producedAbs = cs[c].engineOutCount > START_SKIP ?
                cs[c].engineOutCount - START_SKIP : 0;
            if (producedAbs < first + count) {
                covered = false;
                break;
            }
        }
        if (covered) break;

        if (chunkIndex < chunkCount) {
            size_t skip = std::min(d.increment, ready);
            readPointer += skip;
            ++chunkIndex;
        } else {
            bool anyFill = false;
            for (size_t c = 0; c < d.channels; ++c) {
                if (cs[c].accumulatorFill > 0) anyFill = true;
            }
            if (!anyFill) break;
        }
    }

    if (ok) {
        // Commit only after every reader call has succeeded, so a
        // failed render leaves the destination buffers untouched.
        for (size_t c = 0; c < d.channels; ++c) {
            v_copy(output[c], cs[c].temp, count);
        }
    }

    for (size_t c = 0; c < d.channels; ++c) {
        deallocate(cs[c].fltbuf);
        deallocate(cs[c].dblbuf);
        deallocate(cs[c].mag);
        deallocate(cs[c].phase);
        deallocate(cs[c].prevPhase);
        deallocate(cs[c].prevError);
        deallocate(cs[c].unwrapped);
        deallocate(cs[c].nextPrevPhase);
        deallocate(cs[c].nextPrevError);
        deallocate(cs[c].nextUnwrapped);
        deallocate(cs[c].accumulator);
        deallocate(cs[c].windowAccumulator);
        deallocate(cs[c].temp);
    }

    return ok;
}

} // anonymous namespace

// -------------------------------------------------------------------
// Public entry points
// -------------------------------------------------------------------

std::shared_ptr<const R2RenderIndex>
R2RenderIndex::build(size_t sampleRate,
                     size_t channels,
                     size_t inputFrames,
                     double timeRatio,
                     size_t stride,
                     const std::map<size_t, size_t> &keyFrames,
                     ReadAt reader,
                     void *context)
{
    if (!reader) return nullptr;
    if (sampleRate != SAMPLE_RATE || channels < 1 || channels > 2) {
        return nullptr;
    }
    if (inputFrames == 0 || stride < 4096 || stride > 65536) {
        return nullptr;
    }
    if (!std::isfinite(timeRatio) || timeRatio <= 0.0) return nullptr;
    if (!validateKeyFrames(inputFrames, timeRatio, keyFrames)) {
        return nullptr;
    }

    auto data = std::make_shared<IndexData>();
    if (!runBuild(sampleRate, channels, inputFrames, timeRatio, stride,
                  keyFrames, reader, context, *data)) {
        return nullptr;
    }
    return std::shared_ptr<const R2RenderIndex>(
        new R2RenderIndexImpl(data));
}

std::shared_ptr<const R2RenderIndex>
R2RenderIndex::load(std::istream &is)
{
    return loadIndex(is);
}

} // namespace RubberBand
