package ebml_test

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"testing"

	ebml "github.com/at-wat/ebml-go"
)

func TestSeedBlockClean(t *testing.T) {
	raw := []byte{0x82, 0xff, 0xfe, 0x06, 0x02, 0x81, 0xc0, 0x0a, 0x0b, 0x1b, 0x0c}
	b, e := ebml.UnmarshalBlock(bytes.NewReader(raw), int64(len(raw)))
	if e != nil {
		t.Fatal(e)
	}
	if b.TrackNumber != 2 || b.Timecode != -2 || len(b.Data) != 3 || !bytes.Equal(b.Data[1], []byte{0x0b, 0x1b}) {
		t.Fatalf("bad decoded block: %+v", b)
	}
	t.Log("EBML lace: track=2 timecode=-2 frames=0a/0b1b/0c")
}

func TestSeedBlockBoundary(t *testing.T) {
	r := bytes.NewReader([]byte{0x81, 0xaa, 0xbb, 0xcc, 0xdd})
	b, e := ebml.UnmarshalBlock(r, 1)
	used := 5 - r.Len()
	if !errors.Is(e, io.ErrUnexpectedEOF) || b != nil || used > 1 {
		t.Fatalf("n=1: used=%d block=%v error=%v; want UnexpectedEOF, nil block and at most 1 byte consumed", used, b, e)
	}
}

// blockBudgetSentinel is appended after every valid payload to prove that
// parsing never touches bytes belonging to the next element.
var blockBudgetSentinel = []byte{0xde, 0xad, 0xbe, 0xef}

// oneByteReader delivers at most one byte per Read. Parsing results must be
// identical regardless of how the underlying reader chunks the data.
type oneByteReader struct {
	b   []byte
	pos int
}

func (r *oneByteReader) Read(p []byte) (int, error) {
	if r.pos >= len(r.b) {
		return 0, io.EOF
	}
	if len(p) == 0 {
		return 0, nil
	}
	p[0] = r.b[r.pos]
	r.pos++
	return 1, nil
}

func (r *oneByteReader) consumed() int { return r.pos }

// unread returns the bytes of payload that a bytes.Reader has not consumed yet.
func unread(r *bytes.Reader, payload []byte) []byte {
	return payload[len(payload)-r.Len():]
}

// blockBudgetFrames is the common expectation of the lacing fixtures:
// three frames 0a / 0b1b / 0c.
var blockBudgetFrames = [][]byte{{0x0a}, {0x0b, 0x1b}, {0x0c}}

func TestBlockBudgetNegativeLength(t *testing.T) {
	payload := append([]byte{0x81, 0x00, 0x00, 0x00}, blockBudgetSentinel...)
	r := bytes.NewReader(payload)
	b, err := ebml.UnmarshalBlock(r, -1)
	if err == nil {
		t.Fatal("UnmarshalBlock with negative length must fail")
	}
	if !errors.Is(err, ebml.ErrInvalidElementSize) {
		t.Fatalf("negative block length: want ErrInvalidElementSize, got %v", err)
	}
	if b != nil {
		t.Fatalf("negative block length: want nil Block, got %+v", b)
	}
	if used := len(payload) - r.Len(); used != 0 {
		t.Fatalf("negative block length consumed %d bytes", used)
	}

	ctors := map[string]func(io.Reader, int64) (ebml.Unlacer, error){
		"no":    ebml.NewNoUnlacer,
		"xiph":  ebml.NewXiphUnlacer,
		"fixed": ebml.NewFixedUnlacer,
		"ebml":  ebml.NewEBMLUnlacer,
	}
	for name, ctor := range ctors {
		t.Run(name, func(t *testing.T) {
			payload := append([]byte{0x02, 0x01, 0x02}, blockBudgetSentinel...)
			r := bytes.NewReader(payload)
			ul, err := ctor(r, -1)
			if err == nil {
				t.Fatal("negative lacing budget must fail")
			}
			if !errors.Is(err, ebml.ErrInvalidElementSize) {
				t.Fatalf("negative budget: want ErrInvalidElementSize, got %v", err)
			}
			if ul != nil {
				t.Fatalf("negative budget: want nil Unlacer, got %v", ul)
			}
			if used := len(payload) - r.Len(); used != 0 {
				t.Fatalf("negative budget consumed %d bytes", used)
			}
		})
	}
}

