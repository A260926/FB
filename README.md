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



DAB STREAMING MODE
------------------

By default, DAB ScF-CRC handling is manual: the frontend must encode exactly
one frame (1152 samples per channel) at a time and call
`twolame_set_DAB_scf_crc()` to write the CRC it just computed into the tail
of the *previous* frame. With DAB streaming mode the library does this
itself: the caller feeds PCM in arbitrarily sized blocks through the normal
`twolame_encode_buffer()` / `twolame_encode_buffer_interleaved()` calls and
receives finished MP2 frames with the backfill already applied.

Setup order (all parameters are fixed before initialisation):

    twolame_options *opts = twolame_init();
    twolame_set_num_channels(opts, 2);
    twolame_set_in_samplerate(opts, 48000);
    twolame_set_out_samplerate(opts, 48000);   /* must match the input rate */
    twolame_set_bitrate(opts, 192);
    twolame_set_mode(opts, TWOLAME_STEREO);    /* or TWOLAME_MONO */
    twolame_set_DAB(opts, 1);
    twolame_set_DAB_crc_length(opts, 2);       /* 2 or 4 bytes */
    twolame_set_DAB_xpad_length(opts, 0);
    twolame_set_num_ancillary_bits(opts, 8 * (2 + 2));  /* CRC + 2 F-PAD bytes */
    twolame_set_DAB_streaming(opts, 1);
    twolame_init_params(opts);

`twolame_set_DAB_streaming()` only accepts 0 or 1 and is locked in once
`twolame_init_params()` has succeeded; invalid calls return -1 and leave the
previous setting untouched. `twolame_init_params()` fails with -1 unless the
configuration is MPEG-1 or MPEG-2 CBR (no VBR, no freeformat) with mono or
stereo mode matching the input channel count, equal input/output samplerates,
DAB enabled with a 2 or 4 byte CRC, zero XPAD, no energy levels, and a
reserved ancillary bit count that is a multiple of 8 with room for the
ScF-CRC plus 2 bytes of F-PAD. Both MPEG header CRC (error protection) and
both padding settings are supported. The float32 encode entry points return
-1 in this mode.

Latency and buffering: a frame is only handed out once the *next* frame has
been encoded, because that next frame's ScF-CRC is written into it. The
output therefore lags the input by one frame; the library internally keeps
at most one complete MP2 frame plus less than 1152 samples of unencoded PCM,
no matter how large the input blocks are.

Output space: give each encode call at least `7200 + 2 * num_samples` bytes
and each flush call at least 7200 bytes. The returned bytes are complete
MP2 frames in stream order and belong to the caller - the library never
touches memory it has already handed out, so the buffer may be reused or
overwritten as soon as the bytes have been copied away.

End of stream: call `twolame_encode_flush()` as usual. Any remaining PCM is
zero-padded into a real final frame whose CRC still backfills the previous
frame; the final frame itself carries zero bytes in its ScF-CRC positions
(the service-specific end-of-stream marker) and is then handed out. A flush
may therefore return two frames at once, and it returns the held last frame
even when no PCM is left. For N > 0 input samples per channel the stream
contains exactly ceil(N / 1152) frames; an empty stream produces none. Once
flushed, further flushes and zero-length encode calls return 0, and feeding
new PCM returns -1.

See `examples/dab_stream.c` for a complete program.



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

