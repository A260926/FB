#include "../task_support/check.h"
#include "RtpQueue.h"
#include "ScreamTx.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>

/*
* Regression tests for the optional per-frame deadline strategy.
* Time is fully controlled: queues are driven with float second timestamps
* while the scheduler is called with the matching NTP time (Q16).
*/

extern int task_packet_freed;
extern void packet_free(void* p, uint32_t ssrc);

static const uint32_t kSsrcA = 42;
static const uint32_t kSsrcB = 43;
static const int kPktSize = 100;

static uint32_t ntp(float seconds) {
	return (uint32_t)(seconds * 65536.0f + 0.5f);
}

/*
* Push a packet that is expected to be accepted. On failure the packet is
* released here so that the ownership rules are honored.
*/
static int push_pkt(RtpQueue& q, uint32_t ssrc, unsigned short seq,
	bool marker, float at, uint32_t stamp) {
	void* p = std::malloc(kPktSize);
	TASK_CHECK(p != NULL);
	if (!q.push(p, kPktSize, ssrc, seq, marker, at, stamp)) {
		std::free(p);
		TASK_CHECK(false);
	}
	return 0;
}

/*
* Push a packet and report whether the queue accepted ownership.
* A rejected packet is released by the caller (here, on its behalf).
*/
static int push_accepted(RtpQueue& q, uint32_t ssrc, unsigned short seq,
	bool marker, float at, uint32_t stamp, bool& accepted) {
	void* p = std::malloc(kPktSize);
	TASK_CHECK(p != NULL);
	accepted = q.push(p, kPktSize, ssrc, seq, marker, at, stamp);
	if (!accepted) {
		std::free(p);
	}
	return 0;
}

/*
* Pop a packet, whose ownership is returned to the caller, and release it
* with plain free() so that task_packet_freed only counts packets freed by
* the queue itself (eviction and clear).
*/
static int pop_pkt(RtpQueue& q, unsigned short expectSeq) {
	void* p = NULL;
	int size = 0;
	uint32_t ssrc = 0, stamp = 0;
	unsigned short seq = 0;
	bool marker = false;
	TASK_CHECK(q.pop(&p, size, ssrc, seq, marker, stamp));
	TASK_CHECK(seq == expectSeq);
	TASK_CHECK(size == kPktSize);
	TASK_CHECK(p != NULL);
	std::free(p);
	return 0;
}

/*
* A minimal external implementation of the original queue interface.
* The new frame deadline methods must compile and fall back to defaults.
*/
class ExternalQueue : public RtpQueueIface {
public:
	int clear() override { return 0; }
	int sizeOfNextRtp() override { return -1; }
	int seqNrOfNextRtp() override { return -1; }
	int seqNrOfLastRtp() override { return -1; }
	int bytesInQueue() override { return 0; }
	int sizeOfQueue() override { return 0; }
	float getDelay(float) override { return 0.0f; }
	int getSizeOfLastFrame() override { return 0; }
};

// ============================ frames group ============================

