#include "eely/clip/clip_player_fixed_seek.h"

#include "eely/base/base_utils.h"
#include "eely/clip/clip_cooking_none_fixed.h"
#include "eely/clip/clip_cursor.h"
#include "eely/clip/clip_impl_fixed_seek.h"
#include "eely/clip/clip_utils.h"
#include "eely/math/float3.h"
#include "eely/math/quantization.h"
#include "eely/math/quaternion.h"
#include "eely/skeleton/skeleton_pose.h"
#include "eely/skeleton/skeleton_utils.h"

#include <gsl/narrow>
#include <gsl/util>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace eely::internal {
namespace {
// Largest checkpoint index whose `time_from_s` is not greater than `time_s`.
// Every midpoint evaluation is counted as one checkpoint time comparison.
gsl::index select_checkpoint(std::span<const fixed_seek_checkpoint> checkpoints,
                             float time_s,
                             size_t& out_comparisons)
{
  gsl::index low{0};
  gsl::index high{std::ssize(checkpoints)};

  while (low < high) {
    const gsl::index mid{low + (high - low) / 2};
    ++out_comparisons;

    if (checkpoints[mid].time_from_s <= time_s) {
      low = mid + 1;
    }
    else {
      high = mid;
    }
  }

  return low - 1;
}

void restore_checkpoint(const fixed_seek_checkpoint& checkpoint, cursor& cursor)
{
  cursor.last_play_time_s = -1.0F;
  cursor.last_data_pos = gsl::narrow<gsl::index>(checkpoint.start_pos);
  cursor.last_data_time_s = checkpoint.current_time_s;
  cursor.last_data_joint_index =
      checkpoint.current_joint_index == gsl::narrow<uint32_t>(joints_max_count)
          ? std::numeric_limits<gsl::index>::max()
          : gsl::narrow<gsl::index>(checkpoint.current_joint_index);

  gsl::index component_index{0};

  for (cursor_component<float3>& component : cursor.translations) {
    const fixed_seek_component_state& state{checkpoint.components[component_index++]};
    component.left_time_s = state.left_time_s;
    component.right_time_s = state.right_time_s;
    component.left = float3{state.left[0], state.left[1], state.left[2]};
    component.right = float3{state.right[0], state.right[1], state.right[2]};
  }

  for (cursor_component<quaternion>& component : cursor.rotations) {
    const fixed_seek_component_state& state{checkpoint.components[component_index++]};
    component.left_time_s = state.left_time_s;
    component.right_time_s = state.right_time_s;
    component.left = quaternion{state.left[0], state.left[1], state.left[2], state.left[3]};
    component.right = quaternion{state.right[0], state.right[1], state.right[2], state.right[3]};
  }

  for (cursor_component<float3>& component : cursor.scales) {
    const fixed_seek_component_state& state{checkpoint.components[component_index++]};
    component.left_time_s = state.left_time_s;
    component.right_time_s = state.right_time_s;
    component.left = float3{state.left[0], state.left[1], state.left[2]};
    component.right = float3{state.right[0], state.right[1], state.right[2]};
  }
}
}  // namespace

clip_player_fixed_seek::clip_player_fixed_seek(
    const clip_metadata_fixed_seek& metadata,
    std::span<const uint16_t> data,
    std::span<const fixed_seek_checkpoint> checkpoints,
    const uint32_t encoded_records)
    : _metadata(metadata),
      _data{data},
      _checkpoints{checkpoints},
      _encoded_records{encoded_records}
{
  EXPECTS(!_checkpoints.empty());

  cursor_init(_cursor, _metadata.joints_components);
}

float clip_player_fixed_seek::get_duration_s()
{
  return _metadata.duration_s;
}

