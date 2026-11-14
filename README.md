# gortmplib

[![Test](https://github.com/bluenviron/gortmplib/actions/workflows/test.yml/badge.svg?branch=main)](https://github.com/bluenviron/gortmplib/actions/workflows/test.yml?query=branch%3Amain)
[![Lint](https://github.com/bluenviron/gortmplib/actions/workflows/lint.yml/badge.svg?branch=main)](https://github.com/bluenviron/gortmplib/actions/workflows/lint.yml?query=branch%3Amain)
[![CodeCov](https://codecov.io/gh/bluenviron/gortmplib/branch/main/graph/badge.svg)](https://app.codecov.io/gh/bluenviron/gortmplib/tree/main)
[![PkgGoDev](https://pkg.go.dev/badge/github.com/bluenviron/gortmplib)](https://pkg.go.dev/github.com/bluenviron/gortmplib#pkg-index)

RTMP client and server library for the Go programming language, forked from [MediaMTX](https://github.com/bluenviron/mediamtx).

This was created to provide [Enhanced RTMP](https://veovera.org/docs/enhanced/enhanced-rtmp-v2) features, like multiple video/audio tracks and additional codecs.

Go &ge; 1.26 is required.

Features:

- Play and publish multiple video and audio tracks
- Play and publish tracks encoded with AV1, VP9, H265, H264, Opus, FLAC, MPEG-4 Audio (AAC), MPEG-1/2 Audio (MP3), AC-3, G711 (PCMA, PCMU), LPCM
- Support most Enhanced RTMP features
- Support TLS encryption (RTMPS)
- Support Diffie-hellman based encryption (RTMPE)
- Support encoding and decoding data with the AMF0 format

## Table of contents

- [Examples](#examples)
- [API Documentation](#api-documentation)
- [Specifications](#specifications)
- [Related projects](#related-projects)

## Examples

- [client-play](examples/client-play/main.go)
- [client-publish-h264](examples/client-publish-h264/main.go)
- [client-publish-mpeg4audio](examples/client-publish-mpeg4audio/main.go)
- [client-republish](examples/client-republish/main.go)
- [server](examples/server/main.go)

## API Documentation

[Click to open the API Documentation](https://pkg.go.dev/github.com/bluenviron/gortmplib#pkg-index)

## Clock and acknowledgement wire evidence

The media clock and acknowledgement behavior of `pkg/rawmessage` is pinned
to the exact bytes on the wire by `pkg/rawmessage/clock_wire_test.go`, which
encodes and parses chunks with an independent byte model instead of the
library's own chunk codec. A worked cross-chunk-stream example:

| event | chunk stream | header | clock effect |
| ----- | ------------ | ------ | ------------ |
| 1 | 4 | fmt0, extended timestamp 0xFFFFFFF0 | video message opens at 4294967280 ms |
| 2 | 6 | fmt0, timestamp 500 | audio message opens on an independent clock |
| 3 | 4 | fmt3, extended timestamp inherited | video continuation, clock unchanged |
| 4 | 6 | fmt1, delta 20 | audio clock advances to 520 ms |
| 5 | 4 | fmt2, delta 0x20 | video clock wraps to 16 ms (uint32) |
| 6 | 6 | fmt3 | next audio message, clock advances by the previous delta |

The acknowledgement window counts the bytes that actually enter the
connection — compressed headers, extended timestamp fields and continuation
headers included — and the acknowledged position advances by exactly one
window per `OnAckNeeded` callback.

The older round-trip tests serialized chunk objects and decoded them again
with the same library, so a consistent misunderstanding of the wire format
in both directions could pass unnoticed. The independent byte reference
closes that gap for header layout, fmt 0/1/2/3 selection, timestamp delta
inheritance and acknowledgement byte counting.

## Specifications

| name                                                                                      | area           |
| ----------------------------------------------------------------------------------------- | -------------- |
| [Action Message Format - AMF0](https://veovera.org/docs/legacy/amf0-file-format-spec.pdf) | RTMP           |
| [FLV](https://veovera.org/docs/legacy/video-file-format-v10-1-spec.pdf)                   | RTMP           |
| [RTMP](https://veovera.org/docs/legacy/rtmp-v1-0-spec.pdf)                                | RTMP           |
| [Enhanced RTMP v2](https://veovera.org/docs/enhanced/enhanced-rtmp-v2)                    | RTMP           |
| [Codec specifications](https://github.com/bluenviron/mediacommon#specifications)          | codecs         |
| [Golang project layout](https://github.com/golang-standards/project-layout)               | project layout |

## Related projects

- [MediaMTX](https://github.com/bluenviron/mediamtx)
- [gortsplib](https://github.com/bluenviron/gortsplib)
- [gohlslib](https://github.com/bluenviron/gohlslib)
- [mediacommon](https://github.com/bluenviron/mediacommon)
