#pragma once

#include "eely/anim_graph/anim_graph_player_context.h"

#include <gsl/util>

#include <any>
#include <functional>
#include <optional>
#include <unordered_map>

namespace eely::internal {
class anim_graph_player_node_base;
class anim_graph_player_node_pose_base;
class anim_graph_player_node_blend;

// Phase request with which a pose node is asked to evaluate during one play.
// Two requests are equal only when every field matches exactly,
// i.e. floating point values are compared with `==`.
struct shared_phase_request final {
  bool sync_enabled{false};
  std::optional<float> sync_phase;
  float dt_s{0.0F};

  bool operator==(const shared_phase_request& other) const = default;
};

// Per-node mutable state of a shared-pose graph play.
struct shared_node_runtime final {
  // Duration planning.
  bool duration_planned{false};

  // Phase planning.
  bool phase_planned{false};
  shared_phase_request request;

  // Pose evaluation caching.
  bool evaluated{false};
  gsl::index job_index{-1};

  // Cached result of non-pose nodes (e.g. float param nodes).
  bool value_cached{false};
  std::any cached_value;
};

// Saved state of a node, used to roll a failed play back completely.
struct shared_node_snapshot final {
  anim_graph_player_node_base* node{nullptr};

  std::optional<int> last_play_counter;
  bool is_first_play{false};

  // Only valid for pose nodes.
  bool has_phase{false};
  float phase{0.0F};

  // Only valid for blend nodes.
  bool has_blend_selection{false};
  std::optional<float> prev_factor;
  const anim_graph_player_node_pose_base* source_node{nullptr};
  const anim_graph_player_node_pose_base* destination_node{nullptr};
  float destination_node_weight{0.0F};
};

// Coordinates a single shared-pose graph play:
// validates phase requests, makes sure every runtime node is evaluated once,
// caches its result for all consumers and snapshots node state for rollback.
class anim_graph_player_shared final {
public:
  explicit anim_graph_player_shared() = default;

  // Prepare for a new graph play.
  void begin_play();

  // Restore every touched node to its state before the current play.
  // Called when the play fails before finishing.
  void rollback();

  // Snapshot a node on its first touch during the current play.
  void snapshot_node(anim_graph_player_node_base& node);
  void snapshot_pose_node(anim_graph_player_node_pose_base& node);
  void snapshot_blend_node(anim_graph_player_node_blend& node);

  [[nodiscard]] shared_node_runtime& get_entry(const anim_graph_player_node_base& node);
  [[nodiscard]] shared_node_runtime& get_entry(int node_id);

  // Validate and remember the phase request of a pose node.
  // Throws `std::invalid_argument` (with node id in the message) if an active
  // node was already planned with a different request.
  // Returns `true` if this node was already planned in the current play.
  bool begin_phase_planning(const anim_graph_player_node_pose_base& node,
                            const anim_graph_player_context& context);

  // Evaluate a pose node once and share its job index on repeated requests.
  // `evaluate` is called only on a cache miss and must enqueue the node's job.
  gsl::index evaluate_cached_pose(const anim_graph_player_node_pose_base& node,
                                  const anim_graph_player_context& context,
                                  const std::function<gsl::index()>& evaluate);

  // Evaluate a non-pose node (e.g. a param node) once and cache its value.
  std::any evaluate_cached_value(const anim_graph_player_node_base& node,
                                 const anim_graph_player_context& context,
                                 const std::function<std::any()>& evaluate);

  // Number of times a cached pose-node result was reused during the play.
  [[nodiscard]] std::size_t get_cache_hits() const;

  // Number of distinct pose nodes actually evaluated during the play.
  [[nodiscard]] std::size_t count_evaluated_pose_nodes() const;

private:
  std::unordered_map<int, shared_node_runtime> _entries;
  std::unordered_map<int, shared_node_snapshot> _snapshots;

  // Distinct pose nodes really evaluated (only pose nodes use cached poses).
  std::size_t _evaluated_pose_nodes{0};
  std::size_t _cache_hits{0};
};
}  // namespace eely::internal
