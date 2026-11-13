# Mapping0 channel-coupling numerical regression

This directory holds a self-contained numerical regression for the
Vorbis I type-0 mapping channel coupling implemented in
`lib/mapping0.c`.

## Files

- `target_cases.c` - the three test groups and the independent model.
  It exposes `int task_cases(const char *group)` and defines no `main`.
- `driver.c` - the existing harness that calls `task_cases()` once per
  group and turns any failed check into exit code 42.
- `check.h` - the `task_check()` prototype shared with the harness.
- `vendor/ogg/` - the fixed, vendored libogg sources used by every
  build; nothing is downloaded.

## How expected values are produced (no frozen decoder output)

No expected number is captured from the decoder under test. Every
expectation comes from an independent model written in
`target_cases.c` from the Vorbis I mapping0 rules.

1. Forward coupling. `model_forward_pair()` applies the spec rule:
   the channel with the larger absolute sample supplies the magnitude
   (ties go to the angle channel) and the angle carries the signed
   difference selected by the dominant sample's sign. Solving that
   piecewise-linear definition for the original pair gives four affine
   sign-quadrant rules, implemented independently in
   `model_inverse_pair()`. Multi-step chains use
   `model_forward_chain()` / `model_inverse_chain()`, which operate on
   arrays of independent channel vectors and apply the steps in
   declared order forward and in reverse order for the inverse. The
   inverse deliberately does not copy the production loop.

2. Inverse MDCT. `model_imdct()` evaluates the cosine basis
   `cos(pi/(2N) (2k+1) (2n + N/2 + 1))` directly for every bin and
   sample. Production `mdct_backward()` is never used to build expected
   PCM. The closed form matches the production transform to float
   round-off.

3. Floor curve. `model_floor_gain()` derives the floor1 dB lookup
   value `exp((ln10/20) (y-255) 140/256)` from the dB definition
   instead of reading the production table.

4. Window and overlap-add. `model_window()` uses the closed form
   `sin(pi/2 sin^2(pi(i+1/2)/N))`; `struct ref_engine` performs the
   same two-buffer overlap-add geometry as the public
   `vorbis_synthesis_blockin()` buffering, including short/long and
   long/short transitions, but as independently written code.

## The three groups

- `quadrants` exercises all four `(m,a)` sign quadrants, positive and
  negative zero, equal-magnitude ties, asymmetric small integers and
  exact binary fractions (multiples of 1/4 through a fractional
  residue codebook). It checks the residue, post-coupling and
  post-floor TRACE snapshots bin by bin, the model round trip, exact
  packet byte lengths from an independent field-width count, and the
  full PCM against the independent IMDCT/window model.
- `chain` uses three channels and two coupling steps that share the
  middle channel, plus two separate submaps. It proves that inverse
  steps must run in reverse declaration order (declaration-order undo
  yields a different map on the same data) and that coefficients from
  different submaps cannot mix.
- `packets` drives only the public API
  (`vorbis_synthesis_headerin`, `vorbis_synthesis`,
  `vorbis_synthesis_blockin`, `vorbis_synthesis_pcmout`,
  `vorbis_synthesis_read`). It contains an all-zero packet, alternating
  short/long blocks, consecutive packets that exercise overlap history,
  a channel whose floor is absent in every packet (coupling still pulls
  it into residue participation, but the floor stage must silence it
  exactly), two interleaved decoder instances that must not leak
  spectral state into each other, and a real production-encoded
  six-channel stream used as a compatibility control.

## Floor presence vs residue participation

The model keeps these two flags distinct, matching mapping0:

- the raw floor presence flag is what `floor1_inverse1()` reads; a
  channel with no floor renders an all-zero spectrum regardless of its
  residue;
- the residue participation flag starts from floor presence and is then
  dirtied by coupling: if either channel of a coupling step
  participates, both do, so a floorless channel can still contribute
  residue bits. `residue_participation()` models this independently.

## Read-only TRACE observation point

`lib/mapping0.c` gains three hooks, each compiled to nothing unless the
translation unit is built with `-DVORBIS_COUPLING_TRACE`:

1. after the residue backends run, before inverse coupling;
2. after inverse coupling, before the floors;
3. after the floors, before the IMDCT.

The hook receives a `const vorbis_block *` and only copies values out.
It never writes through `vb->pcm`, never touches the bitpacker and so
cannot change the number of bits consumed. Normal library builds
contain no trace symbol, callback or allocation (verified with `nm`).

## Bitstream construction and tolerances

All synthetic headers and audio packets are assembled in C with
`oggpack_write()` following the Vorbis I wire formats; values reach the
production inverse coupling only through legal bitstreams decoded by
the real mapping0 backend. Codebooks are complete equal-length trees
whose LSb-first codeword is the bit-reversed entry index.

Per-sample PCM tolerance is `2e-5 (1 + |expected|)`. Packet byte
lengths, output sample counts and silent channels are checked exactly.

## First numerical differences the tests pin down

Before this regression, `test/test.c` only checked an aggregate output
level. The new tests fail, with concrete bin-level numbers, when:

- an inverse quadrant uses the wrong sign for the difference term (the
  `sign` mutant changes `pcmA = m - a` to `m + a`, first visible at the
  `(m>0,a>0)` angle-channel bin where 2 is decoded instead of the
  modeled value);
- the coupling steps are undone in declaration order instead of
  reverse order (the `order` mutant), which only changes output when a
  channel is shared between steps, as in the three-channel chain;
- a floorless coupled channel is not silenced after the floor stage;
- submap residue coefficients bleed across channel groups;
- short/long block overlap geometry or sample accounting drifts.

## Mutation behavior

`task_support/target.sh` rebuilds `lib/mapping0.c` twice with the
patches in `task_support/mutations/`. With either mutant the affected
group prints an `ASSERT ...` line and exits with status 42; the harness
records `MUTANT_CAUGHT`. The test never reads the `TASK_MUTANT`
environment variable, so failure comes purely from the numerical
assertions, not from test branching.
