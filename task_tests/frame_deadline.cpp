#include "task_support/check.h"

#define TASK_CHECK_OK(expr) TASK_CHECK((expr) == 0)
#include "RtpQueue.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>

#define private public
#include "ScreamTx.h"
#undef private

extern int task_packet_freed;
void packet_free(void* packet, uint32_t ssrc);

static const uint32_t SSRC_A = 1001u;
static const uint32_t SSRC_B = 2002u;

static uint32_t ntpTime(float seconds) {
    return static_cast<uint32_t>(seconds * 65536.0f + 0.5f);
}

static int enqueuePacket(RtpQueue& queue,
    uint16_t seqNr,
    bool marker,
    float pushTs,
    uint32_t rtpTs,
    int size = 100,
    uint32_t ssrc = SSRC_A) {
    void* packet = std::malloc(static_cast<size_t>(size));
    TASK_CHECK(packet != NULL);
    bool pushed = queue.push(packet, size, ssrc, seqNr, marker, pushTs, rtpTs);
    if (!pushed)
        std::free(packet);
    TASK_CHECK(pushed);
    return 0;
}

static int enqueueRejected(RtpQueue& queue,
    uint16_t seqNr,
    bool marker,
    float pushTs,
    uint32_t rtpTs,
    uint32_t ssrc = SSRC_A) {
    void* packet = std::malloc(100);
    TASK_CHECK(packet != NULL);
    bool pushed = queue.push(packet, 100, ssrc, seqNr, marker, pushTs, rtpTs);
    TASK_CHECK(!pushed);
    /* A failed push does not transfer ownership. */
    packet_free(packet, ssrc);
    return 0;
}

static int popAndFree(RtpQueue& queue,
    uint16_t expectedSeq,
    uint32_t expectedTs,
    uint32_t ssrc = SSRC_A) {
    void* packet = NULL;
    int size = 0;
    uint32_t outSsrc = 0;
    uint32_t outTs = 0;
    uint16_t seq = 0;
    bool marker = false;
    TASK_CHECK(queue.pop(&packet, size, outSsrc, seq, marker, outTs));
    TASK_CHECK(seq == expectedSeq);
    TASK_CHECK(outTs == expectedTs);
    TASK_CHECK(outSsrc == ssrc);
    TASK_CHECK(packet != NULL);
    packet_free(packet, outSsrc);
    return 0;
}

static int popTransmitAndFree(ScreamV2Tx& tx,
    RtpQueue& queue,
    uint32_t timeNtp,
    uint16_t expectedSeq,
    uint32_t expectedTs,
    uint32_t ssrc = SSRC_A) {
    void* packet = NULL;
    int size = 0;
    uint32_t outSsrc = 0;
    uint32_t outTs = 0;
    uint16_t seq = 0;
    bool marker = false;
    TASK_CHECK(queue.pop(&packet, size, outSsrc, seq, marker, outTs));
    TASK_CHECK(seq == expectedSeq);
    TASK_CHECK(outTs == expectedTs);
    TASK_CHECK(outSsrc == ssrc);
    TASK_CHECK(packet != NULL);
    TASK_CHECK(tx.addTransmitted(timeNtp, outSsrc, size, seq, marker, 0.0f, outTs) >= 0.0f);
    packet_free(packet, outSsrc);
    return 0;
}

static int checkQueueEmpty(RtpQueue& queue) {
    TASK_CHECK(queue.sizeOfQueue() == 0);
    TASK_CHECK(queue.bytesInQueue() == 0);
    TASK_CHECK(queue.seqNrOfNextRtp() == -1);
    TASK_CHECK(queue.seqNrOfLastRtp() == -1);
    TASK_CHECK(queue.getDelay(1234.0f) == 0.0f);
    return 0;
}

static int pumpRing(RtpQueue& queue, int count, uint16_t& seq) {
    for (int n = 0; n < count; ++n) {
        float at = 40.0f + n * 0.001f;
        TASK_CHECK_OK(enqueuePacket(queue, seq, true, at,
            static_cast<uint32_t>(0x6000u + n), 100, SSRC_A));
        TASK_CHECK_OK(popAndFree(queue, seq, static_cast<uint32_t>(0x6000u + n), SSRC_A));
        ++seq;
    }
    return 0;
}

