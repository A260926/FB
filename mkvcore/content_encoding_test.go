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
	"bytes"
	"compress/zlib"
	"errors"
	"io"
	"testing"

	"github.com/at-wat/ebml-go"
	"github.com/at-wat/ebml-go/internal/buffercloser"
)

func u64p(v uint64) *uint64 { return &v }

func zipBytes(t *testing.T, b []byte) []byte {
	t.Helper()
	var o bytes.Buffer
	w := zlib.NewWriter(&o)
	if _, err := w.Write(b); err != nil {
		t.Fatalf("zlib write: %v", err)
	}
	if err := w.Close(); err != nil {
		t.Fatalf("zlib close: %v", err)
	}
	return o.Bytes()
}

// testEncoding mirrors the Matroska ContentEncoding element using the public
// mkvcore types so marshal/unmarshal go through the real ebml tags.
type testEncoding struct {
	Order       *uint64          `ebml:"ContentEncodingOrder"`
	Scope       *uint64          `ebml:"ContentEncodingScope"`
	Type        *uint64          `ebml:"ContentEncodingType"`
	Compression *testCompression `ebml:"ContentCompression,omitempty"`
	Encryption  *testEncryption  `ebml:"ContentEncryption,omitempty"`
}

type testCompression struct {
	Algo     *uint64 `ebml:"ContentCompAlgo"`
	Settings []byte  `ebml:"ContentCompSettings,omitempty"`
}

type testEncryption struct {
	Algo *uint64 `ebml:"ContentEncAlgo"`
}

type testEncodings struct {
	ContentEncoding []testEncoding `ebml:"ContentEncoding"`
}

type testTrack struct {
	TrackNumber      uint64         `ebml:"TrackNumber"`
	TrackType        uint64         `ebml:"TrackType"`
	CodecID          string         `ebml:"CodecID"`
	CodecPrivate     []byte         `ebml:"CodecPrivate,omitempty"`
	ContentEncodings *testEncodings `ebml:"ContentEncodings,omitempty"`
}

type testCluster struct {
	Timecode    uint64           `ebml:"Timecode"`
	SimpleBlock []ebml.Block     `ebml:"SimpleBlock,omitempty"`
	BlockGroup  []testBlockGroup `ebml:"BlockGroup,omitempty"`
}

type testBlockGroup struct {
	Block ebml.Block `ebml:"Block"`
}

type testDoc struct {
	Segment struct {
		Tracks struct {
			TrackEntry []testTrack `ebml:"TrackEntry"`
		} `ebml:"Tracks,stop"`
		Cluster []testCluster `ebml:"Cluster,size=unknown"`
	} `ebml:"Segment,size=unknown"`
}

func marshalDoc(t *testing.T, doc *testDoc) *bytes.Reader {
	t.Helper()
	buf := buffercloser.New()
	if err := ebml.Marshal(doc, buf); err != nil {
		t.Fatalf("marshal: %v", err)
	}
	buf.Close()
	return bytes.NewReader(buf.Bytes())
}

// Read everything a reader has until its terminal result.
func drain(t *testing.T, r BlockReader) ([][]byte, []bool, []int64, error) {
	t.Helper()
	var bs [][]byte
	var keys []bool
	var tss []int64
	for {
		b, key, ts, err := r.Read()
		if err != nil {
			return bs, keys, tss, err
		}
		bs = append(bs, b)
		keys = append(keys, key)
		tss = append(tss, ts)
	}
}

// drainAll drains every reader concurrently (the processing goroutine
// delivers blocks across tracks, so no single reader can reach EOF while the
// others are never read) and returns each reader's terminal result.
type drainResult struct {
	bs   [][]byte
	keys []bool
	tss  []int64
	err  error
}

func drainAll(t *testing.T, rs []BlockReadCloserWithTrackEntry) []drainResult {
	t.Helper()
	res := make([]drainResult, len(rs))
	done := make(chan struct{}, len(rs))
	for i, r := range rs {
		go func(i int, r BlockReader) {
			res[i].bs, res[i].keys, res[i].tss, res[i].err = drain(t, r)
			done <- struct{}{}
		}(i, r)
	}
	for range rs {
		<-done
	}
	return res
}

