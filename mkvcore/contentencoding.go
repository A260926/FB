// Copyright 2026 The ebml-go authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package mkvcore

import (
	"compress/zlib"
	"errors"
	"fmt"
	"io"
	"sort"
)

// Matroska ContentEncodingScope bit values.
const (
	contentScopeFrame        = 1
	contentScopeCodecPrivate = 2
)

// Supported ContentCompAlgo values.
const (
	contentCompAlgoZlib         = 0
	contentCompAlgoHeaderStrip  = 3
	contentCompAlgoBzlib        = 1
	contentCompAlgoLzo          = 2
	contentEncodingTypeCompress = 0
)

var (
	// ErrInvalidContentDecodingLimit means that WithContentDecoding was called
	// with a zero limit. Both limits must be strictly positive.
	ErrInvalidContentDecodingLimit = errors.New("content decoding limits must be positive")
	// ErrUnsupportedContentEncoding means that a declared ContentEncoding uses
	// a feature this package cannot decode (encryption, bzlib, lzo, ...).
	ErrUnsupportedContentEncoding = errors.New("unsupported content encoding")
	// ErrMalformedContentEncoding means that a ContentEncoding declaration or
	// its encoded data is malformed (missing compression, bad scope, truncated
	// or corrupt zlib stream, trailing bytes, ...).
	ErrMalformedContentEncoding = errors.New("malformed content encoding")
	// ErrDuplicateContentEncodingOrder means that two ContentEncoding elements
	// share the same ContentEncodingOrder.
	ErrDuplicateContentEncodingOrder = errors.New("duplicate content encoding order")
	// ErrContentDecodingLimit means that decoded data exceeded the configured
	// maximum frame or CodecPrivate size.
	ErrContentDecodingLimit = errors.New("content decoding size limit exceeded")
)

// ContentEncodingError is a content decoding failure tagged with the track
// number and the ContentEncodingOrder of the failing step. It is delivered as
// the terminal Read error of every still-open reader of the affected reader
// set.
type ContentEncodingError struct {
	// TrackNumber is the Matroska track number of the failing data.
	TrackNumber uint64
	// Order is the ContentEncodingOrder of the failing step.
	Order uint64
	// Target is "frame" or "codec private".
	Target string
	Err    error
}

func (e *ContentEncodingError) Error() string {
	return fmt.Sprintf(
		"mkvcore: content encoding error on track %d at order %d for %s: %v",
		e.TrackNumber, e.Order, e.Target, e.Err,
	)
}

// Unwrap returns the wrapped error.
func (e *ContentEncodingError) Unwrap() error {
	return e.Err
}

// decodingStep is one validated ContentEncoding step bound to a track.
type decodingStep struct {
	order    uint64
	frame    bool
	private  bool
	algo     uint64
	settings []byte
}

// contentDecoder holds the validated decoding chain of one track.
type contentDecoder struct {
	trackNumber     uint64
	steps           []*decodingStep // sorted by order descending
	restoredPrivate []byte
	maxFrameBytes   uint64
	maxPrivateBytes uint64
}

// uintDefault dereferences an optional Matroska uint and falls back to def.
func uintDefault(p *uint64, def uint64) uint64 {
	if p == nil {
		return def
	}
	return *p
}

