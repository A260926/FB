package wav

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math/bits"
	"os"
	"time"

	"github.com/go-audio/audio"
	"github.com/go-audio/riff"
)

// Encoder encodes LPCM data into a wav containter.
type Encoder struct {
	w   io.WriteSeeker
	buf *bytes.Buffer

	SampleRate int
	BitDepth   int
	NumChans   int

	// A number indicating the WAVE format category of the file. The content of
	// the <format-specific-fields> portion of the ‘fmt’ chunk, and the
	// interpretation of the waveform data, depend on this value. PCM = 1 (i.e.
	// Linear quantization) Values other than 1 indicate some form of
	// compression.
	// Set this to 0xfffe (WAVE_FORMAT_EXTENSIBLE) to write an extensible fmt
	// chunk carrying ValidBitsPerSample and ChannelMask.
	WavAudioFormat int

	// ValidBitsPerSample is the number of bits actually used per sample when
	// encoding a WAVE_FORMAT_EXTENSIBLE (0xfffe) file. BitDepth stays the
	// container width (16, 24 or 32). IntBuffer.Data values are shifted into
	// the high ValidBitsPerSample bits of the container and the remaining low
	// bits are written as zero. It must be between 1 and BitDepth and has to be
	// set before the first Write call. It is ignored for regular PCM files.
	ValidBitsPerSample uint16
	// ChannelMask describes the speaker layout of the channels when encoding
	// a WAVE_FORMAT_EXTENSIBLE (0xfffe) file. A zero value means the layout is
	// unspecified; otherwise the number of set bits must equal NumChans. It is
	// ignored for regular PCM files.
	ChannelMask uint32

	// Metadata contains metadata to inject in the file.
	Metadata *Metadata

	WrittenBytes    int
	frames          int
	pcmChunkStarted bool
	pcmChunkSizePos int
	wroteHeader     bool // true if we've written the header out
}

// NewEncoder creates a new encoder to create a new wav file.
// Don't forget to add Frames to the encoder before writing.
// Set audioFormat to 0xfffe to produce a WAVE_FORMAT_EXTENSIBLE file, in which
// case BitDepth is the container width and the ValidBitsPerSample and
// ChannelMask fields have to be populated before the first Write call.
func NewEncoder(w io.WriteSeeker, sampleRate, bitDepth, numChans, audioFormat int) *Encoder {
	return &Encoder{
		w:              w,
		buf:            bytes.NewBuffer(make([]byte, 0, bytesNumFromDuration(time.Minute, sampleRate, bitDepth)*numChans)),
		SampleRate:     sampleRate,
		BitDepth:       bitDepth,
		NumChans:       numChans,
		WavAudioFormat: audioFormat,
	}
}

// AddLE serializes and adds the passed value using little endian
func (e *Encoder) AddLE(src interface{}) error {
	e.WrittenBytes += binary.Size(src)
	return binary.Write(e.w, binary.LittleEndian, src)
}

// AddBE serializes and adds the passed value using big endian
func (e *Encoder) AddBE(src interface{}) error {
	e.WrittenBytes += binary.Size(src)
	return binary.Write(e.w, binary.BigEndian, src)
}

func (e *Encoder) extensible() bool {
	return e.WavAudioFormat == int(waveFormatExtensible)
}

// validateWriteBuffer verifies the encoder parameters and the input buffer
// without touching the underlying writer, so a rejected Write leaves the
// encoder in a state where a later, valid Write still succeeds.
func (e *Encoder) validateWriteBuffer(buf *audio.IntBuffer) error {
	if e == nil {
		return fmt.Errorf("can't write with a nil encoder")
	}
	if e.w == nil {
		return fmt.Errorf("can't write to a nil writer")
	}
	if buf == nil {
		return fmt.Errorf("can't add a nil buffer")
	}
	if buf.Format == nil {
		return fmt.Errorf("can't write a buffer without a format")
	}
	if buf.Format.NumChannels != e.NumChans {
		return fmt.Errorf("buffer channel count %d doesn't match the encoder channel count %d",
			buf.Format.NumChannels, e.NumChans)
	}
	if buf.Format.SampleRate != e.SampleRate {
		return fmt.Errorf("buffer sample rate %d doesn't match the encoder sample rate %d",
			buf.Format.SampleRate, e.SampleRate)
	}
	if e.NumChans < 1 {
		return fmt.Errorf("invalid number of channels: %d", e.NumChans)
	}
	if e.SampleRate < 1 {
		return fmt.Errorf("invalid sample rate: %d", e.SampleRate)
	}
	switch e.BitDepth {
	case 8, 16, 24, 32:
	default:
		return fmt.Errorf("can't encode samples of bit size %d, supported container sizes are 8, 16, 24 and 32", e.BitDepth)
	}
	if len(buf.Data)%e.NumChans != 0 {
		return fmt.Errorf("buffer of %d samples doesn't contain an integer number of %d channel frames",
			len(buf.Data), e.NumChans)
	}

	validBits := uint16(e.BitDepth)
	if e.extensible() {
		// Only integer PCM containers are supported in extensible mode.
		if e.BitDepth != 16 && e.BitDepth != 24 && e.BitDepth != 32 {
			return fmt.Errorf("WAVE_FORMAT_EXTENSIBLE only supports 16, 24 or 32 bit containers, got %d", e.BitDepth)
		}
		if e.ValidBitsPerSample < 1 || e.ValidBitsPerSample > uint16(e.BitDepth) {
			return fmt.Errorf("invalid valid bits per sample: %d for a %d bit container",
				e.ValidBitsPerSample, e.BitDepth)
		}
		validBits = e.ValidBitsPerSample
		if e.ChannelMask != 0 && bits.OnesCount32(e.ChannelMask) != e.NumChans {
			return fmt.Errorf("channel mask 0x%x sets %d channel(s) but the encoder declares %d",
				e.ChannelMask, bits.OnesCount32(e.ChannelMask), e.NumChans)
		}
	}

	// Make sure every sample fits in the effective bit depth.
	if e.BitDepth == 8 {
		// 8-bit WAV PCM is unsigned, every other container width is signed.
		for _, v := range buf.Data {
			if v < 0 || v > 255 {
				return fmt.Errorf("sample value %d doesn't fit in an unsigned 8 bit container (range [0, 255])", v)
			}
		}
		return nil
	}
	limit := 1 << (validBits - 1)
	for _, v := range buf.Data {
		if v < -limit || v >= limit {
			return fmt.Errorf("sample value %d doesn't fit in %d valid bits (range [%d, %d])",
				v, validBits, -limit, limit-1)
		}
	}
	return nil
}

