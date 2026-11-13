// Copyright 2024 Google LLC. All Rights Reserved.
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

package proof

import (
	"bytes"
	"fmt"
	"math/bits"

	"github.com/transparency-dev/merkle"
	"github.com/transparency-dev/merkle/compact"
)

// UpdateInclusion converts an inclusion proof for a leaf in an older version
// of an append-only log into an inclusion proof for the same leaf in a newer,
// larger version of the log, using a consistency proof between the two
// versions. No new data is requested from the log: the result is derived
// entirely from the two supplied proofs and the digests already known to the
// caller.
//
// The proofs are in the RFC 6962 format, i.e. each is the sequence of node
// digests produced by [Nodes.Rehash], in which the nodes of an ephemeral
// (incomplete) subtree are already folded into a single digest. They are *not*
// the raw per-node digests corresponding to [Nodes.IDs]. The same on-wire
// digest therefore stands for a leaf interval, and its identity must be tracked
// by that interval: a compact NodeID such as {Level: 3, Index: 1} covers
// [8, 16), but the ephemeral node with that ID in a tree of size 13 only covers
// [8, 13). Substituting digests by NodeID alone would silently mix intervals of
// different widths.
//
// The parameters must satisfy 0 < oldSize <= newSize and index < oldSize, and
// every digest must have hasher.Size() bytes. The old inclusion proof must
// prove leafHash at index in the tree of size oldSize with root oldRoot, and the
// consistency proof must prove oldRoot is consistent with newRoot at newSize.
// If oldSize == newSize then the consistency proof must be empty and the two
// roots must be equal, but the inclusion proof is still verified.
//
// On success the returned proof is a freshly allocated, independently owned
// slice: it does not alias any input, and mutating it does not affect later
// calls. The input slices are never modified. If any argument or either proof
// is invalid, the result is nil and a non-nil error; a partial proof is never
// returned.
//
// The work and memory used are proportional to the tree height and the lengths
// of the two proof slices, not to newSize or the number of appended leaves.
// In particular, the number of HashChildren calls is O(len(inclusion)+
// len(consistency)+log(newSize)).
func UpdateInclusion(hasher merkle.LogHasher, index, oldSize, newSize uint64, leafHash, oldRoot, newRoot []byte, inclusion, consistency [][]byte) ([][]byte, error) {
	switch {
	case oldSize == 0:
		return nil, fmt.Errorf("oldSize must be positive")
	case oldSize > newSize:
		return nil, fmt.Errorf("oldSize %d is greater than newSize %d", oldSize, newSize)
	case index >= oldSize:
		return nil, fmt.Errorf("index %d out of bounds for tree size %d", index, oldSize)
	}

	size := hasher.Size()
	if err := checkDigestSize("leafHash", leafHash, size); err != nil {
		return nil, err
	}
	if err := checkDigestSize("oldRoot", oldRoot, size); err != nil {
		return nil, err
	}
	if err := checkDigestSize("newRoot", newRoot, size); err != nil {
		return nil, err
	}
	for i, h := range inclusion {
		if err := checkDigestSize(fmt.Sprintf("inclusion[%d]", i), h, size); err != nil {
			return nil, err
		}
	}
	for i, h := range consistency {
		if err := checkDigestSize(fmt.Sprintf("consistency[%d]", i), h, size); err != nil {
			return nil, err
		}
	}

	// The old inclusion proof is verified in all cases, including oldSize==newSize.
	if err := VerifyInclusion(hasher, index, oldSize, leafHash, inclusion, oldRoot); err != nil {
		return nil, fmt.Errorf("invalid old inclusion proof: %w", err)
	}

	if oldSize == newSize {
		if len(consistency) != 0 {
			return nil, fmt.Errorf("consistency proof must be empty for equal tree sizes, got %d items", len(consistency))
		}
		if !bytes.Equal(oldRoot, newRoot) {
			return nil, fmt.Errorf("roots differ for equal tree sizes")
		}
		out := make([][]byte, len(inclusion))
		for i, h := range inclusion {
			out[i] = bytes.Clone(h)
		}
		return out, nil
	}

	if err := VerifyConsistency(hasher, oldSize, newSize, consistency, oldRoot, newRoot); err != nil {
		return nil, fmt.Errorf("invalid consistency proof: %w", err)
	}

	// The proof slices were verified above, so the node layouts below are those
	// of a well-formed proof. They let us attach every digest to the leaf
	// interval it actually commits to.
	oldNodes, err := Inclusion(index, oldSize)
	if err != nil {
		return nil, err
	}
	consNodes, err := Consistency(oldSize, newSize)
	if err != nil {
		return nil, err
	}
	targetNodes, err := Inclusion(index, newSize)
	if err != nil {
		return nil, err
	}

	r := &updateResolver{
		hasher: hasher,
		known:  make(map[leafInterval][]byte),
	}
	if err := r.addEvidence(oldNodes, inclusion, oldSize); err != nil {
		return nil, err
	}
	if err := r.addEvidence(consNodes, consistency, newSize); err != nil {
		return nil, err
	}
	// The caller already knows the target leaf digest, and the old root is a
	// digest of the interval [0, oldSize). Note that the latter is used only as
	// an opaque digest: nothing tries to split it into its children.
	r.known[leafInterval{index, index + 1}] = bytes.Clone(leafHash)
	r.known[leafInterval{0, oldSize}] = bytes.Clone(oldRoot)

	elements, err := proofElements(targetNodes, newSize)
	if err != nil {
		return nil, err
	}
	out := make([][]byte, len(elements))
	for i, e := range elements {
		h, err := r.resolve(e)
		if err != nil {
			return nil, fmt.Errorf("cannot derive inclusion proof element %d for [%d, %d): %w", i, e.begin, e.end, err)
		}
		out[i] = bytes.Clone(h)
	}
	return out, nil
}

