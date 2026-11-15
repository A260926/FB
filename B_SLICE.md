# B-slice explicit bi-predictive decoding

This document describes the B-slice support added to the portable C
decoder. The original library only decoded I and P slices; B slices were
rejected at the slice-header stage. The decoder now accepts non-reference
B pictures (`nal_ref_idc == 0`) containing the explicit macroblock and
sub-macroblock partition types, while out-of-range syntax is still rejected
rather than hidden behind a concealed or substituted picture.

## Supported scope

A B slice uses two reference picture lists, L0 and L1, each built from the
short-term reference pictures already in the decoded picture buffer (DPB).

The following are supported:

* B slice macroblock types 1..22 (table 7-11), i.e. every explicit
  `Pred_L0`, `Pred_L1` and `BiPred` partition:
  * 16x16, 16x8 and 8x16 macroblock partitions;
  * the `B_8x8` macroblock type;
  * sub-macroblock types 1..12 (table 7-13): 8x8, 8x4, 4x8 and 4x4
    sub-partitions in all three prediction modes.
* Both reference lists on every supported partition, including mixed
  L0/L1 partitions inside one macroblock.
* Quarter-pel luma and eighth-pel chroma interpolation, reusing the
  existing `h264bsdPredictSamples` machinery.
* Unweighted bi-prediction: the L0 and L1 predictions are averaged sample
  by sample with the standard `(p0 + p1 + 1) >> 1` rounding.
* Explicit reference list reordering commands and the PPS/slice active
  reference index counts for both lists.
* Multi-slice B pictures: each slice reuses the per-picture picture order
  count and reference lists, so the POC history advances only once even
  when several slices decode the same access unit.
* Deblocking of B macroblocks, including the two-list reference-identity
  and motion-vector boundary strength rules.
* Intra macroblocks embedded in a B slice reuse the existing intra path.

Explicitly rejected (the decoder returns `H264BSD_ERROR`, it never emits a
substituted picture):

* B direct prediction (`B_Direct_16x16`, sub type `B_B_Direct_8x8`);
* B skip macroblocks (`mb_skip_run` over a B slice);
* weighted bi-prediction and weighted prediction;
* long-term reference pictures;
* reference pictures that are themselves B pictures (only I/P frames are
  allowed as references).

## Reference list construction

For a current B picture with picture order count `POC(C)`, the short-term
references are partitioned by POC relative to the current picture:

* L0 lists the references whose POC is smaller than `POC(C)` in descending
  POC order, followed by the references whose POC is greater than
  `POC(C)` in ascending POC order;
* L1 lists the greater-POC references in ascending POC order followed by
  the smaller-POC references in descending POC order.

When the initial L0 and L1 lists are identical (which happens when every
reference lies on one side of the current picture), the first two entries
of L1 are exchanged, as required by H.264 8.2.4.2.3. The lists are then
passed through the per-slice `ref_pic_list_reordering` syntax. The lists
are stored in `dpbStorage_t.list` (L0) and the new
`dpbStorage_t.list1` (L1); accessor helpers return the picture data for
either list by index.

## Motion vector derivation

Each B partition stores, per 4x4 luma block:

* the reference index and picture data pointer for L0 and/or L1;
* the motion vector for each list it uses;
* a `listUsage` bitmask recording which lists contribute to the block.

The motion vector predictor follows H.264 8.52. Neighbouring 4x4 blocks
are only considered when they use the same list as the partition being
derived. When a neighbour references a different picture in the same
list, the temporal-motion-vector scaling rule is applied; when the
reference pictures are identical the predictor is taken directly. A
partition that does not use a list never inherits motion state from that
list. The final vector is the predictor plus the decoded motion vector
difference, clipped to the permitted luma range.

## Bi-prediction

For a `BiPred` partition the L0 and L1 predictions are each interpolated
into a private, stack-local scratch macroblock (so concurrent
macroblocks cannot overwrite each other) and then averaged with
`(p0 + p1 + 1) >> 1` for luma and both chroma planes. The residual is
added afterwards through the existing `h264bsdWriteOutputBlocks` path, so
the residual handling is identical to the P-slice implementation.

## Output ordering

A B picture is non-reference and is not inserted into the reference
sliding window. It still participates in the normal DPB display-order
queue, so with reordering enabled the pictures are delivered in POC
order; with `noReordering` set the pictures are delivered in decode
order. Flushing after the last NAL unit drains any buffered B picture
through `h264bsdFlushBuffer`.

## Public API

No public function signature changed. A B picture is fed through the same
calls as any other picture:

```c
storage_t dec;
h264bsdInit(&dec, 0);                 /* 0 = display-order output */
u32 used = 0;
u32 rc = h264bsdDecode(&dec, nal, nalLen, picId, &used);
/* ... */
u32 id, isIdr, errMbs;
uint8_t *pic = h264bsdNextOutputPicture(&dec, &id, &isIdr, &errMbs);
h264bsdFlushBuffer(&dec);
h264bsdShutdown(&dec);
```

A complete runnable example is provided in
[`examples/b_slice_decode.c`](../examples/b_slice_decode.c). The
end-to-end behaviour, including list ordering, the L1 swap, fractional
motion, sub-partitions, deblocking and scope rejection, is exercised by
the target test suite in `task_support/target_cases.c`.
