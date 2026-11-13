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

package proof_test

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"math/bits"
	"testing"

	"github.com/transparency-dev/merkle/proof"
	"github.com/transparency-dev/merkle/rfc6962"
	"github.com/transparency-dev/merkle/testonly"
)

const testMaxLeaves = 33

// buildTree returns a reference tree holding testMaxLeaves distinct leaves.
func buildTree(t *testing.T) *testonly.Tree {
	t.Helper()
	tr := testonly.New(rfc6962.DefaultHasher)
	for i := 0; i < testMaxLeaves; i++ {
		tr.AppendData([]byte(fmt.Sprintf("entry-%d", i)))
	}
	return tr
}

// updateOnce runs UpdateInclusion for one geometry and checks that the result
// is byte-identical to the proof the reference tree independently re-issues for
// the new size, and that VerifyInclusion accepts it.
func updateOnce(t *testing.T, tr *testonly.Tree, index, oldSize, newSize uint64) {
	t.Helper()
	h := rfc6962.DefaultHasher
	inclusion, err := tr.InclusionProof(index, oldSize)
	if err != nil {
		t.Fatalf("InclusionProof(%d, %d): %v", index, oldSize, err)
	}
	consistency, err := tr.ConsistencyProof(oldSize, newSize)
	if err != nil {
		t.Fatalf("ConsistencyProof(%d, %d): %v", oldSize, newSize, err)
	}
	want, err := tr.InclusionProof(index, newSize)
	if err != nil {
		t.Fatalf("InclusionProof(%d, %d): %v", index, newSize, err)
	}
	leafHash, oldRoot, newRoot := tr.LeafHash(index), tr.HashAt(oldSize), tr.HashAt(newSize)

	got, err := proof.UpdateInclusion(h, index, oldSize, newSize, leafHash, oldRoot, newRoot, inclusion, consistency)
	if err != nil {
		t.Fatalf("UpdateInclusion(%d, %d->%d): %v", index, oldSize, newSize, err)
	}
	if want, got := len(want), len(got); want != got {
		t.Fatalf("UpdateInclusion(%d, %d->%d): got %d elements, want %d", index, oldSize, newSize, got, want)
	}
	for i := range want {
		if !bytes.Equal(got[i], want[i]) {
			t.Fatalf("UpdateInclusion(%d, %d->%d): element %d = %x, want %x", index, oldSize, newSize, i, got[i], want[i])
		}
	}
	if err := proof.VerifyInclusion(h, index, newSize, leafHash, got, newRoot); err != nil {
		t.Fatalf("VerifyInclusion(%d, %d) rejected converted proof: %v", index, newSize, err)
	}
}

