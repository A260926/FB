# CooperativeFFTConvolver — scheduling and work accounting

`fftconvolver::CooperativeFFTConvolver` produces the same causal convolution as
the two-stage convolver,

```
y[n] = sum_k h[k] * x[n-k]
```

from a zero history, without ever reading future input, but runs entirely on
the caller's single audio thread. The long-tail FFT work that
`TwoStageFFTConvolver` performs in one burst when a tail block fills up is
instead split into resumable pieces and spread over the following small audio
blocks. No worker thread, lock, wait or extra block of latency is introduced,
and no heap memory is allocated or released inside `process()`.

## Geometry

- `B = headBlockSize`, `T = tailBlockSize`, `N = 2*T`, `Q = T/B`.
- The first `2*T` impulse-response coefficients are handled by the zero-latency
  **head path**: a block-`B` partitioned convolution with a `2B` FFT. The FFT of
  the partially filled input block is recomputed on every call with the
  unfilled samples zeroed, so output is strictly causal and has no block delay.
- The remaining coefficients are split into
  `P = ceil(max(irLen - 2*T, 0) / T)` partitions of length `T`, each
  transformed with an `N = 2T` FFT (the **tail path**).

## When data becomes available

Every `T` captured input samples produce one tail **job**. Job `j` consumes the
input block `x[jT .. (j+1)T)`.

- The block is fully captured at absolute sample `(j+1)T`, i.e. at the end of
  quantum `(j+1)*Q - 1` (a quantum is a consecutive run of `B` input samples
  counted from `init()`). A job is never started before that quantum has really
  completed, even when one `process()` call delivers several quanta at once.
- Because the tail IR begins at offset `2T`, job `j`'s **earliest acoustic
  contribution is at sample `jT + 2T`** — the start of its output window.
- Therefore exactly `T` samples, or `Q` quanta, lie between capture
  (`(j+1)T`) and the deadline (`jT + 2T = (j+2)T`). The inverse transform,
  frequency multiply-accumulate and overlap-add for the block all finish during
  that interval.

Jobs run strictly one at a time. Job `j-1`'s window opens at `jT + T`, before
job `j` is even captured, so the per-quantum budget (below) guarantees every
block is ready before it is needed; there is no head-of-line blocking. Captured
blocks are copied into a small fixed snapshot ring, so a job always reads the
exact input it was captured with even while newer calls overwrite the capture
accumulator.

Quantum boundaries follow the cumulative `B`-sample count, never the host call
length. A trailing fragment shorter than `B` completes no quantum and performs
no tail work. Feeding the same signal in calls of 1, 7, `B` or `T+3` samples
reaches the same absolute sample positions and produces the same per-quantum
tail-task rhythm.

## Resumable tail pipeline

Each job walks through the following resumable stages:

| Stage             | Work performed |
|-------------------|--------------------------------------------------------------|
| `JOB_COPY`        | pack `T` real samples + `T` zeros into `N` complex slots     |
| `JOB_FORWARD`     | bit reversal + radix-2 forward FFT, butterfly by butterfly   |
| `JOB_FORWARD_STORE` | store the input spectrum in the partition ring, clear the accumulator |
| `JOB_MAC`         | `P*N` complex multiply-accumulates, one frequency bin at a time |
| `JOB_INVERSE_LOAD`| copy the accumulator into the inverse FFT buffer             |
| `JOB_INVERSE`     | bit reversal + radix-2 inverse FFT, butterfly by butterfly   |
| `JOB_SCALE`       | `1/N` scaling of every complex result slot                   |
| `JOB_OVERLAP`     | overlap-add the `N` real samples into the causal output ring |

Both transforms use a dedicated resumable radix-2 kernel (`ResumableFFT`) whose
state is just the current stage and the next butterfly index; it can stop after
any butterfly and resume on the next quantum. Twiddle factors and all other
coefficients are precomputed in `init()`, so processing never calls a
trigonometric function.

## Work units and the per-quantum bound

A tail job costs, in total, no more than `2F + P*N + 12N` units, where
`F = (N/2) * log2(N)`:

- `F` butterflies per FFT, times two FFTs → `2F`,
- `P*N` complex frequency-bin multiply-accumulates,
- the remaining `12N` covers packing, spectrum store/clear, inverse load,
  scaling, overlap-add and the FFT bit-reversal slot moves.

Each quantum may consume at most

```
budget = ceil((2F + P*N + 12N) / Q) + 16
```

tail units. Since `Q * budget >= 2F + P*N + 12N`, one job always completes
within its `Q` available quanta. The implementation enforces the bound
structurally: every counted loop checks the remaining budget before doing the
work, and loops that cost two units (bit-reversal swaps, the combined
store-and-clear) test `used + 2 <= budget` before starting.

Units are counted from the loops that actually execute:

- one radix-2 complex butterfly → one `butterfly` unit;
- one frequency-bin complex multiply-accumulate → one `mac` unit;
- one complex slot moved, cleared or scaled, or one real sample overlap-added
  → one `move` unit (a bit-reversal swap counts the two complex slots it
  actually moves).

Pure index arithmetic, the short head path, and the once-per-call input capture
and final output sample copies are not charged to the tail budget. Every charged
segment is reported through

```cpp
FFTCONVOLVER_TAIL_WORK(quantum, kind, units)
```

with `quantum` starting at 0 and identifying the `B`-sample interval that just
completed, and `kind` equal to `"butterfly"`, `"mac"` or `"move"`. The reported
counts are accumulated from the real loops in both the forward and inverse
kernels — they are never derived from the configuration or from the number of
jobs the scheduler claims to have finished. In a product build that does not
force-include `support/work_trace.h`, the macro is defined as a no-op.

## Output integrity

Head and tail contributions are summed in a fixed causal output ring; each slot
is cleared when read back. A job created at sample `(j+1)T` never writes past
`jT + 4T - 1`, which is less than `3T` ahead of the read position, so no slot is
reused before it has been consumed. The tail history, the in-flight spectrum and
the completed-but-not-yet-played blocks therefore never overwrite one another.
Input is captured before any output is written, which also makes
`input == output` safe.
