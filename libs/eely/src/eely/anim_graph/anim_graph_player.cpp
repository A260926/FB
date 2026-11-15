#include "eely/anim_graph/anim_graph_player.h"

#include "eely/anim_graph/anim_graph.h"
#include "eely/anim_graph/anim_graph_node_and.h"
#include "eely/anim_graph/anim_graph_node_base.h"
#include "eely/anim_graph/anim_graph_node_blend.h"
#include "eely/anim_graph/anim_graph_node_clip.h"
#include "eely/anim_graph/anim_graph_node_param.h"
#include "eely/anim_graph/anim_graph_node_param_comparison.h"
#include "eely/anim_graph/anim_graph_node_random.h"
#include "eely/anim_graph/anim_graph_node_state.h"
#include "eely/anim_graph/anim_graph_node_state_condition.h"
#include "eely/anim_graph/anim_graph_node_state_machine.h"
#include "eely/anim_graph/anim_graph_node_state_transition.h"
#include "eely/anim_graph/anim_graph_node_sum.h"
#include "eely/anim_graph/anim_graph_player_context.h"
#include "eely/anim_graph/anim_graph_player_node_and.h"
#include "eely/anim_graph/anim_graph_player_node_base.h"
#include "eely/anim_graph/anim_graph_player_node_blend.h"
#include "eely/anim_graph/anim_graph_player_node_clip.h"
#include "eely/anim_graph/anim_graph_player_node_param.h"
#include "eely/anim_graph/anim_graph_player_node_param_comparison.h"
#include "eely/anim_graph/anim_graph_player_node_pose_base.h"
#include "eely/anim_graph/anim_graph_player_node_random.h"
#include "eely/anim_graph/anim_graph_player_node_speed.h"
#include "eely/anim_graph/anim_graph_player_node_state.h"
#include "eely/anim_graph/anim_graph_player_node_state_condition.h"
#include "eely/anim_graph/anim_graph_player_node_state_machine.h"
#include "eely/anim_graph/anim_graph_player_node_state_transition.h"
#include "eely/anim_graph/anim_graph_player_node_sum.h"
#include "eely/anim_graph/anim_graph_player_shared.h"
#include "eely/base/base_utils.h"
#include "eely/base/graph.h"
#include "eely/clip/clip.h"
#include "eely/job/job_queue.h"
#include "eely/params/params.h"
#include "eely/project/project.h"
#include "eely/skeleton/skeleton_pose.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace eely {
anim_graph_player::anim_graph_player(const anim_graph& anim_graph)
    : anim_graph_player(anim_graph, false)
{
}

anim_graph_player::anim_graph_player(const anim_graph& anim_graph, const bool shared_pose_nodes)
    : _project{anim_graph.get_project()},
      _job_queue{*_project.get_resource<skeleton>(anim_graph.get_skeleton_id())},
      _shared_pose_nodes{shared_pose_nodes}
{
  using namespace eely::internal;

  if (_shared_pose_nodes) {
    validate_shared_graph(anim_graph);
  }

  // Runtime nodes are created in three steps:
  //  - constructing the nodes
  //  - filling their data (done after creation due to possible circular dependencies)
  //  - initializing state's breakpoints (they require fully inited states and transitions)

  const std::vector<anim_graph_node_uptr>& nodes{anim_graph.get_nodes()};

  std::unordered_map<int, anim_graph_player_node_base*> id_to_player_node;

  // Create nodes

  const int root_node_id{anim_graph.get_root_node_id()};

  for (const anim_graph_node_uptr& node : nodes) {
    const int node_id{node->get_id()};

    EXPECTS(!id_to_player_node.contains(node_id));

    anim_graph_player_node_uptr player_node{create_player_node(node)};
    EXPECTS(player_node);

    id_to_player_node[node_id] = player_node.get();
    if (node_id == root_node_id) {
      _root_node = player_node.get();
    }

    _nodes.push_back(std::move(player_node));
  }

  EXPECTS(!_nodes.empty());
  EXPECTS(_root_node != nullptr);

  // Fill node's data

  for (const anim_graph_node_uptr& node : nodes) {
    const int node_id{node->get_id()};

    EXPECTS(id_to_player_node.contains(node_id));

    anim_graph_player_node_base* player_node{id_to_player_node[node_id]};
    init_player_node(node, player_node, id_to_player_node);
  }

  // Initialize breakpoints

  for (const anim_graph_node_uptr& node : nodes) {
    if (node->get_type() != anim_graph_node_type::state) {
      continue;
    }

    const int node_id{node->get_id()};

    EXPECTS(id_to_player_node.contains(node_id));

    anim_graph_player_node_base* player_node{id_to_player_node[node_id]};
    auto* player_node_state{polymorphic_downcast<anim_graph_player_node_state*>(player_node)};

    player_node_state->update_breakpoints();
  }
}

