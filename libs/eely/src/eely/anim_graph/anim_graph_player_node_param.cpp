#include "eely/anim_graph/anim_graph_player_node_param.h"

#include "eely/anim_graph/anim_graph_node_param.h"
#include "eely/anim_graph/anim_graph_player_context.h"
#include "eely/anim_graph/anim_graph_player_node_base.h"
#include "eely/base/string_id.h"
#include "eely/params/params.h"

#include <any>
#include <cmath>
#include <stdexcept>
#include <string>

namespace eely::internal {
anim_graph_player_node_param::anim_graph_player_node_param(const int id, string_id param_id)
    : anim_graph_player_node_base{anim_graph_node_type::param, id}, _param_id{std::move(param_id)}
{
}

void anim_graph_player_node_param::compute_impl(const anim_graph_player_context& context,
                                                std::any& out_result)
{
  anim_graph_player_node_base::compute_impl(context, out_result);

  // TODO: a less verbose way to convert variant to any?
  // This is ugly.

  const param_value& value{context.params.get(_param_id)};

  switch (value.index()) {
    case 0: {
      static_assert(std::is_same_v<std::variant_alternative_t<0, param_value>, std::monostate>);
      if (context.shared_runtime != nullptr) {
        // Shared graphs only support float params; an unset one defaults to zero.
        out_result = 0.0F;
      }
      else {
        out_result = {};
      }
    } break;

    case 1: {
      static_assert(std::is_same_v<std::variant_alternative_t<1, param_value>, int>);
      if (context.shared_runtime != nullptr) {
        throw std::invalid_argument("Param node id " + std::to_string(get_id()) +
                                    " must provide a float value in shared pose mode");
      }
      out_result = std::get<int>(value);
    } break;

    case 2: {
      static_assert(std::is_same_v<std::variant_alternative_t<2, param_value>, float>);
      const float float_value{std::get<float>(value)};
      if (context.shared_runtime != nullptr && !std::isfinite(float_value)) {
        throw std::invalid_argument("Param node id " + std::to_string(get_id()) +
                                    " has a non-finite float value");
      }
      out_result = float_value;
    } break;

    case 3: {
      static_assert(std::is_same_v<std::variant_alternative_t<3, param_value>, bool>);
      if (context.shared_runtime != nullptr) {
        throw std::invalid_argument("Param node id " + std::to_string(get_id()) +
                                    " must provide a float value in shared pose mode");
      }
      out_result = std::get<bool>(value);
    } break;

    default: {
      throw std::runtime_error("Unknown param value type");
    } break;
  }
}
}  // namespace eely::internal