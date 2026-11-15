#include "eely/anim_graph/anim_graph_player_shared.h"

#include "eely/anim_graph/anim_graph_player_node_base.h"
#include "eely/anim_graph/anim_graph_player_node_blend.h"
#include "eely/anim_graph/anim_graph_player_node_pose_base.h"
#include "eely/base/assert.h"

#include <stdexcept>
#include <string>

namespace eely::internal {
void anim_graph_player_shared::begin_play()
{
  _entries.clear();
  _snapshots.clear();
  _evaluated_pose_nodes = 0;
  _cache_hits = 0;
}

shared_node_runtime& anim_graph_player_shared::get_entry(
    const anim_graph_player_node_base& node)
{
  return _entries[node.get_id()];
}

shared_node_runtime& anim_graph_player_shared::get_entry(const int node_id)
{
  return _entries[node_id];
}

void anim_graph_player_shared::snapshot_node(anim_graph_player_node_base& node)
{
  if (_snapshots.contains(node.get_id())) {
    return;
  }

  shared_node_snapshot snapshot;
  snapshot.node = &node;
  snapshot.last_play_counter = node._last_graph_play_counter;
  snapshot.is_first_play = node._is_first_play;
  _snapshots[node.get_id()] = snapshot;
}

void anim_graph_player_shared::snapshot_pose_node(anim_graph_player_node_pose_base& node)
{
  snapshot_node(node);

  shared_node_snapshot& snapshot{_snapshots[node.get_id()]};
  if (!snapshot.has_phase) {
    snapshot.has_phase = true;
    snapshot.phase = node._phase;
  }
}

void anim_graph_player_shared::snapshot_blend_node(anim_graph_player_node_blend& node)
{
  snapshot_pose_node(node);

  shared_node_snapshot& snapshot{_snapshots[node.get_id()]};
  if (!snapshot.has_blend_selection) {
    snapshot.has_blend_selection = true;
    snapshot.prev_factor = node._prev_factor;
    snapshot.source_node = node._sorce_node;
    snapshot.destination_node = node._destination_node;
    snapshot.destination_node_weight = node._destination_node_weight;
  }
}

bool anim_graph_player_shared::begin_phase_planning(
    const anim_graph_player_node_pose_base& node,
    const anim_graph_player_context& context)
{
  shared_node_runtime& entry{get_entry(node)};

  const shared_phase_request request{.sync_enabled = context.sync_enabled,
                                     .sync_phase = context.sync_phase,
                                     .dt_s = context.dt_s};

  if (entry.phase_planned) {
    if (!(entry.request == request)) {
      throw std::invalid_argument("Conflicting pose requests for shared node with id " +
                                  std::to_string(node.get_id()));
    }

    return true;
  }

  entry.phase_planned = true;
  entry.request = request;
  return false;
}

gsl::index anim_graph_player_shared::evaluate_cached_pose(
    const anim_graph_player_node_pose_base& node,
    const anim_graph_player_context& /*context*/,
    const std::function<gsl::index()>& evaluate)
{
  const int node_id{node.get_id()};
  shared_node_runtime& entry{get_entry(node_id)};

  if (entry.evaluated) {
    ++_cache_hits;
    return entry.job_index;
  }

  // Run the evaluation first: it can recursively touch other nodes and
  // rehash the entries map, so no map reference may be held across it.
  const gsl::index job_index{evaluate()};

  shared_node_runtime& result_entry{get_entry(node_id)};
  result_entry.evaluated = true;
  result_entry.job_index = job_index;
  ++_evaluated_pose_nodes;
  return job_index;
}

std::any anim_graph_player_shared::evaluate_cached_value(
    const anim_graph_player_node_base& node,
    const anim_graph_player_context& /*context*/,
    const std::function<std::any()>& evaluate)
{
  const int node_id{node.get_id()};
  shared_node_runtime& entry{get_entry(node_id)};

  if (entry.value_cached) {
    return entry.cached_value;
  }

  // Same rehash caveat as in `evaluate_cached_pose`.
  std::any value{evaluate()};

  shared_node_runtime& result_entry{get_entry(node_id)};
  result_entry.value_cached = true;
  result_entry.cached_value = value;
  return value;
}

void anim_graph_player_shared::rollback()
{
  for (auto& [id, snapshot] : _snapshots) {
    anim_graph_player_node_base& node{*snapshot.node};

    node._last_graph_play_counter = snapshot.last_play_counter;
    node._is_first_play = snapshot.is_first_play;

    if (snapshot.has_phase) {
      auto* pose_node{polymorphic_downcast<anim_graph_player_node_pose_base*>(&node)};
      pose_node->_phase = snapshot.phase;
    }

    if (snapshot.has_blend_selection) {
      auto* blend_node{polymorphic_downcast<anim_graph_player_node_blend*>(&node)};
      blend_node->_prev_factor = snapshot.prev_factor;
      blend_node->_sorce_node = const_cast<anim_graph_player_node_pose_base*>(snapshot.source_node);
      blend_node->_destination_node =
          const_cast<anim_graph_player_node_pose_base*>(snapshot.destination_node);
      blend_node->_destination_node_weight = snapshot.destination_node_weight;
    }
  }

  _entries.clear();
  _snapshots.clear();
}

std::size_t anim_graph_player_shared::get_cache_hits() const
{
  return _cache_hits;
}

std::size_t anim_graph_player_shared::count_evaluated_pose_nodes() const
{
  return _evaluated_pose_nodes;
}
}  // namespace eely::internal
