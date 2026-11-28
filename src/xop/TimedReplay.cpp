// Timed replay: application-driven in-memory playback for MediaSession.
//
// Frames are packetized once at enqueue time through the production
// H264Source/G711ASource packetizers (MediaSource::PacketizeFrame) and
// the resulting RtpPacket snapshots are queued on a single media
// timeline. PumpReplay() advances a manual microsecond play clock and
// stamps each due packet with RtpConnection::FillRtpHeader() before
// offering it to the configured sink. Single threaded; no network I/O.

#include "MediaSession.h"
#include "RtpConnection.h"
#include <cstring>

using namespace xop;

bool MediaSession::EnableTimedReplay(const TimedReplayConfig& config)
{
	if (replay_enabled_ || replay_in_sink_) {
		return false;
	}

	if (config.media_origin_us < 0 || config.clock_origin_us < 0) {
		return false;
	}

	if (config.max_frames == 0 || config.max_bytes == 0) {
		return false;
	}

	if (!config.sink) {
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(map_mutex_);
		if (!clients_.empty()) {
			return false;
		}
	}

	size_t source_count = 0;
	for (int chn = 0; chn < MAX_MEDIA_CHANNEL; chn++) {
		if (media_sources_[chn]) {
			source_count++;
		}
	}

	// Track configs must correspond exactly to the added sources.
	if (source_count == 0 || config.tracks.size() != source_count) {
		return false;
	}

	bool seen[MAX_MEDIA_CHANNEL] = { false };
	for (const auto& track : config.tracks) {
		int chn = (int)track.channel;
		if (chn < 0 || chn >= MAX_MEDIA_CHANNEL || seen[chn]) {
			return false;
		}
		seen[chn] = true;

		MediaSource* source = media_sources_[chn].get();
		if (source == nullptr) {
			return false;
		}

		MediaType type = source->GetMediaType();
		if (type != H264 && type != PCMA) {
			return false;
		}
	}

	replay_origin_us_ = config.media_origin_us;
	replay_clock_origin_us_ = config.clock_origin_us;
	replay_max_frames_ = config.max_frames;
	replay_max_bytes_ = config.max_bytes;
	replay_sink_ = config.sink;

	replay_position_half_us_ = 2 * config.media_origin_us;
	replay_last_now_us_ = config.clock_origin_us;
	replay_rate_half_ = 2; // 1x
	replay_paused_ = false;
	replay_frames_ = 0;
	replay_bytes_ = 0;
	replay_presented_us_ = config.media_origin_us;
	replay_order_ = 0;
	replay_generation_ = 0;
	replay_capture_ = nullptr;
	replay_queue_.clear();

	for (int chn = 0; chn < MAX_MEDIA_CHANNEL; chn++) {
		replay_tracks_[chn] = ReplayTrackState();
	}

	for (const auto& track : config.tracks) {
		int chn = (int)track.channel;
		ReplayTrackState& state = replay_tracks_[chn];
		MediaSource* source = media_sources_[chn].get();

		state.configured = true;
		state.ssrc = track.ssrc;
		state.next_sequence = track.initial_sequence;
		state.initial_rtp_timestamp = track.initial_rtp_timestamp;
		state.clock_rate = source->GetClockRate();
		state.enqueue_frontier_us = config.media_origin_us;

		memset(&state.rtp_header, 0, sizeof(state.rtp_header));
		state.rtp_header.version = RTP_VERSION;
		state.rtp_header.payload = source->GetPayloadType();
		state.rtp_header.ssrc = htonl(track.ssrc);
	}

	// Detach the live broadcast path; replay packetization is captured
	// into a per-enqueue snapshot instead.
	for (int chn = 0; chn < MAX_MEDIA_CHANNEL; chn++) {
		if (media_sources_[chn]) {
			media_sources_[chn]->SetSendFrameCallback(
				[this](MediaChannelId, RtpPacket pkt) {
					if (replay_capture_) {
						replay_capture_->push_back(pkt);
					}
					return true;
				});
		}
	}

	replay_enabled_ = true;
	return true;
}

