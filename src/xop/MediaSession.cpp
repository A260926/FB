// PHZ
// 2018-9-30

#include "MediaSession.h"
#include "RtpConnection.h"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <forward_list>
#include "net/Logger.h"
#include "net/SocketUtil.h"

namespace
{

/* Internal clock unit is half a microsecond so 1/2x, 1x and 2x speed as well
 * as odd microsecond media times keep exact integer arithmetic. */
const int64_t HALF_US_PER_US = 2;

int64_t FloorDiv(int64_t value, int64_t divisor)
{
	int64_t quotient = value / divisor;
	if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) {
		--quotient;
	}
	return quotient;
}

bool IsSupportedReplaySource(xop::MediaSource* source)
{
	if (source == nullptr) {
		return false;
	}
	xop::MediaType type = source->GetMediaType();
	return type == xop::H264 || type == xop::PCMA;
}

}

using namespace xop;
using namespace std;

std::atomic_uint MediaSession::last_session_id_(1);

MediaSession::MediaSession(std::string url_suffxx)
	: suffix_(url_suffxx)
	, media_sources_(MAX_MEDIA_CHANNEL)
	, buffer_(MAX_MEDIA_CHANNEL)
{
	has_new_client_ = false;
	session_id_ = ++last_session_id_;

	for(int n=0; n<MAX_MEDIA_CHANNEL; n++) {
		multicast_port_[n] = 0;
	}
}

MediaSession* MediaSession::CreateNew(std::string url_suffix)
{
	return new MediaSession(std::move(url_suffix));
}

MediaSession::~MediaSession()
{
	if (multicast_ip_ != "") {
		MulticastAddr::instance().Release(multicast_ip_);
	}
}

void MediaSession::AddNotifyConnectedCallback(const NotifyConnectedCallback& callback)
{
	notify_connected_callbacks_.push_back(callback);
}

void MediaSession::AddNotifyDisconnectedCallback(const NotifyDisconnectedCallback& callback)
{
	notify_disconnected_callbacks_.push_back(callback);
}

bool MediaSession::AddSource(MediaChannelId channel_id, MediaSource* source)
{
	if (replay_enabled_ || (int)channel_id >= MAX_MEDIA_CHANNEL || source == nullptr) {
		return false;
	}

	source->SetSendFrameCallback([this](MediaChannelId channel_id, RtpPacket pkt) {
		std::forward_list<std::shared_ptr<RtpConnection>> clients;
		std::map<int, RtpPacket> packets;
		{
			std::lock_guard<std::mutex> lock(map_mutex_);
			for (auto iter = clients_.begin(); iter != clients_.end();) {
				auto conn = iter->second.lock();
				if (conn == nullptr) {
					clients_.erase(iter++);
				}
				else  {				
					int id = conn->GetId();
					if (id >= 0) {
						if (packets.find(id) == packets.end()) {
							RtpPacket tmp_pkt;
							memcpy(tmp_pkt.data.get(), pkt.data.get(), pkt.size);
							tmp_pkt.size = pkt.size;
							tmp_pkt.last = pkt.last;
							tmp_pkt.timestamp = pkt.timestamp;
							tmp_pkt.type = pkt.type;
							packets.emplace(id, tmp_pkt);
						}
						clients.emplace_front(conn);
					}
					iter++;
				}
			}
		}
        
		int count = 0;
		for(auto iter : clients) {
			int ret = 0;
			int id = iter->GetId();
			if (id >= 0) {
				auto iter2 = packets.find(id);
				if (iter2 != packets.end()) {
					count++;
					ret = iter->SendRtpPacket(channel_id, iter2->second);
					if (is_multicast_ && ret == 0) {
						break;
					}				
				}
			}					
		}
		return true;
		});

	media_sources_[channel_id].reset(source);
	return true;
}

bool MediaSession::RemoveSource(MediaChannelId channel_id)
{
	if (replay_enabled_ || (int)channel_id >= MAX_MEDIA_CHANNEL) {
		return false;
	}
	media_sources_[channel_id] = nullptr;
	return true;
}

