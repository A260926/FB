package wav_test

import (
	"bytes"
	"encoding/binary"
	"reflect"
	"testing"

	"github.com/go-audio/audio"
	"github.com/go-audio/wav"
)

// pcmSubformatGUIDBytes is the on-disk PCM subformat GUID.
var pcmSubformatGUIDBytes = []byte{
	0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x10, 0x00,
	0x80, 0x00, 0x00, 0xaa,
	0x00, 0x38, 0x9b, 0x71,
}

// extStdFmt writes the 16 standard fmt bytes for a 6 channel, 48kHz, 24-bit
// file tagged as WAVE_FORMAT_EXTENSIBLE.
func extStdFmt() []byte {
	b := new(bytes.Buffer)
	for _, v := range []interface{}{uint16(0xfffe), uint16(6), uint32(48000), uint32(48000 * 18), uint16(18), uint16(24)} {
		binary.Write(b, binary.LittleEndian, v)
	}
	return b.Bytes()
}

// extFmtPayload builds an extensible fmt payload, optionally mutating the
// canonical 20 valid bits / 0x3f mask / PCM GUID values. extraBytes are
// appended after the 22 extension bytes (cbSize grows accordingly).
func extFmtPayload(validBits uint16, mask uint32, guid []byte, extraBytes []byte) []byte {
	b := new(bytes.Buffer)
	b.Write(extStdFmt())
	cbSize := uint16(22 + len(extraBytes))
	binary.Write(b, binary.LittleEndian, cbSize)
	binary.Write(b, binary.LittleEndian, validBits)
	binary.Write(b, binary.LittleEndian, mask)
	b.Write(guid)
	b.Write(extraBytes)
	return b.Bytes()
}

// riffChunk serializes one RIFF chunk with an explicit declared size; an odd
// sized chunk gets the mandatory zero padding byte.
func riffChunk(t *testing.T, id string, declaredSize uint32, payload []byte) []byte {
	t.Helper()
	b := new(bytes.Buffer)
	b.WriteString(id)
	binary.Write(b, binary.LittleEndian, declaredSize)
	b.Write(payload)
	if declaredSize%2 == 1 {
		b.WriteByte(0)
	}
	return b.Bytes()
}

// wrapRIFF assembles a complete WAVE file out of byte-authored chunks.
func wrapRIFF(t *testing.T, chunks ...[]byte) []byte {
	t.Helper()
	body := append([]byte("WAVE"), bytes.Join(chunks, nil)...)
	b := new(bytes.Buffer)
	b.WriteString("RIFF")
	binary.Write(b, binary.LittleEndian, uint32(len(body)))
	b.Write(body)
	return b.Bytes()
}

// extPCM serializes the baseline values as left-aligned 24-bit samples.
func extPCM(vals []int, shift uint) []byte {
	pcm := make([]byte, 0, len(vals)*3)
	for _, v := range vals {
		v <<= shift
		pcm = append(pcm, byte(v), byte(v>>8), byte(v>>16))
	}
	return pcm
}

// canonicalVals returns the two frame values used by the byte-authored
// extensible fixtures.
func canonicalVals() []int {
	return []int{-524288, -1, 0, 1, 123456, 524287, 524287, 123456, 1, 0, -1, -524288}
}

