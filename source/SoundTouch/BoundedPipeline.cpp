////////////////////////////////////////////////////////////////////////////////
///
/// BoundedPipeline implementation. The scheduling logic in this file drives
/// the unmodified production stages (TDStretch WSOLA, RateTransposer cubic
/// interpolation + 64-tap anti-alias FIR and their FIFOSampleBuffer queues)
/// under fixed-capacity back pressure. All storage is acquired by the
/// constructor; process()/resetSegment() only move frames and bookkeeping.
///
////////////////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>

#include "BoundedPipeline.h"
#include "RateTransposer.h"
#include "TDStretch.h"

using namespace soundtouch;

namespace
{
    /// Largest single batch moved between a stage FIFO and a scratch area.
    constexpr uint kFeedChunkFrames = 64u;

    /// Every stage FIFO is grown to at least this many frames during
    /// prepareBounded(). It covers the biggest WSOLA output batch
    /// (seekWindowLength <= 6000 frames at 48 kHz) with margin.
    constexpr uint kScratchFrames = 8192u;

    std::atomic<uint64_t> g_allocations{0};
    std::atomic<uint64_t> g_deallocations{0};
}

uint64_t BoundedPipeline::totalAllocations() noexcept
{
    return g_allocations.load(std::memory_order_relaxed);
}

uint64_t BoundedPipeline::totalDeallocations() noexcept
{
    return g_deallocations.load(std::memory_order_relaxed);
}

void *BoundedPipeline::allocateStorage(size_t bytes)
{
    void *ptr = ::operator new(bytes);
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    return ptr;
}

void BoundedPipeline::releaseStorage(void *ptr) noexcept
{
    if (ptr == nullptr) return;
    ::operator delete(ptr);
    g_deallocations.fetch_add(1, std::memory_order_relaxed);
}

////////////////////////////////////////////////////////////////////////////////
// BoundedFrameFifo
////////////////////////////////////////////////////////////////////////////////

BoundedFrameFifo::BoundedFrameFifo()
    : storage(nullptr), capacityFrames(0), channels(0), head(0), fill(0)
{
}

BoundedFrameFifo::~BoundedFrameFifo()
{
    release();
}

void BoundedFrameFifo::allocate(uint frameCapacity, uint numChannels)
{
    release();
    channels = numChannels;
    capacityFrames = frameCapacity;
    head = 0;
    fill = 0;
    storage = static_cast<SAMPLETYPE *>(
        BoundedPipeline::allocateStorage(sizeof(SAMPLETYPE) * frameCapacity * numChannels));
}

void BoundedFrameFifo::release() noexcept
{
    if (storage)
    {
        BoundedPipeline::releaseStorage(storage);
        storage = nullptr;
    }
    capacityFrames = 0;
    head = 0;
    fill = 0;
}

void BoundedFrameFifo::clear() noexcept
{
    head = 0;
    fill = 0;
}

uint BoundedFrameFifo::push(const SAMPLETYPE *src, uint numFrames) noexcept
{
    uint accepted = std::min(numFrames, capacityFrames - fill);
    uint tail = (head + fill) % capacityFrames;
    uint first = std::min(accepted, capacityFrames - tail);
    if (first)
    {
        std::memcpy(storage + tail * channels, src,
                    sizeof(SAMPLETYPE) * first * channels);
    }
    if (accepted > first)
    {
        std::memcpy(storage, src + first * channels,
                    sizeof(SAMPLETYPE) * (accepted - first) * channels);
    }
    fill += accepted;
    return accepted;
}

uint BoundedFrameFifo::pop(SAMPLETYPE *dst, uint numFrames) noexcept
{
    uint popped = std::min(numFrames, fill);
    uint first = std::min(popped, capacityFrames - head);
    if (first)
    {
        std::memcpy(dst, storage + head * channels,
                    sizeof(SAMPLETYPE) * first * channels);
    }
    if (popped > first)
    {
        std::memcpy(dst + first * channels, storage,
                    sizeof(SAMPLETYPE) * (popped - first) * channels);
    }
    head = (head + popped) % capacityFrames;
    fill -= popped;
    return popped;
}

////////////////////////////////////////////////////////////////////////////////
// BoundedPipeline
////////////////////////////////////////////////////////////////////////////////

