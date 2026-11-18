# libttaR

Tau's True Audio (TTA) codec rewrite


## What is TTA?

TTA is a lossless audio codec designed around realtime hardware coding.
The codec has no tunable parameters, and each input produces a unique output.
It supports 8, 16, and 24-bit PCM.

http://tausoft.org/wiki/True_Audio_Codec_Overview


## Blog

How ttaR is faster than ttaenc (2024-12-28)
https://gist.github.com/stseelig/0b9afe795a547b5798587db996694f97


## Platforms

The library is platform agnostic.
The program is POSIX native
and has a mostly native (mostly working) Win32 port via MinGW.


### Processors

2's complement integers are assumed.

While developing, I mostly tested on:

* AMD Ryzen 7 1700    (primary tuning)

* Intel Celeron N2830 (secondary tuning)

honorable mentions:

* IBM PowerPC 7400 (intrinsics)

* ARM Cortex-A53 (intrinsics)


## Building

Unix
```
$ clang -O3 -DNDEBUG -nolibc -ffreestanding -fPIC -shared ./src/build_lib.c -o libttaR.so
$ clang -O3 -DNDEBUG ./src/build_cli.c -o ttaR -L./ -lpthread -lttaR
```

Windows
```
$ mingw32-clang -O3 -DNDEBUG -ffreestanding -fPIC -shared ./src/build_lib.c -o libttaR.dll
$ mingw32-clang -O3 -DNDEBUG ./src/build_cli.c -o ttaR.exe -L./ -lttaR
```

gcc(1) would also work, but it produces a much slower binary than clang(1).


### Defines

NDEBUG

* disables all debug assertions

LIBTTAr_OPT_DISABLE_UNROLLED_1CH

* disables the unrolled mono loop

LIBTTAr_OPT_DISABLE_UNROLLED_2CH

* disables the unrolled stereo loop

LIBTTAr_OPT_DISABLE_MCH

 * disables the general/multichannel loop

LIBTTAr_OPT_DISABLE_SIMD_INTRINSICS

* disables SIMD intrinsics

LIBTTAr_OPT_SLOW_CPU

* for weak and/or old CPUs (specifically the Intel Celeron N2830)

* fine-grained suboptions in 'src/lib/common.h'


## Basic Usage

```
$ ttaR encode file.(wav|w64)
$ ttaR decode file.tta
```

By default, ttaR will multithread with the number of coder threads equal to
the number of online processors.

Read the man page (./man/ttaR.1) for a full list of the options.


## Streaming frame decode

The libttaR API has a second decoder entry point for callers that receive a
frame's compressed bytes in pieces (a socket, a ring buffer, a hardware
transport) instead of owning the whole frame up front.  It decodes exactly one
TTA1 audio frame per initialization:

```
size_t libttaR_streamdec_size(unsigned int nchan);
int    libttaR_streamdec_init(void *workspace, size_t workspace_bytes,
                              unsigned int samplebytes, unsigned int nchan,
                              size_t sample_frames, size_t frame_bytes);
int    libttaR_streamdec_process(void *workspace, const uint8_t *src,
                                 size_t *bytes, int32_t *dst, size_t *frames,
                                 int final);
```

* `samplebytes` is 1, 2, or 3 (8/16/24-bit), and `nchan` is 1 through 8.
* `sample_frames` is the number of samples per channel in the frame.  It must
  be positive.
* `frame_bytes` is the compressed size the seektable reports for the frame,
  *including* the final 4-byte little-endian CRC.

`libttaR_streamdec_size()` returns the workspace size for `nchan`, or `0` for
an invalid channel count.  The size never exceeds `4096 * (nchan + 1)` bytes
and is the same for every frame of a given channel count.  The caller owns the
workspace and must align it to 16 bytes.  The decoder itself never allocates
heap memory; the workspace holds only per-channel Rice, predictor and filter
state plus one sample frame of `int32_t` per channel.

`libttaR_streamdec_init()` validates the parameters, the workspace size, and
all the arithmetic (it returns `-1` on any failure), then initializes the
state.  Calling it again on a live workspace resets it so the next frame can be
started.

