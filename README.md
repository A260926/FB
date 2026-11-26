# xvc codec [![AppVeyor Build Status](https://ci.appveyor.com/api/projects/status/github/divideon/xvc?branch=master&svg=true)](https://ci.appveyor.com/project/divideon/xvc) [![Travis Build Status](https://travis-ci.com/divideon/xvc.svg?branch=master)](https://travis-ci.com/divideon/xvc)

The xvc codec is a next-generation software-defined video compression format, built using
world-class compression technologies.

The xvc software is available under a dual-licensing scheme with an LGPL option and a
commercial option.

Visit [xvc.io](https://xvc.io) for more information.

The master branch represents the current official version of xvc. Development
of new versions of xvc happens in the dev branch.

## Building instructions

### Prerequisites

 1. [CMake](https://cmake.org) version 3.1 or higher.
 2. [Git](https://git-scm.com/).
 3. C++ compiler with C++11 support

The following C++11 compilers have been known to work:

 * Visual Studio 2015 or later
 * GCC 4.8 or later
 * Clang 3.3 or later

### Linux build steps

The following commands will checkout the project source code and create a
directory called 'build' where the compiler output will be placed.
CMake is then used for generating build files and compiling the xvc binaries.

    $ git clone https://github.com/divideon/xvc.git
    $ cd xvc
    $ mkdir build
    $ cd build
    $ cmake ..
    $ make

This will create xvc encoder and decoder binaries in the xvc/build/app/ folder.

### Windows build steps

The following commands will checkout the project source code and create a
directory called 'build' where the compiler output will be placed.
CMake is then used for generating build files and creating the Visual Studio
solution.

    $ git clone https://github.com/divideon/xvc.git
    $ cd xvc
    $ mkdir build
    $ cd build
    $ cmake -G "Visual Studio 14 2015 Win64" ..

The -G argument should be adjusted to match your version of Visual Studio and
the target architecture.
This will generate a Visual Studio solution called xvc.sln in the build folder.
After building the solution, the xvc encoder and decoder binaries will be found
in the xvc/build/app/Release/ folder.

## Testing the xvc codec

### Command line encoder and decoder

For generating xvc bitstreams the following command shows typical usage:

    $ xvcenc -input-file input.yuv -input-width 1920 -input-height 1080 \
        -framerate 30 -output-file mybitstream.xvc -qp 32

For decoding an xvc bitstream use the xvcdec application:

    $ xvcdec -bitstream-file mybitstream.xvc -output-file decoded.yuv

Both xvcenc and xvcdec supports reading and writing y4m output.

Pipes can be used together with e.g. ffmpeg to stream y4m input to encoder:

    $ ffmpeg -i movie.mkv -f yuv4mpegpipe - | xvcenc -input-file - \
        -qp 30 -output-file mybitstream.xvc

On the decoder, pipes can also be used to stream decoded output fo ffplay:

    $ xvcdec -bitstream-file mybitstream.xvc -output-file - | ffplay -i -

### Command line syntax

To show all available encoder arguments run:

	$ xvcenc -help

For decoder arguments run:

	$ xvcdec -help

## Per CTU row restart (row_restart, bitstream version 3.0)

By default a picture is coded as one continuous CABAC stream (bitstream
version 2.0). For line/row based processing, every 64 pixel CTU row can
instead be made an independent CABAC substream. The feature is disabled by
default and is enabled through the encoder parameter API:

```c
xvc_encoder_parameters *p = api->parameters_create();
api->parameters_set_default(p);          // p->row_restart == 0
xvc_encoder_set_row_restart(p, 1);       // XVC_ENC_OK
```

`xvc_encoder_set_row_restart(xvc_encoder_parameters*, int enabled)` returns
`XVC_ENC_OK` for `enabled` 0/1 and `XVC_ENC_INVALID_ARGUMENT` for a null
parameter pointer or any other value of `enabled`; rejected calls leave the
parameters unchanged. `parameters_check` also validates the field.

### Bitstream layout

Enabling row restart writes segment header version **3.0**. An unmodified
version 2 decoder rejects it because the major version is higher than 2; the
current decoder reads both version 2.0 and version 3.0 streams. The picture
header keeps its existing fields and byte alignment. Immediately after the
aligned picture header, a version 3.0 picture NAL contains:

- 4 bytes ASCII `XRST`
- `uint16_be row_count`
- `uint16_be reserved_zero`
- `row_count` entries of `uint32_be` nonzero CABAC byte lengths
- `row_count` byte aligned, independently terminated CABAC streams in
  top-to-bottom order, each coding exactly one CTU row
- the existing image level checksum after **all** substreams (when the normal
  checksum rules require one)

`row_count = ceil(coded_luma_height / 64)`. Each row covers the CTUs that
actually exist; CTUs at the right and bottom picture borders stay cropped as
in ordinary coding. There are no extra offsets, gaps or row ids: row offsets
are derived contiguously from the stored lengths.

### Coding and decoding rules

Every row starts from the picture initial QP and picture prediction type with
fresh CABAC probability models, a fresh arithmetic coder and reset RDO
fractional bit accounting, and finishes its own CABAC stream. No probability
states, arithmetic registers, rate state or QP predictors are copied from the
preceding row. Same picture intra reference samples, spatial motion candidates
and neighboring QP prediction never cross a CTU row border; in-row left
neighbors and earlier blocks remain available. Reference picture sampling and
temporal motion candidates keep their normal meaning because they address
other reference pictures, not the current picture's row state. Both CU trees
(luma and separate chroma dual tree) obey the same reset.

The decoder validates the complete directory (row count, every length,
boundaries and integer overflow) before decoding any row. Each row is then
decoded through a reader bounded to that row's exact byte range, so a row
cannot borrow bytes from the next row or from the trailing checksum, and it
must contain a legal CABAC termination. An internal order hook allows rows to
be scheduled top-to-bottom or bottom-to-top into distinct row areas; spatial
isolation makes both orders reconstruct identical pixels. Deblocking,
padding, checksum verification, output and DPB/reference commit happen only
after every row has reconstructed successfully. Short directories, zero
length rows, out of range lengths or bad terminations are rejected through the
public decode API without publishing a half reconstructed picture or making
it available as a reference. 8/10 bit, 4:2:0/4:4:4, I/P/B pictures and full
flush are supported.

### Compression tradeoff

Independent rows lose cross row context adaptation and prediction: the top
CTU edge is treated like a picture border, CABAC contexts are reinitialized
for every row and each row pays its own arithmetic termination and byte
alignment. This slightly increases bitrate (typically more noticeably at low
QP/high quality and for smooth content) in exchange for error robustness,
independent row scheduling and a direct entry point for row based or
parallel downstream processing.

### Example and tests

`examples/row_restart.cc` is a standalone consumer that generates five
128x128 8 bit I420 frames, performs real encoding and decoding and writes
`row-restart.xvc` and `row-restart.yuv`, printing
`ROW_RESTART_EXAMPLE frames=5` on success.

`task_support/row_cases.cc` registers the `entropy`, `neighbors`,
`reconstruction`, `malformed` and `compat` groups through
`task_support/check.h`. Run the full acceptance build and checks with:

	$ bash task_support/run.sh target

## Coding style

The xvc source code follows the [Google C++ Style Guide](
https://google.github.io/styleguide/cppguide.html).

## Support

For questions/comments please email support@xvc.io

## Contributing

Patches are encouraged, and may be submitted by forking this project and
submitting a pull request through GitHub.

Please visit [xvc.io](https://xvc.io/developers) for more information on how
to become an xvc contributor.

