#include "receive_restart.hh"

#include "uvgrtp/util.hh"
#include "uvgrtp/frame.hh"

#include "debug.hh"
#include "reception_flow.hh"
#include "rtp.hh"
#include "frame_internal.hh"
#include "rtcp_internal.hh"
#include "formats/media.hh"
#include "formats/h264.hh"
#include "formats/h265.hh"
#include "socket.hh"

#include <cstring>
#include <vector>

namespace {
    /* d = uint16_t(S - H).
     * d < 3000 is normal forward (d == 0 is a duplicate),
     * d > 65536 - 100 is a bounded late packet,
     * anything else is a restart candidate. Mirrors RFC 3550 MAX_DROPOUT/MAX_MISORDER used by the
     * RTCP statistics, but with the stricter acceptance range the restart feature needs. */
    constexpr uint16_t FORWARD_LIMIT = 3000; /* exclusive upper bound */
    constexpr uint16_t LATE_LIMIT = 65536 - 100; /* exclusive lower bound for "late" */
}

uvgrtp::receive_restart::receive_restart() = default;

uvgrtp::receive_restart::~receive_restart()
{
    clear_candidate();
}

void uvgrtp::receive_restart::clear_candidate()
{
    if (candidate_ != nullptr) {
        (void)uvgrtp::frame::dealloc_frame(candidate_);
        candidate_ = nullptr;
        candidate_seq_ = 0;
    }
}

rtp_error_t uvgrtp::receive_restart::configure(bool enable, rtp_format_t fmt, int rce_flags)
{
    if (enable) {
        if (started_) {
            /* The generation must be known before the first packet is classified, otherwise the
             * active base would already have been established by the old rules. The state is left
             * unchanged (if it was already enabled it stays enabled). */
            UVG_LOG_WARN("Cannot enable receive restart after the first RTP packet");
            return RTP_INVALID_VALUE;
        }
        if (fmt != RTP_FORMAT_H264 && fmt != RTP_FORMAT_H265) {
            UVG_LOG_WARN("Receive restart supports plaintext H264/H265 only");
            return RTP_INVALID_VALUE;
        }
        if (rce_flags & (RCE_SRTP | RCE_ZRTP_DIFFIE_HELLMAN_MODE | RCE_ZRTP_MULTISTREAM_MODE)) {
            UVG_LOG_WARN("Receive restart does not support encrypted reception");
            return RTP_INVALID_VALUE;
        }
        enabled_ = true;
        return RTP_OK;
    }

    /* Disabling is always allowed and drops the stored candidate. */
    enabled_ = false;
    clear_candidate();
    return RTP_OK;
}

uvgrtp::frame::rtp_frame* uvgrtp::receive_restart::take_candidate()
{
    uvgrtp::frame::rtp_frame* frame = candidate_;
    candidate_ = nullptr;
    return frame;
}

uvgrtp::receive_restart_result uvgrtp::receive_restart::classify(
    uvgrtp::frame::rtp_frame* frame, std::chrono::steady_clock::time_point arrival)
{
    (void)arrival;
    if (frame == nullptr) {
        return receive_restart_result::feature_disabled;
    }

    const uint16_t seq = frame->header.seq;

    if (!started_) {
        /* First valid RTP packet of this stream: the active base is established immediately and
         * the packet is delivered through the normal path. This happens even while the feature is
         * disabled so that enabling it after the first packet can be rejected. */
        started_ = true;
        highest_ = seq;
        return receive_restart_result::feature_disabled;
    }

    if (!enabled_) {
        return receive_restart_result::feature_disabled;
    }

    const uint16_t d = uint16_t(seq - highest_);

    if (d < FORWARD_LIMIT) {
        /* Normal forward movement (d == 0 is a duplicate). Only a strict advance updates H. */
        if (d > 0) {
            highest_ = seq;
        }
        return receive_restart_result::forward;
    }

    if (d > LATE_LIMIT) {
        /* Bounded late packet: handled like a normal old packet, does not change the candidate. */
        return receive_restart_result::forward;
    }

    /* Restart candidate region. Note that reaching this point already implies
     * FORWARD_LIMIT <= d <= LATE_LIMIT for the packet itself, so it is still in the candidate
     * region relative to the old H. */
    if (candidate_ != nullptr && seq == uint16_t(candidate_seq_ + 1)) {
        /* This packet is exactly the successor of the stored candidate: the new generation is
         * confirmed. The new active highest sequence number is the confirming packet. */
        highest_ = seq;
        return receive_restart_result::confirmed;
    }

    if (candidate_ != nullptr && seq == candidate_seq_) {
        /* Exact duplicate of the stored candidate: keep the first copy. */
        return receive_restart_result::candidate_duplicate;
    }

    /* Replace the stored candidate with this packet. */
    clear_candidate();
    /* The datagram pointer may reference the reception ring buffer, which is reused; the stored
     * candidate outlives this call, so detach it. Nothing in the media pipeline reads dgram. */
    frame->dgram = nullptr;
    frame->dgram_size = 0;
    candidate_ = frame;
    candidate_seq_ = seq;
    return receive_restart_result::candidate;
}

