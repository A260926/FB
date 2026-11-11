package chunk_test

import (
	"bytes"
	"strconv"
	"testing"

	"github.com/stretchr/testify/require"

	"github.com/bluenviron/gortmplib/pkg/chunk"
)

func extCSIDBasicHeader(typ byte, id uint32) []byte {
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

func TestBasicHeaderEncoding(t *testing.T) {
	for _, id := range []uint32{63, 64, 319, 320, 65599} {
		for typ := byte(0); typ <= 3; typ++ {
			enc := extCSIDBasicHeader(typ, id)

			require.Equal(t, len(enc), chunk.BasicHeaderLength(enc[0]))

			decTyp, decID := chunk.DecodeBasicHeader(enc)
			require.Equal(t, typ, decTyp)
			require.Equal(t, id, decID)
		}
	}
}

func TestBasicHeaderNonMinimal(t *testing.T) {
	// IDs 64 and 319, encoded with the three-byte form.
	for _, ca := range []struct {
		enc []byte
		id  uint32
	}{
		{[]byte{0x40 | 1, 0, 0}, 64},
		{[]byte{0x40 | 1, 0xff, 0x00}, 319},
	} {
		typ, id := chunk.DecodeBasicHeader(ca.enc)
		require.Equal(t, byte(1), typ)
		require.Equal(t, ca.id, id)

		var c chunk.Chunk0
		enc := append(append([]byte{}, ca.enc...), make([]byte, 11)...)
		err := c.Read(bytes.NewReader(enc), 0, false)
		require.NoError(t, err)
		require.Equal(t, ca.id, c.ChunkStreamID)
	}
}

func TestBasicHeaderTruncated(t *testing.T) {
	for _, enc := range [][]byte{
		{0x00},
		{0x40},
		{0x40 | 1},
		{0x40 | 1, 0x00},
	} {
		var c chunk.Chunk0
		err := c.Read(bytes.NewReader(enc), 0, false)
		require.Error(t, err)
	}
}

func TestChunkMarshalHeaderExtendedCSID(t *testing.T) {
	chunks := []chunk.Chunk{
		&chunk.Chunk0{ChunkStreamID: 65599, Timestamp: 0xffffff, BodyLen: 1},
		&chunk.Chunk1{ChunkStreamID: 320, TimestampDelta: 0xffffff, BodyLen: 1},
		&chunk.Chunk2{ChunkStreamID: 64, TimestampDelta: 0xffffff},
		&chunk.Chunk3{ChunkStreamID: 319, ExtendedTimestamp: 0xffffff},
	}

	for _, c := range chunks {
		buf := bytes.Repeat([]byte{0xAA}, 24)

		hasExtendedTimestamp := false
		if _, ok := c.(*chunk.Chunk3); ok {
			hasExtendedTimestamp = true
		}
		n := c.MarshalHeaderTo(buf, hasExtendedTimestamp)

		var csid uint32
		switch ch := c.(type) {
		case *chunk.Chunk0:
			csid = ch.ChunkStreamID
		case *chunk.Chunk1:
			csid = ch.ChunkStreamID
		case *chunk.Chunk2:
			csid = ch.ChunkStreamID
		case *chunk.Chunk3:
			csid = ch.ChunkStreamID
			require.Equal(t, chunk.BasicHeaderLength(buf[0])+4, n)
		}

		var typ byte
		switch c.(type) {
		case *chunk.Chunk0:
			typ = 0
		case *chunk.Chunk1:
			typ = 1
		case *chunk.Chunk2:
			typ = 2
		case *chunk.Chunk3:
			typ = 3
		}

		require.Equal(t, extCSIDBasicHeader(typ, csid), buf[:chunk.BasicHeaderLength(buf[0])])
	}
}

func TestChunkReadExtendedCSID(t *testing.T) {
	for _, ca := range []struct {
		name string
		c    chunk.Chunk
		enc  func() ([]byte, error)
		dec  func() chunk.Chunk
		ext  bool
	}{
		{
			"chunk0",
			&chunk.Chunk0{ChunkStreamID: 64, Timestamp: 0x1000000, Type: 8, MessageStreamID: 3, BodyLen: 2, Body: []byte{1, 2}},
			func() ([]byte, error) {
				return (&chunk.Chunk0{
					ChunkStreamID: 64, Timestamp: 0x1000000, Type: 8,
					MessageStreamID: 3, BodyLen: 2, Body: []byte{1, 2},
				}).Marshal(false) //nolint:staticcheck
			},
			func() chunk.Chunk { return &chunk.Chunk0{} },
			false,
		},
		{
			"chunk1",
			&chunk.Chunk1{ChunkStreamID: 319, TimestampDelta: 0x1000000, Type: 8, BodyLen: 2, Body: []byte{3, 4}},
			func() ([]byte, error) {
				return (&chunk.Chunk1{
					ChunkStreamID: 319, TimestampDelta: 0x1000000, Type: 8,
					BodyLen: 2, Body: []byte{3, 4},
				}).Marshal(false) //nolint:staticcheck
			},
			func() chunk.Chunk { return &chunk.Chunk1{} },
			false,
		},
		{
			"chunk2",
			&chunk.Chunk2{ChunkStreamID: 320, TimestampDelta: 0x1000000, Body: []byte{5, 6}},
			func() ([]byte, error) {
				return (&chunk.Chunk2{
					ChunkStreamID: 320, TimestampDelta: 0x1000000, Body: []byte{5, 6},
				}).Marshal(false) //nolint:staticcheck
			},
			func() chunk.Chunk { return &chunk.Chunk2{} },
			false,
		},
		{
			"chunk3",
			&chunk.Chunk3{ChunkStreamID: 65599, ExtendedTimestamp: 0x1000000, Body: []byte{7, 8}},
			func() ([]byte, error) {
				return (&chunk.Chunk3{
					ChunkStreamID: 65599, ExtendedTimestamp: 0x1000000, Body: []byte{7, 8},
				}).Marshal(true) //nolint:staticcheck
			},
			func() chunk.Chunk { return &chunk.Chunk3{} },
			true,
		},
	} {
		t.Run(ca.name, func(t *testing.T) {
			enc, err := ca.enc()
			require.NoError(t, err)

			dec := ca.dec()
			err = dec.Read(bytes.NewReader(enc), 2, ca.ext)
			require.NoError(t, err)
			require.Equal(t, ca.c, dec)
		})
	}
}

func TestChunkMarshalInvalidChunkStreamID(t *testing.T) {
	for _, id := range []uint32{0, 1, 65599 + 1} {
		expected := "invalid chunk stream ID: " + strconv.FormatUint(uint64(id), 10)

		_, err := chunk.Chunk0{ChunkStreamID: id}.Marshal(false) //nolint:staticcheck
		require.EqualError(t, err, expected)

		_, err = chunk.Chunk1{ChunkStreamID: id}.Marshal(false) //nolint:staticcheck
		require.EqualError(t, err, expected)

		_, err = chunk.Chunk2{ChunkStreamID: id}.Marshal(false) //nolint:staticcheck
		require.EqualError(t, err, expected)

		_, err = chunk.Chunk3{ChunkStreamID: id}.Marshal(false) //nolint:staticcheck
		require.EqualError(t, err, expected)
	}
}