bool MediaSession::EnqueueReplay(MediaChannelId channel_id, AVFrame frame, int64_t media_us)
{
	if (!replay_enabled_ || replay_in_sink_) {
		return false;
	}

	int chn = (int)channel_id;
	if (chn < 0 || chn >= MAX_MEDIA_CHANNEL) {
		return false;
	}

	ReplayTrackState& track = replay_tracks_[chn];
	if (!track.configured) {
		return false;
	}

	MediaSource* source = media_sources_[chn].get();
	if (source == nullptr) {
		return false;
	}

	MediaType type = source->GetMediaType();
	if (type == H264) {
		if (frame.buffer == nullptr || frame.size == 0 || frame.size > 256 * 1024) {
			return false;
		}
	}
	else if (type == PCMA) {
		if (frame.buffer == nullptr || frame.size == 0 || frame.size > MAX_RTP_PAYLOAD_SIZE) {
			return false;
		}
	}
	else {
		return false;
	}

	if (media_us < replay_origin_us_ ||
		media_us < replay_presented_us_ ||
		media_us < track.enqueue_frontier_us) {
		return false;
	}

	// A partially delivered frame must finish before any frame that
	// would sort ahead of it on the same media instant.
	if (!replay_queue_.empty()) {
		const ReplayFrame& front = replay_queue_.front();
		if (front.next_packet > 0 && media_us == front.media_us && chn < (int)front.channel) {
			return false;
		}
	}

	// RTP timestamp: initial + floor((media_us - M) * clock_rate / 1e6),
	// 32-bit wrap, independent of playback speed. AVFrame.timestamp of
	// the input is not used.
	uint64_t delta_us = (uint64_t)(media_us - replay_origin_us_);
	uint32_t rtp_timestamp = track.initial_rtp_timestamp +
		(uint32_t)((delta_us * track.clock_rate) / 1000000);

	AVFrame snapshot_frame = frame;
	snapshot_frame.timestamp = rtp_timestamp;

	// Packetize through the production packetizer; the produced packets
	// own copies of the payload, so the caller's frame may be reused or
	// freed as soon as this call returns.
	std::vector<RtpPacket> packets;
	replay_capture_ = &packets;
	bool ok = source->PacketizeFrame(channel_id, snapshot_frame);
	replay_capture_ = nullptr;

	if (!ok || packets.empty()) {
		return false;
	}

	uint64_t bytes = 0;
	for (const auto& pkt : packets) {
		bytes += pkt.size;
	}

	if (replay_frames_ + 1 > replay_max_frames_ ||
		replay_bytes_ + bytes > replay_max_bytes_) {
		return false;
	}

	ReplayFrame replay_frame;
	replay_frame.channel = channel_id;
	replay_frame.media_us = media_us;
	replay_frame.order = replay_order_++;
	replay_frame.bytes = bytes;
	replay_frame.packets = std::move(packets);
	replay_frame.next_packet = 0;

	// Keep the queue sorted by (media_us, channel, enqueue order).
	auto iter = replay_queue_.begin();
	for (; iter != replay_queue_.end(); ++iter) {
		if (iter->media_us > media_us ||
			(iter->media_us == media_us && (int)iter->channel > chn)) {
			break;
		}
	}
	replay_queue_.insert(iter, std::move(replay_frame));

	replay_frames_ += 1;
	replay_bytes_ += bytes;
	track.enqueue_frontier_us = media_us;
	return true;
}

