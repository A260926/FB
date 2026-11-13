// Copyright 2024 The ebml-go authors.
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
	"bytes"
	"compress/zlib"
	"errors"
	"fmt"
	"io"
	"io/ioutil"
	"math"
	"sort"
)

// Errors returned by the content decoding of Matroska ContentEncodings.
var (
	// ErrContentDecodingLimits means that WithContentDecoding was called with
	// a non-positive maximum size.
	ErrContentDecodingLimits = errors.New("content decoding limits must be positive")

	// ErrContentEncodingUnsupported means that a ContentEncoding uses an
	// algorithm or a feature which is not supported by this package.
	ErrContentEncodingUnsupported = errors.New("unsupported content encoding")

	// ErrContentEncodingInvalid means that a ContentEncoding declaration is
	// malformed.
	ErrContentEncodingInvalid = errors.New("invalid content encoding")

	// ErrContentDecodingLimitExceeded means that the decoded data exceeded the
	// corresponding maximum size.
	ErrContentDecodingLimitExceeded = errors.New("content decoding size limit exceeded")
)

// ContentCompression represents Matroska ContentCompression element.
type ContentCompression struct {
	ContentCompAlgo     uint64
	ContentCompSettings []byte
}

// ContentEncryption represents Matroska ContentEncryption element.
//
// Encryption is preserved in TrackEntry.ContentEncodings for inspection but is
// not supported by the content decoding. Enabling WithContentDecoding on a
// track declaring encryption returns an error.
type ContentEncryption struct {
	ContentEncAlgo        uint64
	ContentEncKeyID       []byte
	ContentEncAESSettings ContentEncAESSettings
}

// ContentEncAESSettings represents Matroska ContentEncAESSettings element.
type ContentEncAESSettings struct {
	AESSettingsCipherMode uint64
}

// ContentEncoding represents Matroska ContentEncoding element.
type ContentEncoding struct {
	ContentEncodingOrder uint64
	ContentEncodingScope uint64
	ContentEncodingType  uint64
	ContentCompression   ContentCompression
	ContentEncryption    ContentEncryption
}

// ContentEncodings represents Matroska ContentEncodings element.
type ContentEncodings struct {
	ContentEncoding []ContentEncoding
}

// ContentDecodingError is returned when a frame or a CodecPrivate cannot be
// decoded. It carries the track number and the ContentEncodingOrder of the
// failing decoding step.
type ContentDecodingError struct {
	TrackNumber   uint64
	EncodingOrder uint64
	Err           error
}

func (e *ContentDecodingError) Error() string {
	return fmt.Sprintf(
		"content decoding failed on track %d at encoding order %d: %v",
		e.TrackNumber, e.EncodingOrder, e.Err,
	)
}

// Unwrap returns the underlying error.
func (e *ContentDecodingError) Unwrap() error {
	return e.Err
}

type contentDecodingOptions struct {
	maxFrameBytes   uint64
	maxPrivateBytes uint64
}

// contentEncodingStep is a validated ContentEncoding which is applicable to at
// least one decoding target.
type contentEncodingStep struct {
	order    uint64
	scope    uint64
	algo     uint64
	settings []byte
}

func (s contentEncodingStep) appliesToFrame() bool {
	return s.scope&1 != 0
}

func (s contentEncodingStep) appliesToPrivate() bool {
	return s.scope&2 != 0
}

// contentDecoder is the validated decoding chain of a single track. steps are
// sorted by descending ContentEncodingOrder, which is the order in which they
// must be undone.
type contentDecoder struct {
	trackNumber uint64
	steps       []contentEncodingStep
}

const (
	contentEncodingScopeFrame   = 1
	contentEncodingScopePrivate = 2

	contentEncodingTypeCompression = 0

	contentCompAlgoZlib   = 0
	contentCompAlgoHeader = 3
)

// parsedTrackEntry is a TrackEntry decoded from an EBML map. It keeps enough
// information to distinguish omitted elements from explicitly written zero
// values.
type parsedTrackEntry struct {
	entry           TrackEntry
	codecPrivateSet bool
	encodingsSet    bool
	encodings       []parsedContentEncoding
	parseErr        error
}

