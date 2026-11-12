# wav codec

[![GoDoc](https://godoc.org/github.com/go-audio/wav?status.svg)](https://godoc.org/github.com/go-audio/wav)
[![Go Report Card](https://goreportcard.com/badge/github.com/go-audio/wav)](https://goreportcard.com/report/github.com/go-audio/wav)
[![Coverage Status](https://codecov.io/gh/go-audio/wav/graph/badge.svg)](https://codecov.io/gh/go-audio/wav)
[![Build Status](https://travis-ci.org/go-audio/wav.svg)](https://travis-ci.org/go-audio/wav)

Wav audio file encoder and decoder. See [GoDoc](https://godoc.org/github.com/go-audio/wav) for more details.

## Playlist ("plst") playback

Some WAV files keep the original recording in the single `data` chunk but
describe the intended edit order in a `cue ` chunk plus a `plst` playlist
chunk. By default the decoder ignores those chunks and plays `data` from
start to end. Set `Decoder.UsePlaylist` to `true` before the first read to
follow the playlist instead:

```go
d := wav.NewDecoder(f)
d.UsePlaylist = true // must be set before reading any audio

buf := &audio.IntBuffer{Data: make([]int, 4096)}
for {
    n, err := d.PCMBuffer(buf)
    // err == io.EOF (or n == 0) marks the end of the logical stream
    ...
}
```

Each playlist entry references a cue point by its raw four byte ID and
plays a number of whole frames from that cue's sample offset, repeated a
number of times. A frame always covers every channel, and entries with a
zero frame length or zero play count produce no sound. When the file has
no `plst` chunk, playlist mode simply plays the raw `data` chunk; an empty
playlist is an empty stream.

In this mode `PCMBuffer`, `FullPCMBuffer`, `PCMLen`, `Duration`, `EOF` and
`Rewind` all describe the same logical stream. `PCMBuffer` still returns a
sample count, so the buffer length must be a multiple of the channel count;
an invalid length is rejected without moving the position. Repetitions are
played by seeking inside the original `data` chunk — the decoder never
expands the playlist, so memory does not grow with the play count.

Use `SeekPlaylistFrame` to position within the logical stream, across
segment and repetition boundaries. The valid range is `0` to the total
frame count (inclusive); an out of range seek leaves the position
untouched.

```go
if err := d.SeekPlaylistFrame(frame); err != nil {
    // frame was out of range
}
```

Raw byte `Seek` is rejected in playlist mode because physical offsets do
not map onto the edited stream; use `SeekPlaylistFrame` instead. Outside
playlist mode `SeekPlaylistFrame` is rejected and `Seek` keeps its original
meaning. Invalid or unsupported files (truncated tables, duplicate cue IDs
or chunks, missing references, segments outside `data`, a non whole-frame
data chunk) are reported when the playlist is validated, before the first
sample is returned.

Playlist mode supports plain RIFF/WAVE files with one top level `data`
chunk, integer PCM at 8/16/24/32 bits, mono or stereo. The encoder, RF64
and `wavl` are not affected.