bool MediaSession::StartMulticast()
{  
	if (is_multicast_) {
		return true;
	}

	multicast_ip_ = MulticastAddr::instance().GetAddr();
	if (multicast_ip_ == "") {
		return false;
	}

	std::random_device rd;
	multicast_port_[channel_0] = htons(rd() & 0xfffe);
	multicast_port_[channel_1] = htons(rd() & 0xfffe);

	is_multicast_ = true;
	return true;
}

std::string MediaSession::GetSdpMessage(std::string ip, std::string session_name)
{
	if (sdp_ != "") {
		return sdp_;
	}
    
	if (media_sources_.empty()) {
		return "";
	}

	char buf[2048] = {0};

	snprintf(buf, sizeof(buf),
			"v=0\r\n"
			"o=- 9%ld 1 IN IP4 %s\r\n"
			"t=0 0\r\n"
			"a=control:*\r\n" ,
			(long)std::time(NULL), ip.c_str()); 

	if(session_name != "") {
		snprintf(buf+strlen(buf), sizeof(buf)-strlen(buf), 
				"s=%s\r\n",
				session_name.c_str());
	}
    
	if(is_multicast_) {
		snprintf(buf+strlen(buf), sizeof(buf)-strlen(buf),
				"a=type:broadcast\r\n"
				"a=rtcp-unicast: reflection\r\n");
	}
		
	for (uint32_t chn=0; chn<media_sources_.size(); chn++) {
		if(media_sources_[chn]) {	
			if(is_multicast_) {
				snprintf(buf+strlen(buf), sizeof(buf)-strlen(buf), 
						"%s\r\n",
						media_sources_[chn]->GetMediaDescription(multicast_port_[chn]).c_str()); 
                     
				snprintf(buf+strlen(buf), sizeof(buf)-strlen(buf), 
						"c=IN IP4 %s/255\r\n",
						multicast_ip_.c_str()); 
			}
			else {
				snprintf(buf+strlen(buf), sizeof(buf)-strlen(buf), 
						"%s\r\n",
						media_sources_[chn]->GetMediaDescription(0).c_str());
			}
            
			snprintf(buf+strlen(buf), sizeof(buf)-strlen(buf), 
					"%s\r\n",
					media_sources_[chn]->GetAttribute().c_str());
                     
			snprintf(buf+strlen(buf), sizeof(buf)-strlen(buf),											
					"a=control:track%d\r\n", chn);	
		}
	}

	sdp_ = buf;
	return sdp_;
}

MediaSource* MediaSession::GetMediaSource(MediaChannelId channel_id)
{
	if ((int)channel_id >= MAX_MEDIA_CHANNEL) {
		return nullptr;
	}
	if (media_sources_[channel_id]) {
		return media_sources_[channel_id].get();
	}

	return nullptr;
}

bool MediaSession::HandleFrame(MediaChannelId channel_id, AVFrame frame)
{
	if (replay_in_callback_) {
		return false;
	}

	std::lock_guard<std::mutex> lock(mutex_);

	if (replay_enabled_ || (int)channel_id >= MAX_MEDIA_CHANNEL) {
		return false;
	}

	if(media_sources_[channel_id]) {
		media_sources_[channel_id]->HandleFrame(channel_id, frame);
	}
	else {
		return false;
	}

	return true;
}

