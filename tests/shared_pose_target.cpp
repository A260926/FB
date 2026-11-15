#include "task_support/check.h"

#include <eely/anim_graph/anim_graph.h>
#include <eely/anim_graph/anim_graph_node_blend.h>
#include <eely/anim_graph/anim_graph_node_clip.h>
#include <eely/anim_graph/anim_graph_node_param.h>
#include <eely/anim_graph/anim_graph_node_sum.h>
#include <eely/anim_graph/anim_graph_player.h>
#include <eely/anim_graph/anim_graph_uncooked.h>
#include <eely/clip/clip.h>
#include <eely/clip/clip_uncooked.h>
#include <eely/math/transform.h>
#include <eely/params/params.h>
#include <eely/project/project.h>
#include <eely/project/project_uncooked.h>
#include <eely/skeleton/skeleton.h>
#include <eely/skeleton/skeleton_pose.h>
#include <eely/skeleton/skeleton_uncooked.h>

#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace eely;

constexpr float eps = 1e-5F;

// Cooked project together with its backing storage.
struct cooked_project final {
  std::vector<std::byte> storage{4 * 1024 * 1024};
  std::unique_ptr<project> cooked;

  void cook(project_uncooked& uncooked)
  {
    std::span<std::byte> bytes{storage};
    project::cook(uncooked, bytes);
    cooked = std::make_unique<project>(bytes);
  }

  [[nodiscard]] const skeleton& rig() const
  {
    return *cooked->get_resource<skeleton>("rig");
  }
};

// Build a two-joint skeleton and three one-second clips with known
// root translations:
//   a: x 0 -> 2, b: x 4 -> 6, c: x 8 -> 10
void add_base_resources(project_uncooked& in)
{
  auto& sk = in.add_resource<skeleton_uncooked>("rig");
  sk.get_joints() = {
      {.id = "root", .parent_index = std::nullopt, .rest_pose_transform = {}},
      {.id = "child",
       .parent_index = 0,
       .rest_pose_transform = {.translation = float3{0.0F, 1.0F, 0.0F}}}};

  const float starts[3] = {0.0F, 4.0F, 8.0F};
  const char* names[3] = {"a", "b", "c"};
  for (int i = 0; i < 3; ++i) {
    auto& clip = in.add_resource<clip_uncooked>(names[i]);
    clip.set_target_skeleton_id("rig");
    clip.set_compression_scheme(clip_compression_scheme::none);

    const float x = starts[i];
    clip.set_tracks(
        {{.joint_id = "root",
          .keys = {{0.0F, {.translation = float3{x, 0.0F, 0.0F}}},
                   {1.0F, {.translation = float3{x + 2.0F, 0.0F, 0.0F}}}}},
         {.joint_id = "child",
          .keys = {{0.0F, {.translation = float3{0.0F, 1.0F, 0.0F}}},
                   {1.0F, {.translation = float3{0.0F, 1.0F, 0.0F}}}}}});
  }
}

// Independently sample a cooked clip at an absolute time.
skeleton_pose sample_clip(const project& cooked, const char* id, const float time_s)
{
  const auto* clip_resource = cooked.get_resource<clip>(id);
  auto player = clip_resource->create_player();
  skeleton_pose pose{*cooked.get_resource<skeleton>("rig")};
  player->play(time_s, pose);
  return pose;
}

bool floats_equal(float a, float b)
{
  return std::fabs(a - b) <= eps;
}

bool float3_equal(const float3& a, const float3& b)
{
  return floats_equal(a.x, b.x) && floats_equal(a.y, b.y) && floats_equal(a.z, b.z);
}

bool quaternion_equal(const quaternion& a, const quaternion& b)
{
  // Quaternions q and -q describe the same rotation.
  const quaternion second = floats_equal(a.x, b.x) ? b : quaternion{-b.x, -b.y, -b.z, -b.w};
  return floats_equal(a.x, second.x) && floats_equal(a.y, second.y) &&
         floats_equal(a.z, second.z) && floats_equal(a.w, second.w);
}

bool poses_equal(const skeleton_pose& lhs, const skeleton_pose& rhs)
{
  if (lhs.get_joints_count() != rhs.get_joints_count()) {
    return false;
  }

  for (gsl::index i = 0; i < lhs.get_joints_count(); ++i) {
    const transform& tl = lhs.get_transform_object_space(i);
    const transform& tr = rhs.get_transform_object_space(i);
    if (!float3_equal(tl.translation, tr.translation)) {
      return false;
    }
    if (!float3_equal(tl.scale, tr.scale)) {
      return false;
    }
    if (!quaternion_equal(tl.rotation, tr.rotation)) {
      return false;
    }
  }

  return true;
}

