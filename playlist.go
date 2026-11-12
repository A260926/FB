package wav

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"
	"time"

	"github.com/go-audio/audio"
	"github.com/go-audio/riff"
)

// cidPlaylist is the chunk ID for a playlist ("plst") chunk.
var cidPlaylist = [4]byte{'p', 'l', 's', 't'}

// maxInt is the largest value an int can hold on this platform.
const maxInt = int(^uint(0) >> 1)

// Playlist mode errors.
var (
	errPlaylistTruncated    = errors.New("wav: truncated playlist chunk")
	errPlaylistDupCueID     = errors.New("wav: duplicate cue point ID")
	errPlaylistMissingCue   = errors.New("wav: playlist segment references an unknown cue point")
	errPlaylistOutOfData    = errors.New("wav: playlist segment runs past the end of the data chunk")
	errPlaylistUnaligned    = errors.New("wav: data chunk size is not a whole number of frames")
	errPlaylistDupData      = errors.New("wav: multiple data chunks are not supported in playlist mode")
	errPlaylistDupPlst      = errors.New("wav: multiple playlist chunks are not supported")
	errPlaylistDupCue       = errors.New("wav: multiple cue chunks are not supported")
	errPlaylistBadCue       = errors.New("wav: cue point does not target the single data chunk")
	errPlaylistOverflow     = errors.New("wav: expanded playlist overflows the supported size")
	errPlaylistRawSeek      = errors.New("wav: raw byte Seek is not supported in playlist mode, use SeekPlaylistFrame")
	errPlaylistSeekDefault  = errors.New("wav: SeekPlaylistFrame is only available when UsePlaylist is enabled")
	errPlaylistBadBufferLen = errors.New("wav: PCM buffer length must be a multiple of the channel count")
)

// playlistSegment describes one entry of a plst chunk.
type playlistSegment struct {
	// cueID matches a CuePoint.ID on its raw four bytes.
	cueID [4]byte
	// length is the number of frames played per repetition. A frame
	// covers every channel.
	length uint32
	// playCount is the number of times the segment is played in full.
	playCount uint32
	// startFrame is the frame index into the physical data chunk where
	// this segment begins. Resolved once during validation.
	startFrame int64
}

// playlistState is the logical view over the physical data chunk built
// from the cue and plst chunks. It never holds expanded audio data: reads
// seek back into the underlying stream on demand, so memory use does not
// grow with play counts or the expanded playlist length.
type playlistState struct {
	d *Decoder

	// segments keeps the plst entries in their on-disk order. Its size is
	// bounded by the playlist chunk itself.
	segments []playlistSegment
	// cues indexes cue points by their raw four byte ID.
	cues map[[4]byte]*CuePoint

	// dataAbs is the absolute offset of the data chunk payload in d.r.
	dataAbs int64
	// dataSize is the declared, unpadded size of the data chunk in bytes.
	dataSize int64
	// frameBytes is the byte size of one frame (all channels).
	frameBytes int64

	totalFrames int64
	totalBytes  int64
	duration    time.Duration

	// Logical playback position, in frames.
	logicalPos int64
	// Index of the segment currently being played.
	segIndex int
	// Repetition currently being played inside segments[segIndex].
	repeatIndex int64
	// Frames already consumed inside the current repetition.
	segFrame int64
}

