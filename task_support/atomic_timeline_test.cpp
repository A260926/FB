/*
    Deterministic regression tests for atomic multi-track buffered consumption.

    Three groups are registered through task_cases/task_case_count and driven
    by task_support/driver.c:
      - tickets:  ReadTicket lifecycle, rejection rules and ring wraparound
      - atomic:   group commit atomicity, deadlock freedom and stale-read rules
      - timeline: BufferedAudioTimeline mixing, seek/retime and failure modes

    Every interleaving is pinned down with WaitableEvents or atomics; no timing
    guesses are used to order threads.
*/

#include "task_support/check.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
    using juce::int64;

    // Shared deterministic, aperiodic reference signal. Every test source uses
    // it, so expected PCM can be recomputed independently of the renderer.
    float testSample (int id, int64 n, int channel)
    {
        const auto phase = (double) n * 0.031 + id * 0.23 + channel * 0.41;
        return float (0.11 * std::sin (phase) + 0.001 * (n % 97) + 0.025 * id
                      + 0.05 * std::sin ((double) n * 0.0071 + id));
    }

    bool closeFloat (float a, float b)
    {
        return std::fabs (a - b) < 1e-6;
    }

    // AudioBuffer::clear() only writes zero, so fills use this helper.
    void fillBuffer (juce::AudioBuffer<float>& buffer, float value)
    {
        for (int c = 0; c < buffer.getNumChannels(); ++c)
            for (int n = 0; n < buffer.getNumSamples(); ++n)
                buffer.setSample (c, n, value);
    }

    // A finite, non-looping, positionable float source with 1 or 2 channels.
    struct TestSource : juce::PositionableAudioSource
    {
        int64 position = 0;
        const int id;
        const int channelCount;
        const int64 sourceLength;

        TestSource (int id_, int channels, int64 length = 20000)
            : id (id_), channelCount (channels), sourceLength (length) {}

        static float sample (int id, int64 n, int channel) { return testSample (id, n, channel); }

        void prepareToPlay (int, double) override { position = 0; }
        void releaseResources() override {}

        void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
        {
            for (int c = 0; c < info.buffer->getNumChannels(); ++c)
                for (int n = 0; n < info.numSamples; ++n)
                    info.buffer->setSample (c, info.startSample + n, testSample (id, position + n, c));

            position += info.numSamples;
        }

        void setNextReadPosition (int64 p) override { position = p; }
        int64 getNextReadPosition() const override { return position; }
        int64 getTotalLength() const override { return sourceLength; }
        bool isLooping() const override { return false; }
    };

    // A source whose background reads can be frozen mid-decode. A read whose
    // starting position is at or below an armed threshold blocks on a gate and
    // announces itself through an event, pinning the interleaving down exactly
    // without timing guesses. While blocked the BufferingAudioSource's PCM
    // lock is held.
    struct BlockableSource : juce::PositionableAudioSource
    {
        int64 position = 0;
        const int id;
        const int channelCount;
        const int64 sourceLength;

        juce::WaitableEvent entered { false }; // auto-reset, signalled by a blocked read
        juce::WaitableEvent gate { true };     // manual-reset, opened in the constructor
        std::atomic<int64> blockAtPosition { std::numeric_limits<int64>::max() };
        std::atomic<int> activeReads { 0 };
        std::atomic<int> totalReads { 0 };

        BlockableSource (int id_, int channels, int64 length = 20000)
            : id (id_), channelCount (channels), sourceLength (length)
        {
            // WaitableEvent has no initial-state argument; open the gate
            // explicitly so ordinary reads run until a block is armed.
            gate.signal();
        }

        // Reads starting at or beyond this position block until unblock().
        void armBlock (int64 threshold)
        {
            blockAtPosition.store (threshold);
            gate.reset();
        }

        void unblock()
        {
            blockAtPosition.store (std::numeric_limits<int64>::max());
            gate.signal();
        }

        void prepareToPlay (int, double) override { position = 0; }
        void releaseResources() override {}

        void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
        {
            activeReads.fetch_add (1);

            if (position >= blockAtPosition.load())
            {
                entered.signal();
                gate.wait (-1.0);
            }

            for (int c = 0; c < info.buffer->getNumChannels(); ++c)
                for (int n = 0; n < info.numSamples; ++n)
                    info.buffer->setSample (c, info.startSample + n, testSample (id, position + n, c));

            position += info.numSamples;
            activeReads.fetch_sub (1);
            totalReads.fetch_add (1);
        }

        void setNextReadPosition (int64 p) override { position = p; }
        int64 getNextReadPosition() const override { return position; }
        int64 getTotalLength() const override { return sourceLength; }
        bool isLooping() const override { return false; }
    };

    constexpr int blockSize = 64;
    constexpr double sampleRate = 48000.0;

    struct Worker
    {
        juce::TimeSliceThread thread { "atomic-test-prefetch" };

        Worker() { thread.startThread(); }
        ~Worker() { thread.stopThread (5000); }
    };

    juce::BufferingAudioSource makeBuffer (Worker& w, juce::PositionableAudioSource* s, int channels = 2)
    {
        return juce::BufferingAudioSource (s, w.thread, false, 4096, channels, false);
    }

    bool waitUntilPeeked (juce::BufferingAudioSource& source,
                         juce::AudioSourceChannelInfo& info,
                         juce::BufferingAudioSource::ReadTicket& ticket,
                         int attempts = 10000)
    {
        for (int i = 0; i < attempts; ++i)
        {
            source.waitForNextAudioBlockReady (info, 1000);

            if (source.peekNextBlock (info, ticket))
                return true;

            juce::Thread::sleep (1);
        }

        return false;
    }

    bool waitUntilRendered (juce::BufferedAudioTimeline& timeline,
                           const juce::AudioSourceChannelInfo& info,
                           int attempts = 20000)
    {
        for (int i = 0; i < attempts; ++i)
        {
            if (timeline.tryRender (info))
                return true;

            juce::Thread::sleep (1);
        }

        return false;
    }

    //==============================================================================
    // tickets group
    //==============================================================================

    int tickets_lifecycle()
    {
        Worker w;
        TestSource src (1, 2);
        auto bas = makeBuffer (w, &src);
        bas.prepareToPlay (blockSize, sampleRate);

        juce::AudioBuffer<float> dst (2, blockSize);
        juce::AudioSourceChannelInfo info (&dst, 0, blockSize);
        juce::BufferingAudioSource::ReadTicket ticket;

        TASK_CHECK (! ticket.isValid());
        TASK_CHECK (waitUntilPeeked (bas, info, ticket));
        TASK_CHECK (ticket.isValid());
        TASK_CHECK (ticket.getStartPosition() == 0);
        TASK_CHECK (bas.getNextReadPosition() == 0); // peek does not consume

        for (int c = 0; c < 2; ++c)
            for (int n = 0; n < blockSize; ++n)
                TASK_CHECK (closeFloat (dst.getSample (c, n), testSample (1, n, c)));

        juce::BufferingAudioSource::ReadTicket* handles[] = { &ticket };
        TASK_CHECK (juce::BufferingAudioSource::commitReadTickets (handles, 1));
        TASK_CHECK (! ticket.isValid());
        TASK_CHECK (bas.getNextReadPosition() == blockSize);

        // A consumed ticket cannot be committed again.
        TASK_CHECK (! juce::BufferingAudioSource::commitReadTickets (handles, 1));
        TASK_CHECK (bas.getNextReadPosition() == blockSize);

        bas.releaseResources();
        return 0;
    }

    int tickets_rejections()
    {
        Worker w;
        TestSource src (1, 2);
        auto bas = makeBuffer (w, &src);

        juce::AudioBuffer<float> dst (2, blockSize);
        juce::BufferingAudioSource::ReadTicket ticket;
        juce::AudioSourceChannelInfo good (&dst, 0, blockSize);

        // Before prepare.
        TASK_CHECK (! bas.peekNextBlock (good, ticket));
        TASK_CHECK (! ticket.isValid());

        bas.prepareToPlay (blockSize, sampleRate);

        // Zero and negative lengths.
        juce::AudioSourceChannelInfo zero (&dst, 0, 0);
        TASK_CHECK (! bas.peekNextBlock (zero, ticket));
        TASK_CHECK (! ticket.isValid());

        juce::AudioSourceChannelInfo negative (&dst, 0, -4);
        TASK_CHECK (! bas.peekNextBlock (negative, ticket));
        TASK_CHECK (! ticket.isValid());

        // Out-of-bounds region.
        juce::AudioSourceChannelInfo oob (&dst, 10, blockSize);
        TASK_CHECK (! bas.peekNextBlock (oob, ticket));
        TASK_CHECK (! ticket.isValid());

        // Null buffer.
        juce::AudioSourceChannelInfo nullInfo (nullptr, 0, blockSize);
        TASK_CHECK (! bas.peekNextBlock (nullInfo, ticket));
        TASK_CHECK (! ticket.isValid());

        // A rejected peek never touches the destination.
        fillBuffer (dst, 7.0f);
        TASK_CHECK (! bas.peekNextBlock (zero, ticket));
        for (int c = 0; c < 2; ++c)
            for (int n = 0; n < blockSize; ++n)
                TASK_CHECK (closeFloat (dst.getSample (c, n), 7.0f));

        TASK_CHECK (waitUntilPeeked (bas, good, ticket));
        TASK_CHECK (ticket.getStartPosition() == 0);

        bas.releaseResources();
        return 0;
    }

    int tickets_blocked_background()
    {
        Worker w;
        BlockableSource src (4, 2);
        src.armBlock (0); // freeze the first background decode, planned at position 0

        auto bas = makeBuffer (w, &src);
        bas.prepareToPlay (blockSize, sampleRate);

        // The worker is now stuck inside the read, holding the PCM lock.
        TASK_CHECK (src.entered.wait (5000));
        TASK_CHECK (src.activeReads.load() == 1);

        juce::AudioBuffer<float> dst (2, blockSize);
        fillBuffer (dst, 7.0f);
        juce::AudioSourceChannelInfo info (&dst, 0, blockSize);
        juce::BufferingAudioSource::ReadTicket ticket;

        // peek must fail immediately rather than wait on the slow decode.
        TASK_CHECK (! bas.peekNextBlock (info, ticket));
        TASK_CHECK (! ticket.isValid());
        for (int c = 0; c < 2; ++c)
            for (int n = 0; n < blockSize; ++n)
                TASK_CHECK (closeFloat (dst.getSample (c, n), 7.0f));

        src.unblock();
        TASK_CHECK (waitUntilPeeked (bas, info, ticket));
        TASK_CHECK (ticket.getStartPosition() == 0);
        for (int c = 0; c < 2; ++c)
            for (int n = 0; n < blockSize; ++n)
                TASK_CHECK (closeFloat (dst.getSample (c, n), testSample (4, n, c)));

        bas.releaseResources();
        return 0;
    }

    int tickets_seek_invalidation()
    {
        Worker w;
        TestSource src (1, 2);
        auto bas = makeBuffer (w, &src);
        bas.prepareToPlay (blockSize, sampleRate);

        juce::AudioBuffer<float> dst (2, blockSize);
        juce::AudioSourceChannelInfo info (&dst, 0, blockSize);
        juce::BufferingAudioSource::ReadTicket ticket;
        TASK_CHECK (waitUntilPeeked (bas, info, ticket));

        // A seek, even back to the same position, invalidates the ticket.
        bas.setNextReadPosition (0);
        TASK_CHECK (! ticket.isValid());
        juce::BufferingAudioSource::ReadTicket* handles[] = { &ticket };
        TASK_CHECK (! juce::BufferingAudioSource::commitReadTickets (handles, 1));
        TASK_CHECK (bas.getNextReadPosition() == 0);

        // Seeking away and back again must not revive it.
        TASK_CHECK (waitUntilPeeked (bas, info, ticket));
        bas.setNextReadPosition (128);
        bas.setNextReadPosition (0);
        TASK_CHECK (! juce::BufferingAudioSource::commitReadTickets (handles, 1));
        TASK_CHECK (bas.getNextReadPosition() == 0);

        // A fresh ticket at the same position works normally.
        TASK_CHECK (waitUntilPeeked (bas, info, ticket));
        TASK_CHECK (ticket.getStartPosition() == 0);
        TASK_CHECK (juce::BufferingAudioSource::commitReadTickets (handles, 1));
        TASK_CHECK (bas.getNextReadPosition() == blockSize);

        bas.releaseResources();
        return 0;
    }

    int tickets_release_and_reprepare()
    {
        Worker w;
        TestSource src (1, 2);
        auto bas = makeBuffer (w, &src);
        bas.prepareToPlay (blockSize, sampleRate);

        juce::AudioBuffer<float> dst (2, blockSize);
        juce::AudioSourceChannelInfo info (&dst, 0, blockSize);
        juce::BufferingAudioSource::ReadTicket ticket;
        TASK_CHECK (waitUntilPeeked (bas, info, ticket));

        bas.releaseResources();
        TASK_CHECK (! ticket.isValid());
        juce::BufferingAudioSource::ReadTicket* handles[] = { &ticket };
        TASK_CHECK (! juce::BufferingAudioSource::commitReadTickets (handles, 1));

        // Re-prepare starts a fresh generation; old tickets stay dead.
        bas.prepareToPlay (blockSize, sampleRate);
        TASK_CHECK (! ticket.isValid());
        TASK_CHECK (! juce::BufferingAudioSource::commitReadTickets (handles, 1));
        TASK_CHECK (bas.getNextReadPosition() == 0);

        bas.releaseResources();
        return 0;
    }

    int tickets_normal_consume_invalidates()
    {
        Worker w;
        TestSource src (1, 2);
        auto bas = makeBuffer (w, &src);
        bas.prepareToPlay (blockSize, sampleRate);

        juce::AudioBuffer<float> dst (2, blockSize);
        juce::AudioSourceChannelInfo info (&dst, 0, blockSize);
        juce::BufferingAudioSource::ReadTicket ticket;
        TASK_CHECK (waitUntilPeeked (bas, info, ticket));

        // The legacy single-source consume follows the same generation rules.
        bas.getNextAudioBlock (info);
        TASK_CHECK (! ticket.isValid());
        juce::BufferingAudioSource::ReadTicket* handles[] = { &ticket };
        TASK_CHECK (! juce::BufferingAudioSource::commitReadTickets (handles, 1));
        TASK_CHECK (bas.getNextReadPosition() == blockSize);

        bas.releaseResources();
        return 0;
    }

    int tickets_commit_validation()
    {
        Worker w;
        TestSource sa (1, 2), sb (2, 2);
        auto a = makeBuffer (w, &sa);
        auto b = makeBuffer (w, &sb);
        a.prepareToPlay (blockSize, sampleRate);
        b.prepareToPlay (blockSize, sampleRate);

        juce::AudioBuffer<float> da (2, blockSize), db (2, blockSize);
        juce::AudioSourceChannelInfo ia (&da, 0, blockSize), ib (&db, 0, blockSize);
        juce::BufferingAudioSource::ReadTicket ta, tb;
        TASK_CHECK (waitUntilPeeked (a, ia, ta));
        TASK_CHECK (waitUntilPeeked (b, ib, tb));

        // Illegal counts.
        TASK_CHECK (! juce::BufferingAudioSource::commitReadTickets (nullptr, 1));
        juce::BufferingAudioSource::ReadTicket* good[] = { &ta, &tb };
        TASK_CHECK (! juce::BufferingAudioSource::commitReadTickets (good, 0));
        TASK_CHECK (! juce::BufferingAudioSource::commitReadTickets (good, -1));
        TASK_CHECK (! juce::BufferingAudioSource::commitReadTickets (good, 9));

        // Null ticket in the group.
        juce::BufferingAudioSource::ReadTicket* withNull[] = { &ta, nullptr };
        TASK_CHECK (! juce::BufferingAudioSource::commitReadTickets (withNull, 2));

        // Empty ticket in the group.
        juce::BufferingAudioSource::ReadTicket empty;
        juce::BufferingAudioSource::ReadTicket* withEmpty[] = { &ta, &empty };
        TASK_CHECK (! juce::BufferingAudioSource::commitReadTickets (withEmpty, 2));

        // Duplicate source.
        juce::BufferingAudioSource::ReadTicket ta2;
        TASK_CHECK (waitUntilPeeked (a, ia, ta2));
        juce::BufferingAudioSource::ReadTicket* duplicated[] = { &ta, &ta2 };
        TASK_CHECK (! juce::BufferingAudioSource::commitReadTickets (duplicated, 2));

        // Nothing moved after any of the rejected groups.
        TASK_CHECK (a.getNextReadPosition() == 0);
        TASK_CHECK (b.getNextReadPosition() == 0);

        // Stale ticket in an otherwise valid group fails the whole group.
        b.setNextReadPosition (32);
        TASK_CHECK (! tb.isValid());
        TASK_CHECK (waitUntilPeeked (a, ia, ta));
        juce::BufferingAudioSource::ReadTicket* mixed[] = { &ta, &tb };
        TASK_CHECK (! juce::BufferingAudioSource::commitReadTickets (mixed, 2));
        TASK_CHECK (a.getNextReadPosition() == 0);
        TASK_CHECK (b.getNextReadPosition() == 32);

        a.releaseResources();
        b.releaseResources();
        return 0;
    }

    int tickets_ring_wraparound()
    {
        Worker w;
        TestSource src (1, 2);
        // Ring size in prepare is max(block*2, 4096) == 4096 frames.
        auto bas = makeBuffer (w, &src);
        bas.prepareToPlay (blockSize, sampleRate);

        juce::AudioBuffer<float> dst (2, blockSize);
        juce::AudioSourceChannelInfo info (&dst, 0, blockSize);
        juce::BufferingAudioSource::ReadTicket ticket;
        juce::BufferingAudioSource::ReadTicket* handles[] = { &ticket };

        // Consume whole blocks up to 4032.
        for (int64 pos = 0; pos < 4032; pos += blockSize)
        {
            TASK_CHECK (waitUntilPeeked (bas, info, ticket));
            TASK_CHECK (ticket.getStartPosition() == pos);
            TASK_CHECK (juce::BufferingAudioSource::commitReadTickets (handles, 1));
        }

        TASK_CHECK (bas.getNextReadPosition() == 4032);

        // Straddle the ring boundary at frame 4096: 4048..4112 wraps the ring.
        bas.setNextReadPosition (4048);
        TASK_CHECK (waitUntilPeeked (bas, info, ticket));
        TASK_CHECK (ticket.getStartPosition() == 4048);

        for (int c = 0; c < 2; ++c)
            for (int n = 0; n < blockSize; ++n)
                TASK_CHECK (closeFloat (dst.getSample (c, n), testSample (1, 4048 + n, c)));

        TASK_CHECK (juce::BufferingAudioSource::commitReadTickets (handles, 1));
        TASK_CHECK (bas.getNextReadPosition() == 4048 + blockSize);

        bas.releaseResources();
        return 0;
    }

    int tickets_failure_retry()
    {
        Worker w;
        BlockableSource src (5, 2);
        auto bas = makeBuffer (w, &src);
        bas.prepareToPlay (blockSize, sampleRate);

        juce::AudioBuffer<float> dst (2, blockSize);
        juce::AudioSourceChannelInfo info (&dst, 0, blockSize);
        juce::BufferingAudioSource::ReadTicket ticket;

        // Seek well beyond the already-prefetched ring window and freeze that
        // region's decode.
        src.armBlock (8000);
        bas.setNextReadPosition (8000);
        TASK_CHECK (src.entered.wait (5000));
        TASK_CHECK (! bas.peekNextBlock (info, ticket));
        TASK_CHECK (bas.getNextReadPosition() == 8000);

        src.unblock();
        TASK_CHECK (waitUntilPeeked (bas, info, ticket));
        TASK_CHECK (ticket.getStartPosition() == 8000);
        TASK_CHECK (bas.getNextReadPosition() == 8000);
        for (int c = 0; c < 2; ++c)
            for (int n = 0; n < blockSize; ++n)
                TASK_CHECK (closeFloat (dst.getSample (c, n), testSample (5, 8000 + n, c)));

        bas.releaseResources();
        return 0;
    }

    //==============================================================================
    // atomic group
    //==============================================================================

    int atomic_reverse_order_groups()
    {
        Worker w;
        TestSource sa (1, 2), sb (2, 2);
        auto a = makeBuffer (w, &sa);
        auto b = makeBuffer (w, &sb);
        a.prepareToPlay (blockSize, sampleRate);
        b.prepareToPlay (blockSize, sampleRate);

        juce::WaitableEvent startGate { true }; // manual reset
        startGate.reset();

        struct Counters { std::atomic<int> ok { 0 }; std::atomic<int> attempts { 0 }; };
        Counters c1, c2;

        auto workerLoop = [&] (Counters& counters, bool reverse)
        {
            startGate.wait (-1.0);

            juce::AudioBuffer<float> da (2, blockSize), db (2, blockSize);
            juce::AudioSourceChannelInfo ia (&da, 0, blockSize), ib (&db, 0, blockSize);
            juce::BufferingAudioSource::ReadTicket ta, tb;

            while (counters.ok.load() < 100 && counters.attempts.load() < 100000)
            {
                counters.attempts.fetch_add (1);

                if (! waitUntilPeeked (a, ia, ta, 1000) || ! waitUntilPeeked (b, ib, tb, 1000))
                    continue;

                juce::BufferingAudioSource::ReadTicket* forward[] = { &ta, &tb };
                juce::BufferingAudioSource::ReadTicket* reversed[] = { &tb, &ta };
                juce::BufferingAudioSource::ReadTicket** handles = reverse ? reversed : forward;

                if (juce::BufferingAudioSource::commitReadTickets (handles, 2))
                    counters.ok.fetch_add (1);
            }
        };

        std::thread t1 (workerLoop, std::ref (c1), false);
        std::thread t2 (workerLoop, std::ref (c2), true);

        startGate.signal();
        t1.join();
        t2.join();

        TASK_CHECK (c1.ok.load() == 100);
        TASK_CHECK (c2.ok.load() == 100);

        // Every successful commit advanced both sources by exactly one block,
        // so the cursors stay lock-step and never partially consume.
        const auto expected = (c1.ok.load() + c2.ok.load()) * blockSize;
        TASK_CHECK (a.getNextReadPosition() == expected);
        TASK_CHECK (b.getNextReadPosition() == expected);

        a.releaseResources();
        b.releaseResources();
        return 0;
    }

    int atomic_commit_while_seek()
    {
        Worker w;
        TestSource src (1, 2);
        auto bas = makeBuffer (w, &src);
        bas.prepareToPlay (blockSize, sampleRate);

        juce::AudioBuffer<float> dst (2, blockSize);
        juce::AudioSourceChannelInfo info (&dst, 0, blockSize);
        juce::BufferingAudioSource::ReadTicket ticket;
        TASK_CHECK (waitUntilPeeked (bas, info, ticket));

        // Pin the ordering: the seek fully completes before the commit runs.
        juce::WaitableEvent go { false }, done { false };
        std::thread seeker ([&]
        {
            go.wait (-1.0);
            bas.setNextReadPosition (bas.getNextReadPosition()); // same spot, new epoch
            done.signal();
        });

        go.signal();
        TASK_CHECK (done.wait (5000));
        seeker.join();

        juce::BufferingAudioSource::ReadTicket* handles[] = { &ticket };
        TASK_CHECK (! juce::BufferingAudioSource::commitReadTickets (handles, 1));
        TASK_CHECK (bas.getNextReadPosition() == 0);
        TASK_CHECK (! ticket.isValid());

        // A fresh ticket at the unchanged position commits normally.
        TASK_CHECK (waitUntilPeeked (bas, info, ticket));
        TASK_CHECK (juce::BufferingAudioSource::commitReadTickets (handles, 1));
        TASK_CHECK (bas.getNextReadPosition() == blockSize);

        bas.releaseResources();
        return 0;
    }

    int atomic_stale_read_after_seek()
    {
        Worker w;
        BlockableSource src (6, 2);
        auto bas = makeBuffer (w, &src);
        bas.prepareToPlay (blockSize, sampleRate);

        // Let the first region fill, then freeze the next background decode.
        juce::AudioBuffer<float> dst (2, blockSize);
        juce::AudioSourceChannelInfo info (&dst, 0, blockSize);
        juce::BufferingAudioSource::ReadTicket ticket;
        TASK_CHECK (waitUntilPeeked (bas, info, ticket));

        // Freeze a decode well beyond the prefetched ring window.
        src.armBlock (8000);
        bas.setNextReadPosition (8000);
        TASK_CHECK (src.entered.wait (5000)); // a decode planned at position 8000

        // While that decode is stuck, move the cursor again. The blocked read
        // now belongs to stale coordinates.
        bas.setNextReadPosition (9000);
        src.unblock();

        // After refill, peeked PCM must be the 9000 data, never stale content.
        TASK_CHECK (waitUntilPeeked (bas, info, ticket));
        TASK_CHECK (ticket.getStartPosition() == 9000);
        for (int c = 0; c < 2; ++c)
            for (int n = 0; n < blockSize; ++n)
                TASK_CHECK (closeFloat (dst.getSample (c, n), testSample (6, 9000 + n, c)));

        bas.releaseResources();
        return 0;
    }

    int atomic_single_source_group()
    {
        Worker w;
        TestSource src (1, 2);
        auto bas = makeBuffer (w, &src);
        bas.prepareToPlay (blockSize, sampleRate);

        juce::AudioBuffer<float> dst (2, blockSize);
        juce::AudioSourceChannelInfo info (&dst, 0, blockSize);
        juce::BufferingAudioSource::ReadTicket ticket;
        TASK_CHECK (waitUntilPeeked (bas, info, ticket));

        juce::BufferingAudioSource::ReadTicket* handles[] = { &ticket };
        TASK_CHECK (juce::BufferingAudioSource::commitReadTickets (handles, 1));
        TASK_CHECK (bas.getNextReadPosition() == blockSize);
        TASK_CHECK (! ticket.isValid());

        bas.releaseResources();
        return 0;
    }

    int atomic_group_pcm_and_positions()
    {
        Worker w;
        TestSource sa (1, 2), sb (2, 2);
        auto a = makeBuffer (w, &sa);
        auto b = makeBuffer (w, &sb);
        a.prepareToPlay (blockSize, sampleRate);
        b.prepareToPlay (blockSize, sampleRate);

        juce::AudioBuffer<float> da (2, blockSize), db (2, blockSize);
        juce::AudioSourceChannelInfo ia (&da, 0, blockSize), ib (&db, 0, blockSize);
        juce::BufferingAudioSource::ReadTicket ta, tb;
        TASK_CHECK (waitUntilPeeked (a, ia, ta));
        TASK_CHECK (waitUntilPeeked (b, ib, tb));

        juce::BufferingAudioSource::ReadTicket* handles[] = { &ta, &tb };
        TASK_CHECK (juce::BufferingAudioSource::commitReadTickets (handles, 2));
        TASK_CHECK (a.getNextReadPosition() == blockSize);
        TASK_CHECK (b.getNextReadPosition() == blockSize);
        TASK_CHECK (! ta.isValid() && ! tb.isValid());

        for (int c = 0; c < 2; ++c)
            for (int n = 0; n < blockSize; ++n)
            {
                TASK_CHECK (closeFloat (da.getSample (c, n), testSample (1, n, c)));
                TASK_CHECK (closeFloat (db.getSample (c, n), testSample (2, n, c)));
            }

        a.releaseResources();
        b.releaseResources();
        return 0;
    }

    //==============================================================================
    // timeline group
    //==============================================================================

    int timeline_construction_rules()
    {
        Worker w;
        std::vector<std::unique_ptr<TestSource>> sources;
        std::vector<std::unique_ptr<juce::BufferingAudioSource>> buffers;

        for (int i = 0; i < 9; ++i)
        {
            sources.push_back (std::make_unique<TestSource> (i + 1, 2));
            buffers.push_back (std::make_unique<juce::BufferingAudioSource>
                                   (sources.back().get(), w.thread, false, 4096, 2, false));
        }

        auto mk = [&] (int count)
        {
            std::vector<juce::BufferedAudioTimeline::Track> tracks;

            for (int i = 0; i < count; ++i)
                tracks.push_back ({ buffers[i].get(), i, 1.0f });

            return tracks;
        };

        bool threw = false;

        // Zero tracks.
        try { juce::BufferedAudioTimeline t (mk (0)); }
        catch (const std::invalid_argument&) { threw = true; }
        TASK_CHECK (threw);

        // Nine tracks.
        threw = false;
        try { juce::BufferedAudioTimeline t (mk (9)); }
        catch (const std::invalid_argument&) { threw = true; }
        TASK_CHECK (threw);

        // Duplicate source.
        threw = false;
        try
        {
            juce::BufferedAudioTimeline t ({ { buffers[0].get(), 0, 1.f },
                                             { buffers[0].get(), 4, 1.f } });
        }
        catch (const std::invalid_argument&) { threw = true; }
        TASK_CHECK (threw);

        // Non-finite gains.
        for (float bad : { std::numeric_limits<float>::infinity(),
                           -std::numeric_limits<float>::infinity(),
                           std::numeric_limits<float>::quiet_NaN() })
        {
            threw = false;
            try
            {
                juce::BufferedAudioTimeline t ({ { buffers[0].get(), 0, bad },
                                                 { buffers[1].get(), 0, 1.f } });
            }
            catch (const std::invalid_argument&) { threw = true; }
            TASK_CHECK (threw);
        }

        // Bad source starts.
        for (int64 badStart : { int64 (-1), int64 (20000) })
        {
            threw = false;
            try
            {
                juce::BufferedAudioTimeline t ({ { buffers[0].get(), badStart, 1.f } });
            }
            catch (const std::invalid_argument&) { threw = true; }
            TASK_CHECK (threw);
        }

        // A valid 1..8 track configuration, including a mono source.
        auto mono = std::make_unique<TestSource> (11, 1);
        auto monoBuffer = std::make_unique<juce::BufferingAudioSource>
                              (mono.get(), w.thread, false, 4096, 1, false);
        juce::BufferedAudioTimeline timeline ({ { buffers[0].get(), 10, 0.5f },
                                                { monoBuffer.get(), 3, -0.25f } });
        TASK_CHECK (timeline.getTotalLength() == 19990);
        TASK_CHECK (timeline.getOutputPosition() == 0);

        return 0;
    }

    int timeline_prepare_validation()
    {
        Worker w;
        TestSource sa (1, 2), sb (2, 2);
        auto a = makeBuffer (w, &sa);
        auto b = makeBuffer (w, &sb);
        juce::BufferedAudioTimeline timeline ({ { &a, 0, 1.f }, { &b, 0, 1.f } });

        TASK_CHECK (! timeline.prepareToPlay (0, sampleRate));
        TASK_CHECK (! timeline.prepareToPlay (-8, sampleRate));
        TASK_CHECK (! timeline.prepareToPlay (4097, sampleRate));
        TASK_CHECK (! timeline.prepareToPlay (blockSize, 0.0));
        TASK_CHECK (! timeline.prepareToPlay (blockSize, -1.0));
        TASK_CHECK (! timeline.prepareToPlay (blockSize,
                                              std::numeric_limits<double>::quiet_NaN()));
        TASK_CHECK (! timeline.prepareToPlay (blockSize,
                                              std::numeric_limits<double>::infinity()));

        TASK_CHECK (timeline.prepareToPlay (4096, sampleRate));
        TASK_CHECK (timeline.getOutputPosition() == 0);

        // Re-prepare restarts at output 0.
        juce::AudioBuffer<float> out (2, blockSize);
        juce::AudioSourceChannelInfo info (&out, 0, blockSize);
        TASK_CHECK (waitUntilRendered (timeline, info));
        TASK_CHECK (timeline.getOutputPosition() == blockSize);
        TASK_CHECK (timeline.prepareToPlay (128, sampleRate));
        TASK_CHECK (timeline.getOutputPosition() == 0);

        timeline.releaseResources();
        return 0;
    }

    // Independent per-sample reference mixer for the current track layout.
    float referenceMix (const std::vector<juce::BufferedAudioTimeline::Track>& config,
                        const std::vector<int>& channelCounts,
                        int64 outputFrame, int outputChannel)
    {
        float sum = 0.0f;

        for (size_t i = 0; i < config.size(); ++i)
        {
            const auto sourceFrame = config[i].sourceStart + outputFrame;
            const auto sourceChannel = channelCounts[i] == 1 ? 0 : outputChannel;
            const auto id = (int) i + 1;
            sum += config[i].gain * testSample (id, sourceFrame, sourceChannel);
        }

        return sum;
    }

    int timeline_mix_matches_reference()
    {
        Worker w;
        TestSource sa (1, 2), sb (2, 1); // stereo + mono
        auto a = makeBuffer (w, &sa, 2);
        auto b = makeBuffer (w, &sb, 1);

        std::vector<juce::BufferedAudioTimeline::Track> config ({ { &a, 7, 1.0f },
                                                                  { &b, 13, -0.5f } });
        std::vector<int> channelCounts { 2, 1 };

        juce::BufferedAudioTimeline timeline (config);
        TASK_CHECK (timeline.getTotalLength() == 19987);
        TASK_CHECK (timeline.prepareToPlay (128, sampleRate));

        const int64 rendered = 300; // exercises varying block sizes
        juce::AudioBuffer<float> output (2, (int) rendered);
        int64 done = 0;
        const int blocks[] = { 64, 32, 128, 76 };

        for (int bi = 0; done < rendered; ++bi)
        {
            const auto num = juce::jmin<int> (blocks[bi % 4], (int) (rendered - done));
            juce::AudioSourceChannelInfo info (&output, (int) done, num);
            TASK_CHECK (waitUntilRendered (timeline, info));
            done += num;
        }

        TASK_CHECK (timeline.getOutputPosition() == rendered);

        for (int64 frame = 0; frame < rendered; ++frame)
            for (int chan = 0; chan < 2; ++chan)
                TASK_CHECK (closeFloat (output.getSample (chan, (int) frame),
                                        referenceMix (config, channelCounts, frame, chan)));

        timeline.releaseResources();
        return 0;
    }

    int timeline_seek_and_retime()
    {
        Worker w;
        TestSource sa (1, 2), sb (2, 2);
        auto a = makeBuffer (w, &sa);
        auto b = makeBuffer (w, &sb);

        std::vector<juce::BufferedAudioTimeline::Track> config ({ { &a, 7, 1.0f },
                                                                  { &b, 13, -0.5f } });
        std::vector<int> channelCounts { 2, 2 };
        juce::BufferedAudioTimeline timeline (config);
        TASK_CHECK (timeline.prepareToPlay (128, sampleRate));

        TASK_CHECK (! timeline.seek (-1));
        TASK_CHECK (! timeline.seek (timeline.getTotalLength() + 1));
        TASK_CHECK (timeline.seek (100));
        TASK_CHECK (timeline.getOutputPosition() == 100);
        TASK_CHECK (a.getNextReadPosition() == 107);
        TASK_CHECK (b.getNextReadPosition() == 113);

        // Illegal retimes change nothing.
        TASK_CHECK (! timeline.retime ({ 11 }));
        TASK_CHECK (! timeline.retime ({ -1, 19 }));
        TASK_CHECK (! timeline.retime ({ 19999, 19 })); // does not fit at position 100
        TASK_CHECK (timeline.getOutputPosition() == 100);
        TASK_CHECK (a.getNextReadPosition() == 107);
        TASK_CHECK (b.getNextReadPosition() == 113);

        TASK_CHECK (timeline.retime ({ 11, 19 }));
        TASK_CHECK (timeline.getOutputPosition() == 100);
        TASK_CHECK (timeline.getTotalLength() == 19981);
        TASK_CHECK (a.getNextReadPosition() == 111);
        TASK_CHECK (b.getNextReadPosition() == 119);

        std::vector<juce::BufferedAudioTimeline::Track> newConfig ({ { &a, 11, 1.0f },
                                                                     { &b, 19, -0.5f } });

        juce::AudioBuffer<float> out (2, blockSize);
        juce::AudioSourceChannelInfo info (&out, 0, blockSize);
        TASK_CHECK (waitUntilRendered (timeline, info));
        TASK_CHECK (timeline.getOutputPosition() == 100 + blockSize);

        for (int c = 0; c < 2; ++c)
            for (int n = 0; n < blockSize; ++n)
                TASK_CHECK (closeFloat (out.getSample (c, n),
                                        referenceMix (newConfig, channelCounts, 100 + n, c)));

        timeline.releaseResources();
        return 0;
    }

    int timeline_render_failures_and_sentinel()
    {
        Worker w;
        BlockableSource sa (1, 2);
        TestSource sb (2, 2);
        auto a = makeBuffer (w, &sa);
        auto b = makeBuffer (w, &sb);

        juce::BufferedAudioTimeline timeline ({ { &a, 0, 1.f }, { &b, 0, 1.f } });

        juce::AudioBuffer<float> out (2, 256);

        // Not prepared.
        juce::AudioSourceChannelInfo info (&out, 0, blockSize);
        TASK_CHECK (! timeline.tryRender (info));

        TASK_CHECK (timeline.prepareToPlay (128, sampleRate));
        TASK_CHECK (waitUntilRendered (timeline, info));
        TASK_CHECK (timeline.getOutputPosition() == blockSize);

        // Block larger than the prepared maximum.
        fillBuffer (out, 7.0f);
        juce::AudioSourceChannelInfo tooBig (&out, 0, 129);
        TASK_CHECK (! timeline.tryRender (tooBig));
        for (int c = 0; c < 2; ++c)
            for (int n = 0; n < 129; ++n)
                TASK_CHECK (closeFloat (out.getSample (c, n), 0.0f)); // sentinel
        TASK_CHECK (timeline.getOutputPosition() == blockSize);

        // Request past the remaining length.
        fillBuffer (out, 7.0f);
        TASK_CHECK (timeline.seek (timeline.getTotalLength() - 8));
        juce::AudioSourceChannelInfo pastEnd (&out, 0, 64);
        TASK_CHECK (! timeline.tryRender (pastEnd));
        for (int c = 0; c < 2; ++c)
            for (int n = 0; n < 64; ++n)
                TASK_CHECK (closeFloat (out.getSample (c, n), 0.0f));
        TASK_CHECK (timeline.getOutputPosition() == timeline.getTotalLength() - 8);

        // Invalid output region: rejected without writing anything.
        fillBuffer (out, 7.0f);
        juce::AudioSourceChannelInfo badRegion (&out, 200, 64);
        TASK_CHECK (! timeline.tryRender (badRegion));
        for (int c = 0; c < 2; ++c)
            for (int n = 0; n < 256; ++n)
                TASK_CHECK (closeFloat (out.getSample (c, n), 7.0f));

        // Not-ready failure after a seek into a frozen prefetch region.
        sa.armBlock (8000);
        TASK_CHECK (timeline.seek (8000));
        TASK_CHECK (sa.entered.wait (5000));
        fillBuffer (out, 7.0f);
        juce::AudioSourceChannelInfo blockedInfo (&out, 0, blockSize);
        TASK_CHECK (! timeline.tryRender (blockedInfo));
        for (int c = 0; c < 2; ++c)
            for (int n = 0; n < blockSize; ++n)
                TASK_CHECK (closeFloat (out.getSample (c, n), 0.0f));
        TASK_CHECK (timeline.getOutputPosition() == 8000);
        TASK_CHECK (a.getNextReadPosition() == 8000);
        TASK_CHECK (b.getNextReadPosition() == 8000);

        sa.unblock();
        TASK_CHECK (waitUntilRendered (timeline, blockedInfo));
        TASK_CHECK (timeline.getOutputPosition() == 8000 + blockSize);

        timeline.releaseResources();
        return 0;
    }

    int timeline_zero_length()
    {
        Worker w;
        TestSource sa (1, 2), sb (2, 2);
        auto a = makeBuffer (w, &sa);
        auto b = makeBuffer (w, &sb);
        juce::BufferedAudioTimeline timeline ({ { &a, 0, 1.f }, { &b, 0, 1.f } });
        TASK_CHECK (timeline.prepareToPlay (128, sampleRate));

        juce::AudioBuffer<float> out (2, 32);
        fillBuffer (out, 7.0f);
        juce::AudioSourceChannelInfo zero (&out, 4, 0);
        TASK_CHECK (timeline.tryRender (zero));
        TASK_CHECK (timeline.getOutputPosition() == 0);
        TASK_CHECK (a.getNextReadPosition() == 0);
        TASK_CHECK (b.getNextReadPosition() == 0);
        for (int c = 0; c < 2; ++c)
            for (int n = 0; n < 32; ++n)
                TASK_CHECK (closeFloat (out.getSample (c, n), 7.0f));

        timeline.releaseResources();
        return 0;
    }

    int timeline_retime_across_block_sizes()
    {
        Worker w;
        TestSource sa (1, 2), sb (2, 2);
        auto a = makeBuffer (w, &sa);
        auto b = makeBuffer (w, &sb);

        std::vector<juce::BufferedAudioTimeline::Track> config ({ { &a, 0, 0.75f },
                                                                  { &b, 5, 0.5f } });
        std::vector<int> channelCounts { 2, 2 };
        juce::BufferedAudioTimeline timeline (config);
        TASK_CHECK (timeline.prepareToPlay (256, sampleRate));

        constexpr int64 total = 256;
        constexpr int64 retimePoint = 128;
        juce::AudioBuffer<float> output (2, (int) total);

        int64 done = 0;

        while (done < retimePoint)
        {
            const auto num = juce::jmin<int64> (48, retimePoint - done);
            juce::AudioSourceChannelInfo info (&output, (int) done, (int) num);
            TASK_CHECK (waitUntilRendered (timeline, info));
            done += num;
        }

        TASK_CHECK (timeline.retime ({ 20, 33 }));
        std::vector<juce::BufferedAudioTimeline::Track> newConfig ({ { &a, 20, 0.75f },
                                                                     { &b, 33, 0.5f } });

        while (done < total)
        {
            const auto num = juce::jmin<int64> (7, total - done); // different block size
            juce::AudioSourceChannelInfo info (&output, (int) done, (int) num);
            TASK_CHECK (waitUntilRendered (timeline, info));
            done += num;
        }

        TASK_CHECK (timeline.getOutputPosition() == total);

        for (int64 frame = 0; frame < total; ++frame)
        {
            const auto& active = frame < retimePoint ? config : newConfig;

            for (int chan = 0; chan < 2; ++chan)
                TASK_CHECK (closeFloat (output.getSample (chan, (int) frame),
                                        referenceMix (active, channelCounts, frame, chan)));
        }

        timeline.releaseResources();
        return 0;
    }

    int timeline_external_seek_then_retry()
    {
        Worker w;
        TestSource sa (1, 2), sb (2, 2);
        auto a = makeBuffer (w, &sa);
        auto b = makeBuffer (w, &sb);
        juce::BufferedAudioTimeline timeline ({ { &a, 0, 1.f }, { &b, 0, 1.f } });
        TASK_CHECK (timeline.prepareToPlay (128, sampleRate));

        // Render one block, then invalidate the sources' generation externally.
        juce::AudioBuffer<float> out (2, blockSize);
        juce::AudioSourceChannelInfo info (&out, 0, blockSize);
        TASK_CHECK (waitUntilRendered (timeline, info));
        const int64 afterFirst = timeline.getOutputPosition();
        TASK_CHECK (afterFirst == blockSize);

        juce::WaitableEvent go { false }, done { false };
        std::thread seeker ([&]
        {
            go.wait (-1.0);
            // An explicit seek by another thread is not a renderer consume.
            // Moving to a different position makes the renderer's next peeks
            // cover the wrong frame, so tryRender must fail without consuming.
            a.setNextReadPosition (a.getNextReadPosition() + 128);
            b.setNextReadPosition (b.getNextReadPosition() + 128);
            done.signal();
        });

        go.signal();
        TASK_CHECK (done.wait (5000));
        seeker.join();

        // The group cursor is untouched while the underlying sources moved.
        TASK_CHECK (! timeline.tryRender (info));
        TASK_CHECK (timeline.getOutputPosition() == afterFirst);

        // Put the sources back where the timeline expects them; rendering then
        // succeeds again from the unchanged group position.
        a.setNextReadPosition (afterFirst);
        b.setNextReadPosition (afterFirst);
        TASK_CHECK (waitUntilRendered (timeline, info));
        TASK_CHECK (timeline.getOutputPosition() == afterFirst + blockSize);

        timeline.releaseResources();
        return 0;
    }
}

