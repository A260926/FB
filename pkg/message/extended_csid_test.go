package message_test

import (
	"bytes"
	"io"
	"strconv"
	"testing"
	"time"

	"github.com/stretchr/testify/require"

	"github.com/bluenviron/gortmplib/pkg/bytecounter"
	"github.com/bluenviron/gortmplib/pkg/message"
)

// The tests in this file use independent, hand-written RTMP byte samples.
// They do not rely on the chunk writer to exercise the chunk reader and vice versa.

func testBasicHeader(typ byte, id uint32) []byte {
	switch {
	case id <= 63:
		return []byte{typ<<6 | byte(id)}

	case id <= 319:
		return []byte{typ << 6, byte(id - 64)}

	default:
		v := id - 64
		return []byte{typ<<6 | 1, byte(v), byte(v >> 8)}
	}
}

func testBE24(v uint32) []byte {
	return []byte{byte(v >> 16), byte(v >> 8), byte(v)}
}

func testBE32(v uint32) []byte {
	return []byte{byte(v >> 24), byte(v >> 16), byte(v >> 8), byte(v)}
}

func testChunk0(csid, timestamp, bodyLen uint32, typ byte, messageStreamID uint32, body []byte) []byte {
	buf := append([]byte{}, testBasicHeader(0, csid)...)
	buf = append(buf, testBE24(timestamp)...)
	buf = append(buf, testBE24(bodyLen)...)
	buf = append(buf, typ)
	buf = append(buf, testBE32(messageStreamID)...)

	if timestamp >= 0xFFFFFF {
		buf = append(buf, testBE32(timestamp)...)
	}

	return append(buf, body...)
}

func testChunk1(csid, timestampDelta, bodyLen uint32, typ byte, body []byte) []byte {
	buf := append([]byte{}, testBasicHeader(1, csid)...)
	buf = append(buf, testBE24(timestampDelta)...)
	buf = append(buf, testBE24(bodyLen)...)
	buf = append(buf, typ)

	if timestampDelta >= 0xFFFFFF {
		buf = append(buf, testBE32(timestampDelta)...)
	}

	return append(buf, body...)
}

func testChunk2(csid, timestampDelta uint32, body []byte) []byte {
	buf := append([]byte{}, testBasicHeader(2, csid)...)
	buf = append(buf, testBE24(timestampDelta)...)

	if timestampDelta >= 0xFFFFFF {
		buf = append(buf, testBE32(timestampDelta)...)
	}

	return append(buf, body...)
}

func testChunk3(csid uint32, body []byte, extendedTimestamp *uint32) []byte {
	buf := append([]byte{}, testBasicHeader(3, csid)...)
	if extendedTimestamp != nil {
		buf = append(buf, testBE32(*extendedTimestamp)...)
	}
	return append(buf, body...)
}

func testMPEG4AudioBody(size int, fill byte) []byte {
	buf := make([]byte, size)
	buf[0] = 0xAF // MPEG-4 audio, 44.1 kHz, 16 bit, stereo
	buf[1] = byte(message.AudioAACTypeAU)
	for i := 2; i < size; i++ {
		buf[i] = fill
	}
	return buf
}

func testH264VideoBody(size int, fill byte) []byte {
	buf := make([]byte, size)
	buf[0] = 0x17 // key frame, H264
	buf[1] = byte(message.VideoPacketTypeAU)
	buf[2] = 0
	buf[3] = 0
	buf[4] = 0
	for i := 5; i < size; i++ {
		buf[i] = fill
	}
	return buf
}

func newTestMessageReader(br io.Reader) *message.Reader {
	bc := bytecounter.NewReader(br)
	r := &message.Reader{
		BR:  bc,
		BCR: bc,
	}
	r.Initialize()
	return r
}

func readOneTestMessage(t *testing.T, r *message.Reader) message.Message {
	t.Helper()
	msg, err := r.Read()
	require.NoError(t, err)
	return msg
}