func TestExtensibleDecode(t *testing.T) {
	data, want := testWAVE(true)

	check := func(t *testing.T, d *wav.Decoder) {
		t.Helper()
		if d.WavAudioFormat != 0xfffe {
			t.Fatalf("WavAudioFormat=%#x want 0xfffe", d.WavAudioFormat)
		}
		if d.BitDepth != 24 {
			t.Fatalf("BitDepth=%d want 24 (container width)", d.BitDepth)
		}
		if d.SampleBitDepth() != 24 {
			t.Fatalf("SampleBitDepth=%d want 24 (container width)", d.SampleBitDepth())
		}
		if d.ValidBitsPerSample != 20 {
			t.Fatalf("ValidBitsPerSample=%d want 20", d.ValidBitsPerSample)
		}
		if d.ChannelMask != 0x3f {
			t.Fatalf("ChannelMask=%#x want 0x3f", d.ChannelMask)
		}
		if d.NumChans != 6 || d.SampleRate != 48000 {
			t.Fatalf("NumChans=%d SampleRate=%d", d.NumChans, d.SampleRate)
		}
	}

	// Direct full read.
	d := wav.NewDecoder(bytes.NewReader(data))
	got, err := d.FullPCMBuffer()
	if err != nil {
		t.Fatal(err)
	}
	check(t, d)
	if !reflect.DeepEqual(got.Data, want) {
		t.Fatalf("FullPCMBuffer got=%v want=%v", got.Data, want)
	}
	if got.SourceBitDepth != 20 {
		t.Fatalf("SourceBitDepth=%d want 20 (effective depth)", got.SourceBitDepth)
	}
	if got.Format.NumChannels != 6 {
		t.Fatalf("format channels=%d", got.Format.NumChannels)
	}

	// Frame-sized reads after a rewind.
	if err := d.Rewind(); err != nil {
		t.Fatal(err)
	}
	check(t, d)
	var all []int
	for i := 0; i < 2; i++ {
		b := &audio.IntBuffer{Data: make([]int, 6)}
		n, err := d.PCMBuffer(b)
		if err != nil {
			t.Fatal(err)
		}
		if n != 6 {
			t.Fatalf("read %d samples, want 6", n)
		}
		if b.SourceBitDepth != 20 {
			t.Fatalf("PCMBuffer SourceBitDepth=%d want 20", b.SourceBitDepth)
		}
		all = append(all, b.Data[:n]...)
	}
	if !reflect.DeepEqual(all, want) {
		t.Fatalf("frame reads got=%v want=%v", all, want)
	}

	// ReadInfo first, then read: the outcome must be identical and the error
	// state must stay clean.
	d2 := wav.NewDecoder(bytes.NewReader(data))
	d2.ReadInfo()
	if err := d2.Err(); err != nil {
		t.Fatal(err)
	}
	check(t, d2)
	got2, err := d2.FullPCMBuffer()
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(got2.Data, want) || got2.SourceBitDepth != 20 {
		t.Fatalf("ReadInfo path got=%v depth=%d", got2.Data, got2.SourceBitDepth)
	}
}

func TestExtensibleDecodeMaskZero(t *testing.T) {
	payload := extFmtPayload(20, 0, pcmSubformatGUIDBytes, nil)
	file := wrapRIFF(t,
		riffChunk(t, "fmt ", uint32(len(payload)), payload),
		riffChunk(t, "data", 36, extPCM(canonicalVals(), 4)),
	)
	d := wav.NewDecoder(bytes.NewReader(file))
	d.ReadInfo()
	if err := d.Err(); err != nil {
		t.Fatal(err)
	}
	if d.ChannelMask != 0 || d.ValidBitsPerSample != 20 {
		t.Fatalf("mask=%#x validBits=%d", d.ChannelMask, d.ValidBitsPerSample)
	}
	buf, err := d.FullPCMBuffer()
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(buf.Data, canonicalVals()) {
		t.Fatalf("data=%v", buf.Data)
	}
}

func TestExtensibleRegularPCMFields(t *testing.T) {
	data, _ := testWAVE(false)
	d := wav.NewDecoder(bytes.NewReader(data))
	d.ReadInfo()
	if err := d.Err(); err != nil {
		t.Fatal(err)
	}
	if d.WavAudioFormat != 1 || d.BitDepth != 24 || d.ValidBitsPerSample != 24 || d.ChannelMask != 0 {
		t.Fatalf("format=%d container=%d valid=%d mask=%#x",
			d.WavAudioFormat, d.BitDepth, d.ValidBitsPerSample, d.ChannelMask)
	}
	buf, err := d.FullPCMBuffer()
	if err != nil {
		t.Fatal(err)
	}
	if buf.SourceBitDepth != 24 {
		t.Fatalf("SourceBitDepth=%d want 24", buf.SourceBitDepth)
	}
}