uint32_t MediaSession::PumpReplay(int64_t now_us)
{
	if (!replay_enabled_ || replay_in_sink_ || replay_paused_) {
		return 0;
	}

	AdvanceReplayClock(now_us);

	uint32_t accepted = 0;
	uint64_t generation = replay_generation_;

	while (!replay_queue_.empty()) {
		ReplayFrame& front = replay_queue_.front();
		if (front.media_us > ReplayPositionUs()) {
			break;
		}

		ReplayTrackState& track = replay_tracks_[(int)front.channel];
		RtpPacket& pkt = front.packets[front.next_packet];

		// RTP header at data+4, reserved 4-byte prefix zeroed.
		memset(pkt.data.get(), 0, RTP_TCP_HEAD_SIZE);
		RtpConnection::FillRtpHeader(pkt, track.rtp_header, track.next_sequence);

		if (front.media_us > replay_presented_us_) {
			replay_presented_us_ = front.media_us;
		}

		replay_in_sink_ = true;
		bool taken = replay_sink_(front.channel, pkt);
		replay_in_sink_ = false;

		if (!taken) {
			break; // retry this exact packet on the next pump
		}

		accepted++;
		track.next_sequence = (uint16_t)(track.next_sequence + 1);

		if (replay_generation_ != generation) {
			// ClearReplay() inside the sink: the accepted packet still
			// counts, but the queue (and this frame's suffix) is gone.
			break;
		}

		front.next_packet += 1;
		if (front.next_packet == front.packets.size()) {
			// Frame finished: release its capacity quota.
			replay_frames_ -= 1;
			replay_bytes_ -= front.bytes;
			replay_queue_.pop_front();
		}

		if (replay_paused_) {
			// PauseReplay() inside the sink: commit done, stop now.
			break;
		}
	}

	return accepted;
}

bool MediaSession::PauseReplay(int64_t now_us)
{
	if (!replay_enabled_) {
		return false;
	}

	if (replay_paused_) {
		return true; // repeated pause: no extra effect
	}

	AdvanceReplayClock(now_us);
	replay_paused_ = true;
	return true;
}

bool MediaSession::ResumeReplay(int64_t now_us)
{
	if (!replay_enabled_ || replay_in_sink_) {
		return false;
	}

	if (!replay_paused_) {
		return true; // repeated resume: no extra effect
	}

	if (now_us < replay_clock_origin_us_) {
		now_us = replay_clock_origin_us_;
	}
	if (now_us < replay_last_now_us_) {
		now_us = replay_last_now_us_;
	}

	replay_last_now_us_ = now_us;
	replay_paused_ = false;
	return true;
}

bool MediaSession::SetReplaySpeed(double speed, int64_t now_us)
{
	if (!replay_enabled_ || replay_in_sink_) {
		return false;
	}

	int rate_half = 0;
	if (speed == 0.5) {
		rate_half = 1;
	}
	else if (speed == 1.0) {
		rate_half = 2;
	}
	else if (speed == 2.0) {
		rate_half = 4;
	}
	else {
		return false; // invalid speed: no state changes
	}

	if (!replay_paused_) {
		AdvanceReplayClock(now_us); // advance at the old speed first
	}

	replay_rate_half_ = rate_half;
	return true;
}

bool MediaSession::ClearReplay(int64_t now_us)
{
	if (!replay_enabled_) {
		return false;
	}

	AdvanceReplayClock(now_us); // the play clock advances first

	replay_queue_.clear();
	replay_frames_ = 0;
	replay_bytes_ = 0;
	replay_generation_ += 1;

	// Clock, speed and committed sequence numbers are kept; each track's
	// enqueue frontier resets to the current media position.
	int64_t frontier = ReplayPositionUs();
	for (int chn = 0; chn < MAX_MEDIA_CHANNEL; chn++) {
		replay_tracks_[chn].enqueue_frontier_us = frontier;
	}

	return true;
}

void MediaSession::AdvanceReplayClock(int64_t now_us)
{
	if (now_us < replay_clock_origin_us_) {
		now_us = replay_clock_origin_us_;
	}
	if (now_us < replay_last_now_us_) {
		now_us = replay_last_now_us_;
	}

	if (!replay_paused_) {
		int64_t delta = now_us - replay_last_now_us_;
		replay_position_half_us_ += delta * replay_rate_half_;
	}

	replay_last_now_us_ = now_us;
}
