package caf

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"io/ioutil"
	"math"
	"os"
	"path/filepath"
	"testing"

	"github.com/pascoej/caf/task_support/taskcheck"
)

// --- independent CAF walking, not reusing the package decoder ---------------

type walkedChunk struct {
	typ       FourByteString
	headerOff int64
	size      int64
	payload   []byte
}

func walkCAF(b []byte) (FileHeader, []walkedChunk, error) {
	var hdr FileHeader
	if len(b) < 8 {
		return hdr, nil, errors.New("file shorter than caff header")
	}
	r := bytes.NewReader(b)
	if err := hdr.Decode(r); err != nil {
		return hdr, nil, err
	}
	var chunks []walkedChunk
	for r.Len() > 0 {
		if r.Len() < 12 {
			return hdr, nil, errors.New("trailing bytes shorter than a chunk header")
		}
		off := int64(len(b)) - int64(r.Len())
		var ch ChunkHeader
		if err := readBE(r, &ch); err != nil {
			return hdr, nil, err
		}
		if ch.ChunkSize < 0 || int64(r.Len()) < ch.ChunkSize {
			return hdr, nil, fmt.Errorf("chunk %s at %d overruns file", ch.ChunkType[:], off)
		}
		payload := make([]byte, ch.ChunkSize)
		if _, err := io.ReadFull(r, payload); err != nil {
			return hdr, nil, err
		}
		chunks = append(chunks, walkedChunk{typ: ch.ChunkType, headerOff: off, size: ch.ChunkSize, payload: payload})
	}
	return hdr, chunks, nil
}

// decodeVarInt is an independent implementation of the CAF 7-bit integer.
func decodeVarInt(b []byte) (uint64, int, error) {
	var v uint64
	for i, by := range b {
		if i >= 8 {
			return 0, 0, errors.New("varint too long")
		}
		v = (v << 7) | uint64(by&0x7f)
		if by&0x80 == 0 {
			return v, i + 1, nil
		}
	}
	return 0, 0, errors.New("truncated varint")
}

type expectedLayout struct {
	format    AudioFormat
	cookie    []byte
	hasCookie bool
	packets   [][]byte
	fpp       int64
	editCount uint32
	reserve   int64
}

