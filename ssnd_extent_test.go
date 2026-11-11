package aiff

import (
	"bytes"
	"encoding/binary"
	"io"
	"reflect"
	"testing"
	"time"

	"github.com/go-audio/audio"
)

// trickleReader hands out at most 1 to 3 bytes per Read to make sure
// short reads from the underlying reader are never mistaken for the
// end of the sound data.
type trickleReader struct {
	r     *bytes.Reader
	reads int
}

func (t *trickleReader) Read(p []byte) (int, error) {
	max := t.reads%3 + 1
	t.reads++
	if len(p) > max {
		p = p[:max]
	}
	return t.r.Read(p)
}

func (t *trickleReader) Seek(offset int64, whence int) (int64, error) {
	return t.r.Seek(offset, whence)
}

// extentSpec describes an in-memory AIFF file used to test how the
// decoder interprets the SSND sound data extent.
type extentSpec struct {
	bits       uint16
	chans      uint16
	frames     uint32
	offset     uint32
	offsetData int // offset bytes actually present in the SSND body
	blockSize  uint32
	pcm        []byte
	slack      []byte // alignment bytes appended to the SSND body after the audio
	commAfter  bool   // place the SSND chunk before the COMM chunk
	unknown    bool   // insert an unknown odd-sized chunk before COMM
	comt       string // trailing COMT comment text, if not empty
}

func buildExtentFile(spec extentSpec) []byte {
	chunk := func(id string, b []byte) []byte {
		var out bytes.Buffer
		out.WriteString(id)
		binary.Write(&out, binary.BigEndian, uint32(len(b)))
		out.Write(b)
		if len(b)%2 != 0 {
			out.WriteByte(0)
		}
		return out.Bytes()
	}
	var comm, ssnd, body, out bytes.Buffer
	binary.Write(&comm, binary.BigEndian, spec.chans)
	binary.Write(&comm, binary.BigEndian, spec.frames)
	binary.Write(&comm, binary.BigEndian, spec.bits)
	comm.Write([]byte{0x40, 0x0e, 0xac, 0x44, 0, 0, 0, 0, 0, 0}) // 44100
	binary.Write(&ssnd, binary.BigEndian, spec.offset)
	binary.Write(&ssnd, binary.BigEndian, spec.blockSize)
	ssnd.Write(make([]byte, spec.offsetData))
	ssnd.Write(spec.pcm)
	ssnd.Write(spec.slack)
	body.WriteString("AIFF")
	if spec.unknown {
		body.Write(chunk("abcd", []byte{1, 2, 3}))
	}
	if spec.commAfter {
		body.Write(chunk("SSND", ssnd.Bytes()))
		body.Write(chunk("COMM", comm.Bytes()))
	} else {
		body.Write(chunk("COMM", comm.Bytes()))
		body.Write(chunk("SSND", ssnd.Bytes()))
	}
	if spec.comt != "" {
		var c bytes.Buffer
		binary.Write(&c, binary.BigEndian, uint16(1))
		c.Write(make([]byte, 8))
		c.WriteByte(byte(len(spec.comt)))
		c.WriteString(spec.comt)
		body.Write(chunk("COMT", c.Bytes()))
	}
	out.WriteString("FORM")
	binary.Write(&out, binary.BigEndian, uint32(body.Len()))
	out.Write(body.Bytes())
	return out.Bytes()
}

// collectPCM gathers all the samples through repeated PCMBuffer calls
// using a buffer that is intentionally not a multiple of the frame size.
func collectPCM(t *testing.T, d *Decoder) []int {
	t.Helper()
	var got []int
	buf := &audio.IntBuffer{Data: make([]int, 3)}
	for {
		n, err := d.PCMBuffer(buf)
		got = append(got, buf.Data[:n]...)
		if err == io.EOF {
			break
		}
		if err != nil {
			t.Fatalf("PCMBuffer failed: %v", err)
		}
		if n == 0 {
			t.Fatal("PCMBuffer returned 0 samples without an error")
		}
	}
	return got
}

