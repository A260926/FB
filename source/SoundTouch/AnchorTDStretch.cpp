////////////////////////////////////////////////////////////////////////////////
///
/// AnchorTDStretch implementation.
///
/// See AnchorTDStretch.h for the design overview. The class drives the
/// inherited WSOLA correlation/overlap primitives with an explicit piecewise
/// affine time map and verbatim protected zones.
///
/// The stretch driver mirrors the geometry of TDStretch::processSamples():
/// a fixed-size output batch, a constant nominal input skip derived from the
/// affine map, a forward-only correlation seek whose offset is local to one
/// splice, and the same triangular overlap / mid-buffer update. The only
/// structural differences are that the nominal clock comes from the segment's
/// affine map instead of a single tempo value and that every analysis window
/// is constrained to stay inside the stretch segment.
///
////////////////////////////////////////////////////////////////////////////////

#include <math.h>
#include <string.h>
#include <algorithm>

#include "AnchorTDStretch.h"

using namespace soundtouch;

AnchorTDStretch::AnchorTDStretch()
    : TDStretch()
{
    channels = 2;
    overlapLen = 0;
    seekWindowLen = 0;
    seekRange = 0;
    batchOut = 0;
    protectFrames = 0;
    totalIn = 0;
    totalOut = 0;
    inStart = 0;
    inHead = 0;
    outHead = 0;
    outProduced = 0;
    fedFrames = 0;
    finished = false;
    segment = 0;
    headEmitted = false;
    segInBegin = segInEnd = 0;
    segOutBegin = segOutEnd = 0;
    segVin = segVout = 0;
    segNomClock = 0;
    segNomSkip = 0.0;
    segSkipFract = 0.0;
    segOutMade = 0;
    haveMid = false;
}


AnchorTDStretch::~AnchorTDStretch()
{
}


void AnchorTDStretch::configure(int numChannels, int sampleRate,
                                int sequenceMs, int seekWindowMs, int overlapMs)
{
    // The anchored mode only supports mono/stereo float at 48 kHz.
    channels = numChannels;
    TDStretch::setChannels(numChannels);
    TDStretch::setParameters(sampleRate, sequenceMs, seekWindowMs, overlapMs);
    TDStretch::enableQuickSeek(FALSE);

    overlapLen = TDStretch::overlapLength;
    seekWindowLen = TDStretch::seekWindowLength;
    seekRange = TDStretch::seekLength;
    batchOut = seekWindowLen - overlapLen;
}


void AnchorTDStretch::clear()
{
    TDStretch::clear();
    inBuf.clear();
    outBuf.clear();
    inStart = 0;
    inHead = 0;
    outHead = 0;
    outProduced = 0;
    fedFrames = 0;
    finished = false;
    segment = 0;
    headEmitted = false;
    haveMid = false;
    segNomClock = 0;
    segNomSkip = 0.0;
    segSkipFract = 0.0;
    segOutMade = 0;
}


void AnchorTDStretch::reset(const std::vector<TimeAnchor> &aAnchors, uint aProtectFrames)
{
    clear();
    anchors = aAnchors;
    protectFrames = aProtectFrames;
    totalIn = anchors.back().inputFrame;
    totalOut = anchors.back().outputFrame;
    headEmitted = false;
    segment = 0;
    finished = false;
}


uint64_t AnchorTDStretch::getExpectedInputFrames() const
{
    return totalIn;
}


uint64_t AnchorTDStretch::getExpectedOutputFrames() const
{
    return totalOut;
}


uint64_t AnchorTDStretch::getInputFramesFed() const
{
    return fedFrames;
}


bool AnchorTDStretch::isFinished() const
{
    return finished;
}


uint AnchorTDStretch::numSamples() const
{
    return (uint)(outBuf.size() / (size_t)channels - outHead / (size_t)channels);
}


uint AnchorTDStretch::numUnprocessedFrames() const
{
    return (uint)(inBuf.size() / (size_t)channels - inHead / (size_t)channels);
}


uint64_t AnchorTDStretch::storageFrames() const
{
    uint64_t frames = 0;
    frames += (uint64_t)(inBuf.size() / (size_t)channels - inHead / (size_t)channels);
    frames += (uint64_t)(outBuf.size() / (size_t)channels - outHead / (size_t)channels);
    frames += (uint64_t)overlapLen;       // inherited pMidBuffer
    return frames;
}


const float *AnchorTDStretch::inputAt(uint64_t absoluteFrame) const
{
    size_t idx = inHead + (size_t)(absoluteFrame - inStart) * (size_t)channels;
    return inBuf.data() + idx;
}