// verifyLayout independently walks every chunk and varint and checks the
// complete recorder contract, then decodes the same bytes with File.Decode
// and compares the actual audio packets and frame counts.
func verifyLayout(c *taskcheck.C, b []byte, want expectedLayout) {
	hdr, chunks, err := walkCAF(b)
	c.Check(err == nil, "independent walk failed: %v", err)
	if err != nil {
		return
	}
	c.Check(hdr.FileType == stringToChunkType("caff") && hdr.FileVersion == 1,
		"bad caff header %+v", hdr)

	wantTypes := []FourByteString{ChunkTypeAudioDescription}
	if want.hasCookie {
		wantTypes = append(wantTypes, ChunkTypeMagicCookie)
	}
	wantTypes = append(wantTypes, ChunkTypePacketTable, ChunkTypeFree, ChunkTypeAudioData)
	c.Check(len(chunks) == len(wantTypes), "chunk count %d want %d", len(chunks), len(wantTypes))
	for i, ch := range chunks {
		if i >= len(wantTypes) {
			break
		}
		c.Check(ch.typ == wantTypes[i], "chunk %d type %s want %s", i, ch.typ[:], wantTypes[i][:])
	}

	var pakt, free, data *walkedChunk
	var desc *walkedChunk
	var kuki *walkedChunk
	for i := range chunks {
		switch chunks[i].typ {
		case ChunkTypeAudioDescription:
			desc = &chunks[i]
		case ChunkTypeMagicCookie:
			kuki = &chunks[i]
		case ChunkTypePacketTable:
			pakt = &chunks[i]
		case ChunkTypeFree:
			free = &chunks[i]
		case ChunkTypeAudioData:
			data = &chunks[i]
		}
	}
	c.Check(desc != nil && pakt != nil && free != nil && data != nil, "missing required chunk")
	if desc == nil || pakt == nil || free == nil || data == nil {
		return
	}

	// desc: fixed 32-byte layout, independently decoded field by field.
	d := desc.payload
	c.Check(len(d) == 32, "desc payload %d bytes", len(d))
	rate := beFloat64(d[0:8])
	c.Check(rate == want.format.SampleRate, "sample rate %v want %v", rate, want.format.SampleRate)
	c.Check(FourByteString{d[8], d[9], d[10], d[11]} == want.format.FormatID, "format id mismatch")
	c.Check(beUint32(d[12:16]) == want.format.FormatFlags, "format flags")
	c.Check(beUint32(d[16:20]) == 0, "BytesPerPacket must be 0")
	c.Check(beUint32(d[20:24]) == want.format.FramesPerPacket, "FramesPerPacket")
	c.Check(beUint32(d[24:28]) == want.format.ChannelsPerPacket, "ChannelsPerPacket")
	c.Check(beUint32(d[28:32]) == want.format.BitsPerChannel, "BitsPerChannel")

	// kuki is optional and stored byte-for-byte.
	if want.hasCookie {
		c.Check(kuki != nil && bytes.Equal(kuki.payload, want.cookie), "cookie not stored verbatim")
	} else {
		c.Check(kuki == nil, "unexpected kuki chunk")
	}

	// pakt fixed header, then one independent varint per packet.
	pp := pakt.payload
	c.Check(len(pp) >= 24, "pakt payload too short")
	numPackets := int64(beUint64(pp[0:8]))
	validFrames := int64(beUint64(pp[8:16]))
	priming := int32(beUint32(pp[16:20]))
	remainder := int32(beUint32(pp[20:24]))
	c.Check(numPackets == int64(len(want.packets)), "pakt packet count %d want %d", numPackets, len(want.packets))
	c.Check(validFrames == int64(len(want.packets))*want.fpp, "valid frames %d want %d",
		validFrames, int64(len(want.packets))*want.fpp)
	c.Check(priming == 0 && remainder == 0, "priming/remainder must be zero")
	off := 24
	var lengths []uint64
	var varintBytes int64
	for i := 0; i < len(want.packets); i++ {
		v, n, err := decodeVarInt(pp[off:])
		c.Check(err == nil, "pakt varint %d: %v", i, err)
		if err != nil {
			break
		}
		c.Check(v == uint64(len(want.packets[i])), "packet %d length %d want %d", i, v, len(want.packets[i]))
		lengths = append(lengths, v)
		varintBytes += int64(n)
		off += n
	}
	c.Check(off == len(pp), "pakt payload has %d trailing bytes", len(pp)-off)
	c.Check(pakt.size == 24+varintBytes, "pakt chunk size %d excludes free, computed %d", pakt.size, 24+varintBytes)
	var sum uint64
	for _, v := range lengths {
		sum += v
	}

	// free: at least a 12-byte header, zero payload, adjacent to pakt,
	// and the reserved region spans exactly pakt + free up to data.
	c.Check(free.headerOff == pakt.headerOff+12+pakt.size, "free is not adjacent to pakt")
	c.Check(free.size >= 0, "free size negative")
	c.Check(12+free.size >= 12, "free must keep its chunk header")
	c.Check(bytes.Equal(free.payload, make([]byte, len(free.payload))), "free payload not zeroed")
	c.Check(data.headerOff == pakt.headerOff+want.reserve, "data at %d want %d (reserve %d)",
		data.headerOff, pakt.headerOff+want.reserve, want.reserve)
	c.Check(free.headerOff+12+free.size == data.headerOff, "free does not fill reserved region")

	// data: 4-byte edit count then the real audio bytes, nothing after it.
	dp := data.payload
	c.Check(len(dp) >= 4, "data payload too short")
	c.Check(beUint32(dp[0:4]) == want.editCount, "edit count %d want %d", beUint32(dp[0:4]), want.editCount)
	audio := dp[4:]
	c.Check(uint64(len(audio)) == sum, "audio length %d does not match pakt sum %d", len(audio), sum)
	c.Check(data.size == 4+int64(len(audio)), "data chunk size %d want %d", data.size, 4+len(audio))
	var concat []byte
	for _, p := range want.packets {
		concat = append(concat, p...)
	}
	c.Check(bytes.Equal(audio, concat), "audio bytes differ from the appended packets")
	c.Check(data.headerOff+12+data.size == int64(len(b)), "file has trailing bytes or RIFF-style padding")

	// The same file must be readable by the existing File.Decode.
	var f File
	c.Check(f.Decode(bytes.NewReader(b)) == nil, "File.Decode rejected recorder output")
	var gotFormat *AudioFormat
	var gotData *Data
	var gotTable *PacketTable
	for _, ch := range f.Chunks {
		switch v := ch.Contents.(type) {
		case *AudioFormat:
			gotFormat = v
		case *Data:
			gotData = v
		case *PacketTable:
			gotTable = v
		}
	}
	c.Check(gotFormat != nil && gotData != nil && gotTable != nil, "decoded file misses chunks")
	if gotFormat != nil {
		c.Check(*gotFormat == want.format, "decoded desc differs")
	}
	if gotTable != nil {
		c.Check(gotTable.Header.NumberPackets == int64(len(want.packets)), "decoded packet count")
		c.Check(gotTable.Header.NumberValidFrames == int64(len(want.packets))*want.fpp, "decoded frame count")
		c.Check(len(gotTable.Entry) == len(want.packets), "decoded entry count")
	}
	if gotData != nil {
		c.Check(gotData.EditCount == want.editCount, "decoded edit count")
		c.Check(bytes.Equal(gotData.Data, concat), "decoded audio differs")
		// Split the decoded audio using the decoded table and compare packets.
		pos := 0
		for i, p := range want.packets {
			if i >= len(gotTable.Entry) {
				break
			}
			n := int(gotTable.Entry[i])
			c.Check(pos+n <= len(gotData.Data), "packet %d overruns decoded audio", i)
			if pos+n <= len(gotData.Data) {
				c.Check(bytes.Equal(gotData.Data[pos:pos+n], p), "decoded packet %d differs", i)
			}
			pos += n
		}
		c.Check(pos == len(gotData.Data), "decoded packet spans do not cover audio")
	}
}

