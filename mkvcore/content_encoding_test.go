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
	"io"
	"reflect"
	"sync"
	"testing"

	"github.com/at-wat/ebml-go"
)

type testCompression struct {
	ContentCompAlgo     *uint64 `ebml:"ContentCompAlgo"`
	ContentCompSettings []byte  `ebml:"ContentCompSettings,omitempty"`
}

type testEncryption struct {
	ContentEncAlgo  *uint64 `ebml:"ContentEncAlgo"`
	ContentEncKeyID []byte  `ebml:"ContentEncKeyID,omitempty"`
}

type testEncoding struct {
	ContentEncodingOrder *uint64          `ebml:"ContentEncodingOrder"`
	ContentEncodingScope *uint64          `ebml:"ContentEncodingScope"`
	ContentEncodingType  *uint64          `ebml:"ContentEncodingType"`
	ContentCompression   *testCompression `ebml:"ContentCompression"`
	ContentEncryption    *testEncryption  `ebml:"ContentEncryption"`
}

type testContentEncodings struct {
	ContentEncoding []testEncoding `ebml:"ContentEncoding"`
}

type testTrackEntry struct {
	TrackNumber      uint64                `ebml:"TrackNumber"`
	TrackType        uint64                `ebml:"TrackType,omitempty"`
	CodecID          string                `ebml:"CodecID,omitempty"`
	CodecPrivate     []byte                `ebml:"CodecPrivate,omitempty"`
	ContentEncodings *testContentEncodings `ebml:"ContentEncodings"`
}

type testBlockGroup struct {
	ReferencePriority uint64     `ebml:"ReferencePriority,omitempty"`
	Block             ebml.Block `ebml:"Block"`
}

type testCluster struct {
	Timecode    uint64           `ebml:"Timecode"`
	SimpleBlock []ebml.Block     `ebml:"SimpleBlock,omitempty"`
	BlockGroup  []testBlockGroup `ebml:"BlockGroup,omitempty"`
}

type testDocument struct {
	Segment struct {
		Tracks struct {
			TrackEntry []testTrackEntry `ebml:"TrackEntry"`
		} `ebml:"Tracks"`
		Cluster []testCluster `ebml:"Cluster,size=unknown"`
	} `ebml:"Segment,size=unknown"`
}

func testU64(v uint64) *uint64 { return &v }

func testZlib(t *testing.T, b []byte) []byte {
	t.Helper()
	var buf bytes.Buffer
	w := zlib.NewWriter(&buf)
	if _, err := w.Write(b); err != nil {
		t.Fatalf("Failed to compress test data: %v", err)
	}
	if err := w.Close(); err != nil {
		t.Fatalf("Failed to close zlib writer: %v", err)
	}
	return buf.Bytes()
}

func testZlibEncoding(order uint64, scope uint64) testEncoding {
	return testEncoding{
		ContentEncodingOrder: testU64(order),
		ContentEncodingScope: testU64(scope),
		ContentCompression: &testCompression{
			ContentCompAlgo: testU64(0),
		},
	}
}

func testHeaderEncoding(order uint64, scope uint64, header string) testEncoding {
	return testEncoding{
		ContentEncodingOrder: testU64(order),
		ContentEncodingScope: testU64(scope),
		ContentCompression: &testCompression{
			ContentCompAlgo:     testU64(3),
			ContentCompSettings: []byte(header),
		},
	}
}

func testNewReader(t *testing.T, doc *testDocument, opts ...BlockReaderOption) []BlockReadCloserWithTrackEntry {
	t.Helper()
	var buf bytes.Buffer
	if err := ebml.Marshal(doc, &buf); err != nil {
		t.Fatalf("Failed to marshal test document: %v", err)
	}
	rs, err := NewSimpleBlockReader(bytes.NewReader(buf.Bytes()), opts...)
	if err != nil {
		t.Fatalf("Failed to create BlockReader: %v", err)
	}
	return rs
}

func testReadAll(t *testing.T, r BlockReader) (frames [][]byte, keyframes []bool, timestamps []int64, err error) {
	t.Helper()
	for {
		b, key, ts, e := r.Read()
		if e == io.EOF {
			return frames, keyframes, timestamps, nil
		}
		if e != nil {
			return frames, keyframes, timestamps, e
		}
		frames = append(frames, b)
		keyframes = append(keyframes, key)
		timestamps = append(timestamps, ts)
	}
}

type testReadResult struct {
	frames     [][]byte
	keyframes  []bool
	timestamps []int64
	err        error
}

