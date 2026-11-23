[![Linux](https://github.com/Matroska-Org/libmatroska/actions/workflows/linux.yaml/badge.svg)](https://github.com/Matroska-Org/libmatroska/actions/workflows/linux.yaml)
[![Windows](https://github.com/Matroska-Org/libmatroska/actions/workflows/windows.yaml/badge.svg)](https://github.com/Matroska-Org/libmatroska/actions/workflows/windows.yaml)
[![macOS](https://github.com/Matroska-Org/libmatroska/actions/workflows/macos.yaml/badge.svg)](https://github.com/Matroska-Org/libmatroska/actions/workflows/macos.yaml)

# libmatroska
A C++ library to parse and create Matroska files.

# Building and installing

## Building and installing the library

libmatroska is based on `cmake`. It requires a C++ compiler compatible
with the C++17 standard as well as
[libebml](https://github.com/Matroska-Org/libebml). This means that
the normal build process consists of the usual steps:

1. Create a build directory: `mkdir build ; cd build`
2. Generate the make file: `cmake ..`
3. Compilation: `make`
4. Installation (run this as root): `make install`

## Customizing what's built with options

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

## Code of conduct

Please note that this project is released with a [Contributor Code of Conduct](CODE_OF_CONDUCT.md). By participating in this project you agree to abide by its terms.

# Frame payload backing store

A muxer usually compresses frames into in-memory `DataBuffer` objects while it
does not yet know which Matroska output they will be written to. A
`libmatroska::KaxFrameStore` lets a `KaxCluster` move those payload bytes out
of memory once a batch is complete, while the block objects, timestamps,
references, durations, flags and lacing metadata stay in place. The cluster can
later be exported with the usual `KaxCluster::Render()` call, which streams the
payloads back from the store without decoding any media.

## Ownership

* Create a store around a caller provided stream:

  ```cpp
  auto backing = std::make_shared<libebml::MemIOCallback>();
  auto store   = std::make_shared<libmatroska::KaxFrameStore>(backing);
  ```

  The stream must be non-null (the constructor rejects a null pointer with
  `std::invalid_argument`), start empty and be readable, writable and seekable.
  It must be a different stream than the one passed to `Render()`.
* The store and its stream use shared ownership. Every spooled block keeps a
  `std::shared_ptr<KaxFrameStore>`, so the store and stream stay alive until the
  last block that references them is released, even if the caller drops its own
  copies.
* After `KaxCluster::SpoolFrames(store)` succeeds, each moved `DataBuffer` is
  freed and its free callback runs exactly once. The blocks themselves are not
  replaced or recreated.

Clusters that never call `SpoolFrames()` keep their previous, fully resident
behavior.

## Spooling and retrying after a failure

`SpoolFrames()` moves every still-resident frame of the cluster, both frames
added through `AddFrame()` (direct `BlockGroup`s) and frames registered through
`AddBlockBlob()` that have not been rendered yet.

It returns `false`, without modifying any block, when:

* the store pointer is null, or the cluster already holds frames attached to a
  different store;
* a backing stream write is short, or a seek/write throws;
* a buffer free callback throws.

On such a failure every resident frame stays resident and the cluster remains
fully exportable, so the same cluster can be rendered as usual. Once the stream
problem is fixed, simply call `SpoolFrames()` again. Any bytes a failed attempt
left at the end of the backing stream are never interpreted as frame data.

Calling `SpoolFrames()` again with the same store only moves frames added after
the previous call; already spooled payloads are never rewritten. Frames added
after a successful spool always start a new block, they are never appended to a
migrated lace.

## Rendering spooled clusters

While spooled, `UpdateSize()`, lacing selection and all size calculations use
the stored frame sizes only; the backing bytes are read exclusively inside
`Render()`. Payload bytes pass through a single 64 KiB working area
(`KaxFrameStore::WorkBufferSize`), regardless of frame, lace or cluster size;
frames larger than that are transferred in chunks. A short read, a read
exception or an output failure is thrown to the caller instead of producing a
truncated frame. The backing content is not modified by rendering, so a failed
export can be retried with a fresh, empty output stream.

Other behavior notes:

* `KaxInternalBlock::GetBuffer()` keeps working for resident frames and throws
  `std::logic_error` once the frame is spooled; spooled payloads are never
  silently pulled back into memory.
* `ReleaseFrames()` and cluster/block destruction are safe in both the resident
  and the spooled state. After an explicit `ReleaseFrames()` the cluster no
  longer needs to be exportable.
* Copying a block or cluster whose frames were spooled is not supported and
  throws `std::logic_error`.

The runnable example `test/frame_store_example.cpp` builds a small Matroska
file, spools its frames and writes a re-readable `.mkv`; the task tests in
`test/frame_store.cpp` cover the transactional, streaming and lifetime aspects.

# Packaging status

[![Packaging status](https://repology.org/badge/vertical-allrepos/libmatroska.svg?columns=3&header=libmatroska&exclude_unsupported=1)](https://repology.org/project/libmatroska/versions)