// TestUpdateInclusion checks the conversion against independently re-issued
// reference proofs for every geometry of small trees, plus the special cases
// called out in the design: single-leaf trees, power-of-two boundaries,
// repeated conversions, and aliasing of inputs and outputs.
func TestUpdateInclusion(t *testing.T) {
	tr := buildTree(t)
	defer func() {
		if r := recover(); r != nil {
			t.Fatalf("panic: %v", r)
		}
	}()

	// Exhaustive: every oldSize/newSize/index up to testMaxLeaves leaves.
	for oldSize := uint64(1); oldSize <= testMaxLeaves; oldSize++ {
		for newSize := oldSize; newSize <= testMaxLeaves; newSize++ {
			for index := uint64(0); index < oldSize; index++ {
				updateOnce(t, tr, index, oldSize, newSize)
			}
		}
	}

	// Single-leaf trees, including growth from a one-leaf log.
	for _, size := range []uint64{1, 2, 3, testMaxLeaves} {
		updateOnce(t, tr, 0, 1, size)
	}

	// Power-of-two boundaries: perfect trees growing by one leaf, and complete
	// trees reached from just below.
	for _, c := range []struct{ oldSize, newSize uint64 }{
		{1, 2}, {2, 4}, {3, 4}, {4, 5}, {7, 8}, {8, 9}, {15, 16}, {16, 17},
		{31, 32}, {32, 33}, {1, 32}, {17, 33}, {7, 13}, {15, 17},
	} {
		updateOnce(t, tr, 0, c.oldSize, c.newSize)
		updateOnce(t, tr, c.oldSize-1, c.oldSize, c.newSize)
		updateOnce(t, tr, c.oldSize/2, c.oldSize, c.newSize)
	}

	// Consecutive conversions: each step consumes the proof produced by the
	// previous step (with the new size as the next old size). The leaf stays in
	// the stable left side, and is also tracked as the old tree's last leaf.
	t.Run("consecutive", func(t *testing.T) {
		h := rfc6962.DefaultHasher
		sizes := []uint64{1, 2, 3, 5, 7, 8, 9, 13, 15, 17, 32, 33}
		index := uint64(0)
		cur := sizes[0]
		leafHash := tr.LeafHash(index)
		proofNow, err := tr.InclusionProof(index, cur)
		if err != nil {
			t.Fatal(err)
		}
		for i := 1; i < len(sizes); i++ {
			next := sizes[i]
			oldRoot, newRoot := tr.HashAt(cur), tr.HashAt(next)
			consistency, err := tr.ConsistencyProof(cur, next)
			if err != nil {
				t.Fatal(err)
			}
			proofNow, err = proof.UpdateInclusion(h, index, cur, next,
				leafHash, oldRoot, newRoot, proofNow, consistency)
			if err != nil {
				t.Fatalf("step %d (%d->%d): %v", i, cur, next, err)
			}
			want, err := tr.InclusionProof(index, next)
			if err != nil {
				t.Fatal(err)
			}
			if !equalHashes(proofNow, want) {
				t.Fatalf("step %d (%d->%d): proof differs from re-issued proof", i, cur, next)
			}
			if err := proof.VerifyInclusion(h, index, next, leafHash, proofNow, newRoot); err != nil {
				t.Fatalf("step %d (%d->%d): %v", i, cur, next, err)
			}
			cur = next
		}
	})

	// The target leaf as the old tree's last leaf on every step.
	t.Run("last-leaf", func(t *testing.T) {
		h := rfc6962.DefaultHasher
		for oldSize := uint64(1); oldSize < testMaxLeaves; oldSize++ {
			index := oldSize - 1
			leafHash := tr.LeafHash(index)
			for newSize := oldSize + 1; newSize <= testMaxLeaves; newSize++ {
				inclusion, err := tr.InclusionProof(index, oldSize)
				if err != nil {
					t.Fatal(err)
				}
				consistency, err := tr.ConsistencyProof(oldSize, newSize)
				if err != nil {
					t.Fatal(err)
				}
				got, err := proof.UpdateInclusion(h, index, oldSize, newSize,
					leafHash, tr.HashAt(oldSize), tr.HashAt(newSize), inclusion, consistency)
				if err != nil {
					t.Fatalf("%d->%d: %v", oldSize, newSize, err)
				}
				want, err := tr.InclusionProof(index, newSize)
				if err != nil {
					t.Fatal(err)
				}
				if !equalHashes(got, want) {
					t.Fatalf("%d->%d: proof differs from re-issued proof", oldSize, newSize)
				}
			}
		}
	})

	// Aliasing: inputs must be left untouched, and the result must own its
	// storage, so mutating it cannot corrupt inputs or a later conversion.
	t.Run("aliasing", func(t *testing.T) {
		h := rfc6962.DefaultHasher
		const index, oldSize, newSize = 4, 7, 13
		inclusion, err := tr.InclusionProof(index, oldSize)
		if err != nil {
			t.Fatal(err)
		}
		consistency, err := tr.ConsistencyProof(oldSize, newSize)
		if err != nil {
			t.Fatal(err)
		}
		leafHash, oldRoot, newRoot := tr.LeafHash(index), tr.HashAt(oldSize), tr.HashAt(newSize)
		want, err := tr.InclusionProof(index, newSize)
		if err != nil {
			t.Fatal(err)
		}

		inputs := [][][]byte{inclusion, consistency}
		before := make([][][]byte, len(inputs))
		for i, s := range inputs {
			before[i] = deepCopyHashes(s)
		}
		leafBefore, oldBefore, newBefore := bytes.Clone(leafHash), bytes.Clone(oldRoot), bytes.Clone(newRoot)

		got, err := proof.UpdateInclusion(h, index, oldSize, newSize, leafHash, oldRoot, newRoot, inclusion, consistency)
		if err != nil {
			t.Fatal(err)
		}
		if !equalHashes(got, want) {
			t.Fatal("first conversion differs from re-issued proof")
		}

		for i, s := range inputs {
			if !equalHashes(s, before[i]) {
				t.Fatalf("input %d was modified", i)
			}
		}
		if !bytes.Equal(leafHash, leafBefore) || !bytes.Equal(oldRoot, oldBefore) || !bytes.Equal(newRoot, newBefore) {
			t.Fatal("digest argument was modified")
		}

		// Mutate the returned proof in every element; the inputs and any later
		// conversion must be unaffected.
		for _, h := range got {
			for j := range h {
				h[j] ^= 0xff
			}
		}
		for i, s := range inputs {
			if !equalHashes(s, before[i]) {
				t.Fatalf("mutating the result modified input %d", i)
			}
		}

		again, err := proof.UpdateInclusion(h, index, oldSize, newSize, leafHash, oldRoot, newRoot, inclusion, consistency)
		if err != nil {
			t.Fatal(err)
		}
		if !equalHashes(again, want) {
			t.Fatal("conversion after mutating a previous result differs from re-issued proof")
		}

		// Conversely, mutating inputs after obtaining the result must not change
		// the result.
		resultCopy := deepCopyHashes(again)
		for _, s := range inputs {
			for _, h := range s {
				for j := range h {
					h[j] ^= 0xff
				}
			}
		}
		if !equalHashes(again, resultCopy) {
			t.Fatal("mutating inputs changed an already returned proof")
		}
	})
}