void uvgrtp::receive_restart::switch_generation(uint32_t ssrc)
{
    /* The stored candidate is the first packet of the new generation. Re-base the RTCP statistics
     * on it before the media layer is reset so the candidate can also be delivered. */
    if (reset_reports_ && candidate_ != nullptr) {
        reset_reports_(candidate_);
    }
    if (reset_media_) {
        reset_media_();
    }
    if (discard_pending_) {
        discard_pending_(ssrc);
    }
}

void uvgrtp::receive_restart::set_callbacks(
    std::function<void(const uvgrtp::frame::rtp_frame*)> reset_reports,
    std::function<void()> reset_media,
    std::function<void(uint32_t)> discard_pending)
{
    reset_reports_ = std::move(reset_reports);
    reset_media_ = std::move(reset_media);
    discard_pending_ = std::move(discard_pending);
}


/* =====================================================================
 * In-memory receive pipeline
 * ===================================================================== */

struct uvgrtp::receive_restart_pipeline::impl {
    struct stream {
        rtp_format_t fmt = RTP_FORMAT_H264;
        int rce_flags = 0;
        std::shared_ptr<std::atomic<std::uint32_t>> local_ssrc;
        std::shared_ptr<std::atomic<std::uint32_t>> remote_ssrc;
        std::shared_ptr<uvgrtp::rtp> rtp;
        std::unique_ptr<uvgrtp::rtcp_internal> rtcp;
        std::unique_ptr<uvgrtp::formats::media> media;
        std::unique_ptr<uvgrtp::receive_restart> restart;
    };

    std::shared_ptr<uvgrtp::reception_flow> flow;
    std::vector<std::unique_ptr<stream>> streams;
    int feed_flags = 0;
};

uvgrtp::receive_restart_pipeline::receive_restart_pipeline() :
    impl_(new impl)
{
    impl_->flow = std::make_shared<uvgrtp::reception_flow>(false);
}

uvgrtp::receive_restart_pipeline::~receive_restart_pipeline() = default;

int uvgrtp::receive_restart_pipeline::add_stream(rtp_format_t fmt, uint32_t local_ssrc,
    uint32_t remote_ssrc, int rce_flags)
{
    std::unique_ptr<impl::stream> stream(new impl::stream);
    stream->fmt = fmt;
    stream->rce_flags = rce_flags;
    stream->local_ssrc = std::make_shared<std::atomic<std::uint32_t>>(local_ssrc);
    stream->remote_ssrc = std::make_shared<std::atomic<std::uint32_t>>(remote_ssrc);
    stream->rtp = std::make_shared<uvgrtp::rtp>(fmt, stream->local_ssrc, false);
    stream->restart.reset(new uvgrtp::receive_restart);

    stream->rtcp.reset(new uvgrtp::rtcp_internal(stream->rtp, stream->local_ssrc,
        stream->remote_ssrc, "pipeline", nullptr, rce_flags));
    stream->rtcp->activate_receive_statistics();

    if (fmt == RTP_FORMAT_H264) {
        stream->media.reset(new uvgrtp::formats::h264(nullptr, stream->rtp, rce_flags));
    }
    else if (fmt == RTP_FORMAT_H265) {
        stream->media.reset(new uvgrtp::formats::h265(nullptr, stream->rtp, rce_flags));
    }
    else {
        stream->media.reset(new uvgrtp::formats::media(nullptr, stream->rtp, rce_flags));
    }

    /* Handlers capture raw pointers owned by the stream, which lives in impl_->streams for the
     * lifetime of the pipeline (and the flow dies with the pipeline). */
    uvgrtp::rtp* rtp = stream->rtp.get();
    uvgrtp::rtcp_internal* rtcp = stream->rtcp.get();
    uvgrtp::formats::media* media = stream->media.get();
    uvgrtp::reception_flow* flow = impl_->flow.get();
    uvgrtp::receive_restart* restart = stream->restart.get();
    const uint32_t rssrc = remote_ssrc;

    impl_->flow->install_handler(1, stream->remote_ssrc,
        [rtp](void*, int f, uint8_t* p, size_t s, uvgrtp::frame::rtp_frame** o) {
            return rtp->packet_handler(nullptr, f, p, s, o);
        }, nullptr);

    impl_->flow->install_handler(6, stream->remote_ssrc,
        [rtcp](void*, int f, uint8_t* p, size_t s, uvgrtp::frame::rtp_frame** o) {
            return rtcp->recv_packet_handler_common(rtcp, f, p, s, o);
        }, rtcp);

    if (fmt == RTP_FORMAT_H264 || fmt == RTP_FORMAT_H265) {
        uvgrtp::formats::h26x* h26x = static_cast<uvgrtp::formats::h26x*>(media);
        impl_->flow->install_handler(5, stream->remote_ssrc,
            [h26x](void*, int f, uint8_t* p, size_t s, uvgrtp::frame::rtp_frame** o) {
                return h26x->packet_handler(nullptr, f, p, s, o);
            }, nullptr);
        impl_->flow->install_getter(stream->remote_ssrc,
            [h26x](uvgrtp::frame::rtp_frame** o) { return h26x->frame_getter(o); });
    }
    else {
        void* info = media->get_media_frame_info();
        impl_->flow->install_handler(5, stream->remote_ssrc,
            [media, info](void*, int f, uint8_t* p, size_t s, uvgrtp::frame::rtp_frame** o) {
                return media->packet_handler(info, f, p, s, o);
            }, info);
    }

    restart->set_callbacks(
        [rtcp, rssrc](const uvgrtp::frame::rtp_frame* candidate) {
            rtcp->reset_receive_generation(rssrc, candidate);
        },
        [media]() { media->reset_receive_state(); },
        [flow, media, rssrc](uint32_t) {
            /* The media layer may hold frames it has not returned yet (H26x aggregation output). */
            media->discard_pending_output();
            flow->discard_pending_frames(rssrc);
        });

    const int index = (int)impl_->streams.size();
    impl_->streams.push_back(std::move(stream));

    impl_->flow->install_receive_restart(impl_->streams.back()->remote_ssrc,
        impl_->streams.back()->restart.get());

    /* The reception flow is started with the flags of the streams sharing the socket. The pipeline
     * ORs them so the RTCP statistics handler runs for streams that enabled RTCP. */
    impl_->feed_flags |= rce_flags;

    return index;
}

