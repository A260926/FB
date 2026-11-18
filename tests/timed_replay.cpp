// Timed replay regression tests for MediaSession.
//
// Groups: clock, delivery, lifecycle.

#include "task_support/check.h"

#include "xop/MediaSession.h"
#include "xop/H264Source.h"
#include "xop/G711ASource.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <memory>
#include <vector>
#include <functional>
#include <algorithm>

using namespace xop;

namespace
{

struct PacketView
{
	bool     zero_prefix = false;
	uint8_t  version = 0;
	uint8_t  pt = 0;
	uint8_t  marker = 0;
	uint16_t seq = 0;
	uint32_t ts = 0;
	uint32_t ssrc = 0;
	std::vector<uint8_t> payload;
};

PacketView Decode(const RtpPacket& pkt)
{
	PacketView view;
	if (pkt.data == nullptr || pkt.size < RTP_TCP_HEAD_SIZE + RTP_HEADER_SIZE) {
		return view;
	}

	const uint8_t* d = pkt.data.get();
	view.zero_prefix = (d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 0);
	view.version = (uint8_t)(d[4] >> 6);
	view.marker  = (uint8_t)((d[5] >> 7) & 0x01);
	view.pt      = (uint8_t)(d[5] & 0x7f);
	view.seq     = (uint16_t)(((uint16_t)d[6] << 8) | d[7]);
	view.ts      = ((uint32_t)d[8] << 24) | ((uint32_t)d[9] << 16) |
	               ((uint32_t)d[10] << 8) | d[11];
	view.ssrc    = ((uint32_t)d[12] << 24) | ((uint32_t)d[13] << 16) |
	               ((uint32_t)d[14] << 8) | d[15];
	view.payload.assign(d + RTP_TCP_HEAD_SIZE + RTP_HEADER_SIZE, d + pkt.size);
	return view;
}

AVFrame MakeH264(uint32_t size, uint8_t nal_header, uint32_t seed)
{
	AVFrame frame(size);
	frame.type = VIDEO_FRAME_I;
	frame.timestamp = 0;
	frame.buffer.get()[0] = nal_header;
	for (uint32_t i = 1; i < size; i++) {
		frame.buffer.get()[i] = (uint8_t)(i * 17u + seed + 3u);
	}
	return frame;
}

AVFrame MakePcma(uint32_t size, uint32_t seed)
{
	AVFrame frame(size);
	frame.type = AUDIO_FRAME;
	frame.timestamp = 0;
	for (uint32_t i = 0; i < size; i++) {
		frame.buffer.get()[i] = (uint8_t)(0x55u ^ (i + seed));
	}
	return frame;
}

ReplayTrackConfig Track(MediaChannelId channel, uint32_t ssrc,
                        uint16_t seq, uint32_t ts)
{
	ReplayTrackConfig track;
	track.channel = channel;
	track.ssrc = ssrc;
	track.initial_sequence = seq;
	track.initial_rtp_timestamp = ts;
	return track;
}

ReplayConfig Config(int64_t media_start, int64_t wall_start,
                    uint32_t max_frames, uint32_t max_bytes)
{
	ReplayConfig config;
	config.media_start_us = media_start;
	config.wall_start_us = wall_start;
	config.max_pending_frames = max_frames;
	config.max_pending_bytes = max_bytes;
	return config;
}

std::unique_ptr<MediaSession> SessionWithSources(bool video, bool audio)
{
	std::unique_ptr<MediaSession> session(MediaSession::CreateNew("replay"));
	if (video) {
		session->AddSource(channel_0, H264Source::CreateNew());
	}
	if (audio) {
		session->AddSource(channel_1, G711ASource::CreateNew());
	}
	return session;
}

/* Reconstruct one H264 frame from FU-A fragments. */
std::vector<uint8_t> RebuildH264(const std::vector<PacketView>& packets,
                                 size_t begin, size_t end)
{
	std::vector<uint8_t> out;
	if (begin >= end) {
		return out;
	}

	const std::vector<uint8_t>& first = packets[begin].payload;
	if (first.size() < 2) {
		return out;
	}
	out.push_back((uint8_t)((first[0] & 0xe0) | (first[1] & 0x1f)));

	for (size_t i = begin; i < end; i++) {
		const std::vector<uint8_t>& payload = packets[i].payload;
		if (payload.size() < 2) {
			return std::vector<uint8_t>();
		}
		out.insert(out.end(), payload.begin() + 2, payload.end());
	}
	return out;
}

} // namespace

