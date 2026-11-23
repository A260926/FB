#include "task_support/check.h"

#include "receive_restart.hh"
#include "rtcp_internal.hh"
#include "formats/media.hh"

#include "uvgrtp/frame.hh"
#include "uvgrtp/util.hh"

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

namespace {

    constexpr int FLAGS = RCE_NO_H26X_PREPEND_SC;

    std::vector<uint8_t> rtp(uint16_t seq, uint32_t ts, uint32_t ssrc,
        const std::vector<uint8_t>& payload, bool marker = false)
    {
        std::vector<uint8_t> b(12, 0);
        b[0] = 0x80;
        b[1] = (uint8_t)(96 | (marker ? 0x80 : 0));
        b[2] = (uint8_t)(seq >> 8);
        b[3] = (uint8_t)seq;
        for (int i = 0; i < 4; ++i) {
            b[4 + i] = (uint8_t)(ts >> (24 - 8 * i));
            b[8 + i] = (uint8_t)(ssrc >> (24 - 8 * i));
        }
        b.insert(b.end(), payload.begin(), payload.end());
        return b;
    }

    /* H.264 single NAL unit: payload[0] is the NAL header. */
    std::vector<uint8_t> nal(uint8_t header, const std::vector<uint8_t>& body = {})
    {
        std::vector<uint8_t> p;
        p.push_back(header);
        p.insert(p.end(), body.begin(), body.end());
        return p;
    }

    /* H.264 FU-A start/middle/end. Fragment header: F|NRI|28, FU header: S|E|type. */
    std::vector<uint8_t> fu(bool start, bool end, uint8_t nal_type, const std::vector<uint8_t>& body)
    {
        std::vector<uint8_t> p;
        p.push_back((uint8_t)(0x60 | 28));
        p.push_back((uint8_t)((start ? 0x80 : 0) | (end ? 0x40 : 0) | nal_type));
        p.insert(p.end(), body.begin(), body.end());
        return p;
    }

    /* H.264 STAP-A carrying the given single NAL units. */
    std::vector<uint8_t> stap_a(const std::vector<std::vector<uint8_t>>& nalus)
    {
        std::vector<uint8_t> p;
        p.push_back((uint8_t)(0x60 | 24));
        for (auto& n : nalus) {
            p.push_back((uint8_t)(n.size() >> 8));
            p.push_back((uint8_t)n.size());
            p.insert(p.end(), n.begin(), n.end());
        }
        return p;
    }

    std::vector<uint8_t> bytes(const std::vector<std::vector<uint8_t>>& ps)
    {
        std::vector<uint8_t> b;
        for (auto& p : ps) {
            b.insert(b.end(), p.begin(), p.end());
        }
        return b;
    }

    /* Drains the non-blocking pull queue of the pipeline. */
    std::vector<uvgrtp::frame::rtp_frame*> drain(uvgrtp::receive_restart_pipeline& p, size_t index)
    {
        std::vector<uvgrtp::frame::rtp_frame*> out;
        while (auto* f = p.pull(index)) {
            out.push_back(f);
        }
        return out;
    }

    void release(std::vector<uvgrtp::frame::rtp_frame*>& frames)
    {
        for (auto* f : frames) {
            (void)uvgrtp::frame::dealloc_frame(f);
        }
        frames.clear();
    }

    void release(std::vector<uvgrtp::frame::rtp_frame*>&& frames)
    {
        release(frames);
    }

    struct rr_block {
        bool found = false;
        uint8_t fraction = 0;
        uint32_t lost = 0;
        uint16_t cycles = 0;
        uint16_t max_seq = 0;
        uint32_t jitter = 0;
        uint32_t lsr = 0;
        uint32_t dlsr = 0;
    };

    /* Parse the RR report block for "report_ssrc" out of a compound RTCP packet. */
    rr_block parse_rr(const uint8_t* data, size_t size, uint32_t report_ssrc)
    {
        rr_block info;
        size_t off = 0;
        while (off + 4 <= size) {
            uint8_t type = data[off + 1];
            uint16_t len = (uint16_t)((data[off + 2] << 8) | data[off + 3]);
            size_t packet_size = ((size_t)len + 1) * 4;
            if (off + packet_size > size) {
                break;
            }
            if (type == 201 && packet_size >= 8) {
                uint8_t count = (uint8_t)(data[off] & 0x1f);
                for (uint8_t i = 0; i < count; ++i) {
                    size_t b = off + 8 + (size_t)i * 24;
                    if (b + 24 > off + packet_size) {
                        break;
                    }
                    uint32_t ssrc = ((uint32_t)data[b] << 24) | ((uint32_t)data[b + 1] << 16) |
                        ((uint32_t)data[b + 2] << 8) | data[b + 3];
                    if (ssrc != report_ssrc) {
                        continue;
                    }
                    info.found = true;
                    info.fraction = data[b + 4];
                    info.lost = ((uint32_t)data[b + 5] << 16) | ((uint32_t)data[b + 6] << 8) | data[b + 7];
                    uint32_t ext = ((uint32_t)data[b + 8] << 24) | ((uint32_t)data[b + 9] << 16) |
                        ((uint32_t)data[b + 10] << 8) | data[b + 11];
                    info.cycles = (uint16_t)(ext >> 16);
                    info.max_seq = (uint16_t)ext;
                    info.jitter = ((uint32_t)data[b + 12] << 24) | ((uint32_t)data[b + 13] << 16) |
                        ((uint32_t)data[b + 14] << 8) | data[b + 15];
                    info.lsr = ((uint32_t)data[b + 16] << 24) | ((uint32_t)data[b + 17] << 16) |
                        ((uint32_t)data[b + 18] << 8) | data[b + 19];
                    info.dlsr = ((uint32_t)data[b + 20] << 24) | ((uint32_t)data[b + 21] << 16) |
                        ((uint32_t)data[b + 22] << 8) | data[b + 23];
                    return info;
                }
            }
            off += packet_size;
        }
        return info;
    }

