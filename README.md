# wav codec

[![GoDoc](https://godoc.org/github.com/go-audio/wav?status.svg)](https://godoc.org/github.com/go-audio/wav)
[![Go Report Card](https://goreportcard.com/badge/github.com/go-audio/wav)](https://goreportcard.com/report/github.com/go-audio/wav)
[![Coverage Status](https://codecov.io/gh/go-audio/wav/graph/badge.svg)](https://codecov.io/gh/go-audio/wav)
[![Build Status](https://travis-ci.org/go-audio/wav.svg)](https://travis-ci.org/go-audio/wav)

Wav audio file encoder and decoder. See [GoDoc](https://godoc.org/github.com/go-audio/wav) for more details.

## WAVE_FORMAT_EXTENSIBLE PCM

Multi-channel PCM recordings commonly use the `WAVE_FORMAT_EXTENSIBLE`
(`0xfffe`) format tag. The decoder treats it as a first-class PCM path while
keeping regular PCM (format tag `1`) behavior unchanged. Only the PCM
subformat is supported; float and compressed subformats return an error.

### Decoding

`Decoder` exposes two additional fields:

- `ValidBitsPerSample uint16`: the number of bits actually carrying signal.
  `BitDepth` and `SampleBitDepth()` keep reporting the container width (16, 24
  or 32 bits). For regular PCM files `ValidBitsPerSample` equals `BitDepth`.
- `ChannelMask uint32`: the speaker layout. `0` means the layout is
  unspecified; when non-zero, the number of set bits equals the channel count.
  Regular PCM files always report `0`.

Samples are stored left-aligned in their containers with zero-filled low
padding bits. The decoder shifts the padding away, so `IntBuffer.Data` holds
signed integers at the effective bit depth and
`IntBuffer.SourceBitDepth` reports `ValidBitsPerSample`. A 24-bit container
with 20 valid bits therefore yields values in the 20-bit range regardless of
whether you use `FullPCMBuffer()` or frame-sized `PCMBuffer()` calls, and the
same holds after `Rewind()`.

### Encoding

Create the encoder with `audioFormat = 0xfffe` and set `BitDepth` to the
container width, then populate `Encoder.ValidBitsPerSample` and
`Encoder.ChannelMask` before the first `Write` call:

```go
e := wav.NewEncoder(out, 48000, 24 /* container bits */, 6, 0xfffe)
e.ValidBitsPerSample = 20
e.ChannelMask = 0x3f // 0 means unspecified layout
// IntBuffer.Data contains 20-bit signed integers; Write left-aligns them
// into the 24-bit containers with zero-filled padding bits.
if err := e.Write(buf); err != nil {
    // ...
}
```

`Write` accepts complete channel frames and may be called several times.
Invalid parameters, mismatched buffer formats or samples outside the valid
range fail before any byte is written and leave the encoder usable. The
written `fmt` chunk uses a 40-byte size with `cbSize = 22` and the PCM
subformat GUID; `blockAlign`, the average byte rate and the RIFF/data sizes
all match the actual payload. `WriteFrame` is rejected for extensible files
because a raw frame carries no valid-bit convention; use `Write` with an
`audio.IntBuffer` instead. It keeps working unchanged for regular PCM.
