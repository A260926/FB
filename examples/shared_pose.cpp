// Windowless example of the shared-pose-node animation graph player.
//
// API summary
// -----------
//   anim_graph_player player{graph, true};
//
// The second constructor argument enables shared-pose-node mode. The usual
// single-argument constructor is equivalent to passing `false` and keeps the
// original tree-like evaluation, where every node reference is traversed on
// its own.
//
// In shared mode a single runtime node that is referenced by several
// branches is evaluated at most once per `play`; its pose is shared between
// every consumer. The mode supports clip, blend, sum and float param nodes,
// and the root node must produce a pose. Structural problems (missing or
// duplicated ids, wrong node types, cycles, invalid blend factor tables,
// unsupported reachable nodes) make the constructor throw
// `std::invalid_argument`.
//
// During a play, an active pose node that receives two different evaluation
// requests (different dt/sync settings) throws `std::invalid_argument`.
// A failed play changes neither the output pose nor the player's state:
// after fixing the parameters, playback continues as if it never happened.
// `get_last_play_stats()` reports per-play evaluation, job and pose-pool
// usage and keeps the values of the last successful play.

#include <eely/anim_graph/anim_graph.h>
#include <eely/anim_graph/anim_graph_node_blend.h>
#include <eely/anim_graph/anim_graph_node_clip.h>
#include <eely/anim_graph/anim_graph_node_param.h>
#include <eely/anim_graph/anim_graph_node_sum.h>
#include <eely/anim_graph/anim_graph_player.h>
#include <eely/anim_graph/anim_graph_uncooked.h>
#include <eely/clip/clip.h>
#include <eely/clip/clip_uncooked.h>
#include <eely/params/params.h>
#include <eely/project/project.h>
#include <eely/project/project_uncooked.h>
#include <eely/skeleton/skeleton.h>
#include <eely/skeleton/skeleton_pose.h>
#include <eely/skeleton/skeleton_uncooked.h>

#include <cmath>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <vector>

