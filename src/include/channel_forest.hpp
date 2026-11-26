#ifndef _CHANNEL_FOREST_H_
#define _CHANNEL_FOREST_H_

#include <cstdint>
#include <vector>

namespace frame {

// parent[c] is either the parent channel or -1 when channel c is a root.
struct ChannelForestSelection {
    std::vector<int> parent;
    uint64_t totalCost;
};

// Exact minimum-cost parent selection. Costs are indexed as
// parentCosts[child][parent]. For equal totals, the parent-code vector is
// compared lexicographically, with a root encoded as the channel count.
ChannelForestSelection selectOptimalChannelForest(
    const std::vector<uint64_t>& rootCosts,
    const std::vector<std::vector<uint64_t> >& parentCosts);

// Root is represented by channelCount, matching the tie-breaking rule.
std::vector<int> channelForestTieCodes(const std::vector<int>& parent, size_t channelCount);

// Returns false when a child points to itself or when parent links contain a
// directed cycle. Out-of-range parents are also invalid.
bool isAcyclicChannelForest(const std::vector<int>& parent, size_t channelCount);

}

#endif