rtp_error_t uvgrtp::receive_restart_pipeline::configure_receive_restart(size_t index, bool enable)
{
    if (index >= impl_->streams.size()) {
        return RTP_INVALID_VALUE;
    }
    impl::stream& s = *impl_->streams[index];
    rtp_error_t ret = s.restart->configure(enable, s.fmt, s.rce_flags);
    return ret;
}

rtp_error_t uvgrtp::receive_restart_pipeline::feed(const uint8_t* data, size_t size,
    std::chrono::steady_clock::time_point arrival)
{
    if (data == nullptr || size == 0) {
        return RTP_INVALID_VALUE;
    }
    /* Copy the datagram so the production entry point can treat it like the ring buffer slot. */
    std::vector<uint8_t> copy(data, data + size);
    rtp_error_t ret = impl_->flow->process_datagram(copy.data(), copy.size(), impl_->feed_flags, arrival);
    /* process_datagram() returns the last packet handler's result (RTP_PKT_READY etc.). For the
     * replay API only the negative error codes matter. */
    return ret < 0 ? ret : RTP_OK;
}

uvgrtp::frame::rtp_frame* uvgrtp::receive_restart_pipeline::pull(size_t index)
{
    if (index >= impl_->streams.size()) {
        return nullptr;
    }
    return impl_->flow->pull_frame_available(impl_->streams[index]->remote_ssrc->load());
}

rtp_error_t uvgrtp::receive_restart_pipeline::install_receive_hook(size_t index, void* arg,
    void (*hook)(void*, uvgrtp::frame::rtp_frame*))
{
    if (index >= impl_->streams.size()) {
        return RTP_INVALID_VALUE;
    }
    return impl_->flow->install_receive_hook(arg, hook, impl_->streams[index]->remote_ssrc->load());
}

rtp_error_t uvgrtp::receive_restart_pipeline::build_report(size_t index,
    std::unique_ptr<uint8_t[]>& bytes, size_t& size)
{
    if (index >= impl_->streams.size()) {
        return RTP_INVALID_VALUE;
    }
    uint32_t frame_size = 0;
    rtp_error_t ret = impl_->streams[index]->rtcp->build_compound_report(bytes, frame_size);
    size = frame_size;
    return ret;
}

uvgrtp::receive_restart& uvgrtp::receive_restart_pipeline::restart(size_t index)
{
    return *impl_->streams[index]->restart;
}

uvgrtp::rtcp_internal* uvgrtp::receive_restart_pipeline::rtcp(size_t index)
{
    return impl_->streams[index]->rtcp.get();
}

uvgrtp::formats::media* uvgrtp::receive_restart_pipeline::media(size_t index)
{
    return impl_->streams[index]->media.get();
}

uint32_t uvgrtp::receive_restart_pipeline::local_ssrc(size_t index) const
{
    return impl_->streams[index]->local_ssrc->load();
}

uint32_t uvgrtp::receive_restart_pipeline::remote_ssrc(size_t index) const
{
    return impl_->streams[index]->remote_ssrc->load();
}

rtp_format_t uvgrtp::receive_restart_pipeline::format(size_t index) const
{
    return impl_->streams[index]->fmt;
}

int uvgrtp::receive_restart_pipeline::rce_flags(size_t index) const
{
    return impl_->streams[index]->rce_flags;
}