func TestBlockBudgetHeaderTruncation(t *testing.T) {
	// The 4-byte Block header is track VINT (1), timecode (2) and flags (1).
	// Every declared size shorter than 4 must fail with ErrUnexpectedEOF
	// without reading past the declared boundary.
	raw := []byte{0x81, 0xaa, 0xbb, 0xcc, 0xdd, 0xee}
	for _, n := range []int64{1, 2, 3} {
		t.Run(fmt.Sprintf("declared=%d", n), func(t *testing.T) {
			payload := append(append([]byte{}, raw...), blockBudgetSentinel...)
			r := bytes.NewReader(payload)
			b, err := ebml.UnmarshalBlock(r, n)
			if !errors.Is(err, io.ErrUnexpectedEOF) {
				t.Fatalf("n=%d: want ErrUnexpectedEOF, got %v", n, err)
			}
			if b != nil {
				t.Fatalf("n=%d: want nil Block, got %+v", n, b)
			}
			used := len(payload) - r.Len()
			if int64(used) > n {
				t.Fatalf("n=%d: consumed %d bytes, exceeds declared size", n, used)
			}
			wantUnread := append(append([]byte{}, raw[n:]...), blockBudgetSentinel...)
			if !bytes.Equal(unread(r, payload), wantUnread) {
				t.Fatalf("n=%d: unread suffix mismatch: %x", n, unread(r, payload))
			}
		})
	}

	// Declared size is large enough but the underlying reader ends early.
	t.Run("short reader", func(t *testing.T) {
		r := bytes.NewReader(raw[:3])
		b, err := ebml.UnmarshalBlock(r, 4)
		if !errors.Is(err, io.ErrUnexpectedEOF) {
			t.Fatalf("want ErrUnexpectedEOF, got %v", err)
		}
		if b != nil {
			t.Fatalf("want nil Block, got %+v", b)
		}
		if r.Len() != 0 {
			t.Fatalf("want all available bytes consumed, remaining=%d", r.Len())
		}
	})

	// A multi-byte track VINT that the budget cannot finish.
	t.Run("long track vint", func(t *testing.T) {
		payload := append([]byte{0x42, 0x13, 0x01}, blockBudgetSentinel...)
		r := bytes.NewReader(payload)
		b, err := ebml.UnmarshalBlock(r, 1)
		if !errors.Is(err, io.ErrUnexpectedEOF) {
			t.Fatalf("want ErrUnexpectedEOF, got %v", err)
		}
		if b != nil {
			t.Fatalf("want nil Block, got %+v", b)
		}
		if used := len(payload) - r.Len(); used != 1 {
			t.Fatalf("consumed %d bytes with n=1", used)
		}
		if !bytes.Equal(unread(r, payload), append([]byte{0x13, 0x01}, blockBudgetSentinel...)) {
			t.Fatalf("sentinel corrupted: %x", unread(r, payload))
		}
	})
}

