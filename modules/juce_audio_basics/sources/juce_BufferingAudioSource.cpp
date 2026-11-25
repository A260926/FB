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

#include <algorithm>
#include <array>

namespace juce
{

BufferingAudioSource::BufferingAudioSource (PositionableAudioSource* s,
                                            TimeSliceThread& thread,
                                            bool deleteSourceWhenDeleted,
                                            int bufferSizeSamples,
                                            int numChannels,
                                            bool prefillBufferOnPrepareToPlay)
    : source (s, deleteSourceWhenDeleted),
      backgroundThread (thread),
      numberOfSamplesToBuffer (jmax (1024, bufferSizeSamples)),
      numberOfChannels (numChannels),
      prefillBuffer (prefillBufferOnPrepareToPlay)
{
    jassert (source != nullptr);

    jassert (numberOfSamplesToBuffer > 1024); // not much point using this class if you're
                                              //  not using a larger buffer..
}

BufferingAudioSource::~BufferingAudioSource()
{
    releaseResources();
}

//==============================================================================
void BufferingAudioSource::prepareToPlay (int samplesPerBlockExpected, double newSampleRate)
{
    auto bufferSizeNeeded = jmax (samplesPerBlockExpected * 2, numberOfSamplesToBuffer);

    if (! approximatelyEqual (newSampleRate, sampleRate)
         || bufferSizeNeeded != buffer.getNumSamples()
         || ! isPrepared)
    {
        backgroundThread.removeTimeSliceClient (this);

        isPrepared = true;
        sampleRate = newSampleRate;

        // Every re-prepare invalidates tickets issued against the previous run.
        {
            const ScopedLock tl (ticketLock);
            ++ticketEpoch;
        }

        source->prepareToPlay (samplesPerBlockExpected, newSampleRate);

        buffer.setSize (numberOfChannels, bufferSizeNeeded);
        buffer.clear();

        const ScopedLock sl (bufferRangeLock);

        bufferValidStart = 0;
        bufferValidEnd = 0;

        backgroundThread.addTimeSliceClient (this);

        do
        {
            const ScopedUnlock ul (bufferRangeLock);

            backgroundThread.moveToFrontOfQueue (this);
            Thread::sleep (5);
        }
        while (prefillBuffer
         && (bufferValidEnd - bufferValidStart < jmin (((int) newSampleRate) / 4, buffer.getNumSamples() / 2)));
    }
}

void BufferingAudioSource::releaseResources()
{
    isPrepared = false;
    backgroundThread.removeTimeSliceClient (this);

    // Tickets can no longer be committed once the source has been released.
    {
        const ScopedLock tl (ticketLock);
        ++ticketEpoch;
    }

    buffer.setSize (numberOfChannels, 0);

    // MSVC2017 seems to need this if statement to not generate a warning during linking.
    // As source is set in the constructor, there is no way that source could
    // ever equal this, but it seems to make MSVC2017 happy.
    if (source != this)
        source->releaseResources();
}

void BufferingAudioSource::getNextAudioBlock (const AudioSourceChannelInfo& info)
{
    const auto bufferRange = getValidBufferRange (info.numSamples);

    if (bufferRange.isEmpty())
    {
        // total cache miss
        info.clearActiveBufferRegion();
        return;
    }

    const auto validStart = bufferRange.getStart();
    const auto validEnd = bufferRange.getEnd();

    const ScopedLock sl (callbackLock);

    if (validStart > 0)
        info.buffer->clear (info.startSample, validStart);  // partial cache miss at start

    if (validEnd < info.numSamples)
        info.buffer->clear (info.startSample + validEnd,
                            info.numSamples - validEnd);    // partial cache miss at end

    if (validStart < validEnd)
    {
        for (int chan = jmin (numberOfChannels, info.buffer->getNumChannels()); --chan >= 0;)
        {
            jassert (buffer.getNumSamples() > 0);

            const auto startBufferIndex = (int) ((validStart + nextPlayPos) % buffer.getNumSamples());
            const auto endBufferIndex   = (int) ((validEnd + nextPlayPos)   % buffer.getNumSamples());

            if (startBufferIndex < endBufferIndex)
            {
                info.buffer->copyFrom (chan, info.startSample + validStart,
                                       buffer,
                                       chan, startBufferIndex,
                                       validEnd - validStart);
            }
            else
            {
                const auto initialSize = buffer.getNumSamples() - startBufferIndex;

                info.buffer->copyFrom (chan, info.startSample + validStart,
                                       buffer,
                                       chan, startBufferIndex,
                                       initialSize);

                info.buffer->copyFrom (chan, info.startSample + validStart + initialSize,
                                       buffer,
                                       chan, 0,
                                       (validEnd - validStart) - initialSize);
            }
        }
    }

    // A normal consume invalidates any tickets issued against the old position,
    // including tickets held by single-source readers.
    {
        const ScopedLock tl (ticketLock);
        ++ticketEpoch;
    }

    nextPlayPos += info.numSamples;
}

bool BufferingAudioSource::waitForNextAudioBlockReady (const AudioSourceChannelInfo& info, uint32 timeout)
{
    if (source == nullptr || source->getTotalLength() <= 0)
        return false;

    if ((nextPlayPos + info.numSamples < 0)
        || (! isLooping() && nextPlayPos > getTotalLength()))
        return true;

    const auto startTime = Time::getMillisecondCounter();
    auto now = startTime;

    auto elapsed = (now >= startTime ? now - startTime
                                     : (std::numeric_limits<uint32>::max() - startTime) + now);

    while (elapsed <= timeout)
    {
        const auto bufferRange = getValidBufferRange (info.numSamples);

        const auto validStart = bufferRange.getStart();
        const auto validEnd = bufferRange.getEnd();

        if (validStart <= 0
            && validStart < validEnd
            && validEnd >= info.numSamples)
        {
            return true;
        }

        if (elapsed < timeout
            && ! bufferReadyEvent.wait (static_cast<int> (timeout - elapsed)))
        {
            return false;
        }

        now = Time::getMillisecondCounter();
        elapsed = (now >= startTime ? now - startTime
                                    : (std::numeric_limits<uint32>::max() - startTime) + now);
    }

    return false;
}

int64 BufferingAudioSource::getNextReadPosition() const
{
    jassert (source->getTotalLength() > 0);
    const auto pos = nextPlayPos.load();

    return (source->isLooping() && nextPlayPos > 0)
                    ? pos % source->getTotalLength()
                    : pos;
}

void BufferingAudioSource::setNextReadPosition (int64 newPosition)
{
    const ScopedLock sl (bufferRangeLock);

    nextPlayPos = newPosition;

    // Any seek invalidates outstanding tickets, even when the new position
    // happens to equal the old one.
    {
        const ScopedLock tl (ticketLock);
        ++ticketEpoch;
    }

    backgroundThread.moveToFrontOfQueue (this);
}

Range<int> BufferingAudioSource::getValidBufferRange (int numSamples) const
{
    const ScopedLock sl (bufferRangeLock);

    const auto pos = nextPlayPos.load();

    return { (int) (jlimit (bufferValidStart, bufferValidEnd, pos) - pos),
             (int) (jlimit (bufferValidStart, bufferValidEnd, pos + numSamples) - pos) };
}

bool BufferingAudioSource::readNextBufferChunk()
{
    int64 newBVS, newBVE, sectionToReadStart, sectionToReadEnd;

    {
        const ScopedLock sl (bufferRangeLock);

        if (wasSourceLooping != isLooping())
        {
            wasSourceLooping = isLooping();
            bufferValidStart = 0;
            bufferValidEnd = 0;
        }

        newBVS = jmax ((int64) 0, nextPlayPos.load());
        newBVE = newBVS + buffer.getNumSamples() - 4;
        sectionToReadStart = 0;
        sectionToReadEnd = 0;

        constexpr int maxChunkSize = 2048;

        if (newBVS < bufferValidStart || newBVS >= bufferValidEnd)
        {
            newBVE = jmin (newBVE, newBVS + maxChunkSize);

            sectionToReadStart = newBVS;
            sectionToReadEnd = newBVE;

            bufferValidStart = 0;
            bufferValidEnd = 0;
        }
        else if (std::abs ((int) (newBVS - bufferValidStart)) > 512
                  || std::abs ((int) (newBVE - bufferValidEnd)) > 512)
        {
            newBVE = jmin (newBVE, bufferValidEnd + maxChunkSize);

            sectionToReadStart = bufferValidEnd;
            sectionToReadEnd = newBVE;

            bufferValidStart = newBVS;
            bufferValidEnd = jmin (bufferValidEnd, newBVE);
        }
    }

    if (sectionToReadStart == sectionToReadEnd)
        return false;

    jassert (buffer.getNumSamples() > 0);

    const auto bufferIndexStart = (int) (sectionToReadStart % buffer.getNumSamples());
    const auto bufferIndexEnd   = (int) (sectionToReadEnd   % buffer.getNumSamples());

    if (bufferIndexStart < bufferIndexEnd)
    {
        readBufferSection (sectionToReadStart,
                           (int) (sectionToReadEnd - sectionToReadStart),
                           bufferIndexStart);
    }
    else
    {
        const auto initialSize = buffer.getNumSamples() - bufferIndexStart;

        readBufferSection (sectionToReadStart,
                           initialSize,
                           bufferIndexStart);

        readBufferSection (sectionToReadStart + initialSize,
                           (int) (sectionToReadEnd - sectionToReadStart) - initialSize,
                           0);
    }

    {
        const ScopedLock sl2 (bufferRangeLock);

        // A seek (or a consume) may have moved the cursor while the underlying
        // source was being decoded. The PCM just read belongs to the old
        // coordinates, so it must never be advertised as data for the new
        // position; discard it and let the next time slice re-plan.
        if (nextPlayPos.load() != newBVS)
        {
            bufferReadyEvent.signal();
            return true;
        }

        bufferValidStart = newBVS;
        bufferValidEnd = newBVE;
    }

    bufferReadyEvent.signal();
    return true;
}

void BufferingAudioSource::readBufferSection (int64 start, int length, int bufferOffset)
{
    if (source->getNextReadPosition() != start)
        source->setNextReadPosition (start);

    AudioSourceChannelInfo info (&buffer, bufferOffset, length);

    const ScopedLock sl (callbackLock);
    source->getNextAudioBlock (info);
}

int BufferingAudioSource::useTimeSlice()
{
    return readNextBufferChunk() ? 1 : 100;
}

//==============================================================================
bool BufferingAudioSource::ticketMatchesCurrentPosition (const ReadTicket& ticket) const noexcept
{
    return ticket.valid
            && ticket.owner == this
            && ticket.epoch == ticketEpoch.load()
            && ticket.startPosition == nextPlayPos.load();
}

bool BufferingAudioSource::peekNextBlock (const AudioSourceChannelInfo& info, ReadTicket& ticket)
{
    // Rejecting must always leave the incoming ticket invalid.
    ticket.reset();

    if (! isPrepared || info.buffer == nullptr || info.numSamples <= 0
        || info.startSample < 0
        || info.startSample + info.numSamples > info.buffer->getNumSamples())
    {
        return false;
    }

    const auto numSamples = info.numSamples;
    const auto ringSize = buffer.getNumSamples();

    // Never wait for the background thread: if the PCM ring is currently being
    // written, fail immediately and let the caller retry.
    const ScopedTryLock callbackLocker (callbackLock);

    if (! callbackLocker.isLocked())
        return false;

    int64 startPosition;
    bool fullyReady = false;

    {
        const ScopedLock rangeLocker (bufferRangeLock);

        startPosition = nextPlayPos.load();

        fullyReady = startPosition >= bufferValidStart
                      && startPosition + numSamples <= bufferValidEnd;
    }

    if (! fullyReady)
        return false;

    const auto channelsToCopy = jmin (numberOfChannels, info.buffer->getNumChannels());

    if (channelsToCopy <= 0)
        return false;

    for (int chan = 0; chan < channelsToCopy; ++chan)
    {
        const auto startBufferIndex = (int) (startPosition % ringSize);
        const auto endBufferIndex   = startBufferIndex + numSamples;

        if (endBufferIndex <= ringSize)
        {
            info.buffer->copyFrom (chan, info.startSample,
                                   buffer, chan, startBufferIndex, numSamples);
        }
        else
        {
            const auto initialSize = ringSize - startBufferIndex;

            info.buffer->copyFrom (chan, info.startSample,
                                   buffer, chan, startBufferIndex, initialSize);

            info.buffer->copyFrom (chan, info.startSample + initialSize,
                                   buffer, chan, 0,
                                   numSamples - initialSize);
        }
    }

    // The samples are now private to the caller; only the metadata is published.
    {
        const ScopedLock ticketLocker (ticketLock);

        ticket.owner = this;
        ticket.startPosition = startPosition;
        ticket.length = numSamples;
        ticket.epoch = ticketEpoch.load();
        ticket.valid = true;
    }

    return true;
}

bool BufferingAudioSource::commitReadTickets (ReadTicket* const* tickets, int count)
{
    struct Item
    {
        BufferingAudioSource* owner;
        ReadTicket* ticket;
    };

    if (tickets == nullptr || count <= 0 || count > maxCommitTickets)
        return false;

    std::array<Item, maxCommitTickets> items {};
    int numItems = 0;

    // Phase 1: validate everything without touching a cursor or a lock.
    for (int i = 0; i < count; ++i)
    {
        ReadTicket* const ticket = tickets[i];

        if (ticket == nullptr || ! ticket->isValid() || ticket->owner == nullptr)
            return false;

        BufferingAudioSource* const owner = ticket->owner;

        for (int j = 0; j < numItems; ++j)
        {
            if (items[j].owner == owner)
                return false; // a source may appear at most once in a group
        }

        items[numItems++] = { owner, ticket };
    }

    // Deterministic order means two groups that name the same sources in
    // opposite orders acquire the same locks and can never deadlock.
    std::sort (items.begin(), items.begin() + numItems,
               [] (const Item& a, const Item& b) { return a.owner < b.owner; });

    // Phase 2: acquire every source's locks first. The order bufferRangeLock
    // then ticketLock matches setNextReadPosition(), so seek and commit can
    // never invert each other. No underlying source is read and no thread is
    // waited on while these locks are held.
    std::array<CriticalSection*, maxCommitTickets * 2> heldLocks {};
    int locksHeld = 0;

    auto releaseLocks = [&]
    {
        for (int i = locksHeld - 1; i >= 0; --i)
            heldLocks[i]->exit();
    };

    for (int i = 0; i < numItems; ++i)
    {
        auto& rangeLock = items[i].owner->bufferRangeLock;
        rangeLock.enter();
        heldLocks[locksHeld++] = &rangeLock;

        auto& epochLock = items[i].owner->ticketLock;
        epochLock.enter();
        heldLocks[locksHeld++] = &epochLock;
    }

    bool success = true;

    for (int i = 0; i < numItems; ++i)
    {
        if (! items[i].owner->ticketMatchesCurrentPosition (*items[i].ticket))
        {
            success = false;
            break;
        }
    }

    if (success)
    {
        // Phase 3: the single commit point - all cursors advance together and
        // all the committed tickets become invalid.
        for (int i = 0; i < numItems; ++i)
        {
            BufferingAudioSource* const owner = items[i].owner;
            ReadTicket* const ticket = items[i].ticket;

            owner->nextPlayPos = ticket->startPosition + ticket->length;
            ++owner->ticketEpoch;
            ticket->reset();
        }
    }

    releaseLocks();

    return success;
}

} // namespace juce
