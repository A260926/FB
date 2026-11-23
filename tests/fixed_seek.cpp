#include "task_support/check.h"

#include "eely/anim_graph/anim_graph.h"
#include "eely/anim_graph/anim_graph_node_clip.h"
#include "eely/anim_graph/anim_graph_node_sum.h"
#include "eely/anim_graph/anim_graph_player.h"
#include "eely/anim_graph/anim_graph_uncooked.h"
#include "eely/clip/clip.h"
#include "eely/clip/clip_compression_scheme.h"
#include "eely/clip/clip_impl_fixed.h"
#include "eely/clip/clip_impl_fixed_seek.h"
#include "eely/clip/clip_player_fixed_seek.h"
#include "eely/clip/clip_uncooked.h"
#include "eely/math/float3.h"
#include "eely/math/math_utils.h"
#include "eely/math/quaternion.h"
#include "eely/math/transform.h"
#include "eely/params/params.h"
#include "eely/project/axis_system.h"
#include "eely/project/measurement_unit.h"
#include "eely/project/project.h"
#include "eely/project/project_uncooked.h"
#include "eely/skeleton/skeleton.h"
#include "eely/skeleton/skeleton_pose.h"
#include "eely/skeleton/skeleton_uncooked.h"

#include <cmath>
#include <cstdlib>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace eely;
using namespace eely::internal;

namespace {
constexpr float test_epsilon{1e-5F};

bool near_float3(const float3& a, const float3& b, const float epsilon = test_epsilon)
{
  return float3_near(a, b, epsilon);
}

// Rotations are compared up to the equivalent direction (negated quaternion).
bool near_quaternion_equiv(const quaternion& a, const quaternion& b,
                           const float epsilon = test_epsilon)
{
  const auto distance = [](const quaternion& x, const quaternion& y) {
    return std::fabs(x.x - y.x) + std::fabs(x.y - y.y) + std::fabs(x.z - y.z) +
           std::fabs(x.w - y.w);
  };

  const quaternion negated{-b.x, -b.y, -b.z, -b.w};
  return distance(a, b) <= epsilon || distance(a, negated) <= epsilon;
}

bool poses_equal(const skeleton_pose& a, const skeleton_pose& b)
{
  if (a.get_joints_count() != b.get_joints_count()) {
    return false;
  }

  for (gsl::index joint_index{0}; joint_index < a.get_joints_count(); ++joint_index) {
    const transform& ta{a.get_transform_joint_space(joint_index)};
    const transform& tb{b.get_transform_joint_space(joint_index)};

    if (!near_float3(ta.translation, tb.translation)) {
      return false;
    }
    if (!near_quaternion_equiv(ta.rotation, tb.rotation)) {
      return false;
    }
    if (!near_float3(ta.scale, tb.scale)) {
      return false;
    }
  }

  return true;
}

// Build a skeleton with a linear parent chain and non-trivial rest poses.
skeleton_uncooked& make_chain_skeleton(project_uncooked& input, const std::string& id,
                                       gsl::index joints_count)
{
  auto& skeleton = input.add_resource<skeleton_uncooked>(id);
  for (gsl::index i{0}; i < joints_count; ++i) {
    skeleton.get_joints().push_back(
        {.id = "j" + std::to_string(i),
         .parent_index = i == 0 ? std::nullopt : std::optional<gsl::index>{i - 1},
         .rest_pose_transform =
             transform{.translation = float3{0.0F, static_cast<float>(i) + 1.0F, 0.0F}}});
  }
  return skeleton;
}

// Dense, irregular, partial-TRS tracks.
// Every changing channel has keys at 0 and duration, key times stay strictly
// increasing after quantization, and many joints share time thresholds.
std::vector<clip_uncooked_track> make_complex_tracks(gsl::index joints_count, float duration,
                                                     unsigned seed)
{
  std::mt19937 gen(seed);
  std::uniform_real_distribution<float> unit(-1.0F, 1.0F);

  std::vector<clip_uncooked_track> tracks;

  for (gsl::index joint_index{0}; joint_index < joints_count; ++joint_index) {
    clip_uncooked_track track;
    track.joint_id = "j" + std::to_string(joint_index);

    const gsl::index key_count{18 + joint_index * 5};
    const bool has_translation{joint_index != 1};
    const bool has_rotation{true};
    const bool has_scale{joint_index % 2 == 0};

    auto fill_key = [&](float time) {
      clip_uncooked_key key;

      if (has_translation) {
        key.translation = float3{unit(gen), unit(gen), unit(gen)};
      }
      if (has_rotation) {
        key.rotation =
            quaternion_from_yaw_pitch_roll_intrinsic(unit(gen), unit(gen), unit(gen));
      }
      if (has_scale) {
        // Non-unit, non-uniform scales.
        key.scale = float3{1.0F + 0.4F * unit(gen), 1.0F + 0.25F * unit(gen),
                           1.0F + 0.6F * unit(gen)};
      }

      track.keys[time] = key;
    };

    // Irregular per-joint key grid.
    for (gsl::index key_index{0}; key_index < key_count; ++key_index) {
      const float time{duration * static_cast<float>(key_index) /
                       static_cast<float>(key_count - 1)};
      fill_key(time);
    }

    // Shared thresholds across every joint: many joints update at once.
    for (const float fraction : {0.25F, 0.5F, 0.75F}) {
      fill_key(duration * fraction);
    }

    tracks.push_back(std::move(track));
  }

  return tracks;
}

struct cooked_clips final {
  std::vector<std::byte> storage;
  std::unique_ptr<project> project_owner;
  const clip* fixed_clip{nullptr};
  const clip* seek_clip{nullptr};
  const skeleton* rig{nullptr};
};

// Cook the same tracks twice: once as `fixed`, once as `fixed_seek`.
cooked_clips cook_pair(const std::vector<clip_uncooked_track>& tracks,
                       clip_compression_scheme reference_scheme = clip_compression_scheme::fixed)
{
  cooked_clips result;
  result.storage.resize(8 * 1024 * 1024);

  constexpr gsl::index joints_count{4};

  {
    project_uncooked input{measurement_unit::meters, axis_system::y_up_x_right_z_forward};
    make_chain_skeleton(input, "rig", joints_count);

    auto& fixed_uncooked = input.add_resource<clip_uncooked>("clip_fixed");
    fixed_uncooked.set_target_skeleton_id("rig");
    fixed_uncooked.set_compression_scheme(reference_scheme);
    fixed_uncooked.set_tracks(tracks);

    auto& seek_uncooked = input.add_resource<clip_uncooked>("clip_seek");
    seek_uncooked.set_target_skeleton_id("rig");
    seek_uncooked.set_compression_scheme(clip_compression_scheme::fixed_seek);
    seek_uncooked.set_tracks(tracks);

    project::cook(input, std::span{result.storage});
  }

  result.project_owner = std::make_unique<project>(std::span{result.storage});
  result.rig = result.project_owner->get_resource<skeleton>("rig");
  result.fixed_clip = result.project_owner->get_resource<clip>("clip_fixed");
  result.seek_clip = result.project_owner->get_resource<clip>("clip_seek");
  return result;
}
}  // namespace

