#include "eely/anim_graph/anim_graph_player_node_base.h"

#include "eely/anim_graph/anim_graph_node_base.h"
#include "eely/anim_graph/anim_graph_player_context.h"
#include "eely/anim_graph/anim_graph_player_shared.h"

#include <functional>
#include <optional>

namespace eely::internal {
anim_graph_player_node_base::anim_graph_player_node_base(const anim_graph_node_type type,
                                                         const int id)
    : _type{type}, _id{id}
{
}

std::any anim_graph_player_node_base::compute(const anim_graph_player_context& context)
{
  if (context.shared_runtime != nullptr) {
    return compute_shared(context);
  }

  std::any result;
  compute_impl(context, result);

  return result;
}

std::any anim_graph_player_node_base::compute_shared(
    const anim_graph_player_context& context)
{
  anim_graph_player_shared& shared{*context.shared_runtime};

  shared.snapshot_node(*this);

  return shared.evaluate_cached_value(
      *this, context, [&]() {
        std::any result;
        compute_impl(context, result);
        return result;
      });
}

void anim_graph_player_node_base::compute_impl(const anim_graph_player_context& context,
                                               std::any& /*out_result*/)
{
  register_play(context);
}
}  // namespace eely::internal