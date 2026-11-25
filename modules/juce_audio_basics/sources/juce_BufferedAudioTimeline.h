/*
  ==============================================================================

   This file is part of the JUCE library.
   Copyright (c) 2022 - Raw Material Software Limited

   JUCE is an open source library subject to commercial or open-source
   licensing.

   The code included in this file is provided under the terms of the ISC license
   http://www.isc.org/downloads/software-support-policy/isc-license. Permission
   To use, copy, modify, and/or distribute this software for any purpose with or
   without fee is hereby granted provided that the above copyright notice and
   this permission notice appear in all copies.

   JUCE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
   EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
   DISCLAIMED.

  ==============================================================================
*/

#include <cstdint>
#include <memory>
#include <vector>

namespace juce
{

//==============================================================================
/**
    Renders several BufferingAudioSources as one sample-accurate, seekable
    timeline.

    Every track wraps a separate finite, non-looping, positionable float source
    with 1 or 2 channels, all running at the same sample rate. A track's
    sourceStart is the source frame that is aligned with the timeline's output
    position 0, so tracks can be staggered on the timeline and re-timed without
    ever drifting apart: a block is only ever rendered when every track can
    supply the whole block, and all tracks consume it together through
    BufferingAudioSource tickets.

    The timeline does not own its sources and never takes over their lifetime.
    Public calls must be serialised by the caller, but the sources' own
    background read-ahead, seeks and ticket operations may interleave with
    tryRender().

    @tags{Audio}
*/
class JUCE_API  BufferedAudioTimeline
{
public:
    //==============================================================================
    /** One track of the timeline. */
    struct Track
    {
        /** The buffering source that supplies this track. Must stay alive while
            the timeline uses it. */
        BufferingAudioSource* source;

        /** Source frame aligned with output position 0. */
        int64 sourceStart;

        /** Linear gain applied when mixing this track. Must be finite. */
        float gain;
    };

    //==============================================================================
    /** The minimum and maximum number of tracks a timeline can contain. */
    static constexpr int minTracks = 1;
    static constexpr int maxTracks = 8;

    /** The largest block size a prepared timeline accepts. */
    static constexpr int maxBlockSize = 4096;

    //==============================================================================
    /** Creates a timeline from a copy of the supplied track configuration.

        Throws std::invalid_argument when there are not between 1 and 8 tracks,
        when two tracks name the same source, when a gain is non-finite, or when
        a source start is outside its source's [0, length) range.
    */
    explicit BufferedAudioTimeline (std::vector<Track> tracks);

    /** Destructor. */
    ~BufferedAudioTimeline() = default;

    JUCE_DECLARE_NON_COPYABLE (BufferedAudioTimeline)
    JUCE_DECLARE_NON_MOVEABLE (BufferedAudioTimeline)

    //==============================================================================
    /** Prepares every source.

        maxBlockSamples must be between 1 and 4096 and sampleRate must be a
        finite, positive number; otherwise the call returns false and leaves
        every source untouched. A successful prepare restarts the timeline at
        output position 0.
    */
    bool prepareToPlay (int maxBlockSamples, double sampleRate);

    /** Releases every source and discards the prepared state. */
    void releaseResources();

    /** Moves the whole group to an output position in [0, getTotalLength()].

        The full configuration is checked before any source moves; on failure
        nothing changes. After a successful seek the background threads may
        still need to refill their buffers.
    */
    bool seek (int64 outputFrame);

    /** Assigns new source starts to the tracks, in construction order.

        The current output position is preserved and the total length is
        recomputed. Every new start must be inside its source and the current
        output position must still fit; otherwise the call fails without
        moving any source.
    */
    bool retime (const std::vector<int64>& sourceStarts);

    /** Renders one complete block, or changes nothing.

        Each track's ticket must start at sourceStart + the current output
        position. Track PCM is multiplied by its gain and added channel by
        channel without clipping; a mono track is copied into both output
        channels. If any track is not ready, is mispositioned, loses its ticket
        before the group commit, or the request exceeds maxBlockSize, the
        remaining length or the output region, the method returns false after
        writing a sentinel into the requested region: no source consumption and
        no timeline position changes.

        A zero-length request consumes nothing and writes nothing; it returns
        true when the timeline is prepared and the request itself is valid.
    */
    bool tryRender (const AudioSourceChannelInfo& info);

    /** Returns the current output position. */
    int64 getOutputPosition() const noexcept          { return outputPosition; }

    /** Returns the smallest (source length - source start) across the tracks. */
    int64 getTotalLength() const noexcept             { return totalLength; }

private:
    //==============================================================================
    struct TrackState
    {
        BufferingAudioSource* source;
        int64 sourceStart;
        float gain;
        int channels;
        BufferingAudioSource::ReadTicket ticket;
    };

    //==============================================================================
    bool configurationFits (int64 outputPosition) const;
    void recomputeTotalLength();

    //==============================================================================
    std::vector<TrackState> trackStates;
    std::vector<std::unique_ptr<AudioBuffer<float>>> trackScratch;
    AudioBuffer<float> mixScratch;
    int64 outputPosition = 0;
    int64 totalLength = 0;
    int preparedBlockSize = 0;
    double preparedSampleRate = 0;
    bool isPrepared = false;
};

} // namespace juce
