Proof Updating
==============

A client of an append-only transparency log often keeps a single inclusion
proof for a record over a long time, while the log keeps growing. Once the
client has also obtained a consistency proof between two checkpoints it trusts,
both pieces of evidence can be verified independently, but the old inclusion
proof does not by itself prove inclusion in the new tree: an RFC 6962 inclusion
proof for a smaller tree is not an inclusion proof for a larger one.

`proof.UpdateInclusion` closes that gap. It converts an inclusion proof from an
older, smaller tree into an inclusion proof for the same leaf in a newer,
larger tree, using a consistency proof between the two. The client supplies only
digests it already has: the leaf hash, the two roots, the RFC 6962 inclusion
proof, and the RFC 6962 consistency proof. Nothing is fetched from the log,
and no leaf data is re-read.

Intervals, not node IDs
-----------------------

The key point is *what a proof digest stands for*. The bytes passed to
`UpdateInclusion` are the ordinary RFC 6962 proof format: the sequence of
digests that `Nodes.Rehash` produces. In that format the nodes of an
**ephemeral** (incomplete) subtree have already been folded into a single
digest. Such proof elements are *not* the raw per-node digests addressed by
`Nodes.IDs`.

The same on-wire digest therefore stands for a *leaf interval*, and its identity
must be tracked by that interval, not by a `compact.NodeID`. A `NodeID` implies
a fixed width: `{Level: 3, Index: 1}` nominally covers leaves `[8, 16)`. But in
a tree of size 13 the ephemeral node with that ID covers only `[8, 13)` - it is
the folded root of the right-hand fragment. Substituting digests "by ID" would
silently mix intervals of different widths, and would produce a proof that
cannot be verified.

The conversion therefore labels every digest it is given with the interval it
actually commits to:

- every non-ephemeral element of `Nodes.IDs` commits to `NodeID.Coverage()`;
- each ephemeral block `IDs[begin:end)` is folded into exactly one digest, which
  commits to the union of those nodes' leaf intervals, clipped at the tree's
  right edge.

The old root is used only as an opaque digest of `[0, oldSize)`. It is valid
evidence because the consistency proof verifies it, but nothing tries to split
it back into its children: a hash cannot be inverted.

Deriving the new proof
----------------------

To build an element of the proof at the new size, the converter needs the
digest of a particular interval `[begin, end)` of the new tree. It resolves that
interval recursively, using only the labelled evidence:

- if the interval is known, use it;
- an aligned perfect interval is the hash of its two halves;
- any other interval is assembled by folding the first block of its canonical
  compact-range decomposition into the rest.

Recursion stops at known intervals. Because the evidence supplied by an
inclusion proof plus a consistency proof is enough to cover the path to the
leaf, every required element resolves. The work is proportional to the height
of the tree and the lengths of the two proofs - not to `newSize` or to the
number of appended leaves. In particular, the number of `HashChildren` calls is
`O(len(inclusion) + len(consistency) + log(newSize))`.

The old tree's right-edge ephemeral subtree may split apart or merge when the
log grows. Both cases are handled by the same interval recursion, because it
recomputes the fragment's digest from the now-separate children rather than
reusing a folded digest under the old node ID. The target leaf may lie in the
stable left-hand perfect subtree or be the old tree's last leaf; both are
ordinary intervals.

Failure contract
----------------

`UpdateInclusion` returns `(nil, error)` for any invalid input, and never a
partial proof. Inputs that are rejected include:

- `oldSize == 0`, `oldSize > newSize`, or `index >= oldSize`;
- any digest whose length is not `hasher.Size()`;
- an inclusion proof that does not verify against `(leafHash, index, oldSize,
  oldRoot)`;
- a consistency proof that does not verify against `(oldSize, newSize,
  oldRoot, newRoot)`;
- a consistency proof with a missing or extra element, or with reordered
  siblings.

When `oldSize == newSize` the consistency proof must be empty and the two roots
must be equal; the inclusion proof is still verified, and the result is a
freshly owned copy of it.

On success the returned proof is independently owned: it shares no writable
storage with any input, mutating it does not affect a later call, and the input
slices are never modified.
