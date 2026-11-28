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

## Timed replay (in-memory playback)

`MediaSession` has an optional timed replay mode: one audio/video timeline is
enqueued with media timestamps and pumped into an application-provided sink
with a manual microsecond clock. The production `H264Source`/`G711ASource`
packetizers and the `RtpConnection` RTP header logic are reused (no packetizer
is re-implemented); RTSP text commands, decoders and network I/O are not
involved. Only single H264 NAL and PCMA inputs are supported and the mode is
single threaded.

Enable it after all sources were added and while the session has no network
clients. Once enabled, `HandleFrame()`, `AddSource()`/`RemoveSource()` and
re-enabling are rejected; while not enabled, every replay entry point is
rejected.

```cpp
xop::MediaSession::TimedReplayConfig config;
config.media_origin_us = 0;   // M: common media timeline origin, >= 0
config.clock_origin_us = 0;   // W: local clock origin, >= 0; the play clock is M at W
config.max_frames    = 64;    // queued unfinished-frame limit, > 0
config.max_bytes     = 4u << 20; // queued RtpPacket.size limit (incl. the 4-byte prefix), > 0

// One entry per added source (exactly); channels must not repeat.
xop::MediaSession::TimedReplayTrackConfig video;
video.channel = xop::channel_0;
video.ssrc = 0x12345678;            // SSRC written into every RTP header of the track
video.initial_sequence = 0;         // first RTP sequence number of the track
video.initial_rtp_timestamp = 0;    // RTP timestamp of a frame at media_origin_us
config.tracks.push_back(video);
xop::MediaSession::TimedReplayTrackConfig audio = video;
audio.channel = xop::channel_1;
audio.ssrc = 0x87654321;
config.tracks.push_back(audio);

config.sink = [](xop::MediaChannelId channel, const xop::RtpPacket& packet) {
	// packet.data[0..3] is zero, the 12-byte RTP header is at data+4.
	// Return true to take the packet over, false to reject it (it is
	// offered again, unchanged, on the next pump).
	return true;
};

session->EnableTimedReplay(config); // false on bad config/clients/sources
```

Entry points (all `now_us` must be monotone, non-negative and `>= W`):

```cpp
bool     EnqueueReplay(MediaChannelId channel_id, AVFrame frame, int64_t media_us);
uint32_t PumpReplay(int64_t now_us);   // packets accepted this call
bool     PauseReplay(int64_t now_us);
bool     ResumeReplay(int64_t now_us);
bool     SetReplaySpeed(double speed, int64_t now_us); // 0.5, 1.0 or 2.0 only
bool     ClearReplay(int64_t now_us);
```

Key semantics:

- `EnqueueReplay` snapshots the frame through the production packetizer, so
  the caller may reuse or free it on return. Per-track `media_us` is
  non-decreasing (`>= M`, and never behind the largest `media_us` already
  presented to the sink). Capacity is counted in unfinished frames and the
  sum of their `RtpPacket.size`; a frame's quota is released only when its
  last fragment is accepted. Failures change nothing.
- A frame's RTP timestamp is `initial + floor((media_us - M) * clock_rate /
  1000000)` with 32-bit wrap, independent of playback speed; the input
  `AVFrame.timestamp` is ignored and an explicit 0 is kept as 0.
- The play clock starts at `M`, runs from `W` at 1x, and is advanced by
  `PumpReplay` and the control calls; pausing freezes it and speeds of
  1/2, 1 and 2 are supported (half-microsecond remainders are kept).
- `PumpReplay` offers due frames in `(media_us, channel, enqueue order)`,
  one packet at a time, and stops at the first rejected packet; the retry
  is byte-identical and sequence numbers only advance on acceptance.
- Inside the sink callback only `PauseReplay`/`ClearReplay` (with the
  pump's own `now_us`) are allowed: a pause commits the accepted packet
  and stops the pump; a clear advances the clock, drops the queued frames
  (including the current suffix) and resets each track's enqueue frontier
  to the current media position.

Replay loop sketch:

```cpp
int64_t now_us = config.clock_origin_us;
for (;;) {
	AVFrame nal = read_next_nal();            // your own timeline
	int64_t media_us = nal_media_time_us();
	while (!session->EnqueueReplay(xop::channel_0, nal, media_us)) {
		now_us += 10000;                      // queue full: drain first
		session->PumpReplay(now_us);
	}
	now_us += 10000;                          // manual clock, e.g. 10 ms steps
	session->PumpReplay(now_us);              // accepted packets go to the sink
}
```


## Dependencies
 - [websocketpp-md5](https://github.com/zaphoyd/websocketpp)

## License
 - [MIT License](https://github.com/PHZ76/RtspServer/blob/master/LICENSE)

## Contact
 - Email: penghaoze76@qq.com