bool MediaSession::EnableTimedReplay(const ReplayConfig& config)
{
	/* Enabling from inside a sink callback is never allowed. */
	if (replay_in_callback_) {
		return false;
	}

	std::lock_guard<std::mutex> lock(mutex_);

	if (replay_enabled_) {
		return false;
	}

	{
		std::lock_guard<std::mutex> client_lock(map_mutex_);
		if (!clients_.empty()) {
			return false;
		}
	}

	if (config.media_start_us < 0 || config.wall_start_us < 0) {
		return false;
	}

	if (config.max_pending_frames == 0 || config.max_pending_bytes == 0) {
		return false;
	}

	if (!config.sink) {
		return false;
	}

	if (config.tracks.empty() || config.tracks.size() > (size_t)MAX_MEDIA_CHANNEL) {
		return false;
	}

	bool seen_channel[MAX_MEDIA_CHANNEL] = { false, false };
	bool seen_source[MAX_MEDIA_CHANNEL] = { false, false };

	for (size_t i = 0; i < config.tracks.size(); i++) {
		const ReplayTrackConfig& track = config.tracks[i];

		if ((int)track.channel >= MAX_MEDIA_CHANNEL) {
			return false;
		}

		if (seen_channel[track.channel]) {
			return false;
		}
		seen_channel[track.channel] = true;

		MediaSource* source = media_sources_[track.channel].get();
		if (!IsSupportedReplaySource(source)) {
			return false;
		}
		seen_source[track.channel] = true;
	}

	/* The configured tracks correspond exactly to the added sources. */
	for (int chn = 0; chn < MAX_MEDIA_CHANNEL; chn++) {
		if ((media_sources_[chn] != nullptr) && !seen_source[chn]) {
			return false;
		}
	}

	for (int chn = 0; chn < MAX_MEDIA_CHANNEL; chn++) {
		replay_tracks_[chn] = ReplayTrack();
	}

	for (size_t i = 0; i < config.tracks.size(); i++) {
		const ReplayTrackConfig& track = config.tracks[i];
		MediaSource* source = media_sources_[track.channel].get();
		ReplayTrack& state = replay_tracks_[track.channel];

		state.configured            = true;
		state.ssrc                  = track.ssrc;
		state.next_seq              = track.initial_sequence;
		state.initial_rtp_timestamp = track.initial_rtp_timestamp;
		state.payload_type          = source->GetPayloadType();
		state.clock_rate            = source->GetClockRate();
		state.front_half_us         = config.media_start_us * HALF_US_PER_US;
		state.frames.clear();
	}

	replay_sink_             = config.sink;
	replay_max_frames_       = config.max_pending_frames;
	replay_max_bytes_        = config.max_pending_bytes;
	replay_media_start_us_   = config.media_start_us;
	replay_wall_start_us_    = config.wall_start_us;
	replay_anchor_half_us_   = config.media_start_us * HALF_US_PER_US;
	replay_anchor_wall_us_   = config.wall_start_us;
	replay_speed_num_        = 1;
	replay_speed_den_        = 1;
	replay_playing_          = true;
	replay_pending_frames_   = 0;
	replay_pending_bytes_    = 0;
	replay_order_            = 0;
	replay_generation_       = 0;
	replay_in_callback_      = false;
	replay_has_current_      = false;
	replay_presented_us_     = config.media_start_us;
	replay_enabled_          = true;

	return true;
}

int64_t MediaSession::ReplayPositionHalf(int64_t now_us) const
{
	if (!replay_playing_) {
		return replay_anchor_half_us_;
	}

	/* position = anchor + (now - anchor_wall) * speed, in half microseconds. */
	int64_t delta_us = now_us - replay_anchor_wall_us_;
	if (delta_us < 0) {
		return replay_anchor_half_us_;
	}

	int64_t delta_half = delta_us * HALF_US_PER_US;
	int64_t scaled = FloorDiv(delta_half * (int64_t)replay_speed_num_,
	                          (int64_t)replay_speed_den_);
	return replay_anchor_half_us_ + scaled;
}