func beUint32(b []byte) uint32 {
	return uint32(b[0])<<24 | uint32(b[1])<<16 | uint32(b[2])<<8 | uint32(b[3])
}
func beUint64(b []byte) uint64 {
	var v uint64
	for _, by := range b {
		v = v<<8 | uint64(by)
	}
	return v
}
func beFloat64(b []byte) float64 { return math.Float64frombits(beUint64(b)) }

func readBE(r io.Reader, data interface{}) error {
	return binary.Read(r, binary.BigEndian, data)
}

// --- fake recording file with bounded reads and failure injection ----------

type fakeFile struct {
	data       []byte
	pos        int64
	maxRead    int
	readCalls  int
	writeCalls int
	seekCalls  int
	truncCalls int
	totalCalls int
	maxReadLen int
	inject     func(op string, off, n int64) error
	shortWrite bool
	closed     bool
}

func (f *fakeFile) accesses() int { return f.totalCalls }

func (f *fakeFile) maybeFail(op string, off, n int64) error {
	f.totalCalls++
	if f.inject != nil {
		return f.inject(op, off, n)
	}
	return nil
}

func (f *fakeFile) Read(p []byte) (int, error) {
	f.readCalls++
	if err := f.maybeFail("read", f.pos, int64(len(p))); err != nil {
		return 0, err
	}
	if f.pos >= int64(len(f.data)) {
		return 0, io.EOF
	}
	n := len(p)
	if f.maxRead > 0 && n > f.maxRead {
		n = f.maxRead
	}
	if int64(n) > int64(len(f.data))-f.pos {
		n = int(int64(len(f.data)) - f.pos)
	}
	if n > f.maxReadLen {
		f.maxReadLen = n
	}
	copy(p, f.data[f.pos:f.pos+int64(n)])
	f.pos += int64(n)
	return n, nil
}

func (f *fakeFile) Write(p []byte) (int, error) {
	f.writeCalls++
	if err := f.maybeFail("write", f.pos, int64(len(p))); err != nil {
		return 0, err
	}
	if f.shortWrite && len(p) > 1 {
		n := 1
		f.grow(f.pos + int64(n))
		copy(f.data[f.pos:], p[:n])
		f.pos += int64(n)
		return n, nil
	}
	f.grow(f.pos + int64(len(p)))
	copy(f.data[f.pos:], p)
	f.pos += int64(len(p))
	return len(p), nil
}