void AnchorTDStretch::consumeInput(uint64_t absoluteFrame)
{
    uint64_t inEnd = inStart + (uint64_t)(inBuf.size() / (size_t)channels)
                   - (uint64_t)(inHead / (size_t)channels);
    if (absoluteFrame > inEnd)
    {
        // never mark frames not yet fed as consumed
        absoluteFrame = inEnd;
    }
    if (absoluteFrame <= inStart) return;

    size_t newHead = inHead + (size_t)(absoluteFrame - inStart) * (size_t)channels;
    if (newHead > inBuf.size()) newHead = inBuf.size();
    inHead = newHead;
    inStart = absoluteFrame;

    if (inHead == inBuf.size())
    {
        inBuf.clear();
        inHead = 0;
    }
    else if (inHead > 64u * 1024u * (size_t)channels)
    {
        compactInput();
    }
}


void AnchorTDStretch::compactInput()
{
    if (inHead == 0) return;
    inBuf.erase(inBuf.begin(), inBuf.begin() + (long)inHead);
    inHead = 0;
}


void AnchorTDStretch::compactOutput()
{
    if (outHead == 0) return;
    outBuf.erase(outBuf.begin(), outBuf.begin() + (long)outHead);
    outHead = 0;
}


void AnchorTDStretch::appendOutput(const float *data, uint frames)
{
    outBuf.insert(outBuf.end(), data, data + (size_t)frames * (size_t)channels);
    outProduced += frames;
}


uint AnchorTDStretch::receiveSamples(float *outBuffer, uint maxFrames)
{
    size_t avail = outBuf.size() - outHead;
    size_t framesAvail = avail / (size_t)channels;
    size_t n = (maxFrames < framesAvail) ? (size_t)maxFrames : framesAvail;
    if (n)
    {
        memcpy(outBuffer, outBuf.data() + outHead, n * (size_t)channels * sizeof(float));
        outHead += n * (size_t)channels;
        if (outHead == outBuf.size())
        {
            outBuf.clear();
            outHead = 0;
        }
        else if (outHead > 64u * 1024u * (size_t)channels)
        {
            compactOutput();
        }
    }
    return (uint)n;
}


void AnchorTDStretch::putSamples(const float *samples, uint numFrames)
{
    if (numFrames == 0) return;
    inBuf.insert(inBuf.end(), samples,
                 samples + (size_t)numFrames * (size_t)channels);
    fedFrames += numFrames;
    pump();
}


void AnchorTDStretch::setupSegment(int index)
{
    segment = index;
    const TimeAnchor &a = anchors[index];
    const TimeAnchor &b = anchors[index + 1];

    segInBegin = a.inputFrame + protectFrames;
    segInEnd = b.inputFrame - protectFrames;
    segOutBegin = a.outputFrame + protectFrames;
    segOutEnd = b.outputFrame - protectFrames;
    segVin = segInEnd - segInBegin;
    segVout = segOutEnd - segOutBegin;

    // Constant nominal input advance per fixed output batch, equivalent to the
    // affine map of this segment. Fractional accumulation is carried exactly
    // like TDStretch::skipFract so the mapping error never builds up.
    segNomSkip = (double)batchOut * (double)segVin / (double)segVout;
    segNomClock = 0;
    segSkipFract = 0.0;
    segOutMade = 0;
    haveMid = false;
}


bool AnchorTDStretch::emitProtectedZone(uint64_t inBegin, uint64_t outBegin, uint length)
{
    // Protected samples are emitted exactly once, at their mapped position,
    // with the original gain and sample order.
    if (outProduced != outBegin) return false;

    uint64_t inEnd = inStart + (uint64_t)(inBuf.size() / (size_t)channels)
                   - (uint64_t)(inHead / (size_t)channels);
    if (inEnd < (uint64_t)inBegin + length) return false;   // wait for more input

    appendOutput(inputAt(inBegin), length);
    consumeInput(inBegin + length);
    return true;
}


