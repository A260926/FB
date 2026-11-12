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

// CIDPlaylist is the chunk ID for the playlist chunk ("plst").
var CIDPlaylist = [4]byte{'p', 'l', 's', 't'}

var (
	// ErrTruncatedPlaylist is returned when a playlist chunk does not
	// contain as many entries as its declared segment count.
	ErrTruncatedPlaylist = errors.New("wav: truncated playlist chunk")
	// ErrDuplicateCueID is returned when two cue points share the same ID.
	ErrDuplicateCueID = errors.New("wav: duplicate cue point ID")
	// ErrMissingCueReference is returned when a playlist segment points at
	// a cue point that is not present in the file.
	ErrMissingCueReference = errors.New("wav: playlist references an unknown cue point")
	// ErrSegmentOutOfData is returned when a playlist segment plays frames
	// outside of the data chunk.
	ErrSegmentOutOfData = errors.New("wav: playlist segment is out of the data chunk bounds")
	// ErrMisalignedData is returned when the data chunk size is not a whole
	// number of frames.
	ErrMisalignedData = errors.New("wav: data chunk size is not a whole number of frames")
	// ErrDuplicateChunk is returned when the file contains more than one
	// data or playlist chunk.
	ErrDuplicateChunk = errors.New("wav: duplicate data or playlist chunk")
	// ErrPlaylistOverflow is returned when the expanded playlist does not
	// fit in the public size/duration types.
	ErrPlaylistOverflow = errors.New("wav: expanded playlist is too large")
	// ErrBufferNotFrameAligned is returned when a PCM buffer length cannot
	// hold whole frames in playlist mode.
	ErrBufferNotFrameAligned = errors.New("wav: PCM buffer length must be a multiple of the channel count")
	// ErrSeekPlaylistDisabled is returned when SeekPlaylistFrame is used
	// while playlist mode is off.
	ErrSeekPlaylistDisabled = errors.New("wav: SeekPlaylistFrame requires UsePlaylist; use Seek for raw byte offsets")
	// ErrRawSeekDisabled is returned when Seek is used in playlist mode.
	ErrRawSeekDisabled = errors.New("wav: raw byte Seek is disabled in playlist mode; use SeekPlaylistFrame")
	// ErrPlaylistFormatUnsupported is returned in playlist mode for
	// containers outside the supported range: integer PCM, 8/16/24/32 bit,
	// mono or stereo.
	ErrPlaylistFormatUnsupported = errors.New("wav: playlist mode only supports integer PCM, 8/16/24/32 bit, mono or stereo")
)

// playlistSegment is one entry of the "plst" chunk: play Length frames
// starting at the referenced cue point, PlayCount times in a row.
type playlistSegment struct {
	cueID      [4]byte
	startFrame uint32 // resolved from the cue point's SampleOffset
	length     uint32 // frames per repetition
	playCount  uint32
}

// playlistState holds the validated playlist and the current logical
// playback position. Nothing here grows with the play count: repetitions
// are replayed by seeking back into the single data chunk.
type playlistState struct {
	entries []playlistSegment

	dataStart int64 // absolute offset of the data chunk payload in d.r
	dataSize  int64 // unpadded data chunk size, in bytes
	frameSize int64 // bytes per frame (all channels)

	totalFrames int64 // expanded logical frame count
	totalBytes  int64 // expanded logical byte count
	hasPlaylist bool  // false means "no plst chunk": play the raw data

	// logical cursor
	pos int64  // frames already handed out
	seg int    // index into entries
	rep uint32 // repetition inside the current segment
	off uint32 // frames consumed inside the current repetition

	// physical cursor inside the data chunk
	physPos  int64 // next byte expected from d.r relative to dataStart
	seekNext bool  // d.r must be seeked before the next read

	atEOF bool
}

func (d *Decoder) preparePlaylist() error {
	if d.pl != nil {
		if d.err != nil && !errors.Is(d.err, io.EOF) {
			return d.err
		}
		return nil
	}
	if d.plErr != nil {
		return d.plErr
	}
	if err := d.scanPlaylist(); err != nil {
		d.plErr = err
		d.err = err
		return err
	}
	return nil
}