// shift returns the left-alignment shift applied to extensible samples.
func (e *Encoder) shift() uint {
	if !e.extensible() || e.ValidBitsPerSample == 0 || int(e.ValidBitsPerSample) == e.BitDepth {
		return 0
	}
	return uint(e.BitDepth - int(e.ValidBitsPerSample))
}

func (e *Encoder) addBuffer(buf *audio.IntBuffer) error {
	frameCount := buf.NumFrames()
	shift := e.shift()
	// performance tweak: setup a buffer so we don't do too many writes
	var err error
	for i := 0; i < frameCount; i++ {
		for j := 0; j < buf.Format.NumChannels; j++ {
			v := buf.Data[i*buf.Format.NumChannels+j]
			if shift > 0 {
				// Left-align the effective-depth integer into its container, the
				// low padding bits are emitted as zero.
				v <<= shift
			}
			switch e.BitDepth {
			case 8:
				if err = binary.Write(e.buf, binary.LittleEndian, uint8(v)); err != nil {
					return err
				}
			case 16:
				if err = binary.Write(e.buf, binary.LittleEndian, int16(v)); err != nil {
					return err
				}
			case 24:
				if err = binary.Write(e.buf, binary.LittleEndian, audio.Int32toInt24LEBytes(int32(v))); err != nil {
					return err
				}
			case 32:
				if err = binary.Write(e.buf, binary.LittleEndian, int32(v)); err != nil {
					return err
				}
			default:
				return fmt.Errorf("can't add frames of bit size %d", e.BitDepth)
			}
		}
		e.frames++
	}
	if n, err := e.w.Write(e.buf.Bytes()); err != nil {
		e.WrittenBytes += n
		return err
	}
	e.WrittenBytes += e.buf.Len()
	e.buf.Reset()

	return nil
}

func (e *Encoder) writeHeader() error {
	if e.wroteHeader {
		return errors.New("already wrote header")
	}
	e.wroteHeader = true
	if e == nil {
		return fmt.Errorf("can't write a nil encoder")
	}
	if e.w == nil {
		return fmt.Errorf("can't write to a nil writer")
	}

	if e.WrittenBytes > 0 {
		return nil
	}

	// riff ID
	if err := e.AddLE(riff.RiffID); err != nil {
		return err
	}
	// file size uint32, to update later on.
	if err := e.AddLE(uint32(4294967295)); err != nil {
		return err
	}
	// wave headers
	if err := e.AddLE(riff.WavFormatID); err != nil {
		return err
	}
	// form
	if err := e.AddLE(riff.FmtID); err != nil {
		return err
	}
	// chunk size: 16 standard bytes, plus 24 extension bytes for extensible
	fmtChunkSize := uint32(16)
	if e.extensible() {
		fmtChunkSize = 40
	}
	if err := e.AddLE(fmtChunkSize); err != nil {
		return err
	}
	// wave format
	if err := e.AddLE(uint16(e.WavAudioFormat)); err != nil {
		return err
	}
	// num channels
	if err := e.AddLE(uint16(e.NumChans)); err != nil {
		return fmt.Errorf("error encoding the number of channels - %w", err)
	}
	// samplerate
	if err := e.AddLE(uint32(e.SampleRate)); err != nil {
		return fmt.Errorf("error encoding the sample rate - %w", err)
	}
	blockAlign := e.NumChans * e.BitDepth / 8
	// avg bytes per sec
	if err := e.AddLE(uint32(e.SampleRate * blockAlign)); err != nil {
		return fmt.Errorf("error encoding the avg bytes per sec - %w", err)
	}
	// block align
	if err := e.AddLE(uint16(blockAlign)); err != nil {
		return err
	}
	// bits per sample (container width)
	if err := e.AddLE(uint16(e.BitDepth)); err != nil {
		return fmt.Errorf("error encoding bits per sample - %w", err)
	}

	if e.extensible() {
		// extension size, always 22 bytes for the PCM subformat
		if err := e.AddLE(uint16(22)); err != nil {
			return fmt.Errorf("error encoding the extension size - %w", err)
		}
		// number of bits actually carrying signal
		if err := e.AddLE(e.ValidBitsPerSample); err != nil {
			return fmt.Errorf("error encoding valid bits per sample - %w", err)
		}
		// speaker layout, 0 means unspecified
		if err := e.AddLE(e.ChannelMask); err != nil {
			return fmt.Errorf("error encoding the channel mask - %w", err)
		}
		// PCM subformat GUID
		if err := e.AddLE(pcmSubformatGUID); err != nil {
			return fmt.Errorf("error encoding the subformat GUID - %w", err)
		}
	}

	return nil
}

