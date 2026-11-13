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

### CBR muxer (constant bitrate MPEG-TS)

`CBRMuxer` emits a constant-bit-rate transport stream. `Write` only queues
media packets; nothing is sent until `AdvanceTo` releases the transmission
slots whose 27 MHz position is strictly below the elapsed time you pass in.
PAT/PMT tables, PCR heartbeats and null packets fill every 188-byte slot, so
the output keeps flowing even when no media is queued. No timers or sleeps
are used: the caller decides how far the transport clock has advanced.

```go
package main

import (
    "os"

    "github.com/yapingcat/gomedia/go-mpeg2"
)

func main() {
    out, _ := os.Create("output.cbr.ts")
    defer out.Close()

    mux, err := mpeg2.NewCBRMuxer(mpeg2.CBRConfig{
        Bitrate:          4_000_000, // bit/s
        StartPCR27MHz:    0,
        PCRInterval27MHz: 2_700_000, // at most 100 ms between PCR values
        Streams: []mpeg2.TS_STREAM_TYPE{
            mpeg2.TS_STREAM_H264, // PID 0x100, also the PCR PID
            mpeg2.TS_STREAM_AAC, // PID 0x101
        },
    })
    if err != nil {
        panic(err)
    }

    // The buffer passed to OnPacket is only valid during the call.
    mux.OnPacket = func(pkg []byte) {
        if _, err := out.Write(pkg); err != nil {
            panic(err)
        }
    }

    videoPID, audioPID := mux.StreamPIDs()[0], mux.StreamPIDs()[1]

    // Feed access units ahead of time. Write never blocks and never calls
    // OnPacket; timestamps are in milliseconds and data may be reused after
    // Write returns.
    // mux.Write(videoPID, h264Frame, ptsMs, dtsMs)
    // mux.Write(audioPID, aacFrame, ptsMs, dtsMs)

    // Drive the transport clock. Each step advances it by 10 ms
    // (10 ms * 27 MHz = 270000 ticks). The same elapsed values always produce
    // the identical byte stream, whether advanced once or in many steps.
    var elapsed27MHz uint64
    for i := 0; i < 100; i++ {
        elapsed27MHz += 270000
        if err := mux.AdvanceTo(elapsed27MHz); err != nil {
            panic(err)
        }
        _ = videoPID
        _ = audioPID
    }
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
 





  