func checkExtent(t *testing.T, data []byte, spec extentSpec, want []int, wantLen int64) {
	t.Helper()

	// full read
	d := NewDecoder(&trickleReader{r: bytes.NewReader(data)})
	buf, err := d.FullPCMBuffer()
	if err != nil {
		t.Fatalf("FullPCMBuffer failed: %v", err)
	}
	if !reflect.DeepEqual(buf.Data, want) {
		t.Fatalf("FullPCMBuffer samples: expected %v, got %v", want, buf.Data)
	}
	if d.PCMLen() != wantLen {
		t.Fatalf("PCMLen: expected %d, got %d", wantLen, d.PCMLen())
	}
	// Duration is based on the COMM frame count, not the SSND sizing
	wantDur := time.Duration(float64(spec.frames) / 44100.0 * float64(time.Second))
	if dur, err := d.Duration(); err != nil || dur != wantDur {
		t.Fatalf("Duration: expected %v, got %v (%v)", wantDur, dur, err)
	}
	// draining after a full read keeps parsing the trailing metadata
	if err := d.Drain(); err != nil {
		t.Fatalf("Drain after full read failed: %v", err)
	}
	if spec.comt != "" {
		if len(d.Comments) != 1 || d.Comments[0] != spec.comt {
			t.Fatalf("expected trailing comment %q, got %v", spec.comt, d.Comments)
		}
	}

	// incremental reads collect the exact same samples
	d = NewDecoder(&trickleReader{r: bytes.NewReader(data)})
	if got := collectPCM(t, d); !reflect.DeepEqual(got, want) {
		t.Fatalf("PCMBuffer samples: expected %v, got %v", want, got)
	}
	// rewinding reads the sound data from the first sample again
	if err := d.Rewind(); err != nil {
		t.Fatalf("Rewind failed: %v", err)
	}
	if got := collectPCM(t, d); !reflect.DeepEqual(got, want) {
		t.Fatalf("PCMBuffer samples after rewind: expected %v, got %v", want, got)
	}

	// ReadInfo/Duration can be called repeatedly without eating audio
	d = NewDecoder(&trickleReader{r: bytes.NewReader(data)})
	d.ReadInfo()
	if _, err := d.Duration(); err != nil {
		t.Fatalf("Duration failed: %v", err)
	}
	d.ReadInfo()
	if _, err := d.Duration(); err != nil {
		t.Fatalf("Duration failed: %v", err)
	}
	if got := collectPCM(t, d); !reflect.DeepEqual(got, want) {
		t.Fatalf("PCMBuffer samples after ReadInfo/Duration: expected %v, got %v", want, got)
	}

	// a partial read followed by Drain skips the remaining audio and
	// the SSND padding before resuming the container parsing
	d = NewDecoder(&trickleReader{r: bytes.NewReader(data)})
	n, err := d.PCMBuffer(&audio.IntBuffer{Data: make([]int, 2)})
	if err != nil || n == 0 {
		t.Fatalf("partial PCMBuffer failed: %d %v", n, err)
	}
	if err := d.Drain(); err != nil {
		t.Fatalf("Drain after partial read failed: %v", err)
	}
	if spec.comt != "" {
		if len(d.Comments) != 1 || d.Comments[0] != spec.comt {
			t.Fatalf("expected trailing comment %q after partial read, got %v", spec.comt, d.Comments)
		}
	}
}