func TestBlockBudgetValidBlocks(t *testing.T) {
	// Each fixture parses into the listed frames; no-lacing stores one frame.
	cases := map[string]struct {
		raw      []byte
		frames   [][]byte
		track    uint64
		timecode int16
	}{
		"NoLacing": {
			raw:      []byte{0x81, 0x00, 0x00, 0x00, 0x0a, 0x0b, 0x1b, 0x0c},
			frames:   [][]byte{{0x0a, 0x0b, 0x1b, 0x0c}},
			track:    1,
			timecode: 0,
		},
		"NoLacingEmptyFrame": {
			raw:      []byte{0x81, 0x00, 0x00, 0x00},
			frames:   [][]byte{{}},
			track:    1,
			timecode: 0,
		},
		"FixedLacing": {
			// Fixed lacing requires equal-sized frames: three 1-byte frames.
			raw:      []byte{0x81, 0x00, 0x00, 0x04, 0x02, 0x0a, 0x0b, 0x0c},
			frames:   [][]byte{{0x0a}, {0x0b}, {0x0c}},
			track:    1,
			timecode: 0,
		},
		"XiphLacing": {
			raw:      []byte{0x81, 0x00, 0x00, 0x02, 0x02, 0x01, 0x02, 0x0a, 0x0b, 0x1b, 0x0c},
			frames:   blockBudgetFrames,
			track:    1,
			timecode: 0,
		},
		"EBMLLacing": {
			raw:      []byte{0x82, 0xff, 0xfe, 0x06, 0x02, 0x81, 0xc0, 0x0a, 0x0b, 0x1b, 0x0c},
			frames:   blockBudgetFrames,
			track:    2,
			timecode: -2,
		},
	}
	for name, c := range cases {
		payload := append(append([]byte{}, c.raw...), blockBudgetSentinel...)
		for _, slow := range []bool{false, true} {
			readerName := "bytes.Reader"
			var r io.Reader
			var consumed func() int
			if slow {
				readerName = "oneByteReader"
				sr := &oneByteReader{b: payload}
				r, consumed = sr, sr.consumed
			} else {
				br := bytes.NewReader(payload)
				r, consumed = br, func() int { return len(payload) - br.Len() }
			}

			t.Run(name+"/"+readerName, func(t *testing.T) {
				b, err := ebml.UnmarshalBlock(r, int64(len(c.raw)))
				if err != nil {
					t.Fatalf("UnmarshalBlock failed: %v", err)
				}
				if b.TrackNumber != c.track || b.Timecode != c.timecode {
					t.Fatalf("header mismatch: track=%d timecode=%d, want %d/%d",
						b.TrackNumber, b.Timecode, c.track, c.timecode)
				}
				if len(b.Data) != len(c.frames) {
					t.Fatalf("frame count=%d, want %d", len(b.Data), len(c.frames))
				}
				for i, f := range c.frames {
					if !bytes.Equal(b.Data[i], f) {
						t.Fatalf("frame %d=%x, want %x", i, b.Data[i], f)
					}
				}
				if used := consumed(); used != len(c.raw) {
					t.Fatalf("consumed %d bytes, want exactly %d", used, len(c.raw))
				}
				if tail := payload[consumed():]; !bytes.Equal(tail, blockBudgetSentinel) {
					t.Fatalf("sentinel corrupted: %x", tail)
				}
			})
		}
	}
}

func TestBlockBudgetPartialFramesNotReturned(t *testing.T) {
	// A valid Xiph length table declaring three 1-byte frames, but the
	// underlying reader ends after the first frame byte. The constructor
	// succeeds; the failing frame Read must surface ErrUnexpectedEOF and the
	// Block must be nil rather than holding the already decoded first frame.
	full := []byte{0x81, 0x00, 0x00, 0x02, 0x02, 0x01, 0x01, 0x0a, 0x0b, 0x0c}
	available := []byte{0x81, 0x00, 0x00, 0x02, 0x02, 0x01, 0x01, 0x0a}
	r := bytes.NewReader(available)
	b, err := ebml.UnmarshalBlock(r, int64(len(full)))
	if !errors.Is(err, io.ErrUnexpectedEOF) {
		t.Fatalf("want ErrUnexpectedEOF, got %v", err)
	}
	if b != nil {
		t.Fatalf("want nil Block on truncated frame data, got %+v", b)
	}
	if used := len(available) - r.Len(); used > len(full) {
		t.Fatalf("consumed %d bytes, declared size %d", used, len(full))
	}
	if r.Len() != 0 {
		t.Fatalf("want available bytes exhausted, remaining=%d", r.Len())
	}
}

