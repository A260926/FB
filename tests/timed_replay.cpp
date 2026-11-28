// Timed replay regression tests for MediaSession (src/xop/TimedReplay.cpp).
//
// Groups: clock (play clock, speeds, timestamps), delivery (packet
// production and ordering), lifecycle (mode gating, validation,
// capacity, in-callback Pause/Clear).

#include "task_support/check.h"

#include "xop/MediaSession.h"
#include "xop/H264Source.h"
#include "xop/G711ASource.h"
#include "xop/H265Source.h"
#include "xop/rtp.h"

#include <cstring>
#include <vector>
#include <memory>
#include <functional>

using namespace xop;

namespace {

// One packet offer made to the replay sink.
struct Offer
{
	MediaChannelId channel = channel_0;
	uint8_t  b0 = 0;             // RTP header byte 0 (version...)
	uint8_t  b1 = 0;             // RTP header byte 1 (marker|PT)
	uint16_t seq = 0;
	uint32_t ts = 0;
	uint32_t ssrc = 0;
	bool prefix_zero = false;    // data[0..3] all zero
	std::vector<uint8_t> bytes;  // full packet copy: data[0..size)
	std::shared_ptr<uint8_t> held; // retained packet buffer (optional)
};

struct Sink
{
	std::vector<Offer> offers;
	// Optional per-offer hook: (offer_index, channel, packet) -> accept.
	std::function<bool (size_t, MediaChannelId, const RtpPacket&)> policy;
	bool hold = false; // retain shared_ptr of accepted packets

	bool operator()(MediaChannelId channel, const RtpPacket& pkt)
	{
		Offer offer;
		offer.channel = channel;
		const uint8_t* d = pkt.data.get();
		offer.prefix_zero = (d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 0);
		offer.b0 = d[4];
		offer.b1 = d[5];
		offer.seq = (uint16_t)((d[6] << 8) | d[7]);
		offer.ts  = ((uint32_t)d[8] << 24) | ((uint32_t)d[9] << 16) |
					((uint32_t)d[10] << 8) | (uint32_t)d[11];
		offer.ssrc = ((uint32_t)d[12] << 24) | ((uint32_t)d[13] << 16) |
					((uint32_t)d[14] << 8) | (uint32_t)d[15];
		offer.bytes.assign(d, d + pkt.size);

		bool accept = true;
		if (policy) {
			accept = policy(offers.size(), channel, pkt);
		}
		if (accept && hold) {
			offer.held = pkt.data;
		}
		offers.push_back(std::move(offer));
		return accept;
	}
};

AVFrame MakeVideo(uint32_t size, uint8_t nal_header, uint32_t timestamp, uint8_t seed)
{
	AVFrame frame(size);
	frame.type = VIDEO_FRAME_I;
	frame.timestamp = timestamp;
	if (size > 0) {
		frame.buffer.get()[0] = nal_header;
		for (uint32_t i = 1; i < size; i++) {
			frame.buffer.get()[i] = (uint8_t)(seed + i * 29u + (i >> 4));
		}
	}
	return frame;
}

AVFrame MakeAudio(uint32_t size, uint32_t timestamp, uint8_t seed)
{
	AVFrame frame(size);
	frame.type = AUDIO_FRAME;
	frame.timestamp = timestamp;
	for (uint32_t i = 0; i < size; i++) {
		frame.buffer.get()[i] = (uint8_t)(seed ^ (i * 13u));
	}
	return frame;
}

std::vector<uint8_t> FrameBytes(const AVFrame& frame)
{
	return std::vector<uint8_t>(frame.buffer.get(), frame.buffer.get() + frame.size);
}

std::vector<uint8_t> Payload(const Offer& offer)
{
	return std::vector<uint8_t>(offer.bytes.begin() + RTP_TCP_HEAD_SIZE + RTP_HEADER_SIZE,
								offer.bytes.end());
}

// Rebuilds the original NAL from a run of FU-A offers.
std::vector<uint8_t> ReconstructFu(const std::vector<Offer>& offers, size_t first,
									size_t count, uint8_t nal_header)
{
	std::vector<uint8_t> out(1, nal_header);
	for (size_t i = first; i < first + count; i++) {
		const Offer& offer = offers[i];
		out.insert(out.end(),
				offer.bytes.begin() + RTP_TCP_HEAD_SIZE + RTP_HEADER_SIZE + 2,
				offer.bytes.end());
	}
	return out;
}

MediaSession::TimedReplayTrackConfig Track(MediaChannelId channel, uint32_t ssrc,
											uint16_t sequence, uint32_t rtp_timestamp)
{
	MediaSession::TimedReplayTrackConfig track;
	track.channel = channel;
	track.ssrc = ssrc;
	track.initial_sequence = sequence;
	track.initial_rtp_timestamp = rtp_timestamp;
	return track;
}

struct ReplaySession
{
	std::unique_ptr<MediaSession> session;
	Sink sink;

	ReplaySession()
		: session(MediaSession::CreateNew("replay"))
	{
	}

