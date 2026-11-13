VP9 structural field reference (data only, no parsing implementation)

Primary reference: VP9 Bitstream and Decoding Process Specification v0.6 (2016-03-31), uncompressed_header, loop_filter_params, quantization_params, segmentation_params and tile_info.
https://storage.googleapis.com/downloads.webmproject.org/docs/vp9/vp9-bitstream-specification-v0.6-20160331-draft.pdf
Implementation cross-check: https://chromium.googlesource.com/webm/libvpx/+/refs/heads/main/vp9/decoder/vp9_decodeframe.c

A signed literal carries its magnitude bits followed by one sign bit, including zero magnitudes. A present quantizer delta has four magnitude bits plus sign. Loop-filter reference/mode deltas have six magnitude bits plus sign; update flags govern whether values occur.
Segmentation map update carries seven optional 8-bit probabilities; temporal update adds three optional 8-bit probabilities. Data update carries absolute/delta mode, then eight segments with four feature-enabled flags each. Feature magnitude widths are 8,6,2,0; only the first two are signed. Zero-width enabled features consume no value bits.
Explicit-size inter headers include high-precision-MV flag and interpolation selection: a switchable flag, otherwise a two-bit filter. Context, filter, quantizer and segmentation syntax follows before tile layout.
Tile-row log2 uses one bit, then one extra bit if nonzero. Tile lengths are four-byte big-endian byte counts, excluding their prefixes; the last tile consumes the child frame remainder. Compressed header length is a 16-bit field. Alignment padding is zero.
Superframe marker mask is 0xe0 with value0xc0; low3 bits plus1 give count, bits3..4 plus1 give little-endian size width. The index has two markers and count*width size bytes.
