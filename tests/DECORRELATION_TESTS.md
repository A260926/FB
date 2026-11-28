# WavPack integer decorrelation regression

This document describes the independent decorrelation reference added under
`tests/` and driven by `task_support/check_decorrelation.sh`. It exists so a
portable C decoder can be checked against something other than "encode with the
library, then decode with the library."

Scope is deliberately narrow: **lossless, 16- and 24-bit integer PCM, mono and
stereo, assembly and worker threads disabled**. Floating point, DSD, hybrid and
lossy modes, and noise shaping are out of scope.

## What is new, and what is not

Added:

- `tests/decorr_model.h` — an independent, natural-time implementation of the
  lossless integer decorrelation stage, plus the format conversions it needs.
- `tests/decorr_reference.c` — unit test that drives the model directly.
- `tests/decorr_blocks.c` — integration test that builds planned WavPack blocks
  and decodes them through the public API.
- `tests/decorr_blocks_shim.c` — test-only shim that reuses the library's
  entropy *writer* to encode a residual plan the test chose itself.
- `task_support/check_decorrelation.sh` — builds/runs everything at `-O0` and
  `-O2` and performs two mutation checks.

Changed (test-only, disabled by default):

- `src/wavpack_local.h` and `src/unpack.c` gain read-only observation hooks,
  compiled **only** when `WAVPACK_DECORR_OBSERVE` is defined. The normal
  library build contains no callbacks, no observer globals and no extra state;
  `nm` on the ordinary archive shows no observer symbols. The hooks record the
  entropy residuals entering each pass and the state each pass leaves, and
  never change a decode decision.

The production algorithm, the public API and the existing tests are unchanged.
`bash task_support/baseline.sh` still round-trips 1025 frames of six-channel
24-bit audio and re-reads from a seek position.

## Why a second, natural-time model

The production decoder runs each decorrelation pass over a whole buffer and
keeps history in a rotating ring (`samples_A[8]`, indexed by `m`), then
renormalizes the ring at the end of a call. That layout makes partial reads
(1, 7, 17 frames) awkward to reason about and easy to "test" by copying the ring
into the test.

The reference model instead stores, per pass, a plain shift queue in **natural
time**: `hist_A[0]` is the most recent reconstructed sample, `hist_A[1]` the one
before, and so on. Applying a pass is a loop over frames with an explicit shift.
Because the queue is natural time, it does not need to be rotated between
calls: a 1-frame call, a 7-frame call and a whole-block call leave exactly the
same queue contents. That is the property the integration test checks by
decoding the same image at chunk sizes 1, 7, 17 and whole-block and requiring
all four to match the one model sequence value by value.

## Prediction, weights and history (the rules the model implements)

- **Prediction contribution** is `floor((weight * source + 512) / 1024)`,
  computed with a 64-bit dividend and an explicit floor correction, so negative
  half-steps round *down*. It is never defined by overflow: the tests check
  every intermediate against `INT32_MIN..INT32_MAX` and fail if one leaves the
  range.
- **Weight update** looks at the sign of the prediction *source* and of the
  current *residual* (the value entering that pass). Same sign grows the weight,
  opposite sign shrinks it, and a zero source or residual leaves it alone. It
  does **not** look at the reconstructed sample.
- **Weight clamp**: negative terms (`-1`, `-2`, `-3`) use the clipped update,
  limiting the weight to `±1024` on both ends. Ordinary positive terms use the
  unclipped update. The two rules are distinct and a positive term must not
  borrow the negative clamp.
- **Positive terms 1..8** are simple delays: the source is the reconstructed
  sample `term` ticks back in the same channel.
- **Terms 17 and 18** are two-point extrapolations: `17` is `2*a - b`, `18` is
  `floor((3*a - b)/2)` where `a` is the most recent and `b` the previous
  reconstructed sample.
- **Negative terms** have directional cross-channel dependencies:
  - `-1` restores this tick's left first, then uses it as the source for the
    right channel; the stored value for the next tick is this tick's right.
  - `-2` is the mirror: right first, then left; the stored value is this tick's
    left.
  - `-3` uses the saved *previous tick's opposite channel* for each side (left
    uses the previous right, right uses the previous left), then stores this
    tick's outputs crossed for the next tick.

## Bitstream plan vs. model state

A WavPack block declares its decorrelation state in metadata:

- `ID_DECORR_TERMS` lists the terms, one byte each (`(term+5) & 0x1f` in the
  low five bits, delta in the top three).
- `ID_DECORR_WEIGHTS` stores weights as signed bytes (the format's
  `store_weight`/`restore_weight` conversion).
- `ID_DECORR_SAMPLES` stores history as signed 16-bit base-2 logarithms.

The metadata lists terms in **declaration order**, which is the reverse of the
order the decoder executes them. The model consumes declaration order and is
told the execution order explicitly (`exec_order`), so a chain whose declared
order differs from decoding order is decoded correctly. The integration test
includes such a mixed `+/-` multi-stage chain and separately proves that using
declaration order as execution order produces different output.