func TestExtensibleDecodeErrors(t *testing.T) {
	goodGUID := pcmSubformatGUIDBytes
	badGUID := append([]byte(nil), goodGUID...)
	badGUID[0] = 0x02

	cases := map[string]struct {
		payload []byte
	}{
		"extension truncated before cbSize": {
			payload: extStdFmt(), // 16 bytes, no extension at all
		},
		"cbSize below 22": {
			payload: func() []byte {
				b := new(bytes.Buffer)
				b.Write(extStdFmt())
				binary.Write(b, binary.LittleEndian, uint16(0))
				return b.Bytes()
			}(),
		},
		"declared chunk shorter than cbSize": {
			payload: func() []byte {
				b := new(bytes.Buffer)
				b.Write(extStdFmt())
				binary.Write(b, binary.LittleEndian, uint16(22))
				binary.Write(b, binary.LittleEndian, uint16(20))
				binary.Write(b, binary.LittleEndian, uint32(0x3f))
				// GUID missing: only 14 of the 24 extension bytes are present.
				return b.Bytes()
			}(),
		},
		"valid bits zero": {
			payload: extFmtPayload(0, 0x3f, goodGUID, nil),
		},
		"valid bits above container": {
			payload: extFmtPayload(25, 0x3f, goodGUID, nil),
		},
		"mask channel mismatch": {
			payload: extFmtPayload(20, 0x1, goodGUID, nil),
		},
		"non PCM subformat GUID": {
			payload: extFmtPayload(20, 0x3f, badGUID, nil),
		},
		"unsupported 8 bit extensible container": {
			payload: func() []byte {
				b := new(bytes.Buffer)
				// 1 channel, 8-bit container, tagged extensible.
				for _, v := range []interface{}{uint16(0xfffe), uint16(1), uint32(48000), uint32(48000), uint16(1), uint16(8)} {
					binary.Write(b, binary.LittleEndian, v)
				}
				binary.Write(b, binary.LittleEndian, uint16(22))
				binary.Write(b, binary.LittleEndian, uint16(8))
				binary.Write(b, binary.LittleEndian, uint32(0x4))
				b.Write(goodGUID)
				return b.Bytes()
			}(),
		},
	}

	for name, tc := range cases {
		t.Run(name, func(t *testing.T) {
			file := wrapRIFF(t,
				riffChunk(t, "fmt ", uint32(len(tc.payload)), tc.payload),
				riffChunk(t, "JUNK", 3, []byte{7, 8, 9}),
				riffChunk(t, "data", 36, extPCM(canonicalVals(), 4)),
			)

			// ReadInfo must surface the error.
			d := wav.NewDecoder(bytes.NewReader(file))
			d.ReadInfo()
			if d.Err() == nil {
				t.Fatal("expected ReadInfo to fail")
			}

			// A fresh decoder must refuse to hand out PCM: an error, never a
			// buffer of successfully decoded zeros.
			d2 := wav.NewDecoder(bytes.NewReader(file))
			buf, err := d2.FullPCMBuffer()
			if err == nil {
				t.Fatal("expected FullPCMBuffer to fail")
			}
			if buf != nil {
				t.Fatalf("expected nil buffer on error, got %v", buf.Data)
			}

			// Same via frame-sized reads.
			d3 := wav.NewDecoder(bytes.NewReader(file))
			_, err = d3.PCMBuffer(&audio.IntBuffer{Data: make([]int, 6)})
			if err == nil {
				t.Fatal("expected PCMBuffer to fail")
			}
		})
	}
}