// TestUpdateValidation checks that invalid arguments or evidence produce a nil
// proof and a non-nil error, never a partial result.
func TestUpdateValidation(t *testing.T) {
	h := rfc6962.DefaultHasher
	tr := buildTree(t)
	const index, oldSize, newSize = 4, 7, 13

	validInclusion, err := tr.InclusionProof(index, oldSize)
	if err != nil {
		t.Fatal(err)
	}
	validConsistency, err := tr.ConsistencyProof(oldSize, newSize)
	if err != nil {
		t.Fatal(err)
	}
	leafHash, oldRoot, newRoot := tr.LeafHash(index), tr.HashAt(oldSize), tr.HashAt(newSize)

	type testCase struct {
		name        string
		index       uint64
		oldSize     uint64
		newSize     uint64
		leafHash    []byte
		oldRoot     []byte
		newRoot     []byte
		inclusion   [][]byte
		consistency [][]byte
	}
	cases := []testCase{{
		name:        "replaced old root",
		index:       index,
		oldSize:     oldSize,
		newSize:     newSize,
		leafHash:    leafHash,
		oldRoot:     tr.HashAt(oldSize - 1),
		newRoot:     newRoot,
		inclusion:   validInclusion,
		consistency: validConsistency,
	}, {
		name:        "replaced new root",
		index:       index,
		oldSize:     oldSize,
		newSize:     newSize,
		leafHash:    leafHash,
		oldRoot:     oldRoot,
		newRoot:     tr.HashAt(newSize - 1),
		inclusion:   validInclusion,
		consistency: validConsistency,
	}, {
		name:        "replaced leaf hash",
		index:       index,
		oldSize:     oldSize,
		newSize:     newSize,
		leafHash:    tr.LeafHash(index + 1),
		oldRoot:     oldRoot,
		newRoot:     newRoot,
		inclusion:   validInclusion,
		consistency: validConsistency,
	}, {
		name:        "inclusion with missing element",
		index:       index,
		oldSize:     oldSize,
		newSize:     newSize,
		leafHash:    leafHash,
		oldRoot:     oldRoot,
		newRoot:     newRoot,
		inclusion:   validInclusion[:len(validInclusion)-1],
		consistency: validConsistency,
	}, {
		name:        "inclusion with extra element",
		index:       index,
		oldSize:     oldSize,
		newSize:     newSize,
		leafHash:    leafHash,
		oldRoot:     oldRoot,
		newRoot:     newRoot,
		inclusion:   append(deepCopyHashes(validInclusion), deepCopyHashes(validInclusion)[0]),
		consistency: validConsistency,
	}, {
		name:        "inclusion with swapped siblings",
		index:       index,
		oldSize:     oldSize,
		newSize:     newSize,
		leafHash:    leafHash,
		oldRoot:     oldRoot,
		newRoot:     newRoot,
		inclusion:   swapped(validInclusion),
		consistency: validConsistency,
	}, {
		name:        "consistency with missing element",
		index:       index,
		oldSize:     oldSize,
		newSize:     newSize,
		leafHash:    leafHash,
		oldRoot:     oldRoot,
		newRoot:     newRoot,
		inclusion:   validInclusion,
		consistency: validConsistency[:len(validConsistency)-1],
	}, {
		name:        "consistency with extra element",
		index:       index,
		oldSize:     oldSize,
		newSize:     newSize,
		leafHash:    leafHash,
		oldRoot:     oldRoot,
		newRoot:     newRoot,
		inclusion:   validInclusion,
		consistency: append(deepCopyHashes(validConsistency), deepCopyHashes(validConsistency)[0]),
	}, {
		name:        "consistency with swapped siblings",
		index:       index,
		oldSize:     oldSize,
		newSize:     newSize,
		leafHash:    leafHash,
		oldRoot:     oldRoot,
		newRoot:     newRoot,
		inclusion:   validInclusion,
		consistency: swapped(validConsistency),
	}, {
		name:        "short leaf hash",
		index:       index,
		oldSize:     oldSize,
		newSize:     newSize,
		leafHash:    leafHash[:h.Size()-1],
		oldRoot:     oldRoot,
		newRoot:     newRoot,
		inclusion:   validInclusion,
		consistency: validConsistency,
	}, {
		name:        "long leaf hash",
		index:       index,
		oldSize:     oldSize,
		newSize:     newSize,
		leafHash:    append(bytes.Clone(leafHash), 0),
		oldRoot:     oldRoot,
		newRoot:     newRoot,
		inclusion:   validInclusion,
		consistency: validConsistency,
	}, {
		name:        "short old root",
		index:       index,
		oldSize:     oldSize,
		newSize:     newSize,
		leafHash:    leafHash,
		oldRoot:     oldRoot[:h.Size()-1],
		newRoot:     newRoot,
		inclusion:   validInclusion,
		consistency: validConsistency,
	}, {
		name:        "long new root",
		index:       index,
		oldSize:     oldSize,
		newSize:     newSize,
		leafHash:    leafHash,
		oldRoot:     oldRoot,
		newRoot:     append(bytes.Clone(newRoot), 0),
		inclusion:   validInclusion,
		consistency: validConsistency,
	}, {
		name:        "short inclusion element",
		index:       index,
		oldSize:     oldSize,
		newSize:     newSize,
		leafHash:    leafHash,
		oldRoot:     oldRoot,
		newRoot:     newRoot,
		inclusion:   truncateElement(validInclusion, 0),
		consistency: validConsistency,
	}, {
		name:        "long consistency element",
		index:       index,
		oldSize:     oldSize,
		newSize:     newSize,
		leafHash:    leafHash,
		oldRoot:     oldRoot,
		newRoot:     newRoot,
		inclusion:   validInclusion,
		consistency: truncateElement(validConsistency, 0),
	}, {
		name:        "empty old tree",
		index:       0,
		oldSize:     0,
		newSize:     newSize,
		leafHash:    leafHash,
		oldRoot:     rfc6962.DefaultHasher.EmptyRoot(),
		newRoot:     newRoot,
		inclusion:   nil,
		consistency: validConsistency,
	}, {
		name:        "old tree larger than new",
		index:       index,
		oldSize:     newSize,
		newSize:     oldSize,
		leafHash:    leafHash,
		oldRoot:     newRoot,
		newRoot:     oldRoot,
		inclusion:   nil,
		consistency: validConsistency,
	}, {
		name:        "index out of bounds",
		index:       oldSize,
		oldSize:     oldSize,
		newSize:     newSize,
		leafHash:    leafHash,
		oldRoot:     oldRoot,
		newRoot:     newRoot,
		inclusion:   validInclusion,
		consistency: validConsistency,
	}}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			got, err := proof.UpdateInclusion(h, tc.index, tc.oldSize, tc.newSize,
				tc.leafHash, tc.oldRoot, tc.newRoot, tc.inclusion, tc.consistency)
			if err == nil {
				t.Fatalf("got no error, want one; result %x", got)
			}
			if got != nil {
				t.Fatalf("got partial result %x alongside error %v", got, err)
			}
		})
	}

	// Equal sizes are a valid conversion, but only with an empty consistency
	// proof and equal roots. Both violations must be rejected.
	t.Run("equal sizes", func(t *testing.T) {
		inclusion := deepCopyHashes(validInclusion)
		got, err := proof.UpdateInclusion(h, index, oldSize, oldSize, leafHash, oldRoot, oldRoot, inclusion, nil)
		if err != nil {
			t.Fatalf("valid equal-size conversion failed: %v", err)
		}
		if !equalHashes(got, inclusion) {
			t.Fatal("equal-size result is not a copy of the inclusion proof")
		}

		if got, err := proof.UpdateInclusion(h, index, oldSize, oldSize, leafHash, oldRoot, oldRoot, inclusion, validConsistency); err == nil || got != nil {
			t.Fatalf("non-empty consistency proof accepted for equal sizes: %x", got)
		}
		if got, err := proof.UpdateInclusion(h, index, oldSize, oldSize, leafHash, oldRoot, newRoot, inclusion, nil); err == nil || got != nil {
			t.Fatalf("unequal roots accepted for equal sizes: %x", got)
		}
		// The inclusion proof is still checked when the sizes are equal.
		if got, err := proof.UpdateInclusion(h, index, oldSize, oldSize, leafHash, oldRoot, oldRoot, truncateElement(inclusion, 0), nil); err == nil || got != nil {
			t.Fatalf("invalid inclusion proof accepted for equal sizes: %x", got)
		}
	})
}

