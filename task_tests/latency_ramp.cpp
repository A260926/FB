/*
 * Task tests for the locally scheduled SRT receive-latency ramp.
 *
 * These tests never open sockets, sleep or wait on the wall clock. Time is
 * driven manually through integer-microsecond steady-clock time points, and
 * the same internal schedule/ready entries used by production code are called
 * directly.
 */

// The production library is built with ENABLE_LOGGING=1, which guards data
// members of CUDT/CPacket and therefore changes their memory layout. The test
// must use the same ABI, so define it before including any SRT header. The
// force-included latency_curve.h only pulls in platform_sys.h/sync.h, whose
// layouts are unaffected by this macro.
#ifndef ENABLE_LOGGING
#define ENABLE_LOGGING 1
#endif

#include "../task_support/check.h"

#include <cstring>
#include <future>
#include <thread>
#include <vector>

#include "latency_curve.h"
#include "buffer_rcv.h"
#include "queue.h"
#include "packet.h"
#include "core.h"
#include "api.h"
#include "epoll.h"
#include "srt.h"

using namespace srt;
using namespace srt::sync;

namespace srt
{
// Test-only access to private CUDT / CUDTUnited members. Declared as a
// friend in core.h and api.h, so it must be a named class in namespace srt.
class LatencyRampTestAccess
{
public:
    static CUDTSocket* newRegisteredSocket()
    {
        CUDTSocket* s = NULL;
        CUDT::uglobal().newSocket(&s);
        return s;
    }

    static void destroyRegisteredSocket(CUDTSocket* s)
    {
        CUDTUnited& united = CUDT::uglobal();
        ExclusiveLock glock(united.m_GlobControlLock);
        united.m_Sockets.erase(s->m_SocketID);
        delete s;
    }

    static CUDT& core(CUDTSocket* s) { return s->core(); }

    static SRTSOCKET id(CUDTSocket* s) { return s->m_SocketID; }

    static void setConnected(CUDT& c, bool v) { c.m_bConnected = v; }
    static void setTsbPd(CUDT& c, bool v) { c.m_bTsbPd = v; }
    static void setMessageApi(CUDT& c, bool v) { c.m_config.bMessageAPI = v; }
    static void setDataSender(CUDT& c, bool v) { c.m_config.bDataSender = v; }
    static void setTsbPdOpt(CUDT& c, bool v) { c.m_config.bTSBPD = v; }

    static void attachRcvBuffer(CUDT& c, CRcvBuffer* b) { c.m_pRcvBuffer = b; }

    static bool scheduleRampAt(CUDT& c, const steady_clock::time_point& t0, int target_ms, int ramp_ms)
    {
        return c.scheduleLatencyRampAt(t0, target_ms, ramp_ms);
    }

    static bool isReadyAt(const CUDT& c, const steady_clock::time_point& now)
    {
        return c.isRcvBufferReadyAt(now);
    }

    static Mutex& recvLock(CUDT& c) { return c.m_RecvLock; }
    static Condition& tsbPdCond(CUDT& c) { return c.m_RcvTsbPdCond; }

    static int epollCreate() { return CUDT::epoll_create(); }
    // Subscribe without running CUDT::addEPoll(): the harness has no send
    // buffer, so the readiness probing in addEPoll() does not apply. The
    // watch is created in CEPoll and the eid registered on the socket, which
    // is all scheduleLatencyRampAt() needs to drive update_events().
    static int epollSubscribe(int eid, CUDT& c, int events)
    {
        const int rc = CUDT::uglobal().m_EPoll.update_usock(eid, c.m_SocketID, &events);
        c.m_sPollID.insert(eid);
        return rc;
    }
    static void epollUnsubscribe(int eid, CUDT& c)
    {
        c.m_sPollID.erase(eid);
    }
    static int epollRelease(int eid) { return CUDT::epoll_release(eid); }
    static int epollCheck(int eid, SRT_EPOLL_EVENT* out, int cap)
    {
        return CUDT::epoll_uwait(eid, out, cap, 0);
    }
};
} // namespace srt

namespace
{
    const int64_t US_PER_MS = 1000;
    const int64_t BASE_US = 10000000;
    const int64_t NEG_DELAY_US = 120000;

    steady_clock::time_point Tp(int64_t us)
    {
        return steady_clock::time_point(microseconds_from(us));
    }

    int64_t Us(const steady_clock::time_point& tp)
    {
        return count_microseconds(tp.time_since_epoch());
    }

    // Integer reference implementation of the curve, independent of the
    // production code, used to cross-check LatencyCurve.
    int64_t ref_latency(int64_t t, int64_t t0, int64_t L0, int64_t L1, int64_t R)
    {
        const int64_t x = t - t0;
        if (x <= 0)
            return L0;
        if (x >= R)
            return L1;
        // floor towards negative infinity
        int64_t q = (L1 - L0) * x / R;
        int64_t r = (L1 - L0) * x % R;
        if (r != 0 && r < 0)
            --q;
        return L0 + q;
    }

