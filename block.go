// Copyright 2019 The ebml-go authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package ebml

import (
	"io"
)

// LacingMode is type of laced data.
type LacingMode uint8

// Type of laced data.
const (
	LacingNo    LacingMode = 0
	LacingXiph  LacingMode = 1
	LacingFixed LacingMode = 2
	LacingEBML  LacingMode = 3
)

const (
	blockFlagMaskKeyframe    = 0x80
	blockFlagMaskInvisible   = 0x08
	blockFlagMaskLacing      = 0x06
	blockFlagMaskDiscardable = 0x01
)

// Block represents EBML Block/SimpleBlock element.
type Block struct {
	TrackNumber uint64
	Timecode    int16
	Keyframe    bool
	Invisible   bool
	Lacing      LacingMode
	Discardable bool
	Data        [][]byte
}

func (b *Block) packFlags() byte {
	var f byte
	if b.Keyframe {
		f |= blockFlagMaskKeyframe
	}
	if b.Invisible {
		f |= blockFlagMaskInvisible
	}
	if b.Discardable {
		f |= blockFlagMaskDiscardable
	}
	f |= byte(b.Lacing) << 1
	return f
}

// UnmarshalBlock unmarshals EBML Block structure.
// n is the declared size of the whole Block: the track number VINT, the
// 2-byte signed timecode, the flags byte and the lacing area (lace count,
// length table and frame data). The underlying reader is never read past n
// bytes, so bytes following the Block stay available for the next element.
func UnmarshalBlock(r io.Reader, n int64) (*Block, error) {
	if n < 0 {
		return nil, wrapErrorf(ErrInvalidElementSize, "block size must not be negative: %d", n)
	}

	// Every read of the Block goes through this budget reader, which never
	// requests more than the remaining declared size from r.
	br := &budgetReader{r: r, left: n}

	var b Block
	var err error

	vd := &valueDecoder{}

	if b.TrackNumber, _, err = vd.readVUInt(br); err != nil {
		return nil, err
	}

	if v, err := vd.readInt(br, 2); err == nil {
		b.Timecode = int16(v.(int64))
	} else {
		return nil, err
	}

	var flag [1]byte
	if _, err := br.Read(flag[:]); err != nil {
		return nil, err
	}

	f := flag[0]
	if f&blockFlagMaskKeyframe != 0 {
		b.Keyframe = true
	}
	if f&blockFlagMaskInvisible != 0 {
		b.Invisible = true
	}
	if f&blockFlagMaskDiscardable != 0 {
		b.Discardable = true
	}
	b.Lacing = LacingMode((f & blockFlagMaskLacing) >> 1)

	// The remaining budget is exactly the lacing area.
	lacingSize := br.remaining()
	var ul Unlacer
	switch b.Lacing {
	case LacingNo:
		ul, err = NewNoUnlacer(br, lacingSize)
	case LacingXiph:
		ul, err = NewXiphUnlacer(br, lacingSize)
	case LacingEBML:
		ul, err = NewEBMLUnlacer(br, lacingSize)
	case LacingFixed:
		ul, err = NewFixedUnlacer(br, lacingSize)
	}
	if err != nil {
		return nil, err
	}

	// Frames are collected into a local slice first so that a failure while
	// reading later frames never returns a partially decoded Block.
	var data [][]byte
	for {
		frame, err := ul.Read()
		if err == io.EOF {
			b.Data = data
			return &b, nil
		}
		if err != nil {
			return nil, err
		}
		data = append(data, frame)
	}
}

// MarshalBlock marshals EBML Block structure.
func MarshalBlock(b *Block, w io.Writer) error {
	n, err := encodeElementID(b.TrackNumber)
	if err != nil {
		return err
	}
	if _, err := w.Write(n); err != nil {
		return err
	}
	if _, err := w.Write([]byte{byte(b.Timecode >> 8), byte(b.Timecode)}); err != nil {
		return err
	}
	if _, err := w.Write([]byte{b.packFlags()}); err != nil {
		return err
	}

	var l Lacer
	switch b.Lacing {
	case LacingNo:
		l = NewNoLacer(w)
	case LacingXiph:
		l = NewXiphLacer(w)
	case LacingEBML:
		l = NewEBMLLacer(w)
	case LacingFixed:
		l = NewFixedLacer(w)
	}
	return l.Write(b.Data)
}
