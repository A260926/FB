# VP9 tile range extraction

`common/vp9_tiles.h` adds a small, decode-free API for locating the coded tile
payloads in a VP9 packet:

```cpp
bool vp9_parser::ExtractVP9Tiles(const uint8_t* packet,
                                 size_t size,
                                 std::vector<Vp9FrameLayout>* out);
```

It parses the uncompressed header (and the superframe index, when present) far
enough to know where each tile's compressed bytes live, without parsing the
compressed header or decoding any entropy-coded tile or pixel data. This makes
it usable to hand individual tiles to parallel readers/decoders.

## Output

On success `*out` is replaced with one `Vp9FrameLayout` per frame, in packet
order, and `true` is returned. On failure `false` is returned and both `*out`
and the input buffer are left untouched.

`Vp9FrameLayout`:

| Field | Meaning |
| --- | --- |
| `offset`, `length` | Byte range of this frame (or superframe child) in the packet. |
| `uncompressed_header_bytes` | Size of the uncompressed header, relative to `offset`. |
| `compressed_header_offset`, `compressed_header_size` | Byte range of the compressed header in the packet. |
| `width`, `height` | Frame size from the header. |
| `tile_rows`, `tile_columns` | Number of tile rows/columns. |
| `show_existing` | True for `show_existing_frame`. |
| `existing_index` | `frame_to_show_map_idx` for `show_existing_frame`. |
| `tiles` | Row-major tile ranges (see below). |

`Vp9TileRange` holds `offset`, `length`, `row`, and `column`.

All offsets are measured from the start of the input `packet`. Each tile range
contains only that tile's payload:

* The four-byte big-endian tile length prefix that precedes every tile except
  the last is **not** part of the returned range.
* The uncompressed and compressed headers are **not** part of any tile range.
* The last tile of a frame consumes the remainder of that frame's data and
  never extends into the next superframe child or into the trailing index.

The ranges are non-empty, are in row-major order (`row` outer, `column`
inner), do not overlap, and together exactly cover the frame's tile payload.

For `show_existing_frame` only `offset`, `length`, `show_existing`, and
`existing_index` are populated; `width`/`height`, the header byte counts, and
`tiles` are zero/empty because the displayed picture's layout is not described
by this packet.

## Supported input

* Single frames and packets ending in a VP9 superframe index.
* Superframe indices with 1-4 byte little-endian child sizes, 1-8 children,
  matching leading/trailing markers, and child sizes that sum exactly to the
  payload. A one-byte `show_existing_frame` may be a child.
* Profiles 0-3 key frames, intra-only frames, and inter frames that carry an
  explicit frame size.
* The complete color config, reference index/sign-bias list, interpolation
  filter selection, context flags, loop-filter update, quantizer deltas, and
  the full `segmentation_params()` grammar (map update, temporal update,
  probabilities, and all four segmentation features with their 8/6/2/0-bit
  widths, signedness, and zero-width enabled features).

## Rejected input

`ExtractVP9Tiles` returns `false` for:

* A truncated field anywhere in the uncompressed header.
* A compressed header that runs past the frame.
* A missing or over-long tile length prefix, or an empty tile.
* Child sizes that do not match the superframe payload, or a corrupt present
  index (never downgraded to a plain frame).
* Size/offset arithmetic that would overflow.
* An inter frame whose size is inherited from a reference
  (`frame_size_with_refs()` selects a `found_ref`): there is no reference-slot
  model, so the size is not guessed.
* Non-zero padding bits at the end of the uncompressed header.

This API validates header structure and byte ranges only. It does not decode
the compressed header or the tile contents, so it does not verify that a tile
is a decodable bitstream.

## Example

```cpp
#include <cstdio>
#include <vector>

#include "common/vp9_tiles.h"

void ReadTiles(const uint8_t* packet, size_t size) {
  std::vector<vp9_parser::Vp9FrameLayout> frames;
  if (!vp9_parser::ExtractVP9Tiles(packet, size, &frames)) {
    std::fprintf(stderr, "not a parseable VP9 packet\n");
    return;
  }

  for (size_t f = 0; f < frames.size(); ++f) {
    const vp9_parser::Vp9FrameLayout& frame = frames[f];
    if (frame.show_existing) {
      std::printf("frame %zu: show_existing index=%d\n", f,
                  frame.existing_index);
      continue;
    }
    std::printf("frame %zu: %dx%d, %d x %d tiles\n", f, frame.width,
                frame.height, frame.tile_rows, frame.tile_columns);
    for (size_t t = 0; t < frame.tiles.size(); ++t) {
      const vp9_parser::Vp9TileRange& tile = frame.tiles[t];
      // The tile payload is packet[tile.offset .. tile.offset + tile.length).
      std::printf("  tile[%d][%d]: offset=%zu length=%zu\n", tile.row,
                  tile.column, tile.offset, tile.length);
    }
  }
}
```

## Notes on the existing parser

`Vp9HeaderParser::ParseUncompressedHeader` shares the same bit reader and
header/segmentation grammar as `ExtractVP9Tiles` (`common/vp9_syntax.h`), so
the two cannot drift. Its public calling convention is unchanged. One behavior
was corrected: headers that enable segmentation now consume the full
`segmentation_params()` syntax and therefore report the real `row_tiles()` and
`column_tiles()`. `Vp9LevelStats::GetMaxColumnTiles()` consequently reports the
right maximum column count for segmented streams.