int main()
{
  using namespace eely;

  // One skeleton and three one-second clips with known root translations.
  project_uncooked uncooked{measurement_unit::meters,
                            axis_system::y_up_x_right_z_forward};

  auto& skeleton_resource = uncooked.add_resource<skeleton_uncooked>("rig");
  skeleton_resource.get_joints() = {
      {.id = "root", .parent_index = std::nullopt, .rest_pose_transform = {}},
      {.id = "child",
       .parent_index = 0,
       .rest_pose_transform = {.translation = float3{0.0F, 1.0F, 0.0F}}}};

  const float clip_starts[3] = {0.0F, 4.0F, 8.0F};
  const char* clip_names[3] = {"a", "b", "c"};
  for (int i = 0; i < 3; ++i) {
    auto& clip = uncooked.add_resource<clip_uncooked>(clip_names[i]);
    clip.set_target_skeleton_id("rig");
    clip.set_compression_scheme(clip_compression_scheme::none);
    const float x = clip_starts[i];
    clip.set_tracks(
        {{.joint_id = "root",
          .keys = {{0.0F, {.translation = float3{x, 0.0F, 0.0F}}},
                   {1.0F, {.translation = float3{x + 2.0F, 0.0F, 0.0F}}}}}});
  }

  // Shared diamond:
  //
  //   blend_left  = blend(a, b, w1)
  //   blend_right = blend(a, c, w2)   // clip `a` is referenced twice
  //   root        = blend_left + blend_right
  auto& graph = uncooked.add_resource<anim_graph_uncooked>("graph");
  graph.set_skeleton_id("rig");

  auto& node_a = graph.add_node<anim_graph_node_clip>();
  node_a.set_clip_id("a");
  auto& node_b = graph.add_node<anim_graph_node_clip>();
  node_b.set_clip_id("b");
  auto& node_c = graph.add_node<anim_graph_node_clip>();
  node_c.set_clip_id("c");

  auto& weight_one = graph.add_node<anim_graph_node_param>();
  weight_one.set_param_id("w1");
  auto& weight_two = graph.add_node<anim_graph_node_param>();
  weight_two.set_param_id("w2");

  auto& blend_left = graph.add_node<anim_graph_node_blend>();
  blend_left.get_pose_nodes() = {
      {.id = node_a.get_id(), .factor = 0.0F},
      {.id = node_b.get_id(), .factor = 1.0F}};
  blend_left.set_factor_node_id(weight_one.get_id());

  auto& blend_right = graph.add_node<anim_graph_node_blend>();
  blend_right.get_pose_nodes() = {
      {.id = node_a.get_id(), .factor = 0.0F},
      {.id = node_c.get_id(), .factor = 1.0F}};
  blend_right.set_factor_node_id(weight_two.get_id());

  auto& sum = graph.add_node<anim_graph_node_sum>();
  sum.set_first_node_id(blend_left.get_id());
  sum.set_second_node_id(blend_right.get_id());
  graph.set_root_node_id(sum.get_id());

  // The binary asset format is unchanged: the same cooking pipeline is used.
  std::vector<std::byte> storage(4 * 1024 * 1024);
  std::span<std::byte> bytes{storage};
  project::cook(uncooked, bytes);
  project cooked{bytes};

  const auto* cooked_graph = cooked.get_resource<anim_graph>("graph");
  const auto* rig = cooked.get_resource<skeleton>("rig");

  // Construct the player in shared-pose-node mode.
  anim_graph_player player{*cooked_graph, true};
  skeleton_pose pose{*rig};

  params parameters;
  parameters.get_value<float>("w1") = 0.25F;
  parameters.get_value<float>("w2") = 0.75F;

  // First play at dt = 0 leaves every clip at phase zero, then advance.
  player.play(0.0F, parameters, pose);
  for (int step = 0; step < 5; ++step) {
    player.play(0.1F, parameters, pose);
  }

  // Independently sample every clip and compose the same hierarchy by hand
  // to cross-check the shared player's output. After one dt=0 play and five
  // 0.1s steps every clip is sampled at the same normalized phase (0.5).
  const float phase_time = 0.5F;
  skeleton_pose sample_a{*rig};
  skeleton_pose sample_b{*rig};
  skeleton_pose sample_c{*rig};
  cooked.get_resource<clip>("a")->create_player()->play(phase_time, sample_a);
  cooked.get_resource<clip>("b")->create_player()->play(phase_time, sample_b);
  cooked.get_resource<clip>("c")->create_player()->play(phase_time, sample_c);

  skeleton_pose reference_left{*rig};
  skeleton_pose_blend(sample_a, sample_b, 0.25F, reference_left);
  skeleton_pose reference_right{*rig};
  skeleton_pose_blend(sample_a, sample_c, 0.75F, reference_right);
  skeleton_pose reference{*rig};
  skeleton_pose_add(reference_left, reference_right, reference);

  bool matches = true;
  for (gsl::index joint = 0; joint < pose.get_joints_count(); ++joint) {
    const float3& got = pose.get_transform_object_space(joint).translation;
    const float3& want = reference.get_transform_object_space(joint).translation;
    if (std::fabs(got.x - want.x) > 1e-5F || std::fabs(got.y - want.y) > 1e-5F ||
        std::fabs(got.z - want.z) > 1e-5F) {
      matches = false;
    }
  }
  if (!matches) {
    throw std::runtime_error("shared pose example produced an unexpected pose");
  }

  const float root_x = pose.get_transform_object_space(0).translation.x;
  const float child_y = pose.get_transform_object_space(1).translation.y;

  const anim_graph_play_stats& stats = player.get_last_play_stats();
  // Clip `a` is shared by both blends, so it is evaluated once and the
  // second request is served from the cache.
  if (stats.cache_hits == 0 || stats.evaluated_pose_nodes != 6) {
    throw std::runtime_error("shared pose example reported unexpected stats");
  }

  std::printf("SHARED_POSE_EXAMPLE_OK root_x=%.3f child_y=%.3f "
              "evaluated=%zu jobs=%zu cache_hits=%zu peak_poses=%zu\n",
              root_x, child_y, stats.evaluated_pose_nodes, stats.executed_jobs,
              stats.cache_hits, stats.peak_live_poses);
  return 0;
}