// testReadAllConcurrently drains every reader in its own goroutine, as the
// per-track delivery is synchronous and a reader must be consumed concurrently
// with the others.
func testReadAllConcurrently(t *testing.T, rs []BlockReadCloserWithTrackEntry) []testReadResult {
	t.Helper()
	results := make([]testReadResult, len(rs))
	var wg sync.WaitGroup
	for i := range rs {
		i := i
		wg.Add(1)
		go func() {
			defer wg.Done()
			frames, keys, ts, err := testReadAll(t, rs[i])
			results[i] = testReadResult{
				frames:     frames,
				keyframes:  keys,
				timestamps: ts,
				err:        err,
			}
		}()
	}
	wg.Wait()
	return results
}

func testCheckFrames(
	t *testing.T,
	frames [][]byte,
	keyframes []bool,
	timestamps []int64,
	expected [][]byte,
	expectedKeyframes []bool,
	expectedTimestamps []int64,
) {
	t.Helper()
	if len(frames) != len(expected) {
		t.Fatalf("Expected %d frames, got %d: %q", len(expected), len(frames), frames)
	}
	for i := range expected {
		if !bytes.Equal(frames[i], expected[i]) {
			t.Errorf("Frame[%d]: expected %q, got %q", i, expected[i], frames[i])
		}
		if expectedKeyframes != nil && keyframes[i] != expectedKeyframes[i] {
			t.Errorf("Frame[%d]: expected keyframe %v, got %v", i, expectedKeyframes[i], keyframes[i])
		}
		if expectedTimestamps != nil && timestamps[i] != expectedTimestamps[i] {
			t.Errorf("Frame[%d]: expected timestamp %d, got %d", i, expectedTimestamps[i], timestamps[i])
		}
	}
}

func testCheckEncodingError(t *testing.T, err error, trackNumber, order uint64) {
	t.Helper()
	if err == nil {
		t.Fatalf("Expected a content decoding error, got nil")
	}
	var cde *ContentDecodingError
	if !errors.As(err, &cde) {
		t.Fatalf("Expected *ContentDecodingError, got %T: %v", err, err)
	}
	if cde.TrackNumber != trackNumber {
		t.Errorf("Expected failing track number %d, got %d", trackNumber, cde.TrackNumber)
	}
	if cde.EncodingOrder != order {
		t.Errorf("Expected failing encoding order %d, got %d", order, cde.EncodingOrder)
	}
}