// preparePlaylist scans every top level chunk, parses the cue and plst
// chunks and validates the whole playlist before any sample is handed out.
func (d *Decoder) preparePlaylist() error {
	if d.pl != nil {
		return nil
	}
	if err := d.readHeaders(); err != nil {
		return err
	}
	if d.WavAudioFormat != 1 {
		return fmt.Errorf("%w: format tag %d is not integer PCM", riff.ErrFmtNotSupported, d.WavAudioFormat)
	}
	switch d.BitDepth {
	case 8, 16, 24, 32:
	default:
		return fmt.Errorf("%w: unsupported bit depth %d", riff.ErrFmtNotSupported, d.BitDepth)
	}
	if d.NumChans != 1 && d.NumChans != 2 {
		return fmt.Errorf("%w: playlist mode supports mono or stereo only, got %d channels", riff.ErrFmtNotSupported, d.NumChans)
	}
	if d.SampleRate == 0 {
		return errors.New("wav: invalid sample rate of 0")
	}

	pl := &playlistState{
		d:          d,
		cues:       map[[4]byte]*CuePoint{},
		frameBytes: int64(d.BitDepth) / 8 * int64(d.NumChans),
	}

	var (
		dataChunks int
		cueChunks  int
		plstChunks int
		hasPlst    bool
	)

	// readHeaders leaves the reader positioned at the first top level
	// chunk, so walk the container ourselves. Chunk headers and odd byte
	// padding are handled explicitly rather than through riff.Parser so
	// the declared (unpadded) payload sizes stay available. The data
	// chunk and unknown chunks are skipped over instead of being read
	// into memory, so memory use stays independent of the audio size.
	for {
		var id [4]byte
		if _, err := io.ReadFull(d.r, id[:]); err != nil {
			if errors.Is(err, io.EOF) {
				break
			}
			return fmt.Errorf("failed to read chunk header: %w", err)
		}
		var rawSize uint32
		if err := binary.Read(d.r, binary.LittleEndian, &rawSize); err != nil {
			return fmt.Errorf("failed to read chunk size: %w", err)
		}
		payloadStart, err := d.r.Seek(0, io.SeekCurrent)
		if err != nil {
			return err
		}
		skip := int64(rawSize) + int64(rawSize&1)

		switch id {
		case riff.DataFormatID:
			dataChunks++
			if dataChunks > 1 {
				return errPlaylistDupData
			}
			pl.dataAbs = payloadStart
			pl.dataSize = int64(rawSize)
		case CIDCue:
			cueChunks++
			if cueChunks > 1 {
				return errPlaylistDupCue
			}
			payload, err := readChunkPayload(d.r, int64(rawSize))
			if err != nil {
				return err
			}
			if err := pl.parseCue(payload); err != nil {
				return err
			}
			continue
		case cidPlaylist:
			plstChunks++
			if plstChunks > 1 {
				return errPlaylistDupPlst
			}
			hasPlst = true
			payload, err := readChunkPayload(d.r, int64(rawSize))
			if err != nil {
				return err
			}
			if err := pl.parsePlaylist(payload); err != nil {
				return err
			}
			continue
		case CIDList:
			// Keep metadata decoding available without buffering audio.
			payload, err := readChunkPayload(d.r, int64(rawSize))
			if err != nil {
				return err
			}
			ch := &riff.Chunk{ID: id, Size: len(payload), R: bytes.NewReader(payload)}
			if err := DecodeListChunk(d, ch); err != nil {
				return err
			}
			continue
		case CIDSmpl:
			payload, err := readChunkPayload(d.r, int64(rawSize))
			if err != nil {
				return err
			}
			ch := &riff.Chunk{ID: id, Size: len(payload), R: bytes.NewReader(payload)}
			if err := DecodeSamplerChunk(d, ch); err != nil {
				return err
			}
			continue
		}

		// Data and unknown chunks: jump over payload plus any odd
		// alignment byte without copying the bytes.
		if _, err := d.r.Seek(skip, io.SeekCurrent); err != nil {
			return fmt.Errorf("failed to skip chunk %x: %w", id, err)
		}
	}

	if dataChunks == 0 {
		return ErrPCMChunkNotFound
	}
	if pl.dataSize%pl.frameBytes != 0 {
		return fmt.Errorf("%w: %d bytes for %d bytes/frame", errPlaylistUnaligned, pl.dataSize, pl.frameBytes)
	}
	dataFrames := pl.dataSize / pl.frameBytes

	if !hasPlst {
		// No playlist: the logical stream is the raw data, in order.
		if dataFrames > int64(^uint32(0)) {
			return fmt.Errorf("%w: %d data frames", errPlaylistOverflow, dataFrames)
		}
		pl.segments = []playlistSegment{{
			length:     uint32(dataFrames),
			playCount:  1,
			startFrame: 0,
		}}
		// The implicit segment addresses the data chunk directly, so no
		// cue resolution is needed.
		pl.totalFrames = dataFrames
	} else {
		if err := pl.validate(dataFrames); err != nil {
			return err
		}
	}

	if err := pl.computeSizes(); err != nil {
		return err
	}

	d.pl = pl
	d.metadataRead = true
	return nil
}

// readChunkPayload consumes size payload bytes plus the optional odd
// alignment byte and returns the payload.
func readChunkPayload(r io.Reader, size int64) ([]byte, error) {
	if size < 0 {
		return nil, errors.New("wav: negative chunk size")
	}
	buf := make([]byte, size)
	if _, err := io.ReadFull(r, buf); err != nil {
		return nil, fmt.Errorf("failed to read chunk payload: %w", err)
	}
	if size%2 == 1 {
		var pad [1]byte
		if _, err := io.ReadFull(r, pad[:]); err != nil {
			return nil, fmt.Errorf("failed to read chunk padding: %w", err)
		}
	}
	return buf, nil
}