// TestUpdateGeometry exercises the conversion on logical trees whose size is
// close to 2^63, without materializing leaves. Small symbolic node digests
// label each node by the leaf interval it covers, so proofs for huge trees can
// be constructed from a few witness nodes, and the number of HashChildren calls
// can be counted against the documented bound.
func TestUpdateGeometry(t *testing.T) {
	const boundFactor = 256

	run := func(t *testing.T, index, oldSize, newSize uint64) {
		t.Helper()
		ch := newCountingHasher()
		leafHash := intervalDigest(index, index+1)
		oldRoot := intervalDigest(0, oldSize)
		newRoot := intervalDigest(0, newSize)

		inclusion, err := symbolicProof(ch, mustInclusion(t, index, oldSize))
		if err != nil {
			t.Fatalf("building inclusion proof: %v", err)
		}
		var consistency [][]byte
		if oldSize != newSize {
			consistency, err = symbolicProof(ch, mustConsistency(t, oldSize, newSize))
			if err != nil {
				t.Fatalf("building consistency proof: %v", err)
			}
		}

		ch.calls = 0
		got, err := proof.UpdateInclusion(ch, index, oldSize, newSize, leafHash, oldRoot, newRoot, inclusion, consistency)
		if err != nil {
			t.Fatalf("UpdateInclusion(%d, %d->%d): %v", index, oldSize, newSize, err)
		}
		// The documented bound covers UpdateInclusion's own work: both evidence
		// verifications, the output check, and the conversion.
		if max := boundFactor * (bits.Len64(newSize) + 1); ch.calls > max {
			t.Fatalf("HashChildren called %d times, bound is %d", ch.calls, max)
		}
		want, err := symbolicProof(ch, mustInclusion(t, index, newSize))
		if err != nil {
			t.Fatalf("building expected proof: %v", err)
		}
		if !equalHashes(got, want) {
			t.Fatalf("UpdateInclusion(%d, %d->%d) does not match re-issued proof", index, oldSize, newSize)
		}
		if err := proof.VerifyInclusion(ch, index, newSize, leafHash, got, newRoot); err != nil {
			t.Fatalf("VerifyInclusion(%d, %d): %v", index, newSize, err)
		}
		if ch.bad {
			t.Fatal("symbolic hasher observed a non-canonical fold")
		}
	}

	const big = uint64(1) << 62
	const huge = uint64(1) << 63
	const max = ^uint64(0)
	geometries := []struct{ index, oldSize, newSize uint64 }{
		// The small-tree cases from the design, scaled up.
		{0, big + 7, big + 13},
		{big + 4, big + 7, big + 13},
		{big + 6, big + 7, big + 13},
		{0, big + 15, big + 17},
		{big + 14, big + 15, big + 17},
		{big + 7, big + 8, big + 9},
		// Sizes at the top of the uint64 range, near 2^63.
		{0, huge + 1, huge + 5},
		{huge, huge + 1, huge + 5},
		{huge + 4, huge + 1 + 4, huge + 1 + 12},
		{huge - 1, huge, huge + 3},
		// Sizes within a handful of leaves of 2^64-1, where naive
		// (index+1)<<level coverage arithmetic would wrap around.
		{0, max - 6, max - 2},
		{max - 7, max - 6, max - 2},
		{max - 3, max - 2, max},
		{max - 5, max - 4, max},
		// Growth by an enormous number of leaves from a small old tree. Proof
		// lengths stay O(log newSize), and the conversion must not allocate or
		// loop per appended leaf.
		{0, 7, huge + 13},
		{6, 7, huge + 13},
		{0, 5, big},
		{4, 5, big},
		{0, 13, max},
		{12, 13, max},
		{0, 3, max - 1},
		{2, 3, max - 1},
	}
	for _, g := range geometries {
		t.Run(fmt.Sprintf("%d_%d_to_%d", g.index, g.oldSize, g.newSize), func(t *testing.T) {
			run(t, g.index, g.oldSize, g.newSize)
		})
	}

	// Equal sizes on a huge logical tree: this returns a copy of the inclusion
	// proof, still within the same HashChildren bound.
	t.Run("equal-size", func(t *testing.T) {
		ch := newCountingHasher()
		index, size := huge+3, huge+9
		leafHash := intervalDigest(index, index+1)
		root := intervalDigest(0, size)
		inclusion, err := symbolicProof(ch, mustInclusion(t, index, size))
		if err != nil {
			t.Fatal(err)
		}
		got, err := proof.UpdateInclusion(ch, index, size, size, leafHash, root, root, inclusion, nil)
		if err != nil {
			t.Fatalf("equal-size conversion failed: %v", err)
		}
		if !equalHashes(got, inclusion) {
			t.Fatal("equal-size result is not a copy of the inclusion proof")
		}
		if ch.bad {
			t.Fatal("symbolic hasher observed non-adjacent children")
		}
		if max := boundFactor * (bits.Len64(size) + 1); ch.calls > max {
			t.Fatalf("HashChildren called %d times, bound is %d", ch.calls, max)
		}
	})

	// Corrupted evidence on a huge tree must fail without a partial result,
	// while still staying within the work bound.
	t.Run("corruption", func(t *testing.T) {
		index, oldSize, newSize := uint64(0), huge+7, huge+13
		leafHash := intervalDigest(index, index+1)
		oldRoot, newRoot := intervalDigest(0, oldSize), intervalDigest(0, newSize)
		inclusion, err := symbolicProof(newCountingHasher(), mustInclusion(t, index, oldSize))
		if err != nil {
			t.Fatal(err)
		}
		consistency, err := symbolicProof(newCountingHasher(), mustConsistency(t, oldSize, newSize))
		if err != nil {
			t.Fatal(err)
		}
		if len(consistency) < 2 || len(inclusion) < 2 {
			t.Fatalf("expected multi-element proofs, got %d/%d", len(inclusion), len(consistency))
		}

		corruptions := []struct {
			name        string
			leafHash    []byte
			oldRoot     []byte
			newRoot     []byte
			inclusion   [][]byte
			consistency [][]byte
		}{
			{"old root", leafHash, intervalDigest(0, oldSize-1), newRoot, inclusion, consistency},
			{"new root", leafHash, oldRoot, intervalDigest(0, newSize-1), inclusion, consistency},
			{"leaf", intervalDigest(index, index+1)[:8], oldRoot, newRoot, inclusion, consistency},
			{"inclusion missing", leafHash, oldRoot, newRoot, inclusion[:len(inclusion)-1], consistency},
			{"inclusion extra", leafHash, oldRoot, newRoot, append(deepCopyHashes(inclusion), intervalDigest(0, 1)), consistency},
			{"inclusion swapped", leafHash, oldRoot, newRoot, swapped(inclusion), consistency},
			{"consistency missing", leafHash, oldRoot, newRoot, inclusion, consistency[:len(consistency)-1]},
			{"consistency extra", leafHash, oldRoot, newRoot, inclusion, append(deepCopyHashes(consistency), intervalDigest(0, 1))},
			{"consistency swapped", leafHash, oldRoot, newRoot, inclusion, swapped(consistency)},
		}
		for _, c := range corruptions {
			t.Run(c.name, func(t *testing.T) {
				ch := newCountingHasher()
				got, err := proof.UpdateInclusion(ch, index, oldSize, newSize, c.leafHash, c.oldRoot, c.newRoot, c.inclusion, c.consistency)
				if err == nil {
					t.Fatalf("corrupted evidence accepted: %x", got)
				}
				if got != nil {
					t.Fatalf("partial result returned with error: %x", got)
				}
				if max := boundFactor * (bits.Len64(newSize) + 1); ch.calls > max {
					t.Fatalf("HashChildren called %d times, bound is %d", ch.calls, max)
				}
			})
		}
	})
}