func TestExtendedCSIDIntegration(t *testing.T) {
	t.Run("basic header boundaries", func(t *testing.T) {
		for _, ca := range []struct {
			id    uint32
			basic []byte
		}{
			{63, []byte{0x3F}},
			{64, []byte{0x00, 0x00}},
			{319, []byte{0x00, 0xFF}},
			{320, []byte{0x01, 0x00, 0x01}},
			{65599, []byte{0x01, 0xFF, 0xFF}},
		} {
			body := []byte{0xAF, 0x01, 0xAA, 0xBB}
			buf := append([]byte{}, ca.basic...)
			buf = append(buf, testBE24(7)...)
			buf = append(buf, testBE24(uint32(len(body)))...)
			buf = append(buf, 8)
			buf = append(buf, testBE32(5)...)
			buf = append(buf, body...)

			msg := readOneTestMessage(t, newTestMessageReader(bytes.NewReader(buf)))
			audio := msg.(*message.Audio)
			require.Equal(t, ca.id, audio.ChunkStreamID)
			require.Equal(t, 7*time.Millisecond, audio.DTS)
			require.Equal(t, uint32(5), audio.MessageStreamID)
			require.Equal(t, []byte{0xAA, 0xBB}, audio.AU)
		}
	})

	t.Run("non-minimal three-byte header", func(t *testing.T) {
		for _, ca := range []struct {
			id    uint32
			basic []byte
		}{
			{64, []byte{0x01, 0x00, 0x00}},
			{319, []byte{0x01, 0xFF, 0x00}},
		} {
			body := []byte{0xAF, 0x01, 0x01, 0x02}
			buf := append([]byte{}, ca.basic...)
			buf = append(buf, testBE24(1)...)
			buf = append(buf, testBE24(uint32(len(body)))...)
			buf = append(buf, 8)
			buf = append(buf, testBE32(1)...)
			buf = append(buf, body...)

			msg := readOneTestMessage(t, newTestMessageReader(bytes.NewReader(buf)))
			require.Equal(t, ca.id, msg.(*message.Audio).ChunkStreamID)
		}
	})

	t.Run("truncated extended basic header", func(t *testing.T) {
		for _, buf := range [][]byte{
			{0x00},
			{0x01},
			{0x01, 0x00},
		} {
			_, err := newTestMessageReader(bytes.NewReader(buf)).Read()
			require.Error(t, err)
		}
	})

	t.Run("all four chunk header types with one extended csid", func(t *testing.T) {
		b1 := testMPEG4AudioBody(4, 0x11)
		b2 := testMPEG4AudioBody(4, 0x22)
		b3 := testMPEG4AudioBody(4, 0x33)
		b4 := testMPEG4AudioBody(4, 0x44)

		var buf []byte
		buf = append(buf, testChunk0(64, 100, 4, 8, 1, b1)...)
		buf = append(buf, testChunk1(64, 10, 4, 8, b2)...)
		buf = append(buf, testChunk2(64, 20, b3)...)
		buf = append(buf, testChunk3(64, b4, nil)...)

		r := newTestMessageReader(bytes.NewReader(buf))

		for i, ca := range []struct {
			timestamp time.Duration
			body      []byte
		}{
			{100 * time.Millisecond, b1},
			{110 * time.Millisecond, b2},
			{130 * time.Millisecond, b3},
			{150 * time.Millisecond, b4},
		} {
			msg := readOneTestMessage(t, r).(*message.Audio)
			require.Equal(t, uint32(64), msg.ChunkStreamID)
			require.Equal(t, uint32(1), msg.MessageStreamID)
			require.Equal(t, ca.timestamp, msg.DTS, "message %d", i)
			require.Equal(t, ca.body[2:], msg.AU, "message %d", i)
		}
	})

	t.Run("two extended streams interleaved mid-fragment", func(t *testing.T) {
		audioBody := testMPEG4AudioBody(130, 0x61)
		videoBody := testH264VideoBody(130, 0x62)

		var buf []byte
		buf = append(buf, testChunk0(64, 1000, 130, 8, 1, audioBody[:128])...)
		buf = append(buf, testChunk0(320, 5000, 130, 9, 2, videoBody[:128])...)
		buf = append(buf, testChunk3(64, audioBody[128:], nil)...)
		buf = append(buf, testChunk3(320, videoBody[128:], nil)...)

		r := newTestMessageReader(bytes.NewReader(buf))

		audio := readOneTestMessage(t, r).(*message.Audio)
		require.Equal(t, uint32(64), audio.ChunkStreamID)
		require.Equal(t, uint32(1), audio.MessageStreamID)
		require.Equal(t, 1000*time.Millisecond, audio.DTS)
		require.Equal(t, audioBody[2:], audio.AU)

		video := readOneTestMessage(t, r).(*message.Video)
		require.Equal(t, uint32(320), video.ChunkStreamID)
		require.Equal(t, uint32(2), video.MessageStreamID)
		require.Equal(t, 5000*time.Millisecond, video.DTS)
		require.Equal(t, videoBody[5:], video.AU)
	})

	t.Run("writer emits shortest basic headers and isolates extended streams", func(t *testing.T) {
		var out bytes.Buffer
		bcw := bytecounter.NewWriter(&out)
		w := &message.Writer{
			BW:  bcw,
			BCW: bcw,
		}
		w.Initialize()

		type step struct {
			msg      message.Message
			expected []byte
		}

		audio := func(csid uint32, dts time.Duration, msid uint32, au []byte) message.Message {
			return &message.Audio{
				ChunkStreamID:   csid,
				DTS:             dts,
				MessageStreamID: msid,
				Codec:           message.CodecMPEG1Audio,
				AU:              au,
			}
		}

		steps := []step{
			// CSID 64
			{audio(64, 0, 1, []byte{1}), []byte{
				0x00, 0x00,
				0x00, 0x00, 0x00,
				0x00, 0x00, 0x02,
				0x08,
				0x00, 0x00, 0x00, 0x01,
			}},
			// CSID 320
			{audio(320, 0, 2, []byte{2}), []byte{
				0x01, 0x00, 0x01,
				0x00, 0x00, 0x00,
				0x00, 0x00, 0x02,
				0x08,
				0x00, 0x00, 0x00, 0x02,
			}},
			// CSID 64, type 1 (body length changed)
			{audio(64, 100*time.Millisecond, 1, []byte{3, 4}), []byte{
				0x40, 0x00,
				0x00, 0x00, 0x64,
				0x00, 0x00, 0x03,
				0x08,
			}},
			// CSID 320, type 1 (body length changed)
			{audio(320, 100*time.Millisecond, 2, []byte{5, 6}), []byte{
				0x41, 0x00, 0x01,
				0x00, 0x00, 0x64,
				0x00, 0x00, 0x03,
				0x08,
			}},
			// CSID 64, type 2 (timestamp delta changed)
			{audio(64, 150*time.Millisecond, 1, []byte{7, 8}), []byte{
				0x80, 0x00,
				0x00, 0x00, 0x32,
			}},
			// CSID 320, type 2 (timestamp delta changed)
			{audio(320, 150*time.Millisecond, 2, []byte{9, 10}), []byte{
				0x81, 0x00, 0x01,
				0x00, 0x00, 0x32,
			}},
			// CSID 64, type 3 (same type, length and delta)
			{audio(64, 200*time.Millisecond, 1, []byte{11, 12}), []byte{
				0xC0, 0x00,
			}},
			// CSID 320, type 3
			{audio(320, 200*time.Millisecond, 2, []byte{13, 14}), []byte{
				0xC1, 0x00, 0x01,
			}},
		}

		for i, st := range steps {
			start := out.Len()
			err := w.Write(st.msg)
			require.NoError(t, err, "step %d", i)

			written := out.Bytes()[start:]
			require.GreaterOrEqual(t, len(written), len(st.expected))
			require.Equal(t, st.expected, written[:len(st.expected)], "step %d", i)
		}

		// read back all messages, in order
		r := newTestMessageReader(bytes.NewReader(out.Bytes()))
		for i, st := range steps {
			audio := readOneTestMessage(t, r).(*message.Audio)
			src := st.msg.(*message.Audio)
			require.Equal(t, src.ChunkStreamID, audio.ChunkStreamID, "step %d", i)
			require.Equal(t, src.DTS, audio.DTS, "step %d", i)
			require.Equal(t, src.MessageStreamID, audio.MessageStreamID, "step %d", i)
			require.Equal(t, src.AU, audio.AU, "step %d", i)
		}
	})

	t.Run("long basic header with extended timestamp", func(t *testing.T) {
		timestamp := uint32(0xFFFFFF)
		body := testMPEG4AudioBody(131, 0x61)

		var out bytes.Buffer
		bcw := bytecounter.NewWriter(&out)
		w := &message.Writer{
			BW:  bcw,
			BCW: bcw,
		}
		w.Initialize()

		err := w.Write(&message.Audio{
			ChunkStreamID:   65599,
			DTS:             time.Duration(timestamp) * time.Millisecond,
			MessageStreamID: 3,
			Codec:           message.CodecMPEG4Audio,
			Rate:            message.AudioRate44100,
			Depth:           message.AudioDepth16,
			IsStereo:        true,
			AACType:         message.AudioAACTypeAU,
			AU:              body[2:],
		})
		require.NoError(t, err)

		expected := []byte{
			0x01, 0xFF, 0xFF, // three-byte basic header
			0xFF, 0xFF, 0xFF, // timestamp sentinel
			0x00, 0x00, 0x83, // body length
			0x08,                   // type
			0x00, 0x00, 0x00, 0x03, // message stream ID
			0x00, 0xFF, 0xFF, 0xFF, // extended timestamp
		}
		require.Equal(t, expected, out.Bytes()[:len(expected)])

		// continuation chunk is a type 3 chunk with the extended timestamp
		chunk3 := testChunk3(65599, body[128:], &timestamp)
		require.Equal(t, chunk3, out.Bytes()[len(expected)+128:])

		r := newTestMessageReader(bytes.NewReader(out.Bytes()))
		audio := readOneTestMessage(t, r).(*message.Audio)
		require.Equal(t, uint32(65599), audio.ChunkStreamID)
		require.Equal(t, time.Duration(timestamp)*time.Millisecond, audio.DTS)
		require.Equal(t, uint32(3), audio.MessageStreamID)
		require.Equal(t, body[2:], audio.AU)
	})

	t.Run("reader returns same data one byte at a time", func(t *testing.T) {
		timestamp := uint32(0xFFFFFF)
		body := testMPEG4AudioBody(130, 0x71)

		var buf []byte
		buf = append(buf, testChunk0(65599, timestamp, 130, 8, 4, body[:128])...)
		buf = append(buf, testChunk3(65599, body[128:], &timestamp)...)

		normal := readOneTestMessage(t, newTestMessageReader(bytes.NewReader(buf)))
		short := readOneTestMessage(t, newTestMessageReader(&oneByteReader{b: buf}))

		require.Equal(t, normal, short)
	})

	t.Run("abort targets an extended stream only", func(t *testing.T) {
		audio64 := testMPEG4AudioBody(130, 0xA1)
		audio320 := testMPEG4AudioBody(130, 0xB2)
		newAudio64 := testMPEG4AudioBody(4, 0xC3)

		var buf []byte
		// two messages left incomplete
		buf = append(buf, testChunk0(64, 1000, 130, 8, 1, audio64[:128])...)
		buf = append(buf, testChunk0(320, 2000, 130, 8, 2, audio320[:128])...)

		// abort message, whose own chunk stream ID is 2 while the target is 64
		abortBody := testBE32(64)
		buf = append(buf, testChunk0(2, 0, uint32(len(abortBody)), uint8(message.TypeAbortMessage), 0, abortBody)...)

		// stream 320 completes, stream 64 starts again with a full header
		buf = append(buf, testChunk3(320, audio320[128:], nil)...)
		buf = append(buf, testChunk0(64, 3000, 4, 8, 1, newAudio64)...)

		r := newTestMessageReader(bytes.NewReader(buf))

		abort := readOneTestMessage(t, r).(*message.AbortMessage)
		require.Equal(t, uint32(64), abort.ChunkStreamID)

		msg320 := readOneTestMessage(t, r).(*message.Audio)
		require.Equal(t, uint32(320), msg320.ChunkStreamID)
		require.Equal(t, uint32(2), msg320.MessageStreamID)
		require.Equal(t, 2000*time.Millisecond, msg320.DTS)
		require.Equal(t, audio320[2:], msg320.AU)

		msg64 := readOneTestMessage(t, r).(*message.Audio)
		require.Equal(t, uint32(64), msg64.ChunkStreamID)
		require.Equal(t, uint32(1), msg64.MessageStreamID)
		require.Equal(t, 3000*time.Millisecond, msg64.DTS)
		require.Equal(t, newAudio64[2:], msg64.AU)
	})

	t.Run("invalid csid is rejected before writing", func(t *testing.T) {
		for _, id := range []uint32{0, 1, 65599 + 1} {
			var out bytes.Buffer
			bcw := bytecounter.NewWriter(&out)
			w := &message.Writer{
				BW:  bcw,
				BCW: bcw,
			}
			w.Initialize()

			err := w.Write(&message.Audio{
				ChunkStreamID:   id,
				MessageStreamID: 1,
				Codec:           message.CodecMPEG1Audio,
				AU:              []byte{1},
			})
			require.EqualError(t, err, "invalid chunk stream ID: "+strconv.FormatUint(uint64(id), 10))
			require.Zero(t, out.Len())
		}
	})
}

type oneByteReader struct {
	b []byte
}

func (r *oneByteReader) Read(p []byte) (int, error) {
	if len(r.b) == 0 {
		return 0, io.EOF
	}
	if len(p) == 0 {
		return 0, nil
	}
	p[0] = r.b[0]
	r.b = r.b[1:]
	return 1, nil
}
