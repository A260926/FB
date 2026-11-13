# TinySoundFont
SoundFont2 synthesizer library in a single C/C++ file

## Overview

TinySoundFont is a software synthesizer using SoundFont2 sound bank files.

The library is a single C header file so it is extremely simple to integrate in your C/C++ projects.

```c++
#define TSF_IMPLEMENTATION
#include "tsf.h"

...

tsf* TinySoundFont = tsf_load_filename("soundfont.sf2");
tsf_set_output(TinySoundFont, TSF_MONO, 44100, 0); //sample rate
tsf_note_on(TinySoundFont, 0, 60, 1.0f); //preset 0, middle C
short HalfSecond[22050]; //synthesize 0.5 seconds
tsf_render_short(TinySoundFont, HalfSecond, 22050, 0);
```

The library code is based on [SFZero by Steve Folta](https://github.com/stevefolta/SFZero).

## Chorus effect

The SoundFont `ChorusEffectsSend` generator is now honored. The send amount is
merged exactly like the other zone generators: a zone's local value overrides
its own layer's global value (instrument and preset layers independently), the
two layer values are then added, clamped to `0..1000`, and divided by `1000` to
give the per-voice send coefficient.

### Where the send is tapped

For each voice the mono signal is sent to the chorus *after* sample
interpolation, the low-pass filter, the amplitude envelope and the existing
gain, but *before* panning. The dry signal keeps its existing pan and full
volume — it is never attenuated by the send. All voices add into a single
instance-wide send bus, and the chorus is advanced exactly once per output
frame regardless of how many voices are playing.

### Delay model

`tsf_set_chorus` configures the effect. For output frame `n` the left/right
delays (in output sample frames) are

```
delayL(n) = D + A * sin(phase)
delayR(n) = D - A * sin(phase)
phase    += 2 * pi * rate / sampleRate          // phase starts at zero
```

where `D` and `A` are the center delay and modulation depth converted from
milliseconds to frames at the current output sample rate. Two independent
zero-initialized ring buffers are used with a strict read-before-write rule:
the delayed `yL`/`yR` are first read with linear interpolation at the
(possibly fractional) `n - delay` position, then

```
bufferL[n] = send[n] + feedback * yR
bufferR[n] = send[n] + feedback * yL            // cross feedback
output    += wetGain * yL / wetGain * yR
```

In `TSF_MONO` the two wet channels are averaged. The same path is used for
float and short output and for `flag_mixing`; short quantization and clamping
happen after the dry and wet signals are summed.

```c
// center delay 8ms, +/-2ms modulation, 1.5Hz LFO, 0.3 cross feedback, 0.5 wet
if (!tsf_set_chorus(f, 8.0f, 2.0f, 1.5f, 0.3f, 0.5f)) {
    // invalid parameters: the previous configuration is left untouched
}
```

Constraints (the call returns `0` and changes nothing when violated): all
arguments must be finite; `D - A` must cover at least one sample frame;
`D + A` must not exceed 100ms; `rate` in `[0, 10]` Hz; `feedback` in
`[-0.9, 0.9]`; `wetGain` in `[0, 2]`. A `wetGain` of `0` disables the effect
and clears its history; any successful reconfiguration also clears the
history. Buffers are allocated inside `tsf_set_chorus`, so rendering never
allocates. The output sample rate cannot be changed while the chorus is
enabled — disable it first.

```c
tsf_set_chorus(f, 8.0f, 2.0f, 1.5f, 0.3f, 0.0f); // disable and clear
tsf_set_output(f, TSF_STEREO_INTERLEAVED, 48000, 0);
tsf_set_chorus(f, 8.0f, 2.0f, 1.5f, 0.3f, 0.5f); // re-enable at the new rate
```

### Lifecycle

- The effect is disabled by default; without `tsf_set_chorus`, PCM output and
  all public interfaces behave exactly as before.
- After voices end, the chorus keeps advancing on zero input, so effect tails
  are not cut when `tsf_active_voice_count` reaches zero. A zero-frame render
  does not advance the phase.
- `tsf_reset` clears the chorus history and phase but keeps the configuration
  (alongside its existing quick voice release).
- `tsf_copy` keeps the chorus parameters but gives the copy its own
  zero-initialized history and scratch buffers; active voices are still not
  copied. Instances continue to share the underlying soundfont, so closing the
  original does not affect a copy.

## Documentation

The API documentation can be found on [top of the library source code](https://github.com/schellingb/TinySoundFont/blob/master/tsf.h).

There are also [examples available](https://github.com/schellingb/TinySoundFont/tree/master/examples) which come with a sample SoundFont file and build and play sound on Win32, Win64, Linux and MacOSX with no further dependencies.

## Dependencies

C standard libraries for fopen, math and malloc (can be removed by providing custom functions with #defines).

## Strict No LLM / No AI Policy
No LLMs for issues. No LLMs for source contributions/patches/pull requests.

AI must not be used to generate code for contributions to this project.

Any pull request to this project will ask you to confirm that you are the author and that you are contributing your changes under the license of the project.

## License

TinySoundFont is available under the [MIT license](https://choosealicense.com/licenses/mit/).
