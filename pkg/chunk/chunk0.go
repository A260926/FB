package chunk

import (
	"io"
)

// Chunk0 is a type 0 chunk.
// This type MUST be used at
// the start of a chunk stream, and whenever the stream timestamp goes
// backward (e.g., because of a backward seek).
type Chunk0 struct {
	ChunkStreamID   uint32
	Timestamp       uint32
	BodyLen         uint32
	Type            uint8
	MessageStreamID uint32
	Body            []byte
}

// GetBody returns the chunk body.
func (c Chunk0) GetBody() []byte {
	return c.Body
}

// Read reads the chunk.
func (c *Chunk0) Read(r io.Reader, maxBodyLen uint32, _ bool) error {
	_, chunkStreamID, err := readBasicHeader(r)
	if err != nil {
		return err
	}
	c.ChunkStreamID = chunkStreamID

	header := make([]byte, 11)
	_, err = io.ReadFull(r, header)
	if err != nil {
		return err
	}

	c.Timestamp = uint32(header[0])<<16 | uint32(header[1])<<8 | uint32(header[2])
	c.BodyLen = uint32(header[3])<<16 | uint32(header[4])<<8 | uint32(header[5])
	c.Type = header[6]
	c.MessageStreamID = uint32(header[7])<<24 | uint32(header[8])<<16 | uint32(header[9])<<8 | uint32(header[10])

	if c.Timestamp >= 0xFFFFFF {
		var extendedTimestamp [4]byte
		_, err = io.ReadFull(r, extendedTimestamp[:])
		if err != nil {
			return err
		}

		c.Timestamp = uint32(extendedTimestamp[0])<<24 | uint32(extendedTimestamp[1])<<16 |
			uint32(extendedTimestamp[2])<<8 | uint32(extendedTimestamp[3])
	}

	chunkBodyLen := min(c.BodyLen, maxBodyLen)

	c.Body = make([]byte, chunkBodyLen)
	_, err = io.ReadFull(r, c.Body)
	return err
}

func (c Chunk0) marshalSize() int {
	n := basicHeaderLength(c.ChunkStreamID) + 11 + len(c.Body)
	if c.Timestamp >= 0xFFFFFF {
		n += 4
	}
	return n
}

// MarshalHeaderTo writes the chunk header, that is the chunk without its body, into buf.
// ChunkStreamID must be a valid chunk stream ID (2 through 65599); no validation is
// performed by this method.
// buf must be large enough to contain the header (18 bytes are always enough).
// It returns the number of bytes written.
func (c Chunk0) MarshalHeaderTo(buf []byte, _ bool) int {
	n := marshalBasicHeader(buf, 0, c.ChunkStreamID)
	buf[n] = byte(c.Timestamp >> 16)
	buf[n+1] = byte(c.Timestamp >> 8)
	buf[n+2] = byte(c.Timestamp)
	buf[n+3] = byte(c.BodyLen >> 16)
	buf[n+4] = byte(c.BodyLen >> 8)
	buf[n+5] = byte(c.BodyLen)
	buf[n+6] = c.Type
	buf[n+7] = byte(c.MessageStreamID >> 24)
	buf[n+8] = byte(c.MessageStreamID >> 16)
	buf[n+9] = byte(c.MessageStreamID >> 8)
	buf[n+10] = byte(c.MessageStreamID)

	if c.Timestamp >= 0xFFFFFF {
		buf[n] = 0xFF
		buf[n+1] = 0xFF
		buf[n+2] = 0xFF
		buf[n+11] = byte(c.Timestamp >> 24)
		buf[n+12] = byte(c.Timestamp >> 16)
		buf[n+13] = byte(c.Timestamp >> 8)
		buf[n+14] = byte(c.Timestamp)
		return n + 15
	}

	return n + 11
}

// Marshal writes the chunk.
//
// Deprecated: not used anymore.
func (c Chunk0) Marshal(_ bool) ([]byte, error) {
	err := ValidateChunkStreamID(c.ChunkStreamID)
	if err != nil {
		return nil, err
	}

	buf := make([]byte, c.marshalSize())
	n := c.MarshalHeaderTo(buf, false)
	copy(buf[n:], c.Body)
	return buf, nil
}