/* ------------------------------ frames group ----------------------------- */

static int frames_cross_empty_queue_until_marker(void) {
    RtpQueue queue;
    TASK_CHECK(queue.setFrameDeadline(0.25f));

    const int freedBefore = task_packet_freed;
    TASK_CHECK_OK(enqueuePacket(queue, 10, false, 1.00f, 0x100u));
    TASK_CHECK_OK(enqueuePacket(queue, 11, false, 1.01f, 0x100u));
    TASK_CHECK(queue.sizeOfQueue() == 2);

    /* Sending the complete prefix leaves the queue empty, not the frame. */
    TASK_CHECK_OK(popAndFree(queue, 10, 0x100u));
    TASK_CHECK_OK(popAndFree(queue, 11, 0x100u));
    TASK_CHECK_OK(checkQueueEmpty(queue));
    TASK_CHECK(task_packet_freed == freedBefore + 2);

    TASK_CHECK(!queue.checkFrameDeadline(1.24f));
    TASK_CHECK(queue.checkFrameDeadline(1.25f));
    /* There were no queued packets left to evict. */
    TASK_CHECK(task_packet_freed == freedBefore + 2);
    TASK_CHECK(!queue.checkFrameDeadline(1.30f));
    TASK_CHECK(queue.clear() == 0);

    /* Same-frame suffixes are rejected and remain caller-owned. */
    TASK_CHECK_OK(enqueueRejected(queue, 12, false, 1.26f, 0x100u));
    TASK_CHECK_OK(enqueueRejected(queue, 13, true, 1.27f, 0x100u));

    /* The rejected marker ended that frame; this starts a new generation. */
    TASK_CHECK_OK(enqueuePacket(queue, 14, false, 1.28f, 0x100u));
    TASK_CHECK(queue.sizeOfQueue() == 1);
    TASK_CHECK(queue.seqNrOfNextRtp() == 14);
    TASK_CHECK(queue.seqNrOfLastRtp() == 14);

    const int expireFreedBefore = task_packet_freed;
    TASK_CHECK(queue.checkFrameDeadline(1.53f));
    TASK_CHECK(task_packet_freed == expireFreedBefore + 1);
    TASK_CHECK_OK(checkQueueEmpty(queue));
    return 0;
}

static int frames_timestamp_change_ends_rejection(void) {
    RtpQueue queue;
    TASK_CHECK(queue.setFrameDeadline(0.20f));

    TASK_CHECK_OK(enqueuePacket(queue, 20, false, 2.00f, 0x200u));
    TASK_CHECK_OK(popAndFree(queue, 20, 0x200u));
    TASK_CHECK(queue.checkFrameDeadline(2.20f));

    TASK_CHECK_OK(enqueueRejected(queue, 21, false, 2.21f, 0x200u));

    /* A different timestamp is the next frame boundary and is accepted. */
    TASK_CHECK_OK(enqueuePacket(queue, 22, false, 2.22f, 0x201u));
    TASK_CHECK_OK(enqueuePacket(queue, 23, false, 2.23f, 0x201u));
    TASK_CHECK(queue.sizeOfQueue() == 2);
    TASK_CHECK(queue.seqNrOfNextRtp() == 22);
    TASK_CHECK(queue.seqNrOfLastRtp() == 23);

    TASK_CHECK(queue.clear() == 2);
    TASK_CHECK(queue.sizeOfQueue() == 0);
    return 0;
}

static int frames_boundary_before_deadline_no_late_notification(void) {
    RtpQueue queue;
    TASK_CHECK(queue.setFrameDeadline(0.25f));

    TASK_CHECK_OK(enqueuePacket(queue, 25, false, 3.00f, 0x250u));
    TASK_CHECK_OK(popAndFree(queue, 25, 0x250u));

    /* The next frame starts before the old frame's deadline. */
    TASK_CHECK_OK(enqueuePacket(queue, 26, false, 3.10f, 0x251u));
    TASK_CHECK(queue.checkFrameDeadline(3.30f) == false);
    TASK_CHECK(queue.sizeOfQueue() == 1);
    TASK_CHECK(queue.seqNrOfNextRtp() == 26);
    TASK_CHECK(queue.clear() == 1);
    return 0;
}

