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
	"errors"
	"io"
	"math"
)

// ErrFixedLaceUndivisible means that a length of a fixed lacing data is undivisible.
var ErrFixedLaceUndivisible = errors.New("undivisible fixed lace")

// ErrInvalidLaceSize means that the lacing length table is mathematically
// inconsistent with the lacing area budget. The length table itself was fully
// read inside the budget, so bytes following the lacing area are not consumed.
var ErrInvalidLaceSize = errors.New("invalid lace size")

// Unlacer is the interface to read laced frames in Block.
type Unlacer interface {
	Read() ([]byte, error)
}

// budgetReader restricts every Read to the remaining budget of the lacing
// area (or the whole Block). Bytes past the budget are never requested from
// the underlying reader, so data belonging to the next element cannot be
// consumed. Short reads of the underlying reader are retried until the
// requested bytes are filled or the reader fails.
type budgetReader struct {
	r    io.Reader
	left int64
}

func (r *budgetReader) Read(p []byte) (int, error) {
	if r.left <= 0 {
		return 0, io.ErrUnexpectedEOF
	}
	if int64(len(p)) > r.left {
		p = p[:r.left]
	}
	n, err := io.ReadFull(r.r, p)
	r.left -= int64(n)
	if err == io.ErrUnexpectedEOF || err == io.EOF {
		// ReadFull reports EOF when no byte was available and ErrUnexpectedEOF
		// when only part of the request was filled; both mean that the declared
		// budget could not be fully read.
		return n, io.ErrUnexpectedEOF
	}
	return n, err
}

// remaining reports the unconsumed budget.
func (r *budgetReader) remaining() int64 {
	return r.left
}

type unlacer struct {
	r    *budgetReader
	i    int
	size []int
}

func (u *unlacer) Read() ([]byte, error) {
	if u.i >= len(u.size) {
		return nil, io.EOF
	}
	n := u.size[u.i]
	u.i++

	b := make([]byte, n)
	_, err := io.ReadFull(u.r, b)
	return b, err
}

func negativeSizeError(area string, n int64) error {
	return wrapErrorf(ErrInvalidElementSize, "%s size must not be negative: %d", area, n)
}

// NewNoUnlacer creates pass-through Unlacer for not laced data.
// n is the exact size of the frame data and bounds every Read.
func NewNoUnlacer(r io.Reader, n int64) (Unlacer, error) {
	if n < 0 {
		return nil, negativeSizeError("no-lacing data", n)
	}
	return &unlacer{r: &budgetReader{r: r, left: n}, size: []int{int(n)}}, nil
}

// NewXiphUnlacer creates Unlacer for Xiph laced data.
// n is the size of the whole lacing area: the frame count byte, the length
// table and the frame data. Reads never cross this budget.
func NewXiphUnlacer(r io.Reader, n int64) (Unlacer, error) {
	if n < 0 {
		return nil, negativeSizeError("xiph lacing area", n)
	}
	br := &budgetReader{r: r, left: n}

	var b [1]byte
	if _, err := br.Read(b[:]); err != nil {
		return nil, err
	}
	nFrame := int(b[0]) + 1

	// Read the whole length table first. Each of the first nFrame-1 frames has
	// a length entry; runs of 0xFF bytes extend the length by 255. The values
	// are accumulated as int64: the number of length bytes is bounded by the
	// budget, so the sum cannot overflow, and no frame buffer is allocated
	// until the whole table has been validated.
	sizes := make([]int64, nFrame)
	for i := 0; i < nFrame-1; i++ {
		for {
			if _, err := br.Read(b[:]); err != nil {
				// The table cannot be finished inside the budget.
				return nil, err
			}
			sizes[i] += int64(b[0])
			if b[0] != 0xFF {
				break
			}
		}
	}

	// The table is fully read. Validate it against the remaining frame budget.
	frameBudget := br.remaining()
	var declared int64
	for i := 0; i < nFrame-1; i++ {
		s := sizes[i]
		if s < 0 || s > frameBudget-declared {
			return nil, wrapErrorf(
				ErrInvalidLaceSize, "xiph lace frame %d size %d does not fit in %d bytes of frame data",
				i, s, frameBudget-declared,
			)
		}
		declared += s
	}
	lastSize := frameBudget - declared
	if lastSize <= 0 {
		return nil, wrapErrorf(
			ErrInvalidLaceSize, "xiph lace last frame size is not positive: %d", lastSize,
		)
	}

	size := make([]int, nFrame)
	for i := 0; i < nFrame-1; i++ {
		size[i] = int(sizes[i])
	}
	size[nFrame-1] = int(lastSize)

	return &unlacer{r: br, size: size}, nil
}

