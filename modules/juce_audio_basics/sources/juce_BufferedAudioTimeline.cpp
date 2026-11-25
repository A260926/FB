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

#include <array>
#include <stdexcept>
#include <memory>
#include <vector>

namespace juce
{

BufferedAudioTimeline::BufferedAudioTimeline (std::vector<Track> tracks)
{
    if (tracks.size() < (size_t) minTracks || tracks.size() > (size_t) maxTracks)
        throw std::invalid_argument ("BufferedAudioTimeline: track count must be between 1 and 8");

    trackStates.reserve (tracks.size());

    for (const auto& track : tracks)
    {
        if (track.source == nullptr || ! juce_isfinite (track.gain))
            throw std::invalid_argument ("BufferedAudioTimeline: null source or non-finite gain");

        for (const auto& existing : trackStates)
        {
            if (existing.source == track.source)
                throw std::invalid_argument ("BufferedAudioTimeline: sources must be unique");
        }

        const auto length = track.source->getTotalLength();

        if (length <= 0 || track.sourceStart < 0 || track.sourceStart >= length)
            throw std::invalid_argument ("BufferedAudioTimeline: source start outside the source");

        trackStates.push_back ({ track.source,
                                 track.sourceStart,
                                 track.gain,
                                 jlimit (1, 2, track.source->getNumChannels()),
                                 {} });
    }

    recomputeTotalLength();
}

//==============================================================================
bool BufferedAudioTimeline::prepareToPlay (int maxBlockSamples, double sampleRate)
{
    if (maxBlockSamples < 1 || maxBlockSamples > maxBlockSize
        || ! juce_isfinite (sampleRate) || sampleRate <= 0.0)
    {
        return false;
    }

    // Validate the whole configuration before touching any source.
    for (const auto& ts : trackStates)
    {
        const auto length = ts.source->getTotalLength();

        if (length <= 0 || ts.sourceStart < 0 || ts.sourceStart >= length)
            return false;
    }

    for (auto& ts : trackStates)
        ts.source->prepareToPlay (maxBlockSamples, sampleRate);

    preparedBlockSize = maxBlockSamples;
    preparedSampleRate = sampleRate;
    isPrepared = true;

    // All staging space is acquired here; rendering itself never allocates.
    // Every track gets a private stereo staging buffer because its ticket
    // stays outstanding until the whole group commits.
    trackScratch.clear();
    trackScratch.reserve (trackStates.size());

    for (size_t i = 0; i < trackStates.size(); ++i)
        trackScratch.emplace_back (std::make_unique<AudioBuffer<float>> (2, maxBlockSamples));

    mixScratch.setSize (2, maxBlockSamples);

    // A prepare always restarts the timeline at output position 0.
    outputPosition = 0;
    recomputeTotalLength();

    for (auto& ts : trackStates)
        ts.source->setNextReadPosition (ts.sourceStart);

    return true;
}

void BufferedAudioTimeline::releaseResources()
{
    for (auto& ts : trackStates)
        ts.source->releaseResources();

    trackScratch.clear();
    mixScratch.setSize (2, 0);
    isPrepared = false;
    preparedBlockSize = 0;
    preparedSampleRate = 0;
}

//==============================================================================
bool BufferedAudioTimeline::configurationFits (int64 position) const
{
    if (position < 0)
        return false;

    for (const auto& ts : trackStates)
    {
        if (ts.sourceStart < 0 || position > ts.source->getTotalLength() - ts.sourceStart)
            return false;
    }

    return true;
}

void BufferedAudioTimeline::recomputeTotalLength()
{
    bool first = true;

    for (const auto& ts : trackStates)
    {
        const auto available = ts.source->getTotalLength() - ts.sourceStart;

        if (first || available < totalLength)
        {
            totalLength = available;
            first = false;
        }
    }
}

bool BufferedAudioTimeline::seek (int64 outputFrame)
{
    if (outputFrame < 0 || outputFrame > totalLength || ! configurationFits (outputFrame))
        return false;

    // Everything is valid: move the whole group together.
    for (auto& ts : trackStates)
        ts.source->setNextReadPosition (ts.sourceStart + outputFrame);

    outputPosition = outputFrame;
    return true;
}

bool BufferedAudioTimeline::retime (const std::vector<int64>& sourceStarts)
{
    if ((int64) sourceStarts.size() != (int64) trackStates.size())
        return false;

    // Check the complete new configuration, including the preserved output
    // position, before moving a single source.
    for (size_t i = 0; i < trackStates.size(); ++i)
    {
        const auto newStart = sourceStarts[i];
        const auto length = trackStates[i].source->getTotalLength();

        if (newStart < 0 || newStart >= length
            || outputPosition > length - newStart)
        {
            return false;
        }
    }

    for (size_t i = 0; i < trackStates.size(); ++i)
    {
        auto& ts = trackStates[i];
        ts.sourceStart = sourceStarts[i];
        ts.source->setNextReadPosition (ts.sourceStart + outputPosition);
    }

    recomputeTotalLength();
    return true;
}

//==============================================================================
bool BufferedAudioTimeline::tryRender (const AudioSourceChannelInfo& info)
{
    const auto failWithSentinel = [&]
    {
        if (info.buffer != nullptr && info.numSamples > 0
            && info.startSample >= 0
            && info.startSample + info.numSamples <= info.buffer->getNumSamples())
        {
            info.buffer->clear (info.startSample, info.numSamples);
        }

        return false;
    };

    if (! isPrepared)
        return failWithSentinel();

    if (info.buffer == nullptr || info.numSamples < 0 || info.startSample < 0
        || info.startSample + info.numSamples > info.buffer->getNumSamples())
    {
        return false; // the output region itself is unusable, nothing to write
    }

    // A zero-length request is a successful no-op when prepared and valid.
    if (info.numSamples == 0)
        return true;

    if (info.numSamples > preparedBlockSize
        || outputPosition + info.numSamples > totalLength
        || info.buffer->getNumChannels() < 2)
    {
        return failWithSentinel();
    }

    const auto numSamples = info.numSamples;

    // Phase 1: peek every track. Nothing is consumed and the caller's output
    // region is not touched; a single failure discards the whole attempt.
    for (size_t i = 0; i < trackStates.size(); ++i)
    {
        auto& ts = trackStates[i];
        AudioSourceChannelInfo scratchInfo (trackScratch[i].get(), 0, numSamples);

        if (! ts.source->peekNextBlock (scratchInfo, ts.ticket))
        {
            for (auto& other : trackStates)
                other.ticket = BufferingAudioSource::ReadTicket();

            return failWithSentinel();
        }

        // A repositioning by another thread between wait and peek would show
        // up as a ticket that does not cover the timeline's expected frame.
        if (ts.ticket.getStartPosition() != ts.sourceStart + outputPosition)
        {
            for (auto& other : trackStates)
                other.ticket = BufferingAudioSource::ReadTicket();

            return failWithSentinel();
        }
    }

    // Phase 2: mix the privately staged PCM. The output buffer stays untouched
    // until the group commit has succeeded.
    mixScratch.clear();

    for (size_t i = 0; i < trackStates.size(); ++i)
    {
        const auto& ts = trackStates[i];
        const auto& staged = *trackScratch[i];

        for (int outChan = 0; outChan < 2; ++outChan)
        {
            const auto sourceChan = ts.channels == 1 ? 0 : outChan;

            mixScratch.addFrom (outChan, 0,
                                staged, sourceChan, 0,
                                numSamples, ts.gain);
        }
    }

    // Phase 3: single commit point for all the sources.
    std::array<BufferingAudioSource::ReadTicket*, maxTracks> ticketHandles {};

    for (size_t i = 0; i < trackStates.size(); ++i)
        ticketHandles[i] = &trackStates[i].ticket;

    if (! BufferingAudioSource::commitReadTickets (ticketHandles.data(),
                                                   (int) trackStates.size()))
    {
        return failWithSentinel();
    }

    // Phase 4: committed - publish the mix and advance the timeline cursor.
    for (int chan = 0; chan < 2; ++chan)
    {
        info.buffer->copyFrom (chan, info.startSample,
                               mixScratch, chan, 0, numSamples);
    }

    outputPosition += numSamples;
    return true;
}

} // namespace juce
