package chunk

import (
	"io"
)

// Chunk3 is a type 3 chunk.
// Type 3 chunks have no message header. The stream ID, message length
// and timestamp delta fields are not present; chunks of this type take
// values from the preceding chunk for the same Chunk Stream ID. When a
// single message is split into chunks, all chunks of a message except
// the first one SHOULD use this type.
type Chunk3 struct {
	ChunkStreamID     uint32
	ExtendedTimestamp uint32
	Body              []byte
}

// GetBody returns the chunk body.
func (c Chunk3) GetBody() []byte {
	return c.Body
}

// Read reads the chunk.
func (c *Chunk3) Read(r io.Reader, bodyLen uint32, hasExtendedTimestamp bool) error {
	_, chunkStreamID, err := readBasicHeader(r)
	if err != nil {
		return err
	}
	c.ChunkStreamID = chunkStreamID
	c.ExtendedTimestamp = 0

	if hasExtendedTimestamp {
		var extendedTimestamp [4]byte
		_, err = io.ReadFull(r, extendedTimestamp[:])
		if err != nil {
			return err
		}
		c.ExtendedTimestamp = uint32(extendedTimestamp[0])<<24 | uint32(extendedTimestamp[1])<<16 |
			uint32(extendedTimestamp[2])<<8 | uint32(extendedTimestamp[3])
	}

	c.Body = make([]byte, bodyLen)
	_, err = io.ReadFull(r, c.Body)
	return err
}

func (c Chunk3) marshalSize(hasExtendedTimestamp bool) int {
	n := basicHeaderLength(c.ChunkStreamID) + len(c.Body)
	if hasExtendedTimestamp {
		n += 4
	}
	return n
}

// MarshalHeaderTo writes the chunk header, that is the chunk without its body, into buf.
// ChunkStreamID must be a valid chunk stream ID (2 through 65599); no validation is
// performed by this method.
// buf must be large enough to contain the header (7 bytes are always enough).
// It returns the number of bytes written.
func (c Chunk3) MarshalHeaderTo(buf []byte, hasExtendedTimestamp bool) int {
	n := marshalBasicHeader(buf, 3, c.ChunkStreamID)

	if hasExtendedTimestamp {
		buf[n] = byte(c.ExtendedTimestamp >> 24)
		buf[n+1] = byte(c.ExtendedTimestamp >> 16)
		buf[n+2] = byte(c.ExtendedTimestamp >> 8)
		buf[n+3] = byte(c.ExtendedTimestamp)
		return n + 4
	}

	return n
}

// Marshal writes the chunk.
//
// Deprecated: not used anymore.
func (c Chunk3) Marshal(hasExtendedTimestamp bool) ([]byte, error) {
	err := ValidateChunkStreamID(c.ChunkStreamID)
	if err != nil {
		return nil, err
	}

	buf := make([]byte, c.marshalSize(hasExtendedTimestamp))
	n := c.MarshalHeaderTo(buf, hasExtendedTimestamp)
	copy(buf[n:], c.Body)
	return buf, nil
}
