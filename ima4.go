package aiff

import (
	"fmt"
	"io"

	"github.com/go-audio/audio"
)

// IMA4 (Apple IMA ADPCM, four bits per sample) constants:
//
//	one packet is 34 bytes: a 2 byte big endian header followed by 32
//	bytes holding 64 nibbles. The header stores a signed 9 bit predictor
//	and a 7 bit step index in bits 15..7 and 6..0 respectively.
const (
	ima4PacketBytes   = 34
	ima4SamplesPerPkt = 64
	ima4MaxIndex      = 88
)

// ima4StepTable and ima4IndexTable are the standard IMA step/index
// tables used by the Apple IMA4 (QT) codec.
var ima4StepTable = [89]int{
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
	34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
	157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
	724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
	3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
	15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
}

var ima4IndexTable = [16]int{
	-1, -1, -1, -1, 2, 4, 6, 8,
	-1, -1, -1, -1, 2, 4, 6, 8,
}

// ima4ChannelState is the full decoder state at the end of a packet.
type ima4ChannelState struct {
	predictor int
	index     int
}

// prepareIMA4Info computes the uncompressed lengths for the ima4 COMM
// values. NumSampleFrames keeps the raw compressed packet group count; the
// public frame/byte counts are derived from it.
func (d *Decoder) prepareIMA4Info() {
	d.ima4 = true
	d.ima4Chans = int(d.NumChans)
	d.ima4Groups = int64(d.NumSampleFrames)

	var ok bool
	d.ima4TotalFrames, ok = checkedMul64(d.ima4Groups, ima4SamplesPerPkt)
	if !ok {
		d.ima4MetaErr = fmt.Errorf("ima4: frame count overflows: %d groups", d.ima4Groups)
		return
	}
	d.ima4DataBytes, ok = checkedMul64(d.ima4Groups, int64(ima4PacketBytes*d.ima4Chans))
	if !ok {
		d.ima4MetaErr = fmt.Errorf("ima4: compressed size overflows: %d groups, %d channels", d.ima4Groups, d.ima4Chans)
		return
	}
	d.ima4PCMBytes, ok = checkedMul64(d.ima4TotalFrames, int64(d.ima4Chans))
	if ok {
		d.ima4PCMBytes, ok = checkedMul64(d.ima4PCMBytes, 2)
	}
	if !ok {
		d.ima4MetaErr = fmt.Errorf("ima4: PCM byte count overflows: %d frames, %d channels", d.ima4TotalFrames, d.ima4Chans)
		return
	}

	if d.BitDepth != 16 {
		d.ima4MetaErr = fmt.Errorf("ima4: unsupported bit depth %d, only 16-bit ima4 is supported", d.BitDepth)
		return
	}
	if d.ima4Chans != 1 && d.ima4Chans != 2 {
		d.ima4MetaErr = fmt.Errorf("ima4: unsupported channel count %d, only mono and stereo are supported", d.NumChans)
	}
}

// checkedMul64 multiplies two non-negative int64 values and reports whether
// the result is representable as an int64.
func checkedMul64(a, b int64) (int64, bool) {
	if a < 0 || b < 0 {
		return 0, false
	}
	if a == 0 || b == 0 {
		return 0, true
	}
	if a > (1<<63-1)/b {
		return 0, false
	}
	return a * b, true
}

// setupIMA4PCM parses the SSND offset/block size information and prepares
// the sequential packet reader. dataStart is the absolute offset of the
// 8-byte offset/block size fields (just past "SSND" and its size), rawSize is
// the SSND chunk size declared in the file (without the odd padding byte).
// The block size is only an alignment hint and is therefore ignored.
func (d *Decoder) setupIMA4PCM(dataStart int64, offset uint32, rawSize int64) error {
	d.ima4HaveChunkEnd = true
	d.ima4PCMStart = dataStart + 8 + int64(offset)
	d.ima4ChunkEnd = dataStart + rawSize + rawSize&1
	d.PCMSize = 0

	if d.ima4MetaErr != nil {
		return d.ima4MetaErr
	}
	if rawSize < 8 {
		return fmt.Errorf("ima4: SSND chunk too small for offset and block size: %d bytes", rawSize)
	}
	if int64(offset) > rawSize-8 {
		return fmt.Errorf("ima4: SSND offset %d out of bounds for a %d byte chunk", offset, rawSize)
	}

	avail := rawSize - 8 - int64(offset)
	d.ima4AvailBytes = avail
	d.ima4Remaining = d.ima4DataBytes
	if d.ima4Remaining > avail {
		// Don't read past the declared end of the sound data; the
		// following chunk must never be used to complete a packet.
		d.ima4Remaining = avail
	}
	return nil
}