bool MediaSession::EnqueueReplay(MediaChannelId channel_id, const AVFrame& frame, int64_t media_us)
{
	if (replay_in_callback_) {
		return false;
	}

	std::lock_guard<std::mutex> lock(mutex_);

	if (!replay_enabled_ || (int)channel_id >= MAX_MEDIA_CHANNEL) {
		return false;
	}

	ReplayTrack& track = replay_tracks_[channel_id];
	MediaSource* source = media_sources_[channel_id].get();
	if (!track.configured || !IsSupportedReplaySource(source)) {
		return false;
	}

	if (media_us < 0 || media_us < replay_media_start_us_) {
		return false;
	}

	int64_t media_half = media_us * HALF_US_PER_US;
	if (media_half < track.front_half_us) {
		return false;
	}

	/* Never behind a frame that has already been shown to the sink. */
	if (media_us < replay_presented_us_) {
		return false;
	}

	if (frame.buffer == nullptr || frame.size == 0) {
		return false;
	}

	uint32_t max_size = (source->GetMediaType() == H264)
	                    ? (256u * 1024u) : (uint32_t)MAX_RTP_PAYLOAD_SIZE;
	if (frame.size > max_size) {
		return false;
	}

	/* Snapshot the frame; the caller may release its buffer afterwards. */
	AVFrame copy(frame.size);
	copy.type = frame.type;
	memcpy(copy.buffer.get(), frame.buffer.get(), frame.size);

	int64_t delta_us = media_us - replay_media_start_us_;
	/* offset = floor((media_us - M) * clock_rate / 1000000). */
	int64_t whole_seconds = delta_us / 1000000;
	int64_t remainder_us  = delta_us % 1000000;
	uint32_t offset = (uint32_t)(
		(uint64_t)track.clock_rate * (uint64_t)whole_seconds +
		((uint64_t)track.clock_rate * (uint64_t)remainder_us) / 1000000ULL);
	uint32_t rtp_timestamp = (uint32_t)(track.initial_rtp_timestamp + offset);
	copy.timestamp = rtp_timestamp;

	ReplayFrame queued;
	queued.media_us      = media_us;
	queued.rtp_timestamp = rtp_timestamp;
	queued.order         = replay_order_;
	queued.byte_cost     = 0;
	queued.next_packet   = 0;

	bool ok = true;
	MediaSource::SendFrameCallback collect = [&](MediaChannelId cb_channel, RtpPacket pkt) -> bool {
		if (cb_channel != channel_id) {
			ok = false;
			return false;
		}

		ReplayPacket entry;
		entry.packet = pkt;
		entry.packet.timestamp = rtp_timestamp;
		entry.seq = 0;
		entry.seq_assigned = false;

		queued.packets.push_back(entry);
		queued.byte_cost += pkt.size;
		return true;
	};

	if (!source->PacketizeFrame(channel_id, copy, collect) || !ok || queued.packets.empty()) {
		return false;
	}

	if (replay_pending_frames_ + 1 > replay_max_frames_) {
		return false;
	}

	if (replay_pending_bytes_ + (uint64_t)queued.byte_cost > replay_max_bytes_) {
		return false;
	}

	track.frames.push_back(queued);
	track.front_half_us = media_half;
	replay_pending_frames_++;
	replay_pending_bytes_ += queued.byte_cost;
	replay_order_++;

	return true;
}

bool MediaSession::ReplayFillHeader(ReplayPacket& pkt, const ReplayTrack& track,
                                    uint32_t rtp_timestamp) const
{
	RtpHeader header;
	memset(&header, 0, sizeof(header));
	header.version = RTP_VERSION;
	header.payload = (unsigned char)track.payload_type;
	header.marker  = pkt.packet.last;
	header.seq     = htons(pkt.seq);
	header.ts      = htonl(rtp_timestamp);
	header.ssrc    = htonl(track.ssrc);

	/* The RTP header lives at data+4; the four byte TCP prefix is zero. */
	memset(pkt.packet.data.get(), 0, RTP_TCP_HEAD_SIZE);
	memcpy(pkt.packet.data.get() + RTP_TCP_HEAD_SIZE, &header, RTP_HEADER_SIZE);
	return true;
}

MediaSession::ReplayFrame* MediaSession::ReplayFindCurrent()
{
	if (!replay_has_current_) {
		return nullptr;
	}

	ReplayTrack& track = replay_tracks_[replay_current_channel_];
	if (!track.configured || track.frames.empty()) {
		return nullptr;
	}

	ReplayFrame& front = track.frames.front();
	if (front.order != replay_current_order_) {
		return nullptr;
	}
	return &front;
}