// mustInclusion returns the proof.Nodes for an inclusion proof or fails the
// test. It is used only by the symbolic geometry tests.
func mustInclusion(t *testing.T, index, size uint64) proof.Nodes {
	t.Helper()
	n, err := proof.Inclusion(index, size)
	if err != nil {
		t.Fatalf("Inclusion(%d, %d): %v", index, size, err)
	}
	return n
}

func mustConsistency(t *testing.T, size1, size2 uint64) proof.Nodes {
	t.Helper()
	n, err := proof.Consistency(size1, size2)
	if err != nil {
		t.Fatalf("Consistency(%d, %d): %v", size1, size2, err)
	}
	return n
}

// symbolicHasher is a LogHasher whose digests encode the half-open leaf
// interval a node covers. It is a test double, not a cryptographic hash: its
// purpose is to make the RFC 6962 fold structure exact without materializing
// leaves. Every node digest is a pure function of its interval, so a node has
// the same digest however the tree around it was assembled.
//
// HashChildren only accepts the unique canonical RFC 6962 split of the parent
// interval: the binary halves of an aligned perfect node, or the first compact
// decomposition block otherwise. Any other pair - a swapped sibling order, or
// a fold of the wrong shape - yields a poison digest that no correct tree can
// produce, so corrupt evidence fails verification instead of coincidentally
// hashing to the right interval.
type symbolicHasher struct {
	size int
}

