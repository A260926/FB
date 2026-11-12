# Real-time gain-control history

This note documents the rework of the DynamicAudioNormalizerAPI gain-control
chain that runs inside the audio callback. It records the measurement values
asked for by the task: comparison counts, the maximum output difference against
the pre-optimization engine, and the allocation counts per processing stage.

All numbers in this file were produced by `bash checks/target.sh`
(macOS/arm64, Apple clang, `-O1`), which builds the API core sources listed in
`checks/build.sh` with `DAN_TEST_INSTRUMENT` and runs the three check groups
(`equivalence`, `window`, `allocation`) plus `checks/baseline.sh`.

## What changed

The three gain histories that used to be `std::deque<double>` /
`std::queue<double>` per channel are now fixed-capacity ring buffers backed by
memory that `initialize()` allocates once:

| history | old container | new container | capacity (per channel) |
| --- | --- | --- | --- |
| `original` | `std::deque` | `DoubleRing` | `filterSize` |
| `minimum` | `std::deque` | `DoubleRing` | `filterSize` |
| `smoothed` | `std::deque` | `DoubleRing` | `3*filterSize + 64` |
| minimum window | `std::min_element` over the whole deque | `MinSlidingWindow` (monotone deque, absolute positions) | `filterSize + filterSize/2 + 1` |
| log triplets (only with logging) | `std::queue` | `DoubleRing` | `3*filterSize + 64` |

The Gaussian filter keeps the original weights and the original summation order:
one pass over the window, weight `0` to `filterSize-1`, in that order. It is
still linear per frame; nothing is batched or skipped.

`initialize()` is the only place that allocates on the normal processing path.
No background thread is involved. On destruction the allocations are released
naturally. With no log file and no user log callback, the three logging rings
are not even created; `writeLogFile()` is a no-op and the analysis instants
(and so every recorded triplet) are unchanged when logging is enabled.

## Sliding-window minimum

The old code rescanned all `filterSize` entries for every produced minimum
(`O(filterSize)` per frame). The new implementation keeps a monotone deque of
candidates:

* every incoming gain value is compared with the back of the deque and either
  pops dominated candidates or is appended (amortized `O(1)`);
* the outgoing element is dropped by absolute position, never by scanning;
* the first window (which includes the prefill values) is primed once, and the
  partial minimum history that the original code builds before the first full
  window is reproduced from that same window.

Comparisons are counted where they actually happen, inside the engine, and are
only compiled into test builds (`DAN_TEST_INSTRUMENT`). Nothing is derived from
the number of output frames. `windowOps` is the subset of the count spent in
the minimum stage.

Measured comparison counts (one-channel, default boundary mode, single
`process()` call then `flushBuffer()`; `M` is the number of gain advances the
engine reports, including the synthetic flush frames):

| input samples | filterSize | M | comparisons | bound 4M + O(filterSize) |
| --- | --- | --- | --- | --- |
| 8000 | 3 | 3 | 5 | 15 |
| 8000 | 31 | 31 | 61 | 155 |
| 8000 | 301 | 301 | 601 | 1505 |
| 40000 | 3 | 8 | 11 | 35 |
| 40000 | 31 | 35 | 69 | 171 |
| 40000 | 301 | 305 | 609 | 1521 |
| 120000 | 3 | 18 | 26 | 75 |
| 120000 | 31 | 45 | 89 | 211 |
| 120000 | 301 | 315 | 629 | 1561 |

The `checks/target.cpp` window group asserts, per channel independently, that
`compares <= 4*M + O(filterSize)` for repeated-minimum, monotone-rise,
noise/wrap-around and reset scenarios (1 and 2 channels, `filterSize` 3/31/301).
Note that the constant is proportional to `filterSize` only once (the first
window scan); it is not multiplied by the number of frames.

## Sample compatibility

The `equivalence` group compares the optimized engine sample by sample against
`checks/legacy_engine.cpp`, a verbatim copy of the pre-optimization engine from
commit `c152491` (kept only for tests, original deque/queue code untouched).
The tolerance is `1e-12`; in all covered configurations the observed maximum
difference is exactly `0`.

Matrix covered by `equivalence/configMatrix`:

* channel counts 1, 2, 6;
* `filterSize` 3, 31, 301;
* input lengths 150 (shorter than one frame), 800 (about two frames) and
  8000/40000 (many frames);
* signals generated in C++: silence, dual tone, amplitude steps, repeated
  plateaus, impulses near the start and the end, drifting noise, white noise;
* coupled and independent channels;
* default and `altBoundaryMode`;
* DC correction off/on and compression factor 0/2.5, combined;
* whole-block input, alternating 1/7/113-sample chunks, and inplace processing;
* several different flush output capacities.

Additional cases: the C entry point vs the C++ entry point on real audio,
inplace vs non-inplace, a stream split in different ways (same total length and
same samples at the same positions), the post-flush "must reset first" contract,
and a byte-for-byte comparison of the data lines of a log file produced by the
new and the old engine.

Maximum observed difference: `0` (reported by `checks/target.sh` as the
equivalence group passing).

Boundary behaviour is preserved on purpose: the original-gain prefill, the
stepwise minimum while the first minimum history forms, the Gaussian warm-up,
and the per-sample fade from the previous frame to the current one all remain
exactly as in the old engine. The old flush quirk (synthetic fill values
depend on the flush capacity when DC correction is enabled) is kept as-is; no
cross-capacity equality is asserted for those tails.

## Allocation counts

`allocation` installs counting replacements for `operator new/delete`,
`operator new[]/delete[]`, `malloc/calloc/realloc/free` and counts from the
moment `initialize()` returned - no warm-up run before counting. The
counter's own inputs, expected values and report buffers are created before
counting starts. On macOS this used the malloc zone entry points behind the
same replacements (see the note below).

Results on the no-log path (`checks/target.sh`, allocation group):

| stage | allocations | frees |
| --- | --- | --- |
| initialize -> process (long block stream) | 0 | 0 |
| flushBuffer | 0 | 0 |
| reset | 0 | 0 |
| processInplace after reset | 0 | 0 |
| many ring wrap-arounds, flush, reset, second stream | 0 each | 0 each |
| public C++ API (6 ch, fs 301, 0.25 s frames) process/flush/reset | 0 | 0 |
| with a user log callback (logging rings active) | 0 | 0 |

`checks/baseline.sh` reports `api_allocs=0 api_frees=0` for the same
two-channel stream that previously reported `api_allocs=24` before the rework
(the 24 came from rolling deque/queue node allocation); energy and total
length are unchanged (`energy=32511.1`, `produced=102400`).

Space is bounded by channels, frame length and filter size: the per-channel
gain rings total `3*filterSize + 64` doubles plus `filterSize + filterSize/2 + 1`
minimum-window slots and positions, independent of the stream length. No
fixed-length "recording" buffer is reserved.

### A note on the counting on macOS

`baseline.cpp` counts via `operator new`, which catches the C++ allocations.
The `allocation` group additionally replaces the C allocation entry points.
On macOS the replacements route through `malloc_zone_*` of the default zone so
that they intercept the process's real allocations (a plain forwarding
`malloc()` symbol does not intercept calls already bound to libSystem on this
platform). On other platforms plain `malloc/realloc/free` forwarding is used.
Since counting is global, the checks never allocate unrelated memory while the
flag is set: all test containers are built beforehand and measured calls only
receive raw pointers.

## Files

* `DynamicAudioNormalizerAPI/src/DynamicAudioNormalizer.cpp` - ring storage,
  monotone minimum, test instrumentation hooks.
* `DynamicAudioNormalizerAPI/src/GaussianFilter.h|cpp` - weight accessor.
* `DynamicAudioNormalizerAPI/src/Logging.h|cpp` - `hasLoggingHandler()`.
* `checks/legacy_engine.h|cpp` - pre-optimization reference (tests only).
* `checks/target.cpp` - `equivalence`, `window`, `allocation` groups.
* `checks/target.sh` (and the `EXTRA_CXX_FLAGS` support in `checks/build.sh`).
* `examples/dynaudnorm_realtime.cpp` - initialization and callback lifetime example.

## Running

```sh
bash checks/baseline.sh   # prints api_allocs / api_frees for a real stream
bash checks/target.sh     # builds and runs the three groups, then baseline
```