// ============================================================================
// checkpoints group
// ============================================================================

static int check_layout_bounds(void)
{
  const auto tracks{make_complex_tracks(4, 10.0F, 11)};
  cooked_clips cooked{cook_pair(tracks)};

  std::unique_ptr<clip_player_base> holder{cooked.seek_clip->create_player()};
  auto* seek_player = dynamic_cast<clip_player_fixed_seek*>(holder.get());
  TASK_CHECK(seek_player != nullptr);

  const fixed_seek_layout layout{seek_player->get_seek_layout()};

  // Dense irregular input must produce more than one stride of records.
  TASK_CHECK(layout.encoded_records > 32);
  TASK_CHECK(layout.component_count > 0);
  TASK_CHECK(layout.component_count <= 12);

  // N <= ceil(P / 32) + 2.
  const size_t checkpoint_bound{(layout.encoded_records + 31) / 32 + 2};
  TASK_CHECK(layout.checkpoint_count >= 2);
  TASK_CHECK(layout.checkpoint_count <= checkpoint_bound);

  // Storage is O((C + 1) * (ceil(P / 32) + 1)); the stride above is the
  // observable part of that bound.
  TASK_CHECK(layout.checkpoint_count <=
             (layout.encoded_records + 31) / 32 + 1 + 1);

  return 0;
}