void anim_graph_player::play(const float dt_s, const params& params, skeleton_pose& out_pose)
{
  using namespace eely::internal;

  if (!std::isfinite(dt_s) || dt_s < 0.0F) {
    throw std::invalid_argument("dt_s must be finite and non-negative");
  }

  if (_shared_pose_nodes) {
    // Plan the whole play first. If anything fails, node state is rolled back
    // and the partially filled queue is discarded without touching `out_pose`.
    try {
      ++_play_counter;

      _job_queue.begin_play(true);
      _shared_runtime.begin_play();

      anim_graph_player_context context{.job_queue = _job_queue,
                                        .params = params,
                                        .play_counter = _play_counter,
                                        .dt_s = dt_s,
                                        .shared_runtime = &_shared_runtime};

      _root_node->compute(context);

      _job_queue.execute(out_pose);

      _last_play_stats = anim_graph_play_stats{
          .evaluated_pose_nodes = _shared_runtime.count_evaluated_pose_nodes(),
          .executed_jobs = _job_queue.get_executed_jobs_count(),
          .cache_hits = _shared_runtime.get_cache_hits(),
          .peak_live_poses = _job_queue.get_peak_live_poses()};
    }
    catch (...) {
      _job_queue.abort_play();
      _shared_runtime.rollback();
      --_play_counter;
      throw;
    }

    return;
  }

  ++_play_counter;

  _job_queue.begin_play(false);

  anim_graph_player_context context{
      .job_queue = _job_queue, .params = params, .play_counter = _play_counter, .dt_s = dt_s};

  _root_node->compute(context);

  _job_queue.execute(out_pose);

  _last_play_stats = anim_graph_play_stats{
      .evaluated_pose_nodes = count_active_pose_nodes(),
      .executed_jobs = _job_queue.get_executed_jobs_count(),
      .cache_hits = 0,
      .peak_live_poses = _job_queue.get_peak_live_poses()};
}

const anim_graph_play_stats& anim_graph_player::get_last_play_stats() const
{
  return _last_play_stats;
}

const std::vector<internal::anim_graph_player_node_uptr>& anim_graph_player::get_nodes() const
{
  return _nodes;
}

const internal::anim_graph_player_node_base* anim_graph_player::get_player_node(const int id) const
{
  using namespace internal;

  for (const anim_graph_player_node_uptr& node : _nodes) {
    if (node->get_id() == id) {
      return node.get();
    }
  }

  return nullptr;
}

bool anim_graph_player::is_player_node_active(
    const internal::anim_graph_player_node_base& node) const
{
  return node.get_last_play_counter() == _play_counter;
}

std::size_t anim_graph_player::count_active_pose_nodes() const
{
  std::size_t count{0};
  for (const internal::anim_graph_player_node_uptr& node : _nodes) {
    const anim_graph_node_type type{node->get_type()};
    const bool is_pose_node{type == anim_graph_node_type::clip ||
                            type == anim_graph_node_type::blend ||
                            type == anim_graph_node_type::sum};
    if (is_pose_node && node->get_last_play_counter() == _play_counter) {
      ++count;
    }
  }
  return count;
}

