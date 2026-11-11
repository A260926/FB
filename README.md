# Opusfile

[![GitLab Pipeline Status](https://gitlab.xiph.org/xiph/opusfile/badges/main/pipeline.svg)](https://gitlab.xiph.org/xiph/opusfile/commits/main)
[![GitHub CI](https://github.com/xiph/opusfile/actions/workflows/build.yml/badge.svg)](https://github.com/xiph/opusfile/actions/workflows/build.yml)

The opusfile and opusurl libraries provide a high-level API for
decoding and seeking within .opus files on disk or over http(s).

opusfile depends on libopus and libogg.
opusurl depends on opusfile and openssl.

The library is functional, but there are likely issues
we didn't find in our own testing. Please give feedback
in #opus on irc.libera.chat or at opus@xiph.org.

Programming documentation is available in tree and online at
https://opus-codec.org/docs/

## Chained-stream gain regression

`tests/chain_gain.c` is a self-contained regression test for playback gain in
chained Ogg Opus streams (header gain, `R128_TRACK_GAIN`, `R128_ALBUM_GAIN` and
the caller-supplied offset, including pre-skip and EOS granule end-trim). It
generates its own three-link fixture (mono, stereo, mono; different serial
numbers, pre-skip, end-trim, header gains and R128 tags), encodes it with
libopus and muxes it with libogg, and keeps the encoded packets for an
independent zero-gain libopus decoder that serves as the reference; the gain is
then applied by hand per link. libopusfile output is never the only reference.

Run the test plus its mutation checks (a pristine build must pass, and two
temporary copies with the header gain or EOS end-trim disabled must be caught)
with:

    bash tests/check_chain_gain.sh

It needs the fixed dependencies libopus 1.6.1 and libogg 1.3.6 visible to
pkg-config, uses no audio device and never modifies the source tree (the
mutations live in a temporary directory). A clean run prints `CHAIN_GAIN_OK`,
`DETECTED_HEADER_GAIN` and `DETECTED_END_TRIM`; the small HTTP-free build smoke
test is still available via `bash harness/run.sh`.

Tolerances used by the test:

- straight-through float: relative 2e-5 plus absolute 2e-6. Both decoders use
  the same libopus build on the same packets, so the only difference is one
  float32 gain multiply (at most ~6e-8 relative per operation); the bound
  leaves several hundred ULP of slack.
- after a seek: relative 3e-2 plus absolute 3e-4, because libopusfile decodes
  pre-roll after a decoder reset and post-seek samples are not required to be
  bit-identical to a straight decode.
- 16-bit path with dither disabled: one quantization unit (1 LSB), since the
  output is a round-to-nearest of `32768*float` on low-amplitude, non-clipping
  signals.
