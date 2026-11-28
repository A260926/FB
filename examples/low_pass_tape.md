# Dsp::LowPassTape

Offline, multichannel, time-varying RBJ low-pass cascade with a reverse
(adjoint) pass, intended for automatic equalisation where the equaliser has to
be auditioned repeatedly and per-sample error signals must be sent back to the
cutoff/Q control points.

Public header: `shared/DSPFilters/include/DspFilters/LowPassTape.h`
Production source: `shared/DSPFilters/source/LowPassTape.cpp`

## Signal path

Each channel is filtered by `numStages` (1..4) cascaded RBJ low-pass biquads in
Direct Form II. The forward pass calls `Dsp::RBJ::LowPass::setup()` for the
coefficients and `Dsp::DirectFormII::process1()` for the recursion, so the sound
is the production DSPFilters one. All channels share one control track per
stage.

`Dsp::DirectFormII` gained `getV1()/getV2()/setV1V2()` so the tape can
checkpoint and restore the existing state path; no second filter is written.

## Events

```
struct Dsp::LowPassTape::Event {
  std::uint64_t frame;      // absolute output frame
  std::size_t   stage;      // 0 .. numStages-1
  double        cutoff;     // Hz
  double        q;
  std::uint32_t transition; // ramp length in output frames
};
```

* Events are sorted by nondecreasing frame, at most one per (frame, stage).
* Every stage has exactly one event at frame 0; it applies immediately and its
  `transition` is ignored.
* A later event ramps from the value the stage actually reached on the previous
  frame. A `T > 0` ramp steps once before filtering the event frame: frame `f`
  gets `start + (target-start)*1/T`, frame `f+T-1` is exactly the target.
  `T == 0` switches immediately.
* A new event cancels an unfinished ramp. A target never reached still affects
  the samples already written and the starting point of the next event.

## API

```cpp
Dsp::LowPassTape tape (sampleRate, numChannels, numStages, events, checkpointInterval);

// record + filter one block (input/output are arrays of channel pointers)
tape.push (input, numFrames, output);

tape.seal ();                        // finish recording (idempotent)

auto g = tape.backward (outputAdjoint);
// g.input[c][n]      d(loss)/d(input[c][n])
// g.cutoff[i], g.q[i] d(loss)/d(event[i].cutoff / .q)
// loss = sum_c,n outputAdjoint[c][n] * output[c][n]
```

* `backward()` may be called repeatedly with different error signals; it never
  mutates the recorded audio.
* The gradient includes every channel, every later frame, and the ramp history
  (including abandoned ramps); it is not just the instantaneous coefficient
  derivative.
* Zero-frame `push` touches no pointers and does not advance the clock.
* `push` after `seal` throws. Invalid input/adjoint leaves already written audio
  and previously returned gradients untouched.

## Limits and replay state

Supported: sample rate 44100/48000/96000, channels 1/2/6, stages 1..4,
cutoff in [200, 0.2*sampleRate], Q in [0.5, 2], |input| <= 2, K >= 1.
Non-finite parameters, malformed events and K = 0 throw.

The input is saved for replay. Checkpoints are taken every K frames; during a
backward pass one block of per-frame `v[n]` activations is materialised, so
retained state is `O((N/K + K) * channels * stages)` doubles. The real peak is
reported by `tape.peakSavedStateCount()`. Each backward pass replays exactly N
production-filter frames (`lastReplayFrames()`), within the 2N bound.

## Example

`examples/low_pass_tape_demo.cpp` is a complete runnable program (6 channels,
4 stages, ramps, repeated backward):

```
c++ -std=c++17 -O2 -ffp-contract=off -I. -Ishared/DSPFilters/include \
    shared/DSPFilters/source/*.cpp examples/low_pass_tape_demo.cpp \
    -o low_pass_tape_demo
./low_pass_tape_demo
```

## Tests

`tests/target.cpp` registers Forward, Gradients and Replay groups against the
`task_support` driver. Run `bash task_support/run.sh target`.

* Forward compares the public entry point against an independent per-sample
  production RBJ / DirectFormII chain (including 1/7/127-frame chunking).
* Gradients central-differences a separately hand-coded difference equation
  (not the library types) and covers shared six channels, second-stage
  feedback, an interrupted ramp, a zero-length transition and a tail target.
* Replay covers multiple K values, chunking, two different backward passes,
  empty segments, the replay-frame and peak-state bounds, and reuse after
  rejected operations.

No finite differences, full re-runs per parameter, or third-party
autodiff are used by the implementation; only the library and C++17
standard library are required.
