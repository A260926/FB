package caf

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"
)

// RecordingFile is the storage backing a Recorder. It must be an empty,
// readable, writable and seekable file that can also be truncated.
// *os.File satisfies the interface.
type RecordingFile interface {
	io.Reader
	io.Writer
	io.Seeker
	Truncate(size int64) error
}

// Sentinels so callers can distinguish session state from parameter errors.
var (
	ErrRecorderClosed = errors.New("caf: recorder is closed")
	ErrRecorderFailed = errors.New("caf: recorder has failed")
	ErrEmptyPacket    = errors.New("caf: packet must not be empty")
	ErrPacketTooLarge = errors.New("caf: packet length does not fit in uint32")
	ErrCapacity       = errors.New("caf: packet count, frame count or file size limit exceeded")
	errShortWrite     = errors.New("caf: writer accepted fewer bytes than requested without an error")
)

const (
	fileHeaderSize     = int64(8)
	chunkHeaderSize    = int64(12)
	descPayloadSize    = int64(32)
	packetTableHdrSize = int64(24)
	editCountSize      = int64(4)
	minReserve         = int64(64)
	moveBufferSize     = 32 * 1024
)

// Recorder incrementally writes a single-track, variable bit rate CAF file.
// Audio packets are streamed straight into the file: only packet metadata
// (one length per packet) is kept in memory. The pakt chunk and a following
// free chunk share a reserved region in front of the data chunk; when the
// packet table outgrows it, the data chunk is moved farther back in place.
type Recorder struct {
	f      RecordingFile
	format AudioFormat
	cookie []byte
	fpp    int64

	// paktOffset is the offset of the pakt chunk header and never changes.
	// reserve covers the pakt chunk and the adjacent free chunk together.
	paktOffset  int64
	reserve     int64
	dataOffset  int64
	writeCursor int64 // absolute offset of the next audio byte

	packetCount      int64
	audioBytes       int64
	entries          []uint64
	committedPackets int64
	editCount        uint32

	closed bool
	failed bool
	err    error
}

// NewRecorder creates a valid zero-packet CAF file in f. The file must be
// empty. reserve is rounded up to a power of two (minimum 64) and is the
// size shared by the pakt and free chunks ahead of the data chunk.
// Invalid parameters or a non-empty file leave the file untouched.
func NewRecorder(f RecordingFile, format AudioFormat, cookie []byte, reserve int64) (*Recorder, error) {
	if f == nil {
		return nil, errors.New("caf: nil recording file")
	}
	if err := validateFormat(format); err != nil {
		return nil, err
	}
	if reserve < minReserve {
		return nil, fmt.Errorf("caf: reserve %d is below the minimum of %d", reserve, minReserve)
	}
	res, err := roundUpPowerOfTwo(reserve)
	if err != nil {
		return nil, err
	}
	end, err := f.Seek(0, io.SeekEnd)
	if err != nil {
		return nil, err
	}
	if end != 0 {
		return nil, errors.New("caf: recording file must be empty")
	}

	r := &Recorder{
		f:          f,
		format:     format,
		cookie:     append([]byte(nil), cookie...),
		fpp:        int64(format.FramesPerPacket),
		reserve:    res,
		paktOffset: preambleSize(len(cookie)),
	}
	r.dataOffset = r.paktOffset + r.reserve
	if err := r.writeInitialLayout(); err != nil {
		return nil, err
	}
	r.writeCursor = r.dataOffset + chunkHeaderSize + editCountSize
	return r, nil
}

func validateFormat(format AudioFormat) error {
	if math.IsNaN(format.SampleRate) || math.IsInf(format.SampleRate, 0) || format.SampleRate <= 0 {
		return errors.New("caf: sample rate must be finite and positive")
	}
	if format.BytesPerPacket != 0 {
		return errors.New("caf: BytesPerPacket must be 0 for variable bit rate audio")
	}
	if format.FramesPerPacket == 0 {
		return errors.New("caf: FramesPerPacket must be greater than 0")
	}
	if format.ChannelsPerPacket != 1 && format.ChannelsPerPacket != 2 {
		return errors.New("caf: ChannelsPerPacket must be 1 or 2")
	}
	return nil
}

