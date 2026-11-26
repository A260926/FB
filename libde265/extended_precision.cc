#include "extended_precision.h"

#include <algorithm>
#include <cassert>

namespace de265 {
namespace extended_precision {

int range_bits(int component_bit_depth)
{
  return std::max(15, component_bit_depth + 6);
}

int residual_rounding_shift(int component_bit_depth)
{
  const int range = range_bits(component_bit_depth);
  return range + 5 - component_bit_depth;
}

int dequant_shift(int component_bit_depth, int log2_transform_size,
                  bool scaling_list_enabled)
{
  const int range = range_bits(component_bit_depth);
  int shift = component_bit_depth + log2_transform_size - 5;
  shift -= range - 15;          // log2TransformRange dynamic-range extension
  if (!scaling_list_enabled) {
    shift -= 4;                  // equivalent m_x_y=16 folding
  }
  return shift;
}

int transform_skip_shift(int log2_transform_size, int residual_shift)
{
  return std::min(5, residual_shift - 2) + log2_transform_size;
}

int32_t clip_to_range(int64_t value, int bits)
{
  const int64_t minimum = -(INT64_C(1) << bits);
  const int64_t maximum =  (INT64_C(1) << bits) - 1;
  value = std::max(minimum, std::min(maximum, value));
  return static_cast<int32_t>(value);
}

int level_scale_factor(int q_p_y_prime)
{
  static const int level_scale[6] = { 40, 45, 51, 57, 64, 72 };
  assert(q_p_y_prime >= 0);
  return level_scale[q_p_y_prime % 6] << (q_p_y_prime / 6);
}

bool BitReader::read_bit(int& value)
{
  if (bit_position >= bit_count || !data) return false;
  const size_t byte_position = bit_position >> 3;
  const int bit_in_byte = 7 - static_cast<int>(bit_position & 7);
  value = (data[byte_position] >> bit_in_byte) & 1;
  ++bit_position;
  return true;
}

bool BitReader::read_bits(int count, uint32_t& value)
{
  value = 0;
  if (count < 0 || count > 31) return false;
  for (int i = 0; i < count; ++i) {
    int bit;
    if (!read_bit(bit)) return false;
    value = (value << 1) | static_cast<uint32_t>(bit);
  }
  return true;
}

bool limited_egk_decode(BitReader& reader, int rice_parameter,
                        int range_bits_value, uint32_t& value,
                        size_t* consumed_bits)
{
  if (rice_parameter < 0 || rice_parameter > 28 ||
      range_bits_value < 15 || range_bits_value > 31) {
    return false;
  }

  // The largest ordinary prefix whose k+prefix suffix stays inside the
  // finite CoeffMin..CoeffMax range.  At this boundary the unary zero is
  // implicit and is not present in the bitstream.
  const int max_prefix = range_bits_value - rice_parameter - 1;
  if (max_prefix < 0) return false;

  const size_t start_position = reader.bit_position;
  int prefix = 0;
  bool implicit_terminator = false;

  while (prefix < max_prefix) {
    int bit;
    if (!reader.read_bit(bit)) return false;
    if (bit == 0) break;
    ++prefix;
  }

  if (prefix == max_prefix) {
    // If the next symbol would have been another leading one, this is the
    // finite boundary; otherwise the zero read after max_prefix is an ordinary
    // terminator.  The loop above consumes the terminator for prefixes below
    // the limit, so reaching max_prefix after reading max_prefix ones means
    // the terminator is implicit.
    implicit_terminator = true;
  }

  uint32_t suffix = 0;
  const int suffix_bits = rice_parameter + prefix;
  if (suffix_bits > 31 || !reader.read_bits(suffix_bits, suffix)) {
    return false;
  }

  uint64_t base = (UINT64_C(1) << prefix) - 1;
  base <<= rice_parameter;
  value = static_cast<uint32_t>(base + suffix);

  if (consumed_bits) *consumed_bits = reader.bit_position - start_position;
  (void)implicit_terminator;
  return true;
}

}  // namespace extended_precision
}  // namespace de265
