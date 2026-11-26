#include "../include/channel_forest.hpp"

#include <limits>

namespace frame {

std::vector<int> channelForestTieCodes(const std::vector<int>& parent, size_t channelCount)
{
    std::vector<int> codes;
    codes.reserve(channelCount);
    for (size_t c = 0; c < channelCount; ++c) {
        codes.push_back(parent[c] < 0 ? static_cast<int>(channelCount) : parent[c]);
    }
    return codes;
}

bool isAcyclicChannelForest(const std::vector<int>& parent, size_t channelCount)
{
    if (parent.size() != channelCount) {
        return false;
    }

    // 0 = unvisited, 1 = on the current walk, 2 = completely checked.
    std::vector<char> state(channelCount, 0);
    for (size_t start = 0; start < channelCount; ++start) {
        std::vector<size_t> path;
        size_t current = start;

        while (parent[current] >= 0) {
            const size_t parentChannel = static_cast<size_t>(parent[current]);
            if (parentChannel >= channelCount) {
                return false;
            }
            if (state[current] == 2) {
                break;
            }
            if (state[current] == 1) {
                return false;
            }

            state[current] = 1;
            path.push_back(current);
            current = parentChannel;

            if (state[current] == 1) {
                return false;
            }
        }

        for (size_t node : path) {
            state[node] = 2;
        }
    }

    return true;
}

namespace {

class ExhaustiveSelector {
private:
    const std::vector<uint64_t>& rootCosts;
    const std::vector<std::vector<uint64_t> >& parentCosts;
    size_t channelCount;
    std::vector<int> currentCodes;
    std::vector<int> bestCodes;
    uint64_t bestCost;

    bool currentCodesAcyclic() const
    {
        for (size_t start = 0; start < channelCount; ++start) {
            size_t current = start;
            size_t steps = 0;

            while (true) {
                const int code = currentCodes[current];
                if (code == static_cast<int>(channelCount)) {
                    break;
                }
                if (code < 0 || static_cast<size_t>(code) >= channelCount
                    || static_cast<size_t>(code) == current) {
                    return false;
                }

                current = static_cast<size_t>(code);
                ++steps;

                // An acyclic parent path has at most channelCount - 1 edges.
                if (steps >= channelCount) {
                    return false;
                }
            }
        }
        return true;
    }

    bool currentTieIsSmaller() const
    {
        for (size_t c = 0; c < channelCount; ++c) {
            if (currentCodes[c] != bestCodes[c]) {
                return currentCodes[c] < bestCodes[c];
            }
        }
        return false;
    }

    void evaluateLeaf()
    {
        if (!currentCodesAcyclic()) {
            return;
        }

        uint64_t totalCost = 0;
        for (size_t c = 0; c < channelCount; ++c) {
            if (currentCodes[c] == static_cast<int>(channelCount)) {
                totalCost += rootCosts[c];
            } else {
                totalCost += parentCosts[c][currentCodes[c]];
            }
        }

        if (bestCodes.empty() || totalCost < bestCost
            || (totalCost == bestCost && currentTieIsSmaller())) {
            bestCost = totalCost;
            bestCodes = currentCodes;
        }
    }

    void enumerateChannel(size_t channel)
    {
        if (channel == channelCount) {
            evaluateLeaf();
            return;
        }

        // Actual tie-break codes are in ascending order: parent channel ids
        // except self, followed by root == channelCount.
        for (int code = 0; code <= static_cast<int>(channelCount); ++code) {
            if (code == static_cast<int>(channel)) {
                continue;
            }
            currentCodes[channel] = code;
            enumerateChannel(channel + 1);
        }
    }

public:
    ExhaustiveSelector(const std::vector<uint64_t>& roots,
        const std::vector<std::vector<uint64_t> >& parents)
        : rootCosts(roots)
        , parentCosts(parents)
        , channelCount(roots.size())
        , currentCodes(channelCount, 0)
        , bestCost(std::numeric_limits<uint64_t>::max())
    {
    }

    ChannelForestSelection run()
    {
        // The caller's supported range is 1..8.  Zero channels is still a
        // well-defined empty selection for container-level tests.
        if (channelCount == 0) {
            return ChannelForestSelection();
        }

        enumerateChannel(0);

        ChannelForestSelection result;
        result.totalCost = bestCost;
        result.parent.resize(channelCount, -1);
        for (size_t c = 0; c < channelCount; ++c) {
            if (bestCodes[c] != static_cast<int>(channelCount)) {
                result.parent[c] = bestCodes[c];
            }
        }
        return result;
    }
};

}

ChannelForestSelection selectOptimalChannelForest(
    const std::vector<uint64_t>& rootCosts,
    const std::vector<std::vector<uint64_t> >& parentCosts)
{
    return ExhaustiveSelector(rootCosts, parentCosts).run();
}

}