//==============================================================================
extern const task_case task_cases[];
const task_case task_cases[] =
{
    { "tickets",  "lifecycle",                tickets_lifecycle },
    { "tickets",  "rejections",               tickets_rejections },
    { "tickets",  "blocked_background",       tickets_blocked_background },
    { "tickets",  "seek_invalidation",        tickets_seek_invalidation },
    { "tickets",  "release_and_reprepare",    tickets_release_and_reprepare },
    { "tickets",  "normal_consume",           tickets_normal_consume_invalidates },
    { "tickets",  "commit_validation",        tickets_commit_validation },
    { "tickets",  "ring_wraparound",          tickets_ring_wraparound },
    { "tickets",  "failure_retry",            tickets_failure_retry },

    { "atomic",   "reverse_order_groups",     atomic_reverse_order_groups },
    { "atomic",   "commit_while_seek",        atomic_commit_while_seek },
    { "atomic",   "stale_read_after_seek",    atomic_stale_read_after_seek },
    { "atomic",   "single_source_group",      atomic_single_source_group },
    { "atomic",   "group_pcm_and_positions",  atomic_group_pcm_and_positions },

    { "timeline", "construction_rules",       timeline_construction_rules },
    { "timeline", "prepare_validation",       timeline_prepare_validation },
    { "timeline", "mix_matches_reference",    timeline_mix_matches_reference },
    { "timeline", "seek_and_retime",          timeline_seek_and_retime },
    { "timeline", "render_failures_sentinel", timeline_render_failures_and_sentinel },
    { "timeline", "zero_length",              timeline_zero_length },
    { "timeline", "retime_block_sizes",       timeline_retime_across_block_sizes },
    { "timeline", "external_seek_retry",      timeline_external_seek_then_retry },
};

extern const size_t task_case_count;
const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases[0]);
