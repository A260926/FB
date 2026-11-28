// PHZ
// 2018-6-8

#ifndef XOP_MEDIA_SESSION_H
#define XOP_MEDIA_SESSION_H

#include <memory>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <random>
#include <cstdint>
#include <functional>
#include <unordered_set>
#include "media.h"
#include "H264Source.h"
#include "H265Source.h"
#include "VP8Source.h"
#include "G711ASource.h"
#include "AACSource.h"
#include "MediaSource.h"
#include "net/Socket.h"
#include "net/RingBuffer.h"

namespace xop
{

class RtpConnection;

class MediaSession
{
public:
	using Ptr = std::shared_ptr<MediaSession>;
	using NotifyConnectedCallback = std::function<void (MediaSessionId sessionId, std::string peer_ip, uint16_t peer_port)> ;
	using NotifyDisconnectedCallback = std::function<void (MediaSessionId sessionId, std::string peer_ip, uint16_t peer_port)> ;

	static MediaSession* CreateNew(std::string url_suffix="live");
	virtual ~MediaSession();

	bool AddSource(MediaChannelId channel_id, MediaSource* source);
	bool RemoveSource(MediaChannelId channel_id);

	bool StartMulticast();

	void AddNotifyConnectedCallback(const NotifyConnectedCallback& callback);
	void AddNotifyDisconnectedCallback(const NotifyDisconnectedCallback& callback);

	std::string GetRtspUrlSuffix() const
	{ return suffix_; }

	void SetRtspUrlSuffix(std::string& suffix)
	{ suffix_ = suffix; }

	std::string GetSdpMessage(std::string ip, std::string session_name ="");

	MediaSource* GetMediaSource(MediaChannelId channel_id);

	bool HandleFrame(MediaChannelId channel_id, AVFrame frame);

	// ---- Timed replay (application-driven in-memory playback) ----
	//
	// Optional mode that replays a single audio/video timeline into
	// memory instead of live network clients. Frames are packetized by
	// the production H264Source/G711ASource packetizers at enqueue time
	// and delivered to a sink callback as the application advances a
	// manual microsecond clock with PumpReplay(). Only single H264 NAL
	// and PCMA inputs are supported. Single threaded; no RTSP text
	// commands, decoders or network I/O are involved.
	//
	// The mode must be enabled after all sources were added and while
	// the session has no network clients. Once enabled, HandleFrame(),
	// AddSource()/RemoveSource() and re-enabling are rejected; while not
	// enabled, all replay entry points below are rejected.

	// Per-track replay parameters. Tracks must correspond exactly to the
	// sources added with AddSource() and channels must not repeat.
	struct TimedReplayTrackConfig
	{
		MediaChannelId channel = channel_0;
		uint32_t ssrc = 0;                // SSRC written into every RTP header of this track
		uint16_t initial_sequence = 0;    // first RTP sequence number of this track
		uint32_t initial_rtp_timestamp = 0; // RTP timestamp of a frame at media_origin_us
	};

	struct TimedReplayConfig
	{
		int64_t  media_origin_us = 0;  // M: common media timeline origin, >= 0
		int64_t  clock_origin_us = 0;  // W: local clock origin, >= 0; play clock is M at W
		uint32_t max_frames = 0;       // > 0; unfinished queued frames limit
		uint64_t max_bytes  = 0;       // > 0; sum of RtpPacket.size (incl. 4-byte prefix) limit
		std::vector<TimedReplayTrackConfig> tracks;
		// Returns true when the whole packet was taken over, false when it
		// was not accepted (the packet is offered again on the next pump).
		std::function<bool (MediaChannelId channel, const RtpPacket& packet)> sink;
	};

	bool EnableTimedReplay(const TimedReplayConfig& config);
	bool IsTimedReplayEnabled() const
	{ return replay_enabled_; }

	// Enqueues one frame for replay. media_us must be >= 0, >= M, >= the
	// largest media_us already presented to the sink and >= the last
	// media_us accepted on this track. On success the frame has been
	// snapshotted through the production packetizer and the caller may
	// reuse or free the original frame.
	bool EnqueueReplay(MediaChannelId channel_id, AVFrame frame, int64_t media_us);

	// Advances the play clock to now_us and offers every due packet to
	// the sink in (media_us, channel, enqueue order). Stops at the first
	// rejected packet. Returns the number of packets accepted this call
	// (0 while paused or when nothing is due).
	uint32_t PumpReplay(int64_t now_us);

	// Pause/resume the play clock. Repeated calls have no extra effect
	// and never send anything implicitly.
	bool PauseReplay(int64_t now_us);
	bool ResumeReplay(int64_t now_us);

	// Supported speeds: 0.5, 1.0, 2.0. Advances the clock to now_us at
	// the old speed before switching; while paused only takes effect
	// after ResumeReplay(). Invalid speeds change nothing.
	bool SetReplaySpeed(double speed, int64_t now_us);

