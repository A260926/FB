#include "eely/clip/clip.h"
#include "eely/clip/clip_compression_scheme.h"
#include "eely/clip/clip_player_fixed_seek.h"
#include "eely/clip/clip_uncooked.h"
#include "eely/math/float3.h"
#include "eely/math/quaternion.h"
#include "eely/math/transform.h"
#include "eely/project/axis_system.h"
#include "eely/project/measurement_unit.h"
#include "eely/project/project.h"
#include "eely/project/project_uncooked.h"
#include "eely/skeleton/skeleton.h"
#include "eely/skeleton/skeleton_pose.h"
#include "eely/skeleton/skeleton_uncooked.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace eely;
using namespace eely::internal;

namespace {
constexpr float demo_epsilon{1e-5F};

void require(bool condition, const char* message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

bool quaternion_equivalent(const quaternion& a, const quaternion& b)
{
  const auto distance = [](const quaternion& x, const quaternion& y) {
    return std::fabs(x.x - y.x) + std::fabs(x.y - y.y) + std::fabs(x.z - y.z) +
           std::fabs(x.w - y.w);
  };
  const quaternion negated{-b.x, -b.y, -b.z, -b.w};
  return distance(a, b) <= demo_epsilon || distance(a, negated) <= demo_epsilon;
}

bool object_space_tips_match(const skeleton_pose& a, const skeleton_pose& b, gsl::index tip)
{
  const transform& ta{a.get_transform_object_space(tip)};
  const transform& tb{b.get_transform_object_space(tip)};
  return float3_near(ta.translation, tb.translation, demo_epsilon) &&
         float3_near(ta.scale, tb.scale, demo_epsilon) &&
         quaternion_equivalent(ta.rotation, tb.rotation);
}

// Long, irregular, partial-TRS animation. Every animated channel has keys at
// both endpoints, so the cooked stream stays a valid fixed-scheme clip.
std::vector<clip_uncooked_track> make_long_tracks(gsl::index joints_count, float duration)
{
  std::mt19937 generator(20261123);
  std::uniform_real_distribution<float> unit(-1.0F, 1.0F);

  std::vector<clip_uncooked_track> tracks;

  for (gsl::index joint_index{0}; joint_index < joints_count; ++joint_index) {
    clip_uncooked_track track;
    track.joint_id = "j" + std::to_string(joint_index);

    const gsl::index key_count{24 + joint_index * 6};
    const bool animates_translation{joint_index != 1};
    const bool animates_scale{joint_index % 2 == 0};

    for (gsl::index key_index{0}; key_index < key_count; ++key_index) {
      const float time{duration * static_cast<float>(key_index) /
                       static_cast<float>(key_count - 1)};

      clip_uncooked_key key;
      key.rotation =
          quaternion_from_yaw_pitch_roll_intrinsic(unit(generator), unit(generator),
                                                    unit(generator));
      if (animates_translation) {
        key.translation = float3{unit(generator), unit(generator), unit(generator)};
      }
      if (animates_scale) {
        // Uniform non-unit scales keep the additive-friendly math exact.
        const float scale{1.0F + 0.3F * unit(generator)};
        key.scale = float3{scale, scale, scale};
      }

      track.keys[time] = key;
    }

    tracks.push_back(std::move(track));
  }

  return tracks;
}
}  // namespace

int main()
{
  constexpr gsl::index joints_count{4};
  constexpr gsl::index tip_index{joints_count - 1};
  constexpr float duration{12.0F};

  std::vector<std::byte> storage(16 * 1024 * 1024);

  // Cook both the reference `fixed` clip and the new `fixed_seek` clip from
  // the same uncooked input.
  {
    project_uncooked uncooked{measurement_unit::meters,
                              axis_system::y_up_x_right_z_forward};

    auto& rig_uncooked = uncooked.add_resource<skeleton_uncooked>("rig");
    for (gsl::index joint_index{0}; joint_index < joints_count; ++joint_index) {
      rig_uncooked.get_joints().push_back(
          {.id = "j" + std::to_string(joint_index),
           .parent_index = joint_index == 0
                               ? std::nullopt
                               : std::optional<gsl::index>{joint_index - 1},
           .rest_pose_transform =
               transform{float3{0.0F, static_cast<float>(joint_index) + 1.0F, 0.0F}}});
    }

    const std::vector<clip_uncooked_track> tracks{make_long_tracks(joints_count, duration)};

    auto& reference_uncooked = uncooked.add_resource<clip_uncooked>("reference");
    reference_uncooked.set_target_skeleton_id("rig");
    reference_uncooked.set_compression_scheme(clip_compression_scheme::fixed);
    reference_uncooked.set_tracks(tracks);

    auto& seek_uncooked = uncooked.add_resource<clip_uncooked>("seek");
    seek_uncooked.set_target_skeleton_id("rig");
    seek_uncooked.set_compression_scheme(clip_compression_scheme::fixed_seek);
    seek_uncooked.set_tracks(tracks);

    project::cook(uncooked, std::span{storage});
  }

  // The uncooked project is gone: everything below runs from the cooked bytes.
  project cooked{std::span{storage}};

  const skeleton* rig{cooked.get_resource<skeleton>("rig")};
  const clip* reference_clip{cooked.get_resource<clip>("reference")};
  const clip* seek_clip{cooked.get_resource<clip>("seek")};
  require(rig != nullptr && reference_clip != nullptr && seek_clip != nullptr,
          "cooked resources missing");

  std::unique_ptr<clip_player_base> reference_player{reference_clip->create_player()};
  std::unique_ptr<clip_player_base> seek_player_owner{seek_clip->create_player()};
  auto* seek_player = dynamic_cast<clip_player_fixed_seek*>(seek_player_owner.get());
  require(seek_player != nullptr, "seek player has the wrong type");

  const fixed_seek_layout layout{seek_player->get_seek_layout()};
  require(layout.encoded_records > 32, "long clip must span several checkpoint strides");
  require(layout.checkpoint_count >= 2, "checkpoint index missing");
  require(layout.component_count > 0, "no animated components");

  const size_t checkpoint_bound{(layout.encoded_records + 31) / 32 + 2};
  require(layout.checkpoint_count <= checkpoint_bound, "checkpoint count exceeds the bound");

  skeleton_pose reference_pose{*rig};
  skeleton_pose seek_pose{*rig};

  // First query lands near the end, then reverse, far jump, repeats, endpoints.
  const std::vector<float> query_times{duration,       duration * 0.97F, 0.0F,
                                       duration * .33F, duration * .33F,  duration * .8F,
                                       0.0F,            duration,         duration * .5F};

  const size_t decode_bound{32 + 2 * layout.component_count};
  const size_t comparison_bound{static_cast<size_t>(std::ceil(
                                    std::log2(static_cast<double>(layout.checkpoint_count + 1)))) +
                                1};

  size_t max_decoded{0};
  size_t max_comparisons{0};

  for (const float time : query_times) {
    const fixed_seek_stats before{seek_player->get_last_seek_stats()};

    reference_player->play(time, reference_pose);
    seek_player->play(time, seek_pose);

    const fixed_seek_stats after{seek_player->get_last_seek_stats()};
    max_decoded = std::max(max_decoded, after.decoded_records - before.decoded_records);
    max_comparisons =
        std::max(max_comparisons, after.checkpoint_comparisons - before.checkpoint_comparisons);

    require(object_space_tips_match(reference_pose, seek_pose, tip_index),
            "seek pose diverges from the fixed reference");
  }

  const fixed_seek_stats final_stats{seek_player->get_last_seek_stats()};
  require(final_stats.restored_checkpoints > 0, "reverse playback must restore a checkpoint");
  require(max_decoded <= decode_bound, "too many records decoded in one play");
  require(max_comparisons <= comparison_bound, "too many checkpoint comparisons");

  // A second player on the same clip keeps independent cursor state.
  std::unique_ptr<clip_player_base> second_owner{seek_clip->create_player()};
  auto* second_player = dynamic_cast<clip_player_fixed_seek*>(second_owner.get());
  require(second_player != nullptr, "second seek player has the wrong type");

  skeleton_pose second_pose{*rig};
  second_player->play(0.0F, second_pose);
  seek_player->play(duration, seek_pose);
  second_player->play(duration, second_pose);
  require(object_space_tips_match(second_pose, seek_pose, tip_index),
          "independent players diverge");

  std::printf("FIXED_SEEK_DEMO_OK records=%zu components=%zu checkpoints=%zu max_decoded=%zu "
              "max_comparisons=%zu restores=%zu\n",
              layout.encoded_records, layout.component_count, layout.checkpoint_count,
              max_decoded, max_comparisons, final_stats.restored_checkpoints);
  return 0;
}