static int frames_complete_old_frame_and_fresh_neighbor(void) {
    RtpQueue queue;
    TASK_CHECK(queue.setFrameDeadline(0.20f));

    const int freedBefore = task_packet_freed;
    TASK_CHECK_OK(enqueuePacket(queue, 100, false, 10.00f, 0xFFFFFFFEu, 100));
    TASK_CHECK_OK(enqueuePacket(queue, 101, true, 10.01f, 0xFFFFFFFEu, 140));
    TASK_CHECK_OK(enqueuePacket(queue, 102, false, 10.10f, 0x00000001u, 100));
    TASK_CHECK(queue.bytesInQueue() == 340);

    TASK_CHECK(queue.checkFrameDeadline(10.25f));
    TASK_CHECK(queue.sizeOfQueue() == 1);
    TASK_CHECK(queue.bytesInQueue() == 100);
    TASK_CHECK(queue.seqNrOfNextRtp() == 102);
    TASK_CHECK(queue.seqNrOfLastRtp() == 102);
    TASK_CHECK(std::fabs(queue.getDelay(10.25f) - 0.15f) < 0.00001f);
    TASK_CHECK(task_packet_freed == freedBefore + 2);

    /* The same old frame must not notify/evict twice. */
    TASK_CHECK(!queue.checkFrameDeadline(10.27f));
    TASK_CHECK(queue.sizeOfQueue() == 1);
    TASK_CHECK_OK(popAndFree(queue, 102, 0x00000001u));
    return 0;
}

static int frames_deadline_boundary_is_inclusive(void) {
    RtpQueue queue;
    TASK_CHECK(queue.setFrameDeadline(0.125f));
    TASK_CHECK_OK(enqueuePacket(queue, 30, false, 5.0f, 0x300u));

    TASK_CHECK(!queue.checkFrameDeadline(5.1240234375f));
    TASK_CHECK(queue.sizeOfQueue() == 1);

    const int freedBefore = task_packet_freed;
    TASK_CHECK(queue.checkFrameDeadline(5.125f));
    TASK_CHECK(task_packet_freed == freedBefore + 1);
    TASK_CHECK_OK(checkQueueEmpty(queue));
    TASK_CHECK(!queue.checkFrameDeadline(5.25f));
    return 0;
}

static int frames_timestamp_zero_crossing_uses_equality(void) {
    RtpQueue queue;
    TASK_CHECK(queue.setFrameDeadline(0.10f));
    TASK_CHECK_OK(enqueuePacket(queue, 40, true, 7.00f, 0xFFFFFFFFu));
    TASK_CHECK_OK(enqueuePacket(queue, 41, false, 7.05f, 0x00000000u));

    TASK_CHECK(queue.checkFrameDeadline(7.11f));
    TASK_CHECK(queue.sizeOfQueue() == 1);
    TASK_CHECK(queue.seqNrOfNextRtp() == 41);
    TASK_CHECK(queue.seqNrOfLastRtp() == 41);
    TASK_CHECK(std::fabs(queue.getDelay(7.11f) - 0.06f) < 0.00001f);
    TASK_CHECK(queue.clear() == 1);
    return 0;
}

static int frames_ring_wrap_eviction(void) {
    RtpQueue queue;
    TASK_CHECK(queue.setFrameDeadline(0.25f));

    uint16_t seq = 0;
    TASK_CHECK_OK(pumpRing(queue, kRtpQueueSize + 77, seq));
    TASK_CHECK(queue.sizeOfQueue() == 0);

    const int freedBefore = task_packet_freed;
    TASK_CHECK_OK(enqueuePacket(queue, 65534, false, 30.00f, 0x400u));
    TASK_CHECK_OK(enqueuePacket(queue, 65535, false, 30.01f, 0x400u));
    TASK_CHECK_OK(enqueuePacket(queue, 0, false, 30.10f, 0x401u));

    TASK_CHECK(queue.checkFrameDeadline(30.26f));
    TASK_CHECK(queue.sizeOfQueue() == 1);
    TASK_CHECK(queue.bytesInQueue() == 100);
    TASK_CHECK(queue.seqNrOfNextRtp() == 0);
    TASK_CHECK(queue.seqNrOfLastRtp() == 0);
    TASK_CHECK(std::fabs(queue.getDelay(30.26f) - 0.16f) < 0.00001f);
    TASK_CHECK(task_packet_freed == freedBefore + 2);

    TASK_CHECK_OK(popAndFree(queue, 0, 0x401u));

    /* The rebuilt ring must remain usable after another full wrap. */
    seq = 1000;
    TASK_CHECK_OK(pumpRing(queue, kRtpQueueSize + 31, seq));
    TASK_CHECK(queue.sizeOfQueue() == 0);
    return 0;
}

