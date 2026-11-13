TwoLAME
=======

[![Build Status](https://travis-ci.com/njh/twolame.svg?branch=main)](https://travis-ci.com/njh/twolame)

Based on tooLAME by Michael Cheng

All changes to the ISO source are licensed under the LGPL
(see COPYING for details)

    TwoLAME is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    TwoLAME is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with TwoLAME; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA


INTRODUCTION
------------

TwoLAME is an optimized MPEG Audio Layer 2 (MP2) encoder.
It is based heavily on:

* tooLAME by Michael Cheng
* the ISO dist10 code
* improvement to algorithms as part of the LAME project (lame.sf.net)
* other contributors (see AUTHORS)

TwoLAME should be able to be used as a drop-in replacement for
LAME (a MPEG Layer 3 encoder). The frontend takes very similar
command line options to LAME, and the backend library has a very
similar API to LAME.

For the latest version of TwoLAME, visit the project homepage:
http://www.twolame.org/


MPEG Audio Layer 2 (MP2)
------------------------
(taken from Wikipedia article on MP2)

MP2 (sometimes incorrectly named Musicam) is a short form of MPEG Audio Layer II,
and it is also used as a file extension for files containing audio data of this
type. While it has largely been superseded by MP3 for PC and Internet applications,
it remains a dominant standard for audio broadcasting as part of the DAB digital
radio and DVB digital television standards. It is also used internally within the
radio industry, for example in NPR's PRSS Content Depot programming distribution
system.


INSTALLATION
------------

Standard automake process:

    ./configure
    make
    make install


DAB ScF-CRC streaming
---------------------

TwoLAME can calculate the Eureka 147 DAB ScF-CRC for every frame it encodes.
With the classic interface the caller has to feed exactly 1152 samples per
channel, keep at least two frames in memory and call
`twolame_set_DAB_scf_crc()` to write the CRC calculated for frame *N* into
frame *N-1*.  That does not work when the caller hands over audio in blocks
that span several frames.

`twolame_set_DAB_streaming(options, 1)` moves that bookkeeping into the
library.  The caller can then feed PCM in any chunk size to
`twolame_encode_buffer()` or `twolame_encode_buffer_interleaved()`
(`num_samples` is still per channel) and receives complete MP2 frames with
the CRC of each frame already written into its predecessor.  The two float32
entry points are not supported in this mode and return -1 without consuming
PCM.

Setting order:

1. Configure the encoder as usual (channels, sample rates, bitrate, mode,
   padding, error protection, ...).
2. Enable DAB and set the CRC length:
   `twolame_set_DAB(options, 1)` and
   `twolame_set_DAB_crc_length(options, 2)` (or 4).
3. Reserve ancillary bits with
   `twolame_set_num_ancillary_bits()`.  The count must be a whole number of
   bytes and must cover the CRC plus the 2-byte F-PAD.
4. Enable the mode with `twolame_set_DAB_streaming(options, 1)`.
5. Call `twolame_init_params()`.

`twolame_set_DAB_streaming()` only accepts 0 or 1, and the setting is frozen
once `twolame_init_params()` has succeeded.  Supported configurations are
MPEG-1 or MPEG-2 CBR with mono or stereo mode, equal input and output sample
rates, and a channel count matching the mode.  DAB must be enabled, the CRC
length must be 2 or 4 bytes, XPAD length must be 0, and VBR, freeformat and
energy levels must be off.  If any condition is not met,
`twolame_init_params()` returns -1 rather than silently choosing a different
mode.

Latency: the library holds back one encoded frame because a frame's CRC can
only be written into the previous frame.  The first frame of output therefore
appears only after a second frame has been encoded, and a frame that is still
pending is handed out by `twolame_encode_flush()`.  Each instance keeps its
own audio history and pending frame, so independent encoders may be
interleaved.

Output space: provide at least `7200 + 2 * num_samples` bytes for each
`twolame_encode_*()` call and at least 7200 bytes for
`twolame_encode_flush()`.  The returned data is a contiguous sequence of
complete MP2 frames; the caller owns it and may reuse or overwrite the output
buffer as soon as the call returns.

Ending: call `twolame_encode_flush()`.  PCM left over (fewer than 1152 samples
per channel) is zero padded, as in the original encoder, and encoded as a real
final frame whose CRC is written into the previously held frame.  The final
frame has no successor, so its own ScF-CRC field is left all zero as an
end-of-stream marker.  The first flush may return two frames; if there is no
partial PCM it still returns the held frame.  After a successful flush,
repeated flushes and zero-length encodes return 0, and non-empty PCM is
rejected with -1.  A zero-length encode before flush does nothing at all.

When the mode is disabled every existing entry point, the manual
`twolame_set_DAB_scf_crc()` patching flow and the produced bytes behave
exactly as before.

An example program is in `examples/dab_stream.c`; the test suite lives in
`tests/dab_stream.c`.



REFERENCE PAPERS
----------------

(Specifically Layer II Papers)

Kumar, M & Zubair, M., A high performance software implementation of mpeg audio
encoder, 1996, ICASSP Conf Proceedings (I think)

Fischer, K.A., Calculation of the psychoacoustic simultaneous masked threshold
based on MPEG/Audio Encoder Model One, ICSI Technical Report, 1997
ftp://ftp.icsi.berkeley.edu/pub/real/kyrill/PsychoMpegOne.tar.Z

Hyen-O et al, New Implementation techniques of a real-time mpeg-2 audio encoding
system. p2287, ICASSP 99.

Imai, T., et al, MPEG-1 Audio real-time encoding system, IEEE Trans on Consumer
Electronics, v44, n3 1998. p888

Teh, D., et al, Efficient bit allocation algorithm for ISO/MPEG audio encoder,
Electronics Letters, v34, n8, p721

Murphy, C & Anandakumar, K, Real-time MPEG-1 audio coding and decoding on a DSP
Chip, IEEE Trans on Consumer Electronics, v43, n1, 1997 p40

Hans, M & Bhaskaran, V., A compliant MPEG-1 layer II audio decoder with 16-B
arithmetic operations, IEEE Signal Proc Letters v4 n5 1997 p121