MediaSession::ReplayFrame* MediaSession::ReplaySelectDue(int64_t position_half_us)
{
	ReplayFrame* best = nullptr;
	MediaChannelId best_channel = channel_0;

	for (int chn = 0; chn < MAX_MEDIA_CHANNEL; chn++) {
		ReplayTrack& track = replay_tracks_[chn];
		if (!track.configured || track.frames.empty()) {
			continue;
		}

		ReplayFrame& front = track.frames.front();
		if ((int64_t)front.media_us * HALF_US_PER_US > position_half_us) {
			continue;
		}

		if (best == nullptr) {
			best = &front;
			best_channel = (MediaChannelId)chn;
			continue;
		}

		/* Due ordering: media_us, then channel, then enqueue order. */
		if (front.media_us < best->media_us ||
		    (front.media_us == best->media_us && (int)chn < (int)best_channel)) {
			best = &front;
			best_channel = (MediaChannelId)chn;
		}
	}

	if (best == nullptr) {
		return nullptr;
	}

	replay_has_current_ = true;
	replay_current_channel_ = best_channel;
	replay_current_order_ = best->order;
	return best;
}

void MediaSession::ReplayCompleteFront(MediaChannelId channel_id)
{
	ReplayTrack& track = replay_tracks_[channel_id];
	ReplayFrame& front = track.frames.front();

	replay_pending_frames_--;
	replay_pending_bytes_ -= front.byte_cost;
	track.frames.pop_front();

	replay_has_current_ = false;
}

void MediaSession::PauseReplayImpl(int64_t now_us)
{
	if (!replay_enabled_ || !replay_playing_ || now_us < replay_wall_start_us_) {
		return;
	}

	replay_anchor_half_us_ = ReplayPositionHalf(now_us);
	replay_anchor_wall_us_ = now_us;
	replay_playing_ = false;
}

void MediaSession::ResumeReplayImpl(int64_t now_us)
{
	if (!replay_enabled_ || replay_playing_ || now_us < replay_wall_start_us_) {
		return;
	}

	replay_anchor_wall_us_ = now_us;
	replay_playing_ = true;
}

bool MediaSession::SetReplaySpeedImpl(int64_t now_us, uint32_t numerator, uint32_t denominator)
{
	if (!replay_enabled_ || now_us < replay_wall_start_us_) {
		return false;
	}

	if (denominator == 0) {
		return false;
	}

	if (!((numerator == 1 && denominator == 2) ||
	      (numerator == 1 && denominator == 1) ||
	      (numerator == 2 && denominator == 1))) {
		return false;
	}

	/* Move the clock at the old speed up to now, then switch.  While paused
	 * only the stored speed changes; it takes effect after Resume. */
	if (replay_playing_) {
		replay_anchor_half_us_ = ReplayPositionHalf(now_us);
	}
	replay_anchor_wall_us_ = now_us;
	replay_speed_num_ = numerator;
	replay_speed_den_ = denominator;

	return true;
}

void MediaSession::ClearReplayImpl(int64_t now_us)
{
	if (!replay_enabled_ || now_us < replay_wall_start_us_) {
		return;
	}

	/* Advance the playback clock first, at the current speed. */
	if (replay_playing_) {
		replay_anchor_half_us_ = ReplayPositionHalf(now_us);
	}
	replay_anchor_wall_us_ = now_us;

	const int64_t position_half = replay_anchor_half_us_;
	const int64_t position_us = position_half / HALF_US_PER_US;

	for (int chn = 0; chn < MAX_MEDIA_CHANNEL; chn++) {
		ReplayTrack& track = replay_tracks_[chn];
		track.frames.clear();
		if (track.configured) {
			track.front_half_us = position_half;
		}
	}

	replay_pending_frames_ = 0;
	replay_pending_bytes_ = 0;
	replay_has_current_ = false;
	replay_presented_us_ = position_us;
	replay_generation_++;
}

