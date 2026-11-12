# ALAC LPC Analysis Mode

`ALACEncoder::SetLPCAnalysis(bool)` adds a per-packet linear-prediction analysis
mode to Apple's ALAC encoder. It estimates the initial predictor coefficients
from the **actual samples of the current packet** instead of starting every
packet from the fixed `init_coefs` warm-up values, then hands those coefficients
to the existing adaptive predictor and entropy coder.

Nothing changes on the decode side and nothing changes in the file format: the
coeffients travel in the existing per-segment header (the same `numU`/`numV`
field, the same `int16` precision), so any ALAC decoder reads the output.

## Usage

```cpp
ALACEncoder encoder;
encoder.SetFrameSize( 4096 );
encoder.SetLPCAnalysis( true );     // must be called before InitializeEncoder()
encoder.InitializeEncoder( outputFormat );
```

`SetLPCAnalysis` defaults to `false`. With `false` the encoder is byte-for-byte
identical to the original implementation, including the fast-mode selection.

## What it applies to

| Property | Supported |
| --- | --- |
| Bit depth | 16-bit and 24-bit integer PCM |
| Channels | mono (1) and stereo (2) |
| Other depths | 20/32-bit input is left on the original path |
| Other channel counts | multi-channel input is left on the original path (each element uses the original encoder) |

For stereo, the existing matrix (middle-side) resolution search runs first and
the result is kept. The analysis is then performed separately on the U and V
signals that were actually produced by `mix16()`/`mix24()` — that is, the
matrixed data, not the raw left/right channels. For 24-bit input the existing
shift-off path still applies: one low byte per sample is extracted first and
carried verbatim in the header, and the predictor is fitted on the shifted
U/V samples.

Candidate predictor orders are 4, 8, 12 and 16. Each one is measured with the
real adaptive Golomb coder, and the smallest result wins; the original
coefficient set is always kept as a candidate, so the analysis pass can never
make a packet larger than the original path produces from the same starting
state.

## Interaction with fast mode

`SetLPCAnalysis(true)` takes priority over `SetFastMode(true)`: analysis packets
use the same per-packet analysis path as normal mode, and the result matches
what `SetLPCAnalysis(true) + SetFastMode(false)` produces from an equal state.
When analysis is off, `SetFastMode` keeps its original meaning and output.

## Cost

Per packet, per channel:

* 4 least-squares fits (orders 4, 8, 12, 16) on the packet's samples, using 64-bit
  integer accumulators and double-precision elimination;
* up to 25 candidate pair evaluations for stereo (4 fitted orders × 4 fitted
  orders plus original fallbacks), 5 for mono, each running the full adaptive
  predictor and the adaptive Golomb coder;
* one re-run of the winning candidate to leave the adaptive state consistent
  with the frame that was written;
* one extra packet-sized buffer (allocated only when the mode is enabled) to
  evaluate candidates and compare the final packet against the original path.

This is substantially more CPU-intensive than normal mode — it is intended for
offline archiving, not real-time capture. Encoding is deterministic: the same
input and state produce the same output.

## Robustness

* Silence, constant signals, very low energy, rank-deficient inputs (e.g. two
  identical channels) and short packets are handled: a fit that would need
  missing history, or whose normal equations are degenerate, is rejected and
  the original coefficient set is used.
* Coefficients are always finite, quantized `int16` values. A global scale
  reduction keeps the predictor's `int32` accumulations in range, including
  near-full-scale input.
* A partial final packet uses the actual packet length for all comparisons;
  the transmitted partial-frame length, coefficients and shift-off bytes are
  included in the size decision.
* Escape handling is unchanged: if the chosen compressed packet is still not
  smaller than an escape packet, the encoder writes an escape packet.