func (f *fakeFile) grow(end int64) {
	if end > int64(len(f.data)) {
		f.data = append(f.data, make([]byte, end-int64(len(f.data)))...)
	}
}

func (f *fakeFile) Seek(off int64, whence int) (int64, error) {
	f.seekCalls++
	if err := f.maybeFail("seek", off, int64(whence)); err != nil {
		return 0, err
	}
	var target int64
	switch whence {
	case io.SeekStart:
		target = off
	case io.SeekCurrent:
		target = f.pos + off
	case io.SeekEnd:
		target = int64(len(f.data)) + off
	default:
		return 0, errors.New("bad whence")
	}
	if target < 0 {
		return 0, errors.New("negative seek")
	}
	f.pos = target
	return target, nil
}

func (f *fakeFile) Truncate(size int64) error {
	f.truncCalls++
	if err := f.maybeFail("truncate", size, 0); err != nil {
		return err
	}
	if size < 0 {
		return errors.New("negative truncate")
	}
	switch {
	case size == int64(len(f.data)):
	case size < int64(len(f.data)):
		f.data = f.data[:size]
		if f.pos > size {
			f.pos = size
		}
	default:
		f.grow(size)
	}
	return nil
}

func newFake() *fakeFile { return &fakeFile{maxRead: moveBufferSize} }

func opusFormat(channels uint32) AudioFormat {
	return AudioFormat{
		SampleRate:        48000,
		FormatID:          FourByteString{'o', 'p', 'u', 's'},
		FramesPerPacket:   960,
		ChannelsPerPacket: channels,
	}
}

// opusSilence returns a self-built legal Opus packet: CELT-only 20 ms
// full-band TOC (config 31), one code-0 frame, which decodes as silence.
func opusSilence(channels uint32) []byte {
	if channels == 2 {
		return []byte{0xFC}
	}
	return []byte{0xF8}
}

func mustRecorder(t *testing.T, f RecordingFile, format AudioFormat, cookie []byte, reserve int64) *Recorder {
	t.Helper()
	r, err := NewRecorder(f, format, cookie, reserve)
	if err != nil {
		t.Fatalf("NewRecorder: %v", err)
	}
	return r
}

// --- TestRecorderLayout -----------------------------------------------------