func TestContentEncodingStack(t *testing.T) {
	frame0 := []byte("one")
	frame1 := []byte("two")

	// Track 1 declares its encodings out of order: the physical order is
	// order=0, order=2, order=1. Applying them from the largest order:
	//   frames:        order1 (zlib) -> order0 (prepend "HEAD")
	//   CodecPrivate:  order2 (prepend "PRIV")
	track1 := testTrackEntry{
		TrackNumber:  1,
		TrackType:    2,
		CodecID:      "A_TEST",
		CodecPrivate: []byte("rawcfg"),
		ContentEncodings: &testContentEncodings{
			ContentEncoding: []testEncoding{
				testHeaderEncoding(0, 1, "HEAD"),
				testHeaderEncoding(2, 2, "PRIV"),
				testZlibEncoding(1, 1),
			},
		},
	}
	// Track 2 mixes scopes: order0 applies to both (scope 3), order1 applies
	// to CodecPrivate only and order2 applies to frames only. Declared
	// physically as order0, order2, order1.
	track2 := testTrackEntry{
		TrackNumber:  2,
		TrackType:    2,
		CodecID:      "B_TEST",
		CodecPrivate: testZlib(t, []byte("priv")),
		ContentEncodings: &testContentEncodings{
			ContentEncoding: []testEncoding{
				testHeaderEncoding(0, 3, "H"),
				testZlibEncoding(2, 1),
				testZlibEncoding(1, 2),
			},
		},
	}
	// Track 3 has no content encoding and must stay byte-exact.
	track3 := testTrackEntry{
		TrackNumber:  3,
		TrackType:    2,
		CodecID:      "C_TEST",
		CodecPrivate: []byte("rawpriv"),
	}

	doc := testDocument{}
	doc.Segment.Tracks.TrackEntry = []testTrackEntry{track1, track2, track3}
	doc.Segment.Cluster = []testCluster{
		{
			Timecode: 50,
			SimpleBlock: []ebml.Block{
				{
					TrackNumber: 1,
					Timecode:    7,
					Keyframe:    true,
					Lacing:      ebml.LacingXiph,
					Data: [][]byte{
						testZlib(t, frame0),
						testZlib(t, frame1),
					},
				},
				{
					TrackNumber: 2,
					Timecode:    9,
					Keyframe:    false,
					Data:        [][]byte{testZlib(t, frame0)},
				},
				{
					TrackNumber: 3,
					Timecode:    11,
					Keyframe:    true,
					Data:        [][]byte{[]byte("raw")},
				},
			},
		},
		{
			Timecode: 100,
			BlockGroup: []testBlockGroup{
				{
					ReferencePriority: 1,
					Block: ebml.Block{
						TrackNumber: 1,
						Timecode:    3,
						Data:        [][]byte{testZlib(t, frame0)},
					},
				},
				{
					ReferencePriority: 0,
					Block: ebml.Block{
						TrackNumber: 2,
						Timecode:    5,
						Data:        [][]byte{testZlib(t, frame1)},
					},
				},
			},
		},
	}

	rs := testNewReader(t, &doc, WithContentDecoding(1024, 1024))
	defer func() {
		for _, r := range rs {
			_ = r.Close()
		}
	}()

	// Without the option the old pass-through behavior is preserved: the same
	// encoded document yields the raw, still-compressed bytes.
	rawRS := testNewReader(t, &doc)
	defer func() {
		for _, r := range rawRS {
			_ = r.Close()
		}
	}()
	rawResults := testReadAllConcurrently(t, rawRS)
	if len(rawResults[0].frames) != 3 ||
		!bytes.Equal(rawResults[0].frames[0], testZlib(t, frame0)) ||
		!bytes.Equal(rawResults[0].frames[1], testZlib(t, frame1)) {
		t.Errorf("Default reader must return the raw frames, got %q", rawResults[0].frames)
	}
	if got := rawRS[0].TrackEntry().CodecPrivate; !bytes.Equal(got, []byte("rawcfg")) {
		t.Errorf("Default reader must return the raw CodecPrivate, got %q", got)
	}

	if len(rs) != 3 {
		t.Fatalf("Expected 3 readers, got %d", len(rs))
	}

	trackEntries := []TrackEntry{rs[0].TrackEntry(), rs[1].TrackEntry(), rs[2].TrackEntry()}

	expectedEncodings := []ContentEncodings{
		{
			ContentEncoding: []ContentEncoding{
				{
					ContentEncodingOrder: 0,
					ContentEncodingScope: 1,
					ContentCompression: ContentCompression{
						ContentCompAlgo:     3,
						ContentCompSettings: []byte("HEAD"),
					},
				},
				{
					ContentEncodingOrder: 2,
					ContentEncodingScope: 2,
					ContentCompression: ContentCompression{
						ContentCompAlgo:     3,
						ContentCompSettings: []byte("PRIV"),
					},
				},
				{
					ContentEncodingOrder: 1,
					ContentEncodingScope: 1,
				},
			},
		},
		{
			ContentEncoding: []ContentEncoding{
				{
					ContentEncodingOrder: 0,
					ContentEncodingScope: 3,
					ContentCompression: ContentCompression{
						ContentCompAlgo:     3,
						ContentCompSettings: []byte("H"),
					},
				},
				{
					ContentEncodingOrder: 2,
					ContentEncodingScope: 1,
				},
				{
					ContentEncodingOrder: 1,
					ContentEncodingScope: 2,
				},
			},
		},
		{},
	}
	expectedPrivate := [][]byte{
		[]byte("PRIVrawcfg"),
		[]byte("Hpriv"),
		[]byte("rawpriv"),
	}
	for i := 0; i < 3; i++ {
		if !reflect.DeepEqual(expectedEncodings[i], trackEntries[i].ContentEncodings) {
			t.Errorf("Track[%d]: expected ContentEncodings %v, got %v",
				i, expectedEncodings[i], trackEntries[i].ContentEncodings)
		}
		if !bytes.Equal(expectedPrivate[i], trackEntries[i].CodecPrivate) {
			t.Errorf("Track[%d]: expected CodecPrivate %q, got %q",
				i, expectedPrivate[i], trackEntries[i].CodecPrivate)
		}
		if trackEntries[i].CodecID != [3]string{"A_TEST", "B_TEST", "C_TEST"}[i] {
			t.Errorf("Track[%d]: expected CodecID %q, got %q",
				i, [3]string{"A_TEST", "B_TEST", "C_TEST"}[i], trackEntries[i].CodecID)
		}
	}

	// Mutating metadata obtained from TrackEntry must not affect later calls
	// nor the decoded media.
	entry := rs[0].TrackEntry()
	entry.CodecPrivate[0] = 'X'
	entry.ContentEncodings.ContentEncoding[0].ContentCompression.ContentCompSettings[0] = 'Y'
	entry.ContentEncodings.ContentEncoding = append(entry.ContentEncodings.ContentEncoding[:1], ContentEncoding{})
	entry2 := rs[0].TrackEntry()
	if !bytes.Equal(entry2.CodecPrivate, expectedPrivate[0]) {
		t.Errorf("TrackEntry CodecPrivate was not independent: got %q", entry2.CodecPrivate)
	}
	if !reflect.DeepEqual(entry2.ContentEncodings, expectedEncodings[0]) {
		t.Errorf("TrackEntry ContentEncodings was not independent: got %v", entry2.ContentEncodings)
	}

	results := testReadAllConcurrently(t, rs)
	for i, res := range results {
		if res.err != nil {
			t.Fatalf("Track[%d]: Unexpected Read error: %v", i, res.err)
		}
	}

	// Track 1: two laces decoded independently, then the body of the
	// BlockGroup.Block (keyframe from ReferencePriority).
	testCheckFrames(t, results[0].frames, results[0].keyframes, results[0].timestamps,
		[][]byte{[]byte("HEADone"), []byte("HEADtwo"), []byte("HEADone")},
		[]bool{true, true, true},
		[]int64{57, 57, 103},
	)

	testCheckFrames(t, results[1].frames, results[1].keyframes, results[1].timestamps,
		[][]byte{[]byte("Hone"), []byte("Htwo")},
		[]bool{false, false},
		[]int64{59, 105},
	)

	testCheckFrames(t, results[2].frames, results[2].keyframes, results[2].timestamps,
		[][]byte{[]byte("raw")},
		[]bool{true},
		[]int64{61},
	)
}