    // Reference first-due computation by integer scan over a bounded range.
    int64_t ref_first_due(int64_t B, int64_t t0, int64_t L0, int64_t L1, int64_t R, int64_t notBefore)
    {
        for (int64_t t = notBefore; t < notBefore + R + (L1 > L0 ? L1 : L0) + 10; ++t)
        {
            if (t - B >= ref_latency(t, t0, L0, L1, R))
                return t;
        }
        return INT64_MIN; // not found within the scanned window
    }

    // Builds a registered, connected-looking live/message receiver with a
    // real CRcvBuffer but without any network queues or threads.
    struct CoreHarness
    {
        CUDTSocket* sock;
        CUnitQueue* units;
        CRcvBuffer* buffer;

        CoreHarness(int bufSize = 32)
            : sock(LatencyRampTestAccess::newRegisteredSocket())
            , units(new CUnitQueue(bufSize, 1500))
            , buffer(NULL)
        {
            CUDT& c = LatencyRampTestAccess::core(sock);
            const int isn = 1000;
            buffer = new CRcvBuffer(isn, bufSize, units, true);
            // Enable real TSBPD on the buffer with the negotiated 120 ms delay.
            buffer->setTsbPdMode(Tp(BASE_US), false, microseconds_from(NEG_DELAY_US));
            LatencyRampTestAccess::attachRcvBuffer(c, buffer);
            LatencyRampTestAccess::setConnected(c, true);
            LatencyRampTestAccess::setTsbPd(c, true);
            LatencyRampTestAccess::setMessageApi(c, true);
            LatencyRampTestAccess::setDataSender(c, false);
            LatencyRampTestAccess::setTsbPdOpt(c, true);
        }

        ~CoreHarness()
        {
            // Detach the buffer so ~CUDT does not free it; this fixture owns it.
            LatencyRampTestAccess::attachRcvBuffer(LatencyRampTestAccess::core(sock), NULL);
            delete buffer;
            delete units;
            LatencyRampTestAccess::destroyRegisteredSocket(sock);
        }

        CUDT& core() { return LatencyRampTestAccess::core(sock); }
        SRTSOCKET id() { return LatencyRampTestAccess::id(sock); }
    };

    // Insert a SOLO in-order message packet into a receiver buffer.
    void insertSoloPacket(CRcvBuffer& buf, CUnitQueue& q, int32_t seqno, uint32_t ts,
                          int32_t msgno, const char* payload, size_t len)
    {
        CUnit* unit = q.getNextAvailUnit();
        CPacket& packet = unit->m_Packet;
        packet.set_seqno(seqno);
        packet.set_timestamp(ts);
        packet.set_msgflags(PacketBoundaryBits(PB_SOLO) | MSGNO_PACKET_INORDER::wrap(1) | msgno);
        packet.setLength(len);
        std::memcpy(packet.data(), payload, len);
        buf.insert(unit);
    }
} // namespace

// ---------------------------------------------------------------------------
// curve group: integer reference checks of the latency curve itself.
// ---------------------------------------------------------------------------

static int curve_increase_values()
{
    LatencyCurve curve;
    const int64_t t0 = 10000000, L0 = 100000, L1 = 300000, R = 1000000;
    TASK_CHECK(curve.schedule(Tp(t0), L0, L1, R));
    TASK_CHECK(curve.isActive());

    const int64_t probes[][2] = {
        {t0 - 1, L0},
        {t0, L0},
        {t0 + 500000, 200000},
        {t0 + 999999, 299999},
        {t0 + R, L1},
        {t0 + R + 12345, L1},
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i)
    {
        TASK_CHECK(curve.latencyAt(Tp(probes[i][0])) == probes[i][1]);
        TASK_CHECK(curve.latencyAt(Tp(probes[i][0])) == ref_latency(probes[i][0], t0, L0, L1, R));
    }
    return 0;
}

static int curve_decrease_negative_floor()
{
    LatencyCurve curve;
    const int64_t t0 = 5000000, L0 = 300000, L1 = 100000, R = 1000000;
    TASK_CHECK(curve.schedule(Tp(t0), L0, L1, R));

    // x = 333333: exact value is -66666.6, floor must be -66667 (towards -inf).
    const int64_t x = 333333;
    const int64_t expected = L0 + (-66667);
    TASK_CHECK(curve.latencyAt(Tp(t0 + x)) == expected);
    TASK_CHECK(curve.latencyAt(Tp(t0 + x)) == ref_latency(t0 + x, t0, L0, L1, R));

    // A few more points, all matched against the independent integer reference.
    for (int64_t x2 = 1; x2 < R; x2 += 139977)
    {
        TASK_CHECK(curve.latencyAt(Tp(t0 + x2)) == ref_latency(t0 + x2, t0, L0, L1, R));
    }
    TASK_CHECK(curve.latencyAt(Tp(t0 + R)) == L1);
    return 0;
}

