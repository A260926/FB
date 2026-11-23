#include "eely/clip/clip_impl_fixed_seek.h"

#include "eely/base/base_utils.h"
#include "eely/base/bit_reader.h"
#include "eely/clip/clip_cooking_none_fixed.h"
#include "eely/clip/clip_cursor.h"
#include "eely/clip/clip_impl_fixed.h"
#include "eely/clip/clip_player_fixed_seek.h"
#include "eely/clip/clip_utils.h"
#include "eely/math/quantization.h"
#include "eely/skeleton/skeleton_utils.h"

#include <gsl/narrow>
#include <gsl/util>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace eely::internal {
namespace {
// Number of records that have to be decoded after a checkpoint
// before another checkpoint is emitted.
constexpr gsl::index checkpoint_stride{32};

// Description of one cooked record's location in the `uint16_t` stream.
// A record is one header word plus its time/TRS payload.
struct record_info final {
  gsl::index start_pos{0};
  gsl::index end_pos{0};
  bool has_joint_index{false};
  bool has_time{false};
  bool has_translation{false};
  bool has_rotation{false};
  bool has_scale{false};
};

// Walk the stream and collect every record's boundaries.
std::vector<record_info> collect_records(std::span<const uint16_t> data)
{
  using flags = compression_key_flags;

  std::vector<record_info> result;

  gsl::index pos{0};
  const gsl::index data_size{std::ssize(data)};

  while (pos < data_size) {
    const gsl::index start_pos{pos};
    const uint16_t header{data[pos]};

    record_info info;
    info.start_pos = start_pos;
    info.has_joint_index = has_flag(header, flags::has_joint_index);
    info.has_time = has_flag(header, flags::has_time);
    info.has_translation = has_flag(header, flags::has_translation);
    info.has_rotation = has_flag(header, flags::has_rotation);
    info.has_scale = has_flag(header, flags::has_scale);

    EXPECTS(info.has_translation || info.has_rotation || info.has_scale);

    ++pos;
    if (info.has_time) {
      ++pos;
    }
    if (info.has_translation) {
      pos += 3;
    }
    if (info.has_rotation) {
      pos += 4;
    }
    if (info.has_scale) {
      pos += 3;
    }

    info.end_pos = pos;
    result.push_back(info);
  }

  return result;
}

// Number of animated component cursors (`C`).
gsl::index count_components(const std::vector<joint_components>& joints_components)
{
  gsl::index result{0};
  for (const joint_components& joint : joints_components) {
    if (has_flag(joint.components, transform_components::translation)) {
      ++result;
    }
    if (has_flag(joint.components, transform_components::rotation)) {
      ++result;
    }
    if (has_flag(joint.components, transform_components::scale)) {
      ++result;
    }
  }
  return result;
}

fixed_seek_component_state capture_float3_component(const cursor_component<float3>& component)
{
  fixed_seek_component_state state;
  state.left_time_s = component.left_time_s;
  state.right_time_s = component.right_time_s;
  state.left = {component.left.x, component.left.y, component.left.z, 0.0F};
  state.right = {component.right.x, component.right.y, component.right.z, 0.0F};
  return state;
}

fixed_seek_component_state capture_quaternion_component(
    const cursor_component<quaternion>& component)
{
  fixed_seek_component_state state;
  state.left_time_s = component.left_time_s;
  state.right_time_s = component.right_time_s;
  state.left = {component.left.x, component.left.y, component.left.z, component.left.w};
  state.right = {component.right.x, component.right.y, component.right.z, component.right.w};
  return state;
}

// Snapshot the decoder state right before the record at `next_record`
// is decoded. `next_record == records_count` captures the fully decoded state.
fixed_seek_checkpoint capture_checkpoint(const cursor& simulation_cursor,
                                         const std::vector<record_info>& records,
                                         gsl::index next_record,
                                         gsl::index component_count)
{
  fixed_seek_checkpoint checkpoint;

  checkpoint.start_pos = gsl::narrow<uint32_t>(
      next_record < std::ssize(records) ? records[next_record].start_pos
                                       : records.back().end_pos);
  checkpoint.current_joint_index =
      gsl::narrow<uint32_t>(simulation_cursor.last_data_joint_index ==
                                   std::numeric_limits<gsl::index>::max()
                               ? joints_max_count
                               : simulation_cursor.last_data_joint_index);
  checkpoint.current_time_s = simulation_cursor.last_data_time_s;
  checkpoint.components.resize(component_count);

  gsl::index component_index{0};
  for (const cursor_component<float3>& component : simulation_cursor.translations) {
    checkpoint.components[component_index++] = capture_float3_component(component);
  }
  for (const cursor_component<quaternion>& component : simulation_cursor.rotations) {
    checkpoint.components[component_index++] = capture_quaternion_component(component);
  }
  for (const cursor_component<float3>& component : simulation_cursor.scales) {
    checkpoint.components[component_index++] = capture_float3_component(component);
  }

  return checkpoint;
}

// Earliest time at which a record has to be decoded ("time to use").
// For every component it updates this is the time of that component's
// previous key (0.0 for its first key); the record itself is needed once
// the smallest of these times is strictly behind the query time.
float record_time_to_use(const cursor& simulation_cursor,
                         const record_info& record,
                         gsl::index& translation_index,
                         gsl::index& rotation_index,
                         gsl::index& scale_index,
                         gsl::index joint_index)
{
  float result{std::numeric_limits<float>::infinity()};

  if (record.has_translation) {
    const cursor_component<float3>& component{get_by_joint_index(
        simulation_cursor.translations, translation_index, joint_index)};
    result = std::min(result, component.right_time_s < 0.0F ? 0.0F : component.right_time_s);
  }

  if (record.has_rotation) {
    const cursor_component<quaternion>& component{get_by_joint_index(
        simulation_cursor.rotations, rotation_index, joint_index)};
    result = std::min(result, component.right_time_s < 0.0F ? 0.0F : component.right_time_s);
  }

  if (record.has_scale) {
    const cursor_component<float3>& component{
        get_by_joint_index(simulation_cursor.scales, scale_index, joint_index)};
    result = std::min(result, component.right_time_s < 0.0F ? 0.0F : component.right_time_s);
  }

  return result;
}

// Decode one record into the cursor, exactly like the forward fixed player.
void decode_record(const clip_metadata_fixed& metadata,
                   std::span<const uint16_t> data,
                   const record_info& record,
                   cursor& simulation_cursor,
                   gsl::index& translation_index,
                   gsl::index& rotation_index,
                   gsl::index& scale_index,
                   gsl::index& metadata_index)
{
  gsl::index pos{record.start_pos};
  const uint16_t header{data[pos]};

  if (record.has_joint_index) {
    simulation_cursor.last_data_joint_index = header >> 5;
  }

  ++pos;

  if (record.has_time) {
    simulation_cursor.last_data_time_s = float_dequantize({.data = data[pos],
                                                            .bits_count = 16,
                                                            .range_from = 0.0F,
                                                            .range_length = metadata.duration_s});
    ++pos;
  }

  if (record.has_translation) {
    cursor_component<float3>& component{get_by_joint_index(
        simulation_cursor.translations, translation_index,
        simulation_cursor.last_data_joint_index)};

    const joint_range& joint_metadata{get_by_joint_index(
        metadata.joints_ranges, metadata_index, simulation_cursor.last_data_joint_index)};

    float_dequantize_params params{.bits_count = 16,
                                   .range_from = joint_metadata.range_translation_from,
                                   .range_length = joint_metadata.range_translation_length};

    params.data = data[pos + 0];
    const float x{float_dequantize(params)};
    params.data = data[pos + 1];
    const float y{float_dequantize(params)};
    params.data = data[pos + 2];
    const float z{float_dequantize(params)};
    pos += 3;

    cursor_component_advance(component, float3{x, y, z}, simulation_cursor.last_data_time_s);
  }

  if (record.has_rotation) {
    cursor_component<quaternion>& component{get_by_joint_index(
        simulation_cursor.rotations, rotation_index, simulation_cursor.last_data_joint_index)};

    const quaternion value{quaternion_dequantize(data.subspan(pos, 4))};
    pos += 4;

    cursor_component_advance(component, value, simulation_cursor.last_data_time_s);
  }

  if (record.has_scale) {
    cursor_component<float3>& component{get_by_joint_index(
        simulation_cursor.scales, scale_index, simulation_cursor.last_data_joint_index)};

    const joint_range& joint_metadata{get_by_joint_index(
        metadata.joints_ranges, metadata_index, simulation_cursor.last_data_joint_index)};

    float_dequantize_params params{.bits_count = 16,
                                   .range_from = joint_metadata.range_scale_from,
                                   .range_length = joint_metadata.range_scale_length};

    params.data = data[pos + 0];
    const float x{float_dequantize(params)};
    params.data = data[pos + 1];
    const float y{float_dequantize(params)};
    params.data = data[pos + 2];
    const float z{float_dequantize(params)};
    pos += 3;

    cursor_component_advance(component, float3{x, y, z}, simulation_cursor.last_data_time_s);
  }

  EXPECTS(pos == record.end_pos);
}

// Cooking-time full decode: replay the whole stream once and take sparse
// snapshots of everything required to resume decoding at arbitrary positions.
std::vector<fixed_seek_checkpoint> build_checkpoints(const clip_metadata_fixed_seek& metadata,
                                                     std::span<const uint16_t> data)
{
  const std::vector<record_info> records{collect_records(data)};
  const gsl::index records_count{std::ssize(records)};
  EXPECTS(records_count > 0);

  const gsl::index component_count{count_components(metadata.joints_components)};

  cursor simulation_cursor;
  cursor_init(simulation_cursor, metadata.joints_components);

  // Record index at which each checkpoint starts decoding.
  std::vector<gsl::index> checkpoint_start_records;

  std::vector<fixed_seek_checkpoint> checkpoints;

  // First checkpoint: empty decoder state, right before the first record.
  checkpoints.push_back(capture_checkpoint(simulation_cursor, records, 0, component_count));
  checkpoint_start_records.push_back(0);

  std::vector<float> times_to_use;
  times_to_use.reserve(records_count);

  gsl::index translation_index{0};
  gsl::index rotation_index{0};
  gsl::index scale_index{0};
  gsl::index metadata_index{0};

  for (gsl::index record_index{0}; record_index < records_count; ++record_index) {
    const record_info& record{records[record_index]};

    gsl::index joint_index{simulation_cursor.last_data_joint_index};
    if (record.has_joint_index) {
      joint_index = data[record.start_pos] >> 5;
    }

    times_to_use.push_back(record_time_to_use(simulation_cursor, record, translation_index,
                                               rotation_index, scale_index, joint_index));

    decode_record(metadata, data, record, simulation_cursor, translation_index, rotation_index,
                  scale_index, metadata_index);

    const gsl::index next_record{record_index + 1};
    if (next_record < records_count && (next_record % checkpoint_stride) == 0) {
      checkpoints.push_back(
          capture_checkpoint(simulation_cursor, records, next_record, component_count));
      checkpoint_start_records.push_back(next_record);
    }
  }

  // Last checkpoint: state after the whole stream was decoded.
  checkpoints.push_back(
      capture_checkpoint(simulation_cursor, records, records_count, component_count));
  checkpoint_start_records.push_back(records_count);

  // Assign the time intervals each checkpoint is selected for.
  // `time_from_s` values are computed first because each checkpoint's
  // exclusive upper bound is the next checkpoint's lower bound.
  for (gsl::index checkpoint_index{0}; checkpoint_index < std::ssize(checkpoints);
       ++checkpoint_index) {
    fixed_seek_checkpoint& checkpoint{checkpoints[checkpoint_index]};
    const gsl::index start_record{checkpoint_start_records[checkpoint_index]};

    if (start_record == 0) {
      checkpoint.time_from_s = 0.0F;
    }
    else if (start_record >= records_count) {
      checkpoint.time_from_s = times_to_use.back();
    }
    else {
      checkpoint.time_from_s = times_to_use[start_record];
    }
  }

  for (gsl::index checkpoint_index{0}; checkpoint_index < std::ssize(checkpoints);
       ++checkpoint_index) {
    fixed_seek_checkpoint& checkpoint{checkpoints[checkpoint_index]};

    if (checkpoint_index + 1 < std::ssize(checkpoints)) {
      checkpoint.time_to_s = checkpoints[checkpoint_index + 1].time_from_s;
    }
    else {
      checkpoint.time_to_s = metadata.duration_s;
    }
  }

  return checkpoints;
}

void read_component_states(bit_reader& reader,
                           gsl::index count,
                           bool is_quaternion,
                           std::span<fixed_seek_component_state> out_components)
{
  for (gsl::index i{0}; i < count; ++i) {
    fixed_seek_component_state& state{out_components[i]};
    state.left_time_s = bit_reader_read<float>(reader);
    state.right_time_s = bit_reader_read<float>(reader);

    const gsl::index value_count{is_quaternion ? 4 : 3};
    for (gsl::index value_index{0}; value_index < value_count; ++value_index) {
      state.left[value_index] = bit_reader_read<float>(reader);
    }
    for (gsl::index value_index{0}; value_index < value_count; ++value_index) {
      state.right[value_index] = bit_reader_read<float>(reader);
    }
  }
}

void write_component_states(bit_writer& writer,
                            gsl::index count,
                            bool is_quaternion,
                            std::span<const fixed_seek_component_state> components)
{
  for (gsl::index i{0}; i < count; ++i) {
    const fixed_seek_component_state& state{components[i]};
    bit_writer_write(writer, state.left_time_s);
    bit_writer_write(writer, state.right_time_s);

    const gsl::index value_count{is_quaternion ? 4 : 3};
    for (gsl::index value_index{0}; value_index < value_count; ++value_index) {
      bit_writer_write(writer, state.left[value_index]);
    }
    for (gsl::index value_index{0}; value_index < value_count; ++value_index) {
      bit_writer_write(writer, state.right[value_index]);
    }
  }
}
}  // namespace