BoundedPipeline::BoundedPipeline(SoundTouch &ownerRef, uint numChannels,
                                 float effectiveRate, float effectiveTempo,
                                 bool useAaFilter, uint aaFilterTaps)
    : rateStage(ownerRef.pRateTransposer),
      tdStage(ownerRef.pTDStretch),
      channels(numChannels),
      rate(effectiveRate),
      tempo(effectiveTempo),
      phase(Phase::Running),
      finalRequested(false),
      scratchA(nullptr),
      scratchB(nullptr),
      scratchFrames(kScratchFrames),
      acceptedInput(0),
      deliveredOutput(0),
      targetFrames(0)
{
    std::memset(highWater, 0, sizeof(highWater));

    if (useAaFilter && aaFilterTaps != 64u)
    {
        throw std::invalid_argument("Bounded mode requires the default 64-tap AA filter");
    }

    try
    {
        // Fixed-capacity caller-facing FIFOs.
        inputFifo.allocate(FIFO_FRAME_LIMIT, channels);
        outputFifo.allocate(FIFO_FRAME_LIMIT, channels);

        // Staging areas. Both are large enough for the biggest WSOLA batch.
        scratchA = static_cast<SAMPLETYPE *>(
            allocateStorage(sizeof(SAMPLETYPE) * kScratchFrames * channels));
        scratchB = static_cast<SAMPLETYPE *>(
            allocateStorage(sizeof(SAMPLETYPE) * kScratchFrames * channels));
        std::memset(scratchB, 0, sizeof(SAMPLETYPE) * kScratchFrames * channels);

        prewarmStageBuffers();
    }
    catch (...)
    {
        // The member FIFOs free themselves; only the raw scratch pointers
        // need explicit cleanup if an allocation fails mid-constructor.
        releaseStorage(scratchA);
        releaseStorage(scratchB);
        throw;
    }

    // Start every segment from the same state as a freshly constructed
    // stage: empty queues, zeroed WSOLA mid buffer/phase and cubic position.
    rateStage->resetProcessingState();
    tdStage->resetProcessingState();
}

BoundedPipeline::~BoundedPipeline()
{
    releaseStorage(scratchA);
    releaseStorage(scratchB);
}

void BoundedPipeline::prewarmStageBuffers()
{
    // Grow every internal FIFOSampleBuffer to a deterministic capacity by
    // inserting frames directly into the storage (no DSP is invoked), then
    // clear it. No processing state (WSOLA mid buffer/skip, cubic position)
    // is altered, and process()/resetSegment() never need to grow a buffer.
    auto grow = [this](FIFOSampleBuffer *buf) {
        uint need = kScratchFrames;
        if (buf->getCapacity() < need)
        {
            buf->putSamples(scratchB, need);
            buf->clear();
        }
    };

    grow(static_cast<FIFOSampleBuffer *>(tdStage->getInput()));
    grow(static_cast<FIFOSampleBuffer *>(tdStage->getOutput()));
    grow(rateStage->getInputBuffer());
    grow(rateStage->getMidBuffer());
    grow(static_cast<FIFOSampleBuffer *>(rateStage->getOutput()));
}

void BoundedPipeline::resetSegment() noexcept
{
    inputFifo.clear();
    outputFifo.clear();
    rateStage->resetProcessingState();
    tdStage->resetProcessingState();
    phase = Phase::Running;
    finalRequested = false;
    acceptedInput = 0;
    deliveredOutput = 0;
    targetFrames = 0;
    std::memset(highWater, 0, sizeof(highWater));
}

/// True when the rate stage is the head of the production chain, i.e. rate
/// transposition happens before WSOLA (rate <= 1, same rule as SoundTouch).
static inline bool rateIsHead(float effectiveRate)
{
    return effectiveRate <= 1.0f;
}

void BoundedPipeline::feedHead(const SAMPLETYPE *frames, uint numFrames)
{
    if (rateIsHead(rate))
    {
        rateStage->putSamples(frames, numFrames);
        // moveSamples() resolves to TDStretch::putSamples, which also runs
        // the WSOLA processing batches; moving into the raw input buffer
        // would bypass processSamples() entirely.
        tdStage->moveSamples(*rateStage->getOutput());
    }
    else
    {
        tdStage->putSamples(frames, numFrames);
        rateStage->moveSamples(*tdStage->getOutput());
    }
}

