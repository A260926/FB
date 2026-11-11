package chunk

import (
	"fmt"
	"io"
)

const (
	// MinChunkStreamID is the minimum chunk stream ID that can be encoded in a basic header.
	MinChunkStreamID = 2

	// MaxChunkStreamID is the maximum chunk stream ID that can be encoded in a basic header.
	MaxChunkStreamID = 65599
)

// ValidateChunkStreamID checks whether id can be encoded in a RTMP basic header.
func ValidateChunkStreamID(id uint32) error {
	if id < MinChunkStreamID || id > MaxChunkStreamID {
		return fmt.Errorf("invalid chunk stream ID: %d", id)
	}
	return nil
}

// BasicHeaderLength returns the number of bytes of the basic header that starts with first.
func BasicHeaderLength(first byte) int {
	switch first & 0x3F {
	case 0:
		return 2
	case 1:
		return 3
	default:
		return 1
	}
}

// DecodeBasicHeader decodes the chunk type and chunk stream ID from b,
// that must contain at least BasicHeaderLength(b[0]) bytes.
func DecodeBasicHeader(b []byte) (byte, uint32) {
	typ := b[0] >> 6
	id := uint32(b[0] & 0x3F)

	switch id {
	case 0:
		id = 64 + uint32(b[1])
	case 1:
		id = 64 + uint32(b[1]) + uint32(b[2])<<8
	}

	return typ, id
}

func basicHeaderLength(id uint32) int {
	switch {
	case id <= 63:
		return 1
	case id <= 319:
		return 2
	default:
		return 3
	}
}

func marshalBasicHeader(buf []byte, typ byte, id uint32) int {
	switch {
	case id <= 63:
		buf[0] = typ<<6 | byte(id)
		return 1

	case id <= 319:
		buf[0] = typ << 6
		buf[1] = byte(id - 64)
		return 2

	default:
		v := id - 64
		buf[0] = typ<<6 | 0x01
		buf[1] = byte(v)
		buf[2] = byte(v >> 8)
		return 3
	}
}

func readBasicHeader(r io.Reader) (byte, uint32, error) {
	var b [3]byte

	_, err := io.ReadFull(r, b[:1])
	if err != nil {
		return 0, 0, err
	}

	n := BasicHeaderLength(b[0])
	if n > 1 {
		_, err = io.ReadFull(r, b[1:n])
		if err != nil {
			return 0, 0, err
		}
	}

	typ, id := DecodeBasicHeader(b[:n])
	return typ, id, nil
}