fixed_seek_checkpoint fixed_seek_checkpoint_read(bit_reader& reader,
                                                  const gsl::index translation_count,
                                                  const gsl::index rotation_count,
                                                  const gsl::index scale_count)
{
  fixed_seek_checkpoint checkpoint;

  checkpoint.time_from_s = bit_reader_read<float>(reader);
  checkpoint.time_to_s = bit_reader_read<float>(reader);
  checkpoint.start_pos = bit_reader_read<uint32_t>(reader, 32);

  const bool has_current_joint{bit_reader_read<bool>(reader)};
  if (has_current_joint) {
    checkpoint.current_joint_index =
        gsl::narrow<uint32_t>(bit_reader_read<gsl::index>(reader, bits_joints_count));
  }
  else {
    checkpoint.current_joint_index = gsl::narrow<uint32_t>(joints_max_count);
  }

  const bool has_current_time{bit_reader_read<bool>(reader)};
  if (has_current_time) {
    checkpoint.current_time_s = bit_reader_read<float>(reader);
  }
  else {
    checkpoint.current_time_s = -1.0F;
  }

  const gsl::index component_count{translation_count + rotation_count + scale_count};
  checkpoint.components.resize(component_count);

  read_component_states(reader, translation_count, false,
                        std::span{checkpoint.components.data(),
                                  gsl::narrow<size_t>(translation_count)});
  read_component_states(reader, rotation_count, true,
                        std::span{checkpoint.components.data() + translation_count,
                                  gsl::narrow<size_t>(rotation_count)});
  read_component_states(reader, scale_count, false,
                        std::span{checkpoint.components.data() + translation_count +
                                      rotation_count,
                                  gsl::narrow<size_t>(scale_count)});

  return checkpoint;
}