void BoundedPipeline::drainSink(uint limitFrames)
{
    FIFOSamplePipe *sink = rateIsHead(rate)
        ? tdStage->getOutput()
        : rateStage->getOutput();

    while (sink->numSamples() && outputFifo.freeFrames() && limitFrames)
    {
        uint n = std::min({sink->numSamples(), outputFifo.freeFrames(),
                           limitFrames, scratchFrames});
        sink->receiveSamples(scratchA, n);
        outputFifo.push(scratchA, n);
        limitFrames -= n;
    }
}

uint BoundedPipeline::headFreeCapacity()
{
    FIFOSampleBuffer *tdIn = static_cast<FIFOSampleBuffer *>(tdStage->getInput());
    uint tdInFree = tdIn->getCapacity() - tdIn->numSamples();
    uint cap = kFeedChunkFrames;

    if (rateIsHead(rate))
    {
        // Rate transposer is the head. A rate-down transposer expands frames
        // by up to 1/rate, so scale the WSOLA input free space accordingly.
        FIFOSampleBuffer *rtIn = rateStage->getInputBuffer();
        cap = std::min(cap, rtIn->getCapacity() - rtIn->numSamples());
        cap = std::min(cap, static_cast<uint>(tdInFree * rate));

        // A single WSOLA processing burst is bounded by the seek window
        // (<= 6000 frames at 48 kHz); require the sink to hold one burst.
        FIFOSampleBuffer *tdOut = static_cast<FIFOSampleBuffer *>(tdStage->getOutput());
        if (tdOut->getCapacity() - tdOut->numSamples() < kScratchFrames) cap = 0;
    }
    else
    {
        // WSOLA is the head and its burst is moved straight into the rate
        // stage; the rate-stage output FIFO must absorb one such burst.
        cap = std::min(cap, tdInFree);
        FIFOSampleBuffer *rtOut = static_cast<FIFOSampleBuffer *>(rateStage->getOutput());
        if (rtOut->getCapacity() - rtOut->numSamples() < kScratchFrames) cap = 0;
    }
    return cap;
}

void BoundedPipeline::recordHighWater()
{
    highWater[0] = std::max(highWater[0], inputFifo.numFrames());
    highWater[1] = std::max(highWater[1], outputFifo.numFrames());
    highWater[2] = std::max(highWater[2], tdStage->getInput()->numSamples());
    highWater[3] = std::max(highWater[3], tdStage->getOutput()->numSamples());
    highWater[4] = std::max(highWater[4], rateStage->getInputBuffer()->numSamples());
    highWater[5] = std::max(highWater[5], rateStage->getMidBuffer()->numSamples());
    highWater[6] = std::max(highWater[6], rateStage->getOutput()->numSamples());
}

void BoundedPipeline::pumpRunning()
{
    bool progress = true;
    while (progress)
    {
        progress = false;

        // Move finished frames towards the caller first to free stage space.
        uint before = outputFifo.numFrames();
        drainSink(std::numeric_limits<uint>::max());
        if (outputFifo.numFrames() != before) progress = true;

        if (inputFifo.numFrames())
        {
            uint cap = headFreeCapacity();
            if (cap)
            {
                uint n = std::min({inputFifo.numFrames(), cap, scratchFrames});
                inputFifo.pop(scratchA, n);
                feedHead(scratchA, n);
                acceptedInput += n;
                progress = true;
            }
        }
        recordHighWater();
    }
}

void BoundedPipeline::pumpTail()
{
    FIFOSamplePipe *sink = rateIsHead(rate)
        ? tdStage->getOutput()
        : rateStage->getOutput();

    bool progress = true;
    while (progress)
    {
        progress = false;

        // Deliver whatever is ready, never beyond the exact target length.
        uint pending = deliveredOutput + outputFifo.numFrames();
        if (pending < targetFrames)
        {
            uint before = outputFifo.numFrames();
            drainSink(targetFrames - pending);
            if (outputFifo.numFrames() != before) progress = true;
        }

        pending = deliveredOutput + outputFifo.numFrames();
        if (pending >= targetFrames) break;

        // Real input has all been accepted; push silence through the real
        // stages to flush WSOLA overlap and the interpolator/AA tail.
        uint cap = headFreeCapacity();
        if (cap == 0) break; // output back pressure: retry on next call

        uint n = std::min(cap, kFeedChunkFrames);
        feedHead(scratchB, n);
        progress = true;
        recordHighWater();
    }

    // drainSink() never moves more than target-pending frames, so once the
    // target is queued/delivered any frames still at the stage sink are
    // silence-derived surplus. Drop them at the sink; frames already handed
    // to the caller are never touched.
    if (deliveredOutput + outputFifo.numFrames() >= targetFrames)
    {
        sink->adjustAmountOfSamples(0);
    }
    recordHighWater();
}