/* ---------------------------- scheduler group ---------------------------- */

static int scheduler_checks_every_enabled_stream(void) {
    RtpQueue queueA;
    RtpQueue queueB;
    ScreamV2Tx tx;
    tx.enablePacketPacing(false);
    tx.registerNewStream(&queueA, SSRC_A, 1.0f, 100000, 500000, 1000000, 10.0f);
    tx.registerNewStream(&queueB, SSRC_B, 1.0f, 100000, 500000, 1000000, 10.0f);

    TASK_CHECK(tx.setFrameDeadline(SSRC_A, 0.25f));
    TASK_CHECK(tx.setFrameDeadline(SSRC_B, 0.25f));

    uint32_t selected = 0;
    TASK_CHECK(tx.isOkToTransmit(1, selected) == -1.0f);

    TASK_CHECK_OK(enqueuePacket(queueA, 10, false, 1.0f, 0x111u, 100, SSRC_A));
    TASK_CHECK_OK(enqueuePacket(queueB, 20, false, 1.0f, 0x222u, 100, SSRC_B));
    TASK_CHECK_OK(popTransmitAndFree(tx, queueA, ntpTime(1.0f), 10, 0x111u, SSRC_A));
    TASK_CHECK_OK(popTransmitAndFree(tx, queueB, ntpTime(1.0f), 20, 0x222u, SSRC_B));
    TASK_CHECK_OK(checkQueueEmpty(queueA));
    TASK_CHECK_OK(checkQueueEmpty(queueB));

    /* One scheduling call must expire both ghost frames. */
    TASK_CHECK(tx.isOkToTransmit(ntpTime(1.25f), selected) == -1.0f);
    TASK_CHECK(tx.getTargetBitrate(ntpTime(1.25f), SSRC_A) == -1.0f);
    TASK_CHECK(tx.getTargetBitrate(ntpTime(1.25f), SSRC_B) == -1.0f);

    TASK_CHECK(tx.isOkToTransmit(ntpTime(1.30f), selected) == -1.0f);
    TASK_CHECK(tx.getTargetBitrate(ntpTime(1.30f), SSRC_A) > 0.0f);
    TASK_CHECK(tx.getTargetBitrate(ntpTime(1.30f), SSRC_B) > 0.0f);
    return 0;
}

static int scheduler_fresh_peer_still_selected(void) {
    RtpQueue queueA;
    RtpQueue queueB;
    ScreamV2Tx tx;
    tx.enablePacketPacing(false);
    tx.registerNewStream(&queueA, SSRC_A, 1.0f, 100000, 500000, 1000000, 10.0f);
    tx.registerNewStream(&queueB, SSRC_B, 1.0f, 100000, 500000, 1000000, 10.0f);
    TASK_CHECK(tx.setFrameDeadline(SSRC_A, 0.25f));
    TASK_CHECK(tx.setFrameDeadline(SSRC_B, 0.25f));

    uint32_t selected = 0;
    TASK_CHECK(tx.isOkToTransmit(1, selected) == -1.0f);

    TASK_CHECK_OK(enqueuePacket(queueA, 10, false, 1.0f, 0x111u, 100, SSRC_A));
    TASK_CHECK_OK(popTransmitAndFree(tx, queueA, ntpTime(1.0f), 10, 0x111u, SSRC_A));

    TASK_CHECK_OK(enqueuePacket(queueB, 20, false, 1.20f, 0x222u, 100, SSRC_B));

    float allowed = tx.isOkToTransmit(ntpTime(1.25f), selected);
    TASK_CHECK(allowed == 0.0f);
    TASK_CHECK(selected == SSRC_B);
    TASK_CHECK(tx.getTargetBitrate(ntpTime(1.25f), SSRC_A) == -1.0f);
    TASK_CHECK(tx.getTargetBitrate(ntpTime(1.25f), SSRC_B) > 0.0f);

    TASK_CHECK_OK(enqueueRejected(queueA, 11, false, 1.26f, 0x111u, SSRC_A));
    TASK_CHECK_OK(popAndFree(queueB, 20, 0x222u, SSRC_B));
    return 0;
}