static int curve_validation_and_preservation()
{
    LatencyCurve curve;
    const int64_t t0 = 1000000;

    // Target out of range.
    TASK_CHECK(!curve.schedule(Tp(t0), 1000, -1, 1000));
    TASK_CHECK(!curve.schedule(Tp(t0), 1000, 60000001, 1000));
    // Ramp must be strictly positive.
    TASK_CHECK(!curve.schedule(Tp(t0), 1000, 2000, 0));
    // Increase requires ramp strictly greater than the difference.
    TASK_CHECK(!curve.schedule(Tp(t0), 100000, 200000, 100000)); // equal -> reject
    TASK_CHECK(!curve.schedule(Tp(t0), 100000, 200000, 50000));  // shorter -> reject
    TASK_CHECK(!curve.isActive());

    // A valid increase is accepted.
    TASK_CHECK(curve.schedule(Tp(t0), 100000, 200000, 100001));
    TASK_CHECK(curve.isActive());
    const int64_t goodTarget = curve.latencyAt(Tp(t0 + 100001));

    // A rejected re-schedule must leave the previous curve untouched.
    TASK_CHECK(!curve.schedule(Tp(t0 + 50000), 0, 1, 0));
    TASK_CHECK(curve.isActive());
    TASK_CHECK(curve.latencyAt(Tp(t0 + 100001)) == goodTarget);

    // Decreasing ramps have no minimum-ramp constraint beyond being positive.
    LatencyCurve down;
    TASK_CHECK(down.schedule(Tp(t0), 200000, 0, 1));
    TASK_CHECK(down.latencyAt(Tp(t0 + 1)) == 0);
    return 0;
}

static int curve_reschedule_continuity()
{
    LatencyCurve curve;
    const int64_t t0 = 10000000, L0 = 100000, L1a = 200000, Ra = 1000000;
    TASK_CHECK(curve.schedule(Tp(t0), L0, L1a, Ra));

    // Halfway through, the latency in effect is 140000.
    const int64_t t1 = t0 + 400000;
    TASK_CHECK(curve.latencyAt(Tp(t1)) == 140000);

    // Re-target from the live value: it must continue from 140000, neither
    // jumping to 200000 nor restarting from the original 100000.
    const int64_t L1b = 400000, Rb = 2000000;
    TASK_CHECK(curve.schedule(Tp(t1), curve.latencyAt(Tp(t1)), L1b, Rb));
    TASK_CHECK(curve.latencyAt(Tp(t1)) == 140000);
    TASK_CHECK(curve.latencyAt(Tp(t1 - 1)) == 140000); // clamped to the new start value
    TASK_CHECK(curve.latencyAt(Tp(t1 + Rb)) == L1b);
    TASK_CHECK(curve.latencyAt(Tp(t1 + Rb / 2)) == 140000 + (L1b - 140000) / 2);
    return 0;
}

static int curve_first_due_increase()
{
    LatencyCurve curve;
    const int64_t t0 = 1000000, L0 = 100000, L1 = 300000, R = 1000000;
    TASK_CHECK(curve.schedule(Tp(t0), L0, L1, R));

    const int64_t B = t0 - 40000; // packet source time, well before the ramp
    const int64_t notBefore = t0;
    const int64_t due = curve.firstDueUs(B, notBefore);
    TASK_CHECK(due == ref_first_due(B, t0, L0, L1, R, notBefore));
    // The returned time is the first satisfying integer microsecond.
    TASK_CHECK((due - B) >= curve.latencyAt(Tp(due)));
    TASK_CHECK(due == notBefore || (due - 1 - B) < curve.latencyAt(Tp(due - 1)));

    // Already due at the reference time: reported immediately at notBefore.
    const int64_t oldB = t0 - L0 - 5;
    TASK_CHECK(curve.firstDueUs(oldB, notBefore) == notBefore);
    return 0;
}

static int curve_first_due_decrease()
{
    LatencyCurve curve;
    const int64_t t0 = 2000000, L0 = 400000, L1 = 100000, R = 1000000;
    TASK_CHECK(curve.schedule(Tp(t0), L0, L1, R));

    const int64_t B = t0 + 10000;
    const int64_t notBefore = t0;
    const int64_t due = curve.firstDueUs(B, notBefore);
    TASK_CHECK(due == ref_first_due(B, t0, L0, L1, R, notBefore));
    TASK_CHECK((due - B) >= curve.latencyAt(Tp(due)));
    TASK_CHECK(due == notBefore || (due - 1 - B) < curve.latencyAt(Tp(due - 1)));
    // Decreasing latency must deliver no later than the original fixed delay.
    TASK_CHECK(due <= B + L0);
    return 0;
}

static int curve_reset_and_inactive()
{
    LatencyCurve curve;
    TASK_CHECK(!curve.isActive());
    const int64_t B = 123456, notBefore = 2000000;
    // Without a schedule the due time is simply the source time clamped.
    TASK_CHECK(curve.firstDueUs(B, notBefore) == notBefore);
    TASK_CHECK(curve.firstDueUs(B + 5000000, notBefore) == B + 5000000);
    TASK_CHECK(curve.latencyAt(Tp(notBefore)) == 0);

    TASK_CHECK(curve.schedule(Tp(notBefore), 1000, 2000, 2000));
    TASK_CHECK(curve.isActive());
    curve.reset();
    TASK_CHECK(!curve.isActive());
    TASK_CHECK(curve.latencyAt(Tp(notBefore)) == 0);
    TASK_CHECK(curve.firstDueUs(B, notBefore) == notBefore);
    return 0;
}