bool AnchorTDStretch::stretchStep()
{
    const int O = overlapLen;
    const int SW = seekWindowLen;
    const int B = batchOut;
    const uint64_t voutRem = segVout - segOutMade;

    if (voutRem == 0) return true;

    uint64_t inEnd = inStart + (uint64_t)(inBuf.size() / (size_t)channels)
                   - (uint64_t)(inHead / (size_t)channels);

    if (!haveMid)
    {
        // Seed the overlap mid buffer with the first O input frames and emit
        // them untouched: WSOLA takes over here from the preceding protected
        // zone without cross-fading against it.
        if (inEnd < segInBegin + (uint64_t)O) return false;
        memcpy(TDStretch::pMidBuffer, inputAt(segInBegin),
               (size_t)O * (size_t)channels * sizeof(float));
        appendOutput(TDStretch::pMidBuffer, (uint)O);
        haveMid = true;
        segOutMade = O;
        scratch.resize((size_t)O * (size_t)channels);
        return true;
    }

    // Nominal frame where this splice starts, from the affine clock only.
    uint64_t nomFrame = segInBegin + segNomClock;

    // Close the segment when either the remaining output fits in one window or
    // a full SW window no longer fits before the segment edge. In both cases
    // the last window ends exactly on the segment edge so the next protected
    // zone follows without a gap, duplicate or fade into it.
    bool closing = (voutRem <= (uint64_t)SW) ||
                   (nomFrame + (uint64_t)SW > segInEnd);
    if (closing)
    {
        uint r = (uint)voutRem;                 // remaining output, always > O
        uint64_t winStart = segInEnd - r;
        if (inEnd < segInEnd) return false;     // wait for the remaining input

        const float *pInput = inputAt(winStart);
        overlap(scratch.data(), pInput, 0);
        appendOutput(scratch.data(), (uint)O);
        appendOutput(pInput + (size_t)O * (size_t)channels, r - O);

        segOutMade += r;
        segNomClock = segVin;
        consumeInput(segInEnd);
        return true;
    }

    // Normal splice: one fixed output batch. The correlation scan only picks
    // the splice position; the nominal clock below advances by the affine skip
    // regardless of the chosen offset.
    //
    // Every candidate consumes an SW-frame window starting at the nominal
    // frame, so the last legal candidate keeps the whole window inside the
    // segment: no analysis window may reach into a protected zone.
    long maxStart = (long)segInEnd - (long)SW;
    int range = (int)(maxStart - (long)nomFrame + 1);
    if (range > seekRange) range = seekRange;
    if (range < 1) range = 1;

    // The furthest candidate still reads a full SW-frame window. All of that
    // input must already be present, otherwise the result would depend on how
    // the caller chunked the input.
    uint64_t needThrough = nomFrame + (uint64_t)(range - 1) + (uint64_t)SW;
    if (inEnd < needThrough) return false;                 // wait for input

    int offset = seekBestOverlapPositionLimited(inputAt(nomFrame), range);
    const float *pInput = inputAt(nomFrame + (uint64_t)offset);

    // Triangular overlap against the previous window tail ...
    overlap(scratch.data(), pInput, 0);
    appendOutput(scratch.data(), (uint)O);
    // ... then the straight part of the sequence (SW - 2*O frames) ...
    int temp = SW - 2 * O;
    appendOutput(pInput + (size_t)O * (size_t)channels, (uint)temp);
    // ... and the last O frames become the next overlap reference.
    memcpy(TDStretch::pMidBuffer,
           pInput + (size_t)(temp + O) * (size_t)channels,
           (size_t)O * (size_t)channels * sizeof(float));

    segOutMade += B;

    // Advance the affine clock by the nominal skip, keeping the fraction so
    // the rounding error cannot accumulate. The seek offset is not part of
    // this clock.
    segSkipFract += segNomSkip;
    int ovlSkip = (int)segSkipFract;
    segSkipFract -= (double)ovlSkip;
    segNomClock += (uint64_t)ovlSkip;

    // Release input while keeping a fixed look-back behind the nominal clock:
    // the final splice window can start up to SW frames earlier than the
    // clock. The clock is monotonic, so the retained amount is bounded
    // independently of the gap length.
    uint64_t lookback = (uint64_t)SW + (uint64_t)seekRange;
    uint64_t retain = (segNomClock > lookback)
                      ? segInBegin + segNomClock - lookback
                      : segInBegin;
    consumeInput(retain);
    return true;
}


void AnchorTDStretch::pump()
{
    const uint64_t P = protectFrames;
    const uint lastAnchor = (uint)anchors.size() - 1;

    while (true)
    {
        if (!headEmitted)
        {
            // First protected zone, anchored at (0, 0).
            if (!emitProtectedZone(0, 0, P)) return;
            headEmitted = true;
            setupSegment(0);
        }

        if (segment >= (int)lastAnchor) return;       // final zone already out

        if (segOutMade >= segVout)
        {
            // Stretch segment finished: emit the next anchor's protected
            // zone, then move on to the following segment. Internal anchors
            // protect [s-P, s+P) (length 2P); the final anchor only protects
            // [N-P, N) (length P).
            const TimeAnchor &an = anchors[segment + 1];
            bool isLast = ((uint)(segment + 1) == lastAnchor);
            uint zoneLen = isLast ? P : 2 * P;
            uint64_t inBegin = an.inputFrame - P;
            uint64_t outBegin = an.outputFrame - P;
            if (!emitProtectedZone(inBegin, outBegin, zoneLen)) return;

            segment++;
            if ((uint)segment >= lastAnchor) return;
            setupSegment(segment);
        }

        if (!stretchStep()) return;
    }
}


void AnchorTDStretch::finish()
{
    // All N input frames are present; drain every remaining segment/zone.
    pump();
    finished = true;
}