static int check_serialization_roundtrip(void)
{
  const auto tracks{make_complex_tracks(4, 10.0F, 22)};
  cooked_clips cooked{cook_pair(tracks)};

  std::unique_ptr<clip_player_base> fixed_holder{cooked.fixed_clip->create_player()};
  std::unique_ptr<clip_player_base> seek_holder{cooked.seek_clip->create_player()};
  auto* seek_player = dynamic_cast<clip_player_fixed_seek*>(seek_holder.get());
  TASK_CHECK(seek_player != nullptr);

  // The cooked project was built from a buffer after the uncooked project went
  // out of scope: loading must not retain uncooked data, and the index must be
  // fully available from serialized fields alone.
  const fixed_seek_layout layout{seek_player->get_seek_layout()};
  TASK_CHECK(layout.encoded_records > 0);
  TASK_CHECK(layout.checkpoint_count >= 2);
  TASK_CHECK(layout.component_count > 0);

  skeleton_pose pose_fixed{*cooked.rig};
  skeleton_pose pose_seek{*cooked.rig};

  // Endpoints and a midpoint must already be servable after a plain load.
  for (const float time : {0.0F, 0.37F, 10.0F}) {
    fixed_holder->play(time, pose_fixed);
    seek_player->play(time, pose_seek);
    TASK_CHECK(poses_equal(pose_fixed, pose_seek));
  }

  return 0;
}

static int check_quantized_stream_identical(void)
{
  // The new scheme must store the same uint16_t quantized stream as `fixed`
  // for the same input; only the extra seek index is allowed to differ.
  constexpr gsl::index joints_count{3};
  constexpr float duration{8.0F};
  const auto tracks{make_complex_tracks(joints_count, duration, 88)};

  std::vector<std::byte> storage(4 * 1024 * 1024);
  const skeleton* rig{nullptr};

  {
    project_uncooked input{measurement_unit::meters, axis_system::y_up_x_right_z_forward};
    make_chain_skeleton(input, "rig", joints_count);

    auto& uncooked = input.add_resource<clip_uncooked>("only_needed_to_cook_skeleton");
    uncooked.set_target_skeleton_id("rig");
    uncooked.set_compression_scheme(clip_compression_scheme::fixed);
    uncooked.set_tracks(tracks);

    project::cook(input, std::span{storage});
  }

  project cooked{std::span{storage}};
  rig = cooked.get_resource<skeleton>("rig");
  TASK_CHECK(rig != nullptr);

  // Build both implementations directly from the same reduced inputs.
  clip_impl_fixed fixed_impl{duration, tracks, false, *rig};
  clip_impl_fixed_seek seek_impl{duration, tracks, false, *rig};

  const std::span<const uint16_t> fixed_stream{fixed_impl.get_data()};
  const std::span<const uint16_t> seek_stream{seek_impl.get_data()};

  TASK_CHECK(!fixed_stream.empty());
  TASK_CHECK(fixed_stream.size() == seek_stream.size());

  bool identical{true};
  for (size_t i{0}; i < fixed_stream.size(); ++i) {
    if (fixed_stream[i] != seek_stream[i]) {
      identical = false;
      break;
    }
  }
  TASK_CHECK(identical);

  // The seek index is present and bounded, while metadata stays shared.
  const auto checkpoints{seek_impl.get_checkpoints()};
  TASK_CHECK(checkpoints.size() >= 2);
  TASK_CHECK(seek_impl.get_metadata()->duration_s == fixed_impl.get_metadata()->duration_s);
  TASK_CHECK(seek_impl.get_metadata()->is_additive ==
             fixed_impl.get_metadata()->is_additive);

  return 0;
}

// Global allocation counters; only differences around `play` are inspected.
static unsigned g_operator_new_calls{0};
static unsigned g_operator_new_array_calls{0};

static int check_no_heap_during_play(void)
{
  const auto tracks{make_complex_tracks(4, 10.0F, 33)};
  cooked_clips cooked{cook_pair(tracks)};

  std::unique_ptr<clip_player_base> holder{cooked.seek_clip->create_player()};
  auto* seek_player = dynamic_cast<clip_player_fixed_seek*>(holder.get());
  TASK_CHECK(seek_player != nullptr);

  skeleton_pose pose{*cooked.rig};

  // Warm up the pose's lazily allocated object-space cache and every vector
  // capacity, so the measured region only contains `play` itself.
  seek_player->play(0.0F, pose);
  (void)pose.get_transform_object_space(0);
  seek_player->play(10.0F, pose);
  (void)pose.get_transform_object_space(0);

  const float query_times[]{0.0F, 9.99F, 0.01F, 5.0F, 5.0F, 10.0F, 2.5F, 0.0F};
  for (const float time : query_times) {
    const unsigned before_scalar{g_operator_new_calls};
    const unsigned before_array{g_operator_new_array_calls};

    seek_player->play(time, pose);
    // Touching object-space transforms must not allocate either.
    (void)pose.get_transform_object_space(0);

    TASK_CHECK(g_operator_new_calls == before_scalar);
    TASK_CHECK(g_operator_new_array_calls == before_array);
  }

  return 0;
}