    bool feed_rtp(uvgrtp::receive_restart_pipeline& p, uint16_t seq, uint32_t ts,
        uint32_t ssrc, const std::vector<uint8_t>& payload, bool marker = false)
    {
        auto b = rtp(seq, ts, ssrc, payload, marker);
        return p.feed(b.data(), b.size()) == RTP_OK;
    }

    /* Build a minimal RTCP Sender Report (no report blocks) and feed it through the production
     * RTCP parser of the stream. */
    void feed_sender_report(uvgrtp::receive_restart_pipeline& p, size_t index, uint32_t ssrc,
        uint32_t ntp_msw, uint32_t ntp_lsw)
    {
        std::vector<uint8_t> b(28, 0);
        b[0] = 0x80;          /* V=2, P=0, RC=0 */
        b[1] = 200;           /* PT = SR */
        b[2] = 0x00; b[3] = 6;/* length in 32-bit words - 1: (28/4)-1 = 6 */
        auto put32 = [&b](size_t off, uint32_t v) {
            for (int i = 0; i < 4; ++i) b[off + i] = (uint8_t)(v >> (24 - 8 * i));
        };
        put32(4, ssrc);
        put32(8, ntp_msw);
        put32(12, ntp_lsw);
        put32(16, 0);         /* RTP timestamp */
        put32(20, 0);         /* packet count */
        put32(24, 0);         /* octet count */
        uvgrtp::frame::rtp_frame* out = nullptr;
        (void)p.rtcp(index)->handle_incoming_packet(nullptr, 0, b.data(), b.size(), &out);
    }

    /* A hook sink that records the delivered sequence numbers and frees the frames (the callback
     * owns them once it returns). */
    struct hook_sink {
        std::vector<uint16_t> seqs;
        std::vector<std::vector<uint8_t>> payloads;
    };

    void record_and_free(void* arg, uvgrtp::frame::rtp_frame* frame)
    {
        auto* sink = static_cast<hook_sink*>(arg);
        if (sink == nullptr || frame == nullptr) {
            return;
        }
        sink->seqs.push_back(frame->header.seq);
        sink->payloads.push_back(std::vector<uint8_t>(frame->payload, frame->payload + frame->payload_len));
        (void)uvgrtp::frame::dealloc_frame(frame);
    }

} // namespace

/* -------------------------------------------------------------------------
 * admission
 * ------------------------------------------------------------------------- */

static int case_configure_timing()
{
    {
        uvgrtp::receive_restart_pipeline p;
        int s = p.add_stream(RTP_FORMAT_H264, 111, 7, FLAGS);
        TASK_CHECK(p.configure_receive_restart(s, true) == RTP_OK);
        TASK_CHECK(p.restart((size_t)s).enabled());

        TASK_CHECK(feed_rtp(p, 100, 9000, 7, nal(0x65, {'A'})));
        auto frames = drain(p, (size_t)s);
        TASK_CHECK(frames.size() == 1);
        release(frames);

        /* Enabling after the first packet is rejected and does not change the state: it was already
         * enabled, so it stays enabled. */
        TASK_CHECK(p.configure_receive_restart(s, true) == RTP_INVALID_VALUE);
        TASK_CHECK(p.restart((size_t)s).enabled());

        /* Disabling before any packet is fine. */
        uvgrtp::receive_restart_pipeline q;
        int t = q.add_stream(RTP_FORMAT_H264, 111, 9, FLAGS);
        TASK_CHECK(q.configure_receive_restart(t, false) == RTP_OK);
    }

    /* Enabling a still-disabled stream after the first packet is rejected and leaves it disabled. */
    {
        uvgrtp::receive_restart_pipeline p;
        int s = p.add_stream(RTP_FORMAT_H264, 111, 7, FLAGS);
        TASK_CHECK(feed_rtp(p, 100, 9000, 7, nal(0x65, {'A'})));
        release(drain(p, (size_t)s));
        TASK_CHECK(p.configure_receive_restart(s, true) == RTP_INVALID_VALUE);
        TASK_CHECK(!p.restart((size_t)s).enabled());
    }

    /* Unsupported formats are rejected without changing state. */
    {
        uvgrtp::receive_restart_pipeline p;
        int h266 = p.add_stream(RTP_FORMAT_H266, 111, 7, FLAGS);
        int opus = p.add_stream(RTP_FORMAT_OPUS, 112, 8, FLAGS);
        TASK_CHECK(p.configure_receive_restart(h266, true) == RTP_INVALID_VALUE);
        TASK_CHECK(p.configure_receive_restart(opus, true) == RTP_INVALID_VALUE);
        TASK_CHECK(!p.restart((size_t)h266).enabled());
        TASK_CHECK(!p.restart((size_t)opus).enabled());
    }

    /* Encrypted reception is rejected. */
    {
        uvgrtp::receive_restart_pipeline p;
        int s = p.add_stream(RTP_FORMAT_H265, 111, 7, FLAGS | RCE_SRTP);
        TASK_CHECK(p.configure_receive_restart(s, true) == RTP_INVALID_VALUE);
        TASK_CHECK(!p.restart((size_t)s).enabled());
    }

    /* H.265 plain reception is accepted. */
    {
        uvgrtp::receive_restart_pipeline p;
        int s = p.add_stream(RTP_FORMAT_H265, 111, 7, FLAGS);
        TASK_CHECK(p.configure_receive_restart(s, true) == RTP_OK);
    }

    return 0;
}