	// Advances the play clock to now_us, then drops all queued frames.
	// Clock, speed and committed sequence numbers are kept; each track's
	// enqueue frontier resets to the current media position.
	bool ClearReplay(int64_t now_us);

	bool AddClient(SOCKET rtspfd, std::shared_ptr<RtpConnection> rtp_conn);
	void RemoveClient(SOCKET rtspfd);

	MediaSessionId GetMediaSessionId()
	{ return session_id_; }

	uint32_t GetNumClient() const
	{ return (uint32_t)clients_.size(); }

	bool IsMulticast() const
	{ return is_multicast_; }

	std::string GetMulticastIp() const
	{ return multicast_ip_; }

	uint16_t GetMulticastPort(MediaChannelId channel_id) const
	{
		if (channel_id >= MAX_MEDIA_CHANNEL) {
			return 0;
		}         
		return multicast_port_[channel_id];
	}

private:
	friend class MediaSource;
	friend class RtspServer;
	MediaSession(std::string url_suffxx);

	MediaSessionId session_id_ = 0;
	std::string suffix_;
	std::string sdp_;

	std::vector<std::unique_ptr<MediaSource>> media_sources_;
	std::vector<RingBuffer<AVFrame>> buffer_;

	std::vector<NotifyConnectedCallback> notify_connected_callbacks_;
	std::vector<NotifyDisconnectedCallback> notify_disconnected_callbacks_;
	std::mutex mutex_;
	std::mutex map_mutex_;
	std::map<SOCKET, std::weak_ptr<RtpConnection>> clients_;

	bool is_multicast_ = false;
	uint16_t multicast_port_[MAX_MEDIA_CHANNEL];
	std::string multicast_ip_;
	std::atomic_bool has_new_client_;

	// ---- timed replay state (see TimedReplay.cpp) ----
	struct ReplayTrackState
	{
		bool configured = false;
		uint32_t ssrc = 0;
		uint16_t next_sequence = 0;
		uint32_t initial_rtp_timestamp = 0;
		uint32_t clock_rate = 0;
		int64_t  enqueue_frontier_us = 0; // largest media_us accepted on this track
		RtpHeader rtp_header;             // template: version/payload/ssrc prefilled
	};

	struct ReplayFrame
	{
		MediaChannelId channel = channel_0;
		int64_t  media_us = 0;
		uint64_t order = 0;               // global enqueue order, tie-breaker
		uint64_t bytes = 0;               // sum of RtpPacket.size of all fragments
		std::vector<RtpPacket> packets;   // production packetizer snapshot
		size_t   next_packet = 0;         // next fragment to offer
	};

	void AdvanceReplayClock(int64_t now_us);
	int64_t ReplayPositionUs() const
	{ return replay_position_half_us_ / 2; }

	bool replay_enabled_ = false;
	bool replay_paused_ = false;
	bool replay_in_sink_ = false;
	int64_t replay_origin_us_ = 0;        // M
	int64_t replay_clock_origin_us_ = 0;  // W
	int64_t replay_position_half_us_ = 0; // media position in half-microseconds
	int64_t replay_last_now_us_ = 0;
	int replay_rate_half_ = 2;            // half-us per wall us: 1 => 0.5x, 2 => 1x, 4 => 2x
	uint32_t replay_max_frames_ = 0;
	uint64_t replay_max_bytes_ = 0;
	uint64_t replay_frames_ = 0;          // unfinished queued frames
	uint64_t replay_bytes_ = 0;           // their summed RtpPacket.size
	int64_t replay_presented_us_ = 0;     // largest media_us ever offered to the sink
	uint64_t replay_order_ = 0;
	uint64_t replay_generation_ = 0;      // bumped by ClearReplay()
	std::function<bool (MediaChannelId, const RtpPacket&)> replay_sink_;
	std::vector<RtpPacket>* replay_capture_ = nullptr;
	ReplayTrackState replay_tracks_[MAX_MEDIA_CHANNEL];
	std::deque<ReplayFrame> replay_queue_; // sorted by (media_us, channel, order)

	static std::atomic_uint last_session_id_;
};

class MulticastAddr
{
public:
	static MulticastAddr& instance()
	{
		static MulticastAddr s_multi_addr;
		return s_multi_addr;
	}

	std::string GetAddr()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		std::string addr_str;
		struct sockaddr_in addr = { 0 };
		std::random_device rd;

		for (int n = 0; n <= 10; n++) {
			uint32_t range = 0xE8FFFFFF - 0xE8000100;
			addr.sin_addr.s_addr = htonl(0xE8000100 + (rd()) % range);
			addr_str = inet_ntoa(addr.sin_addr);

			if (addrs_.find(addr_str) != addrs_.end()) {
				addr_str.clear();
			}
			else {
				addrs_.insert(addr_str);
				break;
			}
		}

		return addr_str;
	}

	void Release(std::string addr) {
		std::lock_guard<std::mutex> lock(mutex_);
		addrs_.erase(addr);
	}

private:
	std::mutex mutex_;
	std::unordered_set<std::string> addrs_;
};

}

#endif