func TestRecorderLayout(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		format := opusFormat(1)

		// Invalid parameters must error without touching the file.
		bad := AudioFormat{SampleRate: 48000, FormatID: format.FormatID, FramesPerPacket: 960, ChannelsPerPacket: 1}
		bad.SampleRate = 0
		_, err := NewRecorder(newFake(), bad, nil, 64)
		c.Check(err != nil, "zero sample rate accepted")
		bad.SampleRate = math.NaN()
		_, err = NewRecorder(newFake(), bad, nil, 64)
		c.Check(err != nil, "NaN sample rate accepted")
		bad.SampleRate = math.Inf(1)
		_, err = NewRecorder(newFake(), bad, nil, 64)
		c.Check(err != nil, "+Inf sample rate accepted")
		bad.SampleRate = 48000
		bad.BytesPerPacket = 1
		_, err = NewRecorder(newFake(), bad, nil, 64)
		c.Check(err != nil, "non-zero BytesPerPacket accepted")
		bad.BytesPerPacket = 0
		bad.FramesPerPacket = 0
		_, err = NewRecorder(newFake(), bad, nil, 64)
		c.Check(err != nil, "zero FramesPerPacket accepted")
		bad.FramesPerPacket = 960
		bad.ChannelsPerPacket = 3
		_, err = NewRecorder(newFake(), bad, nil, 64)
		c.Check(err != nil, "three channels accepted")
		_, err = NewRecorder(newFake(), format, nil, 63)
		c.Check(err != nil, "reserve 63 accepted")
		_, err = NewRecorder(newFake(), format, nil, 0)
		c.Check(err != nil, "reserve 0 accepted")
		_, err = NewRecorder(newFake(), format, nil, -1)
		c.Check(err != nil, "negative reserve accepted")
		_, err = NewRecorder(nil, format, nil, 64)
		c.Check(err != nil, "nil file accepted")

		nonEmpty := newFake()
		nonEmpty.data = []byte{1, 2, 3, 4}
		before := append([]byte(nil), nonEmpty.data...)
		_, err = NewRecorder(nonEmpty, format, nil, 64)
		c.Check(err != nil, "non-empty file accepted")
		c.Check(bytes.Equal(nonEmpty.data, before), "non-empty file was modified")
		c.Check(nonEmpty.accesses() == 1, "rejection did more than one seek, got %d calls", nonEmpty.accesses())

		// Construction leaves a valid zero-packet CAF, reserve rounded to 64.
		fk := newFake()
		mustRecorder(t, fk, format, nil, 64)
		verifyLayout(c, fk.data, expectedLayout{format: format, fpp: 960, reserve: 64})

		// Rounding up and optional cookie.
		fk2 := newFake()
		cookie := []byte("magic-cookie-bytes")
		rec2 := mustRecorder(t, fk2, format, cookie, 65)
		c.Check(rec2 != nil, "recorder with cookie")
		verifyLayout(c, fk2.data, expectedLayout{
			format: format, cookie: cookie, hasCookie: true, fpp: 960, reserve: 128,
		})
		// Empty cookie means no kuki chunk.
		fk3 := newFake()
		mustRecorder(t, fk3, format, []byte{}, 64)
		_, chunks, err := walkCAF(fk3.data)
		c.Check(err == nil, "walk empty-cookie file: %v", err)
		for _, ch := range chunks {
			c.Check(ch.typ != ChunkTypeMagicCookie, "empty cookie produced a kuki chunk")
		}

		// Many checkpoints force repeated reserve doubling (64->128->...->1024).
		// 264 is a multiple of the checkpoint interval so the loop ends with
		// every packet committed, making the later no-op checkpoints real.
		fk4 := newFake()
		rec4 := mustRecorder(t, fk4, format, nil, 64)
		var packets [][]byte
		for i := 0; i < 264; i++ {
			p := []byte{byte(i), byte(i + 1), byte(i + 2)} // odd 3-byte packets
			c.Check(rec4.AppendPacket(p) == nil, "append %d", i)
			packets = append(packets, append([]byte(nil), p...))
			if (i+1)%8 == 0 {
				c.Check(rec4.Checkpoint() == nil, "checkpoint %d", i)
				verifyLayout(c, fk4.data, expectedLayout{
					format: format, fpp: 960, packets: packets,
					editCount: uint32((i + 1) / 8), reserve: observedReserve(fk4.data),
				})
			}
		}

		// No-op checkpoints are byte-for-byte identical, EditCount included.
		snapshot := append([]byte(nil), fk4.data...)
		c.Check(rec4.Checkpoint() == nil, "no-op checkpoint")
		c.Check(bytes.Equal(fk4.data, snapshot), "no-op checkpoint changed bytes")
		c.Check(rec4.Checkpoint() == nil, "second no-op checkpoint")
		c.Check(bytes.Equal(fk4.data, snapshot), "repeated no-op checkpoint changed bytes")

		// The caller's slice is not retained: mutate after append, then commit.
		mut := []byte{9, 8, 7, 6, 5}
		c.Check(rec4.AppendPacket(mut) == nil, "append mutable packet")
		for i := range mut {
			mut[i] = 0xFF
		}
		packets = append(packets, []byte{9, 8, 7, 6, 5})
		c.Check(rec4.Checkpoint() == nil, "checkpoint after mutation")
		verifyLayout(c, fk4.data, expectedLayout{
			format: format, fpp: 960, packets: packets,
			editCount: 34, reserve: observedReserve(fk4.data),
		})

		// Empty packet is rejected before any I/O and the session stays usable.
		callsBefore := fk4.accesses()
		c.Check(errors.Is(rec4.AppendPacket(nil), ErrEmptyPacket), "empty packet not rejected")
		c.Check(fk4.accesses() == callsBefore, "empty packet performed I/O")
		c.Check(rec4.AppendPacket(opusSilence(1)) == nil, "session usable after parameter error")
		packets = append(packets, opusSilence(1))

		// Close commits, forbids further appends and is idempotent. The
		// underlying file stays open and owned by the caller.
		c.Check(rec4.Close() == nil, "close")
		verifyLayout(c, fk4.data, expectedLayout{
			format: format, fpp: 960, packets: packets,
			editCount: 35, reserve: observedReserve(fk4.data),
		})
		c.Check(errors.Is(rec4.AppendPacket(opusSilence(1)), ErrRecorderClosed), "append after close")
		c.Check(rec4.Close() == nil, "double close")
		_, probeErr := fk4.Write([]byte{1})
		c.Check(probeErr == nil, "underlying file was closed by Recorder")
	})
}