static int check_operation_bounds(void)
{
  const auto tracks{make_complex_tracks(4, 10.0F, 44)};
  cooked_clips cooked{cook_pair(tracks)};

  std::unique_ptr<clip_player_base> holder{cooked.seek_clip->create_player()};
  auto* seek_player = dynamic_cast<clip_player_fixed_seek*>(holder.get());
  TASK_CHECK(seek_player != nullptr);

  const fixed_seek_layout layout{seek_player->get_seek_layout()};
  const size_t decode_bound{32 + 2 * layout.component_count};
  const size_t comparison_bound{static_cast<size_t>(std::ceil(
                                    std::log2(static_cast<double>(layout.checkpoint_count + 1)))) +
                                1};

  skeleton_pose pose{*cooked.rig};

  // First query near the end, reverse, far jumps, repeats, endpoints.
  const std::vector<float> query_times{10.0F, 9.75F, 0.25F, 8.0F, 8.0F, 0.0F,
                                       4.3F,  4.3F,  6.1F,  0.0F, 10.0F};

  for (const float time : query_times) {
    const fixed_seek_stats before{seek_player->get_last_seek_stats()};
    seek_player->play(time, pose);
    const fixed_seek_stats after{seek_player->get_last_seek_stats()};

    const size_t decoded{after.decoded_records - before.decoded_records};
    const size_t comparisons{after.checkpoint_comparisons - before.checkpoint_comparisons};
    const size_t restores{after.restored_checkpoints - before.restored_checkpoints};

    TASK_CHECK(decoded <= decode_bound);
    TASK_CHECK(comparisons <= comparison_bound);
    TASK_CHECK(restores <= 1);
  }

  return 0;
}

// ============================================================================
// seek_values group
// ============================================================================

static int check_matches_fixed_reference(void)
{
  const auto tracks{make_complex_tracks(4, 10.0F, 55)};
  cooked_clips cooked{cook_pair(tracks)};

  std::unique_ptr<clip_player_base> fixed_holder{cooked.fixed_clip->create_player()};
  std::unique_ptr<clip_player_base> seek_holder{cooked.seek_clip->create_player()};
  auto* seek_player = dynamic_cast<clip_player_fixed_seek*>(seek_holder.get());
  TASK_CHECK(seek_player != nullptr);

  skeleton_pose pose_fixed{*cooked.rig};
  skeleton_pose pose_seek{*cooked.rig};

  std::vector<float> times{0.0F, 10.0F, 9.999F, 0.001F, 5.0F, 5.0F};
  std::mt19937 gen(99);
  std::uniform_real_distribution<float> distr(0.0F, 1.0F);
  for (int i{0}; i < 120; ++i) {
    times.push_back(10.0F * distr(gen));
  }

  for (const float time : times) {
    fixed_holder->play(time, pose_fixed);
    seek_player->play(time, pose_seek);
    TASK_CHECK(poses_equal(pose_fixed, pose_seek));
  }

  return 0;
}

static int check_hand_calculated_quantized_keys(void)
{
  // Endpoint values land exactly on quantization range edges
  // (min -> index 0, max -> index 65535), so interpolated values at simple
  // fractions are exact and can be checked by hand.
  std::vector<std::byte> storage(2 * 1024 * 1024);
  const clip* seek_clip{nullptr};
  const skeleton* rig{nullptr};

  {
    project_uncooked input{measurement_unit::meters, axis_system::y_up_x_right_z_forward};
    make_chain_skeleton(input, "rig", 1);

    auto& uncooked = input.add_resource<clip_uncooked>("hand");
    uncooked.set_target_skeleton_id("rig");
    uncooked.set_compression_scheme(clip_compression_scheme::fixed_seek);
    uncooked.set_tracks(
        {{.joint_id = "j0",
          .keys = {{0.0F,
                    {.translation = float3{2.0F, 4.0F, 6.0F},
                     .scale = float3{1.0F, 1.0F, 1.0F}}},
                   {10.0F,
                    {.translation = float3{8.0F, 2.0F, 4.0F},
                     .scale = float3{3.0F, 5.0F, 7.0F}}}}}});

    project::cook(input, std::span{storage});
  }

  project cooked{std::span{storage}};
  rig = cooked.get_resource<skeleton>("rig");
  seek_clip = cooked.get_resource<clip>("hand");
  TASK_CHECK(seek_clip != nullptr);

  std::unique_ptr<clip_player_base> holder{seek_clip->create_player()};
  auto* seek_player = dynamic_cast<clip_player_fixed_seek*>(holder.get());
  TASK_CHECK(seek_player != nullptr);

  skeleton_pose pose{*rig};

  struct expectation final {
    float time;
    float3 translation;
    float3 scale;
  };

  const expectation expectations[]{
      {0.0F, float3{2.0F, 4.0F, 6.0F}, float3{1.0F, 1.0F, 1.0F}},
      {2.5F, float3{3.5F, 3.5F, 5.5F}, float3{1.5F, 2.0F, 2.5F}},
      {5.0F, float3{5.0F, 3.0F, 5.0F}, float3{2.0F, 3.0F, 4.0F}},
      {10.0F, float3{8.0F, 2.0F, 4.0F}, float3{3.0F, 5.0F, 7.0F}},
  };

  for (const expectation& expected : expectations) {
    seek_player->play(expected.time, pose);
    const transform& actual{pose.get_transform_joint_space(0)};
    TASK_CHECK(near_float3(actual.translation, expected.translation, 1e-6F));
    TASK_CHECK(near_float3(actual.scale, expected.scale, 1e-6F));
    // Unanimated rotation stays at the rest pose.
    TASK_CHECK(near_quaternion_equiv(actual.rotation, quaternion::identity, 1e-6F));
  }

  return 0;
}

