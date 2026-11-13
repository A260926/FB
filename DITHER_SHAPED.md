# Shaped TPDF Dither (`SFD_TPDF_SHAPED`)

This document describes the reproducible triangular probability density
function (TPDF) dither with first order error feedback added to the write
path. It replaces the previous behaviour where the dither interface copied
samples unchanged, so low level floating point audio exported to 16 bit or
24 bit PCM was truncated to silence.

## Scope and limits

The new mode is selected with the existing `SFC_SET_DITHER_ON_WRITE` command
and a new `SF_DITHER_INFO.type` value, `SFD_TPDF_SHAPED`. The layout of
`SF_DITHER_INFO` is unchanged; only new constant values are introduced.

Supported:

- files opened `SFM_WRITE`,
- PCM subtypes `SF_FORMAT_PCM_16` and `SF_FORMAT_PCM_24`, in any container
  that carries them (the tests cover WAV and AIFF),
- input through `sf_writef_float`, `sf_write_float`, `sf_writef_double` and
  `sf_write_double`, with float and double normalization both left at their
  default enabled state.

Everything else is rejected explicitly rather than silently falling back to
the copy path:

- any non-PCM16/PCM24 subtype (PCM32, float, 8 bit, compressed, ...),
- a dither `level` other than exactly `1.0` (the `name` field is ignored),
- float or double normalization switched off,
- enabling the mode or changing the seed after the first audio sample,
- any `sf_seek` while the mode is active.

`SFD_NO_DITHER`, the legacy `SFD_WHITE`/`SFD_TRIANGULAR_PDF` types and all
read interfaces keep their original behaviour.

## Calling sequence

```c
#include <sndfile.h>

SF_INFO info = {0} ;
info.samplerate = 48000 ;
info.channels   = 2 ;
info.format     = SF_FORMAT_WAV | SF_FORMAT_PCM_16 ;

SNDFILE *f = sf_open ("quiet.wav", SFM_WRITE, &info) ;

/* Optional: fix the PRNG seed. Data points to a uint64_t and datasize must
** be exactly sizeof (uint64_t). The command is only accepted before the
** first audio sample is written. */
uint64_t seed = 0x2545F4914F6CDD1DULL ;
if (sf_command (f, SFC_SET_DITHER_SEED, &seed, sizeof (seed)) != 0)
    /* handle error */ ;

SF_DITHER_INFO dither = {0} ;
dither.type  = SFD_TPDF_SHAPED ;
dither.level = 1.0 ;                 /* the only accepted level */
if (sf_command (f, SFC_SET_DITHER_ON_WRITE, &dither, sizeof (dither)) != 0)
    /* handle error */ ;

sf_writef_double (f, samples, frames) ;
sf_close (f) ;
```

Commands follow the existing convention: they return `0` on success and a
non-zero error on failure. If no seed is supplied the documented fixed
default seed is used, so an export is deterministic even without
`SFC_SET_DITHER_SEED`:

```
TPDF_DEFAULT_SEED = 0x9E3779B97F4A7C15
```

## Random number generator

The generator is **SplitMix64** (Sebastiano Vigna), a pure integer PRNG with
one 64 bit state word. It needs no global state and no wall clock entropy.

Constants:

```
GAMMA = 0x9E3779B97F4A7C15
```

One draw from a state `s`:

```
s     = s + GAMMA
z     = s
z     = (z xor (z >> 30)) * 0xBF58476D1CE4E5B9
z     = (z xor (z >> 27)) * 0x94D049BB133111EB
word  = z xor (z >> 31)
```

### Seed expansion and per-channel streams

Every 64 bit seed is legal, including zero, because the first draw always
adds `GAMMA` before any mixing. Each channel gets an independent starting
state derived from the configured seed `S` and the zero-based channel index
`c`:

```
mixed = S xor (GAMMA * (c + 1))
state_c = SplitMix64 (mixed)      /* one SplitMix64 draw */
```

Channels never borrow state or error terms from each other. Changing the
signal on one channel cannot alter the output of another.

A single sample consumes exactly two PRNG words `ru`, `rv`, mapped to
independent uniform variates in `[0, 1)` using the top 53 bits:

```
u = (ru >> 11) * 2^-53
v = (rv >> 11) * 2^-53
```

`u - v` is therefore triangular distributed over `(-1, 1)`.

## Quantization and error feedback

Let `B` be the target bit width (16 or 24). Quantization works in units of
one target LSB. Each channel keeps its own error term `e`, initialized to
zero.