// scanPlaylist scans the whole container once, validates the cue and
// playlist chunks and every referenced range. It runs before the first
// sample is handed out so that invalid tables are reported up front.
func (d *Decoder) scanPlaylist() error {
	// own the reader for a clean single pass even if headers or metadata
	// were inspected beforehand
	if _, err := d.r.Seek(0, io.SeekStart); err != nil {
		return fmt.Errorf("wav: failed to rewind before playlist scan: %w", err)
	}
	d.parser = riff.New(d.r)

	pl := &playlistState{
		seekNext: true,
	}

	// Parse the RIFF/WAVE envelope ourselves so that every sub chunk is
	// visited exactly once, regardless of where fmt, cue, plst and data
	// appear relative to each other.
	id, size, err := d.parser.IDnSize()
	if err != nil {
		return err
	}
	d.parser.ID = id
	if d.parser.ID != riff.RiffID {
		return fmt.Errorf("%s - %s", d.parser.ID, riff.ErrFmtNotSupported)
	}
	d.parser.Size = size
	if err := binary.Read(d.r, binary.BigEndian, &d.parser.Format); err != nil {
		return err
	}

	var (
		cueByID    = map[[4]byte]*CuePoint{}
		dataCount  int
		plstCount  int
		fmtCount   int
		rawEntries []playlistSegment
	)

	for {
		id, size, err := d.parser.IDnSize()
		if err == io.EOF {
			break
		}
		if err != nil {
			return err
		}
		ch := &riff.Chunk{
			ID:   id,
			Size: int(size),
			R:    io.LimitReader(d.r, int64(size)),
		}

		switch id {
		case riff.FmtID:
			fmtCount++
			if fmtCount > 1 {
				return ErrDuplicateChunk
			}
			if err := ch.DecodeWavHeader(d.parser); err != nil {
				return err
			}
			d.NumChans = d.parser.NumChannels
			d.BitDepth = d.parser.BitsPerSample
			d.SampleRate = d.parser.SampleRate
			d.WavAudioFormat = d.parser.WavAudioFormat
			d.AvgBytesPerSec = d.parser.AvgBytesPerSec
		case CIDCue:
			if err := DecodeCueChunk(d, ch); err != nil {
				return err
			}
			ch.Drain()
		case CIDPlaylist:
			plstCount++
			if plstCount > 1 {
				return ErrDuplicateChunk
			}
			raw, err := readChunkBytes(ch)
			if err != nil {
				return err
			}
			rawEntries, err = parsePlaylistPayload(raw)
			if err != nil {
				return err
			}
		case riff.DataFormatID:
			dataCount++
			if dataCount > 1 {
				return ErrDuplicateChunk
			}
			pl.dataStart, err = d.r.Seek(0, io.SeekCurrent)
			if err != nil {
				return err
			}
			pl.dataSize = int64(size)
		case CIDList:
			if err := DecodeListChunk(d, ch); err != nil && !errors.Is(err, io.EOF) {
				d.err = err
			}
		case CIDSmpl:
			if err := DecodeSamplerChunk(d, ch); err != nil && !errors.Is(err, io.EOF) {
				d.err = err
			}
		}

		// Drain consumes exactly size bytes through the LimitReader;
		// only the odd-size padding byte is left on the stream.
		ch.Drain()
		if err := skipBytes(d.r, int64(size%2)); err != nil {
			return err
		}
	}

	if d.NumChans == 0 || d.BitDepth == 0 {
		return fmt.Errorf("wav: missing or invalid fmt chunk")
	}
	// playlist mode is scoped to integer PCM, 8/16/24/32 bit, mono/stereo
	if d.WavAudioFormat != 1 || d.NumChans > 2 ||
		(d.BitDepth != 8 && d.BitDepth != 16 && d.BitDepth != 24 && d.BitDepth != 32) {
		return ErrPlaylistFormatUnsupported
	}
	pl.frameSize = int64(d.NumChans) * int64(bytesPerSample(int(d.BitDepth)))
	if pl.frameSize <= 0 {
		return ErrPlaylistFormatUnsupported
	}
	if dataCount == 0 {
		return ErrPCMChunkNotFound
	}
	if pl.dataSize%pl.frameSize != 0 {
		return ErrMisalignedData
	}
	dataFrames := pl.dataSize / pl.frameSize

	// index cue points by their raw four byte ID; order in the cue chunk
	// must not be used to pair cues with playlist entries.
	if d.Metadata != nil {
		for _, c := range d.Metadata.CuePoints {
			if _, exists := cueByID[c.ID]; exists {
				return ErrDuplicateCueID
			}
			cueByID[c.ID] = c
		}
	}

	pl.hasPlaylist = plstCount > 0
	pl.entries = make([]playlistSegment, 0, len(rawEntries))
	var total uint64
	for _, e := range rawEntries {
		cue, ok := cueByID[e.cueID]
		if !ok || cue.DataChunkID != riff.DataFormatID || cue.ChunkStart != 0 || cue.BlockStart != 0 {
			return fmt.Errorf("%w: cue % x", ErrMissingCueReference, e.cueID[:])
		}
		start := uint64(cue.SampleOffset)
		length := uint64(e.length)
		if start+length > uint64(dataFrames) || start+length < start {
			return fmt.Errorf("%w: cue % x", ErrSegmentOutOfData, e.cueID[:])
		}
		e.startFrame = cue.SampleOffset
		pl.entries = append(pl.entries, e)

		contrib := length * uint64(e.playCount)
		total += contrib
		if total > math.MaxInt64 {
			return ErrPlaylistOverflow
		}
	}

	if pl.hasPlaylist {
		pl.totalFrames = int64(total)
	} else {
		// no playlist chunk: the logical stream is the raw data
		pl.totalFrames = dataFrames
		pl.totalBytes = pl.dataSize
	}
	if pl.totalFrames > math.MaxInt64/pl.frameSize {
		return ErrPlaylistOverflow
	}
	pl.totalBytes = pl.totalFrames * pl.frameSize

	d.pl = pl
	d.pcmDataAccessed = true
	return nil
}