// Write encodes and writes the passed buffer to the underlying writer.
// Each call must carry complete channel frames. Don't forget to Close() the
// encoder or the file won't be valid. Invalid parameters or samples make the
// call fail before any byte is written, leaving the encoder still usable.
func (e *Encoder) Write(buf *audio.IntBuffer) error {
	if err := e.validateWriteBuffer(buf); err != nil {
		return err
	}
	if !e.wroteHeader {
		if err := e.writeHeader(); err != nil {
			return err
		}
	}

	if !e.pcmChunkStarted {
		// sound header
		if err := e.AddLE(riff.DataFormatID); err != nil {
			return fmt.Errorf("error encoding sound header %w", err)
		}
		e.pcmChunkStarted = true

		// write a temporary chunksize
		e.pcmChunkSizePos = e.WrittenBytes
		if err := e.AddLE(uint32(4294967295)); err != nil {
			return fmt.Errorf("%w when writing wav data chunk size header", err)
		}
	}

	return e.addBuffer(buf)
}

// WriteFrame writes a single frame of data to the underlying writer.
// WAVE_FORMAT_EXTENSIBLE encoders reject it because a raw frame carries no
// notion of valid bits; use Write with an audio.IntBuffer instead.
func (e *Encoder) WriteFrame(value interface{}) error {
	if e.extensible() {
		return errors.New("WriteFrame is not supported for WAVE_FORMAT_EXTENSIBLE files, use Write with an audio.IntBuffer")
	}
	if !e.wroteHeader {
		e.writeHeader()
	}
	if !e.pcmChunkStarted {
		// sound header
		if err := e.AddLE(riff.DataFormatID); err != nil {
			return fmt.Errorf("error encoding sound header %w", err)
		}
		e.pcmChunkStarted = true

		// write a temporary chunksize
		e.pcmChunkSizePos = e.WrittenBytes
		if err := e.AddLE(uint32(4294967295)); err != nil {
			return fmt.Errorf("%w when writing wav data chunk size header", err)
		}
	}

	e.frames++
	return e.AddLE(value)
}

func (e *Encoder) writeMetadata() error {
	chunkData := encodeInfoChunk(e)
	if err := e.AddBE(CIDList); err != nil {
		return fmt.Errorf("failed to write the LIST chunk ID: %w", err)
	}
	if err := e.AddLE(uint32(len(chunkData))); err != nil {
		return fmt.Errorf("failed to write the LIST chunk size: %w", err)
	}
	return e.AddBE(chunkData)
}

// Close flushes the content to disk, make sure the headers are up to date
// Note that the underlying writer is NOT being closed.
func (e *Encoder) Close() error {
	if e == nil || e.w == nil {
		return nil
	}

	// inject metadata at the end to not trip implementation not supporting
	// metadata chunks
	if e.Metadata != nil {
		if err := e.writeMetadata(); err != nil {
			return fmt.Errorf("failed to write metadata - %w", err)
		}
	}

	// go back and write total size in header
	if _, err := e.w.Seek(4, 0); err != nil {
		return err
	}
	if err := e.AddLE(uint32(e.WrittenBytes) - 8); err != nil {
		return fmt.Errorf("%w when writing the total written bytes", err)
	}

	// rewrite the audio chunk length header
	if e.pcmChunkSizePos > 0 {
		if _, err := e.w.Seek(int64(e.pcmChunkSizePos), 0); err != nil {
			return err
		}
		chunksize := uint32((int(e.BitDepth) / 8) * int(e.NumChans) * e.frames)
		if err := e.AddLE(uint32(chunksize)); err != nil {
			return fmt.Errorf("%w when writing wav data chunk size header", err)
		}
	}

	// jump back to the end of the file.
	if _, err := e.w.Seek(0, 2); err != nil {
		return err
	}
	switch e.w.(type) {
	case *os.File:
		return e.w.(*os.File).Sync()
	}
	return nil
}
