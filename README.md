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

### mpeg-ts mux: replacing the program at runtime

`TSMuxer` offers two mutually exclusive ways to describe the program:

- legacy: one `AddStream` call per track (behavior unchanged);
- explicit: `ConfigureProgram` replaces the whole program configuration in
  one call. The first successful call switches the muxer into explicit mode;
  mixing with `AddStream` is rejected (ConfigureProgram returns an error
  after `AddStream`, and `AddStream` panics after a successful
  `ConfigureProgram`).

```go
mux := mpeg2.NewTSMuxer()
var ts bytes.Buffer
mux.OnPacket = func(pkg []byte) {
    // pkg is owned by the muxer and its backing buffer may be reused for
    // the packets that follow (e.g. the remaining packets of the same PES).
    // Hand it to the consumer synchronously, or copy it (append([]byte(nil), pkg...))
    // if it must be retained or queued for later use.
    ts.Write(pkg)
}

//program number stays 1, PMT PID stays 0x200. PIDs must be in 0x20..0x1FFE
//(excluding 0x200) and unique; pcrPID must be one of the stream PIDs.
err := mux.ConfigureProgram([]mpeg2.ProgramStream{
    {PID: 0x100, Type: mpeg2.TS_STREAM_H264},
    {PID: 0x101, Type: mpeg2.TS_STREAM_AAC, Descriptors: []byte{0x0A, 0x04, 0x7A, 0x61, 0x6E, 0x67}},
}, 0x100)
if err != nil {
    //invalid configuration: nothing changed, nothing was emitted
    return err
}

//ConfigureProgram emits nothing itself. The next valid Write first outputs
//PAT and the complete new PMT (spanning as many TS packets as needed), then
//the PES of this Write - even if the periodic 400ms table interval has not
//elapsed. Writes to PIDs that are not in the current configuration fail
//without consuming the pending publication.
_ = mux.Write(0x100, h264Frame, pts, dts)

//replacing the audio track at runtime: the first published PMT has version
//0, every actual change of order/PID/type/descriptors/PCR-PID bumps the
//version (mod 32); an identical configuration changes nothing. Continuity
//counters of kept (and re-added) PIDs continue where they stopped.
_ = mux.ConfigureProgram([]mpeg2.ProgramStream{
    {PID: 0x100, Type: mpeg2.TS_STREAM_H264},
    {PID: 0x102, Type: mpeg2.TS_STREAM_AAC},
}, 0x100)
_ = mux.Write(0x102, aacFrame, pts, dts) //PAT + PMT v1, then the PES
```

`ProgramStream.Descriptors` holds raw ES_info descriptors as a sequence of
`tag/length/body` items; the bytes are copied, validated and later carried
verbatim in the PMT. A single descriptor item may not exceed 255 bytes and
the whole PMT `section_length` may not exceed 1021; violations, empty stream
lists, unknown stream types, malformed descriptors, duplicate/out-of-range
PIDs or a PCR PID outside the list make `ConfigureProgram` fail without
changing the current configuration.


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
 





  