func TestContentEncodingStack(t *testing.T) {
	// Track 1: Order 1 zlib, Order 0 header strip "HEAD", declared out of
	// order in the container. Stored frames are zlib(one), zlib(two);
	// decoded frames must be HEADone / HEADtwo.
	strip := &testCompression{Algo: u64p(3), Settings: []byte("HEAD")}
	infl := &testCompression{Algo: u64p(0)}
	track1 := testTrack{
		TrackNumber: 1,
		TrackType:   1,
		CodecID:     "A_TEST",
		ContentEncodings: &testEncodings{ContentEncoding: []testEncoding{
			// Physical order is 1 then 0; decoding order must be 1 -> 0.
			{Order: u64p(1), Scope: u64p(1), Compression: infl},
			{Order: u64p(0), Scope: u64p(1), Compression: strip},
		}},
	}

	// Track 2: scope 3 (both). CodecPrivate is header-stripped ("cfg" without
	// prefix "PRIV"); frames are plain header-stripped.
	track2 := testTrack{
		TrackNumber:  2,
		TrackType:    1,
		CodecID:      "A_TEST2",
		CodecPrivate: []byte("cfg"),
		ContentEncodings: &testEncodings{ContentEncoding: []testEncoding{
			{Order: u64p(0), Scope: u64p(3), Compression: &testCompression{
				Algo: u64p(3), Settings: []byte("PRIV"),
			}},
		}},
	}

	// Track 3: scope 2 (CodecPrivate only). Frames must pass through byte for
	// byte while the private config gets its prefix restored.
	track3 := testTrack{
		TrackNumber:  3,
		TrackType:    1,
		CodecID:      "A_TEST3",
		CodecPrivate: []byte("secret"),
		ContentEncodings: &testEncodings{ContentEncoding: []testEncoding{
			{Order: u64p(0), Scope: u64p(2), Compression: &testCompression{
				Algo: u64p(3), Settings: []byte("P-"),
			}},
		}},
	}

	// Track 4: no encoding at all, raw read path even with the option on.
	track4 := testTrack{TrackNumber: 4, TrackType: 1, CodecID: "A_PLAIN"}

	one := zipBytes(t, []byte("one"))
	two := zipBytes(t, []byte("two"))

	doc := &testDoc{}
	doc.Segment.Tracks.TrackEntry = []testTrack{track1, track2, track3, track4}
	doc.Segment.Cluster = []testCluster{
		{
			Timecode: 10,
			SimpleBlock: []ebml.Block{
				{TrackNumber: 1, Timecode: 2, Keyframe: true, Lacing: ebml.LacingXiph,
					Data: [][]byte{one, two}},
				{TrackNumber: 2, Timecode: 3, Keyframe: true,
					Data: [][]byte{[]byte("frame2")}},
				{TrackNumber: 3, Timecode: 4,
					Data: [][]byte{[]byte("frame3")}},
				{TrackNumber: 4, Timecode: 5, Keyframe: true,
					Data: [][]byte{[]byte("frame4")}},
			},
			BlockGroup: []testBlockGroup{
				{Block: ebml.Block{TrackNumber: 1, Timecode: 6,
					Data: [][]byte{one}}},
			},
		},
	}

	rs, err := NewSimpleBlockReader(marshalDoc(t, doc), WithContentDecoding(4096, 4096))
	if err != nil {
		t.Fatalf("NewSimpleBlockReader: %v", err)
	}
	if len(rs) != 4 {
		t.Fatalf("reader count = %d, want 4", len(rs))
	}
	defer func() {
		for _, r := range rs {
			r.Close()
		}
	}()

	// All readers must be drained concurrently because blocks of different
	// tracks are interleaved on one processing goroutine.
	res := drainAll(t, rs)

	// Track 1 frames.
	if res[0].err != io.EOF {
		t.Fatalf("track1 terminal err = %v, want EOF", res[0].err)
	}
	wantFrames := [][]byte{[]byte("HEADone"), []byte("HEADtwo"), []byte("HEADone")}
	if len(res[0].bs) != len(wantFrames) {
		t.Fatalf("track1 frame count = %d, want %d", len(res[0].bs), len(wantFrames))
	}
	for i, want := range wantFrames {
		if !bytes.Equal(res[0].bs[i], want) {
			t.Errorf("track1 frame %d = %q, want %q", i, res[0].bs[i], want)
		}
	}
	if !res[0].keys[0] || !res[0].keys[1] {
		t.Errorf("track1 laced keyframe flags = %v,%v want true,true", res[0].keys[0], res[0].keys[1])
	}
	wantTS := []int64{12, 12, 16}
	for i, want := range wantTS {
		if res[0].tss[i] != want {
			t.Errorf("track1 timestamp %d = %d, want %d", i, res[0].tss[i], want)
		}
	}

	// Track 2: frames and CodecPrivate both restored.
	if res[1].err != io.EOF {
		t.Fatalf("track2 terminal err = %v, want EOF", res[1].err)
	}
	if got := string(res[1].bs[0]); got != "PRIVframe2" {
		t.Errorf("track2 frame = %q, want PRIVframe2", got)
	}
	te2 := rs[1].TrackEntry()
	if got := string(te2.CodecPrivate); got != "PRIVcfg" {
		t.Errorf("track2 CodecPrivate = %q, want PRIVcfg", got)
	}
	if te2.ContentEncodings == nil || len(te2.ContentEncodings.ContentEncoding) != 1 {
		t.Fatalf("track2 ContentEncodings not preserved: %+v", te2.ContentEncodings)
	}

	// Track 3: private-only scope; frame bytes untouched, private restored.
	if res[2].err != io.EOF {
		t.Fatalf("track3 terminal err = %v, want EOF", res[2].err)
	}
	if got := string(res[2].bs[0]); got != "frame3" {
		t.Errorf("track3 frame = %q, want frame3 (raw passthrough)", got)
	}
	te3 := rs[2].TrackEntry()
	if got := string(te3.CodecPrivate); got != "P-secret" {
		t.Errorf("track3 CodecPrivate = %q, want P-secret", got)
	}

	// Track 4: plain track, raw bytes.
	if res[3].err != io.EOF {
		t.Fatalf("track4 terminal err = %v, want EOF", res[3].err)
	}
	if got := string(res[3].bs[0]); got != "frame4" || !res[3].keys[0] {
		t.Errorf("track4 frame = %q key=%v, want frame4/true", res[3].bs[0], res[3].keys[0])
	}
	if te4 := rs[3].TrackEntry(); te4.CodecPrivate != nil || te4.ContentEncodings != nil {
		t.Errorf("plain track metadata = %+v, want empty optional fields", te4)
	}

	// Ownership: mutating a TrackEntry() slice and a frame slice must not
	// affect subsequent TrackEntry() calls or Reads.
	te2.CodecPrivate[0] = 'X'
	if got := string(rs[1].TrackEntry().CodecPrivate); got != "PRIVcfg" {
		t.Errorf("TrackEntry mutation leaked: %q", got)
	}
	res[1].bs[0][0] = 'X'
	// Frames were already consumed; the stored TrackEntry must remain
	// independent of frame buffers.
	if got := string(rs[1].TrackEntry().CodecPrivate); got != "PRIVcfg" {
		t.Errorf("frame mutation leaked into TrackEntry: %q", got)
	}
	// Two TrackEntry() calls return independent backing arrays.
	a := rs[1].TrackEntry()
	c := rs[1].TrackEntry()
	a.CodecPrivate[0] = 'Y'
	if string(c.CodecPrivate) != "PRIVcfg" {
		t.Errorf("TrackEntry calls share CodecPrivate storage")
	}
}

