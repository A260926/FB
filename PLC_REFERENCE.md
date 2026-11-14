# Narrowband PLC regression reference

This document describes the float-build regression for Speex narrowband packet
loss concealment (PLC). It explains where the reference state comes from, how
the model advances its own sample clock, and why the error limits are what they
are.

Scope: **float build only** (`-DFLOATING_POINT`), narrowband mode, no DTX, no
wideband, no fixed-point. Production algorithms and public interfaces are
unchanged; the regression only adds `tests/plc_model.c`,
`tests/plc_contract.c`, and `tests/check_plc_mutations.sh`.

## How the regression is run

```
bash task_support/run.sh target
```

`run.sh target` rebuilds the production library with the normal build flags
(`-std=c99 -O1 -fno-fast-math -ffp-contract=off -DFLOATING_POINT -DVAR_ARRAYS`),
then compiles `task_support/driver.c`, `tests/plc_contract.c`, and
`tests/plc_model.c` against it. The target runner executes every registered
case and also runs the normal baseline.

`tests/plc_contract.c` includes `task_support/check.h` and registers the case
table. Each registered entry wraps its model case in `TASK_CHECK`, so a
failing case returns 1 through the shared contract as well. `tests/plc_model.c`
holds the independent model, the detailed `TASK_CHECK` assertions, and the
cases. The contract file intentionally has no `main`; `task_support/driver.c`
provides it.

The mutation self-test is:

```
bash tests/check_plc_mutations.sh
```

It works on a temporary copy of the sources, never the original tree.

## Test scenarios

Speech packets are produced through the public encoder API
(`speex_encoder_init` / `speex_encode_int`), at quality 5, VBR off, DTX off.
The input is low-amplitude and changes with sample position, so the normally
decoded excitation history actually varies from frame to frame.

Each case first decodes a run of normal packets to build non-zero history. It
then requests lost frames through the public `speex_decode(dec, NULL, pcm)`
path.

| Group       | Case                        | History | Normal frames | Losses | Recovery | Tail check |
|-------------|-----------------------------|---------|---------------|--------|----------|------------|
| Excitation  | `single_loss_excitation_A`  | A       | 9             | 1      | yes      | no         |
| Excitation  | `twelve_loss_excitation_B`  | B       | 13            | 12     | yes      | no         |
| Synthesis   | `single_loss_synthesis_A`   | A       | 9             | 1      | yes      | no         |
| Synthesis   | `twelve_loss_synthesis_B`   | B       | 13            | 12     | yes      | yes        |
| Recovery    | `single_loss_recovery_A`    | A       | 9             | 1      | yes      | no         |
| Recovery    | `twelve_loss_recovery_B`    | B       | 13            | 12     | yes      | yes        |

History A and history B are two different fixed low-amplitude waveforms, with
different pitch tracks (about 90 and 64 samples in the decoded pitch history).

Scenarios therefore cover:

* one isolated lost packet, and twelve consecutive lost packets;
* both starting from two distinct normal-speech histories;
* the excitation energy, pitch-jitter, per-sample noise, median/memory gain
  evolution, attenuation ramp, and synthesis/high-pass memory;
* the period after the attenuation table is exhausted. In the twelve-loss
  cases the tail is still compared against the model, and the test asserts it
  is not declared silent: on the first frame with a zero attenuation factor
  (loss index 10) the synthesis/high-pass tail still carries energy even
  though the injected excitation has collapsed. The last loss (index 11) is
  then compared normally as the tail decays further.

Every case ends with two normal packets. The production decoder continues with
the same loss schedule, while a second, independent decoder replays the exact
same normal prefix and `NULL`-bits loss schedule. PCM from the two decoders is
compared, together with the RMS of the synthesis and high-pass memories and
the `count_lost`/`seed` state. This checks recovery, not just the concealed
frames.

## Where the initial state comes from

The reference model does **not** read the production state every frame. It
takes one snapshot of `DecState` immediately before the first lost frame, and
from then on advances its own time axis.

The snapshot contains:

* `exc[M_FRAME]` — the last normally decoded excitation frame (`st->exc`).
  This is frame `-1` on the model axis;
* `interp_qlpc[M_ORDER]` — the current LPC coefficients;
* `mem_sp[M_ORDER]`, `mem_hp[2]` — synthesis and high-pass filter memories;
* `pitch_gain_buf[3]`, `pitch_gain_buf_idx`;
* `last_pitch`, `last_pitch_gain`;
* `seed`, `count_lost`.

After this point `compare_frame()` only compares model state against the
production state; it never copies production values back into the model.

Frame `-1` is checked to carry usable energy (RMS of the old excitation above
1, while the input is low-amplitude). Before every lost frame the model
independently measures the RMS of the frame the decoder is about to use as
old energy (`old_excitation_rms`) and compares it with the production
excitation buffer, so the old-energy path is checked directly rather than
inferred from the generated samples.

## Time axis

The model keeps an absolute excitation buffer:

```
[ ... history ... | frame -1 | frame 0 | frame 1 | ... ]
                  ^          ^
                  M_HIST     M_HIST + k*M_FRAME
```

* `M_HIST = 160`. The last good frame occupies
  `[M_HIST - M_FRAME, M_HIST)`.
* Lost frame `k` is generated into
  `[M_HIST + k*M_FRAME, M_HIST + (k+1)*M_FRAME)`.
* The buffer is longer than the generated frames so pitch lookback and the
  one-subframe synthesis delay always have valid samples.