static int check_edge_and_reverse_queries(void)
{
  const auto tracks{make_complex_tracks(4, 10.0F, 66)};
  cooked_clips cooked{cook_pair(tracks)};

  std::unique_ptr<clip_player_base> fixed_holder{cooked.fixed_clip->create_player()};
  std::unique_ptr<clip_player_base> seek_holder{cooked.seek_clip->create_player()};
  auto* seek_player = dynamic_cast<clip_player_fixed_seek*>(seek_holder.get());
  TASK_CHECK(seek_player != nullptr);

  skeleton_pose pose_fixed{*cooked.rig};
  skeleton_pose pose_seek{*cooked.rig};

  // First query lands near the end.
  const std::vector<float> sequence{9.9F, 10.0F, 0.0F, 0.1F, 7.7F, 7.7F,
                                    0.0F, 0.0F,  5.0F, 10.0F};
  for (const float time : sequence) {
    fixed_holder->play(time, pose_fixed);
    seek_player->play(time, pose_seek);
    TASK_CHECK(poses_equal(pose_fixed, pose_seek));
  }

  const fixed_seek_stats stats{seek_player->get_last_seek_stats()};
  TASK_CHECK(stats.restored_checkpoints > 0);
  TASK_CHECK(stats.decoded_records > 0);
  TASK_CHECK(stats.checkpoint_comparisons > 0);

  return 0;
}

static int check_rest_and_missing_channels(void)
{
  std::vector<std::byte> storage(2 * 1024 * 1024);
  const clip* fixed_clip{nullptr};
  const clip* seek_clip{nullptr};
  const skeleton* rig{nullptr};

  {
    project_uncooked input{measurement_unit::meters, axis_system::y_up_x_right_z_forward};
    make_chain_skeleton(input, "rig", 3);

    // j1 stays on the rest pose for every key: its whole track is removed
    // during cooking (rest channel rule). j0 animates translation only, so
    // rotation and scale channels are absent.
    const std::vector<clip_uncooked_track> tracks{
        {.joint_id = "j0",
         .keys = {{0.0F, {.translation = float3{1.0F, 0.0F, 0.0F}}},
                  {5.0F, {.translation = float3{2.0F, 0.0F, 0.0F}}},
                  {10.0F, {.translation = float3{3.0F, 0.0F, 0.0F}}}}},
        {.joint_id = "j1",
         .keys = {{0.0F, {.translation = float3{0.0F, 2.0F, 0.0F}}},
                  {10.0F, {.translation = float3{0.0F, 2.0F, 0.0F}}}}},
        {.joint_id = "j2",
         .keys = {{0.0F, {.rotation = quaternion_from_axis_angle(0.0F, 1.0F, 0.0F, 0.4F)}},
                  {10.0F, {.rotation = quaternion_from_axis_angle(0.0F, 1.0F, 0.0F, 0.8F)}}}}};

    auto& fixed_uncooked = input.add_resource<clip_uncooked>("f");
    fixed_uncooked.set_target_skeleton_id("rig");
    fixed_uncooked.set_compression_scheme(clip_compression_scheme::fixed);
    fixed_uncooked.set_tracks(tracks);

    auto& seek_uncooked = input.add_resource<clip_uncooked>("s");
    seek_uncooked.set_target_skeleton_id("rig");
    seek_uncooked.set_compression_scheme(clip_compression_scheme::fixed_seek);
    seek_uncooked.set_tracks(tracks);

    project::cook(input, std::span{storage});
  }

  project cooked{std::span{storage}};
  rig = cooked.get_resource<skeleton>("rig");
  fixed_clip = cooked.get_resource<clip>("f");
  seek_clip = cooked.get_resource<clip>("s");

  std::unique_ptr<clip_player_base> fixed_holder{fixed_clip->create_player()};
  std::unique_ptr<clip_player_base> seek_holder{seek_clip->create_player()};
  auto* seek_player = dynamic_cast<clip_player_fixed_seek*>(seek_holder.get());
  TASK_CHECK(seek_player != nullptr);

  const fixed_seek_layout layout{seek_player->get_seek_layout()};
  // j1 contributes nothing, j0 contributes one component, j2 one component.
  TASK_CHECK(layout.component_count == 2);

  skeleton_pose pose_fixed{*rig};
  skeleton_pose pose_seek{*rig};

  for (const float time : {0.0F, 0.7F, 5.0F, 9.2F, 10.0F, 0.0F}) {
    fixed_holder->play(time, pose_fixed);
    seek_player->play(time, pose_seek);
    TASK_CHECK(poses_equal(pose_fixed, pose_seek));
  }

  return 0;
}