func checkDigestSize(name string, digest []byte, want int) error {
	if got := len(digest); got != want {
		return fmt.Errorf("%s has unexpected digest size %d, want %d", name, got, want)
	}
	return nil
}

// leafInterval is a half-open range [begin, end) of leaf indices. It is the
// identity of a Merkle tree node in this file: unlike a compact.NodeID, it does
// not imply a fixed width, so the same ID used by an ephemeral node in a
// non-perfect tree maps to the correct truncated interval.
type leafInterval struct {
	begin, end uint64
}

// coverage returns the leaf interval of the node, saturating instead of
// overflowing at the top of the uint64 range. Sizes only need to be non-zero
// uint64s; a tree of 2^64 leaves cannot be represented.
func coverage(id compact.NodeID) leafInterval {
	b := id.Index << id.Level
	var e uint64
	hi := id.Index + 1
	// The nominal right edge overflows iff the shifted-out high bits are
	// non-zero (including hi itself wrapping to zero).
	if id.Level >= 64 || hi == 0 || hi > ^uint64(0)>>id.Level {
		e = ^uint64(0)
	} else {
		e = hi << id.Level
	}
	return leafInterval{begin: b, end: e}
}

// proofElements returns the leaf intervals committed to by the digests of a
// proof after Nodes.Rehash folding. Ephemeral nodes are folded, so the elements
// of IDs[begin:end) correspond to exactly one digest and to the union of their
// leaf intervals. The returned slice is parallel to the digest slice produced by
// Nodes.Rehash.
//
// The nominal coverage of a right-border node overstates the truth: an
// ephemeral node {Level:1, Index:2} nominally covers [4, 8), but in a tree of
// size 7 the folded digest covers only [4, 7). Intervals are therefore clipped
// to the tree size.
func proofElements(nodes Nodes, size uint64) ([]leafInterval, error) {
	ids := nodes.IDs
	_, begin, end := nodes.Ephem()
	out := make([]leafInterval, 0, len(ids))
	for i := 0; i < len(ids); {
		if begin < end && i == begin {
			if begin >= end || end > len(ids) {
				return nil, fmt.Errorf("invalid ephemeral node range [%d, %d) over %d nodes", begin, end, len(ids))
			}
			iv := leafInterval{begin: ^uint64(0)}
			for j := begin; j < end; j++ {
				c := coverage(ids[j])
				if c.begin < iv.begin {
					iv.begin = c.begin
				}
				if c.end > iv.end {
					iv.end = c.end
				}
			}
			out = append(out, clipInterval(iv, size))
			i = end
			continue
		}
		out = append(out, clipInterval(coverage(ids[i]), size))
		i++
	}
	return out, nil
}