static int scheduler_evicts_queued_stale_frame_before_selection(void) {
    RtpQueue queueA;
    RtpQueue queueB;
    ScreamV2Tx tx;
    tx.enablePacketPacing(false);
    tx.registerNewStream(&queueA, SSRC_A, 1.0f, 100000, 500000, 1000000, 10.0f);
    tx.registerNewStream(&queueB, SSRC_B, 1.0f, 100000, 500000, 1000000, 10.0f);
    TASK_CHECK(tx.setFrameDeadline(SSRC_A, 0.25f));
    TASK_CHECK(tx.setFrameDeadline(SSRC_B, 0.25f));

    uint32_t selected = 0;
    TASK_CHECK(tx.isOkToTransmit(1, selected) == -1.0f);

    TASK_CHECK_OK(enqueuePacket(queueA, 30, false, 1.00f, 0x311u, 100, SSRC_A));
    TASK_CHECK_OK(enqueuePacket(queueA, 31, false, 1.01f, 0x311u, 100, SSRC_A));
    TASK_CHECK_OK(enqueuePacket(queueB, 40, false, 1.20f, 0x422u, 100, SSRC_B));

    const int freedBefore = task_packet_freed;
    float allowed = tx.isOkToTransmit(ntpTime(1.25f), selected);
    TASK_CHECK(allowed == 0.0f);
    TASK_CHECK(selected == SSRC_B);
    TASK_CHECK_OK(checkQueueEmpty(queueA));
    TASK_CHECK(task_packet_freed == freedBefore + 2);
    TASK_CHECK(tx.getTargetBitrate(ntpTime(1.25f), SSRC_A) == -1.0f);
    TASK_CHECK_OK(popAndFree(queueB, 40, 0x422u, SSRC_B));
    return 0;
}

static int scheduler_expiration_keeps_ack_records_and_in_flight(void) {
    RtpQueue queue;
    ScreamV2Tx tx;
    tx.enablePacketPacing(false);
    tx.registerNewStream(&queue, SSRC_A, 1.0f, 100000, 500000, 1000000, 10.0f);
    TASK_CHECK(tx.setFrameDeadline(SSRC_A, 0.25f));

    uint32_t selected = 0;
    TASK_CHECK(tx.isOkToTransmit(1, selected) == -1.0f);

    TASK_CHECK_OK(enqueuePacket(queue, 50, false, 1.00f, 0x500u, 120));
    TASK_CHECK(tx.isOkToTransmit(ntpTime(1.0f), selected) == 0.0f);
    TASK_CHECK(selected == SSRC_A);

    void* packet = NULL;
    int size = 0;
    uint32_t ssrc = 0;
    uint32_t rtpTs = 0;
    uint16_t seq = 0;
    bool marker = false;
    TASK_CHECK(queue.pop(&packet, size, ssrc, seq, marker, rtpTs));
    TASK_CHECK(seq == 50);
    TASK_CHECK(tx.addTransmitted(ntpTime(1.0f), ssrc, size, seq, marker, 0.0f, rtpTs) >= 0.0f);
    packet_free(packet, ssrc);
    TASK_CHECK(tx.bytesInFlight == 120);

    /* This unsent suffix is evicted, but must not touch TX/ACK bookkeeping. */
    TASK_CHECK_OK(enqueuePacket(queue, 51, false, 1.01f, 0x500u, 120));
    TASK_CHECK(tx.isOkToTransmit(ntpTime(1.25f), selected) == -1.0f);
    TASK_CHECK(queue.sizeOfQueue() == 0);
    TASK_CHECK(tx.bytesInFlight == 120);
    TASK_CHECK(!tx.isLossEpoch(SSRC_A));

    const int txIndex = 50 % 4096;
    TASK_CHECK(tx.streams[0]->txPackets[txIndex].isUsed);
    TASK_CHECK(tx.streams[0]->txPackets[txIndex].seqNr == 50);
    TASK_CHECK(tx.streams[0]->txPackets[txIndex].size == 120);

    TASK_CHECK(tx.getTargetBitrate(ntpTime(1.25f), SSRC_A) == -1.0f);

    /* The retained transmitted record is still ACKable. */
    tx.incomingStandardizedFeedback(ntpTime(1.30f), 0, ntpTime(1.30f),
        50, 0, true);
    TASK_CHECK(tx.bytesInFlight == 0);
    return 0;
}