void fixed_seek_checkpoint_write(bit_writer& writer,
                                 const fixed_seek_checkpoint& checkpoint,
                                 const gsl::index translation_count,
                                 const gsl::index rotation_count,
                                 const gsl::index scale_count)
{
  bit_writer_write(writer, checkpoint.time_from_s);
  bit_writer_write(writer, checkpoint.time_to_s);
  bit_writer_write(writer, checkpoint.start_pos, 32);

  const bool has_current_joint{checkpoint.current_joint_index !=
                               gsl::narrow<uint32_t>(joints_max_count)};
  bit_writer_write(writer, has_current_joint);
  if (has_current_joint) {
    bit_writer_write(writer, gsl::narrow<gsl::index>(checkpoint.current_joint_index),
                     bits_joints_count);
  }

  const bool has_current_time{checkpoint.current_time_s >= 0.0F};
  bit_writer_write(writer, has_current_time);
  if (has_current_time) {
    bit_writer_write(writer, checkpoint.current_time_s);
  }

  write_component_states(writer, translation_count, false,
                         std::span{checkpoint.components.data(),
                                   gsl::narrow<size_t>(translation_count)});
  write_component_states(writer, rotation_count, true,
                         std::span{checkpoint.components.data() + translation_count,
                                   gsl::narrow<size_t>(rotation_count)});
  write_component_states(writer, scale_count, false,
                         std::span{checkpoint.components.data() + translation_count +
                                       rotation_count,
                                   gsl::narrow<size_t>(scale_count)});
}