// NewFixedUnlacer creates Unlacer for Fixed laced data.
// n is the size of the whole lacing area: the frame count byte followed by
// nFrame equally sized frames. Reads never cross this budget.
func NewFixedUnlacer(r io.Reader, n int64) (Unlacer, error) {
	if n < 0 {
		return nil, negativeSizeError("fixed lacing area", n)
	}
	br := &budgetReader{r: r, left: n}

	var b [1]byte
	if _, err := br.Read(b[:]); err != nil {
		return nil, err
	}
	nFrame := int(b[0]) + 1

	dataSize := br.remaining()
	if dataSize%int64(nFrame) != 0 {
		return nil, wrapErrorf(
			ErrFixedLaceUndivisible, "unlacing %d bytes of %d frames", dataSize, nFrame,
		)
	}
	frameSize := int(dataSize / int64(nFrame))

	size := make([]int, nFrame)
	for i := range size {
		size[i] = frameSize
	}
	return &unlacer{r: br, size: size}, nil
}

// NewEBMLUnlacer creates Unlacer for EBML laced data.
// n is the size of the whole lacing area: the frame count byte, the signed
// length table and the frame data. Reads never cross this budget.
func NewEBMLUnlacer(r io.Reader, n int64) (Unlacer, error) {
	if n < 0 {
		return nil, negativeSizeError("ebml lacing area", n)
	}
	br := &budgetReader{r: r, left: n}

	var b [1]byte
	if _, err := br.Read(b[:]); err != nil {
		return nil, err
	}
	nFrame := int(b[0]) + 1

	vd := &valueDecoder{}

	// Read the whole length table first: an unsigned VINT for the first frame
	// and nFrame-2 signed VINT differences. All reads are bounded by br.
	firstSize, _, err := vd.readVUInt(br)
	if err != nil {
		return nil, err
	}
	diffs := make([]int64, nFrame-2)
	for i := range diffs {
		diff, _, err := vd.readVInt(br)
		if err != nil {
			// The table cannot be finished inside the budget.
			return nil, err
		}
		diffs[i] = diff
	}

	// The table is fully read. Validate the math against the frame budget.
	frameBudget := br.remaining()
	size := make([]int, nFrame)
	frameSize := int64(firstSize)
	if frameSize < 0 || frameSize > frameBudget {
		return nil, wrapErrorf(
			ErrInvalidLaceSize, "ebml lace first frame size %d does not fit in %d bytes of frame data",
			frameSize, frameBudget,
		)
	}
	size[0] = int(frameSize)
	declared := frameSize
	for i, diff := range diffs {
		// Guard against int64 overflow of the running frame size.
		if (diff > 0 && frameSize > math.MaxInt64-diff) ||
			(diff < 0 && frameSize < math.MinInt64-diff) {
			return nil, wrapErrorf(
				ErrInvalidLaceSize, "ebml lace frame %d size overflows", i+1,
			)
		}
		frameSize += diff
		if frameSize < 0 || frameSize > frameBudget-declared {
			return nil, wrapErrorf(
				ErrInvalidLaceSize, "ebml lace frame %d size %d does not fit in %d bytes of frame data",
				i+1, frameSize, frameBudget-declared,
			)
		}
		size[i+1] = int(frameSize)
		declared += frameSize
	}
	lastSize := frameBudget - declared
	if lastSize <= 0 {
		return nil, wrapErrorf(
			ErrInvalidLaceSize, "ebml lace last frame size is not positive: %d", lastSize,
		)
	}
	size[nFrame-1] = int(lastSize)

	return &unlacer{r: br, size: size}, nil
}