static int case_single_jump_candidate()
{
    uvgrtp::receive_restart_pipeline p;
    int s = p.add_stream(RTP_FORMAT_H264, 111, 7, FLAGS);
    TASK_CHECK(p.configure_receive_restart(s, true) == RTP_OK);

    TASK_CHECK(feed_rtp(p, 100, 9000, 7, nal(0x65, {'A'})));
    auto base = drain(p, (size_t)s);
    TASK_CHECK(base.size() == 1);
    release(base);

    /* A single big jump is stored as a candidate, not delivered and not counted. */
    TASK_CHECK(feed_rtp(p, 40000, 100, 7, nal(0x65, {'J'})));
    auto after_jump = drain(p, (size_t)s);
    TASK_CHECK(after_jump.empty());
    TASK_CHECK(p.restart((size_t)s).has_candidate());
    TASK_CHECK(p.restart((size_t)s).candidate_seq() == 40000);

    /* A duplicated candidate keeps the first copy and is not confirmed by itself. */
    TASK_CHECK(feed_rtp(p, 40000, 100, 7, nal(0x65, {'X'})));
    TASK_CHECK(drain(p, (size_t)s).empty());
    TASK_CHECK(p.restart((size_t)s).has_candidate());

    /* The successor confirms the new generation: candidate first, then the confirming packet. */
    TASK_CHECK(feed_rtp(p, 40001, 100, 7, nal(0x65, {'K'})));
    auto confirmed = drain(p, (size_t)s);
    TASK_CHECK(confirmed.size() == 2);
    TASK_CHECK(confirmed[0]->header.seq == 40000);
    TASK_CHECK(confirmed[1]->header.seq == 40001);
    TASK_CHECK(confirmed[0]->payload_len == 2 && confirmed[0]->payload[1] == 'J');
    TASK_CHECK(confirmed[1]->payload_len == 2 && confirmed[1]->payload[1] == 'K');
    release(confirmed);
    TASK_CHECK(!p.restart((size_t)s).has_candidate());

    return 0;
}

static int case_candidate_replacement()
{
    uvgrtp::receive_restart_pipeline p;
    int s = p.add_stream(RTP_FORMAT_H264, 111, 7, FLAGS);
    TASK_CHECK(p.configure_receive_restart(s, true) == RTP_OK);
    TASK_CHECK(feed_rtp(p, 100, 9000, 7, nal(0x65, {'A'})));
    release(drain(p, (size_t)s));

    /* First candidate. */
    TASK_CHECK(feed_rtp(p, 40000, 100, 7, nal(0x65, {'J'})));
    TASK_CHECK(p.restart((size_t)s).candidate_seq() == 40000);

    /* A different candidate region packet replaces the stored candidate. */
    TASK_CHECK(feed_rtp(p, 50000, 100, 7, nal(0x65, {'R'})));
    TASK_CHECK(p.restart((size_t)s).candidate_seq() == 50000);
    TASK_CHECK(drain(p, (size_t)s).empty());

    /* The old candidate's successor must not confirm anything: it becomes the new candidate. */
    TASK_CHECK(feed_rtp(p, 40001, 100, 7, nal(0x65, {'S'})));
    TASK_CHECK(p.restart((size_t)s).candidate_seq() == 40001);
    TASK_CHECK(drain(p, (size_t)s).empty());

    /* Its successor confirms the replacement generation. */
    TASK_CHECK(feed_rtp(p, 40002, 100, 7, nal(0x65, {'T'})));
    auto frames = drain(p, (size_t)s);
    TASK_CHECK(frames.size() == 2);
    TASK_CHECK(frames[0]->header.seq == 40001 && frames[0]->payload[1] == 'S');
    TASK_CHECK(frames[1]->header.seq == 40002 && frames[1]->payload[1] == 'T');
    release(frames);

    return 0;
}

static int case_boundaries()
{
    /* Forward/candidate boundary: d = 2999 is forward, d = 3000 is a candidate. */
    {
        uvgrtp::receive_restart_pipeline p;
        int s = p.add_stream(RTP_FORMAT_H264, 111, 7, FLAGS);
        TASK_CHECK(p.configure_receive_restart(s, true) == RTP_OK);
        TASK_CHECK(feed_rtp(p, 1000, 9000, 7, nal(0x65, {'A'})));
        release(drain(p, (size_t)s));

        /* d = 2999: delivered, H advances. */
        TASK_CHECK(feed_rtp(p, 3999, 9000, 7, nal(0x65, {'B'})));
        auto fwd = drain(p, (size_t)s);
        TASK_CHECK(fwd.size() == 1 && fwd[0]->header.seq == 3999);
        release(fwd);

        /* d = 3000 from H = 3999 -> 6999: candidate, held. */
        TASK_CHECK(feed_rtp(p, 6999, 100, 7, nal(0x65, {'C'})));
        TASK_CHECK(p.restart((size_t)s).has_candidate());
        TASK_CHECK(drain(p, (size_t)s).empty());
    }

    /* Late/candidate boundary: d = 65535 is late, d = 65436 is a candidate. */
    {
        uvgrtp::receive_restart_pipeline p;
        int s = p.add_stream(RTP_FORMAT_H264, 111, 7, FLAGS);
        TASK_CHECK(p.configure_receive_restart(s, true) == RTP_OK);
        TASK_CHECK(feed_rtp(p, 50000, 9000, 7, nal(0x65, {'A'})));
        release(drain(p, (size_t)s));

        /* d = 65535: seq = H - 1 = 49999, delivered as a bounded late packet. */
        TASK_CHECK(feed_rtp(p, 49999, 9000, 7, nal(0x65, {'B'})));
        auto late = drain(p, (size_t)s);
        TASK_CHECK(late.size() == 1 && late[0]->header.seq == 49999);
        release(late);
        TASK_CHECK(!p.restart((size_t)s).has_candidate());

        /* d = 65436: seq = H + 65436 (mod 65536) = 49900. That value is in the candidate region
         * [3000, 65436], so it becomes a candidate (not a late packet). */
        TASK_CHECK(feed_rtp(p, 49900, 100, 7, nal(0x65, {'C'})));
        TASK_CHECK(p.restart((size_t)s).has_candidate());
        TASK_CHECK(drain(p, (size_t)s).empty());
    }
    return 0;
}