The tests recover the block's initial state **only** from the metadata bytes,
using `dm_plan_from_metadata()`, which parses terms/weights/samples itself and
applies the format's fill rule: omitted trailing weights read as zero, and
omitted trailing histories read as zero. The decoder's post-restore structs are
never used as model inputs.

Every block re-declares its state, so the tests roll the metadata forward from
the end-of-block model state and re-seed the model by parsing those same bytes
again. This is how "cross-block state comes from the new block's metadata" is
exercised rather than assumed.

### Natural time vs. the format's stored order

For terms 1..8 the stored history is oldest-first and has exactly `term`
entries. The model stores newest-first, so it reverses on ingest. Terms 17/18
store two entries newest-first; negative terms store one saved value per
channel. `dm_pass_view()` reproduces these stored layouts for comparison.

The end-of-call normalization for delayed terms is checked exactly and at two
granularities:

- **Per block**: after a whole-block decode, the observation hook reports the
  normalized history each pass leaves, and the test compares it slot by slot
  against the model's view.
- **Per return**: a block is decoded in 7-frame chunks, and after *every*
  return the model's natural-time state is compared against the state the
  decoder reports for that same call. This is where crossing a chunk boundary
  mid-block would expose a rotation bug.

Slots `0..term-1` are compared for positive terms; padding slots above `term`
are stale in production and are ignored. The test never copies the production
ring and never compares the ring to itself; both sides are expressed in natural
time and then in the format's stored order.

## What the tests build

The integration test writes its own blocks rather than asking the encoder to
choose terms:

1. A `ChainSpec` fixes the term list, deltas and initial weights.
2. A bounded target PCM signal is chosen (small asymmetric drive, zero and
   negative half-step regions, a couple of excursions).
3. The model **forward-encodes** that target through the same chain to obtain
   the residual plan. Forward encoding inverts the decoder passes in the
   opposite order, which is exactly declaration order; this guarantees the
   plan decodes back to the chosen PCM without any production code.
4. The residual plan is encoded with the library's entropy writer (via the
   shim) and written as `ID_WV_BITSTREAM`.
5. The expected PCM is the model's own decode of that residual plan, and the
   block CRC in the header is computed from that same expected PCM, so a CRC
   complaint can never be confused with a modelling error.

Scenarios:

- **mixed-stereo-16**: a six-term declaration-ordered chain
  `2, -1, 17, 3, -3, 18`; decoding runs `18, -3, 3, 17, -1, 2`, so declaration
  and execution order differ. Three blocks.
- **mono-24**: `1, 8, 18, 3`, with delayed positive terms that stress the
  end-of-call history normalization. Two blocks.
- **negative-stereo-24**: `-3, -1, -2, -3`, weights near both clamp ends. Four
  blocks.
- **seek-interleave**: decode, seek to a mid-file sample, reread; and two
  decoder instances advancing at different chunk sizes.

Each scenario is decoded whole-block and at 1, 7 and 17 frames, and every
returned value is compared against the same model sequence. Cross-block reads
are covered because the chunk boundaries straddle block edges.

A final **compatibility control** encodes a real stereo 24-bit stream with the
real library encoder and decodes it through the public API. It is a sanity
check only and is explicitly not a substitute for the planned blocks.

## Diagnostics

On the first deviation the tests print the block, pass, channel, sample
position and state field, for example:

```
first deviation: case=negative-stereo-24 block=1 pass=3 ch=0 sample=63 field=weight_A got=1027 want=1024
```

`field` is one of `pcm`, `residual`, `weight_A`, `weight_B`, `history_A` or
`history_B`.

## The check script

`bash task_support/check_decorrelation.sh`:

1. Builds an observation-enabled static `libwavpack` at `-O0` and again at
   `-O2`.
2. For each, compiles and runs `decorr_reference` and `decorr_blocks`. Both must
   pass, and the block test prints the comparison count for each term and for
   the multi-stage chains.
3. Mutation checks, each applied to a fresh temporary copy of `src/` and
   compiled directly with the C compiler:
   - **minus3**: in term `-3`, use this tick's opposite channel instead of the
     saved previous tick's. Must be rejected.
   - **noclamp**: remove the negative-term weight clamp. Must be rejected.

   Rejection must come from a numerical state assertion (`weight_*`,
   `history_*` or `residual`), not from a compile failure, a crash, or a bare
   CRC error. The script fails if the mutant is not rejected this way.

The prediction and update arithmetic is checked against a direct rational floor
and explicit sign rules, including sources wider than 16 bits and both clamp
ends. The model reports any intermediate outside `int32`, and every scenario
fails if that happens, so no expected value depends on overflow.

## Limits

- Only lossless 16/24-bit integer PCM in mono and stereo is modelled.
- The observation hooks are compiled only into the test build; a normal
  library has no callbacks and no additional state.
- The mutation checks compile the library sources directly (not via CMake) to
  keep the temporary tree small; they use the same sources otherwise.