// ---------------------------------------------------------------------------
// delivery group: real CUnitQueue/CPacket/CRcvBuffer playout behaviour.
// ---------------------------------------------------------------------------

namespace
{
    struct BufferHarness
    {
        CUnitQueue units;
        CRcvBuffer buffer;
        steady_clock::time_point base;

        BufferHarness()
            : units(32, 1500)
            , buffer(1000, 32, &units, true)
            , base(Tp(BASE_US))
        {
            buffer.setTsbPdMode(base, false, microseconds_from(NEG_DELAY_US));
        }
    };
}

static int delivery_baseline_unchanged_without_ramp()
{
    BufferHarness h;
    const char payload[] = "stream-clock";
    insertSoloPacket(h.buffer, h.units, 1000, 2300, 1, payload, sizeof payload);

    // No ramp scheduled: the negotiated playout time is preserved exactly.
    TASK_CHECK(!h.buffer.isLatencyRampActive());
    const steady_clock::time_point due = Tp(BASE_US + NEG_DELAY_US + 2300);
    TASK_CHECK(!h.buffer.isRcvDataReady(due - microseconds_from(1)));
    TASK_CHECK(h.buffer.isRcvDataReady(due));

    const CRcvBuffer::PacketInfo info = h.buffer.getFirstValidPacketInfoAt(due);
    TASK_CHECK(info.seqno == 1000 && !info.seq_gap && info.tsbpd_time == due);
    return 0;
}

static int delivery_increase_delays_ready_and_keeps_srctime()
{
    BufferHarness h;
    const char payload[] = "ramp-up-payload";
    insertSoloPacket(h.buffer, h.units, 1000, 2300, 7, payload, sizeof payload);

    const int64_t B = BASE_US + 2300;
    const int64_t t0 = BASE_US + 50000;
    // 120ms -> 200ms over 1s: difference 80ms, ramp 1000ms > 80ms.
    TASK_CHECK(h.buffer.scheduleLatencyRamp(Tp(t0), 200 * US_PER_MS, 1000 * US_PER_MS));
    TASK_CHECK(h.buffer.isLatencyRampActive());

    // The negotiated deadline is no longer the effective one.
    const int64_t negotiatedDue = BASE_US + NEG_DELAY_US + 2300;
    TASK_CHECK(!h.buffer.isRcvDataReady(Tp(negotiatedDue)));

    // The effective due time is the curve solution, queried through the same
    // PacketInfo entry used by the TSBPD thread and recv.
    const CRcvBuffer::PacketInfo info = h.buffer.getFirstValidPacketInfoAt(Tp(t0));
    const int64_t due = Us(info.tsbpd_time);
    TASK_CHECK(due > negotiatedDue);
    TASK_CHECK(!h.buffer.isRcvDataReady(Tp(due - 1)));
    TASK_CHECK(h.buffer.isRcvDataReady(Tp(due)));

    // Payload, sequence and message number survive; srctime keeps the fixed
    // negotiated TSBPD meaning and is not shifted by the ramp.
    SRT_MSGCTRL ctrl = srt_msgctrl_default;
    char output[64] = {};
    TASK_CHECK(h.buffer.readMessage(output, sizeof output, &ctrl) == (int) sizeof payload);
    TASK_CHECK(std::memcmp(output, payload, sizeof payload) == 0);
    TASK_CHECK(ctrl.msgno == 7);
    TASK_CHECK(ctrl.pktseq == 1000);
    TASK_CHECK(ctrl.srctime == B + NEG_DELAY_US);
    TASK_CHECK(!h.buffer.hasAvailablePackets());
    return 0;
}

static int delivery_decrease_advances_ready()
{
    BufferHarness h;
    const char payload[] = "ramp-down";
    insertSoloPacket(h.buffer, h.units, 1000, 2300, 2, payload, sizeof payload);

    const int64_t t0 = BASE_US + 50000;
    // 120ms -> 40ms over 100ms.
    TASK_CHECK(h.buffer.scheduleLatencyRamp(Tp(t0), 40 * US_PER_MS, 100 * US_PER_MS));

    const int64_t negotiatedDue = BASE_US + NEG_DELAY_US + 2300;
    const CRcvBuffer::PacketInfo info = h.buffer.getFirstValidPacketInfoAt(Tp(t0));
    const int64_t due = Us(info.tsbpd_time);
    TASK_CHECK(due < negotiatedDue);
    TASK_CHECK(!h.buffer.isRcvDataReady(Tp(due - 1)));
    TASK_CHECK(h.buffer.isRcvDataReady(Tp(due)));
    return 0;
}

