# Dsp::LowPassTape

`Dsp::LowPassTape` (public header `DspFilters/LowPassTape.h`, implementation
`shared/DSPFilters/source/LowPassTape.cpp`) is an offline, recordable and
replayable cascade of RBJ low pass biquads for automatic equalisation. It is
designed so that the rendered sound is bit-for-bit the production
`Dsp::RBJ::LowPass` + `Dsp::DirectFormII` sound, while analytic first order
derivatives are available for a downstream optimiser.

It lives beside the existing API: `SimpleFilter`, `SmoothedFilterDesign` and
the default sound are untouched.

## Quick start

```cpp
#include "DspFilters/LowPassTape.h"

std::vector<Dsp::LowPassTapeEvent> events = {
  //  frame  stage  cutoff   q     transition
  {       0,     0,   1200,  0.8,   0 },
  {       0,     1,   4000,  1.1,   0 },
  {     200,     0,   3000,  1.5,  48 },   // 48 frame linear ramp
};

Dsp::LowPassTape tape (48000,   // sample rate
                       2,      // channels
                       2,      // stages
                       64,     // checkpoint interval K, frames
                       events);

// record + render in blocks
for (... each block ...)
  tape.push (inputChannels, blockFrames, outputChannels);

tape.seal ();

// differentiate  sum_c sum_n adjoint[c][n] * output[c][n]
Dsp::LowPassTape::Gradients g = tape.backward (adjointChannels);

g.input[v]                 // per-channel-per-frame input gradient
g.eventCutoff[e]           // d/dcutoff of event e
g.eventQ[e]                // d/dQ of event e
g.replayFrames             // production filter frames replayed
tape.checkpointStateCount ()
tape.peakLocalActivations ()
```

A runnable example is in `examples/LowPassTapeDemo.cpp`:

```sh
c++ -std=c++17 -O2 -I. -Ishared/DSPFilters/include \
    shared/DSPFilters/source/*.cpp examples/LowPassTapeDemo.cpp -o lowpass_tape_demo
./lowpass_tape_demo
```

## Events and timing

An event is `{frame, stage, cutoff, q, transition}`.

* Events are ordered by frame. Within one frame and stage at most one event is
  allowed. Every stage must have exactly one event at frame 0, which takes
  effect immediately (its `transition` is ignored).
* A later event at frame `f` starts from the value that stage actually used on
  frame `f-1`. With `transition = T > 0` the value advances in `T` equal steps;
  the first step happens *before* frame `f` is filtered, so the value reaches
  the target exactly on frame `f+T-1`. `T = 0` switches immediately.
* A new event cancels the remainder of an older ramp. The partially traversed
  history is still part of the sound and is differentiated correctly. A ramp
  that would run past the end of the recording is truncated; its target is
  still a differentiable parameter.
* Denominator, cutoff and Q interpolation is linear in the *parameters*
  (`cutoff`, `q`); RBJ coefficients are recomputed from the interpolated
  parameters on every frame, exactly like `SmoothedFilterDesign`.

## Accepted range

Sample rates 44100 / 48000 / 96000 Hz. Cutoff in `[200, 0.2*sampleRate]`, Q in
`[0.5, 2]`. 1, 2 or 6 channels; 1 to 4 stages. Input samples must be finite and
`|x| <= 2`. Non-finite parameters, malformed event lists, out-of-range values
and `K == 0` are rejected with `std::invalid_argument`.

`push` with zero frames does not dereference either pointer and does not
advance the clock. A rejected `push` throws before writing output, appending
input or advancing the clock, so earlier audio and gradients are unchanged.
After `seal()` a further `push` throws `std::logic_error`. `seal()` is
idempotent.

## Derivatives

`backward()` reports the exact first order derivative of the scalar loss above.
It accumulates the recursive feedback through all stages and all channels, and
the sensitivity of every frame's coefficients with respect to the *event
parameters* — not merely the coefficient derivative at the current frame. The
derivative of RBJ's `w0`, `sin`, `cos` and `1/a0` normalisation is analytic.
Denormals are handled with the production alternating `anti_denormal_vsa`
bias; the bias is treated as a constant, as required.

`backward()` may be called repeatedly, including with a different adjoint each
time; input, events and checkpoints are immutable during it.

## Memory and replay

The full input is retained for replay. Filter activations are *not* retained
per frame. At every `K` frames a checkpoint of the production
`DirectFormII` state is captured; a backward pass replays one interval at a
time from its checkpoint, accumulating parameter adjoints across intervals.
The saved checkpoint state plus local activations is

```
O((N/K + K) * channels * stages)
```

and `checkpointStateCount()`, `peakLocalActivations()` and
`peakSavedStateCount()` report the real peak counts (no estimate). Each
`backward()` replays exactly `N` production filter frames (at most `2N`), as
reported by `lastReplayFrameCount()` / `peakReplayFrameCount()`.

The forward pass reuses `Dsp::RBJ::LowPass::setup` and
`Dsp::DirectFormII::process1` through the normal `BiquadBase::State<DirectFormII>`
route, so there is no second sound-identical filter alongside the production
one. `DirectFormII` gained read-only accessors (`getState1`, `getState2`,
`setState`) for checkpointing; processing behaviour is unchanged.

## Tests

`tests/target.cpp` registers the `Forward`, `Gradients` and `Replay` groups and
is driven by `task_support/driver.c`:

```sh
bash task_support/run.sh target
```

Forward compares the new entry point against a per-sample reference built from
the existing `RBJ::LowPass` / `DirectFormII` chain, including 1/7/127 frame
block splits and in-place operation. Gradients compares the analytic input and
event derivatives against central differences of an independently written
difference equation (6 channels, second stage feedback, a target changed before
its ramp completes, zero-length transitions and a tail target never reached),
to `2e-7 + 2e-5*|reference|`. Replay covers several `K`, block splits, two
different backward passes, empty segments, replay frame counts, peak state
counts and reuse after failures.