type parsedContentEncoding struct {
	order    uint64
	scope    uint64
	typeVal  uint64
	algo     uint64
	settings []byte

	compressionSet bool
	encryptionSet  bool
	encryption     ContentEncryption
}

func asUint64(v interface{}) (uint64, bool) {
	switch x := v.(type) {
	case uint64:
		return x, true
	case uint32:
		return uint64(x), true
	case uint16:
		return uint64(x), true
	case uint8:
		return uint64(x), true
	case uint:
		return uint64(x), true
	case int64:
		if x >= 0 {
			return uint64(x), true
		}
	case int32:
		if x >= 0 {
			return uint64(x), true
		}
	case int:
		if x >= 0 {
			return uint64(x), true
		}
	}
	return 0, false
}

func uintField(m map[string]interface{}, name string, def uint64) (uint64, bool, error) {
	v, ok := m[name]
	if !ok {
		return def, false, nil
	}
	u, ok := asUint64(v)
	if !ok {
		return 0, true, fmt.Errorf("%w: %s has unexpected type %T", ErrContentEncodingInvalid, name, v)
	}
	return u, true, nil
}

func binaryField(m map[string]interface{}, name string) ([]byte, bool, error) {
	v, ok := m[name]
	if !ok {
		return nil, false, nil
	}
	b, ok := v.([]byte)
	if !ok {
		return nil, true, fmt.Errorf("%w: %s has unexpected type %T", ErrContentEncodingInvalid, name, v)
	}
	// A present but empty element is kept as an empty, non-nil slice so that
	// presence can be distinguished from absence.
	return cloneBytes(b), true, nil
}

func stringField(m map[string]interface{}, name string) (string, error) {
	v, ok := m[name]
	if !ok {
		return "", nil
	}
	s, ok := v.(string)
	if !ok {
		return "", fmt.Errorf("%w: %s has unexpected type %T", ErrContentEncodingInvalid, name, v)
	}
	return s, nil
}

func childMap(m map[string]interface{}, name string) (map[string]interface{}, bool, error) {
	v, ok := m[name]
	if !ok {
		return nil, false, nil
	}
	c, ok := v.(map[string]interface{})
	if !ok {
		return nil, true, fmt.Errorf("%w: %s has unexpected type %T", ErrContentEncodingInvalid, name, v)
	}
	return c, true, nil
}

func childMapSlice(m map[string]interface{}, name string) ([]map[string]interface{}, bool, error) {
	v, ok := m[name]
	if !ok {
		return nil, false, nil
	}
	switch x := v.(type) {
	case map[string]interface{}:
		return []map[string]interface{}{x}, true, nil
	case []interface{}:
		out := make([]map[string]interface{}, 0, len(x))
		for _, e := range x {
			c, ok := e.(map[string]interface{})
			if !ok {
				return nil, true, fmt.Errorf("%w: %s has unexpected element type %T", ErrContentEncodingInvalid, name, e)
			}
			out = append(out, c)
		}
		return out, true, nil
	}
	return nil, true, fmt.Errorf("%w: %s has unexpected type %T", ErrContentEncodingInvalid, name, v)
}

