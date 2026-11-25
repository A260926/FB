# WavStream bidirectional playback (`setWavStreamDirection`)

## Purpose

For clip pre-listening ("scrubbing") a playing WAV must be able to switch between
forward and reverse playback at any moment, and resume forward playback when the
mouse button is released, without restarting the sound or rebuilding a player.

The new entry point is:

```cpp
result Soloud::setWavStreamDirection(SoLoud::handle voice, int direction);
```

`direction` accepts only `+1` (forward) and `-1` (reverse). Any other value is
rejected.

## Scope and compatibility

The first successful call switches the voice onto the bidirectional path. Until
then, and for every unsupported source, the original forward-only pipeline is
used unchanged, so all existing behaviour (other audio sources, buses, looping,
filters, 3D, the rejection of negative relative play speeds) stays compatible.

Bidirectional playback is supported only for a voice that is **all** of:

- a `WavStream` (not an in-memory `Wav`, not a bus voice),
- played on the root mixer (`aBus == 0`),
- non-looping and without any filter,
- not a 3D voice,
- backed by a mono or stereo PCM16 or float32 RIFF WAV,
- loaded through `load`, `loadMem` or `loadToMem` (or an equivalent `File`).

An invalid handle, a voice-group handle, an unsupported source or an unsupported
configuration returns `INVALID_PARAMETER` and leaves the playback state
completely untouched. Repeating the current direction has no additional effect.

## The material coordinate `x`

A bidirectional voice owns a single playhead `x`: the source coordinate (in
source sample frames, stored with the mixer's 20-bit fixed-point resolution) of
the **next output frame**.

Per output frame the engine first samples the material at `x`, then advances

```
x <- x + direction * h
```

where `h` is the same fixed-point step the mixer already uses for resampling:

```
h = floor((sourceRate * relativePlaySpeed / outputRate) * 2^20) / 2^20
```

The fractional part always belongs to the material coordinate. Reversing only
flips the sign of the step; it never mirrors or otherwise rewrites the
fractional part. Switching direction keeps `x` (including its fraction) fixed.

When bidirectional playback is enabled, `x` is seeded from the position actually
delivered by the legacy resampler, never from the decoder's read-ahead position.
After that, forward playback uses the same bidirectional state as reverse.

### Interpolation

- `RESAMPLER_POINT`: take `floor(x)`.
- `RESAMPLER_LINEAR`: interpolate between `floor(x)` and `floor(x) + 1`. At the
  final source frame the right neighbour is a copy of the last frame.

A frame only sounds while `0 <= x < frameCount`. Once `x` leaves that range the
rest of the current mix buffer is silence, and the existing auto-stop setting
decides whether the voice is stopped (so a prelisten voice typically disables
auto-stop to survive reaching an endpoint).

## Transport and queries

- `setRelativePlaySpeed` still takes a positive absolute speed. Changing the
  speed changes `h` but never changes `x`. Negative speeds remain rejected.
- Pausing freezes `x`; nothing advances while a voice is paused.
- `seek` still uses material seconds with the valid range `[0, length)`. On
  success `x = floor(seconds * sourceRate)`, while the direction and the
  accumulated `getStreamTime` are preserved; the old decode window and resampler
  leftovers are discarded. An out-of-range seek returns `INVALID_PARAMETER` and
  preserves all state.
- `getStreamPosition` returns `x / sourceRate` for the next frame. It does not
  move while paused and is clamped to the nearest endpoint once `x` walks out of
  range.
- `getStreamTime` only accumulates output time actually spent mixing, and keeps
  increasing while reversing.
- Re-calling `play` cannot rebind a handle or reset its pan/volume; the API only
  ever operates on an existing live voice.

## Bounded memory and windowed file I/O

The whole clip is never expanded into memory and no second player is created.
Each bidirectional voice keeps one fixed decode window of at most
`2048` source frames per channel, regardless of file length, plus a raw read
buffer of the same size.

The window is placed in the direction of travel so that it always contains the
interpolation right neighbour (`floor(x) + 1`) and the playhead can walk a full
window (up to 2048 source frames) before another refill is needed. Continuous
reverse playback therefore serves many output frames from the window and never
seeks the file once per frame; crossing a window boundary costs one seek and one
contiguous read. The same coordinate drives the first fill after a direction
change, 512-frame read boundaries and mismatched source/output sample rates.

## Threading

Control calls happen between two `mix` calls; there is no concurrency or audio
callback re-entrancy to handle. `setWavStreamDirection` takes the audio mutex
just like the other voice controls.