/*
* The prefix of a frame is transmitted and the queue drains, but without a
* marker the frame age is retained. A late suffix is still rejected once the
* frame reaches its deadline, and the next frame gets through normally.
*/
static int frames_suffix_across_empty_queue() {
	RtpQueue q;
	TASK_CHECK(q.setFrameDeadline(0.1f));

	if (push_pkt(q, kSsrcA, 1, false, 1.000f, 90000)) return 1;
	if (push_pkt(q, kSsrcA, 2, false, 1.000f, 90000)) return 1;
	if (pop_pkt(q, 1)) return 1;
	if (pop_pkt(q, 2)) return 1;

	TASK_CHECK(q.sizeOfQueue() == 0);
	TASK_CHECK(q.bytesInQueue() == 0);
	TASK_CHECK(q.seqNrOfNextRtp() == -1);
	TASK_CHECK(q.getDelay(1.05f) == 0.0f);

	/* Still within the deadline, a suffix packet is accepted. */
	if (push_pkt(q, kSsrcA, 3, false, 1.050f, 90000)) return 1;
	TASK_CHECK(q.sizeOfQueue() == 1);

	/* Age is measured from the first packet (1.0s), not from the suffix. */
	TASK_CHECK(q.expireFrameDeadlines(1.099f) == 0);
	int freedBefore = task_packet_freed;
	TASK_CHECK(q.expireFrameDeadlines(1.101f) == 1);
	TASK_CHECK(task_packet_freed == freedBefore + 1);

	TASK_CHECK(q.sizeOfQueue() == 0);
	TASK_CHECK(q.bytesInQueue() == 0);
	TASK_CHECK(q.seqNrOfNextRtp() == -1);
	TASK_CHECK(q.seqNrOfLastRtp() == -1);
	TASK_CHECK(q.getDelay(1.101f) == 0.0f);

	/* The deadline event is signaled once for this frame only. */
	TASK_CHECK(q.consumeFrameDeadlineEvent());
	TASK_CHECK(!q.consumeFrameDeadlineEvent());
	TASK_CHECK(q.expireFrameDeadlines(1.12f) == 0);
	TASK_CHECK(!q.consumeFrameDeadlineEvent());

	/* Suffix packets of the dead frame are refused and never freed by the queue. */
	bool accepted = true;
	if (push_accepted(q, kSsrcA, 4, false, 1.102f, 90000, accepted)) return 1;
	TASK_CHECK(!accepted);
	TASK_CHECK(task_packet_freed == freedBefore + 1);

	/* The marker of the dead frame is refused as well but ends rejection. */
	if (push_accepted(q, kSsrcA, 5, true, 1.103f, 90000, accepted)) return 1;
	TASK_CHECK(!accepted);

	/* A fresh frame is accepted again. */
	if (push_pkt(q, kSsrcA, 6, false, 1.110f, 93000)) return 1;
	TASK_CHECK(q.sizeOfQueue() == 1);
	TASK_CHECK(q.seqNrOfNextRtp() == 6);

	/* Timestamp equality only: crossing the 32 bit zero is a normal boundary. */
	if (push_pkt(q, kSsrcA, 7, false, 1.111f, 0xFFFFFFF0u)) return 1;
	if (push_pkt(q, kSsrcA, 8, false, 1.112f, 0x00000008u)) return 1;
	TASK_CHECK(q.sizeOfQueue() == 3);
	TASK_CHECK(q.expireFrameDeadlines(1.112f) == 0);
	return 0;
}

/*
* An expired complete frame is evicted while an adjacent, still fresh frame
* survives with all queue counters pointing at the survivors.
*/
static int frames_full_and_fresh_neighbor() {
	RtpQueue q;
	TASK_CHECK(q.setFrameDeadline(0.1f));

	/* Frame A, complete with marker. */
	if (push_pkt(q, kSsrcA, 10, false, 1.000f, 90000)) return 1;
	if (push_pkt(q, kSsrcA, 11, true, 1.000f, 90000)) return 1;
	/* Frame B, 50ms younger. */
	if (push_pkt(q, kSsrcA, 12, false, 1.050f, 93000)) return 1;
	if (push_pkt(q, kSsrcA, 13, true, 1.050f, 93000)) return 1;

	int freedBefore = task_packet_freed;
	TASK_CHECK(q.expireFrameDeadlines(1.101f) == 2);
	TASK_CHECK(task_packet_freed == freedBefore + 2);

	/* Only the fresh frame B remains. */
	TASK_CHECK(q.sizeOfQueue() == 2);
	TASK_CHECK(q.bytesInQueue() == 2 * kPktSize);
	TASK_CHECK(q.seqNrOfNextRtp() == 12);
	TASK_CHECK(q.seqNrOfLastRtp() == 13);
	TASK_CHECK(std::fabs(q.getDelay(1.101f) - 0.051f) < 0.001f);

	/* Exactly one frame expired, so the event fires once. */
	TASK_CHECK(q.consumeFrameDeadlineEvent());
	TASK_CHECK(!q.consumeFrameDeadlineEvent());

	/* The fresh frame is transmitted normally, ownership goes to the caller. */
	if (pop_pkt(q, 12)) return 1;
	if (pop_pkt(q, 13)) return 1;
	TASK_CHECK(q.sizeOfQueue() == 0);
	TASK_CHECK(task_packet_freed == freedBefore + 2);
	return 0;
}

/*
* Boundary semantics: the frame is evicted exactly when the age reaches the
* configured deadline, not before.
*/
static int frames_deadline_boundary() {
	RtpQueue q;
	TASK_CHECK(q.setFrameDeadline(0.1f));
	if (push_pkt(q, kSsrcA, 20, false, 1.000f, 90000)) return 1;

	TASK_CHECK(q.expireFrameDeadlines(1.0999f) == 0);
	TASK_CHECK(q.sizeOfQueue() == 1);
	TASK_CHECK(q.seqNrOfNextRtp() == 20);
	TASK_CHECK(!q.consumeFrameDeadlineEvent());

	TASK_CHECK(q.expireFrameDeadlines(1.1000f) == 1);
	TASK_CHECK(q.sizeOfQueue() == 0);
	TASK_CHECK(q.bytesInQueue() == 0);
	TASK_CHECK(q.seqNrOfNextRtp() == -1);
	TASK_CHECK(q.seqNrOfLastRtp() == -1);
	TASK_CHECK(q.consumeFrameDeadlineEvent());
	TASK_CHECK(!q.consumeFrameDeadlineEvent());
	return 0;
}

