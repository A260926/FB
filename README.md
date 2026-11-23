# SDL_sound

SDL_sound; an abstract soundfile decoder.

## What is this?

SDL_sound is a library that handles the decoding of several popular sound file
formats, such as .WAV and .MP3. It is meant to make the programmer's sound
playback tasks simpler. The programmer gives SDL_sound a filename, or feeds
it data directly from one of many sources, and then reads the decoded
waveform data back at her leisure. If resource constraints are a concern,
SDL_sound can process sound data in programmer-specified blocks. Alternately,
SDL_sound can decode a whole sound file and hand back a single pointer to the
whole waveform. SDL_sound can also handle sample rate, audio format, and
channel conversion on-the-fly and behind-the-scenes, if the programmer
desires.

Please check the website for the most up-to-date information about SDL_sound:

https://icculus.org/SDL_sound/

SDL_sound _REQUIRES_ Simple Directmedia Layer (SDL) 3.x to function, and cannot
be built without it. You can get SDL from https://www.libsdl.org/

Current versions of SDL_sound do not use any external libraries for decoding
various file formats. All needed decoder source code is included with the
library. Unless explicitly disabled during initial build configuration,
SDL_sound always supports the following file formats internally:

- Wave (.WAV) files
- MPEG-1 layers I-III (.MP3, .MP2, .MP1)
- Ogg Vorbis (.OGG) files
- Free Lossless Audio Codec (.FLAC) files
- Audio Interchange format (.AIFF) files
- Sun/NeXT Audio (.AU) files
- Shorten (.SHN) files
- Creative Labs Voice (.VOC) files
- Various "module" formats (.MOD, .669, .AMF, .XM, .IT, .S3M, .STM, etc)
- MIDI (.mid) files
- Raw PCM data
- (macOS/iOS only) anything that CoreAudio can decode.

## Building/Installing:

Please read docs/INSTALL.txt.

## Finite loop regions

For game-style sound preview, a seekable 16-bit PCM or Microsoft ADPCM
RIFF/WAVE (mono or stereo) can be given a finite loop region in native sample
frames with `Sound_SetLoop(sample, start, end, additional_repeats)`. The
decoded stream is then logically

```
[0,end), additional_repeats extra plays of [start,end), then [end,N)
```

where N is the source length. Zero repeats is the original material, and only
the region bounds and the repeat count are stored — the expanded waveform is
never allocated. Loop points are exact frames; an ADPCM start or end may land
in the middle of a block and is never rounded to milliseconds.

There are two distinct ways playback position changes, and they keep
different converter history:

- **Natural loop jumps** happen inside the decoder when a read reaches the
  region end. The underlying WAV decoder seeks back to the region start, but
  the same `SDL_AudioStream` keeps running: it is not flushed, cleared or
  recreated, so resampling and any filter history stay continuous across the
  jump. All repeats and the tail flow through that one stream.
- **Explicit seeks** (`Sound_SeekLoopFrame`, and `Sound_Rewind`, which is a
  seek to logical frame zero) start a brand-new playback generation. Queued
  PCM, pending EOF/error state and all converter/resampler history are
  discarded, so the suffix at the target decodes with zero prior history, as
  if it were the start of a new stream. The repeat configuration and the
  repeats still remaining are retained. Out-of-range targets are rejected
  before touching the backend, leaving the current scene unchanged; a failed
  underlying seek sets the error state instead of delivering stale audio, and
  a later successful seek recovers.

While a loop is configured, the millisecond `Sound_Seek` is refused; use
`Sound_SeekLoopFrame`. The region can be configured before the first decode,
or replaced after a `Sound_Rewind`. `Sound_GetLoopPosition` reports the
delivery clock (frames actually handed to the application), never the
decoder's read-ahead position.

## Reporting bugs/commenting:

Please visit https://github.com/icculus/SDL_sound/issues for the bug tracker.

