package wav

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"math"
	"time"

	"github.com/youpy/go-riff"
	"github.com/zaf/g711"
)

type chunkInfo struct {
	id   []byte
	size uint32
	r    io.ReaderAt
	off  int64
}

type segmentKind int

const (
	segData segmentKind = iota
	segSilent
)

type segment struct {
	kind      segmentKind
	r         io.Reader
	remaining int64
	silent    uint32
}

type audioStream struct {
	segments   []segment
	index      int
	blockAlign int
	eightBit   bool
	partial    []byte
	hold       []byte
	holdOff    int
	frameReady bool
}

type Reader struct {
	rr     riff.RIFFReader
	parsed bool
	chunks []chunkInfo
	format *WavFormat
	*WavData
}

func NewReader(r riff.RIFFReader) *Reader {
	return &Reader{rr: r}
}

func readAtFull(r io.ReaderAt, p []byte, off int64) error {
	n := 0
	for n < len(p) {
		got, err := r.ReadAt(p[n:], off+int64(n))
		n += got
		if err != nil {
			if err == io.EOF && n == len(p) {
				return nil
			}
			return errors.New("riff chunk is truncated")
		}
	}

	return nil
}

func (r *Reader) parse() error {
	if r.parsed {
		return nil
	}

	header := make([]byte, 12)
	if err := readAtFull(r.rr, header, 0); err != nil {
		return err
	}
	if string(header[0:4]) != "RIFF" {
		return errors.New("Given bytes is not a RIFF format")
	}
	if string(header[8:12]) != "WAVE" {
		return errors.New("Given bytes is not a WAVE file")
	}

	riffSize := binary.LittleEndian.Uint32(header[4:8])
	if riffSize < 4 {
		return errors.New("riff chunk is truncated")
	}
	contentEnd := int64(riffSize) + 8
	off := int64(12)
	chunks := make([]chunkInfo, 0)

	for off < contentEnd {
		if contentEnd-off < 8 {
			return errors.New("riff chunk is truncated")
		}

		head := make([]byte, 8)
		if err := readAtFull(r.rr, head, off); err != nil {
			return err
		}
		size := binary.LittleEndian.Uint32(head[4:8])
		padded := uint64(size) + uint64(size&1)
		if padded > uint64(contentEnd-off-8) {
			return errors.New("riff chunk is truncated")
		}

		chunks = append(chunks, chunkInfo{
			id:   head[0:4],
			size: size,
			r:    r.rr,
			off:  off + 8,
		})
		off += 8 + int64(padded)
	}

	r.chunks = chunks
	r.parsed = true

	return nil
}

func (r *Reader) Format() (format *WavFormat, err error) {
	if r.format == nil {
		format, err = r.readFormat()
		if err != nil {
			return
		}
		r.format = format
	} else {
		format = r.format
	}

	return
}

func (r *Reader) Duration() (time.Duration, error) {
	format, err := r.Format()
	if err != nil {
		return 0.0, err
	}

	err = r.loadWavData()
	if err != nil {
		return 0.0, err
	}

	sec := float64(r.WavData.Size) / float64(format.BlockAlign) / float64(format.SampleRate)

	return time.Duration(sec*1000000000) * time.Nanosecond, nil
}

func (r *Reader) Read(p []byte) (n int, err error) {
	err = r.loadWavData()
	if err != nil {
		return n, err
	}

	return r.WavData.Read(p)
}

func (r *Reader) ReadSamples(params ...uint32) (samples []Sample, err error) {
	var bytes []byte
	var numSamples, b, n int

	if len(params) > 0 {
		numSamples = int(params[0])
	} else {
		numSamples = 2048
	}

	format, err := r.Format()
	if err != nil {
		return
	}

	numChannels := int(format.NumChannels)
	blockAlign := int(format.BlockAlign)
	bitsPerSample := int(format.BitsPerSample)

	err = r.loadWavData()
	if err != nil {
		return
	}
	if r.WavData.pos%uint32(blockAlign) != 0 {
		err = errors.New("cannot read samples from a position not aligned to a frame: use Read to finish the current frame first")
		return
	}

	bytes = make([]byte, numSamples*blockAlign)
	n, err = r.Read(bytes)
	if n == 0 {
		return nil, err
	}
	if err == io.EOF {
		err = nil
	}

	numSamples = n / blockAlign
	samples = make([]Sample, numSamples)
	offset := 0

	for i := 0; i < numSamples; i++ {
		for j := 0; j < numChannels; j++ {
			soffset := offset + (j * bitsPerSample / 8)

			switch format.AudioFormat {
			case AudioFormatIEEEFloat:
				bits :=
					uint32((int(bytes[soffset+3]) << 24) +
						(int(bytes[soffset+2]) << 16) +
						(int(bytes[soffset+1]) << 8) +
						int(bytes[soffset]))
				samples[i].Values[j] = int(math.MaxInt32 * math.Float32frombits(bits))

			case AudioFormatALaw:
				var val uint
				pcm := g711.DecodeAlaw(bytes[soffset : soffset+(bitsPerSample/8)])
				for b = 0; b < len(pcm); b++ {
					val += uint(pcm[b]) << uint(b*8)
				}

				samples[i].Values[j] = toInt(val, bitsPerSample*2)

			case AudioFormatMULaw:
				var val uint
				pcm := g711.DecodeUlaw(bytes[soffset : soffset+(bitsPerSample/8)])
				for b = 0; b < len(pcm); b++ {
					val += uint(pcm[b]) << uint(b*8)
				}

				samples[i].Values[j] = toInt(val, bitsPerSample*2)

			default:
				var val uint
				for b = 0; b*8 < bitsPerSample; b++ {
					val += uint(bytes[soffset+b]) << uint(b*8)
				}

				samples[i].Values[j] = toInt(val, bitsPerSample)
			}
		}

		offset += blockAlign
	}

	return
}