func TestContentEncodingLaces(t *testing.T) {
	// Track 1 uses ContentCompAlgo 3 only so that fixed laces keep equal
	// lengths.
	track1 := testTrackEntry{
		TrackNumber: 1,
		TrackType:   2,
		CodecID:     "FIXED",
		ContentEncodings: &testContentEncodings{
			ContentEncoding: []testEncoding{testHeaderEncoding(0, 1, "HEAD")},
		},
	}
	// Track 2 uses zlib for the variable length lacing modes.
	track2 := testTrackEntry{
		TrackNumber: 2,
		TrackType:   2,
		CodecID:     "ZLIB",
		ContentEncodings: &testContentEncodings{
			ContentEncoding: []testEncoding{
				testHeaderEncoding(0, 1, "H"),
				testZlibEncoding(1, 1),
			},
		},
	}

	zipBlock := func(timecode int16, keyframe bool, lacing ebml.LacingMode, payloads ...string) ebml.Block {
		data := make([][]byte, len(payloads))
		for i, p := range payloads {
			data[i] = testZlib(t, []byte(p))
		}
		return ebml.Block{
			TrackNumber: 2,
			Timecode:    timecode,
			Keyframe:    keyframe,
			Lacing:      lacing,
			Data:        data,
		}
	}

	doc := testDocument{}
	doc.Segment.Tracks.TrackEntry = []testTrackEntry{track1, track2}
	doc.Segment.Cluster = []testCluster{
		{
			Timecode: 100,
			SimpleBlock: []ebml.Block{
				{
					TrackNumber: 1,
					Timecode:    1,
					Keyframe:    true,
					Lacing:      ebml.LacingFixed,
					Data:        [][]byte{[]byte("aa"), []byte("bb"), []byte("cc")},
				},
				zipBlock(2, false, ebml.LacingXiph, "a", "bb", "ccc"),
				zipBlock(3, true, ebml.LacingNo, "single"),
			},
			BlockGroup: []testBlockGroup{
				{
					ReferencePriority: 1,
					Block:             zipBlock(4, false, ebml.LacingEBML, "x", "yy", "zzz", "wwww"),
					// The keyframe flag comes from ReferencePriority.
				},
			},
		},
	}

	rs := testNewReader(t, &doc, WithContentDecoding(1024, 1024))
	defer func() {
		for _, r := range rs {
			_ = r.Close()
		}
	}()

	results := testReadAllConcurrently(t, rs)
	for i, res := range results {
		if res.err != nil {
			t.Fatalf("Track[%d]: Unexpected Read error: %v", i, res.err)
		}
	}

	testCheckFrames(t, results[0].frames, results[0].keyframes, results[0].timestamps,
		[][]byte{[]byte("HEADaa"), []byte("HEADbb"), []byte("HEADcc")},
		[]bool{true, true, true},
		[]int64{101, 101, 101},
	)

	testCheckFrames(t, results[1].frames, results[1].keyframes, results[1].timestamps,
		[][]byte{
			[]byte("Ha"), []byte("Hbb"), []byte("Hccc"),
			[]byte("Hsingle"),
			[]byte("Hx"), []byte("Hyy"), []byte("Hzzz"), []byte("Hwwww"),
		},
		[]bool{
			false, false, false,
			true,
			true, true, true, true,
		},
		[]int64{
			102, 102, 102,
			103,
			104, 104, 104, 104,
		},
	)
}