// Independent hierarchical reference for the shared diamond:
//   sum(blend(a, b, w1), blend(a, c, w2))
// Clips are sampled directly and poses are combined with the same
// blend/add primitives but without any graph machinery.
skeleton_pose reference_diamond(const project& cooked,
                                const float time_a,
                                const float time_b,
                                const float time_c,
                                const float w1,
                                const float w2)
{
  const skeleton& skeleton_resource = *cooked.get_resource<skeleton>("rig");

  skeleton_pose pa = sample_clip(cooked, "a", time_a);
  skeleton_pose pb = sample_clip(cooked, "b", time_b);
  skeleton_pose pc = sample_clip(cooked, "c", time_c);

  skeleton_pose blend_left{skeleton_resource};
  skeleton_pose_blend(pa, pb, w1, blend_left);

  skeleton_pose blend_right{skeleton_resource};
  skeleton_pose_blend(pa, pc, w2, blend_right);

  skeleton_pose result{skeleton_resource};
  skeleton_pose_add(blend_left, blend_right, result);
  return result;
}

// Build the shared diamond graph. Clip `a` is referenced by both blends.
int build_shared_diamond(project_uncooked& in)
{
  auto& g = in.add_resource<anim_graph_uncooked>("graph");
  g.set_skeleton_id("rig");

  auto& a = g.add_node<anim_graph_node_clip>();
  a.set_clip_id("a");
  auto& b = g.add_node<anim_graph_node_clip>();
  b.set_clip_id("b");
  auto& c = g.add_node<anim_graph_node_clip>();
  c.set_clip_id("c");
  auto& p1 = g.add_node<anim_graph_node_param>();
  p1.set_param_id("w1");
  auto& p2 = g.add_node<anim_graph_node_param>();
  p2.set_param_id("w2");

  auto& blend_left = g.add_node<anim_graph_node_blend>();
  blend_left.get_pose_nodes() = {
      {.id = a.get_id(), .factor = 0.0F}, {.id = b.get_id(), .factor = 1.0F}};
  blend_left.set_factor_node_id(p1.get_id());

  auto& blend_right = g.add_node<anim_graph_node_blend>();
  blend_right.get_pose_nodes() = {
      {.id = a.get_id(), .factor = 0.0F}, {.id = c.get_id(), .factor = 1.0F}};
  blend_right.set_factor_node_id(p2.get_id());

  auto& sum = g.add_node<anim_graph_node_sum>();
  sum.set_first_node_id(blend_left.get_id());
  sum.set_second_node_id(blend_right.get_id());

  g.set_root_node_id(sum.get_id());
  return 0;
}

// Expanded, pure-tree equivalent of the shared diamond:
// the shared clip `a` is duplicated into two independent clip nodes,
// so the ordinary (non-shared) player can traverse it.
int build_expanded_diamond(project_uncooked& in)
{
  auto& g = in.add_resource<anim_graph_uncooked>("graph");
  g.set_skeleton_id("rig");

  auto& a1 = g.add_node<anim_graph_node_clip>();
  a1.set_clip_id("a");
  auto& a2 = g.add_node<anim_graph_node_clip>();
  a2.set_clip_id("a");
  auto& b = g.add_node<anim_graph_node_clip>();
  b.set_clip_id("b");
  auto& c = g.add_node<anim_graph_node_clip>();
  c.set_clip_id("c");
  auto& p1 = g.add_node<anim_graph_node_param>();
  p1.set_param_id("w1");
  auto& p2 = g.add_node<anim_graph_node_param>();
  p2.set_param_id("w2");

  auto& blend_left = g.add_node<anim_graph_node_blend>();
  blend_left.get_pose_nodes() = {
      {.id = a1.get_id(), .factor = 0.0F}, {.id = b.get_id(), .factor = 1.0F}};
  blend_left.set_factor_node_id(p1.get_id());

  auto& blend_right = g.add_node<anim_graph_node_blend>();
  blend_right.get_pose_nodes() = {
      {.id = a2.get_id(), .factor = 0.0F}, {.id = c.get_id(), .factor = 1.0F}};
  blend_right.set_factor_node_id(p2.get_id());

  auto& sum = g.add_node<anim_graph_node_sum>();
  sum.set_first_node_id(blend_left.get_id());
  sum.set_second_node_id(blend_right.get_id());

  g.set_root_node_id(sum.get_id());
  return 0;
}

params make_params(const float w1, const float w2)
{
  params p;
  p.get_value<float>("w1") = w1;
  p.get_value<float>("w2") = w2;
  return p;
}

