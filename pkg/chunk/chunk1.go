package chunk

import (
	"io"
)

// Chunk1 is a type 1 chunk.
// The message stream ID is not
// included; this chunk takes the same stream ID as the preceding chunk.
// Streams with variable-sized messages (for example, many video
// formats) SHOULD use this format for the first chunk of each new
// message after the first.
type Chunk1 struct {
	ChunkStreamID  uint32
	TimestampDelta uint32
	BodyLen        uint32
	Type           uint8
	Body           []byte
}

// GetBody returns the chunk body.
func (c Chunk1) GetBody() []byte {
	return c.Body
}

// Read reads the chunk.
func (c *Chunk1) Read(r io.Reader, maxBodyLen uint32, _ bool) error {
	_, chunkStreamID, err := readBasicHeader(r)
	if err != nil {
		return err
	}
	c.ChunkStreamID = chunkStreamID

	header := make([]byte, 7)
	_, err = io.ReadFull(r, header)
	if err != nil {
		return err
	}

	c.TimestampDelta = uint32(header[0])<<16 | uint32(header[1])<<8 | uint32(header[2])
	c.BodyLen = uint32(header[3])<<16 | uint32(header[4])<<8 | uint32(header[5])
	c.Type = header[6]

	if c.TimestampDelta >= 0xFFFFFF {
		var extendedTimestamp [4]byte
		_, err = io.ReadFull(r, extendedTimestamp[:])
		if err != nil {
			return err
		}

		c.TimestampDelta = uint32(extendedTimestamp[0])<<24 | uint32(extendedTimestamp[1])<<16 |
			uint32(extendedTimestamp[2])<<8 | uint32(extendedTimestamp[3])
	}

	chunkBodyLen := min(c.BodyLen, maxBodyLen)

	c.Body = make([]byte, chunkBodyLen)
	_, err = io.ReadFull(r, c.Body)
	return err
}

func (c Chunk1) marshalSize() int {
	n := basicHeaderLength(c.ChunkStreamID) + 7 + len(c.Body)
	if c.TimestampDelta >= 0xFFFFFF {
		n += 4
	}
	return n
}

// MarshalHeaderTo writes the chunk header, that is the chunk without its body, into buf.
// ChunkStreamID must be a valid chunk stream ID (2 through 65599); no validation is
// performed by this method.
// buf must be large enough to contain the header (14 bytes are always enough).
// It returns the number of bytes written.
func (c Chunk1) MarshalHeaderTo(buf []byte, _ bool) int {
	n := marshalBasicHeader(buf, 1, c.ChunkStreamID)
	buf[n] = byte(c.TimestampDelta >> 16)
	buf[n+1] = byte(c.TimestampDelta >> 8)
	buf[n+2] = byte(c.TimestampDelta)
	buf[n+3] = byte(c.BodyLen >> 16)
	buf[n+4] = byte(c.BodyLen >> 8)
	buf[n+5] = byte(c.BodyLen)
	buf[n+6] = c.Type

	if c.TimestampDelta >= 0xFFFFFF {
		buf[n] = 0xFF
		buf[n+1] = 0xFF
		buf[n+2] = 0xFF
		buf[n+7] = byte(c.TimestampDelta >> 24)
		buf[n+8] = byte(c.TimestampDelta >> 16)
		buf[n+9] = byte(c.TimestampDelta >> 8)
		buf[n+10] = byte(c.TimestampDelta)
		return n + 11
	}

	return n + 7
}

// Marshal writes the chunk.
//
// Deprecated: not used anymore.
func (c Chunk1) Marshal(_ bool) ([]byte, error) {
	err := ValidateChunkStreamID(c.ChunkStreamID)
	if err != nil {
		return nil, err
	}

	buf := make([]byte, c.marshalSize())
	n := c.MarshalHeaderTo(buf, false)
	copy(buf[n:], c.Body)
	return buf, nil
}