func testMarshalDoc(t *testing.T, doc *testDocument) []byte {
	t.Helper()
	var buf bytes.Buffer
	if err := ebml.Marshal(doc, &buf); err != nil {
		t.Fatalf("Failed to marshal test document: %v", err)
	}
	return buf.Bytes()
}

func testReaderError(t *testing.T, doc *testDocument, opts ...BlockReaderOption) error {
	t.Helper()
	_, err := NewSimpleBlockReader(bytes.NewReader(testMarshalDoc(t, doc)), opts...)
	return err
}

func TestContentEncodingErrors(t *testing.T) {
	t.Run("Limits", func(t *testing.T) {
		doc := &testDocument{}
		doc.Segment.Tracks.TrackEntry = []testTrackEntry{{TrackNumber: 1}}
		if err := testReaderError(t, doc, WithContentDecoding(0, 1)); !errors.Is(err, ErrContentDecodingLimits) {
			t.Errorf("Expected ErrContentDecodingLimits, got %v", err)
		}
		if err := testReaderError(t, doc, WithContentDecoding(1, 0)); !errors.Is(err, ErrContentDecodingLimits) {
			t.Errorf("Expected ErrContentDecodingLimits, got %v", err)
		}
	})

	constructionError := func(name string, entry testTrackEntry, want error) {
		t.Run(name, func(t *testing.T) {
			doc := &testDocument{}
			entry.TrackNumber = 1
			doc.Segment.Tracks.TrackEntry = []testTrackEntry{entry}
			err := testReaderError(t, doc, WithContentDecoding(1024, 1024))
			if err == nil {
				t.Fatalf("Expected an error at reader creation, got nil")
			}
			if want != nil && !errors.Is(err, want) {
				t.Errorf("Expected error %v, got %v", want, err)
			}
			var cde *ContentDecodingError
			if errors.As(err, &cde) && cde.TrackNumber != 1 {
				t.Errorf("Expected failing track number 1, got %d", cde.TrackNumber)
			}
		})
	}

	constructionError("DuplicateOrder", testTrackEntry{
		ContentEncodings: &testContentEncodings{ContentEncoding: []testEncoding{
			testHeaderEncoding(5, 1, "A"),
			testZlibEncoding(5, 1),
		}},
	}, ErrContentEncodingInvalid)

	constructionError("ScopeZero", testTrackEntry{
		ContentEncodings: &testContentEncodings{ContentEncoding: []testEncoding{{
			ContentEncodingOrder: testU64(0),
			ContentEncodingScope: testU64(0),
			ContentCompression:   &testCompression{ContentCompAlgo: testU64(3)},
		}}},
	}, ErrContentEncodingInvalid)

	constructionError("ScopeFour", testTrackEntry{
		ContentEncodings: &testContentEncodings{ContentEncoding: []testEncoding{{
			ContentEncodingOrder: testU64(0),
			ContentEncodingScope: testU64(4),
			ContentCompression:   &testCompression{ContentCompAlgo: testU64(3)},
		}}},
	}, ErrContentEncodingInvalid)

	constructionError("ScopeFive", testTrackEntry{
		ContentEncodings: &testContentEncodings{ContentEncoding: []testEncoding{{
			ContentEncodingOrder: testU64(0),
			ContentEncodingScope: testU64(5),
			ContentCompression:   &testCompression{ContentCompAlgo: testU64(3)},
		}}},
	}, ErrContentEncodingInvalid)

	constructionError("MissingCompression", testTrackEntry{
		ContentEncodings: &testContentEncodings{ContentEncoding: []testEncoding{{
			ContentEncodingOrder: testU64(0),
			ContentEncodingScope: testU64(1),
		}}},
	}, ErrContentEncodingInvalid)

	constructionError("EncryptionType", testTrackEntry{
		ContentEncodings: &testContentEncodings{ContentEncoding: []testEncoding{{
			ContentEncodingOrder: testU64(0),
			ContentEncodingScope: testU64(1),
			ContentEncodingType:  testU64(1),
			ContentCompression:   &testCompression{ContentCompAlgo: testU64(0)},
		}}},
	}, ErrContentEncodingUnsupported)

	constructionError("EncryptionElement", testTrackEntry{
		ContentEncodings: &testContentEncodings{ContentEncoding: []testEncoding{{
			ContentEncodingOrder: testU64(0),
			ContentEncodingScope: testU64(1),
			ContentCompression:   &testCompression{ContentCompAlgo: testU64(3)},
			ContentEncryption: &testEncryption{
				ContentEncAlgo:  testU64(5),
				ContentEncKeyID: []byte{1, 2, 3},
			},
		}}},
	}, ErrContentEncodingUnsupported)

	constructionError("UnknownType", testTrackEntry{
		ContentEncodings: &testContentEncodings{ContentEncoding: []testEncoding{{
			ContentEncodingOrder: testU64(0),
			ContentEncodingScope: testU64(1),
			ContentEncodingType:  testU64(2),
			ContentCompression:   &testCompression{ContentCompAlgo: testU64(0)},
		}}},
	}, ErrContentEncodingUnsupported)

	constructionError("Bzlib", testTrackEntry{
		ContentEncodings: &testContentEncodings{ContentEncoding: []testEncoding{{
			ContentEncodingOrder: testU64(0),
			ContentEncodingScope: testU64(1),
			ContentCompression:   &testCompression{ContentCompAlgo: testU64(1)},
		}}},
	}, ErrContentEncodingUnsupported)

	constructionError("Lzo", testTrackEntry{
		ContentEncodings: &testContentEncodings{ContentEncoding: []testEncoding{{
			ContentEncodingOrder: testU64(0),
			ContentEncodingScope: testU64(1),
			ContentCompression:   &testCompression{ContentCompAlgo: testU64(2)},
		}}},
	}, ErrContentEncodingUnsupported)

	constructionError("UnknownAlgo", testTrackEntry{
		ContentEncodings: &testContentEncodings{ContentEncoding: []testEncoding{{
			ContentEncodingOrder: testU64(0),
			ContentEncodingScope: testU64(1),
			ContentCompression:   &testCompression{ContentCompAlgo: testU64(4)},
		}}},
	}, ErrContentEncodingUnsupported)

	constructionError("InvalidCodecPrivate", testTrackEntry{
		CodecPrivate: testZlib(t, []byte("priv"))[:5],
		ContentEncodings: &testContentEncodings{ContentEncoding: []testEncoding{
			testZlibEncoding(0, 2),
		}},
	}, nil)

	t.Run("CodecPrivateLimitExact", func(t *testing.T) {
		doc := &testDocument{}
		doc.Segment.Tracks.TrackEntry = []testTrackEntry{{
			TrackNumber:  1,
			CodecPrivate: []byte("abcd"),
			ContentEncodings: &testContentEncodings{ContentEncoding: []testEncoding{
				testHeaderEncoding(0, 3, "H"),
			}},
		}}
		rs, err := NewSimpleBlockReader(bytes.NewReader(testMarshalDoc(t, doc)), WithContentDecoding(1024, 5))
		if err != nil {
			t.Fatalf("Expected exact limit to be accepted, got %v", err)
		}
		if got := rs[0].TrackEntry().CodecPrivate; !bytes.Equal(got, []byte("Habcd")) {
			t.Errorf("Expected CodecPrivate %q, got %q", "Habcd", got)
		}
		_ = rs[0].Close()
	})

	t.Run("CodecPrivateLimitExceeded", func(t *testing.T) {
		doc := &testDocument{}
		doc.Segment.Tracks.TrackEntry = []testTrackEntry{{
			TrackNumber:  1,
			CodecPrivate: []byte("abcde"),
			ContentEncodings: &testContentEncodings{ContentEncoding: []testEncoding{
				testHeaderEncoding(0, 3, "H"),
			}},
		}}
		err := testReaderError(t, doc, WithContentDecoding(1024, 5))
		if !errors.Is(err, ErrContentDecodingLimitExceeded) {
			t.Errorf("Expected ErrContentDecodingLimitExceeded, got %v", err)
		}
	})

	t.Run("CodecPrivateMissingStaysMissing", func(t *testing.T) {
		doc := &testDocument{}
		doc.Segment.Tracks.TrackEntry = []testTrackEntry{{
			TrackNumber: 1,
			ContentEncodings: &testContentEncodings{ContentEncoding: []testEncoding{
				testHeaderEncoding(0, 2, "H"),
			}},
		}}
		rs, err := NewSimpleBlockReader(bytes.NewReader(testMarshalDoc(t, doc)), WithContentDecoding(1024, 1024))
		if err != nil {
			t.Fatalf("Unexpected error: %v", err)
		}
		defer rs[0].Close()
		if got := rs[0].TrackEntry().CodecPrivate; got != nil {
			t.Errorf("Expected nil CodecPrivate, got %q", got)
		}
	})

	t.Run("FrameLimitExactAndOneOver", func(t *testing.T) {
		doc := &testDocument{}
		doc.Segment.Tracks.TrackEntry = []testTrackEntry{{
			TrackNumber: 1,
			ContentEncodings: &testContentEncodings{ContentEncoding: []testEncoding{
				testHeaderEncoding(0, 1, "H"),
			}},
		}}
		doc.Segment.Cluster = []testCluster{{
			Timecode: 10,
			SimpleBlock: []ebml.Block{
				// Decoded length is exactly the limit (1+3).
				{TrackNumber: 1, Timecode: 0, Keyframe: true, Data: [][]byte{[]byte("abc")}},
				// Decoded length is the limit+1 (1+4).
				{TrackNumber: 1, Timecode: 1, Keyframe: false, Data: [][]byte{[]byte("abcd")}},
			},
		}}
		rs := testNewReader(t, doc, WithContentDecoding(4, 1024))
		defer rs[0].Close()

		b, _, ts, err := rs[0].Read()
		if err != nil {
			t.Fatalf("Expected the exact-limit frame to be read, got %v", err)
		}
		if !bytes.Equal(b, []byte("Habc")) || ts != 10 {
			t.Errorf("Unexpected frame %q at %d", b, ts)
		}
		_, _, _, err = rs[0].Read()
		testCheckEncodingError(t, err, 1, 0)
		if !errors.Is(err, ErrContentDecodingLimitExceeded) {
			t.Errorf("Expected ErrContentDecodingLimitExceeded, got %v", err)
		}
		if _, _, _, err := rs[0].Read(); err != io.EOF {
			t.Errorf("Expected EOF after terminal error, got %v", err)
		}
	})

	t.Run("ZlibLimitExactAndOneOver", func(t *testing.T) {
		doc := &testDocument{}
		doc.Segment.Tracks.TrackEntry = []testTrackEntry{{
			TrackNumber: 1,
			ContentEncodings: &testContentEncodings{ContentEncoding: []testEncoding{
				testZlibEncoding(0, 1),
			}},
		}}
		doc.Segment.Cluster = []testCluster{{
			Timecode: 0,
			SimpleBlock: []ebml.Block{
				{TrackNumber: 1, Data: [][]byte{testZlib(t, []byte("abcd"))}},
				{TrackNumber: 1, Timecode: 1, Data: [][]byte{testZlib(t, []byte("abcde"))}},
			},
		}}
		rs := testNewReader(t, doc, WithContentDecoding(4, 1024))
		defer rs[0].Close()

		b, _, _, err := rs[0].Read()
		if err != nil || !bytes.Equal(b, []byte("abcd")) {
			t.Fatalf("Expected exact-limit frame, got %q, %v", b, err)
		}
		_, _, _, err = rs[0].Read()
		if !errors.Is(err, ErrContentDecodingLimitExceeded) {
			t.Errorf("Expected ErrContentDecodingLimitExceeded, got %v", err)
		}
	})

	t.Run("ZlibMalformed", func(t *testing.T) {
		valid := testZlib(t, []byte("payload"))
		cases := map[string][]byte{
			"TrailingBytes": append(append([]byte(nil), valid...), 0xAA),
			"BadChecksum": func() []byte {
				b := append([]byte(nil), valid...)
				b[len(b)-1] ^= 0xFF
				return b
			}(),
			"Truncated": valid[:len(valid)-1],
			"BadHeader": {0x00, 0x01, 0x02},
		}
		for name, compressed := range cases {
			compressed := compressed
			t.Run(name, func(t *testing.T) {
				doc := &testDocument{}
				doc.Segment.Tracks.TrackEntry = []testTrackEntry{{
					TrackNumber: 1,
					ContentEncodings: &testContentEncodings{ContentEncoding: []testEncoding{
						testZlibEncoding(3, 1),
					}},
				}}
				doc.Segment.Cluster = []testCluster{{
					SimpleBlock: []ebml.Block{
						{TrackNumber: 1, Data: [][]byte{compressed}},
					},
				}}
				rs := testNewReader(t, doc, WithContentDecoding(1024, 1024))
				defer rs[0].Close()
				_, _, _, err := rs[0].Read()
				testCheckEncodingError(t, err, 1, 3)
				if errors.Is(err, ErrContentDecodingLimitExceeded) {
					t.Errorf("Malformed zlib must not be reported as a limit error: %v", err)
				}
			})
		}
	})

	t.Run("PartialLacesAndTerminalError", func(t *testing.T) {
		track1 := testTrackEntry{
			TrackNumber: 1,
			ContentEncodings: &testContentEncodings{ContentEncoding: []testEncoding{
				testZlibEncoding(7, 1),
			}},
		}
		track2 := testTrackEntry{TrackNumber: 2, CodecID: "RAW"}

		firstOK := testZlib(t, []byte("ok"))
		secondCorrupt := testZlib(t, []byte("corrupt"))

		doc := &testDocument{}
		doc.Segment.Tracks.TrackEntry = []testTrackEntry{track1, track2}
		doc.Segment.Cluster = []testCluster{
			{
				Timecode: 10,
				SimpleBlock: []ebml.Block{
					{TrackNumber: 1, Timecode: 1, Keyframe: true, Data: [][]byte{firstOK}},
					{TrackNumber: 2, Timecode: 2, Keyframe: false, Data: [][]byte{[]byte("raw0")}},
				},
			},
			{
				Timecode: 20,
				SimpleBlock: []ebml.Block{
					{
						TrackNumber: 1,
						Timecode:    3,
						Keyframe:    true,
						Lacing:      ebml.LacingXiph,
						Data: [][]byte{
							testZlib(t, []byte("first")),
							secondCorrupt[:len(secondCorrupt)-1],
						},
					},
				},
			},
		}

		rs := testNewReader(t, doc, WithContentDecoding(1024, 1024))
		defer func() {
			for _, r := range rs {
				_ = r.Close()
			}
		}()

		type result struct {
			frames [][]byte
			err    error
		}
		results := make([]result, len(rs))
		var wg sync.WaitGroup
		for i := range rs {
			i := i
			wg.Add(1)
			go func() {
				defer wg.Done()
				frames, _, _, err := testReadAll(t, rs[i])
				results[i] = result{frames: frames, err: err}
			}()
		}
		wg.Wait()

		// The complete block before the failure is preserved.
		if len(results[0].frames) != 1 || !bytes.Equal(results[0].frames[0], []byte("ok")) {
			t.Errorf("Expected prior complete block to be delivered, got %q", results[0].frames)
		}
		if len(results[1].frames) != 1 || !bytes.Equal(results[1].frames[0], []byte("raw0")) {
			t.Errorf("Expected prior complete block on track 2, got %q", results[1].frames)
		}
		// No half lace: "first" must not be delivered on its own.
		for i, res := range results {
			for _, f := range res.frames {
				if bytes.Equal(f, []byte("first")) || bytes.Equal(f, []byte("HEADfirst")) {
					t.Errorf("Reader[%d] delivered a partial lace: %q", i, f)
				}
			}
			testCheckEncodingError(t, res.err, 1, 7)
		}
	})

	t.Run("ClosedTrackDoesNotBlock", func(t *testing.T) {
		track1 := testTrackEntry{
			TrackNumber: 1,
			ContentEncodings: &testContentEncodings{ContentEncoding: []testEncoding{
				testZlibEncoding(2, 1),
			}},
		}
		track2 := testTrackEntry{TrackNumber: 2}

		bad := testZlib(t, []byte("bad"))
		doc := &testDocument{}
		doc.Segment.Tracks.TrackEntry = []testTrackEntry{track1, track2}
		doc.Segment.Cluster = []testCluster{{
			SimpleBlock: []ebml.Block{
				// Track 2 is closed before reading; the producer must not block
				// on it and the terminal error must still be delivered.
				{TrackNumber: 2, Data: [][]byte{[]byte("x")}},
				{TrackNumber: 1, Data: [][]byte{bad[:len(bad)-1]}},
			},
		}}

		rs := testNewReader(t, doc, WithContentDecoding(1024, 1024))
		if err := rs[1].Close(); err != nil {
			t.Fatalf("Unexpected Close error: %v", err)
		}
		err := readWithTimeout(rs[0])
		testCheckEncodingError(t, err, 1, 2)
		if _, _, _, err := rs[0].Read(); err != io.EOF {
			t.Errorf("Expected EOF after terminal error, got %v", err)
		}
		_ = rs[0].Close()
	})
}