// clipInterval clips a nominal node interval to the tree size. Without this,
// the folded digest of an ephemeral right-border subtree would be labelled with
// the wider perfect interval it nominally occupies, and could collide with a
// real node of the new tree that covers only part of that range.
func clipInterval(iv leafInterval, size uint64) leafInterval {
	if iv.begin > size {
		iv.begin = size
	}
	if iv.end > size {
		iv.end = size
	}
	return iv
}

// updateResolver derives node digests for intervals of the new tree from a set
// of digests, each labelled with the leaf interval it commits to.
type updateResolver struct {
	hasher merkle.LogHasher
	known  map[leafInterval][]byte
}

// addEvidence registers the digests of a proof for a tree of the given size,
// against the leaf intervals they commit to.
func (r *updateResolver) addEvidence(nodes Nodes, hashes [][]byte, size uint64) error {
	ivs, err := proofElements(nodes, size)
	if err != nil {
		return err
	}
	if got, want := len(hashes), len(ivs); got != want {
		return fmt.Errorf("got %d digests, want %d proof elements", got, want)
	}
	for i, iv := range ivs {
		if iv.begin >= iv.end {
			return fmt.Errorf("proof element %d has empty leaf interval [%d, %d)", i, iv.begin, iv.end)
		}
		r.known[iv] = bytes.Clone(hashes[i])
	}
	return nil
}

// resolve returns the digest of the subtree covering the given interval.
//
// It only produces digests that are implied by the evidence: either the
// interval itself is known, or it can be assembled from known sub-intervals
// using the canonical RFC 6962 shape of the tree. Recursion therefore stops at
// known intervals, and its cost is bounded by the amount of evidence, not by
// the number of leaves in the interval.
func (r *updateResolver) resolve(iv leafInterval) ([]byte, error) {
	if h, ok := r.known[iv]; ok {
		return h, nil
	}
	if iv.begin >= iv.end {
		return nil, fmt.Errorf("empty leaf interval [%d, %d)", iv.begin, iv.end)
	}
	ln := iv.end - iv.begin
	if ln == 1 {
		return nil, fmt.Errorf("no evidence for leaf %d", iv.begin)
	}

	// A perfect, aligned subtree is the canonical binary combination of its
	// halves. Alignment is essential: [6, 14) has a perfect size but no node
	// covers it, and taking a binary split there would invent a tree shape.
	if ln&(ln-1) == 0 && iv.begin%ln == 0 {
		mid := iv.begin + ln/2
		return r.combine(iv, leafInterval{iv.begin, mid}, leafInterval{mid, iv.end})
	}

	// Otherwise split off the first node of the canonical compact-range
	// decomposition of [begin, end): the largest power-of-two block aligned at
	// begin and not larger than the interval.
	block := uint64(1) << uint(bits.Len64(ln)-1)
	if iv.begin != 0 {
		if tz := uint64(1) << uint(bits.TrailingZeros64(iv.begin)); tz < block {
			block = tz
		}
	}
	return r.combine(iv, leafInterval{iv.begin, iv.begin + block}, leafInterval{iv.begin + block, iv.end})
}

// combine resolves both halves, hashes them together, and caches the result.
func (r *updateResolver) combine(parent, left, right leafInterval) ([]byte, error) {
	l, err := r.resolve(left)
	if err != nil {
		return nil, err
	}
	rh, err := r.resolve(right)
	if err != nil {
		return nil, err
	}
	h := r.hasher.HashChildren(l, rh)
	r.known[parent] = h
	return h, nil
}