static int case_old_stream_during_candidate()
{
    uvgrtp::receive_restart_pipeline p;
    int s = p.add_stream(RTP_FORMAT_H264, 111, 7, FLAGS);
    TASK_CHECK(p.configure_receive_restart(s, true) == RTP_OK);
    TASK_CHECK(feed_rtp(p, 100, 9000, 7, nal(0x65, {'A'})));
    release(drain(p, (size_t)s));

    TASK_CHECK(feed_rtp(p, 40000, 100, 7, nal(0x65, {'J'})));
    TASK_CHECK(p.restart((size_t)s).has_candidate());

    /* Old-generation packets keep flowing normally and do not disturb the candidate. */
    TASK_CHECK(feed_rtp(p, 102, 9000, 7, nal(0x65, {'B'})));
    auto old = drain(p, (size_t)s);
    TASK_CHECK(old.size() == 1 && old[0]->header.seq == 102);
    release(old);
    TASK_CHECK(p.restart((size_t)s).has_candidate());

    /* The candidate is still confirmed afterwards. */
    TASK_CHECK(feed_rtp(p, 40001, 100, 7, nal(0x65, {'K'})));
    auto frames = drain(p, (size_t)s);
    TASK_CHECK(frames.size() == 2);
    TASK_CHECK(frames[0]->header.seq == 40000 && frames[0]->payload[1] == 'J');
    release(frames);

    return 0;
}

static int case_wrap_and_late()
{
    uvgrtp::receive_restart_pipeline p;
    int s = p.add_stream(RTP_FORMAT_H264, 111, 7, FLAGS);
    TASK_CHECK(p.configure_receive_restart(s, true) == RTP_OK);

    /* Normal forward wrap across 65535 -> 0 is delivered, not treated as a restart. */
    TASK_CHECK(feed_rtp(p, 65534, 9000, 7, nal(0x65, {'A'})));
    release(drain(p, (size_t)s));
    TASK_CHECK(feed_rtp(p, 65535, 9000, 7, nal(0x65, {'B'})));
    release(drain(p, (size_t)s));
    TASK_CHECK(feed_rtp(p, 0, 9000, 7, nal(0x65, {'C'})));
    auto wrap = drain(p, (size_t)s);
    TASK_CHECK(wrap.size() == 1 && wrap[0]->header.seq == 0);
    release(wrap);
    TASK_CHECK(feed_rtp(p, 1, 9000, 7, nal(0x65, {'D'})));
    auto fwd = drain(p, (size_t)s);
    TASK_CHECK(fwd.size() == 1 && fwd[0]->header.seq == 1);
    release(fwd);

    /* Bounded late packets are delivered and do not move the active highest sequence. */
    TASK_CHECK(feed_rtp(p, 65500, 9000, 7, nal(0x65, {'E'}))); /* d = 65499 > 65436 */
    auto late = drain(p, (size_t)s);
    TASK_CHECK(late.size() == 1 && late[0]->header.seq == 65500);
    release(late);
    TASK_CHECK(!p.restart((size_t)s).has_candidate());

    /* The next forward packet is measured against the unchanged highest (1). */
    TASK_CHECK(feed_rtp(p, 2, 9000, 7, nal(0x65, {'F'})));
    auto next = drain(p, (size_t)s);
    TASK_CHECK(next.size() == 1 && next[0]->header.seq == 2);
    release(next);

    /* Restart candidate across the 65535 -> 0 boundary confirms. Use a fresh stream whose highest
     * is 50000 so both 65535 (d=15535) and 0 (d=15536 mod 65536) are in the candidate region. */
    uvgrtp::receive_restart_pipeline q;
    int t = q.add_stream(RTP_FORMAT_H264, 111, 7, FLAGS);
    TASK_CHECK(q.configure_receive_restart(t, true) == RTP_OK);
    TASK_CHECK(feed_rtp(q, 50000, 9000, 7, nal(0x65, {'A'})));
    release(drain(q, (size_t)t));

    TASK_CHECK(feed_rtp(q, 65535, 100, 7, nal(0x65, {'G'}))); /* d = 15535 -> candidate */
    TASK_CHECK(q.restart((size_t)t).candidate_seq() == 65535);
    TASK_CHECK(drain(q, (size_t)t).empty());
    TASK_CHECK(feed_rtp(q, 0, 100, 7, nal(0x65, {'H'}))); /* candidate + 1 wraps to 0 */
    auto confirmed = drain(q, (size_t)t);
    TASK_CHECK(confirmed.size() == 2);
    TASK_CHECK(confirmed[0]->header.seq == 65535);
    TASK_CHECK(confirmed[1]->header.seq == 0);
    release(confirmed);

    return 0;
}

