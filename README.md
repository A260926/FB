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

### Replacing the whole program at runtime (`ConfigureProgram`)

`ConfigureProgram` atomically replaces the single-program configuration
(program number `1`, PMT PID `0x0200`). After the first successful call the
muxer runs in explicit mode and must not be mixed with `AddStream`; the old
`AddStream` usage is unchanged.

```go
var out [][]byte

muxer := mpeg2.NewTSMuxer()

// OnPacket always receives one 188-byte TS packet.
muxer.OnPacket = func(pkt []byte) {
    // The slice (and, for PES packets, its backing array) can be reused by
    // the next muxer call. Copy the bytes when buffering, handing them to
    // another goroutine, or retaining them; writing them out synchronously
    // (e.g. to a net.Conn or file) inside the callback is fine.
    out = append(out, append([]byte(nil), pkt...))
}

// Descriptors are opaque, complete tag/length/body strings; several can be
// concatenated. The bytes are copied and preserved verbatim in the PMT.
exampleDesc := []byte{0x05, 0x03, 'a', 'b', 'c'} // tag=5, length=3, body

streams := []mpeg2.ProgramStream{
    {PID: 0x0100, Type: mpeg2.TS_STREAM_H264},
    {PID: 0x0101, Type: mpeg2.TS_STREAM_AAC, Descriptors: exampleDesc},
}
if err := muxer.ConfigureProgram(streams, 0x0100 /* PCR PID */); err != nil {
    // empty list, unknown type, bad PID/PCR, malformed/oversized
    // descriptors, or a PMT section_length > 1021
    log.Fatal(err)
}

// The first valid Write emits PAT + the complete PMT (split across TS
// packets when needed) and then this PES, even less than 400ms after the
// previous announcement. pts/dts are in milliseconds.
if err := muxer.Write(0x0100, annexbFrame, ptsMS, dtsMS); err != nil {
    log.Fatal(err)
}

// Later on, replace every track at once (keep/remove/add PIDs, switch PCR).
// The PMT version increments by one mod 32 only when the ordered list,
// PIDs, types, descriptors or PCR PID actually change. Continuity counters
// of retained, removed-and-readded PIDs continue from their last value.
_ = muxer.ConfigureProgram(newStreams, newPCRPID)
```

Rules enforced by `ConfigureProgram`:

- at least one stream; types limited to AAC, MPEG-1/2 Audio, H.264 and H.265;
- elementary PIDs in `0x0020..0x1FFE`, unique, excluding PMT PID `0x0200`;
- `pcrPID` must be one of the configured PIDs and is never auto-reselected;
- `Descriptors` is a concatenation of complete `tag/length/body` descriptors
  and is copied; one item may carry at most 255 descriptor bytes;
- the whole PMT `section_length` must not exceed 1021;
- any validation error leaves the previous configuration untouched and
  emits nothing. `ConfigureProgram` itself never emits packets.

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
 





  