/*
* Eviction and rejection keep working after the ring buffer has wrapped.
*/
static int frames_ring_buffer_wrap() {
	RtpQueue q;
	TASK_CHECK(q.setFrameDeadline(0.1f));

	/* Drive head/tail around the 1024 slot ring several times. */
	for (int i = 0; i < 2500; i++) {
		unsigned short seq = (unsigned short)i;
		uint32_t stamp = 90000u + (uint32_t)(i / 2) * 3000u;
		bool marker = (i % 2 == 1);
		if (push_pkt(q, kSsrcA, seq, marker, 2.0f, stamp)) return 1;
		if (pop_pkt(q, seq)) return 1;
	}
	TASK_CHECK(q.sizeOfQueue() == 0);

	/* A frame pushed at a non-zero ring position is still fully evicted. */
	if (push_pkt(q, kSsrcA, 2500, false, 10.000f, 500000)) return 1;
	if (push_pkt(q, kSsrcA, 2501, false, 10.000f, 500000)) return 1;
	int freedBefore = task_packet_freed;
	TASK_CHECK(q.expireFrameDeadlines(10.101f) == 2);
	TASK_CHECK(task_packet_freed == freedBefore + 2);
	TASK_CHECK(q.sizeOfQueue() == 0);
	TASK_CHECK(q.seqNrOfNextRtp() == -1);
	TASK_CHECK(q.seqNrOfLastRtp() == -1);

	bool accepted = true;
	if (push_accepted(q, kSsrcA, 2502, false, 10.102f, 500000, accepted)) return 1;
	TASK_CHECK(!accepted);
	if (push_pkt(q, kSsrcA, 2503, true, 10.103f, 503000)) return 1;
	TASK_CHECK(q.sizeOfQueue() == 1);
	if (pop_pkt(q, 2503)) return 1;
	return 0;
}

/*
* Two complete frames that expire in the same sweep produce one event each,
* while a fresh third frame in the same queue is untouched.
*/
static int frames_two_expired_one_event_each() {
	RtpQueue q;
	TASK_CHECK(q.setFrameDeadline(0.1f));

	if (push_pkt(q, kSsrcA, 80, true, 1.000f, 90000)) return 1;   /* frame 1, age 0.101 */
	if (push_pkt(q, kSsrcA, 81, true, 0.995f, 93000)) return 1;   /* frame 2, age 0.106 */
	if (push_pkt(q, kSsrcA, 82, true, 1.050f, 96000)) return 1;   /* fresh,   age 0.051 */

	int freedBefore = task_packet_freed;
	TASK_CHECK(q.expireFrameDeadlines(1.101f) == 2);
	TASK_CHECK(task_packet_freed == freedBefore + 2);
	TASK_CHECK(q.sizeOfQueue() == 1);
	TASK_CHECK(q.seqNrOfNextRtp() == 82);

	/* One event per expired frame, none for the surviving fresh frame. */
	TASK_CHECK(q.consumeFrameDeadlineEvent());
	TASK_CHECK(q.consumeFrameDeadlineEvent());
	TASK_CHECK(!q.consumeFrameDeadlineEvent());

	/* Repeated sweeps must not re-signal the already reported frames. */
	TASK_CHECK(q.expireFrameDeadlines(1.120f) == 0);
	TASK_CHECK(!q.consumeFrameDeadlineEvent());
	if (pop_pkt(q, 82)) return 1;
	return 0;
}

