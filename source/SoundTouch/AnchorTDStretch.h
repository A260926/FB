////////////////////////////////////////////////////////////////////////////////
///
/// AnchorTDStretch : anchored time-stretch engine.
///
/// The class implements the anchor-protected time stretching used by
/// SoundTouch::setTimeAnchors(). It reuses the WSOLA primitives of the
/// TDStretch base class (cross-correlation based overlap position seeking and
/// triangular overlap synthesis), but drives them with an explicit piecewise
/// affine time map:
///
///   - Protected zones around every anchor are copied through verbatim, in the
///     original sample order, with no cross-fade, resampling or duplication.
///   - Each gap between two protected zones is stretched independently with
///     WSOLA. The nominal analysis window position follows the affine map of
///     that gap only; correlation seek offsets are applied to the current
///     splice point and are never accumulated into the nominal clock.
///   - Windows are shortened / re-organised next to a protected zone so that
///     overlap tails always finish before the zone begins, and no analysis
///     window ever reaches across a zone.
///
/// The engine keeps bounded PCM storage regardless of the total stream length:
/// only the current analysis window, the overlap 'mid' buffer and a small
/// amount of input/output slack are held.
///
////////////////////////////////////////////////////////////////////////////////

#ifndef AnchorTDStretch_H
#define AnchorTDStretch_H

#include <stdint.h>
#include <vector>

#include "TimeAnchor.h"
#include "TDStretch.h"

namespace soundtouch
{

class AnchorTDStretch : private TDStretch
{
public:
    AnchorTDStretch();
    ~AnchorTDStretch();

    // TDStretch hides plain 'new' behind an error-throwing operator; the
    // anchored engine is always created directly, so restore the global one.
    static void *operator new(size_t s) { return ::operator new(s); }
    static void operator delete(void *p) { ::operator delete(p); }

    /// Sets the channel count / WSOLA window constants. The anchored mode is
    /// only supported for 1 or 2 channels at 48 kHz.
    void configure(int numChannels, int sampleRate,
                   int sequenceMs, int seekWindowMs, int overlapMs);

    /// (Re)starts a finite anchored run. 'anchors' has already been validated
    /// by SoundTouch. Discards every buffered sample.
    void reset(const std::vector<TimeAnchor> &anchors, uint protectFrames);

    /// Feeds input frames and runs as much deterministic processing as the
    /// currently available input allows.
    void putSamples(const float *samples, uint numFrames);

    /// Copies up to 'maxFrames' ready output frames out and removes them.
    uint receiveSamples(float *outBuffer, uint maxFrames);

    /// Number of output frames ready to be received.
    uint numSamples() const;

    /// Number of input frames currently held but not yet consumed.
    uint numUnprocessedFrames() const;

    /// Total input frame count expected for this run.
    uint64_t getExpectedInputFrames() const;

    /// Total output frame count this run must produce.
    uint64_t getExpectedOutputFrames() const;

    /// Number of input frames fed so far.
    uint64_t getInputFramesFed() const;

    /// Finishes the stream once all N input frames have been fed. Flushes the
    /// last stretch segment and the final protected zone so that exactly M
    /// output frames are produced.
    void finish();

    /// True after finish() has completed.
    bool isFinished() const;

    /// Discards all buffered samples and run state.
    void clear();

    /// Total PCM frame capacity currently held by the engine (input buffer +
    /// output buffer + overlap mid buffer). Used to verify the bounded-storage
    /// guarantee; does not count the anchor table.
    uint64_t storageFrames() const;

private:
    int channels;
    int overlapLen;       // O
    int seekWindowLen;    // SW
    int seekRange;        // full-scan candidate half width (== seekLength)
    int batchOut;         // nominal output batch size B = SW - O

    std::vector<TimeAnchor> anchors;
    uint protectFrames;
    uint64_t totalIn;
    uint64_t totalOut;

    // Input FIFO: frames [inStart, inStart + inBuf.size()/channels)
    std::vector<float> inBuf;
    uint64_t inStart;
    size_t inHead;        // consumed position inside inBuf, in floats

    // Output FIFO: ready frames waiting to be received.
    std::vector<float> outBuf;
    size_t outHead;       // received position inside outBuf, in floats
    uint64_t outProduced; // total output frames produced so far

    uint64_t fedFrames;
    bool finished;

    // Current stretch segment state, in absolute frame coordinates.
    int segment;                  // index of the anchor starting the segment
    bool headEmitted;             // first protected zone already copied
    uint64_t segInBegin;          // a = s_i + P
    uint64_t segInEnd;            // s_{i+1} - P
    uint64_t segOutBegin;         // t_i + P
    uint64_t segOutEnd;           // t_{i+1} - P
    uint64_t segVin;              // input void length
    uint64_t segVout;             // output void length
    uint64_t segNomClock;         // integer nominal input position H (segment-local)
    double segNomSkip;            // nominal input advance per output batch
    double segSkipFract;          // fractional part of the nominal skip
    uint64_t segOutMade;          // output frames produced within this segment
    bool haveMid;
    std::vector<float> scratch;   // cross-fade scratch, overlapLen frames

    void setupSegment(int index);
    void pump();
    bool emitProtectedZone(uint64_t inBegin, uint64_t outBegin, uint length);
    bool stretchStep();
    void appendOutput(const float *data, uint frames);
    void consumeInput(uint64_t absoluteFrame);
    const float *inputAt(uint64_t absoluteFrame) const;
    void compactInput();
    void compactOutput();
};

}

#endif  // AnchorTDStretch_H