// -----------------------------------------------------------------------
// sharing
// -----------------------------------------------------------------------

// A blend whose two candidates are the very same clip node (self blend).
int sharing_self_blend()
{
  project_uncooked in;
  add_base_resources(in);

  auto& g = in.add_resource<anim_graph_uncooked>("graph");
  g.set_skeleton_id("rig");
  auto& a = g.add_node<anim_graph_node_clip>();
  a.set_clip_id("a");
  auto& p = g.add_node<anim_graph_node_param>();
  p.set_param_id("w");
  auto& blend = g.add_node<anim_graph_node_blend>();
  blend.get_pose_nodes() = {
      {.id = a.get_id(), .factor = 0.0F}, {.id = a.get_id(), .factor = 1.0F}};
  blend.set_factor_node_id(p.get_id());
  g.set_root_node_id(blend.get_id());

  cooked_project cp;
  cp.cook(in);

  anim_graph_player player{*cp.cooked->get_resource<anim_graph>("graph"), true};
  skeleton_pose pose{cp.rig()};
  params parameters;
  parameters.get_value<float>("w") = 0.5F;

  // First play at dt = 0 leaves every clip at phase zero, i.e. time zero.
  player.play(0.0F, parameters, pose);

  const skeleton_pose expected = sample_clip(*cp.cooked, "a", 0.0F);
  TASK_CHECK(poses_equal(pose, expected));

  // The single clip node is evaluated once and reused by the second edge.
  const anim_graph_play_stats& stats = player.get_last_play_stats();
  TASK_CHECK(stats.evaluated_pose_nodes == 2);  // clip + blend
  TASK_CHECK(stats.cache_hits == 1);
  return 0;
}

// Nested diamond checked against both an independent hierarchical
// reference and the expanded tree, including looping and dt = 0.
int sharing_nested_diamond()
{
  project_uncooked in_shared;
  add_base_resources(in_shared);
  build_shared_diamond(in_shared);
  cooked_project cp_shared;
  cp_shared.cook(in_shared);

  project_uncooked in_tree;
  add_base_resources(in_tree);
  build_expanded_diamond(in_tree);
  cooked_project cp_tree;
  cp_tree.cook(in_tree);

  anim_graph_player shared_player{*cp_shared.cooked->get_resource<anim_graph>("graph"), true};
  anim_graph_player tree_player{*cp_tree.cooked->get_resource<anim_graph>("graph"), false};

  skeleton_pose pose_shared{cp_shared.rig()};
  skeleton_pose pose_tree{cp_tree.rig()};

  const params parameters = make_params(0.25F, 0.75F);

  // dt = 0: the pose must not change between the two plays.
  shared_player.play(0.0F, parameters, pose_shared);
  tree_player.play(0.0F, parameters, pose_tree);
  TASK_CHECK(poses_equal(pose_shared, pose_tree));

  const skeleton_pose zero_reference = reference_diamond(*cp_shared.cooked, 0.0F, 0.0F, 0.0F,
                                                         0.25F, 0.75F);
  TASK_CHECK(poses_equal(pose_shared, zero_reference));

  skeleton_pose pose_before = pose_shared;
  shared_player.play(0.0F, parameters, pose_shared);
  TASK_CHECK(poses_equal(pose_shared, pose_before));

  // Advance with several steps, one of them crossing the normalized phase
  // wrap (clips are one second long and steps are 0.4s).
  float accumulated = 0.0F;
  for (int step = 0; step < 4; ++step) {
    shared_player.play(0.4F, parameters, pose_shared);
    tree_player.play(0.4F, parameters, pose_tree);
    TASK_CHECK(poses_equal(pose_shared, pose_tree));
    accumulated += 0.4F;
  }

  // After wrapping once, the phase is accumulated - 1.0 and every clip is
  // sampled at the same normalized time because blends force synchronization.
  const float phase = std::fmod(accumulated, 1.0F);
  const skeleton_pose loop_reference =
      reference_diamond(*cp_shared.cooked, phase, phase, phase, 0.25F, 0.75F);
  TASK_CHECK(poses_equal(pose_shared, loop_reference));

  const anim_graph_play_stats& shared_stats = shared_player.get_last_play_stats();
  // Unique active pose nodes: three clips, two blends and one sum.
  TASK_CHECK(shared_stats.evaluated_pose_nodes == 6);
  // Clip `a` is shared by both blends: one real evaluation and one reuse.
  TASK_CHECK(shared_stats.cache_hits == 1);
  // Every active node owns at most one live pose.
  TASK_CHECK(shared_stats.peak_live_poses <= shared_stats.evaluated_pose_nodes);

  // The expanded tree recomputes the shared clip: it runs more jobs.
  const anim_graph_play_stats& tree_stats = tree_player.get_last_play_stats();
  TASK_CHECK(tree_stats.executed_jobs > shared_stats.executed_jobs);
  return 0;
}