void anim_graph_player::validate_shared_graph(const anim_graph& anim_graph) const
{
  using namespace eely::internal;

  const std::vector<anim_graph_node_uptr>& nodes{anim_graph.get_nodes()};
  if (nodes.empty()) {
    throw std::invalid_argument("Shared pose graph must contain nodes");
  }

  // Map every id to its node and make sure ids are unique.

  std::unordered_map<int, const anim_graph_node_base*> id_to_node;
  for (const anim_graph_node_uptr& node : nodes) {
    if (id_to_node.contains(node->get_id())) {
      throw std::invalid_argument("Duplicated node id " + std::to_string(node->get_id()));
    }
    id_to_node.emplace(node->get_id(), node.get());
  }

  const int root_node_id{anim_graph.get_root_node_id()};
  const auto root_iter{id_to_node.find(root_node_id)};
  if (root_iter == id_to_node.end()) {
    throw std::invalid_argument("Root node id " + std::to_string(root_node_id) + " is missing");
  }

  const auto is_pose_type = [](const anim_graph_node_type type) {
    return type == anim_graph_node_type::clip || type == anim_graph_node_type::blend ||
           type == anim_graph_node_type::sum;
  };

  if (!is_pose_type(root_iter->second->get_type())) {
    throw std::invalid_argument("Root node with id " + std::to_string(root_node_id) +
                                " must produce a pose");
  }

  const auto require_node = [&](const std::optional<int>& id, const char* edge_name) {
    if (!id.has_value()) {
      throw std::invalid_argument(std::string{"Unset "} + edge_name + " reference");
    }
    const auto iter{id_to_node.find(id.value())};
    if (iter == id_to_node.end()) {
      throw std::invalid_argument(std::string{"Missing node id "} + std::to_string(id.value()) +
                                  " referenced by " + edge_name);
    }
    return iter->second;
  };

  // DFS from the root. Every edge is inspected, including blend candidate
  // edges that are not active for a particular parameter value.
  // Gray marks detect cycles; reaching a black node is a legal shared
  // reference in an acyclic graph, not a cycle.

  enum class mark { none, temporary, permanent };
  std::unordered_map<int, mark> marks;

  const std::function<void(const anim_graph_node_base*)> visit = [&](const anim_graph_node_base* node) {
    const int node_id{node->get_id()};
    mark& node_mark{marks[node_id]};

    if (node_mark == mark::permanent) {
      return;
    }
    if (node_mark == mark::temporary) {
      throw std::invalid_argument("Cycle detected through node id " + std::to_string(node_id));
    }
    node_mark = mark::temporary;

    switch (node->get_type()) {
      case anim_graph_node_type::clip: {
        const auto* node_clip{polymorphic_downcast<const anim_graph_node_clip*>(node)};
        if (_project.get_resource<clip>(node_clip->get_clip_id()) == nullptr) {
          throw std::invalid_argument("Clip node id " + std::to_string(node_id) +
                                      " references missing clip resource");
        }
      } break;

      case anim_graph_node_type::param: {
        // Float parameter nodes are the only data nodes supported.
      } break;

      case anim_graph_node_type::blend: {
        const auto* node_blend{polymorphic_downcast<const anim_graph_node_blend*>(node)};
        const auto& pose_nodes_data{node_blend->get_pose_nodes()};

        if (pose_nodes_data.empty()) {
          throw std::invalid_argument("Blend node id " + std::to_string(node_id) +
                                      " has no pose candidates");
        }

        std::optional<float> previous_factor;
        for (const anim_graph_node_blend::pose_node_data& pose_node_data : pose_nodes_data) {
          if (!std::isfinite(pose_node_data.factor)) {
            throw std::invalid_argument("Blend node id " + std::to_string(node_id) +
                                        " has a non-finite factor");
          }
          if (previous_factor.has_value() &&
              !(pose_node_data.factor > previous_factor.value())) {
            throw std::invalid_argument("Blend node id " + std::to_string(node_id) +
                                        " has factors that are not strictly increasing");
          }
          previous_factor = pose_node_data.factor;

          const anim_graph_node_base* child{require_node(pose_node_data.id, "blend candidate")};
          if (!is_pose_type(child->get_type())) {
            throw std::invalid_argument("Blend candidate id " +
                                        std::to_string(child->get_id()) +
                                        " is not a pose node");
          }
          visit(child);
        }

        const anim_graph_node_base* factor_node{
            require_node(node_blend->get_factor_node_id(), "blend factor")};
        if (factor_node->get_type() != anim_graph_node_type::param) {
          throw std::invalid_argument("Blend factor node id " +
                                      std::to_string(factor_node->get_id()) +
                                      " must be a float param node");
        }
        visit(factor_node);
      } break;

      case anim_graph_node_type::sum: {
        const auto* node_sum{polymorphic_downcast<const anim_graph_node_sum*>(node)};

        const anim_graph_node_base* first{
            require_node(node_sum->get_first_node_id(), "sum first")};
        if (!is_pose_type(first->get_type())) {
          throw std::invalid_argument("Sum first node id " + std::to_string(first->get_id()) +
                                      " is not a pose node");
        }
        visit(first);

        const anim_graph_node_base* second{
            require_node(node_sum->get_second_node_id(), "sum second")};
        if (!is_pose_type(second->get_type())) {
          throw std::invalid_argument("Sum second node id " + std::to_string(second->get_id()) +
                                      " is not a pose node");
        }
        visit(second);
      } break;

      default: {
        throw std::invalid_argument("Shared pose graph does not support node id " +
                                    std::to_string(node_id));
      } break;
    }

    node_mark = mark::permanent;
  };

  visit(root_iter->second);
}

