# Retrigger crossfade

`MikMod_SetRetriggerFade(ULONG frames)` extends the software mixers so that
retriggering a voice that is already sounding hands the old sound over to the
new one instead of cutting it off.

## Public API

```c
int MikMod_SetRetriggerFade(ULONG frames);
```

* Must be called **before** `MikMod_Init()`.
* `0` disables the feature and keeps the historical behaviour exactly (the
  default).
* `1..4096` selects the hand-off length in **output frames**.
* Returns `1` when the setting was accepted.
* Returns `0` when the library is already initialized or `frames` is out of
  range; in that case the previous setting is kept.

The setting is global. It is snapshotted by the active software mixer when the
mixer is initialized, so changing it after `MikMod_Init()` (which is rejected
anyway) has no effect on a running mixer.

Only the C scalar software mixers are extended: the normal mixer
(`DMODE_INTERP`, `not DMODE_HQMIXER`) and the high-quality mixer
(`DMODE_HQMIXER`). Mono/stereo, 8-bit/16-bit/float output and `DMODE_SURROUND`
are supported. Hardware mixing and SIMD mixing are out of scope. When the
feature is off, every existing API and every produced PCM byte is unchanged.

## Hand-off shape

On `Voice_Play()` for a voice that is already producing PCM, the mixer
snapshots the departing voice's **next** playback state:

* integer and fractional sample phase,
* direction (`SF_REVERSE` / `SF_BIDI`),
* loop bounds,
* step (increment),
* the current effective left/right gains.

The new main voice starts from the requested `start`. Both cursors then advance
independently using the mixer's normal interpolation and loop rules. For output
frame `j` (counted from 0):

```
out = ((N-j) * old + j * new) / N
```

so after `N` frames only the new main voice remains. With the HQ mixer the four
internal sampling steps that make up one output frame share the same
`(N-j, j)` weights; they are combined **before** the down-sampling stage,
effects and the output callback, which therefore advance only once per output
frame.

The departing voice is a real, advancing cursor: it is never approximated by
holding its last value, and its tail is never pre-generated. Nothing about the
hand-off delays a tick.

## Attack click and control ramps

Both ends of the hand-off suppress the start-of-note click and the volume /
panning control ramp. The departing gains are frozen at the values reached
before the hand-off; the new main voice uses the frequency, volume and panning
that were set for this output boundary. The caller keeps these three constant
for the duration of the hand-off. Surround keeps each backend's own gain
formula and sign (`vol/2` for the normal mixer, `vol*256/480` for the HQ
mixer).

If there is no previous sound (first attack) or the old voice had already ended
naturally, the retrigger is an ordinary attack. `Voice_GetPosition()` and
`Voice_Stopped()` describe the main voice only and never account for a
departing tail.

## One departing state per voice

Each voice keeps at most one departing state. A retrigger that occurs while a
hand-off is still running drops the previous departing state and re-captures
the **current main voice's** next playback state as the new departing state,
restarting the `N`-frame count. Three-way overlap is not attempted.

Several `Voice_Play()` calls on the same output boundary (with no PCM produced
in between) collapse to a single hand-off: the pending, never-sounded targets
do not become departing states, and only the latest target becomes the new main
voice while the voice that was genuinely sounding before the batch is the
departing state.

`Voice_Stop()` cancels the main voice, any pending attack and the departing
voice together. A slot reused afterwards does not inherit the old tail.

## Sample ownership

A caller may `Sample_Free()` the old sample immediately after the retrigger and
the departing voice keeps playing the original PCM to the end.

The mixer owns sample buffers per **generation**, not per numeric handle. A
loaded buffer gets a reference record; the handle slot owns one reference and a
running departing voice owns one. When `Sample_Free()` releases the handle
while a departing voice still needs the data, the buffer is retired from the
handle instead of being freed; the last reference frees it. A later load may
reuse the same numeric handle without touching the retired generation, so it
cannot replace the old tail.

Memory use is bounded by the loaded samples plus retired samples still
referenced by a hand-off, plus a fixed per-voice playback state. It does not
grow with playback time, and `WriteBytes()` performs no heap allocation.

Sample buffers are not copied. Retired buffers are reclaimed as soon as the
last departing reference ends, is stopped or is replaced, and all remaining
references are released at mixer exit.

## Control boundary

All control calls (`Voice_Play`, `Voice_Stop`, sample load/free, voice
configuration) are serialized between two output calls. The feature does not
extend re-entrancy or concurrency guarantees.