func TestBlockBudgetXiphLengthRuns(t *testing.T) {
	// A 256-byte frame is encoded as 0xFF 0x01 in the Xiph length table.
	// Three frames: 256 bytes, an empty frame and 1 byte.
	area := []byte{0x02, 0xFF, 0x01, 0x00}
	frame1 := bytes.Repeat([]byte{0xAA}, 256)
	area = append(area, frame1...)
	area = append(area, 0xCC)
	payload := append(append([]byte{}, area...), blockBudgetSentinel...)

	for _, slow := range []bool{false, true} {
		name := "bytes.Reader"
		var r io.Reader
		var consumed func() int
		if slow {
			name = "oneByteReader"
			sr := &oneByteReader{b: payload}
			r, consumed = sr, sr.consumed
		} else {
			br := bytes.NewReader(payload)
			r, consumed = br, func() int { return len(payload) - br.Len() }
		}
		t.Run(name, func(t *testing.T) {
			ul, err := ebml.NewXiphUnlacer(r, int64(len(area)))
			if err != nil {
				t.Fatalf("NewXiphUnlacer failed: %v", err)
			}
			got, err := ul.Read()
			if err != nil || !bytes.Equal(got, frame1) {
				t.Fatalf("frame 1: len=%d err=%v", len(got), err)
			}
			got, err = ul.Read()
			if err != nil || len(got) != 0 {
				t.Fatalf("frame 2 (empty): %x err=%v", got, err)
			}
			got, err = ul.Read()
			if err != nil || !bytes.Equal(got, []byte{0xCC}) {
				t.Fatalf("frame 3: %x err=%v", got, err)
			}
			if _, err := ul.Read(); err != io.EOF {
				t.Fatalf("end of lace: want io.EOF, got %v", err)
			}
			if used := consumed(); used != len(area) {
				t.Fatalf("consumed %d, want %d", used, len(area))
			}
			if !bytes.Equal(payload[consumed():], blockBudgetSentinel) {
				t.Fatalf("sentinel corrupted: %x", payload[consumed():])
			}
		})
	}

	// Length table fully inside the budget but declaring more than it can hold.
	t.Run("table exceeds budget", func(t *testing.T) {
		area := []byte{0x02, 0x05, 0x00, 0xAA, 0xBB} // 2 length entries, only 2 frame bytes
		payload := append(append([]byte{}, area...), blockBudgetSentinel...)
		r := bytes.NewReader(payload)
		ul, err := ebml.NewXiphUnlacer(r, int64(len(area)))
		if !errors.Is(err, ebml.ErrInvalidLaceSize) {
			t.Fatalf("want ErrInvalidLaceSize, got %v", err)
		}
		if ul != nil {
			t.Fatalf("want nil Unlacer, got %v", ul)
		}
		// Only the count byte and the two length entries may be consumed.
		wantUnread := append([]byte{0xAA, 0xBB}, blockBudgetSentinel...)
		if !bytes.Equal(unread(r, payload), wantUnread) {
			t.Fatalf("frame data consumed during table validation: %x", unread(r, payload))
		}
	})

	// Table cannot be finished because the underlying reader ends first.
	t.Run("truncated table", func(t *testing.T) {
		r := bytes.NewReader([]byte{0x02})
		_, err := ebml.NewXiphUnlacer(r, 5)
		if !errors.Is(err, io.ErrUnexpectedEOF) {
			t.Fatalf("want ErrUnexpectedEOF, got %v", err)
		}
		if used := 1 - r.Len(); used > 5 {
			t.Fatalf("consumed %d with budget 5", used)
		}
	})

	// A run of 0xFF length bytes cut off by the budget: the length entry never
	// terminates, so the next byte (the sentinel) must not be borrowed.
	t.Run("ff run truncated", func(t *testing.T) {
		// count=1 -> 2 frames; one length entry stuck in 0xFF continuation.
		payload := []byte{0x01, 0xFF, 0xFF, 0xFF}
		payload = append(payload, blockBudgetSentinel...)
		r := bytes.NewReader(payload)
		_, err := ebml.NewXiphUnlacer(r, 4)
		if !errors.Is(err, io.ErrUnexpectedEOF) {
			t.Fatalf("want ErrUnexpectedEOF, got %v", err)
		}
		if used := len(payload) - r.Len(); used > 4 {
			t.Fatalf("consumed %d, budget 4", used)
		}
		if !bytes.Equal(unread(r, payload), blockBudgetSentinel) {
			t.Fatalf("sentinel consumed: %x", unread(r, payload))
		}
	})

	// A complete table but the reader ends in the middle of frame data. The
	// declared area is larger than the bytes actually available.
	t.Run("frame data short", func(t *testing.T) {
		// count -> 2 frames; length entry says the first frame is 3 bytes.
		// Declared area of 6 bytes would leave a 1-byte last frame, but only
		// the three first-frame bytes are actually present.
		available := []byte{0x01, 0x03, 0xAA, 0xBB, 0xCC}
		r := bytes.NewReader(available)
		ul, err := ebml.NewXiphUnlacer(r, 6)
		if err != nil {
			t.Fatalf("constructor failed: %v", err)
		}
		if got, err := ul.Read(); err != nil || !bytes.Equal(got, []byte{0xAA, 0xBB, 0xCC}) {
			t.Fatalf("first frame: %x err=%v", got, err)
		}
		if _, err := ul.Read(); !errors.Is(err, io.ErrUnexpectedEOF) {
			t.Fatalf("want ErrUnexpectedEOF reading last frame, got %v", err)
		}
	})

	// Table complete but the computed last frame has zero size.
	t.Run("zero last frame", func(t *testing.T) {
		area := []byte{0x02, 0x02, 0x01, 0xAA, 0xBB, 0xCC}
		r := bytes.NewReader(append(append([]byte{}, area...), blockBudgetSentinel...))
		_, err := ebml.NewXiphUnlacer(r, int64(len(area)))
		if !errors.Is(err, ebml.ErrInvalidLaceSize) {
			t.Fatalf("want ErrInvalidLaceSize, got %v", err)
		}
	})
}