internal::anim_graph_player_node_uptr anim_graph_player::create_player_node(
    const anim_graph_node_uptr& node)
{
  using namespace eely::internal;

  const int id{node->get_id()};

  switch (node->get_type()) {
    case anim_graph_node_type::and_logic: {
      return std::make_unique<anim_graph_player_node_and>(id);
    } break;

    case anim_graph_node_type::blend: {
      return std::make_unique<anim_graph_player_node_blend>(id);
    } break;

    case anim_graph_node_type::clip: {
      const auto* node_clip{polymorphic_downcast<const anim_graph_node_clip*>(node.get())};
      const auto& clip{*_project.get_resource<eely::clip>(node_clip->get_clip_id())};
      return std::make_unique<anim_graph_player_node_clip>(id, clip);
    } break;

    case anim_graph_node_type::param_comparison: {
      const auto* node_param_comparison{
          polymorphic_downcast<const anim_graph_node_param_comparison*>(node.get())};
      return std::make_unique<anim_graph_player_node_param_comparison>(
          id, node_param_comparison->get_param_id(), node_param_comparison->get_value(),
          node_param_comparison->get_op());
    } break;

    case anim_graph_node_type::param: {
      const auto* node_param{polymorphic_downcast<const anim_graph_node_param*>(node.get())};
      return std::make_unique<anim_graph_player_node_param>(id, node_param->get_param_id());
    } break;

    case anim_graph_node_type::random: {
      return std::make_unique<anim_graph_player_node_random>(id);
    } break;

    case anim_graph_node_type::speed: {
      return std::make_unique<anim_graph_player_node_speed>(id);
    } break;

    case anim_graph_node_type::state_condition: {
      const auto* node_state_condition{
          polymorphic_downcast<const anim_graph_node_state_condition*>(node.get())};
      return std::make_unique<anim_graph_player_node_state_condition>(
          id, node_state_condition->get_phase());
    } break;

    case anim_graph_node_type::state_machine: {
      return std::make_unique<anim_graph_player_node_state_machine>(id);
    } break;

    case anim_graph_node_type::state_transition: {
      const auto* node_state_transition{
          polymorphic_downcast<const anim_graph_node_state_transition*>(node.get())};
      return std::make_unique<anim_graph_player_node_state_transition>(
          id, node_state_transition->get_transition_type(), node_state_transition->get_duration_s(),
          node_state_transition->get_reversible());
    } break;

    case anim_graph_node_type::state: {
      const auto* node_state{polymorphic_downcast<const anim_graph_node_state*>(node.get())};
      return std::make_unique<anim_graph_player_node_state>(id, node_state->get_name());
    } break;

    case anim_graph_node_type::sum: {
      return std::make_unique<anim_graph_player_node_sum>(id);
    } break;

    default: {
      EXPECTS(false);
      return nullptr;
    } break;
  }
}

