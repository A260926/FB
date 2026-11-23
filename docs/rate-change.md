# Output rate changes (`ModPlug_ChangeRate`)

`int ModPlug_ChangeRate(ModPlugFile* file, int rate)` switches the output
sampling rate of an already-loaded module **without reloading it and without
restarting playback**.  The change is applied at the first output frame of the
next `ModPlug_Read` call; nothing is re-triggered and no row effect is processed
again.

## Signature and return value

```c
MODPLUG_EXPORT int ModPlug_ChangeRate(ModPlugFile* file, int rate);
```

* Returns `1` on success.
* Returns `0` when
  * `file` is `NULL`,
  * `rate` is not one of `11025`, `22050`, `44100`,
  * the player is not currently configured for 16-bit stereo output, or
  * MegaBass is enabled.

On failure the playback state is left untouched; reads keep working exactly as
before the call.  A call that requests the rate already in effect **succeeds**
and is a byte-for-byte no-op.

## Scope and limitations

This is an intentionally narrow facility for the legacy, single-handle,
serialized playback model that libmodplug already has:

* Only **one module handle** may be loaded at a time and calls must be
  serialized.  The output format and the DSP effects use libmodplug's existing
  global settings; no per-handle session state is introduced.
* Only **16-bit stereo** output is supported.  Requests under 8-bit, 16-bit mono,
  etc. are rejected.
* `NOISE_REDUCTION`, `SURROUND` and `REVERB` may all be enabled.
* **MegaBass must be disabled**; requests with MegaBass enabled are rejected.
* Supported rates: **11025, 22050, 44100 Hz**.
* The module is not reloaded, playback does not restart from the beginning, and
  no whole-song PCM is buffered.

When the new API is not called, decoding is identical to the original
`ModPlug_Read` behaviour.

## What is preserved

The switch happens at the boundary between two `Read` calls, so everything that
is measured in musical time survives unchanged:

* current order / row / tick count, speed and tempo;
* note and effect memory (`nNote`, `nPeriod`, vibrato/tremolo/portamento memory,
  the "old" parameter bytes, etc.);
* envelope positions (volume / panning / pitch), including loop and sustain
  handling;
* each voice's integer and fractional sample position (`nPos`, `nPosLo`) and the
  direction of a ping-pong loop that is currently running backwards;
* the state of the short DSP filters, their accumulators, and the noise
  reduction history.

No tracker tick is processed as part of the change and `ReadNote` is not called,
so no effect can fire twice.

## What is recomputed

These quantities are stored in output samples and are recomputed immediately for
the new rate:

* each voice's mixing step (`nInc`), so a held or looped note keeps the same
  pitch;
* the coefficients of any active channel filter (the filter **delay** values
  `nFilter_Y1..Y4` are preserved, only `A0/B0/B1` are recomputed);
* the remaining duration of the in-flight tick and of any in-flight volume ramp.

### Tick duration and rounding

At the moment of the switch the player may be in the middle of a tick.  The
remaining wall-clock time of that tick is preserved:

1. On the **first** change the remaining time is taken from the number of old-rate
   frames still owed by the current tick.  The remaining frames at the new rate
   are this time **rounded up** to a whole number of frames.
2. On any **subsequent** change the exact remaining time carried over from the
   previous change is used instead of re-deriving it from a rounded frame count.
   This is what prevents rounding drift when several changes are made without
   producing any output in between (e.g. a quick round-trip through another
   rate).
3. Once output starts again, the player subtracts the real elapsed time
   (`frames * 11025 / rate` in the internal time base) from the carried
   remainder.  Because every supported rate divides the internal base exactly,
   splitting the same output into different block sizes yields identical PCM.

The old integer tick-length rule is untouched for ticks created after the
change: `m_nBufferCount = rate * 5 * tempoFactor / (tempo << 8)` computed at the
new rate.

### Volume ramps

An in-flight amplitude ramp keeps its current value and its target.  Only the
remaining frame count is rescaled (rounded up) to the new rate, and the same
amplitude delta is spread over those frames, so the ramp continues smoothly
towards its target instead of jumping or restarting.

### DSP tails (surround and reverb)

The long delay lines are migrated, not cleared:

* The **surround** line (one ring) and the four **reverb** lines have their new
  lengths computed with the existing formulas at the new rate.
* The old ring is read starting at its next-read position as a periodic sequence
  of length `L`; the new ring of length `M` is filled by taking, for each new
  index `j`, the old sequence value at `j*L/M`, linearly interpolated between the
  two neighbouring old samples.
* Intermediate products use 64-bit arithmetic and the signed division truncates
  toward zero.
* After the migration the next-read position of every migrated ring is `0`, and
  the existing feedback algorithm continues with the unchanged effect parameters
  and enable flags.

The short filter rings, the filter accumulators, the reverb low-pass history, the
XBass state and the noise-reduction history are **not** re-initialised, so their
tails are preserved too.

## Other observable effects

* After a successful change the global `mFrequency` setting reflects the new
  rate (as seen through `ModPlug_GetSettings`).
* `ModPlug_Seek` keeps its existing approximate positioning behaviour.  Seeking
  also clears the fractional tick-duration remainder left behind by a rate
  change, so the next tick starts cleanly at the new rate.
* `ModPlug_Read` output for a given sequence of rate changes and reads is
  independent of how the reads are split into blocks.

## Not covered

* Non-16-bit-stereo output and MegaBass (rejected).
* Multiple simultaneous module handles or concurrent access (the legacy global
  settings model is assumed).

## Testing

The regression tests live in `tests/rate_checks.cpp` and use the small task
harness (`task_support/check.h`).  `bash task_support/run.sh target` builds the
production library, compiles a fixed API caller and the tests, and requires the
`clock`, `tails` and `compat` groups to run to completion.  The groups cover:

* `clock` - tick / envelope positions are not advanced again across a change,
  switching mid-tick and mid-ramp, looped-sample pitch (both constant-rate and
  immediately after the switch), and a strong "the clock did not move when a
  change is merely queued" check.
* `tails` - surround and reverb echoes survive the switch, and the tail stays
  continuous when noise reduction, surround and reverb are all enabled.
* `compat` - success/rejection contract, 16-bit-stereo and MegaBass gating,
  byte-exact identity when the rate does not change, block-size independence,
  no drift under repeated round-trips, the settings rate updating, and seeking
  after a change.