static int case_malformed_keeps_state()
{
    uvgrtp::receive_restart_pipeline p;
    int s = p.add_stream(RTP_FORMAT_H264, 111, 7, FLAGS);
    TASK_CHECK(p.configure_receive_restart(s, true) == RTP_OK);
    TASK_CHECK(feed_rtp(p, 100, 9000, 7, nal(0x65, {'A'})));
    release(drain(p, (size_t)s));
    TASK_CHECK(feed_rtp(p, 40000, 100, 7, nal(0x65, {'J'})));
    TASK_CHECK(p.restart((size_t)s).candidate_seq() == 40000);

    /* Malformed RTP: claims 15 CSRC entries in a 12-byte datagram. The RTP parser rejects it before
     * the restart object sees a frame, so nothing about the restart state may change. */
    std::vector<uint8_t> bad(12, 0);
    bad[0] = (uint8_t)((2 << 6) | 0x0f);
    bad[1] = 96;
    bad[2] = 0x9c; bad[3] = 0x41; /* seq 40001, which would otherwise confirm the candidate */
    TASK_CHECK(p.feed(bad.data(), bad.size()) < 0); /* the parser rejects it */
    TASK_CHECK(p.restart((size_t)s).has_candidate());
    TASK_CHECK(p.restart((size_t)s).candidate_seq() == 40000);
    TASK_CHECK(drain(p, (size_t)s).empty());

    /* A real confirming packet still works. */
    TASK_CHECK(feed_rtp(p, 40001, 100, 7, nal(0x65, {'K'})));
    auto frames = drain(p, (size_t)s);
    TASK_CHECK(frames.size() == 2);
    release(frames);
    return 0;
}

static int case_disable_releases_candidate()
{
    uvgrtp::receive_restart_pipeline p;
    int s = p.add_stream(RTP_FORMAT_H264, 111, 7, FLAGS);
    TASK_CHECK(p.configure_receive_restart(s, true) == RTP_OK);
    TASK_CHECK(feed_rtp(p, 100, 9000, 7, nal(0x65, {'A'})));
    release(drain(p, (size_t)s));
    TASK_CHECK(feed_rtp(p, 40000, 100, 7, nal(0x65, {'J'})));
    TASK_CHECK(p.restart((size_t)s).has_candidate());

    /* Disabling drops the stored candidate. */
    TASK_CHECK(p.configure_receive_restart(s, false) == RTP_OK);
    TASK_CHECK(!p.restart((size_t)s).has_candidate());
    TASK_CHECK(!p.restart((size_t)s).enabled());

    /* Re-enabling is rejected because the first packet has already been seen. */
    TASK_CHECK(p.configure_receive_restart(s, true) == RTP_INVALID_VALUE);

    /* With the feature off the old behaviour is back: a big jump is delivered. */
    TASK_CHECK(feed_rtp(p, 40001, 100, 7, nal(0x65, {'K'})));
    auto frames = drain(p, (size_t)s);
    TASK_CHECK(frames.size() == 1 && frames[0]->header.seq == 40001);
    release(frames);
    return 0;
}

static int case_disabled_baseline()
{
    uvgrtp::receive_restart_pipeline p;
    int s = p.add_stream(RTP_FORMAT_H264, 111, 7, FLAGS);
    /* Never configured. */
    TASK_CHECK(feed_rtp(p, 100, 9000, 7, nal(0x65, {'A'})));
    release(drain(p, (size_t)s));
    TASK_CHECK(feed_rtp(p, 20000, 10, 7, nal(0x65, {'N'})));
    auto frames = drain(p, (size_t)s);
    TASK_CHECK(frames.size() == 1 && frames[0]->header.seq == 20000);
    release(frames);
    return 0;
}

/* -------------------------------------------------------------------------
 * media
 * ------------------------------------------------------------------------- */

static int case_fu_start_candidate_recovered()
{
    uvgrtp::receive_restart_pipeline p;
    int s = p.add_stream(RTP_FORMAT_H264, 111, 7, FLAGS);
    TASK_CHECK(p.configure_receive_restart(s, true) == RTP_OK);
    TASK_CHECK(feed_rtp(p, 100, 9000, 7, nal(0x65, {'A'})));
    release(drain(p, (size_t)s));

    /* The real first fragment of a new-generation FU becomes the candidate. */
    TASK_CHECK(feed_rtp(p, 40000, 2000, 7, fu(true, false, 5, {'P', 'Q'})));
    TASK_CHECK(p.restart((size_t)s).has_candidate());
    TASK_CHECK(drain(p, (size_t)s).empty());

    /* The successor (the end fragment) confirms and the NAL is reconstructed from both. */
    TASK_CHECK(feed_rtp(p, 40001, 2000, 7, fu(false, true, 5, {'R', 'S'})));
    auto frames = drain(p, (size_t)s);
    TASK_CHECK(frames.size() == 1);
    TASK_CHECK(frames[0]->header.seq == 40001);
    /* reconstructed NAL: one-byte NAL header + PQRS */
    TASK_CHECK(frames[0]->payload_len == 5);
    TASK_CHECK(frames[0]->payload[0] == 0x65);
    TASK_CHECK(std::memcmp(frames[0]->payload + 1, "PQRS", 4) == 0);
    release(frames);
    return 0;
}