func (r *Reader) IntValue(sample Sample, channel uint) int {
	return sample.Values[channel]
}

func (r *Reader) FloatValue(sample Sample, channel uint) float64 {
	return float64(r.IntValue(sample, channel)) / math.Pow(2, float64(r.format.BitsPerSample)-1)
}

func (r *Reader) readFormat() (fmt *WavFormat, err error) {
	fmt = new(WavFormat)

	if err = r.parse(); err != nil {
		return
	}

	var formatChunk *chunkInfo
	for i := range r.chunks {
		if string(r.chunks[i].id) == "fmt " {
			formatChunk = &r.chunks[i]
			break
		}
	}
	if formatChunk == nil {
		err = errors.New("format chunk is not found")
		return
	}
	if formatChunk.size < 16 {
		err = errors.New("format chunk is truncated")
		return
	}

	buf := make([]byte, formatChunk.size)
	if err = readAtFull(formatChunk.r, buf, formatChunk.off); err != nil {
		return
	}
	if err = binary.Read(bytes.NewReader(buf[:16]), binary.LittleEndian, fmt); err != nil {
		return
	}

	return
}

func (r *Reader) loadWavData() error {
	if r.WavData == nil {
		data, err := r.readData()
		if err != nil {
			return err
		}
		r.WavData = data
	}

	return nil
}

func addExpanded(expanded *uint64, add uint64) error {
	if add > math.MaxUint32-*expanded {
		return errors.New("expanded audio size exceeds uint32 range")
	}
	*expanded += add

	return nil
}

func (r *Reader) readData() (data *WavData, err error) {
	if err = r.parse(); err != nil {
		return
	}

	format, err := r.Format()
	if err != nil {
		return
	}
	blockAlign := int(format.BlockAlign)
	if blockAlign <= 0 {
		err = errors.New("invalid block align")
		return
	}

	segments := make([]segment, 0)
	var expanded uint64
	audioRoots := 0

	for i := range r.chunks {
		chunk := &r.chunks[i]
		switch string(chunk.id) {
		case "data":
			audioRoots++
			if uint64(chunk.size)%uint64(blockAlign) != 0 {
				err = errors.New("data chunk size is not a multiple of the block align")
				return
			}
			if err = addExpanded(&expanded, uint64(chunk.size)); err != nil {
				return
			}
			segments = append(segments, segment{
				kind:      segData,
				r:         io.NewSectionReader(chunk.r, chunk.off, int64(chunk.size)),
				remaining: int64(chunk.size),
			})

		case "LIST":
			if chunk.size < 4 {
				err = errors.New("LIST chunk is truncated")
				return
			}
			form := make([]byte, 4)
			if err = readAtFull(chunk.r, form, chunk.off); err != nil {
				return
			}
			if string(form) != "wavl" {
				continue
			}
			audioRoots++
			segments, err = r.parseWavl(chunk, format, segments, &expanded)
			if err != nil {
				return
			}
		}
	}

	if audioRoots == 0 {
		err = errors.New("data chunk is not found")
		return
	}
	if audioRoots > 1 {
		err = errors.New("multiple audio data roots are found")
		return
	}

	hold := make([]byte, blockAlign)
	if format.BitsPerSample == 8 {
		for i := range hold {
			hold[i] = 0x80
		}
	}

	data = &WavData{
		stream: &audioStream{
			segments:   segments,
			blockAlign: blockAlign,
			eightBit:   format.BitsPerSample == 8,
			partial:    make([]byte, 0, blockAlign),
			hold:       hold,
		},
		Size: uint32(expanded),
	}

	return
}