// A clip shared by two sums whose results are then blended together.
// Every path into the shared clip stays inside one blend's synchronized
// subtree, so all its requests agree.
int sharing_sum_and_blend_inputs()
{
  project_uncooked in;
  add_base_resources(in);

  auto& g = in.add_resource<anim_graph_uncooked>("graph");
  g.set_skeleton_id("rig");
  auto& a = g.add_node<anim_graph_node_clip>();
  a.set_clip_id("a");
  auto& b = g.add_node<anim_graph_node_clip>();
  b.set_clip_id("b");
  auto& c = g.add_node<anim_graph_node_clip>();
  c.set_clip_id("c");
  auto& p = g.add_node<anim_graph_node_param>();
  p.set_param_id("w");

  // `a` is a shared input of both sums.
  auto& sum_left = g.add_node<anim_graph_node_sum>();
  sum_left.set_first_node_id(a.get_id());
  sum_left.set_second_node_id(b.get_id());

  auto& sum_right = g.add_node<anim_graph_node_sum>();
  sum_right.set_first_node_id(a.get_id());
  sum_right.set_second_node_id(c.get_id());

  auto& blend = g.add_node<anim_graph_node_blend>();
  blend.get_pose_nodes() = {
      {.id = sum_left.get_id(), .factor = 0.0F},
      {.id = sum_right.get_id(), .factor = 1.0F}};
  blend.set_factor_node_id(p.get_id());
  g.set_root_node_id(blend.get_id());

  cooked_project cp;
  cp.cook(in);

  anim_graph_player player{*cp.cooked->get_resource<anim_graph>("graph"), true};
  skeleton_pose pose{cp.rig()};
  params parameters;
  parameters.get_value<float>("w") = 0.5F;
  player.play(0.0F, parameters, pose);

  // Independent combination at phase zero.
  skeleton_pose pa = sample_clip(*cp.cooked, "a", 0.0F);
  skeleton_pose pb = sample_clip(*cp.cooked, "b", 0.0F);
  skeleton_pose pc = sample_clip(*cp.cooked, "c", 0.0F);
  skeleton_pose sum_l{cp.rig()};
  skeleton_pose_add(pa, pb, sum_l);
  skeleton_pose sum_r{cp.rig()};
  skeleton_pose_add(pa, pc, sum_r);
  skeleton_pose expected{cp.rig()};
  skeleton_pose_blend(sum_l, sum_r, 0.5F, expected);
  TASK_CHECK(poses_equal(pose, expected));

  const anim_graph_play_stats& stats = player.get_last_play_stats();
  TASK_CHECK(stats.evaluated_pose_nodes == 6);  // 3 clips + 2 sums + 1 blend
  TASK_CHECK(stats.cache_hits == 1);            // clip a is reused by the second sum
  return 0;
}

// Switching the factor changes which candidate edges are active.
// A candidate excluded by the blend weight must not produce any request.
int sharing_param_switch()
{
  project_uncooked in;
  add_base_resources(in);

  auto& g = in.add_resource<anim_graph_uncooked>("graph");
  g.set_skeleton_id("rig");
  auto& a = g.add_node<anim_graph_node_clip>();
  a.set_clip_id("a");
  auto& b = g.add_node<anim_graph_node_clip>();
  b.set_clip_id("b");
  auto& p = g.add_node<anim_graph_node_param>();
  p.set_param_id("w");
  auto& blend = g.add_node<anim_graph_node_blend>();
  blend.get_pose_nodes() = {
      {.id = a.get_id(), .factor = 0.0F}, {.id = b.get_id(), .factor = 1.0F}};
  blend.set_factor_node_id(p.get_id());
  g.set_root_node_id(blend.get_id());

  cooked_project cp;
  cp.cook(in);
  anim_graph_player player{*cp.cooked->get_resource<anim_graph>("graph"), true};
  skeleton_pose pose{cp.rig()};
  params parameters;

  // Factor exactly on a candidate: only that edge is active.
  parameters.get_value<float>("w") = 1.0F;
  player.play(0.0F, parameters, pose);
  TASK_CHECK(poses_equal(pose, sample_clip(*cp.cooked, "b", 0.0F)));
  TASK_CHECK(player.get_last_play_stats().cache_hits == 0);

  parameters.get_value<float>("w") = 0.0F;
  player.play(0.0F, parameters, pose);
  TASK_CHECK(poses_equal(pose, sample_clip(*cp.cooked, "a", 0.0F)));

  // Interior factor activates both candidates and produces a real blend.
  parameters.get_value<float>("w") = 0.5F;
  player.play(0.0F, parameters, pose);
  skeleton_pose pa = sample_clip(*cp.cooked, "a", 0.0F);
  skeleton_pose pb = sample_clip(*cp.cooked, "b", 0.0F);
  skeleton_pose expected{cp.rig()};
  skeleton_pose_blend(pa, pb, 0.5F, expected);
  TASK_CHECK(poses_equal(pose, expected));
  return 0;
}

