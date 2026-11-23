////////////////////////////////////////////////////////////////////////////////
///
/// BoundedPipeline : pre-allocated, back-pressure aware processing mode for
/// SoundTouch. Unlike the legacy putSamples/receiveSamples interface, every
/// FIFO used by this mode has a fixed capacity decided at prepareBounded()
/// time; processBounded()/clearBounded() never allocate or free memory and
/// honour the output capacity handed by the caller.
///
/// The pipeline drives the real production stages (WSOLA TDStretch, cubic
/// rate transposer with its 64-tap anti-alias FIR and the stage FIFOs)
/// directly; it is not a wrapper around another SoundTouch instance.
///
/// Author        : SoundTouch bounded-mode addition
///
////////////////////////////////////////////////////////////////////////////////

#ifndef BoundedPipeline_H
#define BoundedPipeline_H

#include <cstddef>
#include <cstdint>

#include "SoundTouch.h"
#include "FIFOSampleBuffer.h"

namespace soundtouch
{

/// Fixed-capacity interleaved frame FIFO. Storage is allocated once and
/// never resized.
class BoundedFrameFifo
{
private:
    SAMPLETYPE *storage;
    uint capacityFrames;
    uint channels;
    uint head;       ///< index of oldest frame
    uint fill;       ///< frames currently queued

public:
    BoundedFrameFifo();
    ~BoundedFrameFifo();

    BoundedFrameFifo(const BoundedFrameFifo &) = delete;
    BoundedFrameFifo &operator=(const BoundedFrameFifo &) = delete;

    void allocate(uint frameCapacity, uint numChannels);
    void release() noexcept;
    void clear() noexcept;

    /// Appends up to numFrames frames, returns how many were actually stored.
    uint push(const SAMPLETYPE *src, uint numFrames) noexcept;

    /// Copies up to numFrames frames out and removes them.
    uint pop(SAMPLETYPE *dst, uint numFrames) noexcept;

    uint numFrames() const noexcept { return fill; }
    uint capacity() const noexcept { return capacityFrames; }
    uint freeFrames() const noexcept { return capacityFrames - fill; }
    bool isEmpty() const noexcept { return fill == 0; }
};


/// Drives one bounded SoundTouch stream. Owned by SoundTouch.
class BoundedPipeline
{
public:
    /// Hard upper limit for every production FIFO, including the
    /// intermediate queues living inside the legacy processing stages.
    static constexpr uint FIFO_FRAME_LIMIT = 65536u;

    // Instrumented allocation entry points: prepareBounded() is the only
    // place that may acquire/release bounded-mode storage, so the counters
    // let tests verify that processBounded()/clearBounded() do not allocate.
    static void *allocateStorage(size_t bytes);
    static void releaseStorage(void *ptr) noexcept;
    static uint64_t totalAllocations() noexcept;
    static uint64_t totalDeallocations() noexcept;

    BoundedPipeline(SoundTouch &owner, uint numChannels,
                    float effectiveRate, float effectiveTempo,
                    bool useAaFilter, uint aaFilterTaps);
    ~BoundedPipeline();

    BoundedPipeline(const BoundedPipeline &) = delete;
    BoundedPipeline &operator=(const BoundedPipeline &) = delete;

    /// Discards the current segment and its end-of-stream state while
    /// keeping the configuration and every pre-allocated buffer.
    void resetSegment() noexcept;

    BoundedResult process(const SAMPLETYPE *input, uint inputFrames,
                          SAMPLETYPE *output, uint outputCapacity, bool final);

    /// Number of observable production FIFOs.
    uint observableBufferCount() const noexcept;
    /// Fixed capacity (in frames) of observable FIFO 'index'.
    uint observableBufferCapacity(uint index) const;
    /// Largest fill level (in frames) observed so far.
    uint observableBufferHighWater(uint index) const;

private:
    enum class Phase : unsigned char
    {
        Running,    ///< accepting real input
        FinalLocked,///< all real input accepted, draining end compensation
        Finished    ///< everything delivered
    };

    RateTransposer * const rateStage;
    TDStretch * const tdStage;

    uint channels;
    float rate;
    float tempo;

    Phase phase;
    bool finalRequested;   ///< caller asked for final; lock once input drains

    BoundedFrameFifo inputFifo;   ///< caller input waiting for the stages
    BoundedFrameFifo outputFifo;  ///< finished frames waiting for the caller

    SAMPLETYPE *scratchA;         ///< staging area between stages
    SAMPLETYPE *scratchB;         ///< staging area for zero tail frames
    uint scratchFrames;

    uint acceptedInput;           ///< real input frames accepted so far
    uint deliveredOutput;         ///< frames handed to the caller so far
    uint targetFrames;            ///< exact total output length once final
    uint highWater[7];            ///< fills: 2 bounded FIFOs + 5 stage FIFOs

    void prewarmStageBuffers();
    void recordHighWater();
    void pumpRunning();
    void pumpTail();
    void feedHead(const SAMPLETYPE *frames, uint numFrames);
    void drainSink(uint limitFrames);
    uint headFreeCapacity();
};

} // namespace soundtouch

#endif // BoundedPipeline_H