func TestExtensibleDecodeWidths(t *testing.T) {
	cases := []struct {
		name         string
		container    uint16
		validBits    uint16
		chans        uint16
		vals         []int
		encodeSample func(v int) []byte
	}{
		{
			name:      "16 bit container, 12 valid bits, 2 channels",
			container: 16, validBits: 12, chans: 2,
			vals: []int{-2048, -1, 0, 2047},
			encodeSample: func(v int) []byte {
				v <<= 4
				return []byte{byte(v), byte(v >> 8)}
			},
		},
		{
			name:      "32 bit container, 20 valid bits, 1 channel",
			container: 32, validBits: 20, chans: 1,
			vals: []int{-524288, 0, 524287},
			encodeSample: func(v int) []byte {
				v <<= 12
				return []byte{byte(v), byte(v >> 8), byte(v >> 16), byte(v >> 24)}
			},
		},
		{
			name:      "24 bit container, full 24 valid bits, no shift",
			container: 24, validBits: 24, chans: 1,
			vals: []int{-8388608, -1, 0, 1, 1234567, 8388607},
			encodeSample: func(v int) []byte {
				return []byte{byte(v), byte(v >> 8), byte(v >> 16)}
			},
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			blockAlign := tc.chans * tc.container / 8
			std := new(bytes.Buffer)
			for _, v := range []interface{}{
				uint16(0xfffe), tc.chans, uint32(48000),
				uint32(48000) * uint32(blockAlign),
				blockAlign, tc.container,
			} {
				binary.Write(std, binary.LittleEndian, v)
			}
			ext := extFmtPayload(tc.validBits, 0, pcmSubformatGUIDBytes, nil)
			payload := append(std.Bytes(), ext[16:]...)
			pcm := make([]byte, 0, len(tc.vals)*int(tc.container/8))
			for _, v := range tc.vals {
				pcm = append(pcm, tc.encodeSample(v)...)
			}
			file := wrapRIFF(t,
				riffChunk(t, "fmt ", uint32(len(payload)), payload),
				riffChunk(t, "data", uint32(len(pcm)), pcm),
			)
			d := wav.NewDecoder(bytes.NewReader(file))
			buf, err := d.FullPCMBuffer()
			if err != nil {
				t.Fatal(err)
			}
			if d.BitDepth != tc.container || d.ValidBitsPerSample != tc.validBits {
				t.Fatalf("container=%d valid=%d", d.BitDepth, d.ValidBitsPerSample)
			}
			if buf.SourceBitDepth != int(tc.validBits) {
				t.Fatalf("SourceBitDepth=%d", buf.SourceBitDepth)
			}
			if !reflect.DeepEqual(buf.Data, tc.vals) {
				t.Fatalf("data=%v want=%v", buf.Data, tc.vals)
			}
		})
	}
}

func TestExtensibleDecodeExtraFmtAndOddChunks(t *testing.T) {
	// cbSize 24: two extra, still well-formed extension bytes.
	payload := extFmtPayload(20, 0x3f, pcmSubformatGUIDBytes, []byte{0xab, 0xcd})
	file := wrapRIFF(t,
		riffChunk(t, "fmt ", uint32(len(payload)), payload),
		// odd length unknown chunk with its padding byte
		riffChunk(t, "JUNK", 3, []byte{7, 8, 9}),
		riffChunk(t, "data", 36, extPCM(canonicalVals(), 4)),
	)
	d := wav.NewDecoder(bytes.NewReader(file))
	buf, err := d.FullPCMBuffer()
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(buf.Data, canonicalVals()) {
		t.Fatalf("data=%v", buf.Data)
	}
	if d.ValidBitsPerSample != 20 || d.ChannelMask != 0x3f {
		t.Fatalf("validBits=%d mask=%#x", d.ValidBitsPerSample, d.ChannelMask)
	}
}

// parseExtHeader independently inspects the encoded file's bytes.
func parseExtHeader(t *testing.T, file []byte) (fmtSize, cbSize uint16, validBits uint16, mask uint32, guid []byte, blockAlign uint16, avgRate uint32, dataSize, riffSize uint32) {
	t.Helper()
	if string(file[0:4]) != "RIFF" || string(file[8:12]) != "WAVE" {
		t.Fatal("not a RIFF/WAVE file")
	}
	riffSize = binary.LittleEndian.Uint32(file[4:8])
	if int(riffSize) != len(file)-8 {
		t.Fatalf("RIFF size=%d, file body=%d", riffSize, len(file)-8)
	}
	if string(file[12:16]) != "fmt " {
		t.Fatalf("first chunk=%q", file[12:16])
	}
	fmtChunkSize := binary.LittleEndian.Uint32(file[16:20])
	fmt := file[20 : 20+fmtChunkSize]
	if binary.LittleEndian.Uint16(fmt[0:2]) != 0xfffe {
		t.Fatalf("format tag=%#x", binary.LittleEndian.Uint16(fmt[0:2]))
	}
	avgRate = binary.LittleEndian.Uint32(fmt[8:12])
	blockAlign = binary.LittleEndian.Uint16(fmt[12:14])
	containerBits := binary.LittleEndian.Uint16(fmt[14:16])
	if containerBits != 24 {
		t.Fatalf("container bits=%d", containerBits)
	}
	cbSize = binary.LittleEndian.Uint16(fmt[16:18])
	validBits = binary.LittleEndian.Uint16(fmt[18:20])
	mask = binary.LittleEndian.Uint32(fmt[20:24])
	guid = append([]byte(nil), fmt[24:40]...)
	fmtSize = uint16(fmtChunkSize)

	off := 20 + fmtChunkSize
	if string(file[off:off+4]) != "data" {
		t.Fatalf("second chunk=%q", file[off:off+4])
	}
	dataSize = binary.LittleEndian.Uint32(file[off+4 : off+8])
	return
}