// observedReserve walks the file and derives the current pakt/free reserve.
func observedReserve(b []byte) int64 {
	_, chunks, err := walkCAF(b)
	if err != nil {
		return -1
	}
	var paktOff, dataOff int64 = -1, -1
	for _, ch := range chunks {
		switch ch.typ {
		case ChunkTypePacketTable:
			paktOff = ch.headerOff
		case ChunkTypeAudioData:
			dataOff = ch.headerOff
		}
	}
	if paktOff < 0 || dataOff < 0 {
		return -1
	}
	return dataOff - paktOff
}

// --- TestRecorderContinue ---------------------------------------------------

func TestRecorderContinue(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Real compressed packets taken from the shipped sample.
		raw, err := ioutil.ReadFile(filepath.Join("samples", "helenkane.caf"))
		c.Check(err == nil, "read sample: %v", err)
		var sample File
		c.Check(sample.Decode(bytes.NewReader(raw)) == nil, "decode sample")
		var sampleFormat *AudioFormat
		var sampleData *Data
		var sampleTable *PacketTable
		for _, ch := range sample.Chunks {
			switch v := ch.Contents.(type) {
			case *AudioFormat:
				sampleFormat = v
			case *Data:
				sampleData = v
			case *PacketTable:
				sampleTable = v
			}
		}
		c.Check(sampleFormat != nil && sampleData != nil && sampleTable != nil, "sample chunks missing")
		if sampleFormat == nil || sampleData == nil || sampleTable == nil {
			return
		}
		var realPackets [][]byte
		pos := 0
		for _, n := range sampleTable.Entry {
			realPackets = append(realPackets, append([]byte(nil), sampleData.Data[pos:pos+int(n)]...))
			pos += int(n)
		}

		// Stream the real packets in several batches, checkpointing between
		// them and decoding through File.Decode after every checkpoint.
		fk := newFake()
		rec := mustRecorder(t, fk, *sampleFormat, nil, 64)
		batches := []int{1, 7, 50, 200, 1000}
		done := 0
		for bi, count := range batches {
			for j := 0; j < count && done < len(realPackets); j++ {
				c.Check(rec.AppendPacket(realPackets[done]) == nil, "append real packet %d", done)
				done++
			}
			c.Check(rec.Checkpoint() == nil, "checkpoint batch %d", bi)
			verifyLayout(c, fk.data, expectedLayout{
				format:    *sampleFormat,
				fpp:       int64(sampleFormat.FramesPerPacket),
				packets:   realPackets[:done],
				editCount: uint32(bi + 1),
				reserve:   observedReserve(fk.data),
			})
		}

		// A large audio body forces an overlapping backward move with a
		// non-cache-sized tail: >100 KiB moves through 32 KiB windows.
		big := newFake()
		bigRec := mustRecorder(t, big, opusFormat(2), nil, 64)
		var bigPackets [][]byte
		for i := 0; i < 1500; i++ {
			p := make([]byte, 100+(i%3)) // 100..102 bytes, includes odd sizes
			for k := range p {
				p[k] = byte((i*7 + k) & 0xff)
			}
			c.Check(bigRec.AppendPacket(p) == nil, "big append %d", i)
			bigPackets = append(bigPackets, p)
		}
		c.Check(bigRec.Checkpoint() == nil, "big checkpoint with move")
		c.Check(big.maxReadLen <= moveBufferSize, "move read %d exceeded %d", big.maxReadLen, moveBufferSize)
		c.Check(big.maxReadLen == moveBufferSize, "move never used the full 32 KiB window")
		verifyLayout(c, big.data, expectedLayout{
			format: opusFormat(2), fpp: 960, packets: bigPackets,
			editCount: 1, reserve: observedReserve(big.data),
		})
		c.Check(observedReserve(big.data) > 64, "reserve never grew")

		// Continue appending after the move: tail cursor must be exact, with
		// no gap or overwrite between the moved audio and new packets.
		for i := 0; i < 333; i++ {
			p := []byte{0xAB, byte(i), byte(i >> 8), 0xCD}
			c.Check(bigRec.AppendPacket(p) == nil, "append after move %d", i)
			bigPackets = append(bigPackets, p)
		}
		c.Check(bigRec.Checkpoint() == nil, "checkpoint after move")
		verifyLayout(c, big.data, expectedLayout{
			format: opusFormat(2), fpp: 960, packets: bigPackets,
			editCount: 2, reserve: observedReserve(big.data),
		})

		// Interleave decode, input-slice mutation and more appends.
		var decoded File
		c.Check(decoded.Decode(bytes.NewReader(big.data)) == nil, "decode between batches")
		shared := []byte{1, 2, 3, 4, 5, 6, 7}
		c.Check(bigRec.AppendPacket(shared) == nil, "append shared slice")
		for i := range shared {
			shared[i] = 0
		}
		bigPackets = append(bigPackets, []byte{1, 2, 3, 4, 5, 6, 7})
		c.Check(bigRec.Close() == nil, "final close")
		verifyLayout(c, big.data, expectedLayout{
			format: opusFormat(2), fpp: 960, packets: bigPackets,
			editCount: 3, reserve: observedReserve(big.data),
		})

		// On a real *os.File as well, the output decodes and frames match.
		dir := t.TempDir()
		path := filepath.Join(dir, "recorded.caf")
		osf, err := os.Create(path)
		c.Check(err == nil, "create temp file: %v", err)
		var _ RecordingFile = osf
		orec := mustRecorder(t, osf, opusFormat(1), []byte("kuki"), 64)
		for i := 0; i < 40; i++ {
			c.Check(orec.AppendPacket(opusSilence(1)) == nil, "os file append")
			if i%10 == 9 {
				c.Check(orec.Checkpoint() == nil, "os file checkpoint")
			}
		}
		c.Check(orec.Close() == nil, "os file close")
		c.Check(osf.Close() == nil, "caller closes underlying file")
		onDisk, err := ioutil.ReadFile(path)
		c.Check(err == nil, "read back: %v", err)
		var sil [][]byte
		for i := 0; i < 40; i++ {
			sil = append(sil, opusSilence(1))
		}
		verifyLayout(c, onDisk, expectedLayout{
			format: opusFormat(1), cookie: []byte("kuki"), hasCookie: true,
			fpp: 960, packets: sil, editCount: 4, reserve: observedReserve(onDisk),
		})
	})
}

