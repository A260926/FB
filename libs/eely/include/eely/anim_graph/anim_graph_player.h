#pragma once

#include "eely/anim_graph/anim_graph.h"
#include "eely/anim_graph/anim_graph_play_stats.h"
#include "eely/anim_graph/anim_graph_player_node_base.h"
#include "eely/anim_graph/anim_graph_player_shared.h"
#include "eely/job/job_queue.h"
#include "eely/params/params.h"
#include "eely/project/project.h"
#include "eely/skeleton/skeleton_pose.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace eely {
// Player for animation graphs.
class anim_graph_player final {
public:
  // Create a player for the specified graph.
  // Equivalent to `anim_graph_player(anim_graph, false)`.
  explicit anim_graph_player(const anim_graph& anim_graph);

  // Create a player for the specified graph.
  // When `shared_pose_nodes` is `false` the graph is evaluated as a tree,
  // i.e. every node reference is traversed independently.
  // When it is `true`, a single runtime node is evaluated at most once
  // per play and its pose is shared between every branch that references it.
  // Shared mode currently supports clip, blend, sum and float param nodes,
  // and the root node must produce a pose.
  // Throws `std::invalid_argument` if references reachable from the root
  // are missing, duplicated, have wrong types, form a cycle,
  // if a blend factor table is invalid, or if a reachable node has
  // an unsupported type.
  anim_graph_player(const anim_graph& anim_graph, bool shared_pose_nodes);

  // Play a graph and put results into `out_pose`.
  // In shared mode, throws `std::invalid_argument` if an active pose node
  // receives conflicting evaluation requests during the same play.
  // On failure neither `out_pose`, nor any committed node state, nor
  // the last successful stats are modified.
  void play(float dt_s, const params& params, skeleton_pose& out_pose);

  // Get stats of the last successful play.
  // Before the first successful play all values are zero.
  [[nodiscard]] const anim_graph_play_stats& get_last_play_stats() const;

  // Get list of all runtime nodes.
  [[nodiscard]] const std::vector<internal::anim_graph_player_node_uptr>& get_nodes() const;

  // Get runtime node by id.
  [[nodiscard]] const internal::anim_graph_player_node_base* get_player_node(int id) const;

  // Return `true` if specified node was active on last play.
  // Note: this is queried from player instead of a node itself,
  // because nodes don't receive any signals about being deactivated.
  [[nodiscard]] bool is_player_node_active(const internal::anim_graph_player_node_base& node) const;

private:
  internal::anim_graph_player_node_uptr create_player_node(const anim_graph_node_uptr& node);
  static void init_player_node(
      const anim_graph_node_uptr& node,
      internal::anim_graph_player_node_base* player_node,
      const std::unordered_map<int, internal::anim_graph_player_node_base*>& id_to_player_node);

  // Validate a graph for shared-pose-node mode.
  // Throws `std::invalid_argument` on any structural problem.
  void validate_shared_graph(const anim_graph& anim_graph) const;

  // Count distinct pose nodes (clip, blend, sum) active during the last play.
  [[nodiscard]] std::size_t count_active_pose_nodes() const;

  const project& _project;
  std::vector<internal::anim_graph_player_node_uptr> _nodes;
  internal::anim_graph_player_node_base* _root_node{nullptr};
  internal::job_queue _job_queue;
  internal::anim_graph_player_shared _shared_runtime;
  bool _shared_pose_nodes{false};
  int _play_counter{0};
  anim_graph_play_stats _last_play_stats;
};
}  // namespace eely