#pragma once

#include "eely/job/job_queue.h"
#include "eely/params/params.h"

#include <optional>

namespace eely::internal {
class anim_graph_player_shared;

// Context for animation graph update.
struct anim_graph_player_context final {
  // Queue to feed jobs into by pose nodes.
  job_queue& job_queue;

  // External parameters that control the graph.
  const params& params;

  // Index of a graph play.
  // Incremented every time a graph is computed.
  int play_counter{0};

  // Time passed since last update.
  float dt_s{0.0F};

  // `True` = node that received this context is in a sync mode,
  // i.e. its phase is given by its parent node.
  // Used to synchronize animations based on their normalized phase.
  // This is a separate flag from `sync_phase` because there are cases
  // when we don't know exact phase yet but we do know that it will be synced,
  // e.g. in `update_duration` method of a pose node.
  bool sync_enabled = false;

  // Phase given by a parent node to be used in sync mode.
  std::optional<float> sync_phase;

  // Set only when the player works in shared-pose-node mode.
  // Nodes consult this object to evaluate each runtime node at most once
  // and to share its result between all consuming branches.
  // `nullptr` keeps the original tree-like evaluation behavior.
  anim_graph_player_shared* shared_runtime{nullptr};
};
}  // namespace eely::internal