### Feeding bytes and pulling PCM

Each `libttaR_streamdec_process()` call takes:

* `src` / `*bytes`: the compressed bytes that are readable now,
* `dst` / `*frames`: room for up to `*frames` interleaved sample frames,
* `final`: nonzero only when no more input will ever be supplied after this
  call.

On return, `*bytes` is the number of bytes actually consumed and `*frames` is
the number of sample frames actually written.  Each frame is `nchan`
interleaved `int32_t` samples.  Passing `src == NULL` with `*bytes == 0` is
allowed (output-only drain), and `dst == NULL` with `*frames == 0` is allowed
(input-only advance).  Supplying a NULL pointer for a non-zero count is an
error.

Return values:

| value | meaning |
|-------|---------|
| `0` | made progress, or still waiting for input bytes or output capacity |
| `1` | the frame's audio is delivered and its CRC passed |
| `-1` | bad parameter (the decoder state is not advanced) |
| `-2` | `final` input was consumed but the frame is incomplete |
| `-3` | the frame CRC does not match |
| `-4` | malformed bitstream, bad unary code, or a declared length mismatch |

The input may be split at *any* byte boundary: inside a unary code, inside the
binary remainder, between channels of a group, or in the middle of the
trailing CRC.  A partially-read unary code is carried in a counter, not in a
buffer, so even a unary longer than the entire workspace is fine.  A channel's
Rice, predictor and filter state are updated only after that channel's whole
symbol is read, so the next symbol of that channel always continues from the
right history.  Inverse channel decorrelation and output happen only once a
whole sample frame (`nchan` symbols) is available; nothing larger than one
sample frame is ever buffered.

The CRC covers the compressed payload only.  Every payload byte is fed into
the running CRC as it enters the bit cache, and the trailing 4 bytes are used
only as the expected value.  When the declared samples are exhausted, the
remaining payload bytes and up to 7 zero padding bits in the last byte must
match exactly; the decoder never treats the CRC or the next frame as residual
data.  Bytes that belong to the following frame are left unconsumed (the
decoder stops at `frame_bytes`).

### Verification, back pressure and terminal states

PCM may be delivered *before* the CRC has been read.  Only a return value of
`1` certifies the frame; a caller that needs verified audio must hold (or mark
invalid) the samples delivered by a call that eventually returns a negative
value.  A negative return does not retract PCM already written, and the
decoder must be re-initialized before another frame is attempted.

Running out of output capacity is never reported as truncation: the decoder
keeps the completed sample frame internally and the caller can drain it with
subsequent `src == NULL, *bytes == 0` calls.  A call that has both input and
output space always makes progress.

`final = 1` locks "no more input" only once the whole supplied input has been
consumed.  If the frame is still short at that point the decoder returns `-2`
(and, on the next call, keeps returning it).  After a frame completes, further
calls return `1` with both counts zero; after a bitstream error, further calls
return the same error with both counts zero.

See `examples/streamdec_example.c` for a self-contained round trip that
encodes with libttaR and decodes the frame in awkward fragments.


## About

This project started as a fork of ttaenc 3.4.1-linux (2007).
Any code from ttaenc was put into the library, and the CLI program was
completely rewritten.

The library prioritizes speed.
So, its memory usage is a bit bloated to avoid some bounds checks.
(The safer alternative is maybe 10-20% slower.)

The library is just a collection of functions for reading from one buffer and
writing to another with some support functions to calculate values.
It does not allocate, print, nor make any other syscall.
(Though it can abort, but should not, if NDEBUG is not defined.)
It does not even need libc as long as your compiler
has a builtin memset and memmove (both, ie, not gcc).
So fairly bare-bones as far as codec libraries go.

The two codec functions are reentrant, so you can code as few samples at a
time with buffers as small as you want (with some padding).
ttaR (CLI util) 1.0 did this, but I replaced it with doing the whole frame at
once in 1.1 because it's a little faster and it simplified the source a bit.