/* ------------------------------------------------------------------ */
/* clock                                                              */
/* ------------------------------------------------------------------ */

static int clock_initial_boundary(void)
{
	std::unique_ptr<MediaSession> session = SessionWithSources(true, false);

	std::vector<PacketView> packets;
	ReplayConfig config = Config(1000000, 5000000, 8, 65536);
	config.tracks.push_back(Track(channel_0, 0x11223344u, 100, 900000));
	config.sink = [&](MediaChannelId, const RtpPacket& pkt) {
		packets.push_back(Decode(pkt));
		return true;
	};
	TASK_CHECK(session->EnableTimedReplay(config));

	AVFrame early = MakeH264(100, 0x65, 1);
	TASK_CHECK(session->EnqueueReplay(channel_0, early, 1000000));
	AVFrame later = MakeH264(120, 0x65, 2);
	TASK_CHECK(session->EnqueueReplay(channel_0, later, 1001000));

	/* Position starts exactly at M: only the frame at M is due. */
	TASK_CHECK(session->PumpReplay(5000000) == 1);
	TASK_CHECK(session->PumpReplay(5000999) == 0);
	TASK_CHECK(session->PumpReplay(5001000) == 1);
	TASK_CHECK(packets.size() == 2);
	TASK_CHECK(packets[0].version == RTP_VERSION);
	TASK_CHECK(packets[0].pt == 96);
	TASK_CHECK(packets[0].seq == 100);
	TASK_CHECK(packets[0].ts == 900000);
	TASK_CHECK(packets[0].marker == 1);
	TASK_CHECK(packets[0].ssrc == 0x11223344u);
	TASK_CHECK(packets[0].zero_prefix);
	TASK_CHECK(packets[1].seq == 101);
	TASK_CHECK(packets[1].ts == 900090);

	return 0;
}

static int clock_half_speed_odd(void)
{
	std::unique_ptr<MediaSession> session = SessionWithSources(true, false);

	int delivered = 0;
	ReplayConfig config = Config(0, 0, 4, 65536);
	config.tracks.push_back(Track(channel_0, 7, 0, 0));
	config.sink = [&](MediaChannelId, const RtpPacket&) {
		delivered++;
		return true;
	};
	TASK_CHECK(session->EnableTimedReplay(config));

	TASK_CHECK(session->SetReplaySpeed(0, 1, 2));

	/* Odd microsecond media time: due at wall 6us (position 3.0us). */
	AVFrame frame = MakeH264(64, 0x65, 3);
	TASK_CHECK(session->EnqueueReplay(channel_0, frame, 3));

	TASK_CHECK(session->PumpReplay(5) == 0);	/* position 2.5us */
	TASK_CHECK(delivered == 0);
	TASK_CHECK(session->PumpReplay(6) == 1);	/* position 3.0us */
	TASK_CHECK(delivered == 1);

	return 0;
}

static int clock_speed_change_midway(void)
{
	std::unique_ptr<MediaSession> session = SessionWithSources(true, false);

	std::vector<uint32_t> stamps;
	ReplayConfig config = Config(0, 0, 4, 65536);
	config.tracks.push_back(Track(channel_0, 0xabc, 5, 1000));
	config.sink = [&](MediaChannelId, const RtpPacket& pkt) {
		stamps.push_back(Decode(pkt).ts);
		return true;
	};
	TASK_CHECK(session->EnableTimedReplay(config));

	TASK_CHECK(session->SetReplaySpeed(1000, 2, 1));	/* 1000us at 1x */
	TASK_CHECK(!session->SetReplaySpeed(1000, 3, 1));	/* invalid */
	TASK_CHECK(!session->SetReplaySpeed(1000, 0, 0));	/* invalid */
	TASK_CHECK(!session->SetReplaySpeed(1000, 1, 3));	/* invalid */

	AVFrame frame = MakeH264(64, 0x65, 4);
	TASK_CHECK(session->EnqueueReplay(channel_0, frame, 2000));

	/* 2x from 1000us: at 1499us position is 1998us, at 1500 it is 2000. */
	TASK_CHECK(session->PumpReplay(1499) == 0);
	TASK_CHECK(session->PumpReplay(1500) == 1);
	TASK_CHECK(stamps.size() == 1);

	return 0;
}