/* -------------------------- compatibility group -------------------------- */

class LegacyQueue : public RtpQueueIface {
public:
    int clear() override { return 0; }
    int sizeOfNextRtp() override { return 0; }
    int seqNrOfNextRtp() override { return -1; }
    int seqNrOfLastRtp() override { return -1; }
    int bytesInQueue() override { return 0; }
    int sizeOfQueue() override { return 0; }
    float getDelay(float) override { return 0.0f; }
    int getSizeOfLastFrame() override { return 0; }
};

static int compatibility_external_iface_default_unsupported(void) {
    LegacyQueue legacy;
    ScreamV2Tx tx;
    tx.registerNewStream(&legacy, SSRC_B, 1.0f, 100000, 500000, 1000000);
    TASK_CHECK(!tx.setFrameDeadline(SSRC_B, 0.25f));
    TASK_CHECK(!tx.setFrameDeadline(SSRC_B, 0.0f));
    return 0;
}

static int compatibility_invalid_config_keeps_previous_value(void) {
    RtpQueue queue;
    ScreamV2Tx tx;
    tx.registerNewStream(&queue, SSRC_A, 1.0f, 100000, 500000, 1000000);
    TASK_CHECK(tx.setFrameDeadline(SSRC_A, 0.25f));

    TASK_CHECK(!tx.setFrameDeadline(SSRC_A, -0.01f));
    TASK_CHECK(!tx.setFrameDeadline(SSRC_A, std::numeric_limits<float>::quiet_NaN()));
    TASK_CHECK(!tx.setFrameDeadline(SSRC_A, std::numeric_limits<float>::infinity()));
    TASK_CHECK(!tx.setFrameDeadline(SSRC_A, -std::numeric_limits<float>::infinity()));
    TASK_CHECK(!tx.setFrameDeadline(999999u, 0.25f));

    TASK_CHECK_OK(enqueuePacket(queue, 60, false, 20.0f, 0x600u));
    TASK_CHECK_OK(popAndFree(queue, 60, 0x600u));
    TASK_CHECK(queue.checkFrameDeadline(20.25f));
    return 0;
}

static int compatibility_zero_restores_whole_queue_clear(void) {
    RtpQueue queue;
    ScreamV2Tx tx;
    tx.enablePacketPacing(false);
    tx.registerNewStream(&queue, SSRC_A, 1.0f, 100000, 500000, 1000000, 0.05f);
    uint32_t selected = 0;
    TASK_CHECK(tx.isOkToTransmit(1, selected) == -1.0f);

    TASK_CHECK(tx.setFrameDeadline(SSRC_A, 0.50f));
    TASK_CHECK(tx.setFrameDeadline(SSRC_A, 0.0f));

    TASK_CHECK_OK(enqueuePacket(queue, 70, false, 2.00f, 0x700u));
    tx.newMediaFrame(ntpTime(2.00f), SSRC_A, 100, true);
    TASK_CHECK(queue.sizeOfQueue() == 1);

    const int freedBefore = task_packet_freed;
    tx.newMediaFrame(ntpTime(2.30f), SSRC_A, 100, true);
    TASK_CHECK(queue.sizeOfQueue() == 0);
    TASK_CHECK(task_packet_freed == freedBefore + 1);
    return 0;
}