// parseCue decodes a cue chunk payload, enforcing unique IDs and single
// data chunk targeting.
func (pl *playlistState) parseCue(payload []byte) error {
	if len(payload) < 4 {
		return fmt.Errorf("%w: cue chunk is too small for a cue count", errPlaylistTruncated)
	}
	r := bytes.NewReader(payload)
	var nbrCues uint32
	if err := binary.Read(r, binary.LittleEndian, &nbrCues); err != nil {
		return fmt.Errorf("%w: %v", errPlaylistTruncated, err)
	}
	if uint64(nbrCues)*24+4 > uint64(len(payload)) {
		return fmt.Errorf("%w: cue chunk declares %d cue points but only holds %d bytes", errPlaylistTruncated, nbrCues, len(payload))
	}

	cues := make([]*CuePoint, 0, nbrCues)
	for i := uint32(0); i < nbrCues; i++ {
		c := &CuePoint{}
		var id [4]byte
		if _, err := io.ReadFull(r, id[:]); err != nil {
			return fmt.Errorf("%w: %v", errPlaylistTruncated, err)
		}
		c.ID = id
		if err := binary.Read(r, binary.LittleEndian, &c.Position); err != nil {
			return err
		}
		var dataID [4]byte
		if _, err := io.ReadFull(r, dataID[:]); err != nil {
			return fmt.Errorf("%w: %v", errPlaylistTruncated, err)
		}
		c.DataChunkID = dataID
		if err := binary.Read(r, binary.LittleEndian, &c.ChunkStart); err != nil {
			return err
		}
		if err := binary.Read(r, binary.LittleEndian, &c.BlockStart); err != nil {
			return err
		}
		if err := binary.Read(r, binary.LittleEndian, &c.SampleOffset); err != nil {
			return err
		}
		if c.DataChunkID != riff.DataFormatID || c.ChunkStart != 0 || c.BlockStart != 0 {
			return fmt.Errorf("%w: cue %x", errPlaylistBadCue, c.ID)
		}
		if _, exists := pl.cues[c.ID]; exists {
			return fmt.Errorf("%w: %x", errPlaylistDupCueID, c.ID)
		}
		pl.cues[c.ID] = c
		cues = append(cues, c)
	}

	if pl.d.Metadata == nil {
		pl.d.Metadata = &Metadata{}
	}
	pl.d.Metadata.CuePoints = cues
	return nil
}

// parsePlaylist decodes a plst chunk payload: a little endian uint32
// segment count followed by triplets of cue ID, frame length and play
// count.
func (pl *playlistState) parsePlaylist(payload []byte) error {
	if len(payload) < 4 {
		return fmt.Errorf("%w: playlist chunk is too small for a segment count", errPlaylistTruncated)
	}
	r := bytes.NewReader(payload)
	var nbrSegments uint32
	if err := binary.Read(r, binary.LittleEndian, &nbrSegments); err != nil {
		return fmt.Errorf("%w: %v", errPlaylistTruncated, err)
	}
	if uint64(nbrSegments)*12+4 > uint64(len(payload)) {
		return fmt.Errorf("%w: playlist declares %d segments but only holds %d bytes", errPlaylistTruncated, nbrSegments, len(payload))
	}

	pl.segments = make([]playlistSegment, 0, nbrSegments)
	for i := uint32(0); i < nbrSegments; i++ {
		seg := playlistSegment{}
		if _, err := io.ReadFull(r, seg.cueID[:]); err != nil {
			return fmt.Errorf("%w: %v", errPlaylistTruncated, err)
		}
		if err := binary.Read(r, binary.LittleEndian, &seg.length); err != nil {
			return err
		}
		if err := binary.Read(r, binary.LittleEndian, &seg.playCount); err != nil {
			return err
		}
		pl.segments = append(pl.segments, seg)
	}
	return nil
}