static int delivery_followup_message_and_no_duplication()
{
    BufferHarness h;
    const char p1[] = "first-message";
    const char p2[] = "second-message-body";
    insertSoloPacket(h.buffer, h.units, 1000, 1000, 11, p1, sizeof p1);
    insertSoloPacket(h.buffer, h.units, 1001, 2000, 12, p2, sizeof p2);

    const int64_t t0 = BASE_US;
    TASK_CHECK(h.buffer.scheduleLatencyRamp(Tp(t0), 300 * US_PER_MS, 1000 * US_PER_MS));

    // Exactly one queue, two packets: the ramp must not clone entries.
    TASK_CHECK(h.buffer.size() == 2);
    const int64_t due1 = Us(h.buffer.getFirstValidPacketInfoAt(Tp(t0)).tsbpd_time);

    SRT_MSGCTRL ctrl = srt_msgctrl_default;
    char out[64] = {};
    TASK_CHECK(!h.buffer.isRcvDataReady(Tp(due1 - 1)));
    TASK_CHECK(h.buffer.isRcvDataReady(Tp(due1)));
    TASK_CHECK(h.buffer.readMessage(out, sizeof out, &ctrl) == (int) sizeof p1);
    TASK_CHECK(std::memcmp(out, p1, sizeof p1) == 0);
    TASK_CHECK(ctrl.msgno == 11);

    // The second message is independently re-evaluated under the same ramp.
    TASK_CHECK(h.buffer.size() == 1);
    const int64_t due2 = Us(h.buffer.getFirstValidPacketInfoAt(Tp(due1)).tsbpd_time);
    TASK_CHECK(!h.buffer.isRcvDataReady(Tp(due2 - 1)));
    TASK_CHECK(h.buffer.isRcvDataReady(Tp(due2)));
    char out2[64] = {};
    SRT_MSGCTRL ctrl2 = srt_msgctrl_default;
    TASK_CHECK(h.buffer.readMessage(out2, sizeof out2, &ctrl2) == (int) sizeof p2);
    TASK_CHECK(std::memcmp(out2, p2, sizeof p2) == 0);
    TASK_CHECK(ctrl2.msgno == 12 && ctrl2.pktseq == 1001);
    TASK_CHECK(!h.buffer.hasAvailablePackets());
    return 0;
}

static int delivery_queries_do_not_advance_ramp()
{
    BufferHarness h;
    const char payload[] = "query-only";
    insertSoloPacket(h.buffer, h.units, 1000, 2300, 3, payload, sizeof payload);

    const int64_t t0 = BASE_US;
    TASK_CHECK(h.buffer.scheduleLatencyRamp(Tp(t0), 500 * US_PER_MS, 1000 * US_PER_MS));

    // Repeated queries, including for times in the future, must not move the
    // curve or consume the packet. A future query clamps an already-due packet
    // to the query time, so compare two queries at the same reference time.
    const int64_t future = t0 + 4 * 1000 * US_PER_MS;
    for (int i = 0; i < 5; ++i)
    {
        h.buffer.getFirstValidPacketInfoAt(Tp(future));
        h.buffer.isRcvDataReady(Tp(future));
    }
    TASK_CHECK(h.buffer.hasAvailablePackets());
    TASK_CHECK(h.buffer.isLatencyRampActive());

    const int64_t dueAtStart = Us(h.buffer.getFirstValidPacketInfoAt(Tp(t0)).tsbpd_time);
    const int64_t negotiatedDue = BASE_US + NEG_DELAY_US + 2300;
    // Idempotent: querying again at t0 yields the same first due time.
    TASK_CHECK(Us(h.buffer.getFirstValidPacketInfoAt(Tp(t0)).tsbpd_time) == dueAtStart);
    // The ramp pushes delivery strictly past the negotiated playout time.
    TASK_CHECK(dueAtStart > negotiatedDue);
    TASK_CHECK(!h.buffer.isRcvDataReady(Tp(dueAtStart - 1)));
    TASK_CHECK(h.buffer.isRcvDataReady(Tp(dueAtStart)));
    return 0;
}

static int delivery_drop_all_clears_ramp()
{
    BufferHarness h;
    const char payload[] = "to-be-dropped";
    insertSoloPacket(h.buffer, h.units, 1000, 2300, 4, payload, sizeof payload);

    TASK_CHECK(h.buffer.scheduleLatencyRamp(Tp(BASE_US), 300 * US_PER_MS, 1000 * US_PER_MS));
    TASK_CHECK(h.buffer.isLatencyRampActive());
    TASK_CHECK(h.buffer.dropAll() == 1);
    TASK_CHECK(!h.buffer.isLatencyRampActive());
    TASK_CHECK(h.buffer.empty());

    // A fresh packet after the clear follows the plain negotiated delay again.
    insertSoloPacket(h.buffer, h.units, 1001, 2300, 5, payload, sizeof payload);
    const steady_clock::time_point negotiatedDue = Tp(BASE_US + NEG_DELAY_US + 2300);
    TASK_CHECK(!h.buffer.isRcvDataReady(negotiatedDue - microseconds_from(1)));
    TASK_CHECK(h.buffer.isRcvDataReady(negotiatedDue));
    return 0;
}