	MediaSession::TimedReplayConfig Config(int64_t M, int64_t W, uint32_t max_frames,
											uint64_t max_bytes)
	{
		MediaSession::TimedReplayConfig config;
		config.media_origin_us = M;
		config.clock_origin_us = W;
		config.max_frames = max_frames;
		config.max_bytes = max_bytes;
		config.sink = [this](MediaChannelId channel, const RtpPacket& pkt) {
			return sink(channel, pkt);
		};
		return config;
	}
};

// ---------------------------------------------------------------- clock

int clock_half_speed_odd_us_and_mid_change()
{
	ReplaySession rs;
	TASK_CHECK(rs.session->AddSource(channel_1, G711ASource::CreateNew()));
	auto config = rs.Config(1000, 500, 8, 1u << 20);
	config.tracks.push_back(Track(channel_1, 0xA55A0001u, 400, 8000));
	TASK_CHECK(rs.session->EnableTimedReplay(config));
	TASK_CHECK(rs.session->IsTimedReplayEnabled());

	// Invalid speeds are rejected without touching any state.
	TASK_CHECK(!rs.session->SetReplaySpeed(0.0, 500));
	TASK_CHECK(!rs.session->SetReplaySpeed(-1.0, 500));
	TASK_CHECK(!rs.session->SetReplaySpeed(0.75, 500));
	TASK_CHECK(!rs.session->SetReplaySpeed(3.0, 500));
	TASK_CHECK(rs.session->SetReplaySpeed(0.5, 500));

	for (int i = 0; i < 4; i++) {
		TASK_CHECK(rs.session->EnqueueReplay(channel_1, MakeAudio(160, 0, (uint8_t)(0x10 + i)),
											1000 + i * 1000));
	}

	// Half speed: position = 1000 + (now-500)/2; odd microseconds keep a
	// half-microsecond remainder instead of crossing the next integer.
	TASK_CHECK(rs.session->PumpReplay(1500) == 1); // pos 1500   -> frame@1000
	TASK_CHECK(rs.session->PumpReplay(1501) == 0); // pos 1500.5 -> nothing new
	TASK_CHECK(rs.session->PumpReplay(2500) == 1); // pos 2000   -> frame@2000
	TASK_CHECK(rs.session->PumpReplay(2501) == 0); // pos 2000.5 -> nothing new

	// Mid-way speed change: advance at the old speed to now, then switch.
	TASK_CHECK(rs.session->SetReplaySpeed(2.0, 2501)); // pos stays 2000.5
	TASK_CHECK(rs.session->PumpReplay(3501) == 2);     // pos 4000.5 -> frames@3000,4000
	TASK_CHECK(rs.session->PumpReplay(3501) == 0);

	TASK_CHECK(rs.sink.offers.size() == 4);
	for (size_t i = 0; i < 4; i++) {
		const Offer& offer = rs.sink.offers[i];
		TASK_CHECK(offer.channel == channel_1);
		TASK_CHECK(offer.seq == (uint16_t)(400 + i));
		TASK_CHECK(offer.ts == 8000 + 8 * (uint32_t)i); // 8000 Hz clock, 1 ms spacing
		TASK_CHECK(offer.b0 == 0x80);                   // RTP version 2
		TASK_CHECK(offer.b1 == (0x80 | 8));             // marker + PT 8 (PCMA)
		TASK_CHECK(offer.ssrc == 0xA55A0001u);
		TASK_CHECK(offer.prefix_zero);
		TASK_CHECK(offer.bytes.size() == 4 + 12 + 160);
	}
	return 0;
}

int clock_pause_and_long_resume()
{
	ReplaySession rs;
	TASK_CHECK(rs.session->AddSource(channel_1, G711ASource::CreateNew()));
	auto config = rs.Config(0, 0, 8, 1u << 20);
	config.tracks.push_back(Track(channel_1, 7, 10, 0));
	TASK_CHECK(rs.session->EnableTimedReplay(config));

	TASK_CHECK(rs.session->EnqueueReplay(channel_1, MakeAudio(160, 0, 1), 0));
	TASK_CHECK(rs.session->EnqueueReplay(channel_1, MakeAudio(160, 0, 2), 1000000));
	TASK_CHECK(rs.session->EnqueueReplay(channel_1, MakeAudio(160, 0, 3), 10000000));

	TASK_CHECK(rs.session->PumpReplay(100) == 1); // pos 100 -> frame@0
	TASK_CHECK(rs.sink.offers.size() == 1);

	TASK_CHECK(rs.session->PauseReplay(100));
	TASK_CHECK(rs.session->PauseReplay(100));     // repeated pause: no extra effect
	TASK_CHECK(rs.session->PauseReplay(5000000)); // still no effect
	TASK_CHECK(rs.session->PumpReplay(5000000) == 0); // paused: nothing, returns 0
	TASK_CHECK(rs.session->SetReplaySpeed(2.0, 6000000)); // applies after resume only
	TASK_CHECK(rs.session->ResumeReplay(9000000));
	TASK_CHECK(rs.session->ResumeReplay(9000000)); // repeated resume: no extra effect

	// The position froze at 100 despite ~9 s of wall clock during the pause.
	TASK_CHECK(rs.session->PumpReplay(9000000) == 0);   // pos still 100
	TASK_CHECK(rs.session->PumpReplay(9000100) == 0);   // pos 100 + 100*2 = 300
	TASK_CHECK(rs.sink.offers.size() == 1);
	TASK_CHECK(rs.session->PumpReplay(9500000) == 1);   // pos 1000100 -> frame@1e6
	TASK_CHECK(rs.session->PumpReplay(14500000) == 1);  // pos 11000100 -> frame@1e7

	TASK_CHECK(rs.sink.offers.size() == 3);
	TASK_CHECK(rs.sink.offers[0].seq == 10 && rs.sink.offers[0].ts == 0);
	TASK_CHECK(rs.sink.offers[1].seq == 11 && rs.sink.offers[1].ts == 8000);
	TASK_CHECK(rs.sink.offers[2].seq == 12 && rs.sink.offers[2].ts == 80000);
	return 0;
}

int clock_explicit_zero_and_double_wrap()
{
	// Scenario 1: initial RTP timestamp 0; explicit media timestamp 0 must
	// not trigger the H264 wall-clock fallback; tick counter wraps twice.
	{
		ReplaySession rs;
		TASK_CHECK(rs.session->AddSource(channel_0, H264Source::CreateNew()));
		auto config = rs.Config(0, 0, 8, 1u << 20);
		config.tracks.push_back(Track(channel_0, 0xABC, 1, 0));
		TASK_CHECK(rs.session->EnableTimedReplay(config));

		const int64_t wrap1  = 47721858844LL; // ticks 0xFFFFFFFF
		const int64_t wrap1p = 47721858845LL; // ticks 2^32     -> 0
		const int64_t mid    = 55443717689LL; // ticks 2^32 + 694967296
		const int64_t wrap2  = 95443717689LL; // ticks 2^33     -> 0
		// AVFrame.timestamp inputs are junk on purpose: they must be ignored.
		TASK_CHECK(rs.session->EnqueueReplay(channel_0, MakeVideo(100, 0x65, 0, 1), 0));
		TASK_CHECK(rs.session->EnqueueReplay(channel_0, MakeVideo(100, 0x65, 777, 2), wrap1));
		TASK_CHECK(rs.session->EnqueueReplay(channel_0, MakeVideo(100, 0x65, 0, 3), wrap1p));
		TASK_CHECK(rs.session->EnqueueReplay(channel_0, MakeVideo(100, 0x65, 999999, 4), mid));
		TASK_CHECK(rs.session->EnqueueReplay(channel_0, MakeVideo(100, 0x65, 0, 5), wrap2));

		TASK_CHECK(rs.session->PumpReplay(wrap2) == 5);
		TASK_CHECK(rs.sink.offers.size() == 5);
		const uint32_t expected_ts[5] = { 0u, 0xFFFFFFFFu, 0u, 694967296u, 0u };
		for (size_t i = 0; i < 5; i++) {
			TASK_CHECK(rs.sink.offers[i].seq == (uint16_t)(1 + i));
			TASK_CHECK(rs.sink.offers[i].ts == expected_ts[i]);
			TASK_CHECK(rs.sink.offers[i].b1 == (0x80 | 96)); // marker + PT 96
		}
	}

	// Scenario 2: initial timestamp near the wrap; sequence crosses zero.
	{
		ReplaySession rs;
		TASK_CHECK(rs.session->AddSource(channel_0, H264Source::CreateNew()));
		auto config = rs.Config(0, 0, 8, 1u << 20);
		config.tracks.push_back(Track(channel_0, 5, 65534, 0xFFFFFFFEu));
		TASK_CHECK(rs.session->EnableTimedReplay(config));

		TASK_CHECK(rs.session->EnqueueReplay(channel_0, MakeVideo(50, 0x61, 0, 1), 0));
		TASK_CHECK(rs.session->EnqueueReplay(channel_0, MakeVideo(50, 0x61, 0, 2), 1000));
		TASK_CHECK(rs.session->EnqueueReplay(channel_0, MakeVideo(50, 0x61, 0, 3), 2000));
		TASK_CHECK(rs.session->PumpReplay(2000) == 3);
		TASK_CHECK(rs.sink.offers.size() == 3);
		TASK_CHECK(rs.sink.offers[0].seq == 65534 && rs.sink.offers[0].ts == 0xFFFFFFFEu);
		TASK_CHECK(rs.sink.offers[1].seq == 65535 && rs.sink.offers[1].ts == 88u);  // +90 wrapped
		TASK_CHECK(rs.sink.offers[2].seq == 0 && rs.sink.offers[2].ts == 178u);     // +180 wrapped
	}
	return 0;
}

// ---------------------------------------------------------------- delivery

int delivery_h264_multi_fu_interleaved_pcma()
{
	ReplaySession rs;
	TASK_CHECK(rs.session->AddSource(channel_0, H264Source::CreateNew()));
	TASK_CHECK(rs.session->AddSource(channel_1, G711ASource::CreateNew()));
	auto config = rs.Config(0, 0, 16, 1u << 20);
	config.tracks.push_back(Track(channel_0, 0x11111111u, 100, 1000));
	config.tracks.push_back(Track(channel_1, 0x22222222u, 50, 2000));
	TASK_CHECK(rs.session->EnableTimedReplay(config));

	AVFrame video0 = MakeVideo(3000, 0x65, 0, 1); // 3 FU-A fragments
	AVFrame video1 = MakeVideo(100, 0x61, 0, 2);  // single packet
	AVFrame audio0 = MakeAudio(160, 0, 3);
	AVFrame audio1 = MakeAudio(160, 0, 4);
	AVFrame audio2 = MakeAudio(160, 0, 5);
	auto video0_bytes = FrameBytes(video0);
	auto video1_bytes = FrameBytes(video1);
	auto audio0_bytes = FrameBytes(audio0);
	auto audio1_bytes = FrameBytes(audio1);
	auto audio2_bytes = FrameBytes(audio2);

	// Interleaved across tracks; per-track media_us stays non-decreasing.
	TASK_CHECK(rs.session->EnqueueReplay(channel_0, video0, 0));
	TASK_CHECK(rs.session->EnqueueReplay(channel_1, audio0, 0));
	TASK_CHECK(rs.session->EnqueueReplay(channel_0, video1, 1000));
	TASK_CHECK(rs.session->EnqueueReplay(channel_1, audio1, 500));
	TASK_CHECK(rs.session->EnqueueReplay(channel_1, audio2, 1000));

	// Global order: (0,ch0) x3 FU, (0,ch1), (500,ch1), (1000,ch0), (1000,ch1).
	TASK_CHECK(rs.session->PumpReplay(1000) == 7);
	TASK_CHECK(rs.sink.offers.size() == 7);
	const std::vector<Offer>& o = rs.sink.offers;

	// Video FU-A run: seq 100..102, shared timestamp, markers on the last.
	for (size_t i = 0; i < 3; i++) {
		TASK_CHECK(o[i].channel == channel_0);
		TASK_CHECK(o[i].seq == (uint16_t)(100 + i));
		TASK_CHECK(o[i].ts == 1000);
		TASK_CHECK(o[i].ssrc == 0x11111111u);
		TASK_CHECK(o[i].b0 == 0x80);
		TASK_CHECK(o[i].prefix_zero);
	}
	TASK_CHECK(o[0].b1 == 96 && o[1].b1 == 96 && o[2].b1 == (0x80 | 96));
	TASK_CHECK(o[0].bytes[16] == 0x7C && o[0].bytes[17] == 0x85); // FU-A start, type 5
	TASK_CHECK(o[1].bytes[16] == 0x7C && o[1].bytes[17] == 0x05); // middle
	TASK_CHECK(o[2].bytes[16] == 0x7C && o[2].bytes[17] == 0x45); // end
	TASK_CHECK(ReconstructFu(o, 0, 3, 0x65) == video0_bytes);

	TASK_CHECK(o[3].channel == channel_1 && o[3].seq == 50 && o[3].ts == 2000);
	TASK_CHECK(o[3].b1 == (0x80 | 8) && o[3].ssrc == 0x22222222u);
	TASK_CHECK(Payload(o[3]) == audio0_bytes);

	TASK_CHECK(o[4].channel == channel_1 && o[4].seq == 51 && o[4].ts == 2004); // +500us @8kHz
	TASK_CHECK(Payload(o[4]) == audio1_bytes);

	TASK_CHECK(o[5].channel == channel_0 && o[5].seq == 103 && o[5].ts == 1090); // +1000us @90kHz
	TASK_CHECK(o[5].b1 == (0x80 | 96));
	TASK_CHECK(Payload(o[5]) == video1_bytes); // single NAL, no FU header

	TASK_CHECK(o[6].channel == channel_1 && o[6].seq == 52 && o[6].ts == 2008);
	TASK_CHECK(Payload(o[6]) == audio2_bytes);
	return 0;
}

int delivery_second_fragment_reject_retry()
{
	ReplaySession rs;
	TASK_CHECK(rs.session->AddSource(channel_0, H264Source::CreateNew()));
	auto config = rs.Config(0, 0, 8, 1u << 20);
	config.tracks.push_back(Track(channel_0, 9, 7, 55));
	TASK_CHECK(rs.session->EnableTimedReplay(config));

	AVFrame video0 = MakeVideo(3000, 0x65, 0, 10); // 3 FU-A fragments
	AVFrame video1 = MakeVideo(80, 0x61, 0, 11);
	auto video0_bytes = FrameBytes(video0);
	auto video1_bytes = FrameBytes(video1);
	TASK_CHECK(rs.session->EnqueueReplay(channel_0, video0, 0));
	TASK_CHECK(rs.session->EnqueueReplay(channel_0, video1, 100));

	// Reject the second offer (the middle FU) exactly once.
	int rejected = 0;
	rs.sink.policy = [&rejected](size_t index, MediaChannelId, const RtpPacket&) -> bool {
		if (index == 1 && !rejected) {
			rejected = 1;
			return false;
		}
		return true;
	};

	TASK_CHECK(rs.session->PumpReplay(1000) == 1); // FU0 in, FU1 rejected, stop
	TASK_CHECK(rs.sink.offers.size() == 2);        // the later frame did not overtake
	Offer rejected_offer = rs.sink.offers[1];

	rs.sink.policy = nullptr;
	TASK_CHECK(rs.session->PumpReplay(1000) == 3); // FU1 retry, FU2, next frame
	TASK_CHECK(rs.sink.offers.size() == 5);
	const std::vector<Offer>& o = rs.sink.offers;

	// The retry is byte-identical: same channel/sequence/timestamp/marker/payload.
	TASK_CHECK(o[2].bytes == rejected_offer.bytes);
	TASK_CHECK(o[0].seq == 7 && o[1].seq == 8 && o[2].seq == 8);
	TASK_CHECK(o[3].seq == 9 && o[4].seq == 10);
	TASK_CHECK(o[0].ts == 55 && o[1].ts == 55 && o[2].ts == 55 && o[3].ts == 55);
	TASK_CHECK(o[4].ts == 64); // +100us @90kHz
	TASK_CHECK(o[0].b1 == 96 && o[2].b1 == 96 && o[3].b1 == (0x80 | 96));
	TASK_CHECK(o[4].b1 == (0x80 | 96));

	// Rebuild the first NAL from the accepted fragments (0, retry 2, 3).
	std::vector<Offer> accepted_fu;
	accepted_fu.push_back(o[0]);
	accepted_fu.push_back(o[2]);
	accepted_fu.push_back(o[3]);
	TASK_CHECK(ReconstructFu(accepted_fu, 0, 3, 0x65) == video0_bytes);
	TASK_CHECK(Payload(o[4]) == video1_bytes);
	return 0;
}

int delivery_input_output_snapshot()
{
	ReplaySession rs;
	TASK_CHECK(rs.session->AddSource(channel_0, H264Source::CreateNew()));
	TASK_CHECK(rs.session->AddSource(channel_1, G711ASource::CreateNew()));
	auto config = rs.Config(0, 0, 8, 1u << 20);
	config.tracks.push_back(Track(channel_0, 21, 1, 0));
	config.tracks.push_back(Track(channel_1, 22, 1, 0));
	TASK_CHECK(rs.session->EnableTimedReplay(config));

	AVFrame video = MakeVideo(2000, 0x65, 0, 20); // 2 FU-A fragments
	AVFrame audio = MakeAudio(160, 0, 21);
	auto video_bytes = FrameBytes(video);
	auto audio_bytes = FrameBytes(audio);
	TASK_CHECK(rs.session->EnqueueReplay(channel_0, video, 0));
	TASK_CHECK(rs.session->EnqueueReplay(channel_1, audio, 0));

	// After a successful enqueue the caller may rewrite or free the frame.
	memset(video.buffer.get(), 0xEE, video.size);
	video = AVFrame(); // release the buffer entirely
	memset(audio.buffer.get(), 0x77, audio.size);

	rs.sink.hold = true; // keep accepted packets alive
	TASK_CHECK(rs.session->PumpReplay(0) == 3);
	TASK_CHECK(rs.sink.offers.size() == 3);

	// Delivered payloads are the enqueued snapshot, not the caller's buffer.
	TASK_CHECK(ReconstructFu(rs.sink.offers, 0, 2, 0x65) == video_bytes);
	TASK_CHECK(Payload(rs.sink.offers[2]) == audio_bytes);

	// More traffic must not overwrite packets the sink retained.
	TASK_CHECK(rs.session->EnqueueReplay(channel_0, MakeVideo(1500, 0x65, 0, 30), 1000));
	TASK_CHECK(rs.session->EnqueueReplay(channel_1, MakeAudio(160, 0, 31), 1000));
	TASK_CHECK(rs.session->PumpReplay(1000) == 3);
	TASK_CHECK(rs.sink.offers.size() == 6);
	for (size_t i = 0; i < 3; i++) {
		const Offer& offer = rs.sink.offers[i];
		TASK_CHECK(offer.held != nullptr);
		TASK_CHECK(memcmp(offer.held.get(), offer.bytes.data(), offer.bytes.size()) == 0);
	}
	return 0;
}

// ---------------------------------------------------------------- lifecycle

int lifecycle_mode_gating()
{
	// Production packetizers keep their legacy behavior when the replay
	// mode is not involved (baseline control).
	{
		std::unique_ptr<H264Source> video(H264Source::CreateNew());
		std::vector<RtpPacket> packets;
		video->SetSendFrameCallback([&packets](MediaChannelId channel, RtpPacket pkt) {
			if (channel != channel_0) {
				return false;
			}
			packets.push_back(pkt);
			return true;
		});

		AVFrame frame = MakeVideo(3000, 0x65, 0xfffffff0u, 1);
		TASK_CHECK(video->HandleFrame(channel_0, frame));
		TASK_CHECK(packets.size() == 3);
		for (const auto& pkt : packets) {
			TASK_CHECK(pkt.timestamp == 0xfffffff0u);
		}

		// Legacy ingress: timestamp==0 still means "use the live clock".
		packets.clear();
		TASK_CHECK(video->HandleFrame(channel_0, MakeVideo(100, 0x61, 0, 2)));
		TASK_CHECK(packets.size() == 1);
		TASK_CHECK(packets[0].timestamp != 0);

		// The replay packetize path keeps an explicit 0 instead.
		packets.clear();
		TASK_CHECK(video->PacketizeFrame(channel_0, MakeVideo(100, 0x61, 0, 2)));
		TASK_CHECK(packets.size() == 1);
		TASK_CHECK(packets[0].timestamp == 0);

		std::unique_ptr<G711ASource> audio(G711ASource::CreateNew());
		std::vector<RtpPacket> audio_packets;
		audio->SetSendFrameCallback([&audio_packets](MediaChannelId channel, RtpPacket pkt) {
			if (channel != channel_1) {
				return false;
			}
			audio_packets.push_back(pkt);
			return true;
		});
		TASK_CHECK(audio->HandleFrame(channel_1, MakeAudio(160, 8000, 3)));
		TASK_CHECK(audio_packets.size() == 1);
		TASK_CHECK(audio_packets[0].timestamp == 8000);
		TASK_CHECK(audio_packets[0].size == 4 + 12 + 160);
	}

	// Replay entry points are rejected while the mode is not enabled, and
	// the legacy entries keep working.
	{
		ReplaySession rs;
		TASK_CHECK(rs.session->AddSource(channel_0, H264Source::CreateNew()));
		TASK_CHECK(rs.session->AddSource(channel_1, G711ASource::CreateNew()));
		TASK_CHECK(!rs.session->IsTimedReplayEnabled());

		AVFrame audio = MakeAudio(160, 0, 1);
		TASK_CHECK(rs.session->HandleFrame(channel_1, audio)); // live ingress
		TASK_CHECK(!rs.session->EnqueueReplay(channel_1, audio, 0));
		TASK_CHECK(rs.session->PumpReplay(1000) == 0);
		TASK_CHECK(!rs.session->PauseReplay(0));
		TASK_CHECK(!rs.session->ResumeReplay(0));
		TASK_CHECK(!rs.session->SetReplaySpeed(1.0, 0));
		TASK_CHECK(!rs.session->ClearReplay(0));

		auto config = rs.Config(0, 0, 4, 1u << 16);
		config.tracks.push_back(Track(channel_0, 1, 1, 0));
		config.tracks.push_back(Track(channel_1, 2, 1, 0));
		TASK_CHECK(rs.session->EnableTimedReplay(config));

		// Once enabled, the legacy entries and re-enabling are rejected.
		TASK_CHECK(!rs.session->HandleFrame(channel_1, audio));
		TASK_CHECK(!rs.session->AddSource(channel_0, H264Source::CreateNew()));
		TASK_CHECK(!rs.session->RemoveSource(channel_0));
		TASK_CHECK(!rs.session->EnableTimedReplay(config));

		// The replay path works instead.
		TASK_CHECK(rs.session->EnqueueReplay(channel_1, audio, 0));
		TASK_CHECK(rs.session->PumpReplay(0) == 1);
		TASK_CHECK(rs.sink.offers.size() == 1);
	}
	return 0;
}

int lifecycle_enable_validation()
{
	// No sources added yet: nothing to correspond to.
	{
		ReplaySession rs;
		auto config = rs.Config(0, 0, 4, 4096);
		TASK_CHECK(!rs.session->EnableTimedReplay(config));
	}

	// Track configs must correspond exactly to the added sources.
	{
		ReplaySession rs;
		TASK_CHECK(rs.session->AddSource(channel_0, H264Source::CreateNew()));
		TASK_CHECK(rs.session->AddSource(channel_1, G711ASource::CreateNew()));
		auto good = rs.Config(0, 0, 4, 4096);
		good.tracks.push_back(Track(channel_0, 1, 1, 0));
		good.tracks.push_back(Track(channel_1, 2, 1, 0));

		{
			auto config = good; // missing track
			config.tracks.pop_back();
			TASK_CHECK(!rs.session->EnableTimedReplay(config));
		}
		{
			auto config = good; // duplicate channel
			config.tracks.push_back(Track(channel_0, 3, 1, 0));
			TASK_CHECK(!rs.session->EnableTimedReplay(config));
		}
		{
			auto config = good; // channel out of range
			config.tracks[1].channel = (MediaChannelId)5;
			TASK_CHECK(!rs.session->EnableTimedReplay(config));
		}
		{
			auto config = good; // negative media origin
			config.media_origin_us = -1;
			TASK_CHECK(!rs.session->EnableTimedReplay(config));
		}
		{
			auto config = good; // negative clock origin
			config.clock_origin_us = -1;
			TASK_CHECK(!rs.session->EnableTimedReplay(config));
		}
		{
			auto config = good; // frame limit must be positive
			config.max_frames = 0;
			TASK_CHECK(!rs.session->EnableTimedReplay(config));
		}
		{
			auto config = good; // byte limit must be positive
			config.max_bytes = 0;
			TASK_CHECK(!rs.session->EnableTimedReplay(config));
		}
		{
			auto config = good; // sink is required
			config.sink = nullptr;
			TASK_CHECK(!rs.session->EnableTimedReplay(config));
		}

		// Failed attempts leave the session ready for a valid config.
		TASK_CHECK(!rs.session->IsTimedReplayEnabled());
		TASK_CHECK(rs.session->EnableTimedReplay(good));
		TASK_CHECK(rs.session->IsTimedReplayEnabled());
	}

	// Unsupported source type (H265) cannot be replayed.
	{
		ReplaySession rs;
		TASK_CHECK(rs.session->AddSource(channel_0, H265Source::CreateNew()));
		auto config = rs.Config(0, 0, 4, 4096);
		config.tracks.push_back(Track(channel_0, 1, 1, 0));
		TASK_CHECK(!rs.session->EnableTimedReplay(config));
	}
	return 0;
}

int lifecycle_enqueue_validation()
{
	ReplaySession rs;
	TASK_CHECK(rs.session->AddSource(channel_0, H264Source::CreateNew()));
	TASK_CHECK(rs.session->AddSource(channel_1, G711ASource::CreateNew()));
	auto config = rs.Config(100, 0, 1000, 1u << 22);
	config.tracks.push_back(Track(channel_0, 1, 1, 0));
	config.tracks.push_back(Track(channel_1, 2, 1, 0));
	TASK_CHECK(rs.session->EnableTimedReplay(config));
	MediaSession* session = rs.session.get();

	// H264 input: non-empty, at most 256 KiB.
	TASK_CHECK(!session->EnqueueReplay(channel_0, MakeVideo(0, 0x65, 0, 1), 100));
	TASK_CHECK(session->EnqueueReplay(channel_0, MakeVideo(1, 0x65, 0, 1), 100));
	TASK_CHECK(session->EnqueueReplay(channel_0, MakeVideo(256 * 1024, 0x65, 0, 1), 200));
	TASK_CHECK(!session->EnqueueReplay(channel_0, MakeVideo(256 * 1024 + 1, 0x65, 0, 1), 300));

	// PCMA input: non-empty, at most MAX_RTP_PAYLOAD_SIZE.
	TASK_CHECK(!session->EnqueueReplay(channel_1, MakeAudio(0, 0, 1), 100));
	TASK_CHECK(session->EnqueueReplay(channel_1, MakeAudio(1, 0, 1), 100));
	TASK_CHECK(session->EnqueueReplay(channel_1, MakeAudio(MAX_RTP_PAYLOAD_SIZE, 0, 1), 200));
	TASK_CHECK(!session->EnqueueReplay(channel_1, MakeAudio(MAX_RTP_PAYLOAD_SIZE + 1, 0, 1), 300));

	// Unknown channels are rejected.
	TASK_CHECK(!session->EnqueueReplay((MediaChannelId)2, MakeAudio(10, 0, 1), 100));
	TASK_CHECK(!session->EnqueueReplay((MediaChannelId)5, MakeAudio(10, 0, 1), 100));

	// media_us must be >= M and non-decreasing per track.
	TASK_CHECK(!session->EnqueueReplay(channel_1, MakeAudio(10, 0, 1), -1));
	TASK_CHECK(!session->EnqueueReplay(channel_1, MakeAudio(10, 0, 1), 99));  // before M
	TASK_CHECK(!session->EnqueueReplay(channel_1, MakeAudio(10, 0, 1), 150)); // behind frontier (200)
	TASK_CHECK(session->EnqueueReplay(channel_1, MakeAudio(10, 0, 1), 200));  // equal is fine

	// After frames were presented to the sink, nothing earlier may be enqueued.
	TASK_CHECK(session->PumpReplay(200) > 0); // presents everything up to media 200
	TASK_CHECK(!session->EnqueueReplay(channel_0, MakeVideo(10, 0x65, 0, 1), 150));
	TASK_CHECK(session->EnqueueReplay(channel_0, MakeVideo(10, 0x65, 0, 1), 200));
	return 0;
}

int lifecycle_capacity_release()
{
	// Byte limit: a 3000-byte NAL snapshots to 1436+1436+181 = 3053 bytes
	// (including the 4-byte prefix), a 160-byte PCMA frame to 176.
	{
		ReplaySession rs;
		TASK_CHECK(rs.session->AddSource(channel_0, H264Source::CreateNew()));
		TASK_CHECK(rs.session->AddSource(channel_1, G711ASource::CreateNew()));
		auto config = rs.Config(0, 0, 10, 3053 + 176);
		config.tracks.push_back(Track(channel_0, 1, 1, 0));
		config.tracks.push_back(Track(channel_1, 2, 1, 0));
		TASK_CHECK(rs.session->EnableTimedReplay(config));
		MediaSession* session = rs.session.get();

		TASK_CHECK(session->EnqueueReplay(channel_0, MakeVideo(3000, 0x65, 0, 1), 0));
		TASK_CHECK(session->EnqueueReplay(channel_1, MakeAudio(160, 0, 2), 0)); // exactly full
		TASK_CHECK(!session->EnqueueReplay(channel_1, MakeAudio(160, 0, 3), 5)); // byte limit

		// Partial delivery does not release the frame's quota.
		rs.sink.policy = [](size_t index, MediaChannelId, const RtpPacket&) {
			return index != 2; // reject the last FU once
		};
		TASK_CHECK(session->PumpReplay(0) == 2);
		TASK_CHECK(!session->EnqueueReplay(channel_1, MakeAudio(160, 0, 3), 5));

		// Completing both frames releases everything.
		rs.sink.policy = nullptr;
		TASK_CHECK(session->PumpReplay(0) == 2);
		TASK_CHECK(rs.sink.offers.size() == 5);

		// The failed enqueues did not move the track frontier (still 0).
		TASK_CHECK(session->EnqueueReplay(channel_1, MakeAudio(160, 0, 3), 3));
		TASK_CHECK(session->PumpReplay(3) == 1);
	}

	// Byte accounting includes the 4-byte prefix: 3053+176 = 3229 > 3228.
	{
		ReplaySession rs;
		TASK_CHECK(rs.session->AddSource(channel_0, H264Source::CreateNew()));
		TASK_CHECK(rs.session->AddSource(channel_1, G711ASource::CreateNew()));
		auto config = rs.Config(0, 0, 10, 3228);
		config.tracks.push_back(Track(channel_0, 1, 1, 0));
		config.tracks.push_back(Track(channel_1, 2, 1, 0));
		TASK_CHECK(rs.session->EnableTimedReplay(config));
		TASK_CHECK(rs.session->EnqueueReplay(channel_0, MakeVideo(3000, 0x65, 0, 1), 0));
		TASK_CHECK(!rs.session->EnqueueReplay(channel_1, MakeAudio(160, 0, 2), 0));
	}

	// Frame count limit.
	{
		ReplaySession rs;
		TASK_CHECK(rs.session->AddSource(channel_1, G711ASource::CreateNew()));
		auto config = rs.Config(0, 0, 2, 1u << 20);
		config.tracks.push_back(Track(channel_1, 2, 1, 0));
		TASK_CHECK(rs.session->EnableTimedReplay(config));
		TASK_CHECK(rs.session->EnqueueReplay(channel_1, MakeAudio(160, 0, 1), 0));
		TASK_CHECK(rs.session->EnqueueReplay(channel_1, MakeAudio(160, 0, 2), 1));
		TASK_CHECK(!rs.session->EnqueueReplay(channel_1, MakeAudio(160, 0, 3), 2));
		TASK_CHECK(rs.session->PumpReplay(2) == 2); // drains both
		TASK_CHECK(rs.session->EnqueueReplay(channel_1, MakeAudio(160, 0, 3), 2));
	}
	return 0;
}

int lifecycle_callback_pause()
{
	ReplaySession rs;
	TASK_CHECK(rs.session->AddSource(channel_0, H264Source::CreateNew()));
	TASK_CHECK(rs.session->AddSource(channel_1, G711ASource::CreateNew()));
	auto config = rs.Config(0, 0, 8, 1u << 20);
	config.tracks.push_back(Track(channel_0, 11, 1, 0));
	config.tracks.push_back(Track(channel_1, 22, 50, 0));
	TASK_CHECK(rs.session->EnableTimedReplay(config));
	MediaSession* session = rs.session.get();

	TASK_CHECK(session->EnqueueReplay(channel_0, MakeVideo(3000, 0x65, 0, 1), 0)); // 3 FUs
	TASK_CHECK(session->EnqueueReplay(channel_1, MakeAudio(160, 0, 2), 0));

	// PauseReplay with the pump's own now is allowed inside the sink.
	bool pause_ok = false;
	rs.sink.policy = [&pause_ok, session](size_t index, MediaChannelId, const RtpPacket&) {
		if (index == 1) {
			pause_ok = session->PauseReplay(0);
		}
		return true;
	};

	// The accepted packet is committed, then the pump stops immediately.
	TASK_CHECK(session->PumpReplay(0) == 2);
	TASK_CHECK(pause_ok);
	TASK_CHECK(rs.sink.offers.size() == 2);
	TASK_CHECK(session->PumpReplay(1000) == 0); // paused

	TASK_CHECK(session->ResumeReplay(1000));
	TASK_CHECK(session->PumpReplay(1000) == 2); // remaining FU + audio
	TASK_CHECK(rs.sink.offers.size() == 4);
	// Sequence numbers continued across the pause.
	TASK_CHECK(rs.sink.offers[0].seq == 1 && rs.sink.offers[1].seq == 2);
	TASK_CHECK(rs.sink.offers[2].seq == 3 && rs.sink.offers[2].channel == channel_0);
	TASK_CHECK(rs.sink.offers[3].seq == 50 && rs.sink.offers[3].channel == channel_1);
	return 0;
}

int lifecycle_callback_clear()
{
	ReplaySession rs;
	TASK_CHECK(rs.session->AddSource(channel_0, H264Source::CreateNew()));
	TASK_CHECK(rs.session->AddSource(channel_1, G711ASource::CreateNew()));
	auto config = rs.Config(0, 0, 8, 1u << 20);
	config.tracks.push_back(Track(channel_0, 11, 1, 0));
	config.tracks.push_back(Track(channel_1, 22, 50, 0));
	TASK_CHECK(rs.session->EnableTimedReplay(config));
	MediaSession* session = rs.session.get();

	TASK_CHECK(session->EnqueueReplay(channel_0, MakeVideo(3000, 0x65, 0, 1), 0)); // 3 FUs
	TASK_CHECK(session->EnqueueReplay(channel_1, MakeAudio(160, 0, 2), 0));
	TASK_CHECK(session->EnqueueReplay(channel_1, MakeAudio(160, 0, 3), 1000));

	// ClearReplay with the pump's own now is allowed inside the sink.
	bool clear_ok = false;
	rs.sink.policy = [&clear_ok, session](size_t index, MediaChannelId, const RtpPacket&) {
		if (index == 0) {
			clear_ok = session->ClearReplay(0);
		}
		return true;
	};

	// The accepted packet counts; the rest of the queue (including the
	// current frame's suffix) is discarded and the pump stops.
	TASK_CHECK(session->PumpReplay(0) == 1);
	TASK_CHECK(clear_ok);
	TASK_CHECK(rs.sink.offers.size() == 1);
	TASK_CHECK(session->PumpReplay(100000) == 0); // FU1/FU2 and the audio are gone
	TASK_CHECK(rs.sink.offers.size() == 1);

	// The enqueue frontier reset to the current media position (0).
	TASK_CHECK(session->EnqueueReplay(channel_1, MakeAudio(160, 0, 5), 0));
	TASK_CHECK(session->EnqueueReplay(channel_0, MakeVideo(100, 0x61, 0, 6), 0));
	TASK_CHECK(session->PumpReplay(0) == 2);
	TASK_CHECK(rs.sink.offers.size() == 3);
	// Committed sequence numbers survived the clear.
	TASK_CHECK(rs.sink.offers[1].channel == channel_0 && rs.sink.offers[1].seq == 2);
	TASK_CHECK(rs.sink.offers[2].channel == channel_1 && rs.sink.offers[2].seq == 50);

	// Clear advances the play clock first; the frontier follows it.
	// (now_us is monotone: the clock already reached 100000 above.)
	TASK_CHECK(session->ClearReplay(200000)); // position -> 200000
	TASK_CHECK(!session->EnqueueReplay(channel_1, MakeAudio(160, 0, 7), 150000)); // behind frontier
	TASK_CHECK(session->EnqueueReplay(channel_1, MakeAudio(160, 0, 7), 200000));
	TASK_CHECK(session->PumpReplay(200000) == 1);
	TASK_CHECK(rs.sink.offers.size() == 4);
	TASK_CHECK(rs.sink.offers[3].seq == 51);
	return 0;
}

} // namespace

const struct task_case task_cases[] = {
	{ "clock", "half_speed_odd_us_and_mid_change", clock_half_speed_odd_us_and_mid_change },
	{ "clock", "pause_and_long_resume", clock_pause_and_long_resume },
	{ "clock", "explicit_zero_and_double_wrap", clock_explicit_zero_and_double_wrap },
	{ "delivery", "h264_multi_fu_interleaved_pcma", delivery_h264_multi_fu_interleaved_pcma },
	{ "delivery", "second_fragment_reject_retry", delivery_second_fragment_reject_retry },
	{ "delivery", "input_output_snapshot", delivery_input_output_snapshot },
	{ "lifecycle", "mode_gating", lifecycle_mode_gating },
	{ "lifecycle", "enable_validation", lifecycle_enable_validation },
	{ "lifecycle", "enqueue_validation", lifecycle_enqueue_validation },
	{ "lifecycle", "capacity_release", lifecycle_capacity_release },
	{ "lifecycle", "callback_pause", lifecycle_callback_pause },
	{ "lifecycle", "callback_clear", lifecycle_callback_clear },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