static int compatibility_enabled_deadline_replaces_delay_clear(void) {
    RtpQueue queue;
    ScreamV2Tx tx;
    tx.enablePacketPacing(false);
    tx.registerNewStream(&queue, SSRC_A, 1.0f, 100000, 500000, 1000000, 0.05f);
    TASK_CHECK(tx.setFrameDeadline(SSRC_A, 0.50f));
    uint32_t selected = 0;
    TASK_CHECK(tx.isOkToTransmit(1, selected) == -1.0f);

    TASK_CHECK_OK(enqueuePacket(queue, 80, false, 2.00f, 0x800u));
    tx.newMediaFrame(ntpTime(2.00f), SSRC_A, 100, true);
    tx.newMediaFrame(ntpTime(2.30f), SSRC_A, 100, true);

    /* 0.30s exceeds maxRtpQueueDelay, but the 0.50s frame deadline has not. */
    TASK_CHECK(queue.sizeOfQueue() == 1);
    TASK_CHECK(!queue.checkFrameDeadline(2.49f));
    TASK_CHECK(queue.sizeOfQueue() == 1);
    TASK_CHECK(queue.checkFrameDeadline(2.50f));
    TASK_CHECK(queue.sizeOfQueue() == 0);
    return 0;
}

static int compatibility_baseline_flow_still_works(void) {
    RtpQueue queue;
    ScreamV2Tx tx;
    tx.enablePacketPacing(false);
    tx.registerNewStream(&queue, 42, 1.0f, 100000, 500000, 1000000, 10.0f);

    uint32_t emptySelected = 0;
    TASK_CHECK(tx.isOkToTransmit(1, emptySelected) == -1.0f);

    void* packet = NULL;
    TASK_CHECK_OK(enqueuePacket(queue, 10, false, 1.0f, 90000u, 100, 42));
    tx.newMediaFrame(65536, 42, 100, false);

    uint32_t selected = 0;
    TASK_CHECK(tx.isOkToTransmit(65536, selected) == 0.0f);
    TASK_CHECK(selected == 42);

    int size = 0;
    uint32_t ssrc = 0;
    uint32_t stamp = 0;
    uint16_t seq = 0;
    bool marker = false;
    TASK_CHECK(queue.pop(&packet, size, ssrc, seq, marker, stamp));
    TASK_CHECK(seq == 10);
    TASK_CHECK(tx.addTransmitted(65536, 42, size, seq, marker, 0.0f, stamp) >= 0.0f);
    packet_free(packet, ssrc);

    TASK_CHECK_OK(enqueuePacket(queue, 11, true, 1.20f, 90000u, 100, 42));
    TASK_CHECK_OK(enqueuePacket(queue, 12, true, 1.24f, 93000u, 100, 42));
    TASK_CHECK(std::fabs(queue.getDelay(1.25f) - 0.05f) < 0.00001f);
    TASK_CHECK(queue.sizeOfQueue() == 2);
    TASK_CHECK(queue.bytesInQueue() == 200);

    TASK_CHECK(tx.isOkToTransmit(81920, selected) == 0.0f);
    TASK_CHECK(selected == 42);
    TASK_CHECK(queue.clear() == 2);
    return 0;
}

const struct task_case task_cases[] = {
    {"frames", "cross_empty_queue_until_marker", frames_cross_empty_queue_until_marker},
    {"frames", "timestamp_change_ends_rejection", frames_timestamp_change_ends_rejection},
    {"frames", "boundary_before_deadline_no_late_notification", frames_boundary_before_deadline_no_late_notification},
    {"frames", "complete_old_and_fresh_neighbor", frames_complete_old_frame_and_fresh_neighbor},
    {"frames", "deadline_boundary", frames_deadline_boundary_is_inclusive},
    {"frames", "timestamp_zero_crossing", frames_timestamp_zero_crossing_uses_equality},
    {"frames", "ring_wrap_eviction", frames_ring_wrap_eviction},

    {"scheduler", "checks_every_enabled_stream", scheduler_checks_every_enabled_stream},
    {"scheduler", "fresh_peer_still_selected", scheduler_fresh_peer_still_selected},
    {"scheduler", "evicts_queued_stale_frame", scheduler_evicts_queued_stale_frame_before_selection},
    {"scheduler", "keeps_ack_records_and_in_flight", scheduler_expiration_keeps_ack_records_and_in_flight},

    {"compatibility", "external_iface_default_unsupported", compatibility_external_iface_default_unsupported},
    {"compatibility", "invalid_config_keeps_value", compatibility_invalid_config_keeps_previous_value},
    {"compatibility", "zero_restores_whole_queue_clear", compatibility_zero_restores_whole_queue_clear},
    {"compatibility", "deadline_replaces_delay_clear", compatibility_enabled_deadline_replaces_delay_clear},
    {"compatibility", "baseline_flow", compatibility_baseline_flow_still_works},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