static int clock_pause_long_resume(void)
{
	std::unique_ptr<MediaSession> session = SessionWithSources(true, false);

	int delivered = 0;
	ReplayConfig config = Config(0, 100, 4, 65536);
	config.tracks.push_back(Track(channel_0, 1, 0, 0));
	config.sink = [&](MediaChannelId, const RtpPacket&) {
		delivered++;
		return true;
	};
	TASK_CHECK(session->EnableTimedReplay(config));

	AVFrame frame = MakeH264(64, 0x65, 5);
	TASK_CHECK(session->EnqueueReplay(channel_0, frame, 500));

	session->PauseReplay(200);			/* freeze at 100us */
	TASK_CHECK(session->PumpReplay(1000000000) == 0);
	TASK_CHECK(delivered == 0);

	/* Resume much later: the frozen position is kept, not fast-forwarded. */
	session->ResumeReplay(1000000000);
	TASK_CHECK(session->PumpReplay(1000000000) == 0);
	/* 400us at 1x from the frozen 100us. */
	TASK_CHECK(session->PumpReplay(1000000400) == 1);
	TASK_CHECK(delivered == 1);

	return 0;
}

/* ------------------------------------------------------------------ */
/* delivery                                                           */
/* ------------------------------------------------------------------ */

static int delivery_interleave_h264_pcma(void)
{
	std::unique_ptr<MediaSession> session = SessionWithSources(true, true);

	std::vector<PacketView> packets;
	ReplayConfig config = Config(0, 0, 8, 65536);
	config.tracks.push_back(Track(channel_0, 0x1111, 10, 0));
	config.tracks.push_back(Track(channel_1, 0x2222, 500, 0));
	config.sink = [&](MediaChannelId, const RtpPacket& pkt) {
		packets.push_back(Decode(pkt));
		return true;
	};
	TASK_CHECK(session->EnableTimedReplay(config));

	AVFrame video = MakeH264(3000, 0x65, 6);
	TASK_CHECK(session->EnqueueReplay(channel_0, video, 0));
	AVFrame audio = MakePcma(160, 7);
	TASK_CHECK(session->EnqueueReplay(channel_1, audio, 0));

	TASK_CHECK(session->PumpReplay(0) == 4);
	TASK_CHECK(packets.size() == 4);

	/* Same media time: channel_0 first, then channel_1. */
	TASK_CHECK(packets[0].ssrc == 0x1111);
	TASK_CHECK(packets[1].ssrc == 0x1111);
	TASK_CHECK(packets[2].ssrc == 0x1111);
	TASK_CHECK(packets[3].ssrc == 0x2222);

	/* H264 FU-A boundaries and independent per-track sequence numbers. */
	TASK_CHECK(packets[0].payload.size() == MAX_RTP_PAYLOAD_SIZE);
	TASK_CHECK(packets[1].payload.size() == MAX_RTP_PAYLOAD_SIZE);
	TASK_CHECK(packets[2].payload.size() == 165);
	TASK_CHECK((packets[0].payload[0] & 0x1f) == 28);
	TASK_CHECK((packets[0].payload[1] & 0x80) != 0);
	TASK_CHECK((packets[0].payload[1] & 0x40) == 0);
	TASK_CHECK((packets[2].payload[1] & 0x40) != 0);
	TASK_CHECK(packets[0].marker == 0 && packets[1].marker == 0);
	TASK_CHECK(packets[2].marker == 1);
	TASK_CHECK(packets[0].seq == 10 && packets[1].seq == 11 && packets[2].seq == 12);
	TASK_CHECK(packets[3].seq == 500);
	TASK_CHECK(packets[3].marker == 1);
	TASK_CHECK(packets[3].pt == 8);
	TASK_CHECK(packets[3].payload.size() == 160);

	std::vector<uint8_t> rebuilt = RebuildH264(packets, 0, 3);
	TASK_CHECK(rebuilt.size() == video.size);
	TASK_CHECK(std::memcmp(rebuilt.data(), video.buffer.get(), video.size) == 0);
	TASK_CHECK(std::memcmp(packets[3].payload.data(), audio.buffer.get(), 160) == 0);

	return 0;
}