// ============================================================================
// pipeline group
// ============================================================================

static int check_cook_load_two_players(void)
{
  const auto tracks{make_complex_tracks(4, 10.0F, 77)};
  cooked_clips cooked{cook_pair(tracks)};

  // Two independent players over one shared, read-only cooked clip.
  std::unique_ptr<clip_player_base> holder_a{cooked.seek_clip->create_player()};
  std::unique_ptr<clip_player_base> holder_b{cooked.seek_clip->create_player()};
  auto* player_a = dynamic_cast<clip_player_fixed_seek*>(holder_a.get());
  auto* player_b = dynamic_cast<clip_player_fixed_seek*>(holder_b.get());
  TASK_CHECK(player_a != nullptr);
  TASK_CHECK(player_b != nullptr);

  std::unique_ptr<clip_player_base> reference{cooked.fixed_clip->create_player()};

  skeleton_pose pose_a{*cooked.rig};
  skeleton_pose pose_b{*cooked.rig};
  skeleton_pose pose_ref{*cooked.rig};

  // Interleaved jumps in opposite directions.
  const std::vector<std::pair<float, float>> interleaved{
      {9.5F, 0.5F}, {0.2F, 8.8F}, {7.0F, 3.0F}, {10.0F, 0.0F}, {4.4F, 4.4F}};

  for (const auto& [time_a, time_b] : interleaved) {
    player_a->play(time_a, pose_a);
    player_b->play(time_b, pose_b);

    reference->play(time_a, pose_ref);
    TASK_CHECK(poses_equal(pose_a, pose_ref));

    reference->play(time_b, pose_ref);
    TASK_CHECK(poses_equal(pose_b, pose_ref));
  }

  // Repeating the same time on both keeps producing identical poses.
  player_a->play(6.0F, pose_a);
  player_b->play(6.0F, pose_b);
  TASK_CHECK(poses_equal(pose_a, pose_b));

  return 0;
}

