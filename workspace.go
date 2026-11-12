package vorbis

// floorWorkspace holds the per-channel, per-packet state of a floor decode.
// A decoder keeps one workspace per channel; the slices are reused across
// packets and grow on demand, so decoding never has to allocate on the hot
// path.
type floorWorkspace struct {
	// active reports whether the floor was present in the current packet.
	// A channel whose floor is absent stays silent, even if it still takes
	// part in a type 2 residue.
	active bool
	// noResidue reports whether the residue must be skipped for this channel.
	// This starts out equal to !active, but coupling steps can clear it
	// independently of whether the floor itself is applied.
	noResidue bool

	// floor1 state
	y      []uint32
	step2  []bool
	finalY []uint32

	// floor0 state
	amplitude    uint32
	coefficients []float32
}

// residueWorkspace holds the scratch slices used while decoding a residue
// submap. The slices are always sized for the decoder's full channel count,
// but each submap passes only the prefix for the channels that belong to it;
// entries beyond that prefix are never read for the current submap.
type residueWorkspace struct {
	doNotDecode     []bool
	vectors         [][]float32
	classifications []uint32
}

// ensure grows the residue workspace so it can hold the given number of
// channels. Existing capacity is reused without touching the old contents.
func (w *residueWorkspace) ensure(channels int) {
	if cap(w.doNotDecode) < channels {
		w.doNotDecode = make([]bool, channels)
		w.vectors = make([][]float32, channels)
	} else {
		w.doNotDecode = w.doNotDecode[:channels]
		w.vectors = w.vectors[:channels]
	}
}