static int delivery_reject_retry(void)
{
	std::unique_ptr<MediaSession> session = SessionWithSources(true, false);

	std::vector<PacketView> attempts;
	int call = 0;
	bool rejected_once = false;
	ReplayConfig config = Config(0, 0, 4, 65536);
	config.tracks.push_back(Track(channel_0, 0x3333, 20, 7000));
	config.sink = [&](MediaChannelId, const RtpPacket& pkt) {
		attempts.push_back(Decode(pkt));
		call++;
		if (call == 2 && !rejected_once) {
			rejected_once = true;
			return false;	/* reject the second fragment once */
		}
		return true;
	};
	TASK_CHECK(session->EnableTimedReplay(config));

	AVFrame frame = MakeH264(3000, 0x65, 8);
	TASK_CHECK(session->EnqueueReplay(channel_0, frame, 0));

	TASK_CHECK(session->PumpReplay(0) == 1);
	TASK_CHECK(attempts.size() == 2);
	TASK_CHECK(attempts[0].seq == 20);
	TASK_CHECK(attempts[1].seq == 21);

	/* Retry resends the refused fragment with identical header and payload;
	 * fragments that already succeeded are not sent again. */
	TASK_CHECK(session->PumpReplay(0) == 2);
	TASK_CHECK(attempts.size() == 4);
	TASK_CHECK(attempts[2].seq == 21);
	TASK_CHECK(attempts[2].ts == attempts[1].ts);
	TASK_CHECK(attempts[2].marker == attempts[1].marker);
	TASK_CHECK(attempts[2].payload == attempts[1].payload);
	TASK_CHECK(attempts[3].seq == 22);
	TASK_CHECK(attempts[3].marker == 1);

	std::vector<uint8_t> rebuilt = RebuildH264(attempts, 0, 1);
	rebuilt.insert(rebuilt.end(), attempts[2].payload.begin() + 2, attempts[2].payload.end());
	rebuilt.insert(rebuilt.end(), attempts[3].payload.begin() + 2, attempts[3].payload.end());
	TASK_CHECK(rebuilt.size() == frame.size);
	TASK_CHECK(std::memcmp(rebuilt.data(), frame.buffer.get(), frame.size) == 0);

	return 0;
}

static int delivery_capacity_release(void)
{
	std::unique_ptr<MediaSession> session = SessionWithSources(true, true);

	int call = 0;
	ReplayConfig config = Config(0, 0, 1, 3053);	/* exactly one H264 frame */
	config.tracks.push_back(Track(channel_0, 1, 0, 0));
	config.tracks.push_back(Track(channel_1, 2, 0, 0));
	config.sink = [&](MediaChannelId, const RtpPacket&) {
		call++;
		return call != 2;	/* reject the middle fragment once */
	};
	TASK_CHECK(session->EnableTimedReplay(config));

	AVFrame first = MakeH264(3000, 0x65, 9);
	TASK_CHECK(session->EnqueueReplay(channel_0, first, 0));

	/* Frame and byte budget are full: no second frame fits. */
	AVFrame second = MakeH264(3000, 0x65, 10);
	TASK_CHECK(!session->EnqueueReplay(channel_0, second, 0));
	TASK_CHECK(!session->EnqueueReplay(channel_1, MakePcma(160, 11), 0));

	/* Partial acceptance keeps the whole frame's budget reserved. */
	TASK_CHECK(session->PumpReplay(0) == 1);
	TASK_CHECK(!session->EnqueueReplay(channel_1, MakePcma(160, 12), 0));

	TASK_CHECK(session->PumpReplay(0) == 2);
	/* Only after the final fragment is accepted is the budget released. */
	TASK_CHECK(session->EnqueueReplay(channel_1, MakePcma(160, 13), 0));
	TASK_CHECK(session->PumpReplay(0) == 1);

	return 0;
}

