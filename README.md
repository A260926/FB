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


## Limitations

- no error reporting. PL_MPEG will silently ignore any invalid data.
- the pts (presentation time stamp) for packets in the MPEG-PS container is
ignored. This may cause sync issues with some files.
- bugs, probably.


## Reading the original PES presentation time stamps

The decoder normally synthesizes a frame clock from the frame rate
(`frame->time`). If you need the presentation time stamp that was actually
stored in the MPEG-PS PES layer — for example to sync against another source —
enable PTS pass-through before decoding starts. Each decoded frame then
exposes `has_pts` and `pts90k`, where `pts90k` is the original, 33 bit
unwrapped time stamp in 90kHz ticks. Only the first picture of a PES packet
carries the packet's PTS; the other frames report `has_pts == 0`. B-frame PTS
values are intentionally not made monotonic.

```c
#include <stdio.h>
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

int main(void) {
	plm_t *plm = plm_create_with_filename("some-file.mpg");
	if (!plm) {
		return 1;
	}
	plm_set_audio_enabled(plm, 0);

	// Must be enabled before the first decode/seek call.
	if (!plm_set_video_pts_enabled(plm, 1)) {
		// Decoding had already started; the configuration was not changed.
	}

	plm_frame_t *frame;
	while ((frame = plm_decode_video(plm))) {
		if (frame->has_pts) {
			printf("frame t=%.3f  pts=%lld (90kHz)  %.3fs\n",
				frame->time,
				(long long)frame->pts90k,
				frame->pts90k / 90000.0);
		}
		else {
			printf("frame t=%.3f  (no PES PTS)\n", frame->time);
		}
	}

	plm_destroy(plm);
	return 0;
}
```

The same fields are populated on frames delivered through the
`plm_set_video_decode_callback()` callback, and the feature works identically
for file, memory and custom-buffer inputs. When left disabled (the default),
`has_pts` and `pts90k` are always zero and all other behavior is unchanged.
The lower level `plm_video_*` elementary-stream decoder has no PES layer, so
those frames never carry a PTS either.

