#pragma once

#include "eely/base/bit_reader.h"
#include "eely/base/bit_writer.h"
#include "eely/clip/clip_cursor.h"
#include "eely/clip/clip_impl_base.h"
#include "eely/clip/clip_impl_fixed.h"
#include "eely/clip/clip_utils.h"

#include <gsl/util>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

// `clip_compression_scheme::fixed_seek`
//
// The compressed payload is byte-for-byte the same interleaved `uint16_t`
// stream that `clip_compression_scheme::fixed` produces for the same input
// (same metadata, same quantization, same record order).
// On top of that stream the cooking stage builds a sparse, serializable index
// of *decoding checkpoints* that capture everything needed to resume
// the sequential fixed decoder at an arbitrary position.
//
// Why a single byte offset or a single pose is not enough
// -------------------------------------------------------
// A cooked record (one header plus its time/TRS payload, counted as one
// record `P`) may omit the joint index and/or the time field: both are
// inherited from previously decoded records. Translation, rotation and scale
// each maintain their own interpolation bracket, and a record can update any
// subset of them. Therefore resuming at some byte offset requires, all at
// once:
//   - the byte offset of the first record to decode;
//   - the joint index and time that are currently "in effect"
//     (the values that omitted header fields would inherit);
//   - for every animated component (after rest-pose channels are removed,
//     at most three per joint, total count `C`), the current left and right
//     bracket values together with their times.
//
// What a checkpoint stores
// ------------------------
// `fixed_seek_checkpoint` serializes exactly that state:
//   - `start_pos`          - byte (uint16_t element) offset of the first
//                            record that still has to be decoded;
//   - `current_joint_index`- joint index in effect at `start_pos`
//                            (what a record without `has_joint_index` uses);
//   - `current_time_s`     - record time in effect at `start_pos`
//                            (what a record without `has_time` uses);
//   - one `fixed_seek_component_state` (left/right times and values) per
//     animated component, in the same order as the corresponding
//     `cursor_component` entries of a `cursor` initialized from
//     `clip_metadata_fixed::joints_components` (translations first,
//     then rotations, then scales).
//
// Which time interval a checkpoint serves
// ---------------------------------------
// Checkpoint `i` is valid for every query time `t` with
// `time_from_s <= t < time_to_s` (the last checkpoint also serves
// `t == duration`). The boundary is deliberately *not* the time of the
// checkpoint's future key: it is the earliest time at which the records
// starting at `start_pos` become necessary data, i.e. the minimum
// "time to use" among those records (the left edge of the interpolation
// interval they belong to). A record whose key sits in the future can still
// be required immediately, because it is the right bracket of an interval
// that started earlier; selecting checkpoints by key time would skip it.
//
// After restoring checkpoint `i`, decoding forward stops on its own exactly
// where a from-scratch fixed player would stop, so the resulting cursor state
// (and thus the pose) is identical.
//
// Bounds
// ------
// With `P` cooked records, the number of checkpoints `N` is at most
// `ceil(P / 32) + 2`, and the serialized index is
// `O((C + 1) * (ceil(P / 32) + 1))`. A single `play` call decodes at most
// `32 + 2 * C` records, restores at most one checkpoint, and performs at
// most `ceil(log2(N + 1)) + 1` checkpoint time comparisons.

namespace eely::internal {
// State of a single animated component captured by a checkpoint.
// Meaning of the times matches `cursor_component`: a negative time means
// the corresponding bracket side is absent.
struct fixed_seek_component_state final {
  float left_time_s{-1.0F};
  float right_time_s{-1.0F};

  // Storage is shared by float3 components (translation, scale)
  // and quaternion components (rotation): only the first three/four
  // elements are meaningful respectively.
  std::array<float, 4> left{0.0F, 0.0F, 0.0F, 1.0F};
  std::array<float, 4> right{0.0F, 0.0F, 0.0F, 1.0F};
};

// Fully self-contained snapshot of the sequential fixed decoder state.
struct fixed_seek_checkpoint final {
  // First time (inclusive) this checkpoint can serve.
  float time_from_s{0.0F};

  // First time this checkpoint must not serve (exclusive, except for the
  // last checkpoint which also serves `duration`).
  float time_to_s{0.0F};

  // Offset (in uint16_t elements) of the first record to decode.
  uint32_t start_pos{0};

  // Joint index inherited by records that omit `has_joint_index`.
  uint32_t current_joint_index{0};

  // Record time inherited by records that omit `has_time`.
  float current_time_s{-1.0F};

  // One entry per animated component, matching `cursor` layout.
  std::vector<fixed_seek_component_state> components;
};

// Read/write a checkpoint as explicit fields (never raw structure memory).
// Component group counts are stored once per clip, outside the checkpoints.
fixed_seek_checkpoint fixed_seek_checkpoint_read(bit_reader& reader,
                                                  gsl::index translation_count,
                                                  gsl::index rotation_count,
                                                  gsl::index scale_count);
void fixed_seek_checkpoint_write(bit_writer& writer,
                                 const fixed_seek_checkpoint& checkpoint,
                                 gsl::index translation_count,
                                 gsl::index rotation_count,
                                 gsl::index scale_count);

// Metadata for clips compressed with `clip_compression_scheme::fixed_seek`.
// The fixed-scheme metadata is reused verbatim, so the quantized stream keeps
// exactly the same layout and values as a `fixed` clip of the same input.
using clip_metadata_fixed_seek = clip_metadata_fixed;

// Implementation for clips compressed with `clip_compression_scheme::fixed_seek`.
class clip_impl_fixed_seek final : public clip_impl_base {
public:
  explicit clip_impl_fixed_seek(bit_reader& reader);

  explicit clip_impl_fixed_seek(float duration_s,
                                const std::vector<clip_uncooked_track>& tracks,
                                bool is_additive,
                                const skeleton& skeleton);

  void serialize(bit_writer& writer) const override;

  [[nodiscard]] const clip_metadata_base* get_metadata() const override;

  [[nodiscard]] std::unique_ptr<clip_player_base> create_player() const override;

  // Exposed for the player and for tests.
  [[nodiscard]] const clip_metadata_fixed_seek& get_metadata_fixed_seek() const;
  [[nodiscard]] std::span<const uint16_t> get_data() const;
  [[nodiscard]] std::span<const fixed_seek_checkpoint> get_checkpoints() const;

private:
  clip_metadata_fixed_seek _metadata;
  std::vector<uint16_t> _data;
  std::vector<fixed_seek_checkpoint> _checkpoints;

  // Number of cooked records (`P`), retained so that introspection never
  // has to scan the quantized stream.
  uint32_t _encoded_records{0};
};
}  // namespace eely::internal