// -----------------------------------------------------------------------
// ownership
// -----------------------------------------------------------------------

// One clip result consumed by two blends and a sum (three consuming edges).
// Later consumers must never observe a pose rewritten by an earlier one.
int ownership_multiple_consumers()
{
  project_uncooked in;
  add_base_resources(in);

  auto& g = in.add_resource<anim_graph_uncooked>("graph");
  g.set_skeleton_id("rig");
  auto& a = g.add_node<anim_graph_node_clip>();
  a.set_clip_id("a");
  auto& b = g.add_node<anim_graph_node_clip>();
  b.set_clip_id("b");
  auto& c = g.add_node<anim_graph_node_clip>();
  c.set_clip_id("c");
  auto& p1 = g.add_node<anim_graph_node_param>();
  p1.set_param_id("w1");
  auto& p2 = g.add_node<anim_graph_node_param>();
  p2.set_param_id("w2");
  auto& p3 = g.add_node<anim_graph_node_param>();
  p3.set_param_id("w3");

  auto& blend_one = g.add_node<anim_graph_node_blend>();
  blend_one.get_pose_nodes() = {
      {.id = a.get_id(), .factor = 0.0F}, {.id = b.get_id(), .factor = 1.0F}};
  blend_one.set_factor_node_id(p1.get_id());

  auto& blend_two = g.add_node<anim_graph_node_blend>();
  blend_two.get_pose_nodes() = {
      {.id = a.get_id(), .factor = 0.0F}, {.id = c.get_id(), .factor = 1.0F}};
  blend_two.set_factor_node_id(p2.get_id());

  // `a` is consumed by blend_one, blend_two and this sum: three edges.
  auto& sum_shared = g.add_node<anim_graph_node_sum>();
  sum_shared.set_first_node_id(blend_two.get_id());
  sum_shared.set_second_node_id(a.get_id());

  // Both branches live under a blend, so every request into `a` carries the
  // same synchronized phase.
  auto& root = g.add_node<anim_graph_node_blend>();
  root.get_pose_nodes() = {
      {.id = blend_one.get_id(), .factor = 0.0F},
      {.id = sum_shared.get_id(), .factor = 1.0F}};
  root.set_factor_node_id(p3.get_id());
  g.set_root_node_id(root.get_id());

  cooked_project cp;
  cp.cook(in);
  anim_graph_player player{*cp.cooked->get_resource<anim_graph>("graph"), true};
  skeleton_pose pose{cp.rig()};
  params parameters;
  parameters.get_value<float>("w1") = 0.25F;
  parameters.get_value<float>("w2") = 0.75F;
  parameters.get_value<float>("w3") = 0.4F;

  for (int i = 0; i < 5; ++i) {
    player.play(i == 0 ? 0.0F : 0.1F, parameters, pose);
  }

  // First play leaves phase at zero; four 0.1s steps land at phase 0.4.
  const float phase = 0.4F;
  skeleton_pose pa = sample_clip(*cp.cooked, "a", phase);
  skeleton_pose pb = sample_clip(*cp.cooked, "b", phase);
  skeleton_pose pc = sample_clip(*cp.cooked, "c", phase);
  skeleton_pose one{cp.rig()};
  skeleton_pose_blend(pa, pb, 0.25F, one);
  skeleton_pose two{cp.rig()};
  skeleton_pose_blend(pa, pc, 0.75F, two);
  skeleton_pose sum_pose{cp.rig()};
  skeleton_pose_add(two, pa, sum_pose);
  skeleton_pose expected{cp.rig()};
  skeleton_pose_blend(one, sum_pose, 0.4F, expected);
  TASK_CHECK(poses_equal(pose, expected));

  const anim_graph_play_stats& stats = player.get_last_play_stats();
  TASK_CHECK(stats.cache_hits >= 2);  // clip a served three times, evaluated once
  // Storage grows with unique active nodes, not with the number of edges.
  TASK_CHECK(stats.peak_live_poses <= stats.evaluated_pose_nodes);
  return 0;
}