static int case_old_fu_does_not_cross()
{
    uvgrtp::receive_restart_pipeline p;
    int s = p.add_stream(RTP_FORMAT_H264, 111, 7, FLAGS);
    TASK_CHECK(p.configure_receive_restart(s, true) == RTP_OK);

    /* Leave an incomplete access unit from the old generation (timestamp 9000, seq 100). */
    TASK_CHECK(feed_rtp(p, 100, 9000, 7, fu(true, false, 5, {'O', 'L', 'D'})));
    TASK_CHECK(drain(p, (size_t)s).empty());

    /* Restart with a new-generation FU in the same timestamp. */
    TASK_CHECK(feed_rtp(p, 40000, 9000, 7, fu(true, false, 5, {'N', 'E'})));
    TASK_CHECK(p.restart((size_t)s).has_candidate());
    TASK_CHECK(feed_rtp(p, 40001, 9000, 7, fu(false, true, 5, {'W'})));
    auto frames = drain(p, (size_t)s);
    TASK_CHECK(frames.size() == 1);
    /* Only the new generation bytes are present; the old fragment must not be appended. */
    TASK_CHECK(frames[0]->payload_len == 4); /* NAL header + NEW */
    TASK_CHECK(std::memcmp(frames[0]->payload + 1, "NEW", 3) == 0);
    release(frames);

    /* The old incomplete fragment is gone: an end fragment for the old access unit cannot
     * complete a NAL on its own. */
    TASK_CHECK(feed_rtp(p, 40002, 9000, 7, fu(false, true, 5, {'Z'})));
    TASK_CHECK(drain(p, (size_t)s).empty());
    return 0;
}

static int case_pending_aggregate_not_crossing()
{
    uvgrtp::receive_restart_pipeline p;
    int s = p.add_stream(RTP_FORMAT_H264, 111, 7, FLAGS);
    TASK_CHECK(p.configure_receive_restart(s, true) == RTP_OK);

    /* An aggregation packet yields two complete NAL units that are queued for the application. */
    TASK_CHECK(feed_rtp(p, 100, 9000, 7, stap_a({nal(0x65, {'A'}), nal(0x41, {'B'})})));
    /* Do not pull them: they are pending delivery of the old generation. */

    /* Restart. The pending old NAL units must not be delivered after the generation switch. */
    TASK_CHECK(feed_rtp(p, 40000, 100, 7, nal(0x65, {'J'})));
    TASK_CHECK(feed_rtp(p, 40001, 100, 7, nal(0x65, {'K'})));
    auto frames = drain(p, (size_t)s);
    TASK_CHECK(frames.size() == 2);
    TASK_CHECK(frames[0]->header.seq == 40000);
    TASK_CHECK(frames[1]->header.seq == 40001);
    release(frames);
    return 0;
}

static int case_dedup_cleared()
{
    uvgrtp::receive_restart_pipeline p;
    int s = p.add_stream(RTP_FORMAT_H264, 111, 7, FLAGS);
    TASK_CHECK(p.configure_receive_restart(s, true) == RTP_OK);

    /* Old generation frame at timestamp 9000. */
    TASK_CHECK(feed_rtp(p, 100, 9000, 7, nal(0x65, {'A'})));
    release(drain(p, (size_t)s));

    /* New generation reuses the same timestamp. It must not be deduplicated against the old
     * (timestamp, sequence) history. */
    TASK_CHECK(feed_rtp(p, 40000, 9000, 7, nal(0x65, {'J'})));
    TASK_CHECK(feed_rtp(p, 40001, 9000, 7, nal(0x65, {'K'})));
    auto frames = drain(p, (size_t)s);
    TASK_CHECK(frames.size() == 2);
    TASK_CHECK(frames[0]->payload_len == 2 && frames[0]->payload[1] == 'J');
    TASK_CHECK(frames[1]->payload_len == 2 && frames[1]->payload[1] == 'K');
    release(frames);

    /* The duplicate-packet history must be cleared by a generation switch. A normal restart cannot
     * reuse an exact (timestamp, sequence) key because the new generation's sequence numbers are at
     * least 3000 away from the old ones, so the clear is verified through the very routine the
     * switch calls: reset_receive_state() must forget the recorded duplicate. */
    {
        uvgrtp::receive_restart_pipeline q;
        int t = q.add_stream(RTP_FORMAT_H264, 111, 7, FLAGS);
        TASK_CHECK(q.configure_receive_restart(t, true) == RTP_OK);

        /* Delivered packet, recorded in the duplicate history: ts 5000, seq 10. */
        TASK_CHECK(feed_rtp(q, 10, 5000, 7, nal(0x65, {'P'})));
        release(drain(q, (size_t)t));

        /* Without the reset the same key is a duplicate and is dropped (the media handler reports
         * an error, so feed_rtp's RTP_OK check is not used here). */
        auto dup = rtp(10, 5000, 7, nal(0x65, {'P'}));
        (void)q.feed(dup.data(), dup.size());
        TASK_CHECK(drain(q, (size_t)t).empty());

        /* The generation switch clears the history. */
        q.media((size_t)t)->reset_receive_state();

        /* The same (timestamp, sequence) key is accepted again. */
        TASK_CHECK(feed_rtp(q, 10, 5000, 7, nal(0x65, {'S'})));
        auto replay = drain(q, (size_t)t);
        TASK_CHECK(replay.size() == 1);
        TASK_CHECK(replay[0]->header.timestamp == 5000);
        TASK_CHECK(replay[0]->header.seq == 10);
        release(replay);
    }
    return 0;
}