static int delivery_timestamp_zero_and_wrap(void)
{
	std::unique_ptr<MediaSession> session = SessionWithSources(true, true);

	std::vector<PacketView> packets;
	ReplayConfig config = Config(0, 0, 8, 65536);
	/* Explicit media timestamp 0 with initial RTP timestamp 0 must stay 0
	 * instead of falling back to the live clock. */
	config.tracks.push_back(Track(channel_0, 9, 0, 0));
	config.tracks.push_back(Track(channel_1, 10, 0, 0xFFFFFFF0u));
	config.sink = [&](MediaChannelId, const RtpPacket& pkt) {
		packets.push_back(Decode(pkt));
		return true;
	};
	TASK_CHECK(session->EnableTimedReplay(config));

	TASK_CHECK(session->EnqueueReplay(channel_0, MakeH264(32, 0x65, 14), 0));

	/* PCMA clock 8000: delta = 125 * (2 * 2^32 + 12345) us makes the RTP
	 * timestamp wrap twice; expected offset is 12345. */
	const int64_t delta = 125LL * (2LL * 4294967296LL + 12345LL);
	TASK_CHECK(session->EnqueueReplay(channel_1, MakePcma(160, 15), delta));

	TASK_CHECK(session->PumpReplay(0) == 1);
	TASK_CHECK(packets.size() == 1);
	TASK_CHECK(packets[0].ts == 0);
	TASK_CHECK(packets[0].seq == 0);

	TASK_CHECK(session->PumpReplay(delta) == 1);
	TASK_CHECK(packets.size() == 2);
	TASK_CHECK(packets[1].ts == (uint32_t)(0xFFFFFFF0u + 12345u));

	return 0;
}

static int delivery_snapshots(void)
{
	std::unique_ptr<MediaSession> session = SessionWithSources(true, false);

	std::vector<RtpPacket> retained;
	std::vector<std::vector<uint8_t> > retained_payloads;
	ReplayConfig config = Config(0, 0, 8, 65536);
	config.tracks.push_back(Track(channel_0, 0x55, 1, 0));
	config.sink = [&](MediaChannelId, const RtpPacket& pkt) {
		retained.push_back(pkt);	/* keep the shared_ptr past the call */
		retained_payloads.push_back(Decode(pkt).payload);
		return true;
	};
	TASK_CHECK(session->EnableTimedReplay(config));

	AVFrame first = MakeH264(3000, 0x65, 16);
	std::vector<uint8_t> original(first.buffer.get(), first.buffer.get() + first.size);
	TASK_CHECK(session->EnqueueReplay(channel_0, first, 0));

	/* The replay owns a snapshot: mutating the caller's frame after enqueue
	 * must not affect what is delivered. */
	std::memset(first.buffer.get(), 0xee, first.size);

	TASK_CHECK(session->PumpReplay(0) == 3);
	TASK_CHECK(retained.size() == 3);

	/* A later frame must not overwrite packets the sink still holds. */
	AVFrame next = MakeH264(200, 0x41, 17);
	TASK_CHECK(session->EnqueueReplay(channel_0, next, 1));
	TASK_CHECK(session->PumpReplay(1) == 1);

	std::vector<PacketView> views;
	for (size_t i = 0; i < retained.size(); i++) {
		views.push_back(Decode(retained[i]));
	}
	std::vector<uint8_t> rebuilt = RebuildH264(views, 0, 3);
	TASK_CHECK(rebuilt.size() == original.size());
	TASK_CHECK(std::memcmp(rebuilt.data(), original.data(), original.size()) == 0);
	TASK_CHECK(views[0].seq == 1 && views[1].seq == 2 && views[2].seq == 3);

	return 0;
}

