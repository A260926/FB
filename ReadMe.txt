- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
The Apple Lossless Format
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

Apple Lossless supports the following features. Not all of these are implemented in alacconvert, though they are in the codec code provided.

1. Bit depths 16, 20, 24 and 32 bits.
2. Any arbitrary integer sample rate from 1 to 384,000 Hz. In theory rates up to 4,294,967,295 (2^32 - 1) Hz could be supported.
3. From one to eight channels are supported. Channel orders for the supported formats are described as:
	Num Chan	Order
	1 		mono
	2 		stereo (Left, Right)
	3 		MPEG 3.0 B (Center, Left, Right)
	4 		MPEG 4.0 B (Center, Left, Right, Center Surround)
	5 		MPEG 5.0 D (Center, Left, Right, Left Surround, Right Surround)
	6 		MPEG 5.1 D (Center, Left, Right, Left Surround, Right Surround, Low Frequency Effects)
	7 		Apple AAC 6.1 (Center, Left, Right, Left Surround, Right Surround, Center Surround, Low Frequency Effects)
	8 		MPEG 7.1 B (Center, Left Center, Right Center, Left, Right, Left Surround, Right Surround,  Low Frequency Effects)
4. Packet size defaults to 4096 sample frames of audio per packet. Other packet sizes are certainly possible. However, non-default packet sizes are not guaranteed to work properly on all hardware devices that support Apple Lossless. Packets above 16,384 sample frames are not supported.



- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
This package contains the sources for the Apple Lossless (ALAC) encoder and decoder.

The "codec" directory contains all the sources necessary for a functioning codec. Also includes is a makefile that will build libalac.a on a UNIX/Linux machine.

- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
ALACconvert

The convert-utility directory contains sources to build alacconvert which is a simple utility that demonstrates how to use the included ALAC encoder and decoder.

alacconvert supports the following formats:

1. 16- or 24-bit mono or stereo .wav files where the data is little endian integer. Extended WAVE format chunks are not handled.
2. 16- or 24-bit mono or stereo .caf (Core Audio Format) files as well as certain multi-channel configurations where the data is big or little endian integer. It does no channel order manipulation.
3. ALAC .caf files.

Three project are provided to build a command line utility called alacconvert that converts cpm data to ALAC or vice versa. A Mac OS X Xcode project, A Windows Visual Studio project, and a generic UNIX/Linux make file.

Note: When building on Windows, if you are using a version of Visual Studio before Visual Studio 2010, <stdint.h> is not installed. You will need to acquire this file on your own. It can be put in the same directory as the project.

- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Channel projection (--project)

alacconvert can keep only selected channels of a multi-channel ALAC/CAF
recording and/or change their order, writing a new ALAC/CAF file:

    alacconvert --project=<slots> <input caf file> <output caf file>

<slots> is a comma separated list of zero based input channel slots, given in
the order they should appear in the output.  For example, for a six-channel
C/L/R/Ls/Rs/LFE recording, "--project=1,2" produces a two-channel L/R file,
and "--project=0,3,4,1,2,5" reorders the original six-channel elements without
re-encoding them.

Properties of projection mode:

1. The list holds 1 to 8 distinct slots, and every slot must exist in the
   input.  Slots are the actual decoded channel slots of the input.
2. Audio frames are neither truncated nor resampled.  Nothing is decoded and
   re-encoded wholesale: only the packet directory and one packet of PCM and
   bitstream are held in memory at a time.
3. The output element order follows the ALAC encoder's channel map for the
   output channel count.  A source element whose complete, contiguous channel
   set is kept in its original order and with the same SCE/CPE/LFE type is
   copied bit for bit; only its 4-bit element_instance_tag is renumbered
   (per type, from zero, in output element order).  All other output elements
   are encoded from the real PCM of the packet.
4. The new packet contains exactly one ID_END followed by zero padding; source
   END tags and byte padding are never copied between elements.
5. Projection mode only accepts ALAC input in a CAF container and only writes
   ALAC/CAF output.  The two-filename encode/decode forms are unchanged.
6. Input support: built-in layouts with 3 to 8 channels, 16 or 24 bit, 4096
   frames per packet, full packets except a short final packet, zero priming
   and remainder frames, and a file smaller than 2 GiB.  Elements must be
   SCE, CPE, LFE or END; compressed and uncompressed elements are both
   accepted.  Channel roles come from the CAF 'chan' chunk's built-in tag,
   falling back to the ALAC magic cookie's built-in tag; if both are present
   they must agree and match the desc and cookie channel counts.  A legacy
   cookie 'chan' tag written in host byte order is accepted as the same
   built-in tag; new output always writes big-endian fields.
7. Output desc, ALACSpecificConfig and kuki numbers match the actual output
   packets (channels, maxFrameBytes and chan).  Channel roles are inherited
   from the list.  Standard layouts use the built-in tag in kuki and in the
   CAF 'chan' chunk; other layouts write CAF channel descriptions (Core Audio
   labels L=1, R=2, C=3, LFE=4, Ls=5, Rs=6, Lc=7, Rc=8, Cs=9, zero flags and
   coordinates) and omit the optional kuki 'chan' atom.  The pakt byte sizes
   come from the real output packets and frame counts stay inherited; a short
   final packet is never padded to a full one.
8. Invalid lists, inconsistent layouts, packet frame-count mismatches and
   unsupported elements exit non-zero without publishing a complete output.
   A failure mid-run removes the incomplete output it created.  Using the
   input path as the output is rejected, and the container and list are fully
   checked before any normal output is written.

The tests for this feature live in tests/project_channels.cpp.  They build
their own multichannel ALAC/CAF inputs with the real encoder (plus one
independently hand-built uncompressed multi-element packet), run the
projection path, and verify the result with an independent CAF parser and an
independent ALAC element walker.  Run them with:

    bash task_support/run.sh target



