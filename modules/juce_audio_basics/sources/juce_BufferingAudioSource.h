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

namespace juce
{

//==============================================================================
/**
    An AudioSource which takes another source as input, and buffers it using a thread.

    Create this as a wrapper around another thread, and it will read-ahead with
    a background thread to smooth out playback. You can either create one of these
    directly, or use it indirectly using an AudioTransportSource.

    @see PositionableAudioSource, AudioTransportSource

    @tags{Audio}
*/
class JUCE_API  BufferingAudioSource  : public PositionableAudioSource,
                                        private TimeSliceClient
{
public:
    //==============================================================================
    /**
        A non-owning claim on a block that has been peeked from a BufferingAudioSource.

        A ticket is produced by peekNextBlock() and describes one complete block
        starting at the source's current read position. It does not consume any
        audio: the source's read position only moves when the ticket is handed to
        BufferingAudioSource::commitReadTickets().

        Tickets are single-use and cannot be copied. A ticket becomes stale as soon
        as its source is consumed, repositioned, released or re-prepared, even when
        the source is moved back to the position the ticket was originally issued
        for. Background read-ahead filling the buffer never invalidates a ticket.
    */
    class JUCE_API  ReadTicket
    {
    public:
        /** Creates an empty, invalid ticket. */
        ReadTicket() = default;

        /** Destructor. */
        ~ReadTicket() = default;

        ReadTicket (const ReadTicket&) = delete;
        ReadTicket& operator= (const ReadTicket&) = delete;

        /** Move constructor; the ticket moved from becomes invalid. */
        ReadTicket (ReadTicket&& other) noexcept            { takeFrom (other); }

        /** Move assignment; the ticket moved from becomes invalid. */
        ReadTicket& operator= (ReadTicket&& other) noexcept
        {
            if (this != &other)
                takeFrom (other);

            return *this;
        }

        /** Returns the source read position that the block starts at. */
        int64 getStartPosition() const noexcept             { return startPosition; }

        /** Returns true while this ticket could still be committed.

            Besides the ticket's own state, this live-checks the owning source:
            a consume, seek, release or re-prepare bumps the source's
            generation (or moves the cursor), making the ticket invalid even if
            the cursor later returns to the original position. Filling the
            read-ahead buffer changes neither, so background reads never
            invalidate a ticket here.
        */
        bool isValid() const noexcept
        {
            return valid
                    && owner != nullptr
                    && epoch == owner->ticketEpoch.load()
                    && startPosition == owner->nextPlayPos.load();
        }

    private:
        friend class BufferingAudioSource;

        BufferingAudioSource* owner = nullptr;
        int64 startPosition = 0;
        int length = 0;
        uint64 epoch = 0;
        bool valid = false;

        void reset() noexcept
        {
            owner = nullptr;
            startPosition = 0;
            length = 0;
            epoch = 0;
            valid = false;
        }

        void takeFrom (ReadTicket& other) noexcept
        {
            owner = other.owner;
            startPosition = other.startPosition;
            length = other.length;
            epoch = other.epoch;
            valid = other.valid;
            other.reset();
        }
    };

    //==============================================================================
    /** Creates a BufferingAudioSource.

        @param source                       the input source to read from
        @param backgroundThread             a background thread that will be used for the
                                            background read-ahead. This object must not be deleted
                                            until after any BufferingAudioSources that are using it
                                            have been deleted!
        @param deleteSourceWhenDeleted      if true, then the input source object will
                                            be deleted when this object is deleted
        @param numberOfSamplesToBuffer      the size of buffer to use for reading ahead
        @param numberOfChannels             the number of channels that will be played
        @param prefillBufferOnPrepareToPlay if true, then calling prepareToPlay on this object will
                                            block until the buffer has been filled
    */
    BufferingAudioSource (PositionableAudioSource* source,
                          TimeSliceThread& backgroundThread,
                          bool deleteSourceWhenDeleted,
                          int numberOfSamplesToBuffer,
                          int numberOfChannels = 2,
                          bool prefillBufferOnPrepareToPlay = true);

    /** Destructor.

        The input source may be deleted depending on whether the deleteSourceWhenDeleted
        flag was set in the constructor.
    */
    ~BufferingAudioSource() override;

    //==============================================================================
    /** Implementation of the AudioSource method. */
    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override;

    /** Implementation of the AudioSource method. */
    void releaseResources() override;

    /** Implementation of the AudioSource method. */
    void getNextAudioBlock (const AudioSourceChannelInfo&) override;

    //==============================================================================
    /** Implements the PositionableAudioSource method. */
    void setNextReadPosition (int64 newPosition) override;

    /** Implements the PositionableAudioSource method. */
    int64 getNextReadPosition() const override;

    /** Implements the PositionableAudioSource method. */
    int64 getTotalLength() const override       { return source->getTotalLength(); }

    /** Implements the PositionableAudioSource method. */
    bool isLooping() const override             { return source->isLooping(); }

    /** Returns the number of channels this source was created with. */
    int getNumChannels() const noexcept         { return numberOfChannels; }

    /** A useful function to block until the next the buffer info can be filled.

        This is useful for offline rendering.
    */
    bool waitForNextAudioBlockReady (const AudioSourceChannelInfo& info, uint32 timeout);

    //==============================================================================
    /** Copies the complete block starting at the current read position into the
        caller's buffer without consuming it.

        When the whole block is already available in the read-ahead ring it is
        copied out under the same lock that guards background writes, and a valid
        ticket describing the block is issued. The method never blocks on a slow
        background decode: if the block is not fully ready, or the background
        thread is currently occupying the PCM buffer, it returns false immediately.

        On failure the caller's buffer is left untouched and the supplied ticket
        becomes invalid. Zero-length or negative requests, out-of-bounds buffer
        regions and calls before prepareToPlay() are rejected the same way.

        The PCM stored in the caller's buffer stays valid independently of later
        ring overwrite, because the samples are copied out at peek time.
    */
    bool peekNextBlock (const AudioSourceChannelInfo& info, ReadTicket& ticket);

    /** Atomically commits between 1 and 8 tickets.

        The read positions of all the named sources only move when every ticket
        still matches its source's current read position and generation. On
        success all the sources advance and all the tickets become invalid. On
        failure nothing moves and the tickets are left untouched. Empty tickets,
        repeated sources, stale tickets and an illegal count all fail the whole
        group.

        The call is a single commit point with respect to peeks, seeks, releases,
        re-prepares and the background thread. Groups naming the same sources in
        opposite orders cannot deadlock or partially consume.
    */
    static bool commitReadTickets (ReadTicket* const* tickets, int count);

    /** The maximum number of tickets that can be committed in one group. */
    static constexpr int maxCommitTickets = 8;

private:
    //==============================================================================
    Range<int> getValidBufferRange (int numSamples) const;
    bool readNextBufferChunk();
    void readBufferSection (int64 start, int length, int bufferOffset);
    int useTimeSlice() override;

    bool ticketMatchesCurrentPosition (const ReadTicket&) const noexcept;

    //==============================================================================
    OptionalScopedPointer<PositionableAudioSource> source;
    TimeSliceThread& backgroundThread;
    int numberOfSamplesToBuffer, numberOfChannels;
    AudioBuffer<float> buffer;
    CriticalSection callbackLock, bufferRangeLock, ticketLock;
    WaitableEvent bufferReadyEvent;
    int64 bufferValidStart = 0, bufferValidEnd = 0;
    std::atomic<int64> nextPlayPos { 0 };
    std::atomic<uint64> ticketEpoch { 0 };
    double sampleRate = 0;
    bool wasSourceLooping = false, isPrepared = false;
    const bool prefillBuffer;

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BufferingAudioSource)
};

} // namespace juce