func TestExtensibleEncode(t *testing.T) {
	vals := canonicalVals()
	out := new(bytes.Buffer)
	e := wav.NewEncoder(&writeSeeker{buf: out}, 48000, 24, 6, 0xfffe)
	e.ValidBitsPerSample = 20
	e.ChannelMask = 0x3f

	// Two Write calls, each carrying complete 6-channel frames.
	first := &audio.IntBuffer{
		Format: &audio.Format{NumChannels: 6, SampleRate: 48000},
		Data:   vals[:6],
	}
	second := &audio.IntBuffer{
		Format: &audio.Format{NumChannels: 6, SampleRate: 48000},
		Data:   vals[6:],
	}
	if err := e.Write(first); err != nil {
		t.Fatal(err)
	}
	if err := e.Write(second); err != nil {
		t.Fatal(err)
	}
	if err := e.Close(); err != nil {
		t.Fatal(err)
	}
	file := out.Bytes()

	fmtSize, cbSize, validBits, mask, guid, blockAlign, avgRate, dataSize, riffSize := parseExtHeader(t, file)
	if fmtSize != 40 || cbSize != 22 {
		t.Fatalf("fmt chunk size=%d cbSize=%d", fmtSize, cbSize)
	}
	if validBits != 20 || mask != 0x3f {
		t.Fatalf("validBits=%d mask=%#x", validBits, mask)
	}
	if !bytes.Equal(guid, pcmSubformatGUIDBytes) {
		t.Fatalf("GUID=% x", guid)
	}
	if blockAlign != 18 {
		t.Fatalf("blockAlign=%d", blockAlign)
	}
	if avgRate != 48000*18 {
		t.Fatalf("avgBytesPerSec=%d", avgRate)
	}
	if dataSize != uint32(len(vals)*3) {
		t.Fatalf("data chunk size=%d", dataSize)
	}
	if int(riffSize) != len(file)-8 {
		t.Fatalf("RIFF size mismatch")
	}

	// Independent byte check of the left-aligned PCM payload.
	off := len(file) - len(vals)*3
	wantPCM := extPCM(vals, 4)
	if !bytes.Equal(file[off:], wantPCM) {
		t.Fatalf("PCM bytes mismatch\n got=% x\nwant=% x", file[off:], wantPCM)
	}
	// Every sample is left-aligned, so the low padding bits of its least
	// significant byte must be zero on disk (shift is 4 here).
	for i := range vals {
		if file[off+i*3]&0x0f != 0 {
			t.Fatalf("padding nibble of sample %d not zero: %#x", i, file[off+i*3])
		}
	}

	// Secondary sanity check: the library can read its own output back.
	d := wav.NewDecoder(bytes.NewReader(file))
	back, err := d.FullPCMBuffer()
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(back.Data, vals) || back.SourceBitDepth != 20 {
		t.Fatalf("roundtrip data=%v depth=%d", back.Data, back.SourceBitDepth)
	}
}

