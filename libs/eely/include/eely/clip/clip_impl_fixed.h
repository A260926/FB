#pragma once

#include "eely/base/bit_reader.h"
#include "eely/clip/clip_cursor.h"
#include "eely/clip/clip_impl_base.h"
#include "eely/clip/clip_utils.h"

#include <gsl/util>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace eely::internal {
// Describes intervals joint's translation and scales are within.
// Used for quantization.
struct joint_range final {
  gsl::index joint_index{0};
  float range_translation_from{0.0F};
  float range_translation_length{0.0F};
  float range_scale_from{0.0F};
  float range_scale_length{0.0F};
};

// Metadata for clips compressed with `clip_compression_scheme::fixed`.
struct clip_metadata_fixed final : public clip_metadata_base {
  std::vector<joint_components> joints_components;
  std::vector<joint_range> joints_ranges;
};

// Cook tracks into fixed-scheme metadata and the interleaved quantized
// `uint16_t` stream. Shared by `clip_compression_scheme::fixed` and
// `clip_compression_scheme::fixed_seek`, so both schemes produce byte-for-byte
// identical payloads for the same input.
void clip_impl_fixed_cook(float duration_s,
                          const std::vector<clip_uncooked_track>& tracks,
                          bool is_additive,
                          const skeleton& skeleton,
                          clip_metadata_fixed& out_metadata,
                          std::vector<uint16_t>& out_data);

// Implementation for clips compressed with `clip_compression_scheme::fixed`.
class clip_impl_fixed final : public clip_impl_base {
public:
  explicit clip_impl_fixed(bit_reader& reader);

  explicit clip_impl_fixed(float duration_s,
                           const std::vector<clip_uncooked_track>& tracks,
                           bool is_additive,
                           const skeleton& skeleton);

  void serialize(bit_writer& writer) const override;

  [[nodiscard]] const clip_metadata_base* get_metadata() const override;

  [[nodiscard]] std::unique_ptr<clip_player_base> create_player() const override;

  // Read-only access to the quantized `uint16_t` stream.
  [[nodiscard]] std::span<const uint16_t> get_data() const;

private:
  clip_metadata_fixed _metadata;
  std::vector<uint16_t> _data;
};
}  // namespace eely::internal