// resetIMA4Stream clears the packet streaming state while keeping the
// container/COMM information.
func (d *Decoder) resetIMA4Stream() {
	d.ima4GroupsDecoded = 0
	d.ima4Cur = nil
	d.ima4CurPos = 0
	d.ima4State = [2]ima4ChannelState{}
	d.ima4HaveState = [2]bool{}
	d.ima4DecodeErr = nil
	d.ima4Remaining = d.ima4DataBytes
	if d.ima4Remaining > d.ima4AvailBytes {
		d.ima4Remaining = d.ima4AvailBytes
	}
}

// readIMA4Bytes reads exactly len(buf) bytes from the sequential SSND data
// region. Short reads and a final Read returning data together with io.EOF
// are handled without dropping the bytes that were actually returned.
func (d *Decoder) readIMA4Bytes(buf []byte) (int, error) {
	n := 0
	for n < len(buf) {
		m, err := d.r.Read(buf[n:])
		if m > 0 {
			n += m
			d.ima4Remaining -= int64(m)
			if d.ima4Remaining < 0 {
				d.ima4Remaining = 0
			}
		}
		if err != nil {
			if n == len(buf) {
				// The data is complete even if EOF accompanied it.
				return n, nil
			}
			if err == io.EOF {
				return n, io.ErrUnexpectedEOF
			}
			return n, err
		}
		if m == 0 {
			return n, io.ErrNoProgress
		}
	}
	return n, nil
}

// decodeIMA4Group reads and decodes one group of one packet per channel.
// A group is only published after both the left and right packets have been
// read completely and their headers validated.
func (d *Decoder) decodeIMA4Group() error {
	if d.ima4Chans < 1 || d.ima4Chans > 2 {
		return fmt.Errorf("ima4: unsupported channel count %d", d.ima4Chans)
	}
	groupBytes := ima4PacketBytes * d.ima4Chans
	if d.ima4Remaining < int64(groupBytes) {
		if d.ima4Chans == 2 && d.ima4Remaining >= ima4PacketBytes {
			return fmt.Errorf("ima4: right packet truncated: %d of %d needed bytes available", d.ima4Remaining, groupBytes)
		}
		return fmt.Errorf("ima4: packet group truncated: %d of %d needed bytes available", d.ima4Remaining, groupBytes)
	}

	buf := make([]byte, groupBytes)
	n, err := d.readIMA4Bytes(buf)
	if err != nil || n != groupBytes {
		if err == nil {
			err = io.ErrUnexpectedEOF
		}
		return fmt.Errorf("ima4: truncated packet group at group %d: %v", d.ima4GroupsDecoded, err)
	}

	// Validate every packet header before touching any state.
	indexes := make([]int, d.ima4Chans)
	predictors := make([]int, d.ima4Chans)
	for c := 0; c < d.ima4Chans; c++ {
		off := c * ima4PacketBytes
		h := uint16(buf[off])<<8 | uint16(buf[off+1])
		predictors[c] = ima4HeaderPredictor(h)
		indexes[c] = ima4HeaderIndex(h)
		if indexes[c] > ima4MaxIndex {
			return fmt.Errorf("ima4: invalid step index %d in channel %d (max %d)", indexes[c], c, ima4MaxIndex)
		}
	}

	decoded := make([][]int, d.ima4Chans)
	newState := make([]ima4ChannelState, d.ima4Chans)
	for c := 0; c < d.ima4Chans; c++ {
		predictor := predictors[c]
		index := indexes[c]
		if d.ima4HaveState[c] {
			prev := d.ima4State[c]
			diff := predictors[c] - prev.predictor
			if diff < 0 {
				diff = -diff
			}
			// Continue the previous packet only when the header agrees
			// with the state left behind by the previous packet.
			if prev.index == indexes[c] && diff <= 127 {
				predictor = prev.predictor
				index = prev.index
			}
		}

		out := make([]int, ima4SamplesPerPkt)
		off := c*ima4PacketBytes + 2
		for s := 0; s < ima4SamplesPerPkt; s++ {
			byteVal := buf[off+s/2]
			nibble := int(byteVal & 0x0f) // low nibble first
			if s%2 == 1 {
				nibble = int(byteVal >> 4)
			}
			predictor, index = ima4ExpandNibble(predictor, index, nibble)
			out[s] = predictor
		}
		decoded[c] = out
		newState[c] = ima4ChannelState{predictor: predictor, index: index}
	}

	// Both packets were valid: publish the interleaved frames and commit
	// the per-channel state.
	cur := make([]int, ima4SamplesPerPkt*d.ima4Chans)
	for s := 0; s < ima4SamplesPerPkt; s++ {
		for c := 0; c < d.ima4Chans; c++ {
			cur[s*d.ima4Chans+c] = decoded[c][s]
		}
	}
	for c := 0; c < d.ima4Chans; c++ {
		d.ima4State[c] = newState[c]
		d.ima4HaveState[c] = true
	}
	d.ima4Cur = cur
	d.ima4CurPos = 0
	d.ima4GroupsDecoded++
	return nil
}