func TestSSNDExtent(t *testing.T) {
	t.Run("16bit mono offset0 block0", func(t *testing.T) {
		spec := extentSpec{bits: 16, chans: 1, frames: 2,
			pcm: []byte{0x04, 0xd2, 0xfb, 0x2e}}
		checkExtent(t, buildExtentFile(spec), spec, []int{1234, -1234}, 4)
	})
	t.Run("8bit mono offset2 block0 odd sample", func(t *testing.T) {
		spec := extentSpec{bits: 8, chans: 1, frames: 1, offset: 2, offsetData: 2,
			pcm: []byte{17}}
		checkExtent(t, buildExtentFile(spec), spec, []int{17}, 1)
	})
	t.Run("8bit mono offset0 block0 odd sample", func(t *testing.T) {
		spec := extentSpec{bits: 8, chans: 1, frames: 1,
			pcm: []byte{200}}
		checkExtent(t, buildExtentFile(spec), spec, []int{200}, 1)
	})
	t.Run("8bit mono offset2 block4 slack", func(t *testing.T) {
		// non-zero blockSize with alignment slack after the audio: the
		// slack (here disguised as a chunk header) is not part of the
		// PCM and is skipped when draining.
		spec := extentSpec{bits: 8, chans: 1, frames: 1, offset: 2, offsetData: 2, blockSize: 4,
			pcm:   []byte{17},
			slack: []byte{'C', 'O', 'M', 'T'}}
		checkExtent(t, buildExtentFile(spec), spec, []int{17}, 1)
	})
	t.Run("16bit stereo blockSize ignored", func(t *testing.T) {
		// 2 channels * 3 frames * 2 bytes = 12 bytes whatever blockSize says
		spec := extentSpec{bits: 16, chans: 2, frames: 3, blockSize: 512,
			pcm: []byte{0, 1, 0xff, 0xff, 0, 2, 0xff, 0xfe, 0, 3, 0xff, 0xfd}}
		checkExtent(t, buildExtentFile(spec), spec, []int{1, -1, 2, -2, 3, -3}, 12)
	})
	t.Run("24bit stereo", func(t *testing.T) {
		spec := extentSpec{bits: 24, chans: 2, frames: 3,
			pcm: []byte{
				0x00, 0x00, 0x01, 0xff, 0xff, 0xff,
				0x00, 0x01, 0x00, 0xff, 0xff, 0x00,
				0x01, 0x02, 0x03, 0x80, 0x00, 0x00,
			}}
		checkExtent(t, buildExtentFile(spec), spec,
			[]int{1, -1, 256, -256, 66051, -8388608}, 18)
	})
	t.Run("comm after ssnd", func(t *testing.T) {
		spec := extentSpec{bits: 16, chans: 1, frames: 2, commAfter: true,
			pcm: []byte{0x04, 0xd2, 0xfb, 0x2e}}
		checkExtent(t, buildExtentFile(spec), spec, []int{1234, -1234}, 4)
	})
	t.Run("unknown odd chunk before comm", func(t *testing.T) {
		spec := extentSpec{bits: 16, chans: 1, frames: 2, unknown: true,
			pcm: []byte{0x04, 0xd2, 0xfb, 0x2e}}
		checkExtent(t, buildExtentFile(spec), spec, []int{1234, -1234}, 4)
	})
	t.Run("trailing comt", func(t *testing.T) {
		spec := extentSpec{bits: 16, chans: 1, frames: 2, comt: "tail",
			pcm: []byte{0x04, 0xd2, 0xfb, 0x2e}}
		checkExtent(t, buildExtentFile(spec), spec, []int{1234, -1234}, 4)
	})
	t.Run("trailing comt after odd ssnd", func(t *testing.T) {
		spec := extentSpec{bits: 8, chans: 1, frames: 1, offset: 2, offsetData: 2,
			pcm: []byte{17}, comt: "tail"}
		checkExtent(t, buildExtentFile(spec), spec, []int{17}, 1)
	})

	t.Run("declared frames exceed chunk body", func(t *testing.T) {
		// 4 frames of 16bit mono need 8 bytes but only 2 are present
		spec := extentSpec{bits: 16, chans: 1, frames: 4, pcm: []byte{0, 1}}
		d := NewDecoder(bytes.NewReader(buildExtentFile(spec)))
		if _, err := d.FullPCMBuffer(); err == nil {
			t.Fatal("expected an error for a SSND body smaller than the declared frames")
		}
	})
	t.Run("offset beyond chunk body", func(t *testing.T) {
		spec := extentSpec{bits: 8, chans: 1, frames: 1, offset: 100, offsetData: 0,
			pcm: []byte{17}}
		d := NewDecoder(bytes.NewReader(buildExtentFile(spec)))
		if _, err := d.FullPCMBuffer(); err == nil {
			t.Fatal("expected an error for an offset beyond the SSND body")
		}
	})
	t.Run("ssnd smaller than its header", func(t *testing.T) {
		spec := extentSpec{bits: 16, chans: 1, frames: 1, pcm: []byte{0, 1}}
		data := buildExtentFile(spec)
		// shrink the declared SSND size below the 8 byte offset/blockSize header
		i := bytes.Index(data, []byte("SSND"))
		if i < 0 {
			t.Fatal("SSND not found")
		}
		binary.BigEndian.PutUint32(data[i+4:i+8], 4)
		d := NewDecoder(bytes.NewReader(data))
		if _, err := d.FullPCMBuffer(); err == nil {
			t.Fatal("expected an error for a SSND chunk smaller than its header")
		}
	})
	t.Run("declared size overflows", func(t *testing.T) {
		// 0xffffffff frames of 32bit stereo can't be addressed
		spec := extentSpec{bits: 32, chans: 2, frames: 0xffffffff, pcm: []byte{0, 0, 0, 1}}
		d := NewDecoder(bytes.NewReader(buildExtentFile(spec)))
		if _, err := d.FullPCMBuffer(); err == nil {
			t.Fatal("expected an error for an overflowing sound data size")
		}
	})
	t.Run("truncated sound data", func(t *testing.T) {
		spec := extentSpec{bits: 16, chans: 1, frames: 2,
			pcm: []byte{0x04, 0xd2, 0xfb, 0x2e}}
		data := buildExtentFile(spec)
		truncated := data[:len(data)-1]
		d := NewDecoder(bytes.NewReader(truncated))
		if _, err := d.FullPCMBuffer(); err == nil {
			t.Fatal("expected an error for truncated sound data")
		}
		d = NewDecoder(bytes.NewReader(truncated))
		buf := &audio.IntBuffer{Data: make([]int, 3)}
		var err error
		for err == nil {
			_, err = d.PCMBuffer(buf)
		}
		if err == io.EOF {
			t.Fatal("expected a truncation error, got a clean EOF")
		}
	})
}