uint32_t MediaSession::PumpReplay(int64_t now_us)
{
	if (replay_in_callback_) {
		return 0;
	}

	std::lock_guard<std::mutex> lock(mutex_);

	if (!replay_enabled_ || now_us < replay_wall_start_us_) {
		return 0;
	}

	if (!replay_playing_) {
		return 0;
	}

	const uint32_t generation = replay_generation_;
	const int64_t position_half = ReplayPositionHalf(now_us);
	uint32_t accepted = 0;

	while (replay_playing_ && replay_generation_ == generation) {
		ReplayFrame* frame = ReplayFindCurrent();
		if (frame == nullptr) {
			frame = ReplaySelectDue(position_half);
			if (frame == nullptr) {
				break;
			}
		}

		const MediaChannelId channel_id = replay_current_channel_;
		ReplayTrack& track = replay_tracks_[channel_id];

		/* This frame is now being shown to the sink. */
		if (frame->media_us > replay_presented_us_) {
			replay_presented_us_ = frame->media_us;
		}

		bool stopped = false;
		bool frame_complete = false;
		while (frame->next_packet < frame->packets.size()) {
			ReplayPacket& entry = frame->packets[frame->next_packet];

			if (!entry.seq_assigned) {
				entry.seq = track.next_seq;
				entry.seq_assigned = true;
			}
			ReplayFillHeader(entry, track, frame->rtp_timestamp);

			replay_in_callback_ = true;
			const bool ok = replay_sink_(channel_id, entry.packet);
			replay_in_callback_ = false;

			const bool cleared = (replay_generation_ != generation);

			/* Commit the packet that returned true even when the callback
			 * cleared replay; its frame storage may already be gone, so the
			 * per-frame cursor must not be touched in that case. */
			if (ok) {
				accepted++;
				track.next_seq++;
				if (!cleared) {
					frame->next_packet++;
				}
			}

			/* A clear dropped this frame: never touch it again. */
			if (cleared) {
				stopped = true;
				break;
			}

			/* The last fragment was accepted: the frame's quota is released
			 * as soon as it completes, before handling pause below. */
			if (frame->next_packet >= frame->packets.size()) {
				frame_complete = true;
				break;
			}

			/* Pause inside the callback stops immediately after the packet
			 * that was just committed.  A rejected packet never takes a
			 * sequence number and is retried on the next pump. */
			if (!ok || !replay_playing_) {
				stopped = true;
				break;
			}

			if (!replay_has_current_ ||
			    replay_current_channel_ != channel_id ||
			    ReplayFindCurrent() != frame) {
				stopped = true;
				break;
			}
		}

		if (!stopped && frame_complete) {
			ReplayCompleteFront(channel_id);
		}

		if (stopped) {
			break;
		}
	}

	return accepted;
}

void MediaSession::PauseReplay(int64_t now_us)
{
	if (replay_in_callback_) {
		PauseReplayImpl(now_us);
		return;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	PauseReplayImpl(now_us);
}

void MediaSession::ResumeReplay(int64_t now_us)
{
	/* Only PauseReplay and ClearReplay are permitted from a sink callback. */
	if (replay_in_callback_) {
		return;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	ResumeReplayImpl(now_us);
}

bool MediaSession::SetReplaySpeed(int64_t now_us, uint32_t numerator, uint32_t denominator)
{
	if (replay_in_callback_) {
		return false;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	return SetReplaySpeedImpl(now_us, numerator, denominator);
}

void MediaSession::ClearReplay(int64_t now_us)
{
	if (replay_in_callback_) {
		ClearReplayImpl(now_us);
		return;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	ClearReplayImpl(now_us);
}

bool MediaSession::AddClient(SOCKET rtspfd, std::shared_ptr<RtpConnection> rtp_conn)
{
	if (replay_enabled_) {
		return false;
	}

	std::lock_guard<std::mutex> lock(map_mutex_);

	auto iter = clients_.find (rtspfd);
	if(iter == clients_.end()) {
		std::weak_ptr<RtpConnection> rtp_conn_weak_ptr = rtp_conn;
		clients_.emplace(rtspfd, rtp_conn_weak_ptr);
		for (auto& callback : notify_connected_callbacks_) {
			callback(session_id_, rtp_conn->GetIp(), rtp_conn->GetPort());
		}			
        
		has_new_client_ = true;
		return true;
	}
            
	return false;
}

void MediaSession::RemoveClient(SOCKET rtspfd)
{  
	std::lock_guard<std::mutex> lock(map_mutex_);

	auto iter = clients_.find(rtspfd);
	if (iter != clients_.end()) {
		auto conn = iter->second.lock();
		if (conn) {
			for (auto& callback : notify_disconnected_callbacks_) {
				callback(session_id_, conn->GetIp(), conn->GetPort());
			}				
		}
		clients_.erase(iter);
	}
}

