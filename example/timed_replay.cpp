// Timed (in-memory) replay example.
//
// Shows how an application drives a MediaSession replay clock by hand while
// the session reuses the production H264/G711A packetizers.  No sockets, RTSP
// text or decoder are involved.

#include "xop/MediaSession.h"
#include "xop/H264Source.h"
#include "xop/G711ASource.h"

#include <cstdio>
#include <memory>
#include <vector>

namespace
{

struct CapturedPacket
{
	xop::MediaChannelId channel;
	xop::RtpPacket packet;
};

/* Pretend these buffers came from a decoder. */
xop::AVFrame MakeVideoFrame(uint32_t size)
{
	xop::AVFrame frame(size);
	frame.type = xop::VIDEO_FRAME_I;
	frame.buffer.get()[0] = 0x65;
	for (uint32_t i = 1; i < size; i++) {
		frame.buffer.get()[i] = (uint8_t)i;
	}
	return frame;
}

xop::AVFrame MakeAudioFrame(uint32_t size)
{
	xop::AVFrame frame(size);
	frame.type = xop::AUDIO_FRAME;
	for (uint32_t i = 0; i < size; i++) {
		frame.buffer.get()[i] = (uint8_t)(0x55 ^ i);
	}
	return frame;
}

} // namespace

int main()
{
	using namespace xop;

	const int64_t M = 0;			// common media origin
	const int64_t W = 1000000;		// local clock origin (us)

	std::unique_ptr<MediaSession> session(MediaSession::CreateNew("replay"));
	session->AddSource(channel_0, H264Source::CreateNew());
	session->AddSource(channel_1, G711ASource::CreateNew());

	std::vector<CapturedPacket> captured;

	ReplayConfig config;
	config.media_start_us     = M;
	config.wall_start_us      = W;
	config.max_pending_frames = 16;
	config.max_pending_bytes  = 1024 * 1024;

	ReplayTrackConfig video;
	video.channel = channel_0;
	video.ssrc = 0x11223344u;
	video.initial_sequence = 1000;
	video.initial_rtp_timestamp = 90000;
	config.tracks.push_back(video);

	ReplayTrackConfig audio;
	audio.channel = channel_1;
	audio.ssrc = 0x55667788u;
	audio.initial_sequence = 5000;
	audio.initial_rtp_timestamp = 0;
	config.tracks.push_back(audio);

	// The sink takes ownership of each accepted packet.  Returning false would
	// mean "not accepted" and the packet would be retried unchanged.
	config.sink = [&](MediaChannelId channel, const RtpPacket& pkt) {
		CapturedPacket item;
		item.channel = channel;
		item.packet = pkt;
		captured.push_back(item);
		return true;
	};

	if (!session->EnableTimedReplay(config)) {
		std::fprintf(stderr, "EnableTimedReplay failed\n");
		return 1;
	}

	// Queue a few frames on the shared media timeline.  media_us must not go
	// backwards per track and must be at least M.
	AVFrame video0 = MakeVideoFrame(3000);
	session->EnqueueReplay(channel_0, video0, M);
	AVFrame audio0 = MakeAudioFrame(160);
	session->EnqueueReplay(channel_1, audio0, M);
	AVFrame video1 = MakeVideoFrame(2000);
	session->EnqueueReplay(channel_0, video1, M + 40000);	// 40 ms later

	// Start at M at the wall clock origin: the first video and audio frames are
	// due, the 40 ms video frame is not.
	uint32_t accepted = session->PumpReplay(W);
	std::printf("t=W        accepted=%u total=%zu\n", accepted, captured.size());

	// Half speed: the position advances 0.5 us per wall us.
	session->SetReplaySpeed(W, 1, 2);
	accepted = session->PumpReplay(W + 40000);	// position is M + 20 ms
	std::printf("t=W+20ms   accepted=%u total=%zu\n", accepted, captured.size());

	// Pause, wait, resume: no time is lost while paused.
	session->PauseReplay(W + 40000);
	accepted = session->PumpReplay(W + 1000000);
	std::printf("paused     accepted=%u\n", accepted);
	session->ResumeReplay(W + 1000000);

	// 1x speed after resume; the 40 ms frame is due once the position reaches
	// M + 40 ms, i.e. 40 ms of resumed wall time later.
	session->SetReplaySpeed(W + 1000000, 1, 1);
	accepted = session->PumpReplay(W + 1040000);
	std::printf("resumed    accepted=%u total=%zu\n", accepted, captured.size());

	// Drop whatever is still queued; the clock and sequence numbers are kept.
	session->ClearReplay(W + 1050000);

	for (size_t i = 0; i < captured.size(); i++) {
		const uint8_t* d = captured[i].packet.data.get();
		std::printf("packet channel=%d seq=%u ts=%u size=%u\n",
		            (int)captured[i].channel,
		            (unsigned)(((unsigned)d[6] << 8) | d[7]),
		            (unsigned)(((uint32_t)d[8] << 24) | ((uint32_t)d[9] << 16) |
		                       ((uint32_t)d[10] << 8) | d[11]),
		            captured[i].packet.size);
	}

	return 0;
}
