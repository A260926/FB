# PL_MPEG - MPEG1 Video decoder, MP2 Audio decoder, MPEG-PS demuxer

Single-file MIT licensed library for C/C++

See [pl_mpeg.h](https://github.com/phoboslab/pl_mpeg/blob/master/pl_mpeg.h) for
the documentation.


## Why?

This is meant as a simple way to get video playback into your app or game. Other
solutions, such as ffmpeg require huge libraries and a lot of glue code.

MPEG1 is an old and inefficient codec, but it's still good enough for many use
cases. All patents related to MPEG1 and MP2 have expired, so it's completely
free now.

This library does not make use of any SIMD instructions, but because of
the relative simplicity of the codec it still manages to decode 4k60fps video
on a single CPU core (on my i7-6700k at least).


## Building

A Makefile to build the example applications is included. 

- `pl_mpeg_extract_frames`: a command line tool that dumps all frames into BMPs
- `pl_mpeg_player_gl`: a video player using SDL2 and OpenGL
- `pl_mpeg_player_sdl`: a video player using SDL2 and it's built-in 2d renderer

The players require the SDL2 library to be installed.

```shell
make # build all
make extract # only build pl_mpeg_extract_frames
make player_gl # only build pl_mpeg_player_gl
make player_sdl # only build pl_mpeg_player_sdl
```


## Encoding for PL_MPEG

Most [MPEG-PS](https://en.wikipedia.org/wiki/MPEG_program_stream) (`.mpg`) files
containing MPEG1 Video ("mpeg1") and MPEG1 Audio Layer II ("mp2") streams should
work with PL_MPEG. Note that `.mpg` files can also contain MPEG2 Video, which is
not supported by this library.

You can encode video in a suitable format using ffmpeg:

```
ffmpeg -i input.mp4 -c:v mpeg1video -q:v 0 -c:a libtwolame -b:a 224k -format mpeg output.mpg
```

`-q:v` sets a fixed video quality with a variable bitrate, where `0` is the 
highest. You may use `-b:v` to set a fixed bitrate instead; e.g. 
`-b:v 2000k` for 2000 kbit/s. Please refer to the 
[ffmpeg documentation](http://ffmpeg.org/ffmpeg.html#Options) for more details.

If you just want to quickly test the library, try this file:

https://phoboslab.org/files/bjork-all-is-full-of-love.mpg


## Reading the original PES presentation time stamps

By default, frames only carry a local clock (`plm_frame_t.time`) synthesized
from the frame rate. If you need the presentation time stamp stored in the
MPEG-PS container (e.g. for A/V sync against an external clock), enable PTS
tracking right after creating the decoder and before decoding or seeking. The
original 33 bit PTS is then exposed on each frame in 90kHz ticks:

```c
#include "pl_mpeg.h"

plm_t *plm = plm_create_with_filename("some-file.mpg");
plm_set_audio_enabled(plm, FALSE);

// Opt in before the first plm_decode_video()/plm_decode()/plm_seek() call.
// Returns 1 on success and 0 if decoding already started (configuration left
// untouched). It is off by default.
if (!plm_set_video_pts_enabled(plm, TRUE)) {
    // too late: decoding or seeking already began
}

plm_frame_t *frame;
while ((frame = plm_decode_video(plm))) {
    if (frame->has_pts) {
        // frame->pts90k is the unwrapped PES PTS in 90kHz ticks.
        double pts_seconds = frame->pts90k / 90000.0;
        printf("frame @%.3f local, PTS %.3f s (%lld ticks)\n",
            frame->time, pts_seconds, (long long)frame->pts90k);
    }
    // Frames without an explicit PTS report has_pts == 0 and pts90k == 0.
    // The local frame->time clock is always available regardless.
}
```

Notes:

- A PES packet's PTS is only attached to the first picture whose start code
  begins inside that packet; later pictures in the same packet and pictures in
  PTS-less packets report `has_pts == 0` instead of guessing a time.
- The raw 33 bit values are preserved and unwrapped across the 2^33 wrap, so
  `pts90k` is a signed 64 bit tick count that may legitimately step backwards
  for B-frame streams.
- The same fields are populated for the pull API (`plm_decode_video`), the
  video decode callback (`plm_decode`) and both file and memory sources. The
  standalone `plm_video_*` elementary-stream decoder has no PES layer and
  always reports `has_pts == 0`.
- `plm_rewind()` clears the bindings and re-establishes them on the next read;
  `plm_seek()` starts a fresh expansion from the first PTS seen after the seek.


## Limitations

- no error reporting. PL_MPEG will silently ignore any invalid data.
- PES presentation time stamps are only surfaced when explicitly requested via
  `plm_set_video_pts_enabled()`; otherwise frames use the frame-count based
  local clock, which is sufficient for many playback scenarios.
- bugs, probably.
