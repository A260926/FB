#include "segment_index.hh"

#include <algorithm>

namespace polymesh::detail
{
segment_aabb segment_bounds(finite_segment const& s)
{
    segment_aabb b;
    for (auto i = 0; i < 3; ++i)
    {
        auto const a = s.origin[i];
        auto const c = s.origin[i] + s.direction[i] * s.max_t;
        b.min[i] = a < c ? a : c;
        b.max[i] = a < c ? c : a;
    }
    return b;
}

bool segment_aabb_intersects(segment_aabb const& a, segment_aabb const& b)
{
    for (auto i = 0; i < 3; ++i)
        if (a.max[i] < b.min[i] || b.max[i] < a.min[i])
            return false;
    return true;
}

void segment_index::clear()
{
    mSegments.clear();
    mSegmentBounds.clear();
    mNodes.clear();
    mOrder.clear();
}

void segment_index::build(std::vector<finite_segment> const& segments)
{
    clear();
    mSegments = segments;
    mSegmentBounds.resize(mSegments.size());
    for (auto i = 0u; i < mSegments.size(); ++i)
        mSegmentBounds[i] = segment_bounds(mSegments[i]);

    if (mSegments.empty())
        return;

    mOrder.resize(mSegments.size());
    for (auto i = 0u; i < mOrder.size(); ++i)
        mOrder[i] = int(i);

    mNodes.reserve(mSegments.size() * 2);
    build_recursive(0, int(mSegments.size()), -1);
}

int segment_index::build_recursive(int begin, int end, int parent)
{
    auto const idx = int(mNodes.size());
    mNodes.push_back(node{});
    mNodes[idx].parent = parent;

    segment_aabb bounds = mSegmentBounds[mOrder[begin]];
    for (auto i = begin + 1; i < end; ++i)
    {
        auto const& b = mSegmentBounds[mOrder[i]];
        for (auto k = 0; k < 3; ++k)
        {
            bounds.min[k] = std::min(bounds.min[k], b.min[k]);
            bounds.max[k] = std::max(bounds.max[k], b.max[k]);
        }
    }
    mNodes[idx].bounds = bounds;

    auto const count = end - begin;
    if (count <= 2)
    {
        mNodes[idx].first = begin;
        mNodes[idx].count = count;
        return idx;
    }

    // split along the axis with the largest extent, at the median
    auto axis = 0;
    auto extent = bounds.max[0] - bounds.min[0];
    for (auto k = 1; k < 3; ++k)
        if (bounds.max[k] - bounds.min[k] > extent)
        {
            axis = k;
            extent = bounds.max[k] - bounds.min[k];
        }

    auto const mid = begin + count / 2;
    std::nth_element(mOrder.begin() + begin, mOrder.begin() + mid, mOrder.begin() + end, [&](int a, int b) {
        auto const ca = 0.5 * (mSegmentBounds[a].min[axis] + mSegmentBounds[a].max[axis]);
        auto const cb = 0.5 * (mSegmentBounds[b].min[axis] + mSegmentBounds[b].max[axis]);
        return ca < cb;
    });

    auto const left = build_recursive(begin, mid, idx);
    auto const right = build_recursive(mid, end, idx);
    mNodes[idx].left = left;
    mNodes[idx].right = right;
    return idx;
}

void segment_index::query(segment_aabb const& region, std::vector<int>& out_ids) const
{
    if (mNodes.empty())
        return;

    // iterative traversal; the stack can grow to O(depth) but is bounded by the
    // node count, so use a scratch vector to stay correct for any input size
    std::vector<int> stack;
    stack.push_back(0);

    while (!stack.empty())
    {
        auto const n = stack.back();
        stack.pop_back();
        auto const& nd = mNodes[n];
        if (!segment_aabb_intersects(nd.bounds, region))
            continue;

        if (nd.is_leaf())
        {
            for (auto i = 0; i < nd.count; ++i)
            {
                auto const s = mOrder[nd.first + i];
                if (segment_aabb_intersects(mSegmentBounds[s], region))
                    out_ids.push_back(mSegments[s].id);
            }
        }
        else
        {
            stack.push_back(nd.left);
            stack.push_back(nd.right);
        }
    }
}

} // namespace polymesh::detail