// newContentDecoder validates the ContentEncodings of a track and pre-decodes
// its CodecPrivate. It returns nil without an error for tracks without any
// ContentEncoding declaration, which keep the raw read path.
func newContentDecoder(t TrackEntry, maxFrameBytes, maxPrivateBytes uint64) (*contentDecoder, error) {
	if t.ContentEncodings == nil || len(t.ContentEncodings.ContentEncoding) == 0 {
		return nil, nil
	}

	d := &contentDecoder{
		trackNumber:     t.TrackNumber,
		maxFrameBytes:   maxFrameBytes,
		maxPrivateBytes: maxPrivateBytes,
	}
	orders := make(map[uint64]struct{})

	for _, enc := range t.ContentEncodings.ContentEncoding {
		order := uintDefault(enc.Order, 0)
		if _, ok := orders[order]; ok {
			return nil, &ContentEncodingError{
				TrackNumber: t.TrackNumber,
				Order:       order,
				Target:      "declaration",
				Err:         fmt.Errorf("%w: order %d", ErrDuplicateContentEncodingOrder, order),
			}
		}
		orders[order] = struct{}{}

		scope := uintDefault(enc.Scope, 1)
		// Only 1 (frame), 2 (codec private) and 3 (both) are allowed; any
		// other bit, including scope 4, is rejected.
		if scope < contentScopeFrame || scope > contentScopeFrame|contentScopeCodecPrivate {
			return nil, &ContentEncodingError{
				TrackNumber: t.TrackNumber,
				Order:       order,
				Target:      "declaration",
				Err:         fmt.Errorf("%w: invalid scope %d", ErrMalformedContentEncoding, scope),
			}
		}

		if typ := uintDefault(enc.Type, contentEncodingTypeCompress); typ != contentEncodingTypeCompress {
			return nil, &ContentEncodingError{
				TrackNumber: t.TrackNumber,
				Order:       order,
				Target:      "declaration",
				Err:         fmt.Errorf("%w: encoding type %d", ErrUnsupportedContentEncoding, typ),
			}
		}
		if enc.Encryption != nil {
			return nil, &ContentEncodingError{
				TrackNumber: t.TrackNumber,
				Order:       order,
				Target:      "declaration",
				Err:         fmt.Errorf("%w: ContentEncryption", ErrUnsupportedContentEncoding),
			}
		}
		if enc.Compression == nil {
			return nil, &ContentEncodingError{
				TrackNumber: t.TrackNumber,
				Order:       order,
				Target:      "declaration",
				Err:         fmt.Errorf("%w: missing ContentCompression", ErrMalformedContentEncoding),
			}
		}

		algo := uintDefault(enc.Compression.Algo, contentCompAlgoZlib)
		switch algo {
		case contentCompAlgoZlib, contentCompAlgoHeaderStrip:
		case contentCompAlgoBzlib, contentCompAlgoLzo:
			return nil, &ContentEncodingError{
				TrackNumber: t.TrackNumber,
				Order:       order,
				Target:      "declaration",
				Err:         fmt.Errorf("%w: compression algo %d", ErrUnsupportedContentEncoding, algo),
			}
		default:
			return nil, &ContentEncodingError{
				TrackNumber: t.TrackNumber,
				Order:       order,
				Target:      "declaration",
				Err:         fmt.Errorf("%w: unknown compression algo %d", ErrUnsupportedContentEncoding, algo),
			}
		}

		settings := make([]byte, len(enc.Compression.Settings))
		copy(settings, enc.Compression.Settings)

		d.steps = append(d.steps, &decodingStep{
			order:    order,
			frame:    scope&contentScopeFrame != 0,
			private:  scope&contentScopeCodecPrivate != 0,
			algo:     algo,
			settings: settings,
		})
	}

	// Decoding runs from the highest order down to the lowest.
	sort.Slice(d.steps, func(i, j int) bool {
		return d.steps[i].order > d.steps[j].order
	})

	// CodecPrivate is validated eagerly at reader construction. A missing
	// CodecPrivate stays absent; no configuration is synthesized from prefixes.
	if t.CodecPrivate != nil {
		priv, err := d.apply(t.CodecPrivate, false)
		if err != nil {
			return nil, err
		}
		d.restoredPrivate = priv
	}

	return d, nil
}

// apply runs the chain on one buffer. frame selects frame-scoped steps;
// otherwise codec-private-scoped steps are used. Every step output is a new
// allocation and must fit the corresponding size limit.
func (d *contentDecoder) apply(b []byte, frame bool) ([]byte, error) {
	limit := d.maxPrivateBytes
	target := "codec private"
	if frame {
		limit = d.maxFrameBytes
		target = "frame"
	}

	cur := b
	shared := true
	for _, s := range d.steps {
		if frame && !s.frame {
			continue
		}
		if !frame && !s.private {
			continue
		}

		var out []byte
		var err error
		switch s.algo {
		case contentCompAlgoHeaderStrip:
			if uint64(len(s.settings))+uint64(len(cur)) > limit {
				return nil, &ContentEncodingError{
					TrackNumber: d.trackNumber,
					Order:       s.order,
					Target:      target,
					Err: fmt.Errorf(
						"%w: %d bytes > %d",
						ErrContentDecodingLimit,
						uint64(len(s.settings))+uint64(len(cur)), limit,
					),
				}
			}
			out = make([]byte, 0, len(s.settings)+len(cur))
			out = append(out, s.settings...)
			out = append(out, cur...)
		case contentCompAlgoZlib:
			out, err = inflateZlib(cur, limit)
			if err != nil {
				return nil, &ContentEncodingError{
					TrackNumber: d.trackNumber,
					Order:       s.order,
					Target:      target,
					Err:         err,
				}
			}
		}
		if uint64(len(out)) > limit {
			return nil, &ContentEncodingError{
				TrackNumber: d.trackNumber,
				Order:       s.order,
				Target:      target,
				Err: fmt.Errorf(
					"%w: %d bytes > %d", ErrContentDecodingLimit, len(out), limit,
				),
			}
		}
		cur = out
		shared = false
	}

	// When no step matched (e.g. only codec-private-scoped encodings on a
	// frame) the input is returned as an independent copy so callers can
	// never mutate data still referenced by the reader.
	if shared {
		cp := make([]byte, len(b))
		copy(cp, b)
		cur = cp
	}
	return cur, nil
}