// parseTrackEntryMap converts the generic map produced by ebml.Unmarshal into
// the TrackEntry representation used by the reader.
func parseTrackEntryMap(m map[string]interface{}) parsedTrackEntry {
	var p parsedTrackEntry
	setErr := func(err error) {
		if err != nil && p.parseErr == nil {
			p.parseErr = err
		}
	}

	for _, f := range []struct {
		name string
		dst  *uint64
	}{
		{"TrackNumber", &p.entry.TrackNumber},
		{"TrackUID", &p.entry.TrackUID},
		{"MinCache", &p.entry.MinCache},
		{"DefaultDuration", &p.entry.DefaultDuration},
		{"MaxBlockAdditionID", &p.entry.MaxBlockAdditionID},
		{"SeekPreRoll", &p.entry.SeekPreRoll},
	} {
		v, _, err := uintField(m, f.name, 0)
		setErr(err)
		if err == nil {
			*f.dst = v
		}
	}
	for _, f := range []struct {
		name string
		dst  *uint8
	}{
		{"TrackType", &p.entry.TrackType},
		{"FlagEnabled", &p.entry.FlagEnabled},
		{"FlagDefault", &p.entry.FlagDefault},
		{"FlagForced", &p.entry.FlagForced},
		{"FlagLacing", &p.entry.FlagLacing},
		{"CodecDecodeAll", &p.entry.CodecDecodeAll},
	} {
		v, _, err := uintField(m, f.name, 0)
		setErr(err)
		if err == nil {
			*f.dst = uint8(v)
		}
	}
	for _, f := range []struct {
		name string
		dst  *string
	}{
		{"Name", &p.entry.Name},
		{"Language", &p.entry.Language},
		{"LanguageIETF", &p.entry.LanguageIETF},
		{"CodecID", &p.entry.CodecID},
	} {
		v, err := stringField(m, f.name)
		setErr(err)
		if err == nil {
			*f.dst = v
		}
	}

	if b, ok, err := binaryField(m, "CodecPrivate"); err != nil {
		setErr(err)
	} else if ok {
		p.codecPrivateSet = true
		p.entry.CodecPrivate = b
	}

	encodingsMap, ok, err := childMap(m, "ContentEncodings")
	if err != nil {
		setErr(err)
		return p
	}
	if !ok {
		return p
	}
	p.encodingsSet = true

	encodingMaps, ok, err := childMapSlice(encodingsMap, "ContentEncoding")
	if err != nil {
		setErr(err)
		return p
	}
	if !ok {
		p.entry.ContentEncodings = ContentEncodings{}
		return p
	}

	p.encodings = make([]parsedContentEncoding, 0, len(encodingMaps))
	p.entry.ContentEncodings.ContentEncoding = make([]ContentEncoding, 0, len(encodingMaps))
	for _, em := range encodingMaps {
		pe := parsedContentEncoding{scope: 1}
		pe.order, _, err = uintField(em, "ContentEncodingOrder", 0)
		setErr(err)
		pe.scope, _, err = uintField(em, "ContentEncodingScope", 1)
		setErr(err)
		pe.typeVal, _, err = uintField(em, "ContentEncodingType", 0)
		setErr(err)

		if cm, ok, err := childMap(em, "ContentCompression"); err != nil {
			setErr(err)
		} else if ok {
			pe.compressionSet = true
			pe.algo, _, err = uintField(cm, "ContentCompAlgo", 0)
			setErr(err)
			if pe.settings, _, err = binaryField(cm, "ContentCompSettings"); err != nil {
				setErr(err)
			}
		}

		if encm, ok, err := childMap(em, "ContentEncryption"); err != nil {
			setErr(err)
		} else if ok {
			pe.encryptionSet = true
			if pe.encryption.ContentEncAlgo, _, err = uintField(encm, "ContentEncAlgo", 0); err != nil {
				setErr(err)
			}
			if pe.encryption.ContentEncKeyID, _, err = binaryField(encm, "ContentEncKeyID"); err != nil {
				setErr(err)
			}
			if aes, ok, err := childMap(encm, "ContentEncAESSettings"); err != nil {
				setErr(err)
			} else if ok {
				if pe.encryption.ContentEncAESSettings.AESSettingsCipherMode, _, err = uintField(
					aes, "AESSettingsCipherMode", 0,
				); err != nil {
					setErr(err)
				}
			}
		}

		p.encodings = append(p.encodings, pe)
		p.entry.ContentEncodings.ContentEncoding = append(
			p.entry.ContentEncodings.ContentEncoding,
			ContentEncoding{
				ContentEncodingOrder: pe.order,
				ContentEncodingScope: pe.scope,
				ContentEncodingType:  pe.typeVal,
				ContentCompression: ContentCompression{
					ContentCompAlgo:     pe.algo,
					ContentCompSettings: pe.settings,
				},
				ContentEncryption: pe.encryption,
			},
		)
	}
	return p
}