clip_impl_fixed_seek::clip_impl_fixed_seek(bit_reader& reader)
{
  // Metadata (identical layout to `clip_impl_fixed`).

  _metadata.duration_s = bit_reader_read<float>(reader);
  EXPECTS(_metadata.duration_s >= 0.0F);

  _metadata.is_additive = bit_reader_read<bool>(reader);

  auto metadata_joint_components_size{bit_reader_read<gsl::index>(reader, bits_joints_count)};

  _metadata.joints_components.resize(metadata_joint_components_size);
  for (gsl::index i{0}; i < metadata_joint_components_size; ++i) {
    joint_components j;
    j.joint_index = bit_reader_read<gsl::index>(reader, bits_joints_count);
    j.components = bit_reader_read<int>(reader, bits_transform_components);

    _metadata.joints_components[i] = j;
  }

  const auto metadata_joint_ranges_size{bit_reader_read<gsl::index>(reader, bits_joints_count)};
  _metadata.joints_ranges.resize(metadata_joint_ranges_size);
  for (gsl::index i{0}; i < metadata_joint_ranges_size; ++i) {
    joint_range j;

    j.joint_index = bit_reader_read<gsl::index>(reader, bits_joints_count);
    j.range_translation_from = bit_reader_read<float>(reader);
    j.range_translation_length = bit_reader_read<float>(reader);
    j.range_scale_from = bit_reader_read<float>(reader);
    j.range_scale_length = bit_reader_read<float>(reader);

    _metadata.joints_ranges[i] = j;
  }

  // Quantized stream (identical layout to `clip_impl_fixed`).

  const auto data_size{bit_reader_read<gsl::index>(reader, 32)};
  EXPECTS(data_size > 0);

  _data.resize(data_size);
  for (gsl::index i{0}; i < data_size; ++i) {
    _data[i] = bit_reader_read<uint16_t>(reader);
  }

  // Seek index, field by field. The group counts are written once here,
  // every checkpoint reuses them.

  const auto encoded_records{bit_reader_read<uint32_t>(reader, 32)};
  _encoded_records = encoded_records;

  const auto translation_count{bit_reader_read<gsl::index>(reader, bits_joints_count)};
  const auto rotation_count{bit_reader_read<gsl::index>(reader, bits_joints_count)};
  const auto scale_count{bit_reader_read<gsl::index>(reader, bits_joints_count)};

  const auto checkpoint_count{bit_reader_read<uint32_t>(reader, 32)};
  _checkpoints.reserve(checkpoint_count);
  for (uint32_t i{0}; i < checkpoint_count; ++i) {
    _checkpoints.push_back(fixed_seek_checkpoint_read(reader, translation_count, rotation_count,
                                                      scale_count));
  }
}

