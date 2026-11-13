# Independent Decorrelation Certification

This directory and `task_support/check_decorrelation.sh` add an independent
certification path for the Wavpack lossless integer decorrelator.  The work
is deliberately separate from the existing round-trip tests: nothing here
reuses the production decorrelation, weight-update or state-restoration
functions to compute expected values.

## What is certified

* The prediction contribution is `floor((weight * source + 512) / 1024)`
  with explicit floor semantics and 64-bit intermediates.
* Weight updates key on the sign of the prediction **source** and the
  **residual**, never on the reconstructed sample.
* Terms `1..8` are delay terms with natural time-ordered history queues,
  terms `17`/`18` are the two-point extrapolations, and terms `-1`, `-2`,
  `-3` are the stereo cross-channel dependencies:
  * `-1` rebuilds the left first and feeds that same-beat left to the right;
  * `-2` rebuilds the right first and feeds that same-beat right to the left;
  * `-3` predicts each side from the opposite side saved at the **previous**
    beat (the current-beat opposite is never used).
* Negative-term weights are clipped to `±1024`; ordinary positive-term
  weights are allowed to travel beyond that range.
* Every 32-bit field is range-checked from its 64-bit intermediate, so the
  expected sequence never relies on integer overflow.
* Metadata state is recovered through independent implementations of the
  8-bit weight rounding and the signed `log2`/`exp2` history quantization,
  including omitted (zero-filled) weights and oldest-first delay history.
* Planned blocks are decoded only through the public
  `WavpackOpenFileInputEx64()` / `WavpackUnpackSamples()` chain.

## Files

| File | Role |
| --- | --- |
| `tests/decorr_test.h` | Independent natural-time model, manual WavPack block writer, in-memory public-API reader, and the value-by-value comparison bridge. |
| `tests/decorr_reference.c` | Arithmetic edge cases (floor rounding, sign updates, clipping, the `-1/-2/-3` dependencies) plus single-term public-chain blocks for every term in mono and stereo at 16/24-bit. |
| `tests/decorr_blocks.c` | Multi-block mixed-term chains (declared order differs from execution order), whole/1/7/17-frame interleaved reads, cross-block state restoration, public seek, two concurrent instances, and a real-encoder multichannel compatibility stream. |
| `tests/decorr_observe.[ch]` | Read-only observation points. Linked only into the instrumented test build. |
| `task_support/check_decorrelation.sh` | Builds the instrumented library at `-O0` and `-O2`, runs both programs and the mutation suite, then rebuilds the shipping library and runs the baseline. |
| `task_support/mutate_decorr.sh` | Copies the tests to a scratch tree, injects two faults, and requires the numeric assertions to reject both. |

## Bitstream plan to model/block correspondence

1. A `DtBlockPlan` states, per term and in **declared** order: the term id,
   delta, initial per-channel weights and natural-time initial history, plus
   the residual schedule and entropy medians.
2. The independent model reverses that list to obtain the decoder execution
   order and runs the residual schedule beat by beat, producing expected PCM
   and a per-beat snapshot of every weight and natural history queue.
3. The block writer serializes the plan manually:
   * term bytes use `(term + 5) | (delta << 5)`;
   * weights and sample histories use forward declared order so the decoder's
     reversed `decorr_passes` array receives each term's own bytes;
   * delay-term history is oldest-first, two-point history groups a channel's
     two slots, and stereo delay history interleaves `A_m,B_m` per slot;
   * the residual payload is the only section produced by a production
     component (the lossless entropy writer, via `init_words`,
     `write_entropy_vars`, `send_words_lossless` and `flush_word`);
   * the header CRC and the trailing block checksum are computed from the
     model's expected PCM.
4. The public decoder consumes the block; its decoded residuals, per-pass end
   state and normalized term 1-8 history are compared against the model.
   Ring indices are translated only on the read side — the model itself has
   no ring buffer and never rotates its queues.

## Observation points

`src/unpack.c` includes `tests/decorr_observe.h` only when
`DECORR_TEST_OBSERVE` is defined.  The macro is set exclusively by
`check_decorrelation.sh`; the CMake/library build never defines it, so the
shipped library gains no callbacks, no extra state and no changed behavior.
The hooks record:

* the entropy residual buffer immediately after `get_words_lossless`;
* each decorrelation pass's weights and history at the end of the pass;
* the term 1-8 history after the per-return normalization rotation.

## Mutation tests

`mutate_decorr.sh` proves the assertions have teeth:

* **`neg3_current_opposite`** rewrites the term `-3` branch to consume the
  current-beat opposite (the term `-1` behavior).  The `-3` dependency
  assertions and the public-chain PCM comparison must fail.
* **`no_negative_clip`** disables the negative-term weight clip.  The
  clip-boundary assertions must fail as weights escape `±1024`.

A mutation that merely crashes, or that only trips a block CRC without a
value assertion, is treated as a test failure.

## Running

```sh
bash task_support/check_decorrelation.sh          # -O0 and -O2
bash task_support/check_decorrelation.sh -O0      # a single level
bash task_support/baseline.sh                     # the existing round trip
```

The scripts print the actual number of per-term, weight, history, residual
and PCM comparisons performed, and report the first difference with its
block, pass, channel, sample position and state field.
