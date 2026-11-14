# Ogg

[![GitLab Pipeline Status](https://gitlab.xiph.org/xiph/ogg/badges/main/pipeline.svg)](https://gitlab.xiph.org/xiph/ogg/pipelines)
[![GitHub CI Autotools build](https://github.com/xiph/ogg/actions/workflows/autotools.yml/badge.svg)](https://github.com/xiph/ogg/actions/workflows/autotools.yml)
[![GitHub CI CMake build](https://github.com/xiph/ogg/actions/workflows/cmake.yml/badge.svg)](https://github.com/xiph/ogg/actions/workflows/cmake.yml)
[![AppVeyor Build Status](https://ci.appveyor.com/api/projects/status/github/xiph/ogg?branch=master&svg=true)](https://ci.appveyor.com/project/rillian/ogg)

Ogg project codecs use the Ogg bitstream format to arrange the raw,
compressed bitstream into a more robust, useful form. For example,
the Ogg bitstream makes seeking, time stamping and error recovery
possible, as well as mixing several separate, concurrent media
streams into a single physical bitstream.

## What's here ##
This source distribution includes libogg and nothing else. Other modules
(eg, the modules libvorbis, vorbis-tools for the Vorbis music codec,
libtheora for the Theora video codec) contain the codec libraries for
use with Ogg bitstreams.

Directory:

- `src` The source for libogg, a BSD-license implementation of the public domain Ogg bitstream format

- `include` Library API headers

- `doc` Ogg specification and libogg API documents

- `win32` Win32 projects and build automation

## Contact ##

The Ogg homepage is located at https://www.xiph.org/ogg/ .
Up to date technical documents, contact information, source code and
pre-built utilities may be found there.

## Building ##

#### Building from tarball distributions ####

    ./configure
    make

and optionally (as root):

    make install

This will install the Ogg libraries (static and shared) into
/usr/local/lib, includes into /usr/local/include and API
documentation into /usr/local/share/doc.

#### Building from repository source ####

A standard svn build should consist of nothing more than:

    ./autogen.sh
    ./configure
    make

and as root if desired :

    make install

#### Building on Windows ####

Use the project file in the win32 directory. It should compile out of the box.

#### Cross-compiling from Linux to Windows ####

It is also possible to cross compile from Linux to windows using the MinGW
cross tools and even to run the test suite under Wine, the Linux/*nix
windows emulator.

On Debian and Ubuntu systems, these cross compiler tools can be installed
by doing:

    sudo apt-get mingw32 mingw32-binutils mingw32-runtime wine

Once these tools are installed its possible to compile and test by
executing the following commands, or something similar depending on
your system:

    ./configure --host=i586-mingw32msvc --target=i586-mingw32msvc --build=i586-linux
    make
    make check

(Build instructions for Ogg codecs such as vorbis are similar and may
be found in those source modules' README files)

## Building with CMake ##

Ogg supports building using [CMake](http://www.cmake.org/). CMake is a meta build system that generates native projects for each platform.
To generate projects just run cmake replacing `YOUR-PROJECT-GENERATOR` with a proper generator from a list [here](http://www.cmake.org/cmake/help/v3.2/manual/cmake-generators.7.html):

    mkdir build
    cd build
    cmake -G YOUR-PROJECT-GENERATOR ..

Note that by default cmake generates projects that will build static libraries.
To generate projects that will build dynamic library use `BUILD_SHARED_LIBS` option like this:

    cmake -G YOUR-PROJECT-GENERATOR -DBUILD_SHARED_LIBS=1 ..

After projects are generated use them as usual

#### Building on Windows ####

Use proper generator for your Visual Studio version like:

    cmake -G "Visual Studio 12 2013" ..

#### Building on Mac OS X ####

Use Xcode generator. To build framework run:

    cmake -G Xcode -DBUILD_FRAMEWORK=1 ..

#### Building on Linux ####

Use Makefile generator which is default one.

    cmake ..
    make

## Testing ##

This package includes a collection of automated tests.
Running them is not part of building nor installation but optional.

### Unix-like System or MinGW ###

If build under automake:

    make check

If build under CMake:

    make test

or:

    ctest

### Windows with MSBuild ###

If build with configuration type "Debug", then:

    ctest -C Debug

If build with configuration type "Release", then:

    ctest -C Release

## Receive-side recovery regression tests ##

The library's own self-test (`test_framing`, built from `src/framing.c`)
already round-trips encoder-produced pages and covers several lost-page
branches.  On top of that, `tests/recovery_cases.c` adds an independent
receive-side regression suite for the case a low-latency audio link
occasionally corrupts bytes: after the sync layer drops a bad page, the
packet layer must not glue the leftover fragments onto the audio that
follows.  Production behavior is unchanged; no new player is added.

What the suite adds beyond the existing self-test:

- Independent reference models: input pages are assembled by hand in the
  test file (little-endian header fields, hand-written lacing values
  including 255 runs, zero terminators and zero-length packets, CRC-32
  computed bitwise from polynomial 0x04c11db7).  `ogg_stream_packetin()`/
  `ogg_stream_flush()`/`ogg_page_checksum_set()` are not used to build
  the reference input, and no second decode pass of the library acts as
  the oracle; expected page bytes and packet/hole event sequences are
  stored as standalone models.  Packet bodies contain fake `OggS`
  capture patterns that must stay inert.
- Combination chains the old test does not exercise end to end:
  truncated capture markers and header-complete/body-missing inputs fed
  byte by byte, in header-crossing short chunks and as one block with
  identical event streams; CRC-bad pages adjacent to bogus `OggS` pages
  between good pages; `ogg_sync_pageseek()` skip counts vs. the single
  desync notification of `ogg_sync_pageout()` per garbage run; orphan
  suffixes spanning multiple 255 lacing values followed by zero-length
  and complete packets on a recovery page; granule ownership (only the
  last packet completed on a page) and packetno accounting across holes;
  `ogg_stream_packetpeek()` consuming a hole exactly once while repeated
  peeks (and NULL-output calls) on a packet do not consume; two
  interleaved serials with varied feed chunking and drain timing, a
  >65025-byte packet spanning pages, recovery after an adjacent corrupt
  page, and reset/replay with no stale half-packet or desync state.

### Running ###

    bash task_support/run.sh baseline   # library self-tests only
    bash task_support/run.sh target     # plus the recovery suite
    bash task_support/variants.sh       # suite + mutation checks

`variants.sh` first builds and runs the recovery suite against the
unmodified library, then repeats it against three temporary single-line
degradations of `src/framing.c` (accepting CRC-mismatched pages, not
skipping orphan suffixes, and `packetpeek` not advancing past holes).
Each degradation must be caught by the suite's `TASK_CHECK` assertions.

### Example recovery event sequence ###

For a stream where page 1 (the first 65025 bytes of a 70000-byte
packet) is lost and page 2 resumes with the orphan tail followed by a
zero-length packet and a complete 255-byte packet, the expected event
sequence is:

    packet  300 bytes  packetno=0 granule=1000 bos   (queued before loss)
    hole                                             (page 1 missing)
    packet  0 bytes    packetno=2 granule=-1         (zero-length)
    packet  255 bytes  packetno=3 granule=3000       (page 2 granule)
    packet  10 bytes   packetno=4 granule=4000 eos

The hole consumes packet sequence number 1; the orphan suffix is
dropped silently, producing neither a packet nor an extra hole.

## License ##

THIS FILE IS PART OF THE OggVorbis SOFTWARE CODEC SOURCE CODE.
USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS
GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE
IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.

THE OggVorbis SOURCE CODE IS COPYRIGHT (C) 1994-2019
by the Xiph.Org Foundation https://www.xiph.org/