// validate resolves every playlist entry against the cue table and the
// data chunk bounds, and computes the expanded logical frame count.
func (pl *playlistState) validate(dataFrames int64) error {
	var total int64
	for i := range pl.segments {
		seg := &pl.segments[i]
		cue, ok := pl.cues[seg.cueID]
		if !ok {
			return fmt.Errorf("%w: cue ID %x", errPlaylistMissingCue, seg.cueID)
		}
		seg.startFrame = int64(cue.SampleOffset)
		// Zero length or zero play count produces no sound, but the cue
		// reference itself still has to exist.
		if seg.length == 0 || seg.playCount == 0 {
			continue
		}
		start := seg.startFrame
		end := start + int64(seg.length)
		if start < 0 || end > dataFrames {
			return fmt.Errorf("%w: cue %x offset %d length %d with %d data frames",
				errPlaylistOutOfData, seg.cueID, cue.SampleOffset, seg.length, dataFrames)
		}
		contrib, ok := mul64(int64(seg.length), int64(seg.playCount))
		if !ok {
			return errPlaylistOverflow
		}
		total, ok = add64(total, contrib)
		if !ok {
			return errPlaylistOverflow
		}
	}
	pl.totalFrames = total
	return nil
}

// computeSizes derives the public byte length and duration from the
// logical frame count, rejecting values that would overflow the returned
// types.
func (pl *playlistState) computeSizes() error {
	totalBytes, ok := mul64(pl.totalFrames, pl.frameBytes)
	if !ok {
		return fmt.Errorf("%w: byte length", errPlaylistOverflow)
	}
	pl.totalBytes = totalBytes

	rate := int64(pl.d.SampleRate)
	seconds := pl.totalFrames / rate
	if seconds > math.MaxInt64/int64(time.Second) {
		return fmt.Errorf("%w: duration", errPlaylistOverflow)
	}
	remainder := pl.totalFrames % rate
	pl.duration = time.Duration(seconds)*time.Second + time.Duration(remainder)*time.Second/time.Duration(rate)
	return nil
}

// ensurePlaylistPrepared runs the one-time validation, caching any error.
func (d *Decoder) ensurePlaylistPrepared() error {
	if d.pl != nil {
		return nil
	}
	if d.err != nil {
		return d.err
	}
	if err := d.preparePlaylist(); err != nil {
		d.err = err
		return err
	}
	return nil
}

// readPlaylistFrames fills byteBuf with decoded-frame bytes following the
// playlist, seeking inside the underlying stream as segments and repeats
// require. It never expands the playlist into memory.
func (pl *playlistState) readFrames(byteBuf []byte) (int, error) {
	written := 0
	for written < len(byteBuf) {
		for pl.segIndex < len(pl.segments) {
			seg := pl.segments[pl.segIndex]
			if seg.length > 0 && seg.playCount > 0 && pl.repeatIndex < int64(seg.playCount) {
				break
			}
			pl.segIndex++
			pl.repeatIndex = 0
			pl.segFrame = 0
		}
		if pl.segIndex == len(pl.segments) {
			break
		}

		seg := pl.segments[pl.segIndex]
		framesWanted := int64(len(byteBuf)-written) / pl.frameBytes
		framesLeft := int64(seg.length) - pl.segFrame
		nFrames := framesWanted
		if framesLeft < nFrames {
			nFrames = framesLeft
		}

		offset := pl.dataAbs + (seg.startFrame+pl.segFrame)*pl.frameBytes
		if _, err := pl.d.r.Seek(offset, io.SeekStart); err != nil {
			return written, fmt.Errorf("playlist seek failed: %w", err)
		}
		nBytes := nFrames * pl.frameBytes
		if _, err := io.ReadFull(pl.d.r, byteBuf[written:written+int(nBytes)]); err != nil {
			return written, fmt.Errorf("playlist read failed: %w", err)
		}
		written += int(nBytes)
		pl.logicalPos += nFrames
		pl.segFrame += nFrames
		if pl.segFrame == int64(seg.length) {
			pl.repeatIndex++
			pl.segFrame = 0
			if pl.repeatIndex == int64(seg.playCount) {
				pl.segIndex++
				pl.repeatIndex = 0
			}
		}
	}
	return written, nil
}

// locate moves the logical playback position to frame, which must be
// within [0, totalFrames].
func (pl *playlistState) locate(frame int64) {
	if frame == pl.totalFrames {
		pl.segIndex = len(pl.segments)
		pl.repeatIndex = 0
		pl.segFrame = 0
		pl.logicalPos = frame
		return
	}

	var logical int64
	for i, seg := range pl.segments {
		if seg.length == 0 || seg.playCount == 0 {
			continue
		}
		contrib := int64(seg.length) * int64(seg.playCount)
		if frame < logical+contrib {
			local := frame - logical
			pl.segIndex = i
			pl.repeatIndex = local / int64(seg.length)
			pl.segFrame = local % int64(seg.length)
			pl.logicalPos = frame
			return
		}
		logical += contrib
	}

	// Unreachable for frame < totalFrames, but leave a consistent state.
	pl.segIndex = len(pl.segments)
	pl.repeatIndex = 0
	pl.segFrame = 0
	pl.logicalPos = pl.totalFrames
}