static std::vector<std::byte> build_graph_project(clip_compression_scheme scheme)
{
  std::vector<std::byte> storage(8 * 1024 * 1024);

  project_uncooked input{measurement_unit::meters, axis_system::y_up_x_right_z_forward};
  make_chain_skeleton(input, "rig", 3);

  auto& base_clip = input.add_resource<clip_uncooked>("walk");
  base_clip.set_target_skeleton_id("rig");
  base_clip.set_compression_scheme(scheme);
  // Rotating joints use uniform non-unit scales: the current `transform_inverse`
  // only handles non-uniform scales exactly when the joint itself does not
  // rotate (see the SRT/TODO note in transform.h).
  base_clip.set_tracks(
      {{.joint_id = "j0",
        .keys = {{0.0F, {.translation = float3{0.1F, 0.0F, 0.0F},
                        .rotation = quaternion_from_yaw_pitch_roll_intrinsic(0.2F, 0.0F, 0.0F),
                        .scale = float3{1.2F, 1.2F, 1.2F}}},
                 {10.0F, {.translation = float3{2.0F, 0.0F, 0.0F},
                          .rotation = quaternion_from_yaw_pitch_roll_intrinsic(1.1F, 0.0F, 0.0F),
                          .scale = float3{1.8F, 1.8F, 1.8F}}}}},
       {.joint_id = "j1",
        .keys = {{0.0F, {.rotation = quaternion_from_yaw_pitch_roll_intrinsic(0.0F, 0.3F, 0.0F),
                        .scale = float3{0.9F, 0.9F, 0.9F}}},
                 {10.0F, {.rotation = quaternion_from_yaw_pitch_roll_intrinsic(0.0F, 0.9F, 0.2F),
                          .scale = float3{1.3F, 1.3F, 1.3F}}}}},
       // Non-uniform, non-unit scale on the non-rotating tip joint: it still
       // flows through the parent rotations into object-space output.
       {.joint_id = "j2",
        .keys = {{0.0F, {.translation = float3{0.0F, 0.0F, 0.5F},
                        .scale = float3{1.0F, 1.4F, 1.1F}}},
                 {10.0F, {.translation = float3{0.0F, 0.0F, 2.5F},
                          .scale = float3{1.0F, 0.7F, 1.6F}}}}}});

  const std::vector<clip_uncooked_track> source_tracks{
      {.joint_id = "j1",
       .keys = {{0.0F,
                 {.rotation = quaternion_from_yaw_pitch_roll_intrinsic(0.0F, 0.0F, 0.2F)}},
                {10.0F,
                 {.rotation = quaternion_from_yaw_pitch_roll_intrinsic(0.0F, 0.0F, 0.7F)}}}}};

  auto& source_clip = input.add_resource<clip_uncooked>("look_src");
  source_clip.set_target_skeleton_id("rig");
  source_clip.set_compression_scheme(scheme);
  source_clip.set_tracks(source_tracks);

  auto& additive = input.add_resource<clip_additive_uncooked>("look_add");
  additive.set_target_skeleton_id("rig");
  additive.set_base_clip_id("walk");
  additive.set_source_clip_id("look_src");
  additive.set_compression_scheme(scheme);

  auto& graph_uncooked = input.add_resource<anim_graph_uncooked>("graph");
  graph_uncooked.set_skeleton_id("rig");

  auto& base_node = graph_uncooked.add_node<anim_graph_node_clip>();
  base_node.set_clip_id("walk");

  auto& additive_node = graph_uncooked.add_node<anim_graph_node_clip>();
  additive_node.set_clip_id("look_add");

  auto& sum_node = graph_uncooked.add_node<anim_graph_node_sum>();
  sum_node.set_first_node_id(base_node.get_id());
  sum_node.set_second_node_id(additive_node.get_id());
  graph_uncooked.set_root_node_id(sum_node.get_id());

  project::cook(input, std::span{storage});
  return storage;
}

static int check_graph_additive_hierarchy(void)
{
  // Reference graph uses the legacy fixed scheme; the target graph uses
  // fixed_seek for both the ordinary and the additive clip.
  std::vector<std::byte> fixed_storage{build_graph_project(clip_compression_scheme::fixed)};
  std::vector<std::byte> seek_storage{
      build_graph_project(clip_compression_scheme::fixed_seek)};

  project fixed_project{std::span{fixed_storage}};
  project seek_project{std::span{seek_storage}};

  const auto* fixed_graph = fixed_project.get_resource<anim_graph>("graph");
  const auto* seek_graph = seek_project.get_resource<anim_graph>("graph");
  const auto* seek_rig = seek_project.get_resource<skeleton>("rig");
  const auto* fixed_rig = fixed_project.get_resource<skeleton>("rig");
  TASK_CHECK(fixed_graph != nullptr && seek_graph != nullptr);
  TASK_CHECK(seek_rig != nullptr && fixed_rig != nullptr);

  anim_graph_player fixed_player{*fixed_graph};
  anim_graph_player seek_player{*seek_graph};

  skeleton_pose fixed_pose{*fixed_rig};
  skeleton_pose seek_pose{*seek_rig};

  params parameters;

  // Same dt sequence drives identical phase rules in both graphs.
  const float dts[]{0.0F, 0.2F, 0.7F, 0.15F, 1.3F, 0.05F};
  for (const float dt : dts) {
    fixed_player.play(dt, parameters, fixed_pose);
    seek_player.play(dt, parameters, seek_pose);

    // Object-space tip: exercises parent/child rotation, non-uniform scale
    // and the ordinary + additive combination.
    const transform& tip_fixed{fixed_pose.get_transform_object_space(2)};
    const transform& tip_seek{seek_pose.get_transform_object_space(2)};

    TASK_CHECK(near_float3(tip_fixed.translation, tip_seek.translation));
    TASK_CHECK(near_quaternion_equiv(tip_fixed.rotation, tip_seek.rotation));
    TASK_CHECK(near_float3(tip_fixed.scale, tip_seek.scale));
  }

  return 0;
}