func TestBlockBudgetFixedLacing(t *testing.T) {
	// Valid: count byte + three equal frames.
	area := []byte{0x02, 0x0a, 0x0b, 0x0c}
	payload := append(append([]byte{}, area...), blockBudgetSentinel...)
	t.Run("valid", func(t *testing.T) {
		for _, slow := range []bool{false, true} {
			var r io.Reader
			var consumed func() int
			if slow {
				sr := &oneByteReader{b: payload}
				r, consumed = sr, sr.consumed
			} else {
				br := bytes.NewReader(payload)
				r, consumed = br, func() int { return len(payload) - br.Len() }
			}
			ul, err := ebml.NewFixedUnlacer(r, int64(len(area)))
			if err != nil {
				t.Fatalf("NewFixedUnlacer failed: %v", err)
			}
			for i, want := range [][]byte{{0x0a}, {0x0b}, {0x0c}} {
				got, err := ul.Read()
				if err != nil || !bytes.Equal(got, want) {
					t.Fatalf("frame %d: %x err=%v", i, got, err)
				}
			}
			if _, err := ul.Read(); err != io.EOF {
				t.Fatalf("want io.EOF, got %v", err)
			}
			if used := consumed(); used != len(area) || !bytes.Equal(payload[used:], blockBudgetSentinel) {
				t.Fatalf("budget violation: used=%d tail=%x", used, payload[consumed():])
			}
		}
	})

	// Undivisible data size: the count byte fits, the math does not.
	t.Run("undivisible", func(t *testing.T) {
		area := []byte{0x02, 0xAA, 0xAA, 0xAA, 0xAA} // 4 frame bytes / 3 frames
		payload := append(append([]byte{}, area...), blockBudgetSentinel...)
		r := bytes.NewReader(payload)
		ul, err := ebml.NewFixedUnlacer(r, int64(len(area)))
		if !errors.Is(err, ebml.ErrFixedLaceUndivisible) {
			t.Fatalf("want ErrFixedLaceUndivisible, got %v", err)
		}
		if ul != nil {
			t.Fatalf("want nil Unlacer, got %v", ul)
		}
		// Only the count byte is consumed; frame data and the sentinel stay.
		wantUnread := append([]byte{0xAA, 0xAA, 0xAA, 0xAA}, blockBudgetSentinel...)
		if !bytes.Equal(unread(r, payload), wantUnread) {
			t.Fatalf("bytes after the count byte were consumed: %x", unread(r, payload))
		}
	})

	// Budget too small for even the frame count byte.
	t.Run("truncated count", func(t *testing.T) {
		payload := append([]byte{}, blockBudgetSentinel...)
		r := bytes.NewReader(payload)
		_, err := ebml.NewFixedUnlacer(r, 0)
		if !errors.Is(err, io.ErrUnexpectedEOF) {
			t.Fatalf("want ErrUnexpectedEOF, got %v", err)
		}
		if !bytes.Equal(unread(r, payload), blockBudgetSentinel) {
			t.Fatal("zero-budget unlacer consumed bytes")
		}
	})
}