For every finite input sample `x` (the normalized float/double value):

1. Scale to LSB units: `X = x * 2^(B-1)`.
2. Add the stored feedback: `z = X + e`.
3. Add the triangular dither and round to the nearest integer with half
   values rounded away from zero:
   `q = round_half_away_from_zero (z + u - v)`.
4. If `q` is outside the signed `B` bit range `[-2^(B-1), 2^(B-1) - 1]`,
   saturate it to the nearest boundary and set `e = 0`.
5. Otherwise store `e = z - q`. The dither noise `u - v` is deliberately
   **not** part of the feedback error.
6. Write the integer `q` directly. It is shifted into the left aligned 32 bit
   representation that the existing PCM codec expects (`q << 16` for 16 bit,
   `q << 8` for 24 bit); the codec then emits the on-disk two byte or three
   byte sample. There is no second floating point normalization or
   quantization pass.

The left aligned `int` used internally must not be confused with the actual
three bytes stored for PCM24.

Non-finite inputs are handled deterministically: `+Inf` saturates to the
positive boundary, `-Inf` to the negative boundary, `NaN` maps to zero, and in
all three cases the channel error is cleared as with saturation.

## Reconfiguration and restart rules

- **Re-arming the active mode** (another `SFC_SET_DITHER_ON_WRITE` with
  `SFD_TPDF_SHAPED`) is a no-op. It does not reset the PRNG streams or error
  terms, does not re-wrap the write callbacks, and does not replay the start
  of the source buffer.
- **Disabling** with `SFD_NO_DITHER` restores the codec level write
  callbacks immediately; subsequent samples take the plain conversion path.
- **Re-enabling** `SFD_TPDF_SHAPED` after it was disabled restarts every
  channel from `e = 0` and from the stream derived from the originally
  configured seed (as if a fresh file had been started at that point).
- **`SFC_SET_DITHER_SEED`** is accepted only on an `SFM_WRITE` file, with
  `data != NULL` and `datasize == sizeof (uint64_t)`, and only before the
  first audio sample. On success it returns `0`; any violation returns a
  non-zero error and leaves the existing configuration untouched.
- **Integer writes** (`sf_writef_int`/`sf_writef_short` and the non-frame
  variants) keep the legacy rule while the mode is active: the already
  quantized samples pass through unchanged. They consume no PRNG words and
  clear the error term of every channel, so the following floating point
  samples continue the same random streams with `e = 0`.
- **Seeking** is a sequential-write feature only. Any `sf_seek` while the
  mode is active is rejected and changes no state.

## Streaming guarantees

For an exactly representable input signal, the resulting PCM is independent
of how the samples are delivered:

- one large block versus one frame at a time,
- chunk sizes that straddle the internal fixed-size work buffer,
- `float` versus `double` entry points and mixed calls within one stream,
- two open files written in alternation with the same seed.

The implementation keeps only a fixed bounded work buffer sized by the
channel count; it never retains the whole recording and never modifies the
caller's input array. The same seed always reproduces byte-identical PCM.

## Measured comparison

The numbers below are produced by `examples/dither_shaped_example.c` and the
measurement helper, with seed `0x2545F4914F6CDD1D`, on a constant signal of
`+0.25` LSB (40 samples, mono):

```
bits=16 frac=0.250LSB nonzero=20 pos=15 neg=5 mean=0.2500
bits=24 frac=0.250LSB nonzero=20 pos=15 neg=5 mean=0.2500
bits=16 frac=0.000LSB nonzero=13 pos=6  neg=7 mean=0.0000
```

The legacy copy path writes all zero samples for the same sub-LSB input (as
the `DITHER_BASELINE` check demonstrates). With shaped TPDF dither the quiet
signal survives as sparse +/-1 LSB activity whose long term mean equals the
input level, while a zero input dithers symmetrically around zero. Because
the math runs in LSB units, 16 bit and 24 bit exports of the same
fraction-of-LSB signal produce identical code sequences.

## Building

No external codec libraries and no MPEG support are required. Only CMake, a
C compiler and the system math library are needed:

```
cmake -S . -B build -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF \
      -DENABLE_EXTERNAL_LIBS=OFF -DENABLE_MPEG=OFF -DBUILD_PROGRAMS=OFF \
      -DBUILD_EXAMPLES=OFF -DENABLE_CPACK=OFF -DENABLE_PACKAGE_CONFIG=OFF
cmake --build build --target sndfile
```

The example program is `examples/dither_shaped_example.c`.
