
# SELA
### SimplE Lossless Audio
[![Build Status](https://travis-ci.org/sahaRatul/sela.svg?branch=master)](https://travis-ci.org/sahaRatul/sela)
[![codecov](https://codecov.io/gh/sahaRatul/sela/branch/master/graph/badge.svg)](https://codecov.io/gh/sahaRatul/sela)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)

A lossless audio codec which aims to be as simple as possible while still having good enough compression ratios. 

#### Code Quality Metrics
[![Reliability Rating](https://sonarcloud.io/api/project_badges/measure?project=sahaRatul_sela&metric=reliability_rating)](https://sonarcloud.io/dashboard?id=sahaRatul_sela)
[![Maintainability Rating](https://sonarcloud.io/api/project_badges/measure?project=sahaRatul_sela&metric=sqale_rating)](https://sonarcloud.io/dashboard?id=sahaRatul_sela)
[![Security Rating](https://sonarcloud.io/api/project_badges/measure?project=sahaRatul_sela&metric=security_rating)](https://sonarcloud.io/dashboard?id=sahaRatul_sela)
[![Lines of Code](https://sonarcloud.io/api/project_badges/measure?project=sahaRatul_sela&metric=ncloc)](https://sonarcloud.io/dashboard?id=sahaRatul_sela)

### Build Requirements
- cmake
- MSVC/GCC/CLANG/INTEL (Any compiler supporting C++11 should work)
- libao-dev (linux/bsd) / On windows you can skip this dependency

### Current status
|Task|Status|
|:----:|:------:|
|Encoder|**DONE**|
|Decoder|**DONE**|
|Reading and Writing WAV files|**DONE**|
|Reading and Writing SELA files|**DONE**|
|Multithreaded Encoding & Decoding|**DONE**|
|Player|**DONE**|
|Metadata support|**TODO**|
|Seektable support|**TODO**|
|Support for 24 bit audio|**TODO**|
|Optimization|**TODO**|

### Block Diagrams
![Encoder](https://cloud.githubusercontent.com/assets/12273725/8868411/c24585e6-31f5-11e5-937a-e3c11c632704.png)
![Decoder](https://cloud.githubusercontent.com/assets/12273725/8868418/cbb6a1dc-31f5-11e5-91f6-8290766baa34.png)

To understand the core algorithm, see code in frame namespace, frame namespace utilizes maths which is implemented in lpc and rice namespaces.

### Channel forest coding (multi-channel correlation)

By default each channel is encoded independently, except for the legacy
two-channel case where the second channel may be difference coded against
the first. Passing `channelForest = true` turns on optimal channel forest
coding, which exploits correlation between any pair of channels:

- `frame::FrameEncoder(const data::WavFrame& wavFrame, bool channelForest = false)`
- `sela::Encoder(std::ifstream& ifStream, bool channelForest = false)`

Both arguments default to `false`, so existing call sites compile and
produce byte-identical output without changes. The file-level encoder
forwards the setting to every worker thread.

For each channel the encoder really runs the existing LPC and Rice
pipeline on (a) the channel itself and (b) the `parent - child`
difference against every other channel, then measures each candidate by
its actual subframe size: 12 fixed header bytes plus 4 bytes per 32-bit
word in the reflection-coefficient and residue arrays. Every channel
selects exactly one incoming edge - an independent root or one parent
channel - and a minimum arborescence search
(`src/frame/channel_forest.cpp`, Chu-Liu/Edmonds) picks the globally
cheapest acyclic forest. On equal total cost the parent vector is
compared lexicographically from channel 0, with an independent root
recorded as the channel count. The coefficients, residues and parent
field written to disk always belong to the winning candidates.

Requirements while the mode is enabled: 1 to 8 channels, 16-bit signed
PCM, 2048 samples per channel per frame; a file may contain any number of
complete frames. Invalid input raises `data::Exception`.

The decoder needs no flag. Before reconstruction it verifies that
subframe channel numbers cover `0 .. N-1` exactly once, that every
subframe type is legal, that every referenced parent exists and differs
from the channel itself, that parent and child carry the same sample
count, and that the dependency graph is acyclic; any violation raises
`data::Exception`. Channels are then restored along parent links, so
arbitrary multi-level forests decode correctly even when the subframes
are stored out of order, and the output is always ordered by channel
number. File-level decoding hands worker failures back to the caller
instead of terminating the process. Old independent files and legacy
stereo difference files remain readable.

A complete call example lives in
[`examples/channel_forest_example.cpp`](examples/channel_forest_example.cpp):

```sh
c++ -std=c++11 -O2 -pthread -I. examples/channel_forest_example.cpp \
    .task-build/libsela.a -o channel_forest_example
./channel_forest_example demo.wav demo.sela
```

### References
- Linear Prediction
  - [Wikipedia](https://en.wikipedia.org/wiki/Linear_prediction)
  - [Digital Signal Processing by John G. Proakis & Dimitris G. Monolakis](http://www.amazon.com/Digital-Signal-Processing-4th-Edition/dp/0131873741)
  - [A detailed pdf](http://www.ece.ucsb.edu/Faculty/Rabiner/ece259/digital%20speech%20processing%20course/lectures_new/Lecture%2013_winter_2012_6tp.pdf)
- Golomb-Rice lossless compression algorithm
  - [Wikipedia](https://en.wikipedia.org/wiki/Golomb_coding)
  - [Here is an implementation](http://michael.dipperstein.com/rice/index.html)
- [FLAC overview](https://xiph.org/flac/documentation_format_overview.html)
- [Paper on shorten, the original open source lossless codec](ftp://svr-ftp.eng.cam.ac.uk/pub/reports/robinson_tr156.ps.Z)
- ISO/IEC 14496 Part 3, Subpart 11 (Audio Lossless Coding)

#### NOTE: You can get the legacy C code by switching to `legacy` branch.

Also, check out the Java version of this codec at [https://github.com/sahaRatul/sela-java](https://github.com/sahaRatul/sela-java)
