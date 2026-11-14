# VP9 tile range extraction

`common/vp9_tiles.h` and `common/vp9_tiles.cc` add a structural VP9 packet
analyzer. It reports where every tile of every frame in a packet lives, without
decoding the compressed header, the tile payload, or any pixels. The result is
intended for callers that want to read or process tiles in parallel.

## API

```c++
namespace vp9_parser {

struct Vp9TileRange {
  size_t offset;   // relative to the start of the packet
  size_t length;   // payload bytes only; excludes the 4 byte length prefix
  int row;
  int column;
};

struct Vp9FrameLayout {
  size_t offset;                     // frame start, relative to the packet
  size_t length;                     // whole frame (subframe) length
  size_t uncompressed_header_bytes;
  size_t compressed_header_offset;   // absolute, relative to the packet
  size_t compressed_header_size;
  int width;
  int height;
  int tile_rows;
  int tile_columns;
  bool show_existing;
  int existing_index;
  std::vector<Vp9TileRange> tiles;   // row-major order
};

bool ExtractVP9Tiles(const uint8_t* packet, size_t size,
                     std::vector<Vp9FrameLayout>* out);
}
```

On success `out` is replaced by one `Vp9FrameLayout` per frame and `true` is
returned. On failure `false` is returned and neither the packet nor `out` is
modified. Only ranges and metadata are produced; no compressed byte is copied.

## Supported syntax

* Single-frame packets, and packets ending in a VP9 superframe index. The index
  fields are validated: both markers, 1-8 subframes, and 1-4 byte little-endian
  child lengths whose sum exactly covers the payload.
* Key frames, intra-only frames, and inter frames that carry an explicit frame
  size, for profiles 0 through 3.
* The complete uncompressed header, including color configuration, reference
  indices and sign biases, interpolation filter selection, context flags, loop
  filter updates, quantization deltas, and the full segmentation syntax (map
  probabilities, temporal update probabilities, and per-segment feature data).
* `show_existing_frame` subframes. Only the frame range and the existing index
  are reported; `width`, `height`, and the tile counts are zero and `tiles` is
  empty. The layout of the shown picture is never invented.

Inter frames whose size is inherited through `frame_size_with_refs()` are
rejected. This entry point keeps no reference slot model, and it never reuses a
width from a previous parse to guess a size.

## Range rules

* The uncompressed header ends on a byte boundary; its padding bits must be
  zero.
* The compressed header starts at the next byte and spans the 16-bit length
  field from the header. Its entropy-coded contents are never interpreted,
  even when they resemble tile length prefixes.
* Tiles appear in row-major order. Every tile except the last is preceded by a
  four byte big-endian length, and that prefix is not part of the returned
  range.
* The last tile takes whatever remains of its frame, so it never reaches into
  the next superframe subframe or the trailing index.
* Returned tiles are non-empty, non-overlapping, and together cover the frame's
  tile payload; the compressed header is never counted as tile data.

Supported tile geometry is 1, 2, or 4 rows and a column count that is legally
encoded for the frame width (the same tile-sizing rules used by the
`tile_info` syntax).

## Rejected input

The parser fails rather than guessing when it encounters:

* truncation in the middle of a header field;
* a compressed header that runs past its frame;
* a missing, truncated, zero-length, or over-long tile length prefix;
* a final tile with no payload, or subframe lengths that do not sum to the
  payload;
* size or offset arithmetic that would overflow;
* a nonzero alignment pad bit;
* a superframe marker with a corrupt index (the packet never degrades to a
  plain single frame);
* a frame using inherited `frame_size_with_refs()` dimensions.

Only header and range structure is validated. Pixel data and the entropy-coded
compressed header are opaque.

## Example

```c++
#include <cstdio>
#include <vector>

#include "common/vp9_tiles.h"

void DumpTiles(const unsigned char* packet, size_t size) {
  std::vector<vp9_parser::Vp9FrameLayout> frames;
  if (!vp9_parser::ExtractVP9Tiles(packet, size, &frames)) {
    std::fprintf(stderr, "not a supported VP9 packet\n");
    return;
  }

  for (size_t i = 0; i < frames.size(); ++i) {
    const vp9_parser::Vp9FrameLayout& frame = frames[i];
    std::printf("frame %zu: %dx%d, %d x %d tiles, header %zu+%zu bytes\n", i,
                frame.width, frame.height, frame.tile_columns, frame.tile_rows,
                frame.uncompressed_header_bytes, frame.compressed_header_size);

    for (size_t t = 0; t < frame.tiles.size(); ++t) {
      const vp9_parser::Vp9TileRange& tile = frame.tiles[t];
      std::printf("  tile r%d c%d: packet[%zu..%zu)\n", tile.row, tile.column,
                  tile.offset, tile.offset + tile.length);
      // tile.offset / tile.length can be handed to a worker thread; the payload
      // is packet[tile.offset .. tile.offset + tile.length).
    }
  }
}
```

## Relationship to the existing parser

`Vp9HeaderParser::ParseUncompressedHeader()` keeps its public behavior. It now
shares the uncompressed-header syntax, bit reader, and bounds checks with
`ExtractVP9Tiles()`, so a segmented header no longer stops early and
`row_tiles()` / `column_tiles()` are correct for such frames.
`Vp9LevelStats::GetMaxColumnTiles()` therefore observes the real column count.