// testInterval mirrors the leaf interval identity used by the implementation.
type testInterval struct {
	begin, end uint64
}

func intervalDigest(begin, end uint64) []byte {
	out := make([]byte, 16)
	binary.BigEndian.PutUint64(out[0:8], begin)
	binary.BigEndian.PutUint64(out[8:16], end)
	return out
}

func parseInterval(d []byte) (begin, end uint64) {
	if len(d) != 16 {
		return ^uint64(0), ^uint64(0)
	}
	return binary.BigEndian.Uint64(d[0:8]), binary.BigEndian.Uint64(d[8:16])
}

// poisonDigest is returned for a non-canonical fold. Its interval can never be
// a real node interval, because begin > end.
func poisonDigest() []byte { return intervalDigest(^uint64(0), 0) }

func (h symbolicHasher) EmptyRoot() []byte           { return intervalDigest(0, 0) }
func (h symbolicHasher) HashLeaf(leaf []byte) []byte { return intervalDigest(0, 1) }
func (h symbolicHasher) Size() int                   { return h.size }

func (h symbolicHasher) HashChildren(l, r []byte) []byte {
	lb, le := parseInterval(l)
	rb, re := parseInterval(r)
	if le != rb || lb >= le || rb >= re {
		return poisonDigest()
	}
	// The parent interval [lb, re) has exactly one canonical split; require it.
	if left, _, ok := canonicalSplit(lb, re); !ok || left != (testInterval{lb, le}) {
		return poisonDigest()
	}
	return intervalDigest(lb, re)
}