// parsePlaylistPayload parses a "plst" chunk payload: a little endian
// uint32 segment count followed by triplets of uint32 (cue ID, frame
// length, play count).
func parsePlaylistPayload(raw []byte) ([]playlistSegment, error) {
	if len(raw) < 4 {
		return nil, ErrTruncatedPlaylist
	}
	count := binary.LittleEndian.Uint32(raw[:4])
	body := raw[4:]
	if len(body) != int(count)*12 {
		return nil, ErrTruncatedPlaylist
	}
	entries := make([]playlistSegment, 0, count)
	for i := uint32(0); i < count; i++ {
		triplet := body[i*12 : (i+1)*12]
		var e playlistSegment
		copy(e.cueID[:], triplet[0:4])
		e.length = binary.LittleEndian.Uint32(triplet[4:8])
		e.playCount = binary.LittleEndian.Uint32(triplet[8:12])
		entries = append(entries, e)
	}
	return entries, nil
}

func readChunkBytes(ch *riff.Chunk) ([]byte, error) {
	buf := make([]byte, ch.Size)
	if _, err := io.ReadFull(ch, buf); err != nil {
		return nil, fmt.Errorf("failed to read the %s chunk - %w", string(ch.ID[:]), err)
	}
	return buf, nil
}

func skipBytes(r io.Reader, n int64) error {
	if n <= 0 {
		return nil
	}
	_, err := io.CopyN(io.Discard, r, n)
	return err
}

// playlistLen returns the logical byte count of the playlist stream.
func (d *Decoder) playlistLen() (int64, error) {
	if err := d.preparePlaylist(); err != nil {
		return 0, err
	}
	return d.pl.totalBytes, nil
}

// playlistDuration returns the logical duration of the playlist stream.
func (d *Decoder) playlistDuration() (time.Duration, error) {
	if err := d.preparePlaylist(); err != nil {
		return 0, err
	}
	if d.SampleRate == 0 {
		return 0, errors.New("wav: can't calculate the duration without a sample rate")
	}
	frames := d.pl.totalFrames
	// guard time.Duration (int64 nanoseconds) against overflow
	if frames > math.MaxInt64/int64(time.Second) {
		return 0, ErrPlaylistOverflow
	}
	return time.Duration(frames) * time.Second / time.Duration(d.SampleRate), nil
}

