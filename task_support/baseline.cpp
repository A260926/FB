#include "buffer_rcv.h"
#include <cstring>
#include <iostream>
#define MUST(x) do { if (!(x)) { std::cerr << "baseline assertion line " << __LINE__ << "\n"; return 1; } } while (0)
int main() {
    using namespace srt;
    using namespace srt::sync;
    CUnitQueue units(16, 1500);
    CRcvBuffer buffer(1000, 16, &units, true);
    const steady_clock::time_point base(microseconds_from(10000000));
    buffer.setTsbPdMode(base, false, microseconds_from(120000));
    CUnit* unit = units.getNextAvailUnit();
    MUST(unit != 0);
    CPacket& packet = unit->m_Packet;
    packet.set_seqno(1000); packet.set_timestamp(2300);
    packet.set_msgflags(PacketBoundaryBits(PB_SOLO) | MSGNO_PACKET_INORDER::wrap(1) | 1);
    const char payload[] = "stream-clock";
    packet.setLength(sizeof payload);
    std::memcpy(packet.data(), payload, sizeof payload);
    MUST(buffer.insert(unit) == 0);
    const steady_clock::time_point due = base + microseconds_from(122300);
    MUST(!buffer.isRcvDataReady(due - microseconds_from(1)));
    MUST(buffer.isRcvDataReady(due));
    const CRcvBuffer::PacketInfo info=buffer.getFirstValidPacketInfo();
    MUST(info.seqno == 1000 && !info.seq_gap && info.tsbpd_time == due);
    SRT_MSGCTRL ctrl = srt_msgctrl_default;
    char output[64] = {};
    MUST(buffer.readMessage(output, sizeof output, &ctrl) == sizeof payload);
    MUST(std::memcmp(output, payload, sizeof payload)==0);
    MUST(ctrl.srctime == 10122300);
    MUST(!buffer.hasAvailablePackets());
    std::cout << "SRT_PLAYOUT_BASELINE_OK payload=13 before_due=0 at_due=1 srctime=10122300\n";
    return 0;
}