static int delivery_live_parity(void)
{
	/* Normal (not enabled) production packetization as a reference. */
	std::unique_ptr<H264Source> video(H264Source::CreateNew());
	std::unique_ptr<G711ASource> audio(G711ASource::CreateNew());

	std::vector<RtpPacket> live_video;
	std::vector<RtpPacket> live_audio;
	video->SetSendFrameCallback([&](MediaChannelId c, RtpPacket p) {
		if (c != channel_0) {
			return false;
		}
		live_video.push_back(p);
		return true;
	});
	audio->SetSendFrameCallback([&](MediaChannelId c, RtpPacket p) {
		if (c != channel_1) {
			return false;
		}
		live_audio.push_back(p);
		return true;
	});

	AVFrame video_frame = MakeH264(3000, 0x65, 18);
	video_frame.timestamp = 123456;
	AVFrame audio_frame = MakePcma(160, 19);
	audio_frame.timestamp = 8000;
	TASK_CHECK(video->HandleFrame(channel_0, video_frame));
	TASK_CHECK(audio->HandleFrame(channel_1, audio_frame));
	TASK_CHECK(live_video.size() == 3);
	TASK_CHECK(live_audio.size() == 1);

	std::unique_ptr<MediaSession> session = SessionWithSources(true, true);
	std::vector<PacketView> replay;
	ReplayConfig config = Config(0, 0, 8, 65536);
	config.tracks.push_back(Track(channel_0, 1, 0, 123456));
	config.tracks.push_back(Track(channel_1, 2, 0, 8000));
	config.sink = [&](MediaChannelId, const RtpPacket& pkt) {
		replay.push_back(Decode(pkt));
		return true;
	};
	TASK_CHECK(session->EnableTimedReplay(config));
	TASK_CHECK(session->EnqueueReplay(channel_0, video_frame, 0));
	TASK_CHECK(session->EnqueueReplay(channel_1, audio_frame, 0));
	TASK_CHECK(session->PumpReplay(0) == 4);

	TASK_CHECK(replay.size() == 4);
	for (size_t i = 0; i < 3; i++) {
		TASK_CHECK(replay[i].payload.size() == live_video[i].size - 16);
		TASK_CHECK(std::memcmp(replay[i].payload.data(),
		                       live_video[i].data.get() + 16,
		                       replay[i].payload.size()) == 0);
		TASK_CHECK(replay[i].ts == live_video[i].timestamp);
	}
	TASK_CHECK(replay[3].payload.size() == live_audio[0].size - 16);
	TASK_CHECK(std::memcmp(replay[3].payload.data(),
	                       live_audio[0].data.get() + 16,
	                       replay[3].payload.size()) == 0);
	TASK_CHECK(replay[3].ts == live_audio[0].timestamp);

	return 0;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                          */
/* ------------------------------------------------------------------ */

static int lifecycle_enable_validation(void)
{
	/* No source: enable fails. */
	{
		std::unique_ptr<MediaSession> session = SessionWithSources(false, false);
		ReplayConfig config = Config(0, 0, 1, 1000);
		config.tracks.push_back(Track(channel_0, 1, 0, 0));
		config.sink = [](MediaChannelId, const RtpPacket&) { return true; };
		TASK_CHECK(!session->EnableTimedReplay(config));
	}

	/* Source present but no track / track for a missing source / duplicate. */
	{
		std::unique_ptr<MediaSession> session = SessionWithSources(true, false);
		ReplayConfig config = Config(0, 0, 1, 1000);
		config.sink = [](MediaChannelId, const RtpPacket&) { return true; };
		TASK_CHECK(!session->EnableTimedReplay(config));

		config.tracks.push_back(Track(channel_1, 1, 0, 0));
		TASK_CHECK(!session->EnableTimedReplay(config));

		config.tracks.clear();
		config.tracks.push_back(Track(channel_0, 1, 0, 0));
		config.tracks.push_back(Track(channel_0, 2, 0, 0));
		TASK_CHECK(!session->EnableTimedReplay(config));
	}

	/* Bad limits and negative origins. */
	{
		std::unique_ptr<MediaSession> session = SessionWithSources(true, false);
		ReplayConfig base = Config(0, 0, 1, 1000);
		base.tracks.push_back(Track(channel_0, 1, 0, 0));
		base.sink = [](MediaChannelId, const RtpPacket&) { return true; };

		ReplayConfig bad = base;
		bad.media_start_us = -1;
		TASK_CHECK(!session->EnableTimedReplay(bad));

		bad = base;
		bad.wall_start_us = -1;
		TASK_CHECK(!session->EnableTimedReplay(bad));

		bad = base;
		bad.max_pending_frames = 0;
		TASK_CHECK(!session->EnableTimedReplay(bad));

		bad = base;
		bad.max_pending_bytes = 0;
		TASK_CHECK(!session->EnableTimedReplay(bad));

		bad = base;
		bad.sink = std::function<bool (MediaChannelId, const RtpPacket&)>();
		TASK_CHECK(!session->EnableTimedReplay(bad));

		TASK_CHECK(session->EnableTimedReplay(base));
		TASK_CHECK(!session->EnableTimedReplay(base));	/* second enable */
	}
	return 0;
}

static int lifecycle_mode_exclusive(void)
{
	std::unique_ptr<MediaSession> session = SessionWithSources(true, false);

	/* Before enabling, replay entry points are rejected while live
	 * HandleFrame keeps working. */
	AVFrame frame = MakeH264(64, 0x65, 20);
	TASK_CHECK(!session->EnqueueReplay(channel_0, frame, 0));
	TASK_CHECK(session->PumpReplay(0) == 0);
	session->PauseReplay(0);
	session->ResumeReplay(0);
	TASK_CHECK(!session->SetReplaySpeed(0, 2, 1));
	session->ClearReplay(0);
	TASK_CHECK(session->HandleFrame(channel_0, frame));

	int delivered = 0;
	ReplayConfig config = Config(0, 0, 4, 65536);
	config.tracks.push_back(Track(channel_0, 1, 0, 0));
	config.sink = [&](MediaChannelId, const RtpPacket&) {
		delivered++;
		return true;
	};
	TASK_CHECK(session->EnableTimedReplay(config));

	/* After enabling, live ingress and source mutation are refused. */
	AVFrame live = MakeH264(64, 0x65, 21);
	TASK_CHECK(!session->HandleFrame(channel_0, live));
	MediaSource* source = session->GetMediaSource(channel_0);
	TASK_CHECK(source != nullptr);
	TASK_CHECK(!session->AddSource(channel_0, source));
	TASK_CHECK(!session->RemoveSource(channel_0));
	TASK_CHECK(!session->EnableTimedReplay(config));
	TASK_CHECK(delivered == 0);

	/* Enqueue validation: negative / before M, bad channel, unsupported size. */
	TASK_CHECK(!session->EnqueueReplay(channel_0, MakeH264(64, 0x65, 22), -1));
	/* Wrong channel: no configured track at channel_1. */
	TASK_CHECK(!session->EnqueueReplay(channel_1, MakeH264(64, 0x65, 23), 0));
	TASK_CHECK(!session->EnqueueReplay(channel_0, AVFrame(0), 0));
	TASK_CHECK(!session->EnqueueReplay(channel_0, MakeH264(256u * 1024u + 1u, 0x65, 24), 0));

	return 0;
}

static int lifecycle_pause_resume_idempotent(void)
{
	std::unique_ptr<MediaSession> session = SessionWithSources(true, false);

	int delivered = 0;
	ReplayConfig config = Config(0, 0, 4, 65536);
	config.tracks.push_back(Track(channel_0, 1, 0, 0));
	config.sink = [&](MediaChannelId, const RtpPacket&) {
		delivered++;
		return true;
	};
	TASK_CHECK(session->EnableTimedReplay(config));

	TASK_CHECK(session->EnqueueReplay(channel_0, MakeH264(64, 0x65, 25), 0));

	session->PauseReplay(0);
	session->PauseReplay(100);
	TASK_CHECK(session->PumpReplay(1000) == 0);
	TASK_CHECK(delivered == 0);

	session->ResumeReplay(1000);
	session->ResumeReplay(2000);
	/* Repeated resume does not send by itself. */
	TASK_CHECK(delivered == 0);
	TASK_CHECK(session->PumpReplay(2000) == 1);
	TASK_CHECK(delivered == 1);

	return 0;
}

static int lifecycle_clear(void)
{
	std::unique_ptr<MediaSession> session = SessionWithSources(true, false);

	std::vector<uint16_t> seqs;
	ReplayConfig config = Config(0, 0, 8, 65536);
	config.tracks.push_back(Track(channel_0, 1, 40, 100));
	config.sink = [&](MediaChannelId, const RtpPacket& pkt) {
		PacketView view = Decode(pkt);
		seqs.push_back(view.seq);
		return true;
	};
	TASK_CHECK(session->EnableTimedReplay(config));

	TASK_CHECK(session->EnqueueReplay(channel_0, MakeH264(64, 0x65, 26), 0));
	TASK_CHECK(session->PumpReplay(0) == 1);
	TASK_CHECK(seqs.size() == 1 && seqs[0] == 40);

	TASK_CHECK(session->EnqueueReplay(channel_0, MakeH264(64, 0x65, 27), 1000));
	session->ClearReplay(5000);		/* clock advances to 5000us */

	/* The queued frame is dropped; the front moves to the current position. */
	TASK_CHECK(session->PumpReplay(5000) == 0);
	TASK_CHECK(!session->EnqueueReplay(channel_0, MakeH264(64, 0x65, 28), 4000));
	TASK_CHECK(session->EnqueueReplay(channel_0, MakeH264(64, 0x65, 29), 5000));
	/* Sequence number and clock survive the clear. */
	TASK_CHECK(session->PumpReplay(5000) == 1);
	TASK_CHECK(seqs.size() == 2 && seqs[1] == 41);

	return 0;
}

static int lifecycle_callback_pause(void)
{
	std::unique_ptr<MediaSession> session = SessionWithSources(true, false);

	int delivered = 0;
	bool paused = false;
	ReplayConfig config = Config(0, 0, 4, 65536);
	config.tracks.push_back(Track(channel_0, 1, 0, 0));
	config.sink = [&](MediaChannelId, const RtpPacket&) {
		delivered++;
		if (!paused) {
			paused = true;
			session->PauseReplay(0);
		}
		return true;
	};
	TASK_CHECK(session->EnableTimedReplay(config));

	TASK_CHECK(session->EnqueueReplay(channel_0, MakeH264(3000, 0x65, 30), 0));

	/* The packet that returned true is committed, then pumping stops. */
	TASK_CHECK(session->PumpReplay(0) == 1);
	TASK_CHECK(delivered == 1);
	TASK_CHECK(session->PumpReplay(0) == 0);

	session->ResumeReplay(0);
	TASK_CHECK(session->PumpReplay(0) == 2);
	TASK_CHECK(delivered == 3);

	return 0;
}

static int lifecycle_callback_clear(void)
{
	std::unique_ptr<MediaSession> session = SessionWithSources(true, false);

	int delivered = 0;
	uint16_t first_seq = 0;
	bool cleared = false;
	ReplayConfig config = Config(0, 0, 8, 65536);
	config.tracks.push_back(Track(channel_0, 1, 70, 500));
	config.sink = [&](MediaChannelId, const RtpPacket& pkt) {
		PacketView view = Decode(pkt);
		if (!cleared) {
			first_seq = view.seq;
			cleared = true;
			session->ClearReplay(0);
		}
		delivered++;
		return true;
	};
	TASK_CHECK(session->EnableTimedReplay(config));

	/* Three packets are queued and due; the callback clears after the first. */
	TASK_CHECK(session->EnqueueReplay(channel_0, MakeH264(3000, 0x65, 31), 0));
	TASK_CHECK(session->PumpReplay(0) == 1);
	TASK_CHECK(delivered == 1);
	TASK_CHECK(first_seq == 70);

	/* Queue is empty and the front is reset to the current position. */
	TASK_CHECK(session->PumpReplay(0) == 0);
	TASK_CHECK(!session->EnqueueReplay(channel_0, MakeH264(64, 0x65, 32), -1));
	TASK_CHECK(session->EnqueueReplay(channel_0, MakeH264(64, 0x65, 33), 0));

	/* The committed packet kept its sequence number: the next one is 71. */
	std::vector<uint16_t> seqs;
	config.sink = [&](MediaChannelId, const RtpPacket& pkt) {
		seqs.push_back(Decode(pkt).seq);
		return true;
	};
	config.tracks.clear();
	config.tracks.push_back(Track(channel_0, 1, 71, 500));
	std::unique_ptr<MediaSession> second = SessionWithSources(true, false);
	TASK_CHECK(second->EnableTimedReplay(config));
	TASK_CHECK(second->EnqueueReplay(channel_0, MakeH264(64, 0x65, 34), 0));
	TASK_CHECK(second->PumpReplay(0) == 1);
	TASK_CHECK(seqs.size() == 1 && seqs[0] == 71);

	return 0;
}

const struct task_case task_cases[] =
{
	{ "clock",     "initial_boundary",      clock_initial_boundary },
	{ "clock",     "half_speed_odd",        clock_half_speed_odd },
	{ "clock",     "speed_change_midway",   clock_speed_change_midway },
	{ "clock",     "pause_long_resume",     clock_pause_long_resume },

	{ "delivery",  "interleave_h264_pcma",  delivery_interleave_h264_pcma },
	{ "delivery",  "reject_retry",          delivery_reject_retry },
	{ "delivery",  "capacity_release",      delivery_capacity_release },
	{ "delivery",  "timestamp_zero_wrap",   delivery_timestamp_zero_and_wrap },
	{ "delivery",  "snapshots",             delivery_snapshots },
	{ "delivery",  "live_parity",           delivery_live_parity },

	{ "lifecycle", "enable_validation",     lifecycle_enable_validation },
	{ "lifecycle", "mode_exclusive",        lifecycle_mode_exclusive },
	{ "lifecycle", "pause_resume_idempotent", lifecycle_pause_resume_idempotent },
	{ "lifecycle", "clear",                 lifecycle_clear },
	{ "lifecycle", "callback_pause",        lifecycle_callback_pause },
	{ "lifecycle", "callback_clear",        lifecycle_callback_clear },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
