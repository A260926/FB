#include "../include/frame/channel_forest.hpp"

#include "../include/data/exception.hpp"

#include <cstdint>
#include <vector>

namespace frame {
namespace {
struct Weight {
    int64_t base;
    int64_t tie;
};

struct Edge {
    int from;
    int to;
    Weight weight;
    // Stable identity of the edge in the top-level graph; survives the
    // subsetting and renumbering that each contraction performs.
    size_t origin;
};

bool weightLess(const Weight& left, const Weight& right)
{
    if (left.base != right.base) {
        return left.base < right.base;
    }
    return left.tie < right.tie;
}

Weight weightSubtract(const Weight& left, const Weight& right)
{
    return Weight{ left.base - right.base, left.tie - right.tie };
}

size_t pickIncoming(const std::vector<Edge>& edges, int node)
{
    size_t best = 0;
    bool found = false;
    for (size_t i = 0; i < edges.size(); i++) {
        if (edges[i].to != node) {
            continue;
        }
        if (!found || weightLess(edges[i].weight, edges[best].weight)) {
            best = i;
            found = true;
        }
    }
    return best;
}

// Chu-Liu/Edmonds minimum arborescence. The virtual root has no incoming
// edge, so the selected arborescence is exactly an acyclic channel forest
// where every channel keeps one incoming edge.
std::vector<Edge> edmonds(int nodeCount, int root, const std::vector<Edge>& edges)
{
    // chosenOrigin stores LOCAL indices into edges; edge.origin is only
    // meaningful when expanding a contracted solution back to the caller.
    std::vector<int> chosenOrigin(nodeCount, -1);
    for (int node = 0; node < nodeCount; node++) {
        if (node == root) {
            continue;
        }
        chosenOrigin[node] = (int)pickIncoming(edges, node);
    }

    // Walk the chosen incoming edges to find a directed cycle.
    std::vector<int> cycle;
    bool hasCycle = false;
    for (int start = 0; start < nodeCount && !hasCycle; start++) {
        std::vector<int> position(nodeCount, -1);
        std::vector<int> path;
        int node = start;
        while (node != -1 && position[node] == -1) {
            position[node] = (int)path.size();
            path.push_back(node);
            node = (node == root) ? -1 : edges[chosenOrigin[node]].from;
        }
        if (node != -1 && node != root) {
            for (size_t i = position[node]; i < path.size(); i++) {
                cycle.push_back(path[i]);
            }
            hasCycle = true;
        }
    }

    if (!hasCycle) {
        std::vector<Edge> chosen;
        for (int node = 0; node < nodeCount; node++) {
            if (node != root) {
                chosen.push_back(edges[chosenOrigin[node]]);
            }
        }
        return chosen;
    }

    std::vector<bool> inCycle(nodeCount, false);
    for (int node : cycle) {
        inCycle[node] = true;
    }

    // Contract the cycle into one node and renumber everything else.
    std::vector<int> newId(nodeCount, -1);
    int contractedNode = 0;
    for (int node = 0; node < nodeCount; node++) {
        if (!inCycle[node]) {
            newId[node] = contractedNode++;
        }
    }
    int cycleNode = contractedNode;
    for (int node : cycle) {
        newId[node] = cycleNode;
    }
    int contractedRoot = newId[root];
    int contractedCount = contractedNode + 1;

    std::vector<Edge> contractedEdges;
    contractedEdges.reserve(edges.size());
    for (const Edge& edge : edges) {
        bool fromCycle = inCycle[edge.from];
        bool toCycle = inCycle[edge.to];
        if (fromCycle && toCycle) {
            continue;
        }
        Edge next = edge;
        if (toCycle) {
            // Entering the cycle: the displaced cycle edge would no longer
            // be paid for, so subtract its weight.
            next.weight = weightSubtract(edge.weight, edges[chosenOrigin[edge.to]].weight);
        }
        next.from = newId[edge.from];
        next.to = newId[edge.to];
        contractedEdges.push_back(next);
    }

    std::vector<Edge> contractedChosen = edmonds(contractedCount, contractedRoot, contractedEdges);

    // Map stable edge identities back onto this level's local edge indices;
    // nested contractions make raw indices unusable across recursion levels.
    // Origins are top-level ids and can exceed the current level's edge
    // count, so size the map from the largest id actually present.
    size_t maxOrigin = 0;
    for (const Edge& edge : edges) {
        if (edge.origin > maxOrigin) {
            maxOrigin = edge.origin;
        }
    }
    std::vector<int> localByOrigin(maxOrigin + 1, -1);
    for (size_t i = 0; i < edges.size(); i++) {
        localByOrigin[edges[i].origin] = (int)i;
    }

    // Expand the contracted solution back onto the original graph.
    std::vector<bool> selectedLocal(edges.size(), false);
    int displaced = -1;
    for (const Edge& edge : contractedChosen) {
        int local = localByOrigin[edge.origin];
        selectedLocal[local] = true;
        // The arborescence has exactly one edge entering the contracted
        // node; its current-level target is the displaced cycle node.
        if (edge.to == cycleNode) {
            displaced = edges[local].to;
        }
    }
    for (int node : cycle) {
        if (node != displaced) {
            selectedLocal[chosenOrigin[node]] = true;
        }
    }

    std::vector<Edge> chosen;
    for (size_t local = 0; local < edges.size(); local++) {
        if (selectedLocal[local]) {
            chosen.push_back(edges[local]);
        }
    }
    return chosen;
}
} // namespace

std::vector<uint8_t> selectChannelForest(const std::vector<std::vector<size_t> >& costs)
{
    const size_t channelCount = costs.size();
    if (channelCount < 1 || channelCount > 8) {
        const std::string exceptionMessage = "Channel forest supports 1 to 8 channels per frame.";
        throw data::Exception(std::move(exceptionMessage));
    }

    const size_t root = channelCount;
    const size_t scale = channelCount + 1;
    std::vector<int64_t> power(channelCount);
    power[0] = 1;
    for (size_t i = 1; i < channelCount; i++) {
        power[i] = power[i - 1] * (int64_t)scale;
    }

    // One edge per legal (parent, child) pair. Parent ids are packed into
    // the tie term, so equal-cost forests order exactly like the required
    // lexicographic comparison of parent vectors (root recorded as N).
    std::vector<Edge> edges;
    edges.reserve(channelCount * channelCount);
    for (size_t child = 0; child < channelCount; child++) {
        if (costs[child].size() != root + 1) {
            const std::string exceptionMessage = "Channel forest cost matrix has the wrong shape.";
            throw data::Exception(std::move(exceptionMessage));
        }
        for (size_t parent = 0; parent <= root; parent++) {
            if (parent == child) {
                continue;
            }
            int64_t tie = (int64_t)parent * power[channelCount - 1 - child];
            edges.push_back(Edge{ (int)parent, (int)child, Weight{ (int64_t)costs[child][parent], tie }, edges.size() });
        }
    }

    std::vector<Edge> chosen = edmonds((int)(channelCount + 1), (int)root, edges);

    std::vector<uint8_t> parents(channelCount, (uint8_t)root);
    for (const Edge& edge : chosen) {
        parents[edge.to] = (uint8_t)edge.from;
    }
    return parents;
}
}
