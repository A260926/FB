#ifndef DE265_EXTENDED_PRECISION_H
#define DE265_EXTENDED_PRECISION_H

#include <cstddef>
#include <cstdint>

namespace de265 {
namespace extended_precision {

// Extended-precision coefficient/transform dynamic range, in bits.
// H.265: Max(15, BitDepthC + 6) for the component being decoded.
int range_bits(int component_bit_depth);

// Second transform/skip rounding shift:
// log2TransformRange + 5 - BitDepth, equivalently
// max(20 - BitDepth, 11) for the supported 8..16 bit depths.
int residual_rounding_shift(int component_bit_depth);

// Inverse-quantization shift after the optional m_x_y=16 folding for the
// default scaling list.  scaling_list_enabled selects the spec shift before
// that four-bit folding.
int dequant_shift(int component_bit_depth, int log2_transform_size,
                  bool scaling_list_enabled);

// tsShift used by transform-skip/residual DPCM reconstruction.
int transform_skip_shift(int log2_transform_size, int residual_shift);

int32_t clip_to_range(int64_t value, int range_bits);

// Signed QP is QPYPrime (signalled QPY plus 6*(BitDepth-8)).
int level_scale_factor(int q_p_y_prime);

// Finite/limited EGk reader used by coeff_abs_level_remaining.  Bits are
// packed MSB-first.  At the finite prefix boundary the terminating zero is
// implicit and is therefore not consumed.  Returns false on a truncated
// input or an out-of-range code.
struct BitReader {
  const uint8_t* data;
  size_t bit_count;
  size_t bit_position;

  bool read_bit(int& value);
  bool read_bits(int count, uint32_t& value);
};

bool limited_egk_decode(BitReader& reader, int rice_parameter,
                        int range_bits, uint32_t& value,
                        size_t* consumed_bits = nullptr);

}  // namespace extended_precision
}  // namespace de265

#endif
