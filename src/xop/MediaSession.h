// PHZ
// 2018-6-8

#ifndef XOP_MEDIA_SESSION_H
#define XOP_MEDIA_SESSION_H

#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <random>
#include <cstdint>
#include <functional>
#include <deque>
#include <unordered_set>
#include "media.h"
#include "rtp.h"
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

/*
 * Timed replay configuration.
 *
 * Timed replay is an optional, single-threaded, application driven mode of a
 * MediaSession.  Media frames are packetized by the installed MediaSource
 * (only H264Source and G711ASource are supported), queued in memory and then
 * handed to a sink one RTP packet at a time when the replay clock reaches the
 * frame's media time.  No socket is used in this mode.
 */

/* One replay track.  A track must match a source previously added to the
 * session on the same channel. */
struct ReplayTrackConfig
{
	MediaChannelId channel = channel_0;
	uint32_t ssrc = 0;					/* RTP SSRC used for this track. */
	uint16_t initial_sequence = 0;		/* First RTP sequence number. */
	uint32_t initial_rtp_timestamp = 0;	/* RTP timestamp at media_start_us. */
};

struct ReplayConfig
{
	int64_t  media_start_us = 0;		/* M: non-negative common media origin. */
	int64_t  wall_start_us = 0;			/* W: non-negative local clock origin. */
	uint32_t max_pending_frames = 0;	/* Positive limit of unfinished frames. */
	uint32_t max_pending_bytes = 0;		/* Positive limit; sums RtpPacket::size. */
	std::vector<ReplayTrackConfig> tracks;
	/* Returns true when the whole packet has been accepted by the sink. */
	std::function<bool (MediaChannelId, const RtpPacket&)> sink;
};

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

	bool AddClient(SOCKET rtspfd, std::shared_ptr<RtpConnection> rtp_conn);
	void RemoveClient(SOCKET rtspfd);

	MediaSessionId GetMediaSessionId()
	{ return session_id_; }

	/* Timed replay entry points.  All are no-ops returning false when timed
	 * replay has not been enabled (or, for EnableTimedReplay, when the session
	 * is not in a valid live state). */

	/* Enter timed replay mode.  Requires sources to be already added, no
	 * network clients, valid config and a replay mode not already active. */
	bool EnableTimedReplay(const ReplayConfig& config);

	/* Queue one complete frame on channel.  timestamp in the AVFrame is
	 * ignored; media_us (non-negative, per-track non-decreasing) determines
	 * the RTP timestamp.  On success the caller may release the frame. */
	bool EnqueueReplay(MediaChannelId channel_id, const AVFrame& frame, int64_t media_us);

	/* Deliver every due RTP packet to the sink.  Returns the number of
	 * packets accepted by the sink during this call. */
	uint32_t PumpReplay(int64_t now_us);

	void PauseReplay(int64_t now_us);
	void ResumeReplay(int64_t now_us);

	/* Change playback speed.  Only 1/2, 1 and 2 are accepted. */
	bool SetReplaySpeed(int64_t now_us, uint32_t numerator, uint32_t denominator);

	/* Advance the clock, drop queued frames and reset each track front. */
	void ClearReplay(int64_t now_us);

	uint32_t GetNumClient() const
	{ return (uint32_t)clients_.size(); }

	bool IsMulticast() const
	{ return is_multicast_; }

	std::string GetMulticastIp() const
	{ return multicast_ip_; }

	uint16_t GetMulticastPort(MediaChannelId channel_id) const
	{
		if ((int)channel_id >= MAX_MEDIA_CHANNEL) {
			return 0;
		}
		return multicast_port_[channel_id];
	}

private:
	friend class MediaSource;
	friend class RtspServer;
	MediaSession(std::string url_suffxx);

	struct ReplayPacket
	{
		RtpPacket packet;
		uint16_t  seq = 0;
		bool      seq_assigned = false;
	};

	struct ReplayFrame
	{
		int64_t  media_us = 0;
		uint32_t rtp_timestamp = 0;
		uint64_t order = 0;
		uint32_t byte_cost = 0;
		size_t   next_packet = 0;
		std::vector<ReplayPacket> packets;
	};

	struct ReplayTrack
	{
		bool     configured = false;
		uint32_t ssrc = 0;
		uint16_t next_seq = 0;
		uint32_t initial_rtp_timestamp = 0;
		uint32_t clock_rate = 0;
		uint32_t payload_type = 0;
		/* Lower bound (in half microseconds) for the next enqueue. */
		int64_t  front_half_us = 0;
		std::deque<ReplayFrame> frames;
	};

	int64_t ReplayPositionHalf(int64_t now_us) const;
	bool    ReplayFillHeader(ReplayPacket& pkt, const ReplayTrack& track,
	                         uint32_t rtp_timestamp) const;
	void    ReplayCompleteFront(MediaChannelId channel_id);
	ReplayFrame* ReplayFindCurrent();
	ReplayFrame* ReplaySelectDue(int64_t position_half_us);
	void    PauseReplayImpl(int64_t now_us);
	void    ResumeReplayImpl(int64_t now_us);
	bool    SetReplaySpeedImpl(int64_t now_us, uint32_t numerator, uint32_t denominator);
	void    ClearReplayImpl(int64_t now_us);

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

	/* Timed replay state.  The mode is single-threaded and owns no sockets. */
	bool replay_enabled_ = false;
	std::function<bool (MediaChannelId, const RtpPacket&)> replay_sink_;
	ReplayTrack replay_tracks_[MAX_MEDIA_CHANNEL];
	uint32_t replay_max_frames_ = 0;
	uint64_t replay_max_bytes_ = 0;
	int64_t  replay_media_start_us_ = 0;	/* M */
	int64_t  replay_wall_start_us_ = 0;		/* W */
	int64_t  replay_anchor_half_us_ = 0;	/* clock anchor, half microsecond */
	int64_t  replay_anchor_wall_us_ = 0;
	uint32_t replay_speed_num_ = 1;
	uint32_t replay_speed_den_ = 1;
	bool     replay_playing_ = true;
	uint32_t replay_pending_frames_ = 0;
	uint64_t replay_pending_bytes_ = 0;
	uint64_t replay_order_ = 0;
	uint32_t replay_generation_ = 0;
	bool     replay_in_callback_ = false;
	bool     replay_has_current_ = false;
	MediaChannelId replay_current_channel_ = channel_0;
	uint64_t replay_current_order_ = 0;
	int64_t  replay_presented_us_ = 0;	/* max media_us presented to sink */

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
