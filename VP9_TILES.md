# VP9 tile range extraction

`common/vp9_tiles.h` exposes a single entry point,
`vp9_parser::ExtractVP9Tiles()`, that returns the byte range of every VP9
tile in a packet **without decoding any pixels or interpreting the
range-coded compressed header**. The ranges are intended for parallel reads:
each tile can be fetched and handed to a decoder independently.

## API

```cpp
namespace vp9_parser {

struct Vp9TileRange {
  std::size_t offset;  // Relative to the start of the input packet.
  std::size_t length;  // Tile payload only; size prefixes are excluded.
  int row;
  int column;
};

struct Vp9FrameLayout {
  std::size_t offset;                    // Sub-frame range in the packet.
  std::size_t length;
  std::size_t uncompressed_header_bytes;
  std::size_t compressed_header_offset;  // Relative to the packet start.
  std::size_t compressed_header_size;
  int width;
  int height;
  int tile_rows;
  int tile_columns;
  bool show_existing;
  int existing_index;
  std::vector<Vp9TileRange> tiles;
};

bool ExtractVP9Tiles(const std::uint8_t* packet,
                     std::size_t size,
                     std::vector<Vp9FrameLayout>* out);

}  // namespace vp9_parser
```

All offsets are relative to byte zero of the `packet` passed in. A length
covers only the corresponding payload: the four-byte big-endian size prefix
in front of every non-final tile and the trailing superframe index are never
included. No compressed data is copied; only ranges and metadata are stored.

On success `out` is replaced with one `Vp9FrameLayout` per (sub-)frame. On
failure the function returns `false` and leaves both `out` and the input
buffer unchanged.

## What is parsed

The full VP9 uncompressed header is consumed through one shared,
bounds-checked bit reader (`common/vp9_uncompressed_header.*`, also used by
the existing `Vp9HeaderParser`):

* frame marker, profile 0–3 and the show-existing flag;
* color configuration, including bit depth, color space, range and
  subsampling;
* reference indices, reference sign biases and the interpolation filter
  selection;
* frame context flags, the loop-filter update and quantizer deltas;
* the complete segmentation syntax — map probabilities, temporal update
  probabilities and all eight segments' four feature types. Feature value
  widths are 8, 6, 2 and 0 bits; the first two are signed. An enabled
  zero-width feature consumes no value bits;
* tile row/column counts and the 16-bit compressed-header length;
* the zero alignment padding that terminates the uncompressed header (a
  nonzero padding bit is rejected).

The compressed header is skipped using its declared length and never
interpreted. Tiles follow in row-major order: every tile except the last is
preceded by a four-byte big-endian length, and the last tile takes the
remaining bytes of its sub-frame.

## Supported frames and packets

* Single frames with no superframe index.
* Packets carrying a superframe index: 1–4 byte little-endian length fields,
  1–8 sub-frames, both marker bytes and the declared sub-frame total are all
  validated. A one-byte show-existing frame is a valid sub-frame.
* Key, intra-only and explicit-size inter frames for profiles 0–3.
* One, two or four tile rows and any column count legal for the frame width.
* Show-existing frames report only `show_existing`, `existing_index` and the
  frame range. Width, height and every range beyond the frame stay zero and
  `tiles` is empty; the layout of the displayed frame is not invented.

## When extraction fails

* a field is read past the end of the buffer, or the alignment padding is
  nonzero;
* an inter frame selects `frame_size_with_refs` to inherit its resolution.
  No reference-slot model is maintained and a previous width is never reused
  as a guess, so the strict entry point rejects the frame (the legacy
  `Vp9HeaderParser` keeps accepting it);
* the compressed header runs past the end of the frame;
* a tile size prefix is missing, declares an empty tile, extends past the
  frame, or the final tile would be empty;
* a superframe index is present but malformed, or the sub-frames do not cover
  the payload exactly — there is no fallback to single-frame parsing;
* any offset or size computation overflows.

Only headers and range structure are validated. Compressed-header and tile
contents are treated as opaque bytes.

## Example

```cpp
#include <cstdio>
#include <vector>

#include "common/vp9_tiles.h"

void ReadTileRanges(const std::uint8_t* packet, std::size_t packet_size) {
  std::vector<vp9_parser::Vp9FrameLayout> frames;
  if (!vp9_parser::ExtractVP9Tiles(packet, packet_size, &frames)) {
    std::fprintf(stderr, "VP9 layout extraction failed\n");
    return;
  }

  for (const vp9_parser::Vp9FrameLayout& frame : frames) {
    if (frame.show_existing) {
      std::printf("show-existing frame, index %d\n", frame.existing_index);
      continue;
    }

    std::printf("frame %dx%d tiles=%dx%d\n", frame.width, frame.height,
                frame.tile_columns, frame.tile_rows);
    for (const vp9_parser::Vp9TileRange& tile : frame.tiles) {
      // packet + tile.offset, tile.length bytes can be read independently,
      // for example dispatched to a worker thread.
      const std::uint8_t* tile_data = packet + tile.offset;
      (void)tile_data;
      std::printf("  tile(r=%d,c=%d) offset=%zu length=%zu\n", tile.row,
                  tile.column, tile.offset, tile.length);
    }
  }
}
```