// ima4HeaderPredictor returns the approximate full predictor stored in the
// packet header: the signed 9 most significant bits shifted into place. The
// 7 least significant bits of the real predictor are not stored.
func ima4HeaderPredictor(h uint16) int {
	p := int(h >> 7)
	if p >= 256 {
		p -= 512
	}
	return p << 7
}

func ima4HeaderIndex(h uint16) int {
	return int(h & 0x7f)
}

func ima4ExpandNibble(predictor, index, nibble int) (int, int) {
	step := ima4StepTable[index]
	diff := step >> 3
	if nibble&4 != 0 {
		diff += step
	}
	if nibble&2 != 0 {
		diff += step >> 1
	}
	if nibble&1 != 0 {
		diff += step >> 2
	}
	if nibble&8 != 0 {
		predictor -= diff
	} else {
		predictor += diff
	}
	if predictor > 32767 {
		predictor = 32767
	} else if predictor < -32768 {
		predictor = -32768
	}
	index += ima4IndexTable[nibble&7]
	if index < 0 {
		index = 0
	} else if index > ima4MaxIndex {
		index = ima4MaxIndex
	}
	return predictor, index
}

// ima4PCMBuffer fills buf with scalar samples from the sequential decoder
// state, decoding at most one packet group ahead.
func (d *Decoder) ima4PCMBuffer(buf *audio.IntBuffer) (int, error) {
	if d == nil || buf == nil {
		return 0, nil
	}
	if d.ima4MetaErr != nil {
		return 0, d.ima4MetaErr
	}
	if !d.ima4Ready {
		if d.ima4SetupErr != nil {
			return 0, d.ima4SetupErr
		}
		if err := d.FwdToPCM(); err != nil {
			return 0, err
		}
		if d.ima4SetupErr != nil {
			return 0, d.ima4SetupErr
		}
		if !d.ima4Ready {
			return 0, fmt.Errorf("ima4: PCM data is not initialized")
		}
	}
	buf.SourceBitDepth = 16
	buf.Format = &audio.Format{
		NumChannels: d.ima4Chans,
		SampleRate:  d.SampleRate,
	}

	n := 0
	for n < len(buf.Data) {
		if d.ima4CurPos >= len(d.ima4Cur) {
			if d.ima4DecodeErr != nil {
				return n, d.ima4DecodeErr
			}
			if d.ima4GroupsDecoded >= d.ima4Groups {
				// Declared packet groups are exhausted.
				return n, nil
			}
			if err := d.decodeIMA4Group(); err != nil {
				d.ima4DecodeErr = err
				return n, err
			}
			continue
		}
		buf.Data[n] = d.ima4Cur[d.ima4CurPos]
		d.ima4CurPos++
		n++
	}
	return n, nil
}