// An entire blend node is shared between two downstream sums.
int ownership_shared_blend()
{
  project_uncooked in;
  add_base_resources(in);

  auto& g = in.add_resource<anim_graph_uncooked>("graph");
  g.set_skeleton_id("rig");
  auto& a = g.add_node<anim_graph_node_clip>();
  a.set_clip_id("a");
  auto& b = g.add_node<anim_graph_node_clip>();
  b.set_clip_id("b");
  auto& c = g.add_node<anim_graph_node_clip>();
  c.set_clip_id("c");
  auto& p = g.add_node<anim_graph_node_param>();
  p.set_param_id("w");

  auto& shared_blend = g.add_node<anim_graph_node_blend>();
  shared_blend.get_pose_nodes() = {
      {.id = a.get_id(), .factor = 0.0F}, {.id = b.get_id(), .factor = 1.0F}};
  shared_blend.set_factor_node_id(p.get_id());

  auto& sum_left = g.add_node<anim_graph_node_sum>();
  sum_left.set_first_node_id(shared_blend.get_id());
  sum_left.set_second_node_id(c.get_id());

  auto& sum_right = g.add_node<anim_graph_node_sum>();
  sum_right.set_first_node_id(c.get_id());
  sum_right.set_second_node_id(shared_blend.get_id());

  auto& root = g.add_node<anim_graph_node_sum>();
  root.set_first_node_id(sum_left.get_id());
  root.set_second_node_id(sum_right.get_id());
  g.set_root_node_id(root.get_id());

  cooked_project cp;
  cp.cook(in);
  anim_graph_player player{*cp.cooked->get_resource<anim_graph>("graph"), true};
  skeleton_pose pose{cp.rig()};
  params parameters;
  parameters.get_value<float>("w") = 0.5F;
  player.play(0.0F, parameters, pose);

  skeleton_pose pa = sample_clip(*cp.cooked, "a", 0.0F);
  skeleton_pose pb = sample_clip(*cp.cooked, "b", 0.0F);
  skeleton_pose pc = sample_clip(*cp.cooked, "c", 0.0F);
  skeleton_pose blended{cp.rig()};
  skeleton_pose_blend(pa, pb, 0.5F, blended);
  skeleton_pose left{cp.rig()};
  skeleton_pose_add(blended, pc, left);
  skeleton_pose right{cp.rig()};
  skeleton_pose_add(pc, blended, right);
  skeleton_pose expected{cp.rig()};
  skeleton_pose_add(left, right, expected);
  TASK_CHECK(poses_equal(pose, expected));

  // The shared blend is evaluated once and reused on the second edge.
  TASK_CHECK(player.get_last_play_stats().cache_hits >= 1);
  return 0;
}

// Non-shared mode keeps the original in-place input reuse:
// a long blend chain must keep a small, depth-independent live-pose peak.
int ownership_tree_inplace_reuse()
{
  project_uncooked in;
  add_base_resources(in);

  auto& g = in.add_resource<anim_graph_uncooked>("graph");
  g.set_skeleton_id("rig");
  auto& a = g.add_node<anim_graph_node_clip>();
  a.set_clip_id("a");
  auto& p = g.add_node<anim_graph_node_param>();
  p.set_param_id("w");

  // Chain of five blends, each blending the previous result with clip a.
  int previous = a.get_id();
  for (int depth = 0; depth < 5; ++depth) {
    auto& clip = g.add_node<anim_graph_node_clip>();
    clip.set_clip_id(depth % 2 == 0 ? "b" : "c");
    auto& blend = g.add_node<anim_graph_node_blend>();
    blend.get_pose_nodes() = {
        {.id = previous, .factor = 0.0F}, {.id = clip.get_id(), .factor = 1.0F}};
    blend.set_factor_node_id(p.get_id());
    previous = blend.get_id();
  }
  g.set_root_node_id(previous);

  cooked_project cp;
  cp.cook(in);
  anim_graph_player player{*cp.cooked->get_resource<anim_graph>("graph"), false};
  skeleton_pose pose{cp.rig()};
  params parameters;
  parameters.get_value<float>("w") = 0.3F;
  player.play(0.0F, parameters, pose);

  const anim_graph_play_stats& stats = player.get_last_play_stats();
  // In-place reuse keeps the peak at a small constant regardless of depth:
  // one clip result plus one reusable blend result at any moment.
  TASK_CHECK(stats.peak_live_poses <= 3);
  TASK_CHECK(stats.cache_hits == 0);  // nothing is cached in tree mode
  return 0;
}