static int delivery_timestamp_wrap()
{
    CUnitQueue units(32, 1500);
    CRcvBuffer buffer(1000, 32, &units, true);
    const steady_clock::time_point base = Tp(BASE_US);
    buffer.setTsbPdMode(base, false, microseconds_from(NEG_DELAY_US));

    const uint32_t MAXTS = CPacket::MAX_TIMESTAMP;
    const uint32_t nearWrap = MAXTS - 10 * 1000000u; // inside the last 30s -> opens wrap check
    buffer.updateTsbPdTimeBase(nearWrap);

    // A wrapped, small timestamp after the carryover point.
    const uint32_t wrappedTs = 5 * 1000000u;
    CUnit* unit = units.getNextAvailUnit();
    CPacket& packet = unit->m_Packet;
    packet.set_seqno(1000);
    packet.set_timestamp(wrappedTs);
    packet.set_msgflags(PacketBoundaryBits(PB_SOLO) | MSGNO_PACKET_INORDER::wrap(1) | 1);
    const char payload[] = "wrapped";
    packet.setLength(sizeof payload);
    buffer.insert(unit);

    // B includes the full 2^32 carryover segment.
    const int64_t segment = int64_t(MAXTS) + 1;
    const int64_t B = BASE_US + segment + wrappedTs;
    const int64_t t0 = BASE_US;

    // Without a ramp the due time is B plus the negotiated delay.
    const CRcvBuffer::PacketInfo plain = buffer.getFirstValidPacketInfoAt(Tp(t0));
    TASK_CHECK(Us(plain.tsbpd_time) == B + NEG_DELAY_US);

    // With a ramp the due time is the curve solution evaluated on the wrapped B.
    TASK_CHECK(buffer.scheduleLatencyRamp(Tp(t0), 250 * US_PER_MS, 1000 * US_PER_MS));
    const CRcvBuffer::PacketInfo ramped = buffer.getFirstValidPacketInfoAt(Tp(t0));
    const int64_t due = Us(ramped.tsbpd_time);
    TASK_CHECK(due > B + NEG_DELAY_US);
    TASK_CHECK(!buffer.isRcvDataReady(Tp(due - 1)));
    TASK_CHECK(buffer.isRcvDataReady(Tp(due)));
    return 0;
}

static int delivery_real_drift_settlement()
{
    CUnitQueue units(32, 1500);
    CRcvBuffer buffer(1000, 32, &units, true);
    const steady_clock::time_point base = Tp(BASE_US);
    buffer.setTsbPdMode(base, false, microseconds_from(NEG_DELAY_US));

    const uint32_t ts = 40000u;
    const char payload[] = "drift";
    insertSoloPacket(buffer, units, 1000, ts, 9, payload, sizeof payload);

    // Install a ramp first: drift handling must neither clear it nor fold the
    // buffering delay into the drift estimate.
    TASK_CHECK(buffer.scheduleLatencyRamp(Tp(BASE_US), 300 * US_PER_MS, 1000 * US_PER_MS));
    const steady_clock::time_point baseBefore = buffer.getTsbPdTimeBase(ts);

    bool settled = false;
    // 1000 ACKACK samples with a constant +6000us drift cross the 5000us
    // threshold and shift the TSBPD base by one overdrift step.
    for (int i = 0; i < 1000; ++i)
    {
        const steady_clock::time_point pktBase = buffer.getTsbPdTimeBase(ts) + microseconds_from(ts);
        const steady_clock::time_point arrival = pktBase + microseconds_from(6000);
        if (buffer.addRcvTsbPdDriftSample(ts, arrival, 100))
            settled = true;
    }
    TASK_CHECK(settled);
    TASK_CHECK(buffer.isLatencyRampActive());

    // The base moved forward by exactly one 5000us overdrift step and the
    // residual average drift is 6000 - 5000 = 1000us. Buffering delay never
    // enters this estimate.
    const steady_clock::time_point baseAfter = buffer.getTsbPdTimeBase(ts);
    TASK_CHECK(count_microseconds(baseAfter - baseBefore) == 5000);
    TASK_CHECK(buffer.getDrift() == 1000);

    // The packet's due time follows the shifted source time through the
    // unchanged ramp, with a clean first-due integer boundary.
    const CRcvBuffer::PacketInfo info = buffer.getFirstValidPacketInfoAt(Tp(BASE_US));
    const int64_t due = Us(info.tsbpd_time);
    TASK_CHECK(due > BASE_US + ts + NEG_DELAY_US);
    TASK_CHECK(!buffer.isRcvDataReady(Tp(due - 1)));
    TASK_CHECK(buffer.isRcvDataReady(Tp(due)));
    return 0;
}

// ---------------------------------------------------------------------------
// integration group: the same CUDT schedule/ready coordination entry, plus
// wakeup, epoll event updates and C-entry delegation.
// ---------------------------------------------------------------------------