// ============================ scheduler group ============================
/*
* Every enabled stream is swept at each isOkToTransmit before stream
* selection, not only the stream that would otherwise be prioritized.
*/
static int scheduler_sweeps_all_streams() {
	RtpQueue qA, qB;
	ScreamV2Tx tx;
	tx.enablePacketPacing(false);
	tx.registerNewStream(&qA, kSsrcA, 1.0f, 100000, 500000, 1000000, 10.0f);
	tx.registerNewStream(&qB, kSsrcB, 1.0f, 100000, 500000, 1000000, 10.0f);
	TASK_CHECK(tx.setFrameDeadline(kSsrcA, 0.1f));
	TASK_CHECK(tx.setFrameDeadline(kSsrcB, 0.1f));

	uint32_t selected = 0;
	TASK_CHECK(tx.isOkToTransmit(ntp(1.0f), selected) == -1.0f);

	if (push_pkt(qA, kSsrcA, 1, false, 1.000f, 90000)) return 1;
	if (push_pkt(qB, kSsrcB, 1, false, 1.000f, 90000)) return 1;

	/* Both frames expire in the same scheduler call, in both streams. */
	TASK_CHECK(tx.isOkToTransmit(ntp(1.101f), selected) == -1.0f);
	TASK_CHECK(qA.sizeOfQueue() == 0);
	TASK_CHECK(qB.sizeOfQueue() == 0);

	/* Each stream requests a refresh once, via the existing -1 signal. */
	TASK_CHECK(tx.getTargetBitrate(ntp(1.101f), kSsrcA) == -1.0f);
	TASK_CHECK(tx.getTargetBitrate(ntp(1.101f), kSsrcB) == -1.0f);
	TASK_CHECK(tx.getTargetBitrate(ntp(1.102f), kSsrcA) >= 0.0f);
	TASK_CHECK(tx.getTargetBitrate(ntp(1.102f), kSsrcB) >= 0.0f);

	/* Eviction is not reported as a network loss. */
	TASK_CHECK(!tx.isLossEpoch(kSsrcA));
	TASK_CHECK(!tx.isLossEpoch(kSsrcB));
	return 0;
}

/*
* A frame whose prefix was already sent and whose queue is empty still fires
* the deadline event, without touching bytes-in-flight or loss accounting.
*/
static int scheduler_prefix_sent_empty_queue() {
	RtpQueue q;
	ScreamV2Tx tx;
	tx.enablePacketPacing(false);
	tx.registerNewStream(&q, kSsrcA, 1.0f, 100000, 500000, 1000000, 10.0f);
	TASK_CHECK(tx.setFrameDeadline(kSsrcA, 0.1f));

	uint32_t selected = 0;
	TASK_CHECK(tx.isOkToTransmit(ntp(1.0f), selected) == -1.0f);

	if (push_pkt(q, kSsrcA, 30, false, 1.000f, 90000)) return 1;
	TASK_CHECK(tx.isOkToTransmit(ntp(1.0f), selected) == 0.0f);
	TASK_CHECK(selected == kSsrcA);
	if (pop_pkt(q, 30)) return 1;
	tx.addTransmitted(ntp(1.0f), kSsrcA, kPktSize, 30, false, 0.0f, 90000);
	TASK_CHECK(q.sizeOfQueue() == 0);

	int freedBefore = task_packet_freed;
	/* No queued packets, yet the active frame is past its deadline. */
	TASK_CHECK(tx.isOkToTransmit(ntp(1.101f), selected) == -1.0f);
	TASK_CHECK(task_packet_freed == freedBefore);
	TASK_CHECK(q.sizeOfQueue() == 0);

	/* Refresh requested even though nothing is queued. */
	TASK_CHECK(tx.getTargetBitrate(ntp(1.101f), kSsrcA) == -1.0f);
	TASK_CHECK(tx.getTargetBitrate(ntp(1.102f), kSsrcA) >= 0.0f);

	/* The already transmitted packet is not treated as lost. */
	TASK_CHECK(!tx.isLossEpoch(kSsrcA));

	/* A fresh frame can still be scheduled and transmitted. */
	if (push_pkt(q, kSsrcA, 31, false, 1.110f, 93000)) return 1;
	TASK_CHECK(tx.isOkToTransmit(ntp(1.110f), selected) == 0.0f);
	TASK_CHECK(selected == kSsrcA);
	if (pop_pkt(q, 31)) return 1;
	tx.addTransmitted(ntp(1.110f), kSsrcA, kPktSize, 31, false, 0.0f, 93000);
	return 0;
}