// preambleSize is the size of the caff header, desc chunk and optional kuki.
func preambleSize(cookieLen int) int64 {
	size := fileHeaderSize + chunkHeaderSize + descPayloadSize
	if cookieLen > 0 {
		size += chunkHeaderSize + int64(cookieLen)
	}
	return size
}

func roundUpPowerOfTwo(v int64) (int64, error) {
	if v <= 0 {
		return 0, errors.New("caf: reserve must be positive")
	}
	p := int64(1)
	for p < v {
		if p > math.MaxInt64/2 {
			return 0, fmt.Errorf("%w: reserve %d too large", ErrCapacity, v)
		}
		p <<= 1
	}
	return p, nil
}

func (r *Recorder) fail(err error) error {
	r.failed = true
	if r.err == nil {
		r.err = err
	}
	return err
}

func (r *Recorder) liveErr() error {
	if r.failed {
		return fmt.Errorf("%w: %v", ErrRecorderFailed, r.err)
	}
	return nil
}

// writeFull writes all of p in one call. A short write reported without an
// error is treated as an I/O failure instead of being retried.
func writeFull(w io.Writer, p []byte) error {
	if len(p) == 0 {
		return nil
	}
	n, err := w.Write(p)
	if err != nil {
		return err
	}
	if n != len(p) {
		return errShortWrite
	}
	return nil
}

func (r *Recorder) seek(off int64, whence int) (int64, error) {
	got, err := r.f.Seek(off, whence)
	if err != nil {
		return 0, r.fail(err)
	}
	return got, nil
}

func (r *Recorder) writeAt(p []byte, off int64) error {
	if _, err := r.seek(off, io.SeekStart); err != nil {
		return err
	}
	if err := writeFull(r.f, p); err != nil {
		return r.fail(err)
	}
	return nil
}

// readAt fills p completely; a short read is an I/O failure.
func (r *Recorder) readAt(p []byte, off int64) error {
	if _, err := r.seek(off, io.SeekStart); err != nil {
		return err
	}
	if _, err := io.ReadFull(r.f, p); err != nil {
		return r.fail(err)
	}
	return nil
}

func (r *Recorder) truncate(size int64) error {
	if err := r.f.Truncate(size); err != nil {
		return r.fail(err)
	}
	return nil
}

// zeroFill writes length zero bytes at off in bounded chunks so the working
// set never depends on the region size.
func (r *Recorder) zeroFill(off, length int64) error {
	zero := make([]byte, moveBufferSize)
	for length > 0 {
		n := length
		if n > int64(len(zero)) {
			n = int64(len(zero))
		}
		if err := r.writeAt(zero[:n], off); err != nil {
			return err
		}
		off += n
		length -= n
	}
	return nil
}

func (r *Recorder) writeInitialLayout() error {
	var buf bytes.Buffer
	hdr := FileHeader{FileType: stringToChunkType("caff"), FileVersion: 1}
	if err := hdr.Encode(&buf); err != nil {
		return r.fail(err)
	}
	desc := Chunk{
		Header:   ChunkHeader{ChunkType: ChunkTypeAudioDescription, ChunkSize: descPayloadSize},
		Contents: &r.format,
	}
	if err := desc.Encode(&buf); err != nil {
		return r.fail(err)
	}
	if len(r.cookie) > 0 {
		kuki := Chunk{
			Header:   ChunkHeader{ChunkType: ChunkTypeMagicCookie, ChunkSize: int64(len(r.cookie))},
			Contents: &UnknownContents{Data: r.cookie},
		}
		if err := kuki.Encode(&buf); err != nil {
			return r.fail(err)
		}
	}
	if int64(buf.Len()) != r.paktOffset {
		return r.fail(fmt.Errorf("caf: internal preamble size mismatch: %d != %d", buf.Len(), r.paktOffset))
	}
	if err := r.writeAt(buf.Bytes(), 0); err != nil {
		return err
	}

	// Empty packet table: 24-byte header, no entries.
	table := &PacketTable{}
	paktPayload, err := table.EncodedSize()
	if err != nil {
		return r.fail(err)
	}
	if err := r.writePacketTable(table, paktPayload); err != nil {
		return err
	}

	// The free chunk consumes the rest of the reserved region.
	freePayload := r.reserve - chunkHeaderSize - paktPayload - chunkHeaderSize
	if freePayload < 0 {
		return r.fail(ErrCapacity)
	}
	if err := r.writeFreeChunk(paktPayload, freePayload); err != nil {
		return err
	}

	dataHdr := ChunkHeader{ChunkType: ChunkTypeAudioData, ChunkSize: editCountSize}
	if err := r.writeChunkHeader(r.dataOffset, dataHdr); err != nil {
		return err
	}
	if err := r.writeEditCount(0); err != nil {
		return err
	}
	return nil
}

