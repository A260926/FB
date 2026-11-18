## RtspServer
A RTSP server and pusher based on C++11

[中文介绍](https://github.com/PHZ76/RtspServer/blob/master/README_CN.md)

## Features
 - Support Linux and Windows platforms.
 - Support H.264/H.265/G711A/AAC
 - Support unicast(rtp over udp, rtp over rtsp), multicast
 - Support digest authentication

## Requirements
- Compiler support c++11, gcc4.8/vs2015 or above.

## Example
 - [rtsp-server](https://github.com/PHZ76/RtspServer/blob/master/example/rtsp_h264_file.cpp)
 - [DesktopSharing](https://github.com/PHZ76/DesktopSharing)

## Dependencies
 - [websocketpp-md5](https://github.com/zaphoyd/websocketpp)

## License
 - [MIT License](https://github.com/PHZ76/RtspServer/blob/master/LICENSE)

## Contact
 - Email: penghaoze76@qq.com

## In-memory timed replay

`MediaSession` can optionally drive an application-owned, in-memory replay of
one audio/video timeline.  In this mode the session owns no sockets: frames are
packetized by the installed `H264Source`/`G711ASource` using the same production
fragmentation as the live path, and the resulting RTP packets are handed to a
sink on a manual microsecond clock.  Neither RTSP text, the decoder nor the
network path is involved.

The mode is enabled with `EnableTimedReplay`, fed with `EnqueueReplay` and
drained with `PumpReplay`; `PauseReplay`, `ResumeReplay`, `SetReplaySpeed` and
`ClearReplay` control the playback clock.  See `src/xop/MediaSession.h` for the
exact `ReplayConfig`/`ReplayTrackConfig` layouts and signatures.  Enabling
requires that `AddSource` has already installed every source (only H264 and
PCMA are supported) and that no network client is attached.  Once enabled,
`HandleFrame`, `AddSource` and `RemoveSource` are refused; before enabling, the
replay entry points are refused.

```c++
using namespace xop;

std::unique_ptr<MediaSession> session(MediaSession::CreateNew("replay"));
session->AddSource(channel_0, H264Source::CreateNew());
session->AddSource(channel_1, G711ASource::CreateNew());

ReplayConfig config;
config.media_start_us     = 0;        // M, common media origin
config.wall_start_us      = now_us(); // W, local clock origin
config.max_pending_frames = 64;
config.max_pending_bytes  = 4 * 1024 * 1024;

ReplayTrackConfig video;
video.channel = channel_0;
video.ssrc = 0x11223344;
video.initial_sequence = 1000;
video.initial_rtp_timestamp = 90000;
config.tracks.push_back(video);

ReplayTrackConfig audio;
audio.channel = channel_1;
audio.ssrc = 0x55667788;
audio.initial_sequence = 5000;
audio.initial_rtp_timestamp = 0;
config.tracks.push_back(audio);

config.sink = [](MediaChannelId channel, const RtpPacket& pkt) {
    // The RTP header is at pkt.data+4; the first four bytes are zero.
    // Returning true means the whole packet was accepted.
    return my_rtp_queue.push(channel, pkt);
};

if (!session->EnableTimedReplay(config)) {
    return;
}

// Queue frames on one shared media timeline (media_us must not go backwards
// per track, must be >= M, and must not precede anything already delivered).
AVFrame video_frame(size);
session->EnqueueReplay(channel_0, video_frame, media_us);

AVFrame audio_frame(160);
session->EnqueueReplay(channel_1, audio_frame, media_us);

// Drive the clock by hand: the playback position starts at M and advances at
// the current speed from W.  Pump returns the number of accepted packets.
uint32_t accepted = session->PumpReplay(current_us);

// 1/2x, 1x and 2x are supported.
session->SetReplaySpeed(current_us, 1, 2);
session->PauseReplay(current_us);
session->ResumeReplay(current_us);
session->ClearReplay(current_us);   // drop queued frames, keep clock/seq
```

`EnqueueReplay` snapshots the frame, so the caller may release its buffer as
soon as the call returns.  RTP timestamps are
`initial_rtp_timestamp + floor((media_us - M) * clock_rate / 1000000)` (32-bit
wrapping) and are independent of the playback speed; an explicit media time of
`0` is valid.  Capacity is counted in unfinished frames and in the total
`RtpPacket::size` of their fragments (four-byte prefix included); a frame's
budget is released only after its last fragment is accepted.  `PumpReplay`
retries a rejected packet with the same channel, sequence, timestamp, marker
and payload, and does not resend fragments that already succeeded.  The sink may
retain accepted `std::shared_ptr`s.  From inside the sink callback only
`PauseReplay` and `ClearReplay` are allowed.

The regression suite lives in `tests/timed_replay.cpp` and is run with
`bash task_support/run.sh target`; `bash task_support/run.sh baseline` checks the
ordinary H264/PCMA packetization baseline.  A complete runnable loop is in
`example/timed_replay.cpp`.
