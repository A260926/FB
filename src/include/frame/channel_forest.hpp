#ifndef _CHANNEL_FOREST_H_
#define _CHANNEL_FOREST_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace frame {
// Selects the minimum cost rooted channel forest for one frame.
//
// costs[child][parent] holds the real subframe byte cost of encoding
// `child` differentially against `parent` (difference = parent - child),
// while costs[child][channelCount] is the cost of an independent root
// subframe. Every channel picks exactly one incoming edge: either the
// virtual root (an independent subframe) or another channel as its
// parent, and the resulting directed graph must be acyclic.
//
// The returned vector has one entry per channel: a value in
// [0, channelCount - 1] is the selected parent channel, while
// channelCount itself marks an independent root.
//
// Ties on total byte cost are broken by the lexicographically smallest
// parent vector compared from channel 0 upwards (root encoded as
// channelCount), as required by the channel forest specification.
std::vector<uint8_t> selectChannelForest(const std::vector<std::vector<size_t> >& costs);
}

#endif
