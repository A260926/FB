# PLEASE USE joy5 INSTEAD

[joy5](https://github.com/nareix/joy5)

- High performance Copy-on-write gop cache [code](https://github.com/nareix/joy5/blob/master/cmd/avtool/pubsub.go)
- Better av.Packet design [code](https://github.com/nareix/joy5/blob/master/av/av.go)

# JOY4

> Golang audio/video library and streaming server

JOY4 is powerful library written in golang, well-designed interface makes a few lines of code can do a lot of things such as reading, writing, transcoding among variety media formats, or setting up high-performance live streaming server.

# Features 

Well-designed and easy-to-use interfaces:

- Muxer / Demuxer ([doc](https://godoc.org/github.com/nareix/joy4/av#Demuxer) [example](https://github.com/nareix/joy4/blob/master/examples/open_probe_file/main.go))
- Audio Decoder ([doc](https://godoc.org/github.com/nareix/joy4/av#AudioDecoder) [example](https://github.com/nareix/joy4/blob/master/examples/audio_decode/main.go))
- Transcoding ([doc](https://godoc.org/github.com/nareix/joy4/av/transcode) [example](https://github.com/nareix/joy4/blob/master/examples/transcode/main.go))
- Streaming server ([example](https://github.com/nareix/joy4/blob/master/examples/http_flv_and_rtmp_server/main.go))

Support container formats:

- MP4
- MPEG-TS
- FLV
- AAC (ADTS)

RTSP Client
- High level camera bug tolerance
- Support STAP-A

RTMP Client
- Support publishing to nginx-rtmp-server
- Support playing

RTMP / HTTP-FLV Server 
- Support publishing clients: OBS / ffmpeg / Flash Player (>8)
- Support playing clients: Flash Player 11 / VLC / ffplay / mpv
- High performance


Publisher-subscriber packet buffer queue ([doc](https://godoc.org/github.com/nareix/joy4/av/pubsub))

- Customize publisher buffer time and subscriber read position


- Multiple channels live streaming ([example](https://github.com/nareix/joy4/blob/master/examples/rtmp_server_channels/main.go))

Packet filters ([doc](https://godoc.org/github.com/nareix/joy4/av/pktque))

- Wait first keyframe
- Fix timestamp
- Make A/V sync
- Customize ([example](https://github.com/nareix/joy4/blob/master/examples/rtmp_server_channels/main.go#L19))

FFMPEG Golang-style binding ([doc](https://godoc.org/github.com/nareix/joy4/cgo/ffmpeg))
- Audio Encoder / Decoder
- Video Decoder
- Audio Resampler

Support codec and container parsers:

- H264 SPS/PPS/AVCDecoderConfigure parser ([doc](https://godoc.org/github.com/nareix/joy4/codec/h264parser))
- AAC ADTSHeader/MPEG4AudioConfig parser ([doc](https://godoc.org/github.com/nareix/joy4/codec/aacparser))
- MP4 Atoms parser ([doc](https://godoc.org/github.com/nareix/joy4/format/mp4/mp4io))
- FLV AMF0 object parser ([doc](https://godoc.org/github.com/nareix/joy4/format/flv/flvio))

# Offline MP4 rechunk (fast start)

`mp4.Rechunk` rewrites a finished, non-fragmented, self-contained MP4 (plain
avc1/mp4a tracks) into a layout that is cheaper to read over limited seeking:
the `moov` box is moved to the front, the media is written as a single `mdat`,
and samples are re-grouped into multi-sample chunks interleaved by each
chunk's first sample decode timestamp. Compressed sample bytes and the
`stsd`/`stsz`/`stts`/`ctts`/`stss` tables are preserved as-is: nothing is
decoded and no packet is re-muxed or time re-quantized.

```go
package main

import (
	"log"
	"os"
	"time"

	"github.com/nareix/joy4/format/mp4"
)

func main() {
	in, err := os.Open("input.mp4")
	if err != nil {
		log.Fatal(err)
	}
	defer in.Close()

	out, err := os.Create("output.mp4")
	if err != nil {
		log.Fatal(err)
	}

	// Group each track's samples into chunks whose accumulated decode
	// duration is at most one second, then write moov-first output.
	if err := mp4.Rechunk(in, out, time.Second); err != nil {
		out.Close()
		// A validation error leaves out completely unwritten.
		log.Fatal(err)
	}
	if err := out.Close(); err != nil {
		log.Fatal(err)
	}
}
```

A runnable command lives in
[examples/mp4_rechunk](examples/mp4_rechunk/main.go).

# Requirements

Go version >= 1.6

ffmpeg version >= 3.0 (optional)

# TODO

HLS / MPEG-DASH Server

ffmpeg.VideoEncoder / ffmpeg.SWScale

# License

MIT
