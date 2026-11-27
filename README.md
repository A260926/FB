# ebml-go

[![GoDoc](https://godoc.org/github.com/at-wat/ebml-go?status.svg)](http://godoc.org/github.com/at-wat/ebml-go) [![ci](https://github.com/at-wat/ebml-go/actions/workflows/ci.yml/badge.svg)](https://github.com/at-wat/ebml-go/actions/workflows/ci.yml) [![codecov](https://codecov.io/gh/at-wat/ebml-go/branch/master/graph/badge.svg)](https://codecov.io/gh/at-wat/ebml-go) [![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)

## A pure Go implementation of bi-directional EBML encoder/decoder

EBML (Extensible Binary Meta Language) is a binary and byte-aligned format that was originally developed for the Matroska audio-visual container.
See https://matroska.org/ for details.

This package implements EBML Marshaler and Unmarshaler for Go.
Currently, commonly used elements of WebM subset is supported.


## Usage

Check out the examples placed under [./examples](./examples/) directory.

API is documented using [GoDoc](http://godoc.org/github.com/at-wat/ebml-go).
EBML can be `Marshal`-ed and `Unmarshal`-ed between tagged struct and binary stream through `io.Reader` and `io.Writer`.


### Reading tracks with Matroska ContentEncodings

`mkvcore.NewSimpleBlockReader` returns the exact container bytes by default.
When a Matroska track stores compressed frames (`ContentEncodingType = 0`),
opt in to transparent decoding with `mkvcore.WithContentDecoding`. Without the
option nothing changes; with it, header stripping (`ContentCompAlgo = 3`)
prepends `ContentCompSettings` back to each frame and zlib (`ContentCompAlgo =
0`) inflates each frame separately. Decoding steps run in descending
`ContentEncodingOrder`, independently on every laced frame.

```go
import (
    "io"
    "log"

    "github.com/at-wat/ebml-go/mkvcore"
)

// maxFrameBytes and maxPrivateBytes must both be strictly positive; every
// decoded frame and CodecPrivate is bounded by them.
readers, err := mkvcore.NewSimpleBlockReader(
    file,
    mkvcore.WithContentDecoding(16*1024*1024 /* max frame bytes */,
        1*1024*1024 /* max CodecPrivate bytes */),
)
if err != nil {
    // Invalid or unsupported ContentEncoding declarations, or a CodecPrivate
    // that fails to decode, are reported here before reading starts.
    log.Fatal(err)
}
for _, r := range readers {
    go func(r mkvcore.BlockReadCloserWithTrackEntry) {
        defer r.Close()
        // CodecPrivate is the restored codec configuration. ContentEncodings
        // still holds the original declaration; never run the returned data
        // through that chain a second time.
        _ = r.TrackEntry().CodecPrivate
        for {
            frame, keyframe, timestamp, err := r.Read()
            if err == io.EOF {
                return
            }
            if err != nil {
                // A mid-stream decoding failure terminates the reader with a
                // *mkvcore.ContentEncodingError carrying track and order.
                log.Fatal(err)
            }
            _ = frame
            _ = keyframe
            _ = timestamp
        }
    }(r)
}
```

Tracks without a `ContentEncoding` element keep the raw read path even when
the option is enabled. Encryption, bzlib, lzo, scopes other than 1/2/3 and
missing `ContentCompression` are rejected instead of being silently passed
through.


## References

- [Matroska Container Specifications](https://matroska.org/technical/specs/index.html)
- [WebM Container Guidelines](https://www.webmproject.org/docs/container/)


## License

This package is licensed under [Apache License Version 2.0](./LICENSE).