func TestExtensibleEncodeContainers(t *testing.T) {
	cases := []struct {
		container int
		validBits uint16
		value     int
	}{
		{16, 12, -2048},
		{16, 16, -12345},
		{24, 20, 123456},
		{24, 24, -8000000},
		{32, 20, 524287},
		{32, 32, 100000000},
	}
	for _, tc := range cases {
		out := new(bytes.Buffer)
		e := wav.NewEncoder(&writeSeeker{buf: out}, 48000, tc.container, 1, 0xfffe)
		e.ValidBitsPerSample = tc.validBits
		e.ChannelMask = 0x4 // FC bit, single speaker
		buf := &audio.IntBuffer{
			Format: &audio.Format{NumChannels: 1, SampleRate: 48000},
			Data:   []int{tc.value},
		}
		if err := e.Write(buf); err != nil {
			t.Fatalf("%+v: %v", tc, err)
		}
		if err := e.Close(); err != nil {
			t.Fatal(err)
		}
		file := out.Bytes()

		shift := uint(tc.container) - uint(tc.validBits)
		aligned := tc.value << shift
		var want []byte
		for i := 0; i < tc.container/8; i++ {
			want = append(want, byte(aligned>>(8*i)))
		}
		got := file[len(file)-tc.container/8:]
		if !bytes.Equal(got, want) {
			t.Fatalf("container=%d valid=%d: sample bytes got=% x want=% x",
				tc.container, tc.validBits, got, want)
		}
		// Re-decode to confirm symmetry.
		d := wav.NewDecoder(bytes.NewReader(file))
		back, err := d.FullPCMBuffer()
		if err != nil {
			t.Fatal(err)
		}
		if len(back.Data) != 1 || back.Data[0] != tc.value {
			t.Fatalf("container=%d valid=%d: got=%v want=%d",
				tc.container, tc.validBits, back.Data, tc.value)
		}
	}
}

func TestExtensibleEncodeValidation(t *testing.T) {
	goodBuf := func(data []int) *audio.IntBuffer {
		return &audio.IntBuffer{
			Format: &audio.Format{NumChannels: 6, SampleRate: 48000},
			Data:   data,
		}
	}
	validFrame := canonicalVals()[:6]

	cases := map[string]func(*wav.Encoder) *audio.IntBuffer{
		"sample above valid range": func(e *wav.Encoder) *audio.IntBuffer {
			return goodBuf([]int{524288, 0, 0, 0, 0, 0}) // 20-bit max is 524287
		},
		"sample below valid range": func(e *wav.Encoder) *audio.IntBuffer {
			return goodBuf([]int{-524289, 0, 0, 0, 0, 0})
		},
		"buffer channel mismatch": func(e *wav.Encoder) *audio.IntBuffer {
			return &audio.IntBuffer{
				Format: &audio.Format{NumChannels: 2, SampleRate: 48000},
				Data:   []int{1, 2},
			}
		},
		"buffer sample rate mismatch": func(e *wav.Encoder) *audio.IntBuffer {
			return &audio.IntBuffer{
				Format: &audio.Format{NumChannels: 6, SampleRate: 44100},
				Data:   validFrame,
			}
		},
		"partial frame": func(e *wav.Encoder) *audio.IntBuffer {
			return goodBuf([]int{1, 2, 3})
		},
	}

	for name, build := range cases {
		t.Run(name, func(t *testing.T) {
			out := new(bytes.Buffer)
			ws := &writeSeeker{buf: out}
			e := wav.NewEncoder(ws, 48000, 24, 6, 0xfffe)
			e.ValidBitsPerSample = 20
			e.ChannelMask = 0x3f
			if err := e.Write(build(e)); err == nil {
				t.Fatal("expected Write to fail")
			}
			// Nothing must have been written and the encoder must still work.
			if out.Len() != 0 || e.WrittenBytes != 0 {
				t.Fatalf("failed Write emitted bytes: len=%d written=%d", out.Len(), e.WrittenBytes)
			}
			if err := e.Write(goodBuf(validFrame)); err != nil {
				t.Fatalf("encoder unusable after failed Write: %v", err)
			}
			if err := e.Close(); err != nil {
				t.Fatal(err)
			}
			d := wav.NewDecoder(bytes.NewReader(out.Bytes()))
			buf, err := d.FullPCMBuffer()
			if err != nil {
				t.Fatal(err)
			}
			if !reflect.DeepEqual(buf.Data, validFrame) {
				t.Fatalf("recovered data=%v", buf.Data)
			}
		})
	}

	t.Run("encoder parameter errors", func(t *testing.T) {
		badEncoders := []struct {
			name      string
			validBits uint16
			mask      uint32
		}{
			{"valid bits zero", 0, 0x3f},
			{"valid bits above container", 25, 0x3f},
			{"mask mismatch", 20, 0x1},
		}
		for _, bc := range badEncoders {
			out := new(bytes.Buffer)
			e := wav.NewEncoder(&writeSeeker{buf: out}, 48000, 24, 6, 0xfffe)
			e.ValidBitsPerSample = bc.validBits
			e.ChannelMask = bc.mask
			if err := e.Write(goodBuf(validFrame)); err == nil {
				t.Fatalf("%s: expected error", bc.name)
			}
			if out.Len() != 0 {
				t.Fatalf("%s: wrote %d bytes", bc.name, out.Len())
			}
		}
	})

	t.Run("WriteFrame rejected", func(t *testing.T) {
		out := new(bytes.Buffer)
		e := wav.NewEncoder(&writeSeeker{buf: out}, 48000, 24, 1, 0xfffe)
		e.ValidBitsPerSample = 20
		if err := e.WriteFrame(int32(1)); err == nil {
			t.Fatal("expected WriteFrame to be rejected")
		}
		if out.Len() != 0 {
			t.Fatalf("WriteFrame wrote %d bytes", out.Len())
		}
	})

	t.Run("WriteFrame still works for plain PCM", func(t *testing.T) {
		out := new(bytes.Buffer)
		e := wav.NewEncoder(&writeSeeker{buf: out}, 48000, 16, 1, 1)
		if err := e.WriteFrame(int16(1234)); err != nil {
			t.Fatal(err)
		}
		if err := e.Close(); err != nil {
			t.Fatal(err)
		}
		if bytes.Count(out.Bytes(), []byte("data")) != 1 {
			t.Fatal("plain PCM WriteFrame did not produce a data chunk")
		}
	})
}

