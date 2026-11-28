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


## Streaming single-frame decoder

`libttaR_tta_decode()` needs a safety margin of readable bytes after every
slice, so it cannot be fed raw compressed fragments directly. The streaming
API decodes one compressed TTA frame straight from arbitrary byte fragments
with no margin and no internal heap allocation:

```
size_t      wsz = libttaR_streamdec_size(nchan);
void       *ws  = aligned_alloc(16, wsz);
libttaR_streamdec_init(ws, wsz, samplebytes, nchan, sample_frames, frame_bytes);

int rv = libttaR_streamdec_process(ws, src, &bytes, dst, &frames, final);
```

The workspace is caller supplied, 16-byte aligned, and sized by
`libttaR_streamdec_size()` (0 for an unsupported channel count). Its size is
independent of `frame_bytes`, `sample_frames`, and the length of any code
word. `libttaR_streamdec_init()` validates the parameters, lengths, and
multiplication overflows and may also be reused to reset an existing
workspace for another frame.

Input may be cut at any point: in the middle of a unary or binary remainder,
inside a channel group, or in the middle of the trailing CRC. Half a code
word is kept in the workspace; a channel's Rice, predictor, and filter state
is committed only after a whole symbol arrives, and a complete interleaved
sample frame is emitted only after every channel of the group has arrived.
The next symbol continues with the correct per-channel history.

### Verification status of delivered PCM

Audio is produced as soon as it can be decoded, which may be *before* the
four trailing CRC bytes have arrived. Such PCM is therefore **provisional and
not yet verified**. Only a return value of `1` means the whole frame is
complete and its CRC matched; treat the frame as verified only then. On any
error, PCM already reported is not withdrawn, but the workspace enters a
sticky error state and must be reinitialized before decoding another frame.

### Backpressure and end-of-input

`bytes` and `frames` are capacities on the way in and actual counts on the way
out. When the output capacity (`*frames`) is zero, `dst` may be `NULL`. A
decoder that fills its output capacity pauses and returns `0`; this is
backpressure, never truncation. At most one decoded sample frame is retained
in the workspace, so it can later be drained with a zero-input call
(`bytes == 0`).

`final == 1` means no bytes will follow the current slice. End-of-input is
locked only when the entire offered slice is consumed; an output stall with
bytes still pending does not lock it. Once locked, a stall that still needs
input becomes `-2` (frame incomplete), whereas an output stall remains
drainable. After completion, repeated calls return `1` with both counters
zero; a sticky stream error repeats its code with both counters zero; an
illegal call returns `-1`, leaves the state untouched, and zeroes both
counters. Only bytes inside the declared `frame_bytes` are consumed, so a
buffer that also contains the next frame never reads past the boundary.