static int case_two_streams_pull_and_hook()
{
    uvgrtp::receive_restart_pipeline p;
    int a = p.add_stream(RTP_FORMAT_H264, 111, 7, FLAGS);   /* pull */
    int b = p.add_stream(RTP_FORMAT_H264, 112, 8, FLAGS);   /* hook */
    TASK_CHECK(p.configure_receive_restart(a, true) == RTP_OK);
    TASK_CHECK(p.configure_receive_restart(b, true) == RTP_OK);

    hook_sink sink;
    TASK_CHECK(p.install_receive_hook(b, &sink, record_and_free) == RTP_OK);

    /* Interleave both streams through the shared dispatch entry point. */
    TASK_CHECK(feed_rtp(p, 100, 9000, 7, nal(0x65, {'A'})));
    TASK_CHECK(feed_rtp(p, 100, 9001, 8, nal(0x65, {'a'})));
    auto a1 = drain(p, (size_t)a);
    TASK_CHECK(a1.size() == 1 && a1[0]->header.ssrc == 7);
    release(a1);
    TASK_CHECK(sink.seqs.size() == 1 && sink.seqs.back() == 100);

    /* Stream A gets a candidate; it is not delivered and stream B is unaffected. */
    TASK_CHECK(feed_rtp(p, 40000, 10, 7, nal(0x65, {'J'})));
    auto none = drain(p, (size_t)a);
    TASK_CHECK(none.empty());
    TASK_CHECK(feed_rtp(p, 101, 9001, 8, nal(0x65, {'b'})));
    TASK_CHECK(sink.seqs.size() == 2 && sink.seqs.back() == 101);
    TASK_CHECK(none.empty());

    /* A's confirmation delivers the candidate and the confirming packet; B still unaffected. */
    TASK_CHECK(feed_rtp(p, 40001, 10, 7, nal(0x65, {'K'})));
    auto a2 = drain(p, (size_t)a);
    TASK_CHECK(a2.size() == 2);
    TASK_CHECK(a2[0]->header.seq == 40000 && a2[0]->payload[1] == 'J');
    TASK_CHECK(a2[1]->header.seq == 40001 && a2[1]->payload[1] == 'K');
    release(a2);
    TASK_CHECK(sink.seqs.size() == 2);

    /* No frame was routed to the wrong consumer. */
    TASK_CHECK(drain(p, (size_t)b).empty());
    return 0;
}

/* -------------------------------------------------------------------------
 * reports
 * ------------------------------------------------------------------------- */

static int case_first_and_next_report()
{
    uvgrtp::receive_restart_pipeline p;
    int s = p.add_stream(RTP_FORMAT_H264, 111, 7, FLAGS | RCE_RTCP);
    TASK_CHECK(p.configure_receive_restart(s, true) == RTP_OK);

    TASK_CHECK(feed_rtp(p, 100, 9000, 7, nal(0x65, {'A'})));
    release(drain(p, (size_t)s));
    TASK_CHECK(feed_rtp(p, 101, 9000, 7, nal(0x65, {'B'})));
    release(drain(p, (size_t)s));

    /* Restart: candidate + confirming packet. */
    TASK_CHECK(feed_rtp(p, 40000, 100, 7, nal(0x65, {'J'})));
    TASK_CHECK(feed_rtp(p, 40001, 100, 7, nal(0x65, {'K'})));
    release(drain(p, (size_t)s));

    /* First report of the new generation: expected and received are both 2. */
    uvgrtp::rtcp_internal::receive_stats_snapshot snap;
    TASK_CHECK(p.rtcp((size_t)s)->get_receive_stats(7, snap));
    TASK_CHECK(snap.expected == 2);
    TASK_CHECK(snap.received_pkts == 2);
    TASK_CHECK(snap.expected_prior == 0);
    TASK_CHECK(snap.received_prior == 0);
    TASK_CHECK(snap.cycles == 0);
    TASK_CHECK(snap.max_seq == 40001);

    std::unique_ptr<uint8_t[]> bytes;
    size_t size = 0;
    TASK_CHECK(p.build_report((size_t)s, bytes, size) == RTP_OK);
    TASK_CHECK(size > 0);
    rr_block rr = parse_rr(bytes.get(), size, 7);
    TASK_CHECK(rr.found);
    TASK_CHECK(rr.fraction == 0);
    TASK_CHECK(rr.lost == 0);
    TASK_CHECK(rr.cycles == 0);
    TASK_CHECK(rr.max_seq == 40001);

    /* Two more packets in the new generation: the next interval is 2 expected / 2 received. */
    TASK_CHECK(feed_rtp(p, 40002, 100, 7, nal(0x65, {'L'})));
    release(drain(p, (size_t)s));
    TASK_CHECK(feed_rtp(p, 40003, 100, 7, nal(0x65, {'M'})));
    release(drain(p, (size_t)s));

    TASK_CHECK(p.rtcp((size_t)s)->get_receive_stats(7, snap));
    TASK_CHECK(snap.expected == 4);
    TASK_CHECK(snap.received_pkts == 4);
    TASK_CHECK(snap.expected_prior == 2);
    TASK_CHECK(snap.received_prior == 2);

    bytes.reset();
    size = 0;
    TASK_CHECK(p.build_report((size_t)s, bytes, size) == RTP_OK);
    rr = parse_rr(bytes.get(), size, 7);
    TASK_CHECK(rr.found);
    TASK_CHECK(rr.fraction == 0);
    TASK_CHECK(rr.lost == 0);
    TASK_CHECK(rr.max_seq == 40003);
    return 0;
}