// playlistEOF reports whether the logical stream is fully consumed.
func (d *Decoder) playlistEOF() bool {
	if err := d.preparePlaylist(); err != nil {
		return errors.Is(err, io.EOF)
	}
	return d.pl.atEOF || d.pl.pos >= d.pl.totalFrames
}

// SeekPlaylistFrame moves the logical playback position to frame.
// frame ranges from 0 to the total frame count; seeking to the total
// frame count positions at the end of the stream. Out of range seeks
// leave the current position untouched. It is only available in playlist
// mode.
func (d *Decoder) SeekPlaylistFrame(frame int64) error {
	if !d.UsePlaylist {
		return ErrSeekPlaylistDisabled
	}
	if err := d.preparePlaylist(); err != nil {
		return err
	}
	pl := d.pl
	if frame < 0 || frame > pl.totalFrames {
		return fmt.Errorf("wav: playlist frame %d out of range [0, %d]", frame, pl.totalFrames)
	}

	if frame == pl.totalFrames {
		pl.seg = len(pl.entries)
		pl.rep, pl.off, pl.pos = 0, 0, frame
		pl.seekNext = true
		pl.atEOF = true
		d.err = io.EOF
		return nil
	}

	target := frame
	if !pl.hasPlaylist {
		// raw data playback: a single implicit segment
		pl.seg, pl.rep, pl.off = 0, 0, uint32(target)
	} else {
		var prefix int64
		found := false
		for i, e := range pl.entries {
			span := int64(e.length) * int64(e.playCount)
			if target < prefix+span {
				local := target - prefix
				pl.seg = i
				if e.length > 0 {
					pl.rep = uint32(local / int64(e.length))
					pl.off = uint32(local % int64(e.length))
				} else {
					pl.rep, pl.off = 0, 0
				}
				found = true
				break
			}
			prefix += span
		}
		if !found {
			return fmt.Errorf("wav: playlist frame %d out of range [0, %d]", frame, pl.totalFrames)
		}
	}
	pl.pos = frame
	pl.seekNext = true
	pl.atEOF = false
	if errors.Is(d.err, io.EOF) {
		d.err = nil
	}
	return nil
}

// rewindPlaylist resets the logical cursor to frame 0 without rescanning
// the container.
func (d *Decoder) rewindPlaylist() error {
	if err := d.preparePlaylist(); err != nil {
		return err
	}
	pl := d.pl
	pl.seg, pl.rep, pl.off, pl.pos = 0, 0, 0, 0
	pl.physPos, pl.seekNext, pl.atEOF = 0, true, false
	if errors.Is(d.err, io.EOF) {
		d.err = nil
	}
	return nil
}