// buildDecoder validates the ContentEncodings declaration and decodes the
// CodecPrivate. Any error is returned before the reader starts reading media.
func (p *parsedTrackEntry) buildDecoder(opts *contentDecodingOptions) (*contentDecoder, error) {
	if p.parseErr != nil {
		return nil, fmt.Errorf("track %d: %w", p.entry.TrackNumber, p.parseErr)
	}
	if !p.encodingsSet || len(p.encodings) == 0 {
		// Plain track without content encoding: bytes are passed through as is.
		return nil, nil
	}

	seenOrder := make(map[uint64]struct{}, len(p.encodings))
	steps := make([]contentEncodingStep, 0, len(p.encodings))
	for _, e := range p.encodings {
		if _, duplicate := seenOrder[e.order]; duplicate {
			return nil, &ContentDecodingError{
				TrackNumber:   p.entry.TrackNumber,
				EncodingOrder: e.order,
				Err:           fmt.Errorf("%w: duplicate ContentEncodingOrder %d", ErrContentEncodingInvalid, e.order),
			}
		}
		seenOrder[e.order] = struct{}{}

		if e.encryptionSet || e.typeVal != contentEncodingTypeCompression {
			reason := fmt.Sprintf("ContentEncodingType %d", e.typeVal)
			if e.encryptionSet {
				reason = "ContentEncryption"
			}
			return nil, &ContentDecodingError{
				TrackNumber:   p.entry.TrackNumber,
				EncodingOrder: e.order,
				Err: fmt.Errorf(
					"%w: %s", ErrContentEncodingUnsupported, reason,
				),
			}
		}
		if e.scope < contentEncodingScopeFrame || e.scope > contentEncodingScopeFrame|contentEncodingScopePrivate {
			return nil, &ContentDecodingError{
				TrackNumber:   p.entry.TrackNumber,
				EncodingOrder: e.order,
				Err: fmt.Errorf(
					"%w: ContentEncodingScope %d", ErrContentEncodingInvalid, e.scope,
				),
			}
		}
		if !e.compressionSet {
			return nil, &ContentDecodingError{
				TrackNumber:   p.entry.TrackNumber,
				EncodingOrder: e.order,
				Err:           fmt.Errorf("%w: missing ContentCompression", ErrContentEncodingInvalid),
			}
		}
		switch e.algo {
		case contentCompAlgoZlib, contentCompAlgoHeader:
		default:
			return nil, &ContentDecodingError{
				TrackNumber:   p.entry.TrackNumber,
				EncodingOrder: e.order,
				Err: fmt.Errorf(
					"%w: ContentCompAlgo %d", ErrContentEncodingUnsupported, e.algo,
				),
			}
		}
		steps = append(steps, contentEncodingStep{
			order:    e.order,
			scope:    e.scope,
			algo:     e.algo,
			settings: cloneBytes(e.settings),
		})
	}

	// Undo the encodings from the outermost (largest order) to the innermost.
	sort.Slice(steps, func(i, j int) bool {
		return steps[i].order > steps[j].order
	})

	d := &contentDecoder{
		trackNumber: p.entry.TrackNumber,
		steps:       steps,
	}

	// CodecPrivate is decoded once, here, so that a malformed declaration or
	// private data fails reader creation instead of the media read loop.
	if p.codecPrivateSet {
		priv, err := d.decodePrivate(p.entry.CodecPrivate, opts.maxPrivateBytes)
		if err != nil {
			return nil, err
		}
		p.entry.CodecPrivate = priv
	}
	return d, nil
}

// decodeLaces applies the frame-scoped encoding chain to each lace
// independently. Laces are already split by ebml.UnmarshalBlock from the lace
// length tables; the chain is never applied to the whole Block or across
// lace boundaries. On error no lace is returned, so a block is never delivered
// partially.
func (d *contentDecoder) decodeLaces(laces [][]byte, limit uint64) ([][]byte, error) {
	out := make([][]byte, len(laces))
	for i, lace := range laces {
		b, err := d.decodeFrame(lace, limit)
		if err != nil {
			return nil, err
		}
		out[i] = b
	}
	return out, nil
}