static int integration_coord_ready_consistency()
{
    CoreHarness h;
    const char payload[] = "core-coord";
    insertSoloPacket(*h.buffer, *h.units, 1000, 2300, 1, payload, sizeof payload);

    const int64_t t0 = BASE_US + 50000;
    // Drive the production coordination entry with an explicit monotonic time.
    TASK_CHECK(LatencyRampTestAccess::scheduleRampAt(h.core(),Tp(t0), 200, 1000));
    TASK_CHECK(h.buffer->isLatencyRampActive());

    const int64_t due = Us(h.buffer->getFirstValidPacketInfoAt(Tp(t0)).tsbpd_time);
    // The CUDT-level ready helper must agree with the buffer-level decision.
    TASK_CHECK(LatencyRampTestAccess::isReadyAt(h.core(),Tp(due - 1)) == h.buffer->isRcvDataReady(Tp(due - 1)));
    TASK_CHECK(!LatencyRampTestAccess::isReadyAt(h.core(),Tp(due - 1)));
    TASK_CHECK(LatencyRampTestAccess::isReadyAt(h.core(),Tp(due)));
    TASK_CHECK(h.buffer->isRcvDataReady(Tp(due)));
    return 0;
}

static int integration_rejects_keep_state()
{
    CoreHarness h;
    const char payload[] = "preserve-me";
    insertSoloPacket(*h.buffer, *h.units, 1000, 2300, 1, payload, sizeof payload);

    // Bad parameters.
    TASK_CHECK(!LatencyRampTestAccess::scheduleRampAt(h.core(),Tp(BASE_US), -1, 1000));
    TASK_CHECK(!LatencyRampTestAccess::scheduleRampAt(h.core(),Tp(BASE_US), 60001, 1000));
    TASK_CHECK(!LatencyRampTestAccess::scheduleRampAt(h.core(),Tp(BASE_US), 200, 0));
    TASK_CHECK(!h.buffer->isLatencyRampActive());

    // Unsupported socket states, each restored before the next check.
    LatencyRampTestAccess::setConnected(h.core(), false);
    TASK_CHECK(!LatencyRampTestAccess::scheduleRampAt(h.core(),Tp(BASE_US), 200, 1000));
    LatencyRampTestAccess::setConnected(h.core(), true);

    LatencyRampTestAccess::setTsbPd(h.core(), false);
    TASK_CHECK(!LatencyRampTestAccess::scheduleRampAt(h.core(),Tp(BASE_US), 200, 1000));
    LatencyRampTestAccess::setTsbPd(h.core(), true);

    LatencyRampTestAccess::setMessageApi(h.core(), false); // file/stream socket
    TASK_CHECK(!LatencyRampTestAccess::scheduleRampAt(h.core(),Tp(BASE_US), 200, 1000));
    LatencyRampTestAccess::setMessageApi(h.core(), true);

    // Nothing was installed: the packet still plays at the negotiated time.
    TASK_CHECK(!h.buffer->isLatencyRampActive());
    const int64_t negotiatedDue = BASE_US + NEG_DELAY_US + 2300;
    TASK_CHECK(LatencyRampTestAccess::isReadyAt(h.core(),Tp(negotiatedDue)));
    return 0;
}

static int integration_wakes_waiting_receiver()
{
    CoreHarness h;
    const char payload[] = "wake-up";
    insertSoloPacket(*h.buffer, *h.units, 1000, 2300, 1, payload, sizeof payload);

    Mutex& mtx = LatencyRampTestAccess::recvLock(h.core());
    Condition& cond = LatencyRampTestAccess::tsbPdCond(h.core());

    // Local handshake mutex/CV (not the SRT ones) makes "waiter is parked"
    // observable without spinning or risking a lost wakeup.
    std::mutex parkMtx;
    std::condition_variable parkCv;
    bool parked = false;
    bool done = false;

    std::promise<bool> signaled;
    std::future<bool> future = signaled.get_future();
    std::thread waiter([&]() {
        UniqueLock guard(mtx);
        {
            std::lock_guard<std::mutex> pg(parkMtx);
            parked = true;
        }
        parkCv.notify_all();

        const steady_clock::time_point deadline = steady_clock::now() + seconds_from(5);
        // Returns true only on the schedule's notification (no timeout expected).
        const bool woke = cond.wait_until(guard, deadline);
        done = true;
        signaled.set_value(woke);
    });

    // Block until the waiter has marked itself parked (it still holds mtx and
    // is about to enter cond.wait_until). The schedule then takes mtx, which
    // cannot complete until the waiter has actually released it inside wait.
    {
        std::unique_lock<std::mutex> pg(parkMtx);
        parkCv.wait(pg, [&] { return parked; });
    }

    TASK_CHECK(LatencyRampTestAccess::scheduleRampAt(h.core(),Tp(BASE_US + 50000), 200, 1000));
    TASK_CHECK(future.wait_for(std::chrono::milliseconds(2000)) == std::future_status::ready);
    TASK_CHECK(future.get());
    TASK_CHECK(done);
    waiter.join();
    return 0;
}

