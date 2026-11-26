# gohlslib

[![Test](https://github.com/bluenviron/gohlslib/actions/workflows/test.yml/badge.svg?branch=main)](https://github.com/bluenviron/gohlslib/actions/workflows/test.yml?query=branch%3Amain)
[![Lint](https://github.com/bluenviron/gohlslib/actions/workflows/lint.yml/badge.svg?branch=main)](https://github.com/bluenviron/gohlslib/actions/workflows/lint.yml?query=branch%3Amain)
[![CodeCov](https://codecov.io/gh/bluenviron/gohlslib/branch/main/graph/badge.svg)](https://app.codecov.io/gh/bluenviron/gohlslib/tree/main)
[![PkgGoDev](https://pkg.go.dev/badge/github.com/bluenviron/gohlslib/v2)](https://pkg.go.dev/github.com/bluenviron/gohlslib/v2#pkg-index)

HLS client and muxer library for the Go programming language, written for [MediaMTX](https://github.com/bluenviron/mediamtx).

Go &ge; 1.26 is required.

Features:

- Client
  - Read streams in MPEG-TS, fMP4 or Low-latency format
  - Read a single video track and/or multiple audio tracks
  - Read tracks encoded with AV1, VP9, H265, H264, Opus, FLAC, MPEG-4 Audio (AAC)
  - Get absolute timestamp of incoming data
  - Follow fMP4 discontinuities, with timelines that restart their `tfdt` and
    renditions that enter the next generation at different times

- Muxer
  - Generate streams in MPEG-TS, fMP4 or Low-latency format
  - Write a single video track and/or multiple audio tracks
  - Write tracks encoded with AV1, VP9, H265, H264, Opus, FLAC, MPEG-4 audio (AAC), KLV
  - Save generated segments on disk

- General
  - Parse and produce M3U8 playlists
  - Examples

## Table of contents

- [Examples](#examples)
- [fMP4 discontinuities](#fmp4-discontinuities)
- [API Documentation](#api-documentation)
- [Specifications](#specifications)
- [Related projects](#related-projects)

## Examples

- [playlist-parser](examples/playlist-parser/main.go)
- [client](examples/client/main.go)
- [client-absolute-timestamp](examples/client-absolute-timestamp/main.go)
- [client-codec-h264-save-to-disk](examples/client-codec-h264-save-to-disk/main.go)
- [client-codec-h264-convert-to-jpeg](examples/client-codec-h264-convert-to-jpeg/main.go)
- [client-codec-mpeg4audio-save-to-disk](examples/client-codec-mpeg4audio-save-to-disk/main.go)
- [muxer](examples/muxer/main.go)

## fMP4 discontinuities

Traditional (non low-latency) fMP4 streams can reset every track's `tfdt` at a
discontinuity, and separate audio/video renditions can enter the next
generation at different moments. To follow such streams, set
`Client.EnableFMP4Discontinuities` (it defaults to `false`):

```go
c := &gohlslib.Client{
	URI:                       "...",
	EnableFMP4Discontinuities: true,
}
```

When it is enabled:

- each rendition is synchronized through the absolute discontinuity sequence
  (`EXT-X-DISCONTINUITY-SEQUENCE`, defaulting to `0`, plus the number of
  `EXT-X-DISCONTINUITY` tags up to and including the current segment), so a
  sliding `MEDIA-SEQUENCE` window that moves old tags away does not re-anchor
  the timeline;
- the `PROGRAM-DATE-TIME` of the leading rendition's first segment defines a
  global time origin, and the first segment of every generation must carry a
  `PROGRAM-DATE-TIME`;
- DTS is continuous across generations, while PTS keeps its signed
  `PTSOffset` and absolute timestamps follow the `PROGRAM-DATE-TIME`;
- at most two adjacent generations are tracked. A leading rendition that wants
  to advance further waits until every rendition that is not at its end of
  stream has left the oldest generation.

The initialization object and the track set must stay the same across
discontinuities; switching the `EXT-X-MAP` or the codecs is not supported.
MPEG-TS streams and low-latency fMP4 parts keep their previous behavior, and so
does the client when the option is disabled.

## API Documentation

[Click to open the API Documentation](https://pkg.go.dev/github.com/bluenviron/gohlslib#pkg-index)

## Specifications

| name                                                                                          | area           |
| --------------------------------------------------------------------------------------------- | -------------- |
| [RFC2616, HTTP 1.1](https://datatracker.ietf.org/doc/html/rfc2616)                            | protocol       |
| [RFC8216, HLS](https://datatracker.ietf.org/doc/html/rfc8216)                                 | protocol       |
| [RFC8216bis, HLS v2](https://datatracker.ietf.org/doc/html/draft-pantos-hls-rfc8216bis)       | protocol       |
| [HTTP Live Streaming by Apple](https://developer.apple.com/documentation/http-live-streaming) | protocol       |
| [Codec specifications](https://github.com/bluenviron/mediacommon#specifications)              | codecs         |
| [Golang project layout](https://github.com/golang-standards/project-layout)                   | project layout |

## Related projects

- [MediaMTX](https://github.com/bluenviron/mediamtx)
- [gortsplib](https://github.com/bluenviron/gortsplib)
- [gortmplib](https://github.com/bluenviron/gortmplib)
- [mediacommon](https://github.com/bluenviron/mediacommon)