func TestBlockBudgetEBMLLacing(t *testing.T) {
	// Valid signed-difference table, fed one byte at a time, with a sentinel.
	area := []byte{0x02, 0x81, 0xc0, 0x0a, 0x0b, 0x1b, 0x0c}
	payload := append(append([]byte{}, area...), blockBudgetSentinel...)
	sr := &oneByteReader{b: payload}
	ul, err := ebml.NewEBMLUnlacer(sr, int64(len(area)))
	if err != nil {
		t.Fatalf("NewEBMLUnlacer failed: %v", err)
	}
	for i, want := range blockBudgetFrames {
		got, err := ul.Read()
		if err != nil || !bytes.Equal(got, want) {
			t.Fatalf("frame %d: %x err=%v", i, got, err)
		}
	}
	if _, err := ul.Read(); err != io.EOF {
		t.Fatalf("want io.EOF, got %v", err)
	}
	if sr.consumed() != len(area) || !bytes.Equal(payload[sr.consumed():], blockBudgetSentinel) {
		t.Fatalf("budget violation: used=%d tail=%x", sr.consumed(), payload[sr.consumed():])
	}

	// Table truncated: the budget ends in the middle of the first size VINT.
	t.Run("truncated table", func(t *testing.T) {
		area := []byte{0x02, 0x42} // 0x42 starts a 2-byte VINT that never ends
		payload := append(append([]byte{}, area...), blockBudgetSentinel...)
		r := bytes.NewReader(payload)
		_, err := ebml.NewEBMLUnlacer(r, int64(len(area)))
		if !errors.Is(err, io.ErrUnexpectedEOF) {
			t.Fatalf("want ErrUnexpectedEOF, got %v", err)
		}
		if used := len(payload) - r.Len(); used != len(area) {
			t.Fatalf("consumed %d, budget %d", used, len(area))
		}
		if !bytes.Equal(unread(r, payload), blockBudgetSentinel) {
			t.Fatalf("sentinel corrupted: %x", unread(r, payload))
		}
	})

	// Table fully read but the signed difference makes a frame size negative.
	t.Run("negative frame size", func(t *testing.T) {
		// count=3 frames, first size 1, difference -63 -> -62. The remaining
		// bytes are frame data and must stay untouched.
		area := []byte{0x02, 0x81, 0x80, 0xAA, 0xBB, 0xCC}
		payload := append(append([]byte{}, area...), blockBudgetSentinel...)
		r := bytes.NewReader(payload)
		ul, err := ebml.NewEBMLUnlacer(r, int64(len(area)))
		if !errors.Is(err, ebml.ErrInvalidLaceSize) {
			t.Fatalf("want ErrInvalidLaceSize, got %v", err)
		}
		if ul != nil {
			t.Fatalf("want nil Unlacer, got %v", ul)
		}
		wantUnread := append([]byte{0xAA, 0xBB, 0xCC}, blockBudgetSentinel...)
		if !bytes.Equal(unread(r, payload), wantUnread) {
			t.Fatalf("frame data consumed during table validation: %x", unread(r, payload))
		}
	})

	// Huge first frame size with a complete table: it must be rejected by the
	// budget math instead of allocating the declared size.
	t.Run("huge declared size", func(t *testing.T) {
		area := []byte{
			0x02,                                           // 3 frames
			0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // 8-byte VINT: 2^56-1
			0x80, // 1-byte signed difference: -63
		}
		payload := append(append([]byte{}, area...), blockBudgetSentinel...)
		r := bytes.NewReader(payload)
		ul, err := ebml.NewEBMLUnlacer(r, int64(len(area)))
		if !errors.Is(err, ebml.ErrInvalidLaceSize) {
			t.Fatalf("want ErrInvalidLaceSize, got %v", err)
		}
		if ul != nil {
			t.Fatalf("want nil Unlacer, got %v", ul)
		}
		if used := len(payload) - r.Len(); used != len(area) {
			t.Fatalf("consumed %d, want exactly %d", used, len(area))
		}
		if !bytes.Equal(unread(r, payload), blockBudgetSentinel) {
			t.Fatalf("sentinel corrupted: %x", unread(r, payload))
		}
	})

	// A complete, valid table but the reader ends in the middle of frame data.
	// The declared area is larger than the bytes actually available.
	t.Run("frame data short", func(t *testing.T) {
		// 3 frames: sizes 1, 2 (diff +1) and a 1-byte remainder per the
		// declared area of 8, but only the first frame byte is present.
		available := []byte{0x02, 0x81, 0xC0, 0x0a}
		r := bytes.NewReader(available)
		ul, err := ebml.NewEBMLUnlacer(r, 8)
		if err != nil {
			t.Fatalf("constructor failed: %v", err)
		}
		if got, err := ul.Read(); err != nil || !bytes.Equal(got, []byte{0x0a}) {
			t.Fatalf("first frame: %x err=%v", got, err)
		}
		if _, err := ul.Read(); !errors.Is(err, io.ErrUnexpectedEOF) {
			t.Fatalf("want ErrUnexpectedEOF reading second frame, got %v", err)
		}
	})
}