// Two players sharing one cooked project interleave without interference.
int ownership_two_players()
{
  project_uncooked in_shared;
  add_base_resources(in_shared);
  build_shared_diamond(in_shared);
  cooked_project cp_shared;
  cp_shared.cook(in_shared);

  project_uncooked in_tree;
  add_base_resources(in_tree);
  build_expanded_diamond(in_tree);
  cooked_project cp_tree;
  cp_tree.cook(in_tree);

  const auto* shared_graph = cp_shared.cooked->get_resource<anim_graph>("graph");
  anim_graph_player player_a{*shared_graph, true};
  anim_graph_player player_b{*shared_graph, true};
  anim_graph_player tree_a{*cp_tree.cooked->get_resource<anim_graph>("graph"), false};
  anim_graph_player tree_b{*cp_tree.cooked->get_resource<anim_graph>("graph"), false};

  skeleton_pose pose_a{cp_shared.rig()};
  skeleton_pose pose_b{cp_shared.rig()};
  skeleton_pose ref_a{cp_tree.rig()};
  skeleton_pose ref_b{cp_tree.rig()};

  const params params_a = make_params(0.2F, 0.4F);
  const params params_b = make_params(0.7F, 0.1F);

  // Interleave the two players with different time steps and parameters.
  // Isolation is established by every player matching its own expanded-tree
  // reference despite the interleaving (the numeric poses may coincide by
  // chance on a given step, so inequality is not asserted).
  const float dt_a = 0.1F;
  const float dt_b = 0.15F;
  for (int step = 0; step < 6; ++step) {
    player_a.play(dt_a, params_a, pose_a);
    tree_a.play(dt_a, params_a, ref_a);
    player_b.play(dt_b, params_b, pose_b);
    tree_b.play(dt_b, params_b, ref_b);

    TASK_CHECK(poses_equal(pose_a, ref_a));
    TASK_CHECK(poses_equal(pose_b, ref_b));
  }

  TASK_CHECK(player_a.get_last_play_stats().evaluated_pose_nodes == 6);
  TASK_CHECK(player_b.get_last_play_stats().evaluated_pose_nodes == 6);
  return 0;
}

// -----------------------------------------------------------------------
// rollback
// -----------------------------------------------------------------------

// A conflicting request throws, leaves the output pose untouched and the
// node state rollable back; fixing the parameters then resumes as if the
// failed play never happened.
int rollback_conflict_recovery()
{
  project_uncooked in;
  add_base_resources(in);

  auto& g = in.add_resource<anim_graph_uncooked>("graph");
  g.set_skeleton_id("rig");
  auto& a = g.add_node<anim_graph_node_clip>();
  a.set_clip_id("a");
  auto& b = g.add_node<anim_graph_node_clip>();
  b.set_clip_id("a");  // different node, same clip
  auto& p = g.add_node<anim_graph_node_param>();
  p.set_param_id("w");
  auto& blend = g.add_node<anim_graph_node_blend>();
  blend.get_pose_nodes() = {
      {.id = a.get_id(), .factor = 0.0F}, {.id = b.get_id(), .factor = 1.0F}};
  blend.set_factor_node_id(p.get_id());

  // `a` is also a direct sum child with an unsynced request, while the blend
  // forces a synced request: an interior factor makes them conflict.
  auto& sum = g.add_node<anim_graph_node_sum>();
  sum.set_first_node_id(blend.get_id());
  sum.set_second_node_id(a.get_id());
  g.set_root_node_id(sum.get_id());

  cooked_project cp;
  cp.cook(in);
  anim_graph_player player{*cp.cooked->get_resource<anim_graph>("graph"), true};
  skeleton_pose pose{cp.rig()};
  params parameters;
  parameters.get_value<float>("w") = 0.5F;

  bool threw = false;
  std::string message;
  try {
    player.play(0.1F, parameters, pose);
  }
  catch (const std::invalid_argument& e) {
    threw = true;
    message = e.what();
  }
  TASK_CHECK(threw);
  TASK_CHECK(message.find(std::to_string(a.get_id())) != std::string::npos);

  // Move the blend fully onto b: clip a is now only reached via the sum,
  // so the conflict disappears. The failed play committed nothing, so this
  // is the node's first successful play and its phase starts at zero.
  parameters.get_value<float>("w") = 1.0F;
  player.play(0.1F, parameters, pose);

  // Reference at phase zero (first play): both clips sampled at time zero.
  skeleton_pose pa = sample_clip(*cp.cooked, "a", 0.0F);
  skeleton_pose pb = sample_clip(*cp.cooked, "a", 0.0F);
  skeleton_pose expected{cp.rig()};
  skeleton_pose_add(pb, pa, expected);
  TASK_CHECK(poses_equal(pose, expected));

  // The next healthy play advances the phase normally by dt.
  player.play(0.1F, parameters, pose);
  skeleton_pose pa2 = sample_clip(*cp.cooked, "a", 0.1F);
  skeleton_pose pb2 = sample_clip(*cp.cooked, "a", 0.1F);
  skeleton_pose expected2{cp.rig()};
  skeleton_pose_add(pb2, pa2, expected2);
  TASK_CHECK(poses_equal(pose, expected2));
  return 0;
}