// --- TestRecorderFailure ----------------------------------------------------

func TestRecorderFailure(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Build a session whose next checkpoint must grow the reserve and move
		// more than 100 KiB of audio, so the move spans multiple 32 KiB windows.
		setup := func() (*fakeFile, *Recorder, [][]byte) {
			fk := newFake()
			rec := mustRecorder(t, fk, opusFormat(1), nil, 64)
			var packets [][]byte
			for i := 0; i < 1500; i++ {
				p := make([]byte, 100+(i%2))
				for k := range p {
					p[k] = byte((i*13 + k) & 0xff)
				}
				if err := rec.AppendPacket(p); err != nil {
					t.Fatalf("setup append: %v", err)
				}
				packets = append(packets, p)
			}
			if err := rec.Checkpoint(); err != nil {
				t.Fatalf("setup checkpoint: %v", err)
			}
			// Add more packets so the following checkpoint needs another grow.
			for i := 0; i < 1500; i++ {
				p := make([]byte, 70)
				if err := rec.AppendPacket(p); err != nil {
					t.Fatalf("setup append 2: %v", err)
				}
				packets = append(packets, p)
			}
			return fk, rec, packets
		}

		assertDead := func(c *taskcheck.C, fk *fakeFile, rec *Recorder) {
			snapshot := fk.accesses()
			err := rec.AppendPacket(opusSilence(1))
			c.Check(errors.Is(err, ErrRecorderFailed), "append after failure: %v", err)
			err = rec.Checkpoint()
			c.Check(errors.Is(err, ErrRecorderFailed), "checkpoint after failure: %v", err)
			err = rec.Close()
			c.Check(errors.Is(err, ErrRecorderFailed), "close after failure: %v", err)
			c.Check(fk.accesses() == snapshot, "failed session did file I/O: %d > %d", fk.accesses(), snapshot)
			c.Check(rec.Close() != nil && rec.Checkpoint() != nil && rec.AppendPacket(opusSilence(1)) != nil,
				"failure state is not persistent")
			c.Check(fk.accesses() == snapshot, "retry after failure did file I/O")
		}

		// (a) Backward move read failure, on the second window so one window
		// has already been copied.
		fk, rec, packets := setup()
		oldDataOff := int64(-1)
		_, chunks, err := walkCAF(fk.data)
		c.Check(err == nil, "walk before failure: %v", err)
		for _, ch := range chunks {
			if ch.typ == ChunkTypeAudioData {
				oldDataOff = ch.headerOff
			}
		}
		moveReads := 0
		fk.inject = func(op string, off, n int64) error {
			if op == "read" && off >= oldDataOff {
				moveReads++
				if moveReads == 2 {
					return errors.New("injected move read failure")
				}
			}
			return nil
		}
		err = rec.Checkpoint()
		c.Check(err != nil && !errors.Is(err, ErrCapacity), "move read failure not reported: %v", err)
		assertDead(c, fk, rec)
		// Previously committed audio is still intact at its old location.
		var committed []byte
		for _, p := range packets[:1500] {
			committed = append(committed, p...)
		}
		c.Check(oldDataOff > 0, "old data offset found")
		audioOff := oldDataOff + 12 + 4
		c.Check(bytes.Equal(fk.data[audioOff:audioOff+int64(len(committed))], committed),
			"committed audio corrupted by failed move")

		// (b) Packet table write-back failure, armed only after the move ran.
		fk, rec, _ = setup()
		moveReads = 0
		fk.inject = func(op string, off, n int64) error {
			if op == "read" && off >= oldDataOff {
				moveReads++
			}
			if op == "write" && moveReads > 0 && off == rec.paktOffset {
				return errors.New("injected pakt write failure")
			}
			return nil
		}
		c.Check(rec.Checkpoint() != nil, "pakt write-back failure not reported")
		assertDead(c, fk, rec)

		// (c) Tail-cursor restore failure on the seek of the next append.
		fk, rec, _ = setup()
		tail := rec.writeCursor
		fk.inject = func(op string, off, n int64) error {
			if op == "seek" && off == tail {
				return errors.New("injected seek failure")
			}
			return nil
		}
		c.Check(rec.AppendPacket(opusSilence(1)) != nil, "tail seek failure not reported")
		assertDead(c, fk, rec)

		// (d) Truncate failure: the file size cannot change and the previously
		// committed file stays fully decodable.
		fk, rec, packets = setup()
		sizeBefore := len(fk.data)
		fk.inject = func(op string, off, n int64) error {
			if op == "truncate" {
				return errors.New("injected truncate failure")
			}
			return nil
		}
		c.Check(rec.Checkpoint() != nil, "truncate failure not reported")
		c.Check(len(fk.data) == sizeBefore, "file changed despite truncate failure")
		assertDead(c, fk, rec)
		var still File
		c.Check(still.Decode(bytes.NewReader(fk.data)) == nil, "committed file no longer decodes")
		var stillData *Data
		var stillTable *PacketTable
		for _, ch := range still.Chunks {
			switch v := ch.Contents.(type) {
			case *Data:
				stillData = v
			case *PacketTable:
				stillTable = v
			}
		}
		c.Check(stillData != nil && stillTable != nil && stillTable.Header.NumberPackets == 1500,
			"committed packet bookkeeping lost")
		var expect []byte
		for _, p := range packets[:1500] {
			expect = append(expect, p...)
		}
		c.Check(stillData != nil && bytes.Equal(stillData.Data, expect), "committed audio changed")

		// (e) An unreported short write is an I/O failure too.
		fk, rec, _ = setup()
		fk.shortWrite = true
		c.Check(rec.AppendPacket(make([]byte, 50)) != nil, "short write accepted")
		assertDead(c, fk, rec)

		// Parameter rejections never enter the failed state and never do I/O.
		fk, rec, _ = setup()
		calls := fk.accesses()
		c.Check(errors.Is(rec.AppendPacket(nil), ErrEmptyPacket), "empty packet")
		c.Check(fk.accesses() == calls, "empty packet did I/O")
		c.Check(rec.Checkpoint() == nil, "session still works after parameter error")
		c.Check(rec.Close() == nil, "clean close after parameter error")
	})
}