/*
* Scheduler-time eviction removes stale packets while a fresh neighboring
* frame keeps its transmission opportunity.
*/
static int scheduler_evict_stale_serve_fresh() {
	RtpQueue q;
	ScreamV2Tx tx;
	tx.enablePacketPacing(false);
	tx.registerNewStream(&q, kSsrcA, 1.0f, 100000, 500000, 1000000, 10.0f);
	TASK_CHECK(tx.setFrameDeadline(kSsrcA, 0.1f));

	uint32_t selected = 0;
	TASK_CHECK(tx.isOkToTransmit(ntp(1.0f), selected) == -1.0f);

	/* Stale frame, no marker. */
	if (push_pkt(q, kSsrcA, 40, false, 1.000f, 90000)) return 1;
	if (push_pkt(q, kSsrcA, 41, false, 1.000f, 90000)) return 1;
	/* Fresh adjacent frame. */
	if (push_pkt(q, kSsrcA, 42, false, 1.050f, 93000)) return 1;
	if (push_pkt(q, kSsrcA, 43, true, 1.050f, 93000)) return 1;

	int freedBefore = task_packet_freed;
	TASK_CHECK(tx.isOkToTransmit(ntp(1.101f), selected) == 0.0f);
	TASK_CHECK(selected == kSsrcA);
	TASK_CHECK(task_packet_freed == freedBefore + 2);
	TASK_CHECK(q.sizeOfQueue() == 2);
	TASK_CHECK(q.seqNrOfNextRtp() == 42);

	if (pop_pkt(q, 42)) return 1;
	tx.addTransmitted(ntp(1.101f), kSsrcA, kPktSize, 42, false, 0.0f, 93000);
	TASK_CHECK(tx.isOkToTransmit(ntp(1.102f), selected) == 0.0f);
	if (pop_pkt(q, 43)) return 1;
	tx.addTransmitted(ntp(1.102f), kSsrcA, kPktSize, 43, true, 0.0f, 93000);
	TASK_CHECK(q.sizeOfQueue() == 0);

	TASK_CHECK(tx.getTargetBitrate(ntp(1.102f), kSsrcA) == -1.0f);
	TASK_CHECK(tx.getTargetBitrate(ntp(1.103f), kSsrcA) >= 0.0f);
	TASK_CHECK(!tx.isLossEpoch(kSsrcA));
	return 0;
}

// ============================ compatibility group ============================

/*
* External RtpQueueIface implementations keep compiling and reject the
* capability; parameter validation and unknown SSRC are handled.
*/
static int compatibility_external_iface_and_validation() {
	ExternalQueue external;
	ScreamV2Tx tx;
	tx.enablePacketPacing(false);
	tx.registerNewStream(&external, kSsrcB, 1.0f, 100000, 500000, 1000000, 10.0f);

	/* Default implementations of the new optional capability. */
	TASK_CHECK(!external.frameDeadlineSupported());
	TASK_CHECK(!external.setFrameDeadline(0.1f));
	TASK_CHECK(external.expireFrameDeadlines(1.0f) == 0);
	TASK_CHECK(!external.consumeFrameDeadlineEvent());

	/* The TX refuses to enable a capability the queue does not support. */
	TASK_CHECK(!tx.setFrameDeadline(kSsrcB, 0.1f));

	RtpQueue q;
	tx.registerNewStream(&q, kSsrcA, 1.0f, 100000, 500000, 1000000, 10.0f);
	TASK_CHECK(q.frameDeadlineSupported());

	/* Invalid values are rejected and leave the configuration untouched. */
	TASK_CHECK(!tx.setFrameDeadline(kSsrcA, -1.0f));
	TASK_CHECK(!tx.setFrameDeadline(kSsrcA, 0.0f / 0.0f));      /* NaN */
	TASK_CHECK(!tx.setFrameDeadline(kSsrcA, 1.0f / 0.0f));      /* +inf */
	TASK_CHECK(!tx.setFrameDeadline(9999u, 0.1f));              /* unknown SSRC */
	TASK_CHECK(!q.setFrameDeadline(-0.5f));
	TASK_CHECK(!q.setFrameDeadline(0.0f / 0.0f));

	/* Still disabled: normal push/pop works and nothing is ever expired. */
	if (push_pkt(q, kSsrcA, 50, false, 1.0f, 90000)) return 1;
	TASK_CHECK(q.expireFrameDeadlines(100.0f) == 0);
	TASK_CHECK(!q.consumeFrameDeadlineEvent());
	if (pop_pkt(q, 50)) return 1;

	TASK_CHECK(tx.setFrameDeadline(kSsrcA, 0.1f));
	TASK_CHECK(tx.setFrameDeadline(kSsrcA, 0.0f)); /* disable again */
	return 0;
}