Generation is strictly sample-by-sample in increasing order. New samples
written earlier in the **same** frame are valid pitch history for later samples
of that frame; `intra_frame_pitch_history` records that at least one lost
frame actually read such a fresh sample. A mutation that only repeats the
previous period (`exc[i] = exc[i - pitch]` with no gain and no noise) fails
immediately at the first excitation sample.

Per lost frame the model mirrors the production float arithmetic:

1. `fact = attenuation[count_lost]` for `count_lost < 10`, else `0`.
2. `gain_med = median3(pitch_gain_buf)`; `last_pitch_gain = min(last_pitch_gain,
   gain_med)`.
3. `pitch_gain = min(last_pitch_gain, .85) * fact + 1e-15`.
4. `innov_gain = rms(previous frame excitation)`, measured **before** this
   frame is generated (old excitation energy).
5. `noise_gain = innov_gain * fact * (1 - pitch_gain^2)`.
6. Pitch jitter: `pitch_val = last_pitch + (int)rand(1 + count_lost)`, clamped
   to `[17, 144]`. This draws exactly one random number.
7. Each sample: `exc[i] = pitch_gain * (exc[i - pitch_val] + 1e-15) +
   rand(noise_gain)`. One random draw per sample.
8. In-place `bw_lpc(0.98, lpc)`.
9. Synthesis: `iir_mem16` over 160 samples starting at the current excitation
   **one subframe back**, i.e. at `M_HIST + k*M_FRAME - 40`, followed by the
   narrowband output high-pass (`filtID 1`).
10. `count_lost++`; the attenuated `pitch_gain` is written into the pitch-gain
    ring; index rolls over at 3.

Step 9 is why the model maintains the delayed synthesis history on its own
timeline. The pre-frame region `[M_HIST - M_SUBFRAME, M_HIST)` is available
for the first lost frame, and later frames read the previous lost frame's
tail.

For the first loss of scenario A the state is `count_lost = 0`, so `fact = 1`
and the ring median limits the gain; the seed starts at 1000 and is consumed by
the pitch jitter and 160 noise draws. For scenario B the ring enters at index 1
with different values, so both the median and the ring write-back order differ.
This is what distinguishes the two histories beyond the excitation samples.

## Independent primitives

The model calls **none** of the production PLC helpers:

* random draw (`speex_rand`) — reimplemented from the float LCG
  (`seed = 1664525*seed + 1013904223`, mantissa mapped to `ran.f - 1.5`,
  result `3.4642 * std * ran.f`, with the multiply done in double as in the
  production expression);
* RMS (`compute_rms16`) — reimplemented as `sqrt(.1 + sum(x^2)/len)`;
* bandwidth expansion (`bw_lpc`) — reimplemented as successive multiplication by
  gamma;
* IIR synthesis (`iir_mem16`) — reimplemented with the same memory shift;
* high-pass (`highpass`, narrowband output) — reimplemented from the table.

Production arithmetic macros are not used. The only shared data are the
read-only coefficient tables: the float attenuation table and the narrowband
output high-pass numerator/denominator coefficients. Both are copied into the
model as constants.

## Error limits

The limits are separated because excitation/LPC are signal-domain values while
PCM and filter memories are output/filter-domain values.

| Quantity | Limit |
|----------|-------|
| Excitation, LPC, `last_pitch_gain`, pitch-gain ring values, old-excitation RMS | `1e-4 + 2e-5 * |expected|` |
| PCM, `mem_sp`, `mem_hp` | `0.03 + 3e-4 * |expected|` |
| Seed, `count_lost`, `pitch_gain_buf_idx`, `last_pitch` | exact |

Rationale:

* Excitation is the low-level internal signal (order 1 float units in these
  scenarios). A relative-plus-absolute band of `1e-4 + 2e-5` scales tolerates
  harmless reordering of float operations in the model while catching any
  algorithmic change (the pitch-repeat mutation deviates by ~3.2 units at the
  first sample).
* PCM and filter memories are post-synthesis values that can be an order of
  magnitude larger, and pass through a recursive filter. `0.03 + 3e-4` keeps
  the same relative tightness while allowing for accumulator round-off.
* Seeds, counters, the ring index, and the pitch estimate are discrete state.
  They must match exactly — a single extra or missing random draw changes them,
  and that is a real PLC defect, not numerical noise.

The mutation self-test confirms sensitivity: replacing the excitation
prediction with a bare repeat is caught at `excitation[0]`, and writing the
unattenuated gain into the ring is caught at `loss=1` in
`twelve_loss_excitation_B` (`pitch_gain_buf`).

## Failure reporting

Every comparison reports on the first mismatch only, in this form:

```
CONTENT_MISMATCH group=<group> case=<case> segment=<segment> loss=<n> \
  field=<name>[<index>] expected=<...> actual=<...> absdiff=<...> tol=<...>
```

* `group` / `case` identify the registered case;
* `segment` identifies the history/loss scenario (`A_single_loss` /
  `B_twelve_loss`);
* `loss` is the zero-based lost-frame number;
* `field` names the first differing sample or state field, with its index when
  it is an array.

`tests/check_plc_mutations.sh` relies on the exit code and this line. A
mutation is only considered detected when the mutated tree compiles, the runner
returns **4**, a `CONTENT_MISMATCH` line is present, and the target did not
report `TARGET_COMPLETE`. Build failures, crashes, or an empty case list are
not successes.