static int integration_epoll_event_updates()
{
    CoreHarness h;
    const char payload[] = "epoll-event";
    // Packet source time B = BASE + 2300.
    insertSoloPacket(*h.buffer, *h.units, 1000, 2300, 1, payload, sizeof payload);

    const int eid = LatencyRampTestAccess::epollCreate();
    TASK_CHECK(eid >= 0);
    TASK_CHECK(LatencyRampTestAccess::epollSubscribe(eid, h.core(), SRT_EPOLL_IN) == 0);

    SRT_EPOLL_EVENT ev[2];

    // Before the packet is due, an increase must not leave a stale readable
    // event: the head is not ready and EPOLL_IN stays absent.
    const int64_t tEarly = BASE_US + 1000; // before B, nothing can be ready
    TASK_CHECK(LatencyRampTestAccess::scheduleRampAt(h.core(),Tp(tEarly), 400, 1000));
    TASK_CHECK(!LatencyRampTestAccess::isReadyAt(h.core(),Tp(tEarly)));
    TASK_CHECK(LatencyRampTestAccess::epollCheck(eid, ev, 2) == 0);

    // A later packet deadline under the increased latency confirms the event is
    // still withheld until the new due time is reached.
    const int64_t increasedDue = Us(h.buffer->getFirstValidPacketInfoAt(Tp(tEarly)).tsbpd_time);
    TASK_CHECK(increasedDue > BASE_US + NEG_DELAY_US + 2300);
    TASK_CHECK(LatencyRampTestAccess::epollCheck(eid, ev, 2) == 0);

    // Drop the latency to zero at a time at which the packet is already due
    // under the new schedule: the readable event must now appear.
    const int64_t tLate = BASE_US + 500000;
    TASK_CHECK(LatencyRampTestAccess::scheduleRampAt(h.core(),Tp(tLate), 0, 1));
    TASK_CHECK(LatencyRampTestAccess::isReadyAt(h.core(),Tp(tLate)));
    TASK_CHECK(LatencyRampTestAccess::epollCheck(eid, ev, 2) == 1);
    TASK_CHECK(ev[0].fd == h.id() && (ev[0].events & SRT_EPOLL_IN));

    LatencyRampTestAccess::epollUnsubscribe(eid, h.core());
    TASK_CHECK(LatencyRampTestAccess::epollRelease(eid) == 0);
    return 0;
}

static int integration_c_entry_delegation()
{
    CoreHarness h;
    const char payload[] = "c-entry";
    insertSoloPacket(*h.buffer, *h.units, 1000, 2300, 1, payload, sizeof payload);

    // Valid connected TSBPD receiver: the C entry delegates to the same
    // internal coordination using the real steady clock.
    TASK_CHECK(srt_schedule_rcv_latency(h.id(), 200, 1000) == 0);
    TASK_CHECK(h.buffer->isLatencyRampActive());

    // Invalid socket id.
    TASK_CHECK(srt_schedule_rcv_latency(SRTSOCKET(999999), 200, 1000) == SRT_ERROR);

    // Invalid parameters on an otherwise valid socket.
    TASK_CHECK(srt_schedule_rcv_latency(h.id(), -1, 1000) == SRT_ERROR);
    TASK_CHECK(srt_schedule_rcv_latency(h.id(), 200, 0) == SRT_ERROR);
    // Increase with a ramp not strictly greater than the difference.
    TASK_CHECK(srt_schedule_rcv_latency(h.id(), 60000, 1) == SRT_ERROR);

    // Sender-only / non-message socket is rejected through the C entry as well.
    LatencyRampTestAccess::setMessageApi(h.core(), false);
    TASK_CHECK(srt_schedule_rcv_latency(h.id(), 200, 1000) == SRT_ERROR);
    LatencyRampTestAccess::setMessageApi(h.core(), true);
    return 0;
}

// ---------------------------------------------------------------------------
// Registration.
// ---------------------------------------------------------------------------

const task_case task_cases[] = {
    // curve
    {"curve", "increase_values", curve_increase_values},
    {"curve", "decrease_negative_floor", curve_decrease_negative_floor},
    {"curve", "validation_and_preservation", curve_validation_and_preservation},
    {"curve", "reschedule_continuity", curve_reschedule_continuity},
    {"curve", "first_due_increase", curve_first_due_increase},
    {"curve", "first_due_decrease", curve_first_due_decrease},
    {"curve", "reset_and_inactive", curve_reset_and_inactive},
    // delivery
    {"delivery", "baseline_unchanged_without_ramp", delivery_baseline_unchanged_without_ramp},
    {"delivery", "increase_delays_ready_and_keeps_srctime", delivery_increase_delays_ready_and_keeps_srctime},
    {"delivery", "decrease_advances_ready", delivery_decrease_advances_ready},
    {"delivery", "followup_message_and_no_duplication", delivery_followup_message_and_no_duplication},
    {"delivery", "queries_do_not_advance_ramp", delivery_queries_do_not_advance_ramp},
    {"delivery", "drop_all_clears_ramp", delivery_drop_all_clears_ramp},
    {"delivery", "timestamp_wrap", delivery_timestamp_wrap},
    {"delivery", "real_drift_settlement", delivery_real_drift_settlement},
    // integration
    {"integration", "coord_ready_consistency", integration_coord_ready_consistency},
    {"integration", "rejects_keep_state", integration_rejects_keep_state},
    {"integration", "wakes_waiting_receiver", integration_wakes_waiting_receiver},
    {"integration", "epoll_event_updates", integration_epoll_event_updates},
    {"integration", "c_entry_delegation", integration_c_entry_delegation},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