clip_impl_fixed_seek::clip_impl_fixed_seek(const float duration_s,
                                           const std::vector<clip_uncooked_track>& tracks,
                                           const bool is_additive,
                                           const skeleton& skeleton)
{
  // Reuse the fixed cooking path verbatim: same metadata and same stream.
  clip_impl_fixed_cook(duration_s, tracks, is_additive, skeleton, _metadata, _data);

  _checkpoints = build_checkpoints(_metadata, _data);

  // Documented size invariants.
  _encoded_records = gsl::narrow<uint32_t>(collect_records(_data).size());
  const gsl::index records_count{gsl::narrow<gsl::index>(_encoded_records)};
  const gsl::index checkpoint_bound{records_count / checkpoint_stride + 2};
  EXPECTS(std::ssize(_checkpoints) <= checkpoint_bound);
  EXPECTS(std::ssize(_checkpoints) >= 2);
}

void clip_impl_fixed_seek::serialize(bit_writer& writer) const
{
  // Metadata (identical layout to `clip_impl_fixed`).

  bit_writer_write(writer, _metadata.duration_s);
  bit_writer_write(writer, _metadata.is_additive);

  bit_writer_write(writer, _metadata.joints_components.size(), bits_joints_count);
  for (const joint_components& j : _metadata.joints_components) {
    bit_writer_write(writer, j.joint_index, bits_joints_count);
    bit_writer_write(writer, j.components, bits_transform_components);
  }

  bit_writer_write(writer, _metadata.joints_ranges.size(), bits_joints_count);
  for (const joint_range& j : _metadata.joints_ranges) {
    bit_writer_write(writer, j.joint_index, bits_joints_count);

    bit_writer_write(writer, j.range_translation_from);
    bit_writer_write(writer, j.range_translation_length);
    bit_writer_write(writer, j.range_scale_from);
    bit_writer_write(writer, j.range_scale_length);
  }

  // Quantized stream (identical layout to `clip_impl_fixed`).

  bit_writer_write(writer, _data.size(), 32);
  for (uint16_t d : _data) {
    bit_writer_write(writer, d);
  }

  // Seek index, field by field.

  const std::vector<record_info> records{collect_records(_data)};
  bit_writer_write(writer, gsl::narrow<uint32_t>(records.size()), 32);

  gsl::index translation_count{0};
  gsl::index rotation_count{0};
  gsl::index scale_count{0};
  for (const joint_components& joint : _metadata.joints_components) {
    if (has_flag(joint.components, transform_components::translation)) {
      ++translation_count;
    }
    if (has_flag(joint.components, transform_components::rotation)) {
      ++rotation_count;
    }
    if (has_flag(joint.components, transform_components::scale)) {
      ++scale_count;
    }
  }

  bit_writer_write(writer, translation_count, bits_joints_count);
  bit_writer_write(writer, rotation_count, bits_joints_count);
  bit_writer_write(writer, scale_count, bits_joints_count);

  bit_writer_write(writer, gsl::narrow<uint32_t>(_checkpoints.size()), 32);

  for (const fixed_seek_checkpoint& checkpoint : _checkpoints) {
    fixed_seek_checkpoint_write(writer, checkpoint, translation_count, rotation_count,
                                scale_count);
  }
}

const clip_metadata_base* clip_impl_fixed_seek::get_metadata() const
{
  return &_metadata;
}

std::unique_ptr<clip_player_base> clip_impl_fixed_seek::create_player() const
{
  return std::make_unique<clip_player_fixed_seek>(_metadata, _data, _checkpoints,
                                                  _encoded_records);
}

const clip_metadata_fixed_seek& clip_impl_fixed_seek::get_metadata_fixed_seek() const
{
  return _metadata;
}

std::span<const uint16_t> clip_impl_fixed_seek::get_data() const
{
  return _data;
}

std::span<const fixed_seek_checkpoint> clip_impl_fixed_seek::get_checkpoints() const
{
  return _checkpoints;
}
}  // namespace eely::internal