func TestExtensibleEncodeFailureAfterSuccess(t *testing.T) {
	out := new(bytes.Buffer)
	e := wav.NewEncoder(&writeSeeker{buf: out}, 48000, 24, 2, 0xfffe)
	e.ValidBitsPerSample = 20
	e.ChannelMask = 0x3
	good := &audio.IntBuffer{
		Format: &audio.Format{NumChannels: 2, SampleRate: 48000},
		Data:   []int{100, -100},
	}
	if err := e.Write(good); err != nil {
		t.Fatal(err)
	}
	lenAfterGood := out.Len()

	bad := &audio.IntBuffer{
		Format: &audio.Format{NumChannels: 2, SampleRate: 48000},
		Data:   []int{524288, 0}, // above the 20-bit range
	}
	if err := e.Write(bad); err == nil {
		t.Fatal("expected the out-of-range Write to fail")
	}
	if out.Len() != lenAfterGood {
		t.Fatalf("failed Write changed the output: %d -> %d bytes", lenAfterGood, out.Len())
	}

	// A subsequent valid Write must land in the file and the whole thing must
	// still decode cleanly.
	if err := e.Write(good); err != nil {
		t.Fatalf("encoder not usable after a failed Write: %v", err)
	}
	if err := e.Close(); err != nil {
		t.Fatal(err)
	}
	d := wav.NewDecoder(bytes.NewReader(out.Bytes()))
	buf, err := d.FullPCMBuffer()
	if err != nil {
		t.Fatal(err)
	}
	want := []int{100, -100, 100, -100}
	if !reflect.DeepEqual(buf.Data, want) {
		t.Fatalf("data=%v want=%v", buf.Data, want)
	}
}

// writeSeeker adapts a bytes.Buffer to io.WriteSeeker.
type writeSeeker struct {
	buf *bytes.Buffer
	pos int64
}

func (w *writeSeeker) Write(p []byte) (int, error) {
	end := int(w.pos) + len(p)
	if end > w.buf.Len() {
		w.buf.Grow(end - w.buf.Len())
		w.buf.Write(make([]byte, end-w.buf.Len()))
	}
	copy(w.buf.Bytes()[w.pos:end], p)
	w.pos = int64(end)
	return len(p), nil
}

func (w *writeSeeker) Seek(offset int64, whence int) (int64, error) {
	switch whence {
	case 0:
		w.pos = offset
	case 1:
		w.pos += offset
	case 2:
		w.pos = int64(w.buf.Len()) + offset
	}
	return w.pos, nil
}