static int case_old_sr_isolated()
{
    uvgrtp::receive_restart_pipeline p;
    int s = p.add_stream(RTP_FORMAT_H264, 111, 7, FLAGS | RCE_RTCP);
    TASK_CHECK(p.configure_receive_restart(s, true) == RTP_OK);

    TASK_CHECK(feed_rtp(p, 100, 9000, 7, nal(0x65, {'A'})));
    release(drain(p, (size_t)s));

    /* Record a Sender Report from the old generation through the production RTCP parser. */
    feed_sender_report(p, (size_t)s, 7, 0x12345678, 0x9abcdef0);
    uvgrtp::rtcp_internal::receive_stats_snapshot before;
    TASK_CHECK(p.rtcp((size_t)s)->get_receive_stats(7, before));
    TASK_CHECK(before.lsr != 0);

    /* Restart. */
    TASK_CHECK(feed_rtp(p, 40000, 100, 7, nal(0x65, {'J'})));
    TASK_CHECK(feed_rtp(p, 40001, 100, 7, nal(0x65, {'K'})));
    release(drain(p, (size_t)s));

    uvgrtp::rtcp_internal::receive_stats_snapshot after;
    TASK_CHECK(p.rtcp((size_t)s)->get_receive_stats(7, after));
    TASK_CHECK(after.lsr == 0);

    std::unique_ptr<uint8_t[]> bytes;
    size_t size = 0;
    TASK_CHECK(p.build_report((size_t)s, bytes, size) == RTP_OK);
    rr_block rr = parse_rr(bytes.get(), size, 7);
    TASK_CHECK(rr.found);
    TASK_CHECK(rr.lsr == 0);
    TASK_CHECK(rr.dlsr == 0);

    /* A new SR after the restart is accepted normally. */
    feed_sender_report(p, (size_t)s, 7, 0x0a0b0c0d, 0x11223344);
    TASK_CHECK(p.rtcp((size_t)s)->get_receive_stats(7, after));
    TASK_CHECK(after.lsr != 0);
    return 0;
}

static int case_other_stream_unchanged()
{
    uvgrtp::receive_restart_pipeline p;
    int a = p.add_stream(RTP_FORMAT_H264, 111, 7, FLAGS | RCE_RTCP);
    int b = p.add_stream(RTP_FORMAT_H264, 112, 8, FLAGS | RCE_RTCP);
    TASK_CHECK(p.configure_receive_restart(a, true) == RTP_OK);
    TASK_CHECK(p.configure_receive_restart(b, true) == RTP_OK);

    /* Stream B accumulates its own state. */
    TASK_CHECK(feed_rtp(p, 500, 7000, 8, nal(0x65, {'b'})));
    TASK_CHECK(feed_rtp(p, 501, 7000, 8, nal(0x65, {'c'})));
    TASK_CHECK(feed_rtp(p, 502, 7000, 8, nal(0x65, {'d'})));
    /* Frames of both streams share the pull queue; drain by index order (B has no hook here). */
    auto drained = drain(p, (size_t)a);
    release(drained);

    uvgrtp::rtcp_internal::receive_stats_snapshot b_before;
    TASK_CHECK(p.rtcp((size_t)b)->get_receive_stats(8, b_before));
    TASK_CHECK(b_before.max_seq == 502);

    /* Stream A feeds its base and restarts. B must be completely untouched. */
    TASK_CHECK(feed_rtp(p, 100, 9000, 7, nal(0x65, {'A'})));
    TASK_CHECK(feed_rtp(p, 40000, 100, 7, nal(0x65, {'J'})));
    TASK_CHECK(feed_rtp(p, 40001, 100, 7, nal(0x65, {'K'})));
    auto a_frames = drain(p, (size_t)a);
    release(a_frames);

    uvgrtp::rtcp_internal::receive_stats_snapshot b_after;
    TASK_CHECK(p.rtcp((size_t)b)->get_receive_stats(8, b_after));
    TASK_CHECK(b_after.max_seq == b_before.max_seq);
    TASK_CHECK(b_after.received_pkts == b_before.received_pkts);
    TASK_CHECK(b_after.cycles == b_before.cycles);
    TASK_CHECK(b_after.lsr == b_before.lsr);

    std::unique_ptr<uint8_t[]> bytes;
    size_t size = 0;
    TASK_CHECK(p.build_report((size_t)b, bytes, size) == RTP_OK);
    rr_block rr = parse_rr(bytes.get(), size, 8);
    TASK_CHECK(rr.found);
    TASK_CHECK(rr.max_seq == 502);
    return 0;
}

/* -------------------------------------------------------------------------
 * Registration
 * ------------------------------------------------------------------------- */

extern const struct task_case task_cases[] = {
    { "admission", "configure_timing", case_configure_timing },
    { "admission", "single_jump_candidate", case_single_jump_candidate },
    { "admission", "candidate_replacement", case_candidate_replacement },
    { "admission", "boundaries", case_boundaries },
    { "admission", "old_stream_during_candidate", case_old_stream_during_candidate },
    { "admission", "wrap_and_late", case_wrap_and_late },
    { "admission", "malformed_keeps_state", case_malformed_keeps_state },
    { "admission", "disable_releases_candidate", case_disable_releases_candidate },
    { "admission", "disabled_baseline", case_disabled_baseline },

    { "media", "fu_start_candidate_recovered", case_fu_start_candidate_recovered },
    { "media", "old_fu_does_not_cross", case_old_fu_does_not_cross },
    { "media", "pending_aggregate_not_crossing", case_pending_aggregate_not_crossing },
    { "media", "dedup_cleared", case_dedup_cleared },
    { "media", "two_streams_pull_and_hook", case_two_streams_pull_and_hook },

    { "reports", "first_and_next_report", case_first_and_next_report },
    { "reports", "old_sr_isolated", case_old_sr_isolated },
    { "reports", "other_stream_unchanged", case_other_stream_unchanged },
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