func TestContentEncodingLaces(t *testing.T) {
	enc := &testEncodings{ContentEncoding: []testEncoding{
		{Order: u64p(0), Scope: u64p(1), Compression: &testCompression{
			Algo: u64p(3), Settings: []byte("HEAD"),
		}},
	}}
	track := testTrack{
		TrackNumber: 1, TrackType: 1, CodecID: "A_LACE",
		ContentEncodings: enc,
	}

	cases := map[string]struct {
		lacing ebml.LacingMode
		stored [][]byte
		want   [][]byte
	}{
		"NoLacing": {
			ebml.LacingNo,
			[][]byte{[]byte("one")},
			[][]byte{[]byte("HEADone")},
		},
		"Xiph": {
			ebml.LacingXiph,
			[][]byte{[]byte("one"), []byte("three")},
			[][]byte{[]byte("HEADone"), []byte("HEADthree")},
		},
		"Fixed": {
			// Fixed lacing requires equal stored frame sizes.
			ebml.LacingFixed,
			[][]byte{[]byte("one"), []byte("two")},
			[][]byte{[]byte("HEADone"), []byte("HEADtwo")},
		},
		"EBML": {
			ebml.LacingEBML,
			[][]byte{[]byte("one"), []byte("three"), []byte("X")},
			[][]byte{[]byte("HEADone"), []byte("HEADthree"), []byte("HEADX")},
		},
	}

	for name, tc := range cases {
		tc := tc
		t.Run(name, func(t *testing.T) {
			doc := &testDoc{}
			doc.Segment.Tracks.TrackEntry = []testTrack{track}
			doc.Segment.Cluster = []testCluster{{
				Timecode:    7,
				SimpleBlock: []ebml.Block{{TrackNumber: 1, Lacing: tc.lacing, Data: tc.stored}},
			}}
			rs, err := NewSimpleBlockReader(marshalDoc(t, doc), WithContentDecoding(4096, 4096))
			if err != nil {
				t.Fatalf("NewSimpleBlockReader: %v", err)
			}
			defer rs[0].Close()

			got, _, tss, err := drain(t, rs[0])
			if err != io.EOF {
				t.Fatalf("terminal err = %v, want EOF", err)
			}
			if len(got) != len(tc.want) {
				t.Fatalf("frame count = %d, want %d", len(got), len(tc.want))
			}
			for i := range tc.want {
				if !bytes.Equal(got[i], tc.want[i]) {
					t.Errorf("frame %d = %q, want %q", i, got[i], tc.want[i])
				}
				if tss[i] != 7 {
					t.Errorf("frame %d timestamp = %d, want 7", i, tss[i])
				}
			}
		})
	}
}