static int check_legacy_schemes_play(void)
{
  std::vector<std::byte> storage(8 * 1024 * 1024);

  {
    project_uncooked input{measurement_unit::meters, axis_system::y_up_x_right_z_forward};
    make_chain_skeleton(input, "rig", 2);

    const std::vector<clip_uncooked_track> tracks{
        {.joint_id = "j0",
         .keys = {{0.0F, {.translation = float3{1.0F, 0.0F, 0.0F}}},
                  {10.0F, {.translation = float3{4.0F, 0.0F, 0.0F}}}}}};

    auto& none_clip = input.add_resource<clip_uncooked>("none_clip");
    none_clip.set_target_skeleton_id("rig");
    none_clip.set_compression_scheme(clip_compression_scheme::none);
    none_clip.set_tracks(tracks);

    auto& fixed_clip = input.add_resource<clip_uncooked>("fixed_clip");
    fixed_clip.set_target_skeleton_id("rig");
    fixed_clip.set_compression_scheme(clip_compression_scheme::fixed);
    fixed_clip.set_tracks(tracks);

    auto& acl_clip = input.add_resource<clip_uncooked>("acl_clip");
    acl_clip.set_target_skeleton_id("rig");
    acl_clip.set_compression_scheme(clip_compression_scheme::acl);
    acl_clip.set_tracks(tracks);

    project::cook(input, std::span{storage});
  }

  project cooked{std::span{storage}};
  const auto* rig = cooked.get_resource<skeleton>("rig");
  TASK_CHECK(rig != nullptr);

  for (const string_id& id : {"none_clip", "fixed_clip", "acl_clip"}) {
    const clip* resource{cooked.get_resource<clip>(id)};
    TASK_CHECK(resource != nullptr);

    std::unique_ptr<clip_player_base> player{resource->create_player()};
    skeleton_pose pose{*rig};

    for (const float time : {0.0F, 3.3F, 10.0F, 2.2F}) {
      player->play(time, pose);
      const transform& joint{pose.get_transform_joint_space(0)};
      TASK_CHECK(std::isfinite(joint.translation.x));
      TASK_CHECK(std::isfinite(joint.rotation.w));
      TASK_CHECK(std::isfinite(joint.scale.x));
    }
  }

  return 0;
}

// ============================================================================
// Registration
// ============================================================================

const task_case task_cases[]{
    {"checkpoints", "layout_bounds", check_layout_bounds},
    {"checkpoints", "serialization_roundtrip", check_serialization_roundtrip},
    {"checkpoints", "quantized_stream_identical", check_quantized_stream_identical},
    {"checkpoints", "no_heap_during_play", check_no_heap_during_play},
    {"checkpoints", "operation_bounds", check_operation_bounds},

    {"seek_values", "matches_fixed_reference", check_matches_fixed_reference},
    {"seek_values", "hand_calculated_quantized_keys", check_hand_calculated_quantized_keys},
    {"seek_values", "edge_and_reverse_queries", check_edge_and_reverse_queries},
    {"seek_values", "rest_and_missing_channels", check_rest_and_missing_channels},

    {"pipeline", "cook_load_two_players", check_cook_load_two_players},
    {"pipeline", "graph_additive_hierarchy", check_graph_additive_hierarchy},
    {"pipeline", "legacy_schemes_play", check_legacy_schemes_play},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);

// Global replacements for heap allocation tracking.
void* operator new(size_t size)
{
  ++g_operator_new_calls;
  if (void* pointer{std::malloc(size)}) {
    return pointer;
  }
  throw std::bad_alloc{};
}

void* operator new[](size_t size)
{
  ++g_operator_new_array_calls;
  if (void* pointer{std::malloc(size)}) {
    return pointer;
  }
  throw std::bad_alloc{};
}

void operator delete(void* pointer) noexcept
{
  std::free(pointer);
}

void operator delete[](void* pointer) noexcept
{
  std::free(pointer);
}

void operator delete(void* pointer, size_t) noexcept
{
  std::free(pointer);
}

void operator delete[](void* pointer, size_t) noexcept
{
  std::free(pointer);
}
