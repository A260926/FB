package chunk

import (
	"io"
)

// Chunk2 is a type 2 chunk.
// Neither the stream ID nor the
// message length is included; this chunk has the same stream ID and
// message length as the preceding chunk.
type Chunk2 struct {
	ChunkStreamID  uint32
	TimestampDelta uint32
	Body           []byte
}

// GetBody returns the chunk body.
func (c Chunk2) GetBody() []byte {
	return c.Body
}

// Read reads the chunk.
func (c *Chunk2) Read(r io.Reader, bodyLen uint32, _ bool) error {
	_, chunkStreamID, err := readBasicHeader(r)
	if err != nil {
		return err
	}
	c.ChunkStreamID = chunkStreamID

	header := make([]byte, 3)
	_, err = io.ReadFull(r, header)
	if err != nil {
		return err
	}

	c.TimestampDelta = uint32(header[0])<<16 | uint32(header[1])<<8 | uint32(header[2])

	if c.TimestampDelta >= 0xFFFFFF {
		var extendedTimestamp [4]byte
		_, err = io.ReadFull(r, extendedTimestamp[:])
		if err != nil {
			return err
		}

		c.TimestampDelta = uint32(extendedTimestamp[0])<<24 | uint32(extendedTimestamp[1])<<16 |
			uint32(extendedTimestamp[2])<<8 | uint32(extendedTimestamp[3])
	}

	c.Body = make([]byte, bodyLen)
	_, err = io.ReadFull(r, c.Body)
	return err
}

func (c Chunk2) marshalSize() int {
	n := basicHeaderLength(c.ChunkStreamID) + 3 + len(c.Body)
	if c.TimestampDelta >= 0xFFFFFF {
		n += 4
	}
	return n
}

// MarshalHeaderTo writes the chunk header, that is the chunk without its body, into buf.
// ChunkStreamID must be a valid chunk stream ID (2 through 65599); no validation is
// performed by this method.
// buf must be large enough to contain the header (10 bytes are always enough).
// It returns the number of bytes written.
func (c Chunk2) MarshalHeaderTo(buf []byte, _ bool) int {
	n := marshalBasicHeader(buf, 2, c.ChunkStreamID)
	buf[n] = byte(c.TimestampDelta >> 16)
	buf[n+1] = byte(c.TimestampDelta >> 8)
	buf[n+2] = byte(c.TimestampDelta)

	if c.TimestampDelta >= 0xFFFFFF {
		buf[n] = 0xFF
		buf[n+1] = 0xFF
		buf[n+2] = 0xFF
		buf[n+3] = byte(c.TimestampDelta >> 24)
		buf[n+4] = byte(c.TimestampDelta >> 16)
		buf[n+5] = byte(c.TimestampDelta >> 8)
		buf[n+6] = byte(c.TimestampDelta)
		return n + 7
	}

	return n + 3
}

// Marshal writes the chunk.
//
// Deprecated: not used anymore.
func (c Chunk2) Marshal(_ bool) ([]byte, error) {
	err := ValidateChunkStreamID(c.ChunkStreamID)
	if err != nil {
		return nil, err
	}

	buf := make([]byte, c.marshalSize())
	n := c.MarshalHeaderTo(buf, false)
	copy(buf[n:], c.Body)
	return buf, nil
}