func TestContentEncodingErrors(t *testing.T) {
	t.Run("OptionLimits", func(t *testing.T) {
		doc := &testDoc{}
		doc.Segment.Tracks.TrackEntry = []testTrack{
			{TrackNumber: 1, TrackType: 1, CodecID: "X"},
		}
		if _, err := NewSimpleBlockReader(marshalDoc(t, doc),
			WithContentDecoding(0, 1)); !errors.Is(err, ErrInvalidContentDecodingLimit) {
			t.Errorf("zero frame limit err = %v, want ErrInvalidContentDecodingLimit", err)
		}
		if _, err := NewSimpleBlockReader(marshalDoc(t, doc),
			WithContentDecoding(1, 0)); !errors.Is(err, ErrInvalidContentDecodingLimit) {
			t.Errorf("zero private limit err = %v, want ErrInvalidContentDecodingLimit", err)
		}
	})

	compression := func(algo uint64) *testCompression {
		return &testCompression{Algo: u64p(algo), Settings: []byte("P")}
	}
	goodEnc := func(order, scope uint64) testEncoding {
		return testEncoding{Order: u64p(order), Scope: u64p(scope), Compression: compression(3)}
	}

	declCases := map[string]testEncoding{
		"ScopeIncludes4":     {Order: u64p(0), Scope: u64p(4), Compression: compression(3)},
		"Scope5":             {Order: u64p(0), Scope: u64p(5), Compression: compression(3)},
		"Encryption":         {Order: u64p(0), Scope: u64p(1), Compression: compression(0), Encryption: &testEncryption{Algo: u64p(1)}},
		"Bzlib":              {Order: u64p(0), Scope: u64p(1), Compression: compression(1)},
		"Lzo":                {Order: u64p(0), Scope: u64p(1), Compression: compression(2)},
		"MissingCompression": {Order: u64p(0), Scope: u64p(1)},
		"EncodingType1":      {Order: u64p(0), Scope: u64p(1), Type: u64p(1), Compression: compression(0)},
	}
	for name, enc := range declCases {
		enc := enc
		t.Run("Decl/"+name, func(t *testing.T) {
			doc := &testDoc{}
			doc.Segment.Tracks.TrackEntry = []testTrack{{
				TrackNumber: 1, TrackType: 1, CodecID: "X",
				ContentEncodings: &testEncodings{ContentEncoding: []testEncoding{enc}},
			}}
			_, err := NewSimpleBlockReader(marshalDoc(t, doc), WithContentDecoding(4096, 4096))
			if err == nil {
				t.Fatalf("expected construction error, got nil")
			}
			var ce *ContentEncodingError
			if !errors.As(err, &ce) {
				t.Fatalf("err = %v, want *ContentEncodingError", err)
			}
			if ce.TrackNumber != 1 {
				t.Errorf("track number = %d, want 1", ce.TrackNumber)
			}
		})
	}

	t.Run("DuplicateOrder", func(t *testing.T) {
		doc := &testDoc{}
		doc.Segment.Tracks.TrackEntry = []testTrack{{
			TrackNumber: 1, TrackType: 1, CodecID: "X",
			ContentEncodings: &testEncodings{ContentEncoding: []testEncoding{
				goodEnc(0, 1), goodEnc(0, 3),
			}},
		}}
		_, err := NewSimpleBlockReader(marshalDoc(t, doc), WithContentDecoding(4096, 4096))
		if !errors.Is(err, ErrDuplicateContentEncodingOrder) {
			t.Fatalf("err = %v, want ErrDuplicateContentEncodingOrder", err)
		}
	})

	t.Run("DefaultsAndExplicitZero", func(t *testing.T) {
		// Absent Order/Scope/Type must default to 0/1/0; an explicit zero
		// stays distinguishable by being a populated pointer on the returned
		// declaration. An absent Algo defaults to zlib (0). Settings keeps
		// the otherwise-empty ContentCompression master through marshal.
		doc := &testDoc{}
		doc.Segment.Tracks.TrackEntry = []testTrack{{
			TrackNumber: 1, TrackType: 1, CodecID: "X",
			ContentEncodings: &testEncodings{ContentEncoding: []testEncoding{
				{Compression: &testCompression{Settings: []byte{0x01}}},
			}},
		}}
		doc.Segment.Cluster = []testCluster{{
			Timecode: 0,
			SimpleBlock: []ebml.Block{{TrackNumber: 1, Data: [][]byte{
				zipBytes(t, []byte("abc")),
			}}},
		}}
		rs, err := NewSimpleBlockReader(marshalDoc(t, doc), WithContentDecoding(4096, 4096))
		if err != nil {
			t.Fatalf("construction: %v", err)
		}
		defer rs[0].Close()
		te := rs[0].TrackEntry()
		enc := te.ContentEncodings.ContentEncoding[0]
		if enc.Order != nil {
			t.Errorf("absent Order should stay nil, got %d", *enc.Order)
		}
		if enc.Scope != nil {
			t.Errorf("absent Scope should stay nil, got %d", *enc.Scope)
		}
		if enc.Compression == nil || enc.Compression.Algo != nil {
			t.Errorf("absent Algo should stay nil, got %+v", enc.Compression)
		}
		got, _, _, err := drain(t, rs[0])
		if err != io.EOF {
			t.Fatalf("terminal err = %v, want EOF", err)
		}
		if string(got[0]) != "abc" {
			t.Errorf("default zlib decode = %q, want abc", got[0])
		}
	})

	t.Run("ExplicitZeroPointers", func(t *testing.T) {
		// Explicitly written zero Order/Scope/Type/Algo must round-trip as
		// populated pointers, distinct from the absent (nil) defaults above.
		doc := &testDoc{}
		doc.Segment.Tracks.TrackEntry = []testTrack{{
			TrackNumber: 1, TrackType: 1, CodecID: "X",
			ContentEncodings: &testEncodings{ContentEncoding: []testEncoding{
				{
					Order: u64p(0), Scope: u64p(1), Type: u64p(0),
					Compression: &testCompression{Algo: u64p(0), Settings: []byte{0x01}},
				},
			}},
		}}
		rs, err := NewSimpleBlockReader(marshalDoc(t, doc), WithContentDecoding(4096, 4096))
		if err != nil {
			t.Fatalf("construction: %v", err)
		}
		defer rs[0].Close()
		enc := rs[0].TrackEntry().ContentEncodings.ContentEncoding[0]
		if enc.Order == nil || *enc.Order != 0 {
			t.Errorf("explicit zero Order lost: %+v", enc.Order)
		}
		if enc.Scope == nil || *enc.Scope != 1 {
			t.Errorf("explicit Scope=1 lost: %+v", enc.Scope)
		}
		if enc.Type == nil || *enc.Type != 0 {
			t.Errorf("explicit zero Type lost: %+v", enc.Type)
		}
		if enc.Compression.Algo == nil || *enc.Compression.Algo != 0 {
			t.Errorf("explicit zero Algo lost: %+v", enc.Compression.Algo)
		}
	})

	t.Run("ZlibDataErrors", func(t *testing.T) {
		good := zipBytes(t, []byte("abc"))
		truncated := make([]byte, len(good)-3)
		copy(truncated, good)
		cases := map[string][]byte{
			"Truncated":     truncated,
			"BadChecksum":   append(append([]byte{}, good[:len(good)-1]...), good[len(good)-1]^0xFF),
			"TrailingBytes": append(append([]byte{}, good...), 0xAA),
			// Two zlib streams concatenated into one frame must not be
			// decoded as one stream.
			"ConcatenatedStreams": append(append([]byte{}, good...), zipBytes(t, []byte("d"))...),
			"Garbage":             {1, 2, 3, 4, 5, 6, 7, 8},
		}
		for name, bad := range cases {
			bad := bad
			t.Run(name, func(t *testing.T) {
				doc := &testDoc{}
				doc.Segment.Tracks.TrackEntry = []testTrack{{
					TrackNumber: 1, TrackType: 1, CodecID: "X",
					ContentEncodings: &testEncodings{ContentEncoding: []testEncoding{
						{Scope: u64p(1), Compression: &testCompression{Algo: u64p(0)}},
					}},
				}}
				doc.Segment.Cluster = []testCluster{{
					SimpleBlock: []ebml.Block{{TrackNumber: 1, Data: [][]byte{bad}}},
				}}
				rs, err := NewSimpleBlockReader(marshalDoc(t, doc), WithContentDecoding(4096, 4096))
				if err != nil {
					t.Fatalf("construction: %v", err)
				}
				defer rs[0].Close()
				_, _, _, err = rs[0].Read()
				if err == nil || errors.Is(err, io.EOF) {
					t.Fatalf("Read err = %v, want decoding failure", err)
				}
				var ce *ContentEncodingError
				if !errors.As(err, &ce) {
					t.Fatalf("err = %v, want *ContentEncodingError", err)
				}
				if ce.TrackNumber != 1 || ce.Order != 0 {
					t.Errorf("error loc = track %d order %d, want 1/0", ce.TrackNumber, ce.Order)
				}
				// Next read is EOF.
				if _, _, _, err := rs[0].Read(); err != io.EOF {
					t.Errorf("read after failure = %v, want EOF", err)
				}
			})
		}
	})

	t.Run("FrameLimits", func(t *testing.T) {
		// Header strip makes output len = prefix + input, so limits are exact.
		enc := &testEncodings{ContentEncoding: []testEncoding{
			{Scope: u64p(1), Compression: &testCompression{
				Algo: u64p(3), Settings: []byte("HEAD"),
			}},
		}}
		build := func() *testDoc {
			doc := &testDoc{}
			doc.Segment.Tracks.TrackEntry = []testTrack{{
				TrackNumber: 1, TrackType: 1, CodecID: "X", ContentEncodings: enc,
			}}
			doc.Segment.Cluster = []testCluster{{
				SimpleBlock: []ebml.Block{{TrackNumber: 1, Data: [][]byte{[]byte("one")}}},
			}}
			return doc
		}
		// Decoded frame "HEADone" is exactly 7 bytes.
		if rs, err := NewSimpleBlockReader(marshalDoc(t, build()),
			WithContentDecoding(7, 1)); err != nil {
			t.Fatalf("at-limit construction: %v", err)
		} else {
			got, _, _, err := drain(t, rs[0])
			rs[0].Close()
			if err != io.EOF {
				t.Fatalf("at-limit terminal err = %v, want EOF", err)
			}
			if string(got[0]) != "HEADone" {
				t.Errorf("at-limit frame = %q, want HEADone", got[0])
			}
		}
		// One byte over the limit.
		rs, err := NewSimpleBlockReader(marshalDoc(t, build()),
			WithContentDecoding(6, 1))
		if err != nil {
			t.Fatalf("over-limit construction: %v", err)
		}
		defer rs[0].Close()
		if _, _, _, err := rs[0].Read(); !errors.Is(err, ErrContentDecodingLimit) {
			t.Fatalf("over-limit Read err = %v, want ErrContentDecodingLimit", err)
		}

		// zlib bomb: limit reached without full expansion.
		zenc := &testEncodings{ContentEncoding: []testEncoding{
			{Scope: u64p(1), Compression: &testCompression{Algo: u64p(0)}},
		}}
		zdoc := &testDoc{}
		zdoc.Segment.Tracks.TrackEntry = []testTrack{{
			TrackNumber: 1, TrackType: 1, CodecID: "X", ContentEncodings: zenc,
		}}
		zdoc.Segment.Cluster = []testCluster{{
			SimpleBlock: []ebml.Block{{TrackNumber: 1, Data: [][]byte{
				zipBytes(t, bytes.Repeat([]byte("Z"), 1024)),
			}}},
		}}
		zrs, err := NewSimpleBlockReader(marshalDoc(t, zdoc), WithContentDecoding(16, 1))
		if err != nil {
			t.Fatalf("zlib construction: %v", err)
		}
		defer zrs[0].Close()
		if _, _, _, err := zrs[0].Read(); !errors.Is(err, ErrContentDecodingLimit) {
			t.Fatalf("zlib limit err = %v, want ErrContentDecodingLimit", err)
		}
	})

	t.Run("PrivateLimit", func(t *testing.T) {
		doc := &testDoc{}
		doc.Segment.Tracks.TrackEntry = []testTrack{{
			TrackNumber: 1, TrackType: 1, CodecID: "X", CodecPrivate: []byte("cfg"),
			ContentEncodings: &testEncodings{ContentEncoding: []testEncoding{
				{Scope: u64p(2), Compression: &testCompression{
					Algo: u64p(3), Settings: []byte("PRIV"),
				}},
			}},
		}}
		// Restored private "PRIVcfg" needs 7 bytes; cap at 6 -> construction err.
		if _, err := NewSimpleBlockReader(marshalDoc(t, doc),
			WithContentDecoding(4096, 6)); !errors.Is(err, ErrContentDecodingLimit) {
			t.Fatalf("private limit err = %v, want ErrContentDecodingLimit", err)
		}
	})

	t.Run("HalfLaceAndTermination", func(t *testing.T) {
		// Two tracks. A good block lands first, then a two-frame lace whose
		// second frame is corrupt zlib. The bad block must produce nothing;
		// both open readers must terminate with the located error, then EOF.
		enc := &testEncodings{ContentEncoding: []testEncoding{
			{Order: u64p(0), Scope: u64p(1), Compression: &testCompression{Algo: u64p(0)}},
		}}
		tracks := []testTrack{
			{TrackNumber: 1, TrackType: 1, CodecID: "A", ContentEncodings: enc},
			{TrackNumber: 2, TrackType: 1, CodecID: "B"},
		}
		goodFrame := zipBytes(t, []byte("good"))
		// A real stream with its tail removed: valid header, missing checksum.
		badFrame := make([]byte, len(goodFrame)-4)
		copy(badFrame, goodFrame)

		build := func() *testDoc {
			doc := &testDoc{}
			doc.Segment.Tracks.TrackEntry = tracks
			doc.Segment.Cluster = []testCluster{
				{Timecode: 0, SimpleBlock: []ebml.Block{
					{TrackNumber: 1, Timecode: 0, Keyframe: true, Data: [][]byte{goodFrame}},
					{TrackNumber: 2, Timecode: 1, Data: [][]byte{[]byte("plain")}},
				}},
				{Timecode: 10, SimpleBlock: []ebml.Block{
					{TrackNumber: 1, Timecode: 0, Lacing: ebml.LacingXiph,
						Data: [][]byte{goodFrame, badFrame}},
				}},
			}
			return doc
		}

		expectLocatedError := func(t *testing.T, r BlockReader) {
			t.Helper()
			_, _, _, err := r.Read()
			var ce *ContentEncodingError
			if !errors.As(err, &ce) {
				t.Fatalf("terminal err = %v, want *ContentEncodingError", err)
			}
			if ce.TrackNumber != 1 || ce.Order != 0 {
				t.Errorf("error loc = track %d order %d, want 1/0", ce.TrackNumber, ce.Order)
			}
			// The next read after the terminal error is EOF.
			if _, _, _, err := r.Read(); err != io.EOF {
				t.Errorf("read after terminal error = %v, want EOF", err)
			}
		}

		t.Run("BothOpenReadersTerminate", func(t *testing.T) {
			rs, err := NewSimpleBlockReader(marshalDoc(t, build()), WithContentDecoding(4096, 4096))
			if err != nil {
				t.Fatalf("construction: %v", err)
			}
			// Consume the good first-cluster frames on both tracks.
			if got, key, ts, err := rs[0].Read(); err != nil || string(got) != "good" || !key || ts != 0 {
				t.Fatalf("track1 good block = %q key=%v ts=%d err=%v", got, key, ts, err)
			}
			if got, _, _, err := rs[1].Read(); err != nil || string(got) != "plain" {
				t.Fatalf("track2 good block = %q, %v", got, err)
			}
			// The corrupted lace yields no frame; both still-open readers must
			// terminate with the same located error, then EOF.
			done := make(chan struct{}, 2)
			for _, r := range rs {
				go func(r BlockReader) {
					expectLocatedError(t, r)
					done <- struct{}{}
				}(r)
			}
			<-done
			<-done
			for _, r := range rs {
				r.Close()
			}
		})

		t.Run("ClosedReaderDoesNotBlock", func(t *testing.T) {
			rs, err := NewSimpleBlockReader(marshalDoc(t, build()), WithContentDecoding(4096, 4096))
			if err != nil {
				t.Fatalf("construction: %v", err)
			}
			// Consume the good first-cluster frames.
			if got, _, _, err := rs[0].Read(); err != nil || string(got) != "good" {
				t.Fatalf("track1 good block = %q, %v", got, err)
			}
			if got, _, _, err := rs[1].Read(); err != nil || string(got) != "plain" {
				t.Fatalf("track2 good block = %q, %v", got, err)
			}
			// Close one reader before the failure surfaces; error finalization
			// must still complete and the still-open reader gets the error.
			if err := rs[1].Close(); err != nil {
				t.Fatalf("close: %v", err)
			}
			expectLocatedError(t, rs[0])
			rs[0].Close()
		})
	})

	t.Run("MissingPrivateStaysAbsent", func(t *testing.T) {
		doc := &testDoc{}
		doc.Segment.Tracks.TrackEntry = []testTrack{{
			TrackNumber: 1, TrackType: 1, CodecID: "X",
			ContentEncodings: &testEncodings{ContentEncoding: []testEncoding{
				{Scope: u64p(2), Compression: &testCompression{
					Algo: u64p(3), Settings: []byte("PRIV"),
				}},
			}},
		}}
		rs, err := NewSimpleBlockReader(marshalDoc(t, doc), WithContentDecoding(4096, 4096))
		if err != nil {
			t.Fatalf("construction: %v", err)
		}
		defer rs[0].Close()
		if rs[0].TrackEntry().CodecPrivate != nil {
			t.Errorf("missing CodecPrivate was synthesized: %q",
				rs[0].TrackEntry().CodecPrivate)
		}
	})

	t.Run("StructuralLaceFailure", func(t *testing.T) {
		// A truncated SimpleBlock payload is an EBML/lace error, not a codec
		// chain error. With content decoding enabled it must still terminate
		// the readers with an error instead of panicking in a goroutine.
		doc := &testDoc{}
		doc.Segment.Tracks.TrackEntry = []testTrack{{
			TrackNumber: 1, TrackType: 1, CodecID: "X",
			ContentEncodings: &testEncodings{ContentEncoding: []testEncoding{
				{Scope: u64p(1), Compression: &testCompression{
					Algo: u64p(3), Settings: []byte("HEAD"),
				}},
			}},
		}}
		doc.Segment.Cluster = []testCluster{{
			Timecode: 0,
			SimpleBlock: []ebml.Block{{
				TrackNumber: 1, Lacing: ebml.LacingXiph,
				Data: [][]byte{[]byte("one"), []byte("two")},
			}},
		}}
		buf := buffercloser.New()
		if err := ebml.Marshal(doc, buf); err != nil {
			t.Fatalf("marshal: %v", err)
		}
		buf.Close()
		raw := buf.Bytes()
		// Corrupt the xiph frame-count byte inside the (still well-framed)
		// SimpleBlock: claim 6 frames while only 2 frames' data exists.
		// Prefix: A3 <size> <track=81> <tc 00 00> <flags=02> <count=01>.
		prefix := []byte{0xA3, 0x8C, 0x81, 0x00, 0x00, 0x02, 0x01}
		idx := bytes.Index(raw, prefix)
		if idx < 0 {
			t.Fatalf("no SimpleBlock prefix found in % x", raw)
		}
		countOff := idx + len(prefix) - 1
		cut := append([]byte(nil), raw...)
		cut[countOff] = 0x05

		rs, err := NewSimpleBlockReader(bytes.NewReader(cut), WithContentDecoding(4096, 4096))
		if err != nil {
			t.Fatalf("construction: %v", err)
		}
		defer rs[0].Close()
		if _, _, _, err := rs[0].Read(); err == nil || errors.Is(err, io.EOF) {
			t.Fatalf("structural read err = %v, want non-EOF error", err)
		}
		if _, _, _, err := rs[0].Read(); err != io.EOF {
			t.Errorf("read after structural error = %v, want EOF", err)
		}
	})
}