// ima4FullPCMBuffer reads the remainder of the ima4 stream from the current
// decoder position and returns all samples.
func (d *Decoder) ima4FullPCMBuffer() (*audio.IntBuffer, error) {
	format := &audio.Format{
		NumChannels: d.ima4Chans,
		SampleRate:  d.SampleRate,
	}
	out := &audio.IntBuffer{Format: format, SourceBitDepth: 16}
	if d.ima4MetaErr != nil {
		return out, d.ima4MetaErr
	}
	if !d.ima4Ready {
		if d.ima4SetupErr != nil {
			return out, d.ima4SetupErr
		}
		if err := d.FwdToPCM(); err != nil {
			return out, err
		}
		if d.ima4SetupErr != nil {
			return out, d.ima4SetupErr
		}
		if !d.ima4Ready {
			return out, fmt.Errorf("ima4: PCM data is not initialized")
		}
	}
	tmp := make([]int, 4096)
	for {
		b := &audio.IntBuffer{Format: format, Data: tmp}
		n, err := d.ima4PCMBuffer(b)
		if n > 0 {
			out.Data = append(out.Data, tmp[:n]...)
		}
		if err != nil {
			return out, err
		}
		if n == 0 {
			return out, nil
		}
	}
}

// SeekPCM positions the ima4 decoder at the given uncompressed frame.
// IMA4 packet headers don't retain the low bits of the predictor, so exact
// positioning replays the packet groups from the start of the sound data.
func (d *Decoder) SeekPCM(frame int64) error {
	if d == nil {
		return fmt.Errorf("ima4: can't seek a nil decoder")
	}
	if frame < 0 {
		return fmt.Errorf("ima4: negative frame %d", frame)
	}

	hadInfo := d.SampleRate > 0
	posBefore, posErr := d.r.Seek(0, io.SeekCurrent)

	d.ReadInfo()
	if d.SampleRate == 0 && d.err != nil {
		return fmt.Errorf("ima4: failed to read info - %v", d.err)
	}
	if d.ima4MetaErr != nil {
		d.restorePCMSeekFailure(hadInfo, posBefore, posErr)
		return d.ima4MetaErr
	}
	if !d.ima4 {
		d.restorePCMSeekFailure(hadInfo, posBefore, posErr)
		return fmt.Errorf("ima4: SeekPCM only supports ima4 encoded files")
	}
	if frame > d.ima4TotalFrames {
		d.restorePCMSeekFailure(hadInfo, posBefore, posErr)
		return fmt.Errorf("ima4: frame %d out of range, total frames %d", frame, d.ima4TotalFrames)
	}

	if !d.ima4Ready {
		if d.ima4SetupErr != nil {
			return d.ima4SetupErr
		}
		if err := d.FwdToPCM(); err != nil {
			return err
		}
		if d.ima4SetupErr != nil {
			return d.ima4SetupErr
		}
		if !d.ima4Ready {
			return fmt.Errorf("ima4: PCM data is not initialized")
		}
	}

	if frame == d.ima4TotalFrames {
		// Position at EOF. Everything is already discarded; a later valid
		// SeekPCM replays from the beginning.
		d.ima4Cur = nil
		d.ima4CurPos = 0
		d.ima4GroupsDecoded = d.ima4Groups
		d.ima4Remaining = 0
		d.ima4DecodeErr = nil
		if _, err := d.r.Seek(d.ima4ChunkEnd, io.SeekStart); err != nil {
			return err
		}
		return nil
	}

	targetGroup := frame / ima4SamplesPerPkt
	if targetGroup >= d.ima4Groups {
		return fmt.Errorf("ima4: frame %d out of range", frame)
	}
	if _, err := d.r.Seek(d.ima4PCMStart, io.SeekStart); err != nil {
		return err
	}
	d.resetIMA4Stream()
	for g := int64(0); g <= targetGroup; g++ {
		if err := d.decodeIMA4Group(); err != nil {
			d.ima4DecodeErr = err
			return err
		}
	}
	d.ima4CurPos = int(frame%ima4SamplesPerPkt) * d.ima4Chans
	return nil
}

// restorePCMSeekFailure puts back the pre-SeekPCM position when the decoder
// was not read yet. If the info had already been parsed ReadInfo is a no-op
// and the current position is untouched.
func (d *Decoder) restorePCMSeekFailure(hadInfo bool, posBefore int64, posErr error) {
	if hadInfo || posErr != nil {
		return
	}
	if posBefore == 0 {
		d.Reset()
		return
	}
	d.Reset()
	d.r.Seek(posBefore, io.SeekStart)
}