static bool rangesOverlap(const SAMPLETYPE *a, uint aFrames,
                          const SAMPLETYPE *b, uint bFrames, uint channels)
{
    auto a0 = reinterpret_cast<uintptr_t>(a);
    auto a1 = reinterpret_cast<uintptr_t>(a + aFrames * channels);
    auto b0 = reinterpret_cast<uintptr_t>(b);
    auto b1 = reinterpret_cast<uintptr_t>(b + bFrames * channels);
    return a0 < b1 && b0 < a1;
}

BoundedResult BoundedPipeline::process(const SAMPLETYPE *input,
                                       uint inputFrames,
                                       SAMPLETYPE *output,
                                       uint outputCapacity, bool final)
{
    if (phase == Phase::Finished)
    {
        return BoundedResult{0, 0, true};
    }

    // Argument validation happens before any state change.
    if ((phase == Phase::FinalLocked || finalRequested) && inputFrames != 0)
    {
        throw std::logic_error(
            "Bounded mode: input is locked after final, only zero-input draining is allowed");
    }
    if (inputFrames != 0 && input == nullptr)
    {
        throw std::invalid_argument("Bounded mode: input pointer is null with non-zero input");
    }
    if (outputCapacity != 0 && output == nullptr)
    {
        throw std::invalid_argument("Bounded mode: output pointer is null with non-zero capacity");
    }
    if (inputFrames != 0 && outputCapacity != 0 &&
        rangesOverlap(input, inputFrames, output, outputCapacity, channels))
    {
        throw std::invalid_argument("Bounded mode: input and output buffers must not overlap");
    }

    uint consumed = 0;
    if (phase == Phase::Running)
    {
        if (inputFrames != 0)
        {
            uint want = std::min(inputFrames, inputFifo.freeFrames());
            consumed = inputFifo.push(input, want);
        }

        pumpRunning();

        // Remember the end request; the lock is taken below only once every
        // accepted frame has actually been staged, so the target length is
        // computed from the complete real input even under back pressure.
        if (final && consumed == inputFrames)
        {
            finalRequested = true;
        }
    }

    if (phase == Phase::Running && finalRequested && inputFifo.isEmpty())
    {
        phase = Phase::FinalLocked;
        double denom = static_cast<double>(rate) * static_cast<double>(tempo);
        targetFrames = static_cast<uint>(
            std::floor(static_cast<double>(acceptedInput) / denom + 0.5));
    }

    if (phase == Phase::FinalLocked)
    {
        // Stage any real frames still queued, then drive the silence tail.
        pumpRunning();
        pumpTail();
    }

    uint produced = 0;
    if (outputCapacity != 0)
    {
        produced = outputFifo.pop(output, std::min(outputCapacity,
                                                   outputFifo.numFrames()));
        deliveredOutput += produced;
    }
    recordHighWater();

    bool finished = false;
    if (phase == Phase::FinalLocked &&
        deliveredOutput >= targetFrames &&
        outputFifo.isEmpty())
    {
        phase = Phase::Finished;
        finished = true;
    }

    return BoundedResult{consumed, produced, finished};
}

uint BoundedPipeline::observableBufferCount() const noexcept
{
    return 7u;
}

uint BoundedPipeline::observableBufferCapacity(uint index) const
{
    switch (index)
    {
        case 0: return inputFifo.capacity();
        case 1: return outputFifo.capacity();
        case 2:
            return static_cast<const FIFOSampleBuffer *>(tdStage->getInput())->getCapacity();
        case 3:
            return static_cast<const FIFOSampleBuffer *>(tdStage->getOutput())->getCapacity();
        case 4:
            return static_cast<const FIFOSampleBuffer *>(rateStage->getInputBuffer())->getCapacity();
        case 5:
            return static_cast<const FIFOSampleBuffer *>(rateStage->getMidBuffer())->getCapacity();
        default:
            return static_cast<const FIFOSampleBuffer *>(rateStage->getOutput())->getCapacity();
    }
}

uint BoundedPipeline::observableBufferHighWater(uint index) const
{
    return highWater[index];
}
