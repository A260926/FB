#include "model/causal_trace.hpp"

#include <algorithm>
#include <limits>
#include <queue>
#include <stdexcept>
#include <vector>

namespace model {

CausalTrace ModelChecker::causal_trace(const Schedule& schedule,
                                       std::size_t max_events) const {
    // The evidence hooks currently have SC semantics only. TSO/PSO buffer
    // visibility would need flush-aware provenance that this graph does not
    // claim to provide, so reject the request explicitly instead of emitting
    // an SC-shaped graph for a buffered execution.
    if (memory_model_ != MemoryModel::SC) {
        throw std::invalid_argument(
            "ModelChecker::causal_trace requires MemoryModel::SC; "
            "non-SC causal evidence is not supported");
    }
    return causal_detail::run_causal_trace(
        program_, schedule, step_bound_, memory_model_, max_events);
}

std::vector<std::size_t> CausalTrace::explain(std::size_t from,
                                             std::size_t to) const {
    const std::size_t event_count = events.size();
    if (from >= event_count || to >= event_count) {
        throw std::out_of_range(
            "CausalTrace::explain event id is outside the causal trace");
    }
    // A query of an event against itself is the reflexive case, not a strict
    // happens-before query, and must not return a zero-length "path".
    if (from == to) {
        return {};
    }

    struct Arc {
        std::size_t target;
        std::size_t edge_index;
    };
    std::vector<std::vector<Arc>> adjacency(event_count);
    for (std::size_t edge_index = 0; edge_index < edges.size(); ++edge_index) {
        const CausalEdge& edge = edges[edge_index];
        if (edge.from < event_count && edge.to < event_count) {
            adjacency[edge.from].push_back(Arc{edge.to, edge_index});
        }
    }

    // Unweighted shortest path: edge count.
    const std::size_t unreachable = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> distance(event_count, unreachable);
    std::queue<std::size_t> queue;
    distance[from] = 0;
    queue.push(from);
    while (!queue.empty()) {
        const std::size_t current = queue.front();
        queue.pop();
        for (const Arc& arc : adjacency[current]) {
            if (distance[arc.target] == unreachable) {
                distance[arc.target] = distance[current] + 1;
                queue.push(arc.target);
            }
        }
    }

    // No strict happens-before relation evidenced by the graph.
    if (distance[to] == unreachable) {
        return {};
    }

    // Mark vertices that can still complete a shortest path to `to` using
    // only BFS-layer-advancing edges. Such edges form a DAG, which makes the
    // lexicographic tie-break a deterministic greedy choice.
    std::vector<char> on_shortest_path_to_target(event_count, false);
    on_shortest_path_to_target[to] = true;

    std::vector<std::size_t> descending;
    descending.reserve(event_count);
    for (std::size_t event = 0; event < event_count; ++event) {
        if (distance[event] != unreachable && event != to) {
            descending.push_back(event);
        }
    }
    std::sort(descending.begin(), descending.end(),
              [&](std::size_t lhs, std::size_t rhs) {
                  return distance[lhs] > distance[rhs];
              });

    for (const std::size_t event : descending) {
        for (const Arc& arc : adjacency[event]) {
            if (distance[arc.target] == distance[event] + 1 &&
                on_shortest_path_to_target[arc.target]) {
                on_shortest_path_to_target[event] = true;
                break;
            }
        }
    }

    std::vector<std::size_t> result;
    std::size_t current = from;
    while (current != to) {
        // Among feasible next events pick the smallest event id, which
        // minimizes the event-id sequence lexicographically. Parallel edges
        // to the same event are resolved by the smallest edge index so the
        // returned edge-index sequence stays stable as well.
        std::size_t best_event = unreachable;
        std::size_t best_edge = unreachable;
        for (const Arc& arc : adjacency[current]) {
            if (distance[arc.target] != distance[current] + 1 ||
                !on_shortest_path_to_target[arc.target]) {
                continue;
            }
            if (arc.target < best_event ||
                (arc.target == best_event && arc.edge_index < best_edge)) {
                best_event = arc.target;
                best_edge = arc.edge_index;
            }
        }
        // `to` has finite distance, so a feasible arc always exists.
        result.push_back(best_edge);
        current = best_event;
    }
    return result;
}

} // namespace model