func (r *Recorder) writeChunkHeader(off int64, hdr ChunkHeader) error {
	var buf bytes.Buffer
	if err := binary.Write(&buf, binary.BigEndian, &hdr); err != nil {
		return r.fail(err)
	}
	return r.writeAt(buf.Bytes(), off)
}

// writePacketTable writes the pakt chunk via the shared PacketTable encoder.
func (r *Recorder) writePacketTable(table *PacketTable, payloadSize int64) error {
	var buf bytes.Buffer
	chunk := Chunk{
		Header:   ChunkHeader{ChunkType: ChunkTypePacketTable, ChunkSize: payloadSize},
		Contents: table,
	}
	if err := chunk.Encode(&buf); err != nil {
		return r.fail(err)
	}
	return r.writeAt(buf.Bytes(), r.paktOffset)
}

func (r *Recorder) writeFreeChunk(paktPayload, payloadSize int64) error {
	off := r.paktOffset + chunkHeaderSize + paktPayload
	if err := r.writeChunkHeader(off, ChunkHeader{ChunkType: ChunkTypeFree, ChunkSize: payloadSize}); err != nil {
		return err
	}
	return r.zeroFill(off+chunkHeaderSize, payloadSize)
}

func (r *Recorder) writeEditCount(count uint32) error {
	var buf [4]byte
	binary.BigEndian.PutUint32(buf[:], count)
	return r.writeAt(buf[:], r.dataOffset+chunkHeaderSize)
}

// AppendPacket writes one complete, self-contained compressed packet to the
// data chunk. Only the packet length is retained. The packet is fully on
// disk before the in-memory ledgers are updated; invalid packets are rejected
// before any I/O and do not disturb the session.
func (r *Recorder) AppendPacket(p []byte) error {
	if err := r.liveErr(); err != nil {
		return err
	}
	if r.closed {
		return ErrRecorderClosed
	}
	if len(p) == 0 {
		return ErrEmptyPacket
	}
	if uint64(len(p)) > math.MaxUint32 {
		return ErrPacketTooLarge
	}
	if r.packetCount >= math.MaxInt64 {
		return ErrCapacity
	}
	// Frames per packet are fixed, so the frame total must stay representable.
	if r.packetCount > (math.MaxInt64-r.fpp)/r.fpp {
		return fmt.Errorf("%w: valid frame count", ErrCapacity)
	}
	n := int64(len(p))
	if r.audioBytes > math.MaxInt64-n {
		return fmt.Errorf("%w: audio byte count", ErrCapacity)
	}
	newEnd := r.dataOffset + chunkHeaderSize + editCountSize + r.audioBytes + n
	if newEnd < 0 || newEnd > math.MaxInt64 {
		return fmt.Errorf("%w: file offset", ErrCapacity)
	}

	if err := r.writeAt(p, r.writeCursor); err != nil {
		return err
	}
	// Commit only after the complete packet has been written.
	r.writeCursor += n
	r.audioBytes += n
	r.entries = append(r.entries, uint64(n))
	r.packetCount++
	return nil
}