// reset returns the logical stream to frame 0.
func (pl *playlistState) reset() {
	pl.logicalPos = 0
	pl.segIndex = 0
	pl.repeatIndex = 0
	pl.segFrame = 0
}

// SeekPlaylistFrame moves the logical playback position to the frame
// frame. frame ranges from 0 to the total number of frames; seeking to
// the total leaves the decoder positioned at end of stream. Out of range
// frames leave the position untouched. It is only available when
// UsePlaylist is enabled.
func (d *Decoder) SeekPlaylistFrame(frame int64) error {
	if !d.UsePlaylist {
		return errPlaylistSeekDefault
	}
	if err := d.ensurePlaylistPrepared(); err != nil {
		return err
	}
	if frame < 0 || frame > d.pl.totalFrames {
		return fmt.Errorf("wav: playlist frame %d is out of range [0, %d]", frame, d.pl.totalFrames)
	}
	d.pl.locate(frame)
	// A seek clears a previously recorded end of stream or read error.
	d.err = nil
	return nil
}

// pcmBufferPlaylist implements PCMBuffer against the logical playlist.
func (d *Decoder) pcmBufferPlaylist(buf *audio.IntBuffer) (int, error) {
	if err := d.ensurePlaylistPrepared(); err != nil {
		return 0, err
	}
	pl := d.pl
	numChans := int(d.NumChans)
	if len(buf.Data)%numChans != 0 {
		// Invalid geometry: the logical position must not move.
		return 0, fmt.Errorf("%w: got %d samples for %d channels", errPlaylistBadBufferLen, len(buf.Data), numChans)
	}
	if pl.logicalPos >= pl.totalFrames {
		buf.Format = d.Format()
		buf.SourceBitDepth = int(d.BitDepth)
		return 0, nil
	}

	decodeF, err := sampleDecodeFunc(int(d.BitDepth))
	if err != nil {
		return 0, fmt.Errorf("could not get sample decode func %w", err)
	}
	bPerSample := bytesPerSample(int(d.BitDepth))
	framesWanted := int64(len(buf.Data)) / int64(numChans)
	framesAvailable := pl.totalFrames - pl.logicalPos
	if framesAvailable < framesWanted {
		framesWanted = framesAvailable
	}
	tmpBuf := make([]byte, framesWanted*pl.frameBytes)
	nBytes, err := pl.readFrames(tmpBuf)
	if err != nil {
		d.err = err
		return 0, err
	}
	bufR := bytes.NewReader(tmpBuf[:nBytes])
	sampleBuf := make([]byte, bPerSample, bPerSample)
	n := 0
	for n < len(buf.Data) {
		v, derr := decodeF(bufR, sampleBuf)
		if derr != nil {
			break
		}
		buf.Data[n] = v
		n++
	}
	buf.Format = d.Format()
	buf.SourceBitDepth = int(d.BitDepth)
	return n, nil
}

// fullPCMBufferPlaylist reads the remainder of the logical stream.
func (d *Decoder) fullPCMBufferPlaylist() (*audio.IntBuffer, error) {
	if err := d.ensurePlaylistPrepared(); err != nil {
		return nil, err
	}
	pl := d.pl
	remaining, ok := mul64(pl.totalFrames-pl.logicalPos, int64(d.NumChans))
	if !ok || remaining > int64(maxInt) {
		return nil, fmt.Errorf("%w: remaining sample count", errPlaylistOverflow)
	}
	buf := &audio.IntBuffer{
		Data:           make([]int, int(remaining)),
		Format:         d.Format(),
		SourceBitDepth: int(d.BitDepth),
	}
	if remaining == 0 {
		return buf, nil
	}
	n, err := d.PCMBuffer(buf)
	if err != nil {
		return nil, err
	}
	buf.Data = buf.Data[:n]
	return buf, nil
}

func mul64(a, b int64) (int64, bool) {
	if a == 0 || b == 0 {
		return 0, true
	}
	if a < 0 || b < 0 || a > math.MaxInt64/b {
		return 0, false
	}
	return a * b, true
}

func add64(a, b int64) (int64, bool) {
	if b > math.MaxInt64-a {
		return 0, false
	}
	return a + b, true
}
