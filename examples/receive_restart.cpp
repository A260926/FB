/* Receive restart example.
 *
 * Demonstrates how uvgRTP recovers the first frame of a new RTP sequence generation when the sender
 * process restarts but keeps the same SSRC. The example replays captured RTP datagrams from memory:
 * it uses the production RTP parser / dispatcher / media / RTCP path, but no sockets and no receive
 * threads, so it is deterministic and fast.
 *
 * Build (see task_support/run.sh): linked against the compiled receive_restart.cc object and the
 * uvgrtp library. Run with no arguments.
 */

#include "receive_restart.hh"
#include "rtcp_internal.hh"

#include "uvgrtp/frame.hh"
#include "uvgrtp/util.hh"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

    constexpr uint32_t SSRC = 0xC0FFEEu;
    constexpr uint32_t TS_OLD = 1000;
    constexpr uint32_t TS_NEW = 2000;

    std::vector<uint8_t> make_packet(uint16_t seq, uint32_t ts, uint8_t nal_header,
        const std::vector<uint8_t>& body)
    {
        std::vector<uint8_t> p(12, 0);
        p[0] = 0x80;                                  /* RTP version 2 */
        p[1] = 96;                                    /* dynamic payload type */
        p[2] = (uint8_t)(seq >> 8);
        p[3] = (uint8_t)seq;
        for (int i = 0; i < 4; ++i) {
            p[4 + i] = (uint8_t)(ts >> (24 - 8 * i));
            p[8 + i] = (uint8_t)(SSRC >> (24 - 8 * i));
        }
        p.push_back(nal_header);
        p.insert(p.end(), body.begin(), body.end());
        return p;
    }

    void replay(uvgrtp::receive_restart_pipeline& pipeline, const std::vector<uint8_t>& packet)
    {
        (void)pipeline.feed(packet.data(), packet.size());
    }

    void print_frame(const char* label, uvgrtp::frame::rtp_frame* frame)
    {
        if (frame == nullptr) {
            std::printf("%s: <none>\n", label);
            return;
        }
        std::printf("%s: seq=%u ts=%u payload=", label, frame->header.seq, frame->header.timestamp);
        for (size_t i = 0; i < frame->payload_len; ++i) {
            std::printf("%c", (frame->payload[i] >= 32 && frame->payload[i] < 127)
                ? frame->payload[i] : '.');
        }
        std::printf("\n");
        (void)uvgrtp::frame::dealloc_frame(frame);
    }
}

int main()
{
    uvgrtp::receive_restart_pipeline pipeline;
    int stream = pipeline.add_stream(RTP_FORMAT_H264, 0x1234u, SSRC, RCE_RTCP);

    /* Must be configured before the first RTP packet. */
    rtp_error_t ret = pipeline.configure_receive_restart((size_t)stream, true);
    if (ret != RTP_OK) {
        std::printf("configure_receive_restart failed: %d\n", (int)ret);
        return 1;
    }

    /* Old generation: two frames are delivered normally. */
    replay(pipeline, make_packet(100, TS_OLD, 0x65, {'o', 'l', 'd'}));
    replay(pipeline, make_packet(101, TS_OLD, 0x65, {'f', 'r', 'a', 'm', 'e'}));
    print_frame("old generation", pipeline.pull((size_t)stream));
    print_frame("old generation", pipeline.pull((size_t)stream));

    /* The sender restarts and jumps to an unrelated sequence number with the same SSRC. The first
     * packet of the new generation is held as a candidate and not delivered yet. */
    replay(pipeline, make_packet(30000, TS_NEW, 0x65, {'n', 'e', 'w'}));
    print_frame("after jump (held)", pipeline.pull((size_t)stream));

    /* The next packet confirms the new generation; both packets are delivered. */
    replay(pipeline, make_packet(30001, TS_NEW, 0x65, {'g', 'e', 'n'}));
    print_frame("new generation", pipeline.pull((size_t)stream));
    print_frame("new generation", pipeline.pull((size_t)stream));

    /* RTCP statistics were re-based: the first report of the new generation expects exactly the
     * two packets that were received. */
    uvgrtp::rtcp_internal::receive_stats_snapshot stats;
    if (pipeline.rtcp((size_t)stream)->get_receive_stats(SSRC, stats)) {
        std::printf("new generation report: expected=%u received=%u max_seq=%u\n",
            stats.expected, stats.received_pkts, (unsigned)stats.max_seq);
    }

    std::unique_ptr<uint8_t[]> report;
    size_t report_size = 0;
    if (pipeline.build_report((size_t)stream, report, report_size) == RTP_OK) {
        std::printf("RTCP compound report: %zu bytes\n", report_size);
    }

    std::printf("RECEIVE_RESTART_EXAMPLE_OK\n");
    return 0;
}
