# Signalsmith Stretch: C++ pitch/time library

This is a C++11 library for pitch and time stretching, using the final approach from the ADC22 presentation [Four Ways To Write A Pitch-Shifter](https://www.youtube.com/watch?v=fJUmmcGKZMI).

It can handle a wide-range of pitch-shifts (multiple octaves) but time-stretching sounds best for more modest changes (between 0.75x and 1.5x).  There are some audio examples and an interactive web demo on the [main project page](https://signalsmith-audio.co.uk/code/stretch/).

## How to use it

Just include `signalsmith-stretch.h` where needed:

```cpp
#include "signalsmith-stretch.h"

signalsmith::stretch::SignalsmithStretch<float> stretch;
```

### Configuring

The easiest way to configure is a `.preset???()` method:

```cpp
stretch.presetDefault(channels, sampleRate);
stretch.presetCheaper(channels, sampleRate);
```

If you want to try out different block sizes for yourself. you can use `.configure()` manually:

```cpp
stretch.configure(channels, blockSamples, intervalSamples);

// query the current configuration
int block = stretch.blockSamples();
int interval = stretch.intervalSamples();
```

### Processing and resetting

To process a block, call `.process()`:

```cpp
float **inputBuffers, **outputBuffers;
int inputSamples, outputSamples;
stretch.process(inputBuffers, inputSamples, outputBuffers, outputSamples);
```

The input/output buffers cannot be the same, but they can be any type where `buffer[channel][index]` gives you a sample.  This might be `float **` or a `double **` or some custom object (e.g. providing access to an interleaved buffer), regardless of what sample-type the stretcher is using internally.

To clear the internal buffers:

```cpp
stretch.reset();
```

### Pitch-shifting

```cpp
stretch.setTransposeFactor(2); // up one octave

stretch.setTransposeSemitones(12); // also one octave
```

You can set a "tonality limit", which uses a non-linear frequency map to preserve a bit more of the timbre:

```cpp
stretch.setTransposeSemitones(4, 8000/sampleRate);
```

Alternatively, you can set a custom frequency map, mapping input frequencies to output frequencies (both normalised against the sample-rate): 

```cpp
stretch.setFreqMap([](float inputFreq) {
	return inputFreq*2; // up one octave
});
```

### Time-stretching

To get a time-stretch, hand differently-sized input/output buffers to .process(). There's no maximum block size for either input or output.

Since the buffer lengths (inputSamples and outputSamples above) are integers, it's up to you to make sure that the block lengths average out to the ratio you want over time.

### Latency

Latency is particularly ambiguous for a time-stretching effect. We report the latency in two halves:

```cpp
int inputLatency = stretch.inputLatency();
int outputLatency = stretch.outputLatency();
```

You should be supplying input samples slightly ahead of the processing time (which is where changes to pitch-shift or stretch rate will be centred), and you'll receive output samples slightly behind that processing time.

### Split computation

All of the `.preset???()` and `.configure()` methods have an optional `splitComputation` flag.  When enabled, this introduces one extra interval of output latency, and uses this to spread the computation out more evenly.

Without this (as is common for spectral processing) the library will occasionally do a bunch of computation all at once, to compute the next spectral block of audio.  This is often fine, when audio's being processed across multiple threads with a decent amount of buffering (like mixing in a DAW), but if you're in a stricter situation then this flag might help. 

#### Automation

To follow pitch/time automation accurately, you should give it automation values from the current processing time (`.outputLatency()` samples ahead of the output), and feed it input from `.inputLatency()` samples ahead of the current processing time.

### Seeking and starting

You can use `.seek()` which lets you move around the input audio, by providing a bunch of input samples.  You should ideally provide at least (one block-length + one interval) of input data:

```cpp
stretch.seek(inputBuffers, inputSamples, playbackRateHint);
```

At the very start of playback (or after a `.reset()`), the current processing time is `.inputLatency()` samples *before* the first input samples you give it.  You therefore might want to call `.seek()` to provide the first `inputSamples = stretch.inputLatency()` samples of input, so that the processing time matches the start of the input (meaning your pre-roll output is only `.outputLatency()` samples long).

### Ending

If you're processing a fixed-length sound (instead of an infinite stream), you'll reach the end of your input, but still have some pending output.  You should first make sure the processing time gets to the end, by passing an additional `.inputLatency()` samples of silence to `.process()` (similar to using `.seek()` at the beginning).

You can then read the final part of the output using `.flush()`.  It's recommended to read at least `.outputLatency()` samples of output:

```cpp
stretch.flush(outputBuffers, outputSamples);
``` 

Using `.seek()`/`.flush()` like this, you can perform an exact time-stretch on a fixed-length sound, and your result will have `.outputLatency()` of pre-roll.

### Formant compensation

```cpp
stretch.setFormantFactor(1.2); // up ~3 semitones

stretch.setTransposeSemitones(3);
```

Both of those methods both have an optional `compensatePitch` flag.  Enabling this adjust for the pitch-shift (or non-linear map) when correcting/shifting formants.

The formant correction is not a sharp as monophonic algorithms (such such as PSOLA).  It also needs you to give it a rough estimate of fundamental frequency (relative to Nyquist):

```cpp
// if 200Hz is the middle-register of the instrument
stretch.setFormantBase(200/sampleRate);
```

### Spectral freeze

Spectral freeze holds the current timbre, as if you were holding down the sound.  Once captured, the output keeps that spectrum even if the live input turns to silence or to an entirely different sound.  Releasing it fades back to the current live input.

This is a "capture and hold" effect, not a loop/recorder: there is no buffer of recorded audio, and work memory depends only on channel count and window size, not on how long you hold it.

```cpp
stretch.setSpectralFreeze(true);  // request a capture
bool active = stretch.spectralFreezeActive(); // true once a capture exists and is being held

stretch.setSpectralFreeze(false); // release, fades back to the live input
```

The request is latched at the start of the next internal hop (the window/interval configured via `.configure()`, e.g. 1024/256).  If `splitComputation` is enabled, a hop which has already started keeps computing with its previous state, so the switch happens at the next complete hop boundary rather than mid-calculation.

#### Capture

On the first transition to enabled, the two most recent valid analysis spectra are used to capture, per frequency bin:

* the energy/amplitude, and
* the instantaneous frequency (from the phase advance between the two spectra, without snapping it to the FFT bin centre),

and per channel the captured amplitude and phase, so the inter-channel amplitude ratio and phase difference are kept.  The captured values are fixed for the whole freeze: it does not re-capture, and it does not restart from the same phase each hop - the synthesis phase advances continuously from the captured instantaneous frequencies.

Capture waits until two real analysis frames exist.  Until then `spectralFreezeActive()` is `false`.  If the input is only silence, the output stays at finite zeros rather than synthesising random phase, and the request remains pending: the next real audio is captured.

The real input is still analysed normally for the whole freeze (it is not paused, and the last window is not replayed), so a long silent input does not clear the held timbre and the live path is ready to resume.

#### Release

Calling `.setSpectralFreeze(false)` starts a four-hop crossfade from the frozen spectrum back to the current live analysis.  The request takes effect at the next hop boundary; after four complete hops (plus the usual reported latency) the output is purely live.  It does not wait for the input to become loud again.

If the freeze is re-requested during those four hops, it captures fresh from the live analysis at that moment (it does not resume the old snapshot).  Calling `.setSpectralFreeze(true)` again while already holding is a no-op: it does not restart or re-capture the sound.

#### Tail and lifecycle

`reset()`, `configure()` and `seek()` clear both the pending request and any captured snapshot.  `flush()` clears the freeze state before doing its usual finite flush, so a held tone never turns into an endless tail - the flush is the same bounded synthesis tail as without a freeze.

```cpp
// end of stream: don't leave a frozen tone hanging
stretch.flush(outputBuffers, outputSamples);
```

Spectral freeze currently supports `float`/`double` samples, 1 or 2 channels, 48kHz, and window/interval sizes of 1024/256 or 2048/512, with the same number of input and output frames.  Pitch-shifting, formant compensation and custom frequency maps are bypassed while frozen (the captured spectrum is reproduced as-is).

## Compiling

⚠️ This has mostly been tested with AppleClang (Mac), and MSVC (Windows).  We aim to support other compilers though, so please get in touch if you have any problems.

Enabling `-ffast-math` (or equivalent) is fine, unless you're using Apple Clang 16.0.0 (in which case it will deliberately break, because that version generates incorrect SIMD code).

The algorithm has a lot of number-crunching, so Debug builds are much slower (up to 10x).  If possible, you might want to enable optimisation where Stretch is used, even in Debug builds.

### Dependencies and `#define`s

This uses the [Signalsmith Linear](https://github.com/Signalsmith-Audio/linear) library for FFTs and other speedups.  There are [flags]([Linear repo](https://github.com/Signalsmith-Audio/linear?tab=readme-ov-file#building)) to use faster FFT implementations (`SIGNALSMITH_USE_ACCELERATE`/`SIGNALSMITH_USE_IPP`/`SIGNALSMITH_USE_PFFFT`/`SIGNALSMITH_USE_PFFFT_DOUBLE`), and corresponding CMake options.

## License

Released under the [MIT License](LICENSE.txt) - get in touch if you need anything else.

## Other environments / languages

There's a Web Audio release in [`web/`](web/) (WASM/AudioWorklet), also available on [NPM](https://npmjs.com/package/signalsmith-stretch).  This has its own (higher-level) API, but the stretching algorithm will remain in-sync with the C++ library.

There's a [Python binding](https://pypi.org/project/python-stretch/) written/published by [Gregorio Andrea Giudici](https://github.com/gregogiudici/python-stretch).  This is used as the default pitch/time method in [Audiomentations](https://iver56.github.io/audiomentations/).

There's a [Rust wrapper](https://crates.io/crates/signalsmith-stretch) by [Colin Marc](https://github.com/colinmarc/signalsmith-stretch-rs).

## Thanks

We'd like to particularly thank the following people who sponsored specific features or improvements:

* **Future Audio Workshop**: chunked-computation mode
* **Jochem de Jong**: formant shifting/compensation
* **Metronaut**: web audio (JS/WASM) release
* **Daniel L Bowling** and the Stanford School of Medicine: web audio improvements

We're also grateful for the following community contributions:

* **Steve MacKinnon** for finding/resolving a bug in `.reset()`