func (r *Reader) parseWavl(chunk *chunkInfo, format *WavFormat, segments []segment, expanded *uint64) ([]segment, error) {
	if format.AudioFormat != AudioFormatPCM {
		return nil, errors.New("wavl LIST is only supported for uncompressed PCM audio")
	}
	if format.NumChannels < 1 || format.NumChannels > 2 {
		return nil, errors.New("wavl LIST supports only mono or stereo audio")
	}
	switch format.BitsPerSample {
	case 8, 16, 24, 32:
	default:
		return nil, errors.New("wavl LIST supports only 8, 16, 24 or 32 bit integer samples")
	}
	blockAlign := int(format.BlockAlign)
	if blockAlign != int(format.NumChannels)*int(format.BitsPerSample)/8 {
		return nil, errors.New("invalid block align")
	}

	listSize := int64(chunk.size)
	pos := int64(4)

	for pos < listSize {
		remaining := listSize - pos
		if remaining < 8 {
			return nil, errors.New("wavl sub-chunk is truncated")
		}

		head := make([]byte, 8)
		if err := readAtFull(chunk.r, head, chunk.off+pos); err != nil {
			return nil, err
		}
		id := string(head[0:4])
		size := int64(binary.LittleEndian.Uint32(head[4:8]))
		padded := size + (size & 1)
		if padded > listSize-pos-8 {
			return nil, errors.New("wavl sub-chunk is truncated")
		}
		childOff := chunk.off + pos + 8
		pos += 8 + padded

		switch id {
		case "data":
			if size%int64(blockAlign) != 0 {
				return nil, errors.New("data chunk size is not a multiple of the block align")
			}
			if err := addExpanded(expanded, uint64(size)); err != nil {
				return nil, err
			}
			segments = append(segments, segment{
				kind:      segData,
				r:         io.NewSectionReader(chunk.r, childOff, size),
				remaining: size,
			})

		case "slnt":
			if size != 4 {
				return nil, errors.New("slnt chunk length must be 4")
			}
			buf := make([]byte, 4)
			if err := readAtFull(chunk.r, buf, childOff); err != nil {
				return nil, err
			}
			frames := binary.LittleEndian.Uint32(buf)
			silentBytes := uint64(frames) * uint64(blockAlign)
			if err := addExpanded(expanded, silentBytes); err != nil {
				return nil, err
			}
			if frames > 0 {
				segments = append(segments, segment{
					kind:   segSilent,
					silent: frames,
				})
			}

		default:
			continue
		}
	}

	return segments, nil
}

func (s *audioStream) Read(p []byte) (n int, err error) {
	for n < len(p) {
		for s.index < len(s.segments) {
			seg := &s.segments[s.index]
			if seg.kind == segData && seg.remaining > 0 {
				break
			}
			if seg.kind == segSilent && seg.silent > 0 {
				break
			}
			s.index++
		}
		if s.index >= len(s.segments) {
			if n > 0 {
				return n, io.EOF
			}
			return 0, io.EOF
		}

		seg := &s.segments[s.index]

		if seg.kind == segSilent {
			if !s.frameReady && s.eightBit {
				for i := range s.hold {
					s.hold[i] = 0x80
				}
			}
			s.frameReady = true

			for n < len(p) && seg.silent > 0 {
				p[n] = s.hold[s.holdOff]
				n++
				s.holdOff++
				if s.holdOff == s.blockAlign {
					s.holdOff = 0
					seg.silent--
				}
			}
			continue
		}

		if len(s.partial) > 0 {
			need := s.blockAlign - len(s.partial)
			if len(p)-n < need {
				need = len(p) - n
			}
			got, readErr := seg.r.Read(p[n : n+need])
			seg.remaining -= int64(got)
			s.partial = append(s.partial, p[n:n+got]...)
			n += got
			if readErr != nil && readErr != io.EOF {
				return n, readErr
			}
			if len(s.partial) == s.blockAlign {
				copy(s.hold, s.partial)
				s.frameReady = true
				s.partial = s.partial[:0]
			}
			if readErr == io.EOF && seg.remaining > 0 {
				return n, io.ErrUnexpectedEOF
			}
			continue
		}

		space := len(p) - n
		if space >= s.blockAlign {
			frames := space / s.blockAlign
			full := frames * s.blockAlign
			got, readErr := seg.r.Read(p[n : n+full])
			seg.remaining -= int64(got)
			n += got
			if readErr != nil && readErr != io.EOF {
				return n, readErr
			}
			if got > 0 {
				complete := (got / s.blockAlign) * s.blockAlign
				if complete > 0 {
					copy(s.hold, p[n-s.blockAlign:n])
					s.frameReady = true
				}
				s.partial = append(s.partial[:0], p[n-(got-complete):n]...)
			}
			if readErr == io.EOF && seg.remaining > 0 {
				return n, io.ErrUnexpectedEOF
			}
			continue
		}

		got, readErr := seg.r.Read(p[n:])
		seg.remaining -= int64(got)
		n += got
		if readErr != nil && readErr != io.EOF {
			return n, readErr
		}
		if got > 0 {
			s.partial = append(s.partial, p[n-got:n]...)
		}
		if len(s.partial) == s.blockAlign {
			copy(s.hold, s.partial)
			s.frameReady = true
			s.partial = s.partial[:0]
		}
		if readErr == io.EOF && seg.remaining > 0 {
			return n, io.ErrUnexpectedEOF
		}
	}

	return n, nil
}

func toInt(value uint, bits int) int {
	var result int

	switch bits {
	case 32:
		result = int(int32(value))
	case 16:
		result = int(int16(value))
	case 8:
		result = int(value)
	default:
		msb := uint(1 << (uint(bits) - 1))

		if value >= msb {
			result = -int((1 << uint(bits)) - value)
		} else {
			result = int(value)
		}
	}

	return result
}