func TestBlockBudgetNoUnlacer(t *testing.T) {
	frame := []byte{0x01, 0x02, 0x03, 0x04}
	payload := append(append([]byte{}, frame...), blockBudgetSentinel...)

	t.Run("one frame per Read", func(t *testing.T) {
		r := bytes.NewReader(payload)
		ul, err := ebml.NewNoUnlacer(r, int64(len(frame)))
		if err != nil {
			t.Fatal(err)
		}
		got, err := ul.Read()
		if err != nil || !bytes.Equal(got, frame) {
			t.Fatalf("frame: %x err=%v", got, err)
		}
		if _, err := ul.Read(); err != io.EOF {
			t.Fatalf("want io.EOF, got %v", err)
		}
		if !bytes.Equal(unread(r, payload), blockBudgetSentinel) {
			t.Fatal("sentinel corrupted")
		}
	})

	t.Run("empty frame", func(t *testing.T) {
		payload := append([]byte{}, blockBudgetSentinel...)
		r := bytes.NewReader(payload)
		ul, err := ebml.NewNoUnlacer(r, 0)
		if err != nil {
			t.Fatal(err)
		}
		got, err := ul.Read()
		if err != nil || len(got) != 0 {
			t.Fatalf("empty frame: %x err=%v", got, err)
		}
		if _, err := ul.Read(); err != io.EOF {
			t.Fatalf("want io.EOF, got %v", err)
		}
		if !bytes.Equal(unread(r, payload), blockBudgetSentinel) {
			t.Fatal("zero-budget unlacer consumed bytes")
		}
	})

	t.Run("truncated", func(t *testing.T) {
		r := bytes.NewReader(frame[:2])
		ul, err := ebml.NewNoUnlacer(r, int64(len(frame)))
		if err != nil {
			t.Fatalf("constructor must not read: %v", err)
		}
		if _, err := ul.Read(); !errors.Is(err, io.ErrUnexpectedEOF) {
			t.Fatalf("want ErrUnexpectedEOF, got %v", err)
		}
	})
}
