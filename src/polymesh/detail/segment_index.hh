#pragma once

#include <vector>

// A small, reusable spatial index for finite ray/line segments.
//
// A finite_segment is a point origin plus a direction and a maximum parameter
// max_t. The indexed shape is the set { origin + t * direction | 0 <= t <= max_t }.
// Directions do not have to be normalized.
//
// The index stores an AABB per segment in a bounding volume hierarchy and can
// answer "which segments' bounds overlap this AABB" queries. The result is
// conservative: a segment whose bounds overlap the query region is always
// reported, even if the exact segment does not touch the region.
//
// The index is fully static after build() but can be rebuilt cheaply from a
// new set of segments. It is used by the CPU normal baker to find the texels
// whose projection segments can be affected by a moved high-poly face.

namespace polymesh
{
namespace detail
{
struct segment_aabb
{
    double min[3] = {0.0, 0.0, 0.0};
    double max[3] = {0.0, 0.0, 0.0};
};

struct finite_segment
{
    double origin[3] = {0.0, 0.0, 0.0};
    double direction[3] = {0.0, 0.0, 0.0};
    double max_t = 0.0;
    /// arbitrary caller identity, returned by queries
    int id = -1;
};

/// AABB of the finite segment { origin + t * direction | 0 <= t <= max_t }
segment_aabb segment_bounds(finite_segment const& s);

/// true iff the two closed AABBs overlap
bool segment_aabb_intersects(segment_aabb const& a, segment_aabb const& b);

class segment_index
{
public:
    struct node
    {
        segment_aabb bounds;
        int parent = -1;
        int left = -1;
        int right = -1;
        /// leaf: index into leaf_order(), count > 0
        int first = 0;
        int count = 0;

        bool is_leaf() const { return count > 0; }
    };

public:
    segment_index() = default;
    explicit segment_index(std::vector<finite_segment> const& segments) { build(segments); }

    void build(std::vector<finite_segment> const& segments);
    void clear();

    bool empty() const { return mSegments.empty(); }
    int size() const { return int(mSegments.size()); }

    /// Appends the ids of all segments whose bounds overlap the given region.
    /// The same id can be appended more than once if the index contains
    /// duplicated segments, but a single build appends each stored segment at
    /// most once per query.
    void query(segment_aabb const& region, std::vector<int>& out_ids) const;

    std::vector<finite_segment> const& segments() const { return mSegments; }
    std::vector<node> const& nodes() const { return mNodes; }
    /// indices into segments(), in leaf traversal order
    std::vector<int> const& leaf_order() const { return mOrder; }

private:
    int build_recursive(int begin, int end, int parent);

private:
    std::vector<finite_segment> mSegments;
    std::vector<segment_aabb> mSegmentBounds;
    std::vector<node> mNodes;
    std::vector<int> mOrder;
};
} // namespace detail
} // namespace polymesh