// Stats of the last successful play survive a later failing play.
int rollback_stats_preserved()
{
  project_uncooked in;
  add_base_resources(in);
  build_shared_diamond(in);

  cooked_project cp;
  cp.cook(in);
  anim_graph_player player{*cp.cooked->get_resource<anim_graph>("graph"), true};
  skeleton_pose pose{cp.rig()};
  const params good = make_params(0.25F, 0.75F);

  player.play(0.1F, good, pose);
  const anim_graph_play_stats successful = player.get_last_play_stats();
  TASK_CHECK(successful.evaluated_pose_nodes == 6);
  TASK_CHECK(successful.cache_hits == 1);

  bool threw = false;
  try {
    // NaN dt is rejected before any node state changes.
    player.play(std::nanf(""), good, pose);
  }
  catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);

  const anim_graph_play_stats& after_failure = player.get_last_play_stats();
  TASK_CHECK(after_failure.evaluated_pose_nodes == successful.evaluated_pose_nodes);
  TASK_CHECK(after_failure.executed_jobs == successful.executed_jobs);
  TASK_CHECK(after_failure.cache_hits == successful.cache_hits);
  TASK_CHECK(after_failure.peak_live_poses == successful.peak_live_poses);

  // The next healthy plays are indistinguishable from a continuous sequence:
  // compare against a fresh player that runs the same uninterrupted plays.
  anim_graph_player fresh{*cp.cooked->get_resource<anim_graph>("graph"), true};
  skeleton_pose fresh_pose{cp.rig()};
  fresh.play(0.1F, good, fresh_pose);
  fresh.play(0.1F, good, fresh_pose);
  fresh.play(0.1F, good, fresh_pose);

  player.play(0.1F, good, pose);
  player.play(0.1F, good, pose);
  TASK_CHECK(poses_equal(pose, fresh_pose));
  return 0;
}

// A negative dt fails the validation contract and leaves the player usable.
int rollback_invalid_dt()
{
  project_uncooked in;
  add_base_resources(in);

  auto& g = in.add_resource<anim_graph_uncooked>("graph");
  g.set_skeleton_id("rig");
  auto& a = g.add_node<anim_graph_node_clip>();
  a.set_clip_id("a");
  g.set_root_node_id(a.get_id());

  cooked_project cp;
  cp.cook(in);
  anim_graph_player player{*cp.cooked->get_resource<anim_graph>("graph"), true};
  anim_graph_player fresh{*cp.cooked->get_resource<anim_graph>("graph"), true};
  skeleton_pose pose{cp.rig()};
  skeleton_pose fresh_pose{cp.rig()};
  params empty;

  bool threw = false;
  try {
    player.play(-1.0F, empty, pose);
  }
  catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);

  // Same healthy sequence on the recovered player and on a fresh one.
  for (int i = 0; i < 3; ++i) {
    player.play(0.1F, empty, pose);
    fresh.play(0.1F, empty, fresh_pose);
    TASK_CHECK(poses_equal(pose, fresh_pose));
  }
  return 0;
}
}  // namespace

const struct task_case task_cases[] = {
    {"sharing", "self_blend", sharing_self_blend},
    {"sharing", "nested_diamond", sharing_nested_diamond},
    {"sharing", "sum_and_blend_shared_inputs", sharing_sum_and_blend_inputs},
    {"sharing", "param_switch_active_edges", sharing_param_switch},
    {"ownership", "multiple_consumers", ownership_multiple_consumers},
    {"ownership", "whole_blend_shared", ownership_shared_blend},
    {"ownership", "tree_inplace_reuse", ownership_tree_inplace_reuse},
    {"ownership", "two_players_interleaved", ownership_two_players},
    {"rollback", "conflict_recovery", rollback_conflict_recovery},
    {"rollback", "stats_preserved", rollback_stats_preserved},
    {"rollback", "invalid_dt", rollback_invalid_dt},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
