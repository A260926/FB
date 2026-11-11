package wav_test

import (
	"bytes"
	"encoding/binary"
	"github.com/go-audio/audio"
	"github.com/go-audio/wav"
	"reflect"
	"testing"
)

func testChunk(id string, p []byte) []byte {
	b := new(bytes.Buffer)
	b.WriteString(id)
	binary.Write(b, binary.LittleEndian, uint32(len(p)))
	b.Write(p)
	if len(p)%2 != 0 {
		b.WriteByte(0)
	}
	return b.Bytes()
}

// This fixture is byte-authored, not written using wav.Encoder.
func testWAVE(extensible bool) ([]byte, []int) {
	vals := []int{-524288, -1, 0, 1, 123456, 524287, 524287, 123456, 1, 0, -1, -524288}
	fmt := new(bytes.Buffer)
	tag := uint16(1)
	if extensible {
		tag = 0xfffe
	}
	for _, v := range []interface{}{tag, uint16(6), uint32(48000), uint32(48000 * 18), uint16(18), uint16(24)} {
		binary.Write(fmt, binary.LittleEndian, v)
	}
	if extensible {
		binary.Write(fmt, binary.LittleEndian, uint16(22))
		binary.Write(fmt, binary.LittleEndian, uint16(20))
		binary.Write(fmt, binary.LittleEndian, uint32(0x3f))
		fmt.Write([]byte{1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71})
	}
	pcm := []byte{}
	for _, v := range vals {
		if extensible {
			v <<= 4
		}
		pcm = append(pcm, byte(v), byte(v>>8), byte(v>>16))
	}
	body := append([]byte("WAVE"), testChunk("fmt ", fmt.Bytes())...)
	body = append(body, testChunk("JUNK", []byte{7, 8, 9})...)
	body = append(body, testChunk("data", pcm)...)
	b := new(bytes.Buffer)
	b.WriteString("RIFF")
	binary.Write(b, binary.LittleEndian, uint32(len(body)))
	b.Write(body)
	return b.Bytes(), vals
}
func TestBaseline(t *testing.T) {
	data, want := testWAVE(false)
	d := wav.NewDecoder(bytes.NewReader(data))
	got, e := d.FullPCMBuffer()
	if e != nil {
		t.Fatal(e)
	}
	if !reflect.DeepEqual(got.Data, want) {
		t.Fatalf("PCM got=%v want=%v", got.Data, want)
	}
	if got.Format.NumChannels != 6 || got.SourceBitDepth != 24 {
		t.Fatal(got.Format, got.SourceBitDepth)
	}
	if e = d.Rewind(); e != nil {
		t.Fatal(e)
	}
	var all []int
	for i := 0; i < 2; i++ {
		b := &audio.IntBuffer{Data: make([]int, 6)}
		n, e := d.PCMBuffer(b)
		if e != nil {
			t.Fatal(e)
		}
		if n != 6 {
			t.Fatalf("read=%d", n)
		}
		all = append(all, b.Data[:n]...)
	}
	if !reflect.DeepEqual(all, want) {
		t.Fatal(all, want)
	}
	t.Log("PASS: independent 6-channel 24-bit RIFF, odd JUNK, full and frame-sized reads, rewind")
}