/*
* With the feature disabled the queue behaves exactly as before, including
* whole-queue clear, and re-disabling clears the rejection state.
*/
static int compatibility_disabled_strategy() {
	RtpQueue q;

	/* Default constructed: deadline disabled. */
	TASK_CHECK(!q.setFrameDeadline(-1.0f));
	if (push_pkt(q, kSsrcA, 60, false, 1.0f, 90000)) return 1;
	if (push_pkt(q, kSsrcA, 61, true, 1.0f, 90000)) return 1;
	TASK_CHECK(q.expireFrameDeadlines(5.0f) == 0);
	TASK_CHECK(q.sizeOfQueue() == 2);
	int freedBefore = task_packet_freed;
	TASK_CHECK(q.clear() == 2);
	TASK_CHECK(task_packet_freed == freedBefore + 2);
	TASK_CHECK(q.sizeOfQueue() == 0);
	TASK_CHECK(q.bytesInQueue() == 0);
	TASK_CHECK(q.seqNrOfNextRtp() == -1);
	TASK_CHECK(q.seqNrOfLastRtp() == -1);

	/* Enable, let a frame expire into the rejection state, then disable. */
	TASK_CHECK(q.setFrameDeadline(0.1f));
	if (push_pkt(q, kSsrcA, 62, false, 1.0f, 90000)) return 1;
	TASK_CHECK(q.expireFrameDeadlines(1.101f) == 1);
	bool accepted = true;
	if (push_accepted(q, kSsrcA, 63, false, 1.102f, 90000, accepted)) return 1;
	TASK_CHECK(!accepted);

	TASK_CHECK(q.setFrameDeadline(0.0f));
	if (push_pkt(q, kSsrcA, 64, false, 1.110f, 90000)) return 1;
	TASK_CHECK(q.sizeOfQueue() == 1);
	TASK_CHECK(q.seqNrOfNextRtp() == 64);
	TASK_CHECK(q.expireFrameDeadlines(99.0f) == 0);
	if (pop_pkt(q, 64)) return 1;
	return 0;
}

/*
* The original single-stream flow still works with the capability present but
* switched off, and the base-delay refresh clear keeps running.
*/
static int compatibility_legacy_flow() {
	RtpQueue q;
	ScreamV2Tx tx;
	tx.enablePacketPacing(false);
	tx.registerNewStream(&q, kSsrcA, 1.0f, 100000, 500000, 1000000, 10.0f);
	/* No setFrameDeadline call: the legacy whole-queue strategy stays active. */

	uint32_t selected = 0;
	TASK_CHECK(tx.isOkToTransmit(ntp(1.0f), selected) == -1.0f);
	if (push_pkt(q, kSsrcA, 70, false, 1.20f, 90000)) return 1;
	TASK_CHECK(tx.isOkToTransmit(ntp(1.2f), selected) == 0.0f);
	TASK_CHECK(selected == kSsrcA);
	if (pop_pkt(q, 70)) return 1;
	tx.addTransmitted(ntp(1.2f), kSsrcA, kPktSize, 70, false, 0.0f, 90000);

	if (push_pkt(q, kSsrcA, 71, true, 1.20f, 90000)) return 1;
	if (push_pkt(q, kSsrcA, 72, true, 1.24f, 93000)) return 1;
	TASK_CHECK(q.sizeOfQueue() == 2);
	TASK_CHECK(q.bytesInQueue() == 200);
	TASK_CHECK(std::fabs(q.getDelay(1.25f) - 0.05f) < 0.001f);
	TASK_CHECK(tx.isOkToTransmit(ntp(1.25f), selected) == 0.0f);
	TASK_CHECK(selected == kSsrcA);
	return 0;
}

const struct task_case task_cases[] = {
	{"frames", "suffix_across_empty_queue", frames_suffix_across_empty_queue},
	{"frames", "full_and_fresh_neighbor", frames_full_and_fresh_neighbor},
	{"frames", "deadline_boundary", frames_deadline_boundary},
	{"frames", "ring_buffer_wrap", frames_ring_buffer_wrap},
	{"frames", "two_expired_one_event_each", frames_two_expired_one_event_each},
	{"scheduler", "sweeps_all_streams", scheduler_sweeps_all_streams},
	{"scheduler", "prefix_sent_empty_queue", scheduler_prefix_sent_empty_queue},
	{"scheduler", "evict_stale_serve_fresh", scheduler_evict_stale_serve_fresh},
	{"compatibility", "external_iface_and_validation", compatibility_external_iface_and_validation},
	{"compatibility", "disabled_strategy", compatibility_disabled_strategy},
	{"compatibility", "legacy_flow", compatibility_legacy_flow},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