func (d *contentDecoder) decodeFrame(b []byte) ([]byte, error) {
	return d.apply(b, true)
}

// oneByteReader feeds a zlib reader one byte at a time. compress/zlib may
// read past the end of its own stream when offered larger chunks, which would
// hide trailing bytes; a one-byte feeder makes the number of consumed bytes
// exact.
type oneByteReader struct {
	buf []byte
	pos int
}

func (r *oneByteReader) Read(p []byte) (int, error) {
	if len(p) == 0 {
		return 0, nil
	}
	if r.pos >= len(r.buf) {
		return 0, io.EOF
	}
	p[0] = r.buf[r.pos]
	r.pos++
	return 1, nil
}

// inflateZlib decodes one zlib stream with strict end-of-stream validation.
// Output is collected one byte at a time so that, once limit bytes have been
// produced, at most one additional byte is probed before the limit error is
// returned instead of expanding the whole stream first.
func inflateZlib(src []byte, limit uint64) ([]byte, error) {
	feeder := &oneByteReader{buf: src}
	zr, err := zlib.NewReader(feeder)
	if err != nil {
		return nil, fmt.Errorf("%w: zlib: %v", ErrMalformedContentEncoding, err)
	}

	out := make([]byte, 0, len(src))
	var one [1]byte
loop:
	for {
		n, rerr := zr.Read(one[:])
		if n > 0 {
			if uint64(len(out)) >= limit {
				return nil, fmt.Errorf(
					"%w: more than %d decoded bytes", ErrContentDecodingLimit, limit,
				)
			}
			out = append(out, one[0])
		}
		switch rerr {
		case nil:
		case io.EOF:
			break loop
		default:
			return nil, fmt.Errorf("%w: zlib: %v", ErrMalformedContentEncoding, rerr)
		}
	}
	// Close re-checks the stream checksum and end state.
	if err := zr.Close(); err != nil {
		return nil, fmt.Errorf("%w: zlib: %v", ErrMalformedContentEncoding, err)
	}
	// The stream must consume exactly the input: truncation already failed
	// above, and any unconsumed byte is trailing data after the stream end.
	if feeder.pos != len(src) {
		return nil, fmt.Errorf(
			"%w: %d trailing byte(s) after zlib stream",
			ErrMalformedContentEncoding, len(src)-feeder.pos,
		)
	}
	return out, nil
}

// cloneTrackEntry returns a deep copy of a TrackEntry so slices handed out by
// TrackEntry() cannot affect later Read calls or other TrackEntry() calls.
func cloneTrackEntry(t TrackEntry) TrackEntry {
	if t.CodecPrivate != nil {
		b := make([]byte, len(t.CodecPrivate))
		copy(b, t.CodecPrivate)
		t.CodecPrivate = b
	}
	if t.ContentEncodings != nil {
		encs := &ContentEncodings{
			ContentEncoding: make([]ContentEncoding, len(t.ContentEncodings.ContentEncoding)),
		}
		for i, enc := range t.ContentEncodings.ContentEncoding {
			if enc.Order != nil {
				v := *enc.Order
				encs.ContentEncoding[i].Order = &v
			}
			if enc.Scope != nil {
				v := *enc.Scope
				encs.ContentEncoding[i].Scope = &v
			}
			if enc.Type != nil {
				v := *enc.Type
				encs.ContentEncoding[i].Type = &v
			}
			if enc.Compression != nil {
				c := &ContentCompression{}
				if enc.Compression.Algo != nil {
					v := *enc.Compression.Algo
					c.Algo = &v
				}
				if enc.Compression.Settings != nil {
					c.Settings = append([]byte(nil), enc.Compression.Settings...)
				}
				encs.ContentEncoding[i].Compression = c
			}
			if enc.Encryption != nil {
				e := &ContentEncryption{}
				if enc.Encryption.Algo != nil {
					v := *enc.Encryption.Algo
					e.Algo = &v
				}
				encs.ContentEncoding[i].Encryption = e
			}
		}
		t.ContentEncodings = encs
	}
	return t
}