void clip_player_fixed_seek::play(const float time_s, skeleton_pose& out_pose)
{
  using flags = compression_key_flags;

  EXPECTS(std::isfinite(time_s));
  EXPECTS(time_s >= 0.0F && time_s <= _metadata.duration_s);

  if (_metadata.is_additive) {
    out_pose.reset(skeleton_pose::type::additive);
  }
  else {
    out_pose.reset(skeleton_pose::type::absolute);
  }

  // Select the checkpoint that serves this query time.
  // The binary search is bounded by ceil(log2(N + 1)) + 1 comparisons.
  const gsl::index checkpoint_index{select_checkpoint(_checkpoints, time_s,
                                                      _stats.checkpoint_comparisons)};
  const fixed_seek_checkpoint& checkpoint{_checkpoints[checkpoint_index]};

  const bool can_continue_forward{
      time_s >= _cursor.last_play_time_s && _cursor.last_play_time_s >= 0.0F &&
      _cursor.last_data_pos >= gsl::narrow<gsl::index>(checkpoint.start_pos)};

  if (!can_continue_forward) {
    // First query, a backwards jump, or a forward jump that overshoots the
    // checkpoint currently reached by the cursor. At most one restore.
    restore_checkpoint(checkpoint, _cursor);
    ++_stats.restored_checkpoints;
  }

  _cursor.last_play_time_s = time_s;

  gsl::index data_pos{_cursor.last_data_pos};

  gsl::index cursor_translation_index{0};
  gsl::index cursor_rotation_index{0};
  gsl::index cursor_scale_index{0};
  gsl::index metadata_index{0};

  const gsl::index data_size{std::ssize(_data)};

  // Same sequential decode as `clip_player_fixed`, starting either from the
  // restored checkpoint or from the cursor's current position.
  while (data_pos < data_size) {
    const uint16_t header{_data[data_pos]};

    const bool has_joint_index{has_flag(header, flags::has_joint_index)};
    const bool has_time{has_flag(header, flags::has_time)};
    const bool has_translation{has_flag(header, flags::has_translation)};
    const bool has_rotation{has_flag(header, flags::has_rotation)};
    const bool has_scale{has_flag(header, flags::has_scale)};

    EXPECTS(has_translation || has_rotation || has_scale);

    if (has_joint_index) {
      _cursor.last_data_joint_index = header >> 5;
    }

    cursor_component<float3>* translation{nullptr};
    cursor_component<quaternion>* rotation{nullptr};
    cursor_component<float3>* scale{nullptr};

    bool outdated{false};

    if (has_translation) {
      translation = &get_by_joint_index(_cursor.translations, cursor_translation_index,
                                        _cursor.last_data_joint_index);
      outdated |= cursor_component_is_outdated(*translation, time_s);
    }

    if (has_rotation) {
      rotation = &get_by_joint_index(_cursor.rotations, cursor_rotation_index,
                                     _cursor.last_data_joint_index);
      outdated |= cursor_component_is_outdated(*rotation, time_s);
    }

    if (has_scale) {
      scale = &get_by_joint_index(_cursor.scales, cursor_scale_index,
                                  _cursor.last_data_joint_index);
      outdated |= cursor_component_is_outdated(*scale, time_s);
    }

    if (!outdated) {
      // If this joint is not outdated, others are not as well.
      // This is guaranteed by the sorting.
      break;
    }

    ++_stats.decoded_records;
    ++data_pos;

    if (has_time) {
      _cursor.last_data_time_s = float_dequantize({.data = _data[data_pos],
                                                   .bits_count = 16,
                                                   .range_from = 0.0F,
                                                   .range_length = _metadata.duration_s});
      ++data_pos;
    }

    if (has_translation) {
      const joint_range& joint_metadata{get_by_joint_index(_metadata.joints_ranges, metadata_index,
                                                           _cursor.last_data_joint_index)};

      float_dequantize_params params{.bits_count = 16,
                                     .range_from = joint_metadata.range_translation_from,
                                     .range_length = joint_metadata.range_translation_length};

      params.data = _data[data_pos + 0];
      const float x{float_dequantize(params)};

      params.data = _data[data_pos + 1];
      const float y{float_dequantize(params)};

      params.data = _data[data_pos + 2];
      const float z{float_dequantize(params)};

      data_pos += 3;

      cursor_component_advance(*translation, float3{x, y, z}, _cursor.last_data_time_s);
    }

    if (has_rotation) {
      quaternion value{quaternion_dequantize(_data.subspan(data_pos, 4))};
      data_pos += 4;

      cursor_component_advance(*rotation, value, _cursor.last_data_time_s);
    }

    if (has_scale) {
      const joint_range& joint_metadata{get_by_joint_index(_metadata.joints_ranges, metadata_index,
                                                           _cursor.last_data_joint_index)};

      float_dequantize_params params{.bits_count = 16,
                                     .range_from = joint_metadata.range_scale_from,
                                     .range_length = joint_metadata.range_scale_length};

      params.data = _data[data_pos + 0];
      const float x{float_dequantize(params)};

      params.data = _data[data_pos + 1];
      const float y{float_dequantize(params)};

      params.data = _data[data_pos + 2];
      const float z{float_dequantize(params)};

      data_pos += 3;

      cursor_component_advance(*scale, float3{x, y, z}, _cursor.last_data_time_s);
    }
  }

  _cursor.last_data_pos = data_pos;

  cursor_calculate_pose(_cursor, time_s, out_pose);
}

fixed_seek_stats clip_player_fixed_seek::get_last_seek_stats() const
{
  return _stats;
}

fixed_seek_layout clip_player_fixed_seek::get_seek_layout() const
{
  return fixed_seek_layout{
      .encoded_records = static_cast<size_t>(_encoded_records),
      .component_count = static_cast<size_t>(_cursor.translations.size() +
                                             _cursor.rotations.size() + _cursor.scales.size()),
      .checkpoint_count = static_cast<size_t>(_checkpoints.size())};
}
}  // namespace eely::internal
