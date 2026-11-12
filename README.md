# wav codec

[![GoDoc](https://godoc.org/github.com/go-audio/wav?status.svg)](https://godoc.org/github.com/go-audio/wav)
[![Go Report Card](https://goreportcard.com/badge/github.com/go-audio/wav)](https://goreportcard.com/report/github.com/go-audio/wav)
[![Coverage Status](https://codecov.io/gh/go-audio/wav/graph/badge.svg)](https://codecov.io/gh/go-audio/wav)
[![Build Status](https://travis-ci.org/go-audio/wav.svg)](https://travis-ci.org/go-audio/wav)

Wav audio file encoder and decoder. See [GoDoc](https://godoc.org/github.com/go-audio/wav) for more details.

## Playlist (cue/plst) playback

Some WAV files keep the raw recording in their `data` chunk while storing
the intended edit order in a `cue ` chunk plus a `plst` (playlist) chunk.
By default the decoder still reads the `data` chunk from start to end; set
`Decoder.UsePlaylist` to `true` before the first read to play the logical
stream described by the playlist instead.

```go
d := wav.NewDecoder(f)
d.UsePlaylist = true // must be set before the first read
```

Each playlist entry names a cue point by its raw four byte ID, plays a
number of frames starting at the cue's `SampleOffset`, and repeats that
block a number of times (a frame covers every channel). Entries with zero
frames or zero play count produce no sound. When the option is enabled,
the whole playlist and every range it references are validated before the
first sample is returned: truncated tables, duplicate cue IDs, missing
references, segments running past the data, non whole-frame data, and
duplicate `data` or `plst` chunks are all reported as errors. A file
without a `plst` chunk plays as the raw data, and an empty playlist is an
empty stream.

Playback is lazy: segments are seeked to and repeated on demand, so memory
use does not grow with play counts or the expanded playlist length. In
this mode `PCMBuffer`, `FullPCMBuffer`, `PCMLen`, `Duration`, `EOF` and
`Rewind` all describe the same logical stream. `PCMBuffer` still returns a
sample count and its buffer length must be a multiple of the channel
count; a misaligned buffer errors out without moving the position.

Use `SeekPlaylistFrame` to position the logical stream, including across
repeated segments:

```go
if err := d.SeekPlaylistFrame(frame); err != nil {
    // frame must be within [0, total frame count]
}
```

Raw byte `Seek` is rejected in playlist mode (use `SeekPlaylistFrame`);
`SeekPlaylistFrame` itself errors in the default raw-data mode.

Playlist mode supports standard RIFF/WAVE files with a single top level
`data` chunk, integer PCM at 8/16/24/32 bits, in mono or stereo. The
encoder, RF64 and `wavl` are not covered.

