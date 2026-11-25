[![Linux](https://github.com/Matroska-Org/libmatroska/actions/workflows/linux.yaml/badge.svg)](https://github.com/Matroska-Org/libmatroska/actions/workflows/linux.yaml)
[![Windows](https://github.com/Matroska-Org/libmatroska/actions/workflows/windows.yaml/badge.svg)](https://github.com/Matroska-Org/libmatroska/actions/workflows/windows.yaml)
[![macOS](https://github.com/Matroska-Org/libmatroska/actions/workflows/macos.yaml/badge.svg)](https://github.com/Matroska-Org/libmatroska/actions/workflows/macos.yaml)
[![GCC10](https://github.com/Matroska-Org/libmatroska/actions/workflows/linux-gcc10.yaml/badge.svg)](https://github.com/Matroska-Org/libmatroska/actions/workflows/linux-gcc10.yaml)

# libmatroska
a C++ library to parse and create Matroska files

# Building and installing

## Building and installing the library

libmatroska is based on `cmake`. It requires a C++ compiler compatible
with the C++14 standard as well as
[libebml](https://github.com/Matroska-Org/libebml). This means that
the normal build process consists of the usual steps:

1. Create a build directory: `mkdir build ; cd build`
2. Generate the make file: `cmake ..`
3. Compilation: `make`
4. Installation (run this as root): `make install`

## Customizing what's build with options

By default only a static library is built.

This library supports the usual `cmake` options for specifying
installation paths (e.g. `-DCMAKE_INSTALL_PREFIX=/opt/libmatroska`). The
following additional `cmake` options are supported:

* `-DEbml_DIR=/path/to/libebml` — if `libebml` was installed in a
  non-default location, you have to tell `cmake` where to find
  it. Point this variable to the directory containing the
  `EbmlConfig.cmake` file.
* `-DDISABLE_PKGCONFIG=YES` — don't generate and install the
  `libebml.pc` package configuration module for `pkg-config`
* `-DDISABLE_CMAKE_CONFIG=YES` — don't generate and install the
  package configuration module for `cmake`
* `-DBUILD_SHARED_LIBS=YES` — build the shared library instead of the
  static one (default: no)

## Ordered-chapter reader

`matroska/KaxOrderedReader.h` provides `KaxOrderedReader`, which plays the
ordered chapters of a Matroska root file by following `ChapterSegmentUID`
references to other Matroska files. The sources are not remuxed; the reader
keeps only a compact frame index in memory and re-reads each frame payload from
the caller's `IOCallback`s on demand.

`Open()` takes the source callbacks (shared ownership, never closed by the
reader), the index of the root source, the `EditionUID` (which must be flagged
ordered) and the `TrackUID` of the video track to play.

```cpp
std::vector<std::shared_ptr<libebml::IOCallback>> sources;
sources.push_back(std::make_shared<libebml::StdIOCallback>("part1.mkv", MODE_READ));
sources.push_back(std::make_shared<libebml::StdIOCallback>("part2.mkv", MODE_READ));

std::unique_ptr<libmatroska::KaxOrderedReader> reader;
if (!libmatroska::KaxOrderedReader::Open(sources, 0, editionUID, trackUID, reader))
  return 1;

libmatroska::KaxOrderedReader::Packet packet;
while (reader->ReadNext(packet) == libmatroska::KaxOrderedReader::Status::Ok) {
  if (packet.kind == libmatroska::KaxOrderedReader::Kind::Config) {
    // packet.data is the source's CodecPrivate for this chapter.
  } else {
    // packet.data is a full frame; source_ns/program_ns and the visible
    // window describe where it belongs in the program timeline.
  }
}

// Seek to a program position in nanoseconds; Seek(DurationNS()) is valid.
reader->Seek(5'000'000);
```

The example `examples/ordered_reader.cpp` dumps the event stream. Its usage is

```
ordered_reader <manifest-path> <rootSource> <editionUID> <trackUID> <input> [<input> ...]
```

and it writes one decimal text line per event, either

```
C chapter source track source_ns program_ns size
```

or

```
F chapter source track source_ns program_ns duration_ns visible_begin_ns visible_duration_ns size
```

## Code of conduct

Please note that this project is released with a [Contributor Code of Conduct](CODE_OF_CONDUCT.md). By participating in this project you agree to abide by its terms.