// canonicalSplit returns the left part of the unique canonical fold of the
// interval [begin, end), and whether such a fold exists.
func canonicalSplit(begin, end uint64) (testInterval, testInterval, bool) {
	if begin >= end {
		return testInterval{}, testInterval{}, false
	}
	ln := end - begin
	if ln&(ln-1) == 0 && begin%ln == 0 {
		return testInterval{begin, begin + ln/2}, testInterval{begin + ln/2, end}, true
	}
	block := uint64(1) << uint(bits.Len64(ln)-1)
	if begin != 0 {
		if tz := uint64(1) << uint(bits.TrailingZeros64(begin)); tz < block {
			block = tz
		}
	}
	return testInterval{begin, begin + block}, testInterval{begin + block, end}, true
}

// countingHasher wraps the symbolic hasher, counts HashChildren calls, and
// records whether any fold was non-canonical.
type countingHasher struct {
	symbolicHasher
	calls int
	bad   bool
}

func newCountingHasher() *countingHasher {
	return &countingHasher{symbolicHasher: symbolicHasher{size: 16}}
}

func (h *countingHasher) HashChildren(l, r []byte) []byte {
	h.calls++
	out := h.symbolicHasher.HashChildren(l, r)
	if b, e := parseInterval(out); b == ^uint64(0) && e == 0 {
		h.bad = true
	}
	return out
}