// readPlaylistPCM fills buf with logical samples. The buffer must hold a
// whole number of frames; invalid lengths are rejected without moving the
// cursor.
func (d *Decoder) readPlaylistPCM(buf *audio.IntBuffer) (int, error) {
	if buf == nil {
		return 0, nil
	}
	if err := d.preparePlaylist(); err != nil {
		return 0, err
	}
	pl := d.pl
	numChans := int(d.NumChans)
	if len(buf.Data)%numChans != 0 {
		return 0, ErrBufferNotFrameAligned
	}

	buf.SourceBitDepth = int(d.BitDepth)
	buf.Format = &audio.Format{
		NumChannels: numChans,
		SampleRate:  int(d.SampleRate),
	}
	decodeF, err := sampleDecodeFunc(int(d.BitDepth))
	if err != nil {
		return 0, fmt.Errorf("could not get sample decode func %w", err)
	}

	framesWanted := len(buf.Data) / numChans
	framesRead := 0
	frameSize := pl.frameSize
	// one scratch buffer, sized to the largest contiguous run this call can
	// ask for; it never grows with the number of repetitions.
	byteBuf := make([]byte, int64(framesWanted)*frameSize)
	sampleBuf := make([]byte, bytesPerSample(int(d.BitDepth)))

	for framesRead < framesWanted {
		entry, ok := pl.activeEntry()
		if !ok {
			pl.atEOF = true
			d.err = io.EOF
			break
		}

		take := int64(entry.length) - int64(pl.off)
		if remaining := int64(framesWanted - framesRead); take > remaining {
			take = remaining
		}
		run := byteBuf[:take*frameSize]
		physFrame := int64(entry.startFrame) + int64(pl.off)
		if err := d.readPlaylistBytes(run, physFrame*frameSize); err != nil {
			return framesRead * numChans, err
		}

		byteR := bytes.NewReader(run)
		for f := int64(0); f < take; f++ {
			for c := 0; c < numChans; c++ {
				v, err := decodeF(byteR, sampleBuf)
				if err != nil {
					return framesRead * numChans, err
				}
				buf.Data[framesRead*numChans+c] = v
			}
			framesRead++
		}

		pl.off += uint32(take)
		pl.pos += take
		if pl.off == entry.length {
			pl.rep++
			pl.off = 0
			if pl.rep == entry.playCount {
				pl.seg++
				pl.rep = 0
			}
			pl.seekNext = true
		}
	}

	if pl.pos >= pl.totalFrames {
		pl.atEOF = true
		d.err = io.EOF
	}
	return framesRead * numChans, nil
}

// activeEntry returns the segment repetition currently being played,
// skipping entries that produce no frames (zero length or zero play count).
func (pl *playlistState) activeEntry() (playlistSegment, bool) {
	if !pl.hasPlaylist {
		if pl.pos >= pl.totalFrames {
			return playlistSegment{}, false
		}
		// implicit single segment covering the raw data chunk
		dataFrames := uint32(pl.dataSize / pl.frameSize)
		return playlistSegment{startFrame: 0, length: dataFrames, playCount: 1}, true
	}
	for pl.seg < len(pl.entries) {
		e := pl.entries[pl.seg]
		if e.length == 0 || e.playCount == 0 || pl.rep >= e.playCount {
			pl.seg++
			pl.rep, pl.off = 0, 0
			pl.seekNext = true
			continue
		}
		return e, true
	}
	return playlistSegment{}, false
}

// readPlaylistBytes loads n bytes at physical offset off (relative to the
// data chunk payload). The underlying reader is only seeked when the read
// is not contiguous with the previous one, so repeated segments cost one
// seek per loop boundary rather than per frame.
func (d *Decoder) readPlaylistBytes(p []byte, off int64) error {
	pl := d.pl
	if pl.seekNext || off != pl.physPos {
		if _, err := d.r.Seek(pl.dataStart+off, io.SeekStart); err != nil {
			return err
		}
		pl.seekNext = false
	}
	n, err := io.ReadFull(d.r, p)
	pl.physPos = off + int64(n)
	if err != nil {
		return err
	}
	return nil
}

// fullPlaylistPCM reads the remaining logical stream into an IntBuffer,
// growing it as needed.
func (d *Decoder) fullPlaylistPCM() (*audio.IntBuffer, error) {
	format := &audio.Format{
		NumChannels: int(d.NumChans),
		SampleRate:  int(d.SampleRate),
	}
	numChans := int(d.NumChans)
	buf := &audio.IntBuffer{Data: make([]int, 4096), Format: format, SourceBitDepth: int(d.BitDepth)}
	i := 0
	for {
		// keep every read frame aligned
		if len(buf.Data)-i < numChans {
			buf.Data = append(buf.Data, make([]int, 4096)...)
		}
		chunk := &audio.IntBuffer{Data: buf.Data[i : len(buf.Data)-(len(buf.Data)-i)%numChans], Format: format}
		n, err := d.readPlaylistPCM(chunk)
		i += n
		if err != nil && !errors.Is(err, io.EOF) {
			buf.Data = buf.Data[:i]
			return buf, err
		}
		if n == 0 {
			break
		}
	}
	buf.Data = buf.Data[:i]
	return buf, nil
}