// Checkpoint rebuilds the pakt chunk for every packet written so far and
// updates the data chunk size and edit count. A checkpoint without new
// packets performs no I/O and leaves the file byte-for-byte unchanged.
func (r *Recorder) Checkpoint() error {
	if err := r.liveErr(); err != nil {
		return err
	}
	if r.closed {
		// No further packets can be appended, so nothing is pending.
		return nil
	}
	if r.packetCount == r.committedPackets {
		return nil
	}

	table := r.buildTable()
	payloadSize, err := table.EncodedSize()
	if err != nil {
		// Encoding problem: reject before touching the file.
		return fmt.Errorf("%w: packet table", ErrCapacity)
	}
	if payloadSize < 0 || chunkHeaderSize+payloadSize > math.MaxInt64-chunkHeaderSize {
		return fmt.Errorf("%w: packet table size", ErrCapacity)
	}
	if r.editCount >= math.MaxUint32 {
		return fmt.Errorf("%w: edit count", ErrCapacity)
	}

	// The reserved region must hold the pakt chunk and a free chunk header.
	if r.reserve < chunkHeaderSize+payloadSize+chunkHeaderSize {
		if err := r.growReserve(chunkHeaderSize + payloadSize); err != nil {
			return err
		}
	}

	if err := r.writePacketTable(table, payloadSize); err != nil {
		return err
	}
	freePayload := r.reserve - chunkHeaderSize - payloadSize - chunkHeaderSize
	if err := r.writeFreeChunk(payloadSize, freePayload); err != nil {
		return err
	}
	if err := r.writeChunkHeader(r.dataOffset, ChunkHeader{
		ChunkType: ChunkTypeAudioData,
		ChunkSize: editCountSize + r.audioBytes,
	}); err != nil {
		return err
	}
	r.editCount++
	if err := r.writeEditCount(r.editCount); err != nil {
		return err
	}

	r.committedPackets = r.packetCount
	return nil
}

// Close commits any pending packets and seals the session. It is idempotent;
// the underlying file remains owned by the caller.
func (r *Recorder) Close() error {
	if err := r.liveErr(); err != nil {
		return err
	}
	if r.closed {
		return nil
	}
	if err := r.Checkpoint(); err != nil {
		return err
	}
	r.closed = true
	return nil
}

func (r *Recorder) buildTable() *PacketTable {
	entries := make([]uint64, len(r.entries))
	copy(entries, r.entries)
	return &PacketTable{
		Header: PacketTableHeader{
			NumberPackets:     r.packetCount,
			NumberValidFrames: r.packetCount * r.fpp,
		},
		Entry: entries,
	}
}

// growReserve doubles the reserved region until paktChunkSize plus a free
// chunk header fit, then moves the existing data chunk (header, edit count
// and audio) backward in the same file using a bounded move buffer.
func (r *Recorder) growReserve(paktChunkSize int64) error {
	for r.reserve < paktChunkSize+chunkHeaderSize {
		doubled, err := doubleReserve(r.reserve)
		if err != nil {
			return err
		}
		r.reserve = doubled
	}

	oldData := r.dataOffset
	newData := r.paktOffset + r.reserve
	delta := newData - oldData

	end, err := r.seek(0, io.SeekEnd)
	if err != nil {
		return err
	}
	if end < oldData || delta > math.MaxInt64-end {
		return fmt.Errorf("%w: relocated file size", ErrCapacity)
	}
	// Extend first so the backward copy always has room to land.
	if err := r.truncate(end + delta); err != nil {
		return err
	}
	if err := r.moveRangeBackward(oldData, end, delta); err != nil {
		return err
	}
	r.dataOffset = newData
	r.writeCursor += delta
	// The vacated span is part of the free chunk and must stay zeroed.
	if err := r.zeroFill(oldData, delta); err != nil {
		return err
	}
	return nil
}

func doubleReserve(v int64) (int64, error) {
	if v > math.MaxInt64/2 {
		return 0, fmt.Errorf("%w: reserved region", ErrCapacity)
	}
	return v << 1, nil
}

// moveRangeBackward copies [srcStart, srcEnd) to [srcStart+delta, srcEnd+delta)
// high offsets first, which keeps overlapping source bytes intact. The tail
// chunk may be smaller than the move buffer.
func (r *Recorder) moveRangeBackward(srcStart, srcEnd, delta int64) error {
	buf := make([]byte, moveBufferSize)
	pos := srcEnd
	for pos > srcStart {
		n := pos - srcStart
		if n > int64(len(buf)) {
			n = int64(len(buf))
		}
		readOff := pos - n
		if err := r.readAt(buf[:n], readOff); err != nil {
			return err
		}
		if err := r.writeAt(buf[:n], readOff+delta); err != nil {
			return err
		}
		pos -= n
	}
	return nil
}