// symbolicProof rehashes the node IDs of a proof using interval digests. The
// result is the exact RFC 6962 proof slice a tree with this hasher would issue.
func symbolicProof(h *countingHasher, nodes proof.Nodes) ([][]byte, error) {
	hashes := make([][]byte, len(nodes.IDs))
	for i, id := range nodes.IDs {
		var begin, end uint64
		if id.Level >= 64 {
			begin, end = ^uint64(0), ^uint64(0)
		} else {
			begin = id.Index << id.Level
			if id.Index+1 > ^uint64(0)>>id.Level {
				end = ^uint64(0)
			} else {
				end = (id.Index + 1) << id.Level
			}
		}
		hashes[i] = intervalDigest(begin, end)
	}
	return nodes.Rehash(hashes, h.HashChildren)
}

func equalHashes(a, b [][]byte) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if !bytes.Equal(a[i], b[i]) {
			return false
		}
	}
	return true
}

func deepCopyHashes(s [][]byte) [][]byte {
	out := make([][]byte, len(s))
	for i, h := range s {
		out[i] = bytes.Clone(h)
	}
	return out
}

// swapped returns a copy of the proof with its first two elements exchanged.
// Proofs used with it always have at least two elements.
func swapped(s [][]byte) [][]byte {
	out := deepCopyHashes(s)
	out[0], out[1] = out[1], out[0]
	return out
}

// truncateElement returns a copy of the proof with element i shortened by one
// byte.
func truncateElement(s [][]byte, i int) [][]byte {
	out := deepCopyHashes(s)
	out[i] = out[i][:len(out[i])-1]
	return out
}