func (d *contentDecoder) decodeFrame(src []byte, limit uint64) ([]byte, error) {
	out := src
	for _, s := range d.steps {
		if !s.appliesToFrame() {
			continue
		}
		next, err := s.apply(out, limit)
		if err != nil {
			return nil, &ContentDecodingError{
				TrackNumber:   d.trackNumber,
				EncodingOrder: s.order,
				Err:           err,
			}
		}
		out = next
	}
	// Always hand the caller a private copy.
	return cloneBytes(out), nil
}

func (d *contentDecoder) decodePrivate(src []byte, limit uint64) ([]byte, error) {
	out := src
	for _, s := range d.steps {
		if !s.appliesToPrivate() {
			continue
		}
		next, err := s.apply(out, limit)
		if err != nil {
			return nil, &ContentDecodingError{
				TrackNumber:   d.trackNumber,
				EncodingOrder: s.order,
				Err:           err,
			}
		}
		out = next
	}
	return cloneBytes(out), nil
}

// cloneBytes returns a private copy of b, preserving nil.
func cloneBytes(b []byte) []byte {
	if b == nil {
		return nil
	}
	out := make([]byte, len(b))
	copy(out, b)
	return out
}

func (s contentEncodingStep) apply(src []byte, limit uint64) ([]byte, error) {
	switch s.algo {
	case contentCompAlgoZlib:
		return decodeZlib(src, limit)
	case contentCompAlgoHeader:
		return prependHeader(s.settings, src, limit)
	default:
		return nil, fmt.Errorf("%w: ContentCompAlgo %d", ErrContentEncodingUnsupported, s.algo)
	}
}

func prependHeader(settings, src []byte, limit uint64) ([]byte, error) {
	headerLen := uint64(len(settings))
	if headerLen > limit {
		return nil, ErrContentDecodingLimitExceeded
	}
	if uint64(len(src)) > limit-headerLen {
		return nil, ErrContentDecodingLimitExceeded
	}
	out := make([]byte, 0, len(settings)+len(src))
	out = append(out, settings...)
	out = append(out, src...)
	return out, nil
}

func decodeZlib(src []byte, limit uint64) ([]byte, error) {
	br := bytes.NewReader(src)
	zr, err := zlib.NewReader(br)
	if err != nil {
		return nil, err
	}

	out, readErr := readAtMost(zr, limit)
	closeErr := zr.Close()
	if readErr != nil {
		return nil, readErr
	}
	if closeErr != nil {
		return nil, closeErr
	}

	// bytes.Reader implements io.ByteReader, so the zlib reader does not read
	// ahead past the end of the stream. Any remaining byte is trailing garbage.
	if br.Len() != 0 {
		return nil, fmt.Errorf(
			"%w: %d trailing bytes after zlib stream", ErrContentEncodingInvalid, br.Len(),
		)
	}
	return out, nil
}

func readAtMost(r io.Reader, limit uint64) ([]byte, error) {
	var lr io.Reader = r
	if limit < math.MaxInt64 {
		// Read at most one byte beyond the limit to detect an overrun without
		// expanding the whole stream first.
		lr = io.LimitReader(r, int64(limit)+1)
	}
	out, err := ioutil.ReadAll(lr)
	if err != nil {
		return nil, err
	}
	if uint64(len(out)) > limit {
		return nil, ErrContentDecodingLimitExceeded
	}
	return out, nil
}

func cloneTrackEntry(t TrackEntry) TrackEntry {
	out := t
	out.CodecPrivate = cloneBytes(t.CodecPrivate)
	if t.ContentEncodings.ContentEncoding != nil {
		out.ContentEncodings.ContentEncoding = make(
			[]ContentEncoding, len(t.ContentEncodings.ContentEncoding),
		)
		for i, e := range t.ContentEncodings.ContentEncoding {
			ce := e
			ce.ContentCompression.ContentCompSettings = cloneBytes(e.ContentCompression.ContentCompSettings)
			ce.ContentEncryption.ContentEncKeyID = cloneBytes(e.ContentEncryption.ContentEncKeyID)
			out.ContentEncodings.ContentEncoding[i] = ce
		}
	}
	return out
}
