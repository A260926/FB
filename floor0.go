package vorbis

import (
	"math"
)

type floor0 struct {
	order           uint8
	rate            uint16
	barkMapSize     uint16
	amplitudeBits   uint8
	amplitudeOffset uint8
	bookList        []uint8
}

type floor0Data struct {
	amplitude    uint32
	coefficients []float32
}

func (f *floor0) ReadFrom(r *bitReader) error {
	f.order = r.Read8(8)
	f.rate = r.Read16(16)
	f.barkMapSize = r.Read16(16)
	f.amplitudeBits = r.Read8(6)
	f.amplitudeOffset = r.Read8(8)
	f.bookList = make([]uint8, r.Read8(4)+1)
	for i := range f.bookList {
		f.bookList[i] = r.Read8(8)
	}
	return nil
}

func (f *floor0) Decode(r *bitReader, books []codebook, n uint32) interface{} {
	amplitude := r.Read32(uint(f.amplitudeBits))
	if amplitude == 0 {
		return nil
	}
	bookNumber := r.Read8(ilog(len(f.bookList)))
	book := books[f.bookList[bookNumber]]
	coefficients := make([]float32, f.order)
	// Vorbis I, floor 0: the coefficients form one cumulative LSP sequence.
	// Every codebook vector continues the running sum of the previous one,
	// and the final vector contributes only as many values as are still
	// missing, so no bits belonging to the residue are consumed.
	last := float32(0)
	for i := 0; i < int(f.order); {
		tempVector := book.DecodeVector(r)
		remaining := int(f.order) - i
		if len(tempVector) < remaining {
			remaining = len(tempVector)
		}
		for j := 0; j < remaining; j++ {
			last += tempVector[j]
			coefficients[i] = last
			i++
		}
	}
	return floor0Data{amplitude, coefficients}
}

func (f *floor0) Apply(out []float32, data interface{}) {
	d := data.(floor0Data)
	n := uint32(len(out))
	// The spec's loop bounds use signed arithmetic: for order 1 the first
	// bound is (1-3)/2 = -1 and its loop must not run. Using uint8 here would
	// underflow and read past the coefficient slice.
	order := int(f.order)
	i := uint32(0)
	for i < n {
		mapi := f.mapResult(i, n)
		w := math.Pi * float64(mapi) / float64(f.barkMapSize)
		cosw := math.Cos(w)
		var p, q float64
		if order%2 == 1 {
			p = 1 - cosw*cosw
			for j := 0; j <= (order-3)/2; j++ {
				tmp := math.Cos(float64(d.coefficients[2*j+1])) - cosw
				p *= 4 * tmp * tmp
			}
			q = 1. / 4.
			for j := 0; j <= (order-1)/2; j++ {
				tmp := math.Cos(float64(d.coefficients[2*j])) - cosw
				q *= 4 * tmp * tmp
			}
		} else {
			p = (1 - cosw*cosw) / 2
			for j := 0; j <= (order-2)/2; j++ {
				tmp := math.Cos(float64(d.coefficients[2*j+1])) - cosw
				p *= 4 * tmp * tmp
			}
			q = (1 + cosw*cosw) / 2
			for j := 0; j <= (order-2)/2; j++ {
				tmp := math.Cos(float64(d.coefficients[2*j])) - cosw
				q *= 4 * tmp * tmp
			}
		}
		linearFloorValue := math.Exp(.11512925 * (float64(d.amplitude)*float64(f.amplitudeOffset)/(float64(uint64(1)<<f.amplitudeBits-1)*math.Sqrt(p+q)) - float64(f.amplitudeOffset)))
		// every frequency bin that the current block maps onto this Bark cell
		// gets the same floor value; recompute the mapping for each bin so
		// the loop terminates correctly when several bins share a cell.
		for i < n && f.mapResult(i, n) == mapi {
			out[i] *= float32(linearFloorValue)
			i++
		}
	}
}

// mapResult implements the Vorbis I floor 0 Bark frequency map.
// A block with n spectral bins spans the frequency range 0..rate/2, so
// bin i has the frequency (rate/2)*i/n; this frequency is mapped onto one
// of barkMapSize Bark-scale cells. n is the current block's bin count, so
// short and long blocks are each mapped using their own frequency grid.
func (f *floor0) mapResult(i, n uint32) int {
	if i >= n {
		return -1
	}
	frequency := .5 * float64(f.rate) * float64(i) / float64(n)
	b := int(math.Floor(bark(frequency) * float64(f.barkMapSize) / bark(.5*float64(f.rate))))
	if b > int(f.barkMapSize)-1 {
		return int(f.barkMapSize) - 1
	}
	return b
}

func bark(x float64) float64 {
	return 13.1*math.Atan(.00074*x) + 2.24*math.Atan(.0000000185*x*x) + .0001*x
}
