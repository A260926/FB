# gomedia
 mpeg-ts,mpeg-ps,flv,mp4,rtmp muxer/demuxer
 
## Installation
```
go get github.com/yapingcat/gomedia
```


## H264/H265/AAC/VP8/OPUS/MP3
 [USAGE](https://github.com/yapingcat/gomedia/blob/main/go-codec/README.md)
  - decode sps/pps/vps/slice header
  - decode HEVCDecoderConfigurationRecord/AVCDecoderConfigurationRecord/AAC-ADTS/AudioSpecificConfiguration
  - encode HEVCDecoderConfigurationRecord/AVCDecoderConfigurationRecord/AAC-ADTS/AudioSpecificConfiguration
  - decode OPUS Extradata(ID Head "OpusHead") /OPUS Packet(TOC...)
  - encode OPUS Extradata
  - decode VP8 Frame Tag/Key Frame Head
  - decode MP3 Frame head

## mpeg-ts
  - mux
    - H264
    - H265
    - AAC
    - MP3
  - demux
    - H264
    - H265
    - AAC
    - MP3

### Constant-bitrate MPEG-TS (CBRMuxer)

`CBRMuxer` packetizes media through the same PES/program-table path as
`TSMuxer`, but output is paced by an explicit 27 MHz clock instead of
wall time. Every PAT/PMT/PCR/media/null packet occupies the exact slot
`floor(n*188*8*27000000/Bitrate)`. Feed packets with `Write` (millisecond
PTS/DTS, the buffer may be reused immediately) and drive output with
`AdvanceTo`; no sleeping or networking timer is involved.

```go
package main

import (
	"os"

	"github.com/yapingcat/gomedia/go-mpeg2"
)

func main() {
	f, _ := os.Create("cbr.ts")
	defer f.Close()

	mux, err := mpeg2.NewCBRMuxer(mpeg2.CBRConfig{
		Bitrate:             4_000_000, // bit/s, 128 kbit/s .. 1 Gbit/s
		StartPCR27MHz:       0,
		PCRInterval27MHz:    540_000, // 20 ms, must fit >= 3 packet slots
		Streams: []mpeg2.TS_STREAM_TYPE{
			mpeg2.TS_STREAM_H264, // PID 0x0100 and PCR PID
			mpeg2.TS_STREAM_AAC,  // PID 0x0101
		},
	})
	if err != nil {
		panic(err)
	}

	// The callback buffer is only valid during the call; write it directly.
	mux.OnPacket = func(pkt []byte) { f.Write(pkt) }

	pids := mux.StreamPIDs()
	videoPID, audioPID := pids[0], pids[1]

	// Annex-B frame and ADTS AAC frame; pts/dts are in milliseconds.
	var h264Frame, aacFrame []byte
	mux.Write(audioPID, aacFrame, 40, 40) // audio may be written first
	mux.Write(videoPID, h264Frame, 40, 40)

	// A caller-owned monotonic 27 MHz clock drives the muxer. Advancing in
	// one jump or many small jumps yields the same output byte stream.
	var elapsed uint64
	for mux.PendingPackets() > 0 {
		elapsed += 90_000 // ~3.3 ms in 27 MHz ticks
		if err := mux.AdvanceTo(elapsed); err != nil {
			panic(err) // e.g. elapsed moved backwards or OnPacket is nil
		}
	}

	// The stream never goes idle: advancing further emits PAT/PMT repeats,
	// PCR heartbeats and 0x1FFF null packets to hold the bitrate.
	mux.AdvanceTo(elapsed + 27_000_000)
}
```

## mpeg-ps
  - mux 
    - H264
    - H265
    - AAC
    - G711A
    - G711U
  - demux 
    - H264
    - H265
    - AAC
    - G711A
    - G711U
   
## flv
  - mux 
    - H264
    - H265
    - AAC
    - G711A
    - G711U
    - MP3
  - demux 
    - H264
    - H265
    - AAC
    - G711A
    - G711U
    - MP3
  
## mp4
  - demux 
    - H264
    - H265
    - AAC
    - G711A
    - G711U
    - MP3
  - mux 
    - H264
    - H265
    - AAC
    - G711A
    - G711U
    - MP3
    - OPUS


## fmp4
  - demux 
    - H264
    - H265
    - AAC
    - G711A
    - G711U
  - mux 
    - H264
    - H265
    - AAC
    - G711A
    - G711U

## ogg
  - demux 
    - OPUS
    - VP8
  
## rtmp
  
  [USAGE](https://github.com/yapingcat/gomedia/blob/main/go-rtmp/README.md)
  
  - support client/server
  - support play/publish
  - support h264/h265/aac/g711a/g711u/mp3
  
  
## rtsp

  - support client/server(rfc2326)
  - support basic/digest
  - support rtp(rfc3550)
  - support g711/aac/h264/h265
 





  