void anim_graph_player::init_player_node(
    const anim_graph_node_uptr& node,
    internal::anim_graph_player_node_base* player_node,
    const std::unordered_map<int, internal::anim_graph_player_node_base*>& id_to_player_node)
{
  using namespace eely::internal;

  switch (node->get_type()) {
    case anim_graph_node_type::and_logic: {
      const auto* node_and{polymorphic_downcast<const anim_graph_node_and*>(node.get())};
      auto* player_node_and{polymorphic_downcast<anim_graph_player_node_and*>(player_node)};

      // Init children nodes

      const std::vector<int>& children_nodes{node_and->get_children_nodes()};
      std::vector<anim_graph_player_node_base*>& player_children_nodes{
          player_node_and->get_children_nodes()};

      for (const int children_node_id : children_nodes) {
        EXPECTS(id_to_player_node.contains(children_node_id));
        auto* player_children_node{id_to_player_node.at(children_node_id)};

        player_children_nodes.push_back(player_children_node);
      }

    } break;

    case anim_graph_node_type::blend: {
      const auto* node_blend{polymorphic_downcast<const anim_graph_node_blend*>(node.get())};
      auto* player_node_blend{polymorphic_downcast<anim_graph_player_node_blend*>(player_node)};

      // Init children pose nodes

      const auto& pose_nodes_data{node_blend->get_pose_nodes()};
      std::vector<anim_graph_player_node_blend::pose_node_data>& player_pose_nodes_data{
          player_node_blend->get_pose_nodes()};

      for (const anim_graph_node_blend::pose_node_data& pose_node_data : pose_nodes_data) {
        EXPECTS(pose_node_data.id.has_value());
        const int pose_node_id{pose_node_data.id.value()};

        EXPECTS(id_to_player_node.contains(pose_node_id));
        auto* player_pose_node{id_to_player_node.at(pose_node_id)};

        player_pose_nodes_data.push_back(
            {.node = *polymorphic_downcast<anim_graph_player_node_pose_base*>(player_pose_node),
             .factor = pose_node_data.factor});
      }

      // Init factor node

      EXPECTS(node_blend->get_factor_node_id().has_value());
      const int factor_node_id{node_blend->get_factor_node_id().value()};

      EXPECTS(id_to_player_node.contains(factor_node_id));
      anim_graph_player_node_base* player_factor_node{id_to_player_node.at(factor_node_id)};

      player_node_blend->set_factor_node(player_factor_node);
    } break;

    case anim_graph_node_type::random: {
      const auto* node_random{polymorphic_downcast<const anim_graph_node_random*>(node.get())};
      auto* player_node_random{polymorphic_downcast<anim_graph_player_node_random*>(player_node)};

      // Init children nodes

      const auto& children_nodes{node_random->get_children_nodes()};
      std::vector<anim_graph_player_node_pose_base*> player_children_nodes;

      for (const int children_node_id : children_nodes) {
        EXPECTS(id_to_player_node.contains(children_node_id));
        auto* player_children_node{polymorphic_downcast<anim_graph_player_node_pose_base*>(
            id_to_player_node.at(children_node_id))};

        player_children_nodes.push_back(player_children_node);
      }

      player_node_random->set_children_nodes(std::move(player_children_nodes));

    } break;

    case anim_graph_node_type::speed: {
      const auto* node_speed{polymorphic_downcast<const anim_graph_node_speed*>(node.get())};
      auto* player_node_speed{polymorphic_downcast<anim_graph_player_node_speed*>(player_node)};

      EXPECTS(node_speed->get_child_node().has_value());
      const int child_node_id{node_speed->get_child_node().value()};

      EXPECTS(id_to_player_node.contains(child_node_id));
      auto* player_child_node{id_to_player_node.at(child_node_id)};

      EXPECTS(node_speed->get_speed_provider_node().has_value());
      const int speed_provider_node_id{node_speed->get_speed_provider_node().value()};

      EXPECTS(id_to_player_node.contains(speed_provider_node_id));
      auto* speed_provider_node{id_to_player_node.at(speed_provider_node_id)};

      player_node_speed->set_child_node(player_child_node);
      player_node_speed->set_speed_provider_node(speed_provider_node);
    } break;

    case anim_graph_node_type::state_machine: {
      const auto* node_state_machine{
          polymorphic_downcast<const anim_graph_node_state_machine*>(node.get())};
      auto* player_node_state_machine{
          polymorphic_downcast<anim_graph_player_node_state_machine*>(player_node)};

      const auto& state_nodes{node_state_machine->get_state_nodes()};
      std::vector<anim_graph_player_node_state*> player_state_nodes;

      for (const int state_node_id : state_nodes) {
        EXPECTS(id_to_player_node.contains(state_node_id));
        auto* player_node_state{polymorphic_downcast<anim_graph_player_node_state*>(
            id_to_player_node.at(state_node_id))};

        player_state_nodes.push_back(player_node_state);
      }

      player_node_state_machine->set_state_nodes(std::move(player_state_nodes));
    } break;

    case anim_graph_node_type::state_transition: {
      const auto* node_state_transition{
          polymorphic_downcast<const anim_graph_node_state_transition*>(node.get())};
      auto* player_node_state_transition{
          polymorphic_downcast<anim_graph_player_node_state_transition*>(player_node)};

      // Init condition node

      EXPECTS(node_state_transition->get_condition_node().has_value());
      const int condition_node_id{node_state_transition->get_condition_node().value()};

      EXPECTS(id_to_player_node.contains(condition_node_id));
      anim_graph_player_node_base* player_node_condition{id_to_player_node.at(condition_node_id)};

      player_node_state_transition->set_condition_node(player_node_condition);

      // Init destination state node

      EXPECTS(node_state_transition->get_destination_state_node().has_value());
      const int destination_state_node_id{
          node_state_transition->get_destination_state_node().value()};

      EXPECTS(id_to_player_node.contains(destination_state_node_id));
      anim_graph_player_node_state* player_node_destination_state{
          polymorphic_downcast<anim_graph_player_node_state*>(
              id_to_player_node.at(destination_state_node_id))};

      player_node_state_transition->set_original_destination_state_node(
          player_node_destination_state);
    } break;

    case anim_graph_node_type::state: {
      const auto* node_state{polymorphic_downcast<const anim_graph_node_state*>(node.get())};
      auto* player_node_state{polymorphic_downcast<anim_graph_player_node_state*>(player_node)};

      // Init out transition nodes

      const auto& out_transition_nodes{node_state->get_out_transition_nodes()};
      std::vector<anim_graph_player_node_state_transition*> out_transition_player_nodes;

      for (const int transition_node_id : out_transition_nodes) {
        EXPECTS(id_to_player_node.contains(transition_node_id));

        auto* player_node_transition{polymorphic_downcast<anim_graph_player_node_state_transition*>(
            id_to_player_node.at(transition_node_id))};
        out_transition_player_nodes.push_back(player_node_transition);
      }

      player_node_state->set_out_transitions(out_transition_player_nodes);

      // Init pose node

      EXPECTS(node_state->get_pose_node().has_value());
      const int pose_node_id{node_state->get_pose_node().value()};

      EXPECTS(id_to_player_node.contains(pose_node_id));
      auto* player_node_pose{polymorphic_downcast<anim_graph_player_node_pose_base*>(
          id_to_player_node.at(pose_node_id))};

      player_node_state->set_pose_node(player_node_pose);
    } break;

    case anim_graph_node_type::sum: {
      const auto* node_sum{polymorphic_downcast<const anim_graph_node_sum*>(node.get())};
      auto* player_node_sum{polymorphic_downcast<anim_graph_player_node_sum*>(player_node)};

      EXPECTS(node_sum->get_first_node_id().has_value());
      const int first_node_id{node_sum->get_first_node_id().value()};

      EXPECTS(id_to_player_node.contains(first_node_id));
      auto* player_node_first{polymorphic_downcast<anim_graph_player_node_pose_base*>(
          id_to_player_node.at(first_node_id))};

      EXPECTS(node_sum->get_second_node_id().has_value());
      const int second_node_id{node_sum->get_second_node_id().value()};

      EXPECTS(id_to_player_node.contains(second_node_id));
      auto* player_node_second{polymorphic_downcast<anim_graph_player_node_pose_base*>(
          id_to_player_node.at(second_node_id))};

      player_node_sum->set_first_node(player_node_first);
      player_node_sum->set_second_node(player_node_second);
    } break;

    default: {
      // Other types do not need any setup after creation
    } break;
  }
}
}  // namespace eely