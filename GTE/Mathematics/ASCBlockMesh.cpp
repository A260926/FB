// David Eberly, Geometric Tools, Redmond WA 98052
// Copyright (c) 1998-2026
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt
// https://www.geometrictools.com/License/Boost/LICENSE_1_0.txt
// File Version: 8.0.2026.11.16

#include <Mathematics/ASCBlockMesh.h>
#include <Mathematics/AdaptiveSkeletonClimbing3.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace gte
{
    namespace
    {
        using Extractor = AdaptiveSkeletonClimbing3<int32_t, double>;

        // A rectangle on a face, in face-local coordinates.  'u' and 'v' are
        // the two tangent axes of the face, in increasing axis order.
        struct Rect
        {
            int32_t u0, v0, u1, v1;
        };

        bool operator<(Rect const& r0, Rect const& r1)
        {
            return std::tie(r0.u0, r0.v0, r0.u1, r0.v1) <
                std::tie(r1.u0, r1.v0, r1.u1, r1.v1);
        }

        // A rectilinear partition of a face.  'map' stores, for every unit
        // cell of the S-by-S face, the index of the cell containing it; it
        // is canonical, so two partitions are equal exactly when their maps
        // are equal.  An empty 'cells' vector means "unconstrained".
        struct Partition2
        {
            int32_t size{ 0 };
            std::vector<Rect> cells;
            std::vector<int32_t> map;

            void Build(std::vector<Rect> input)
            {
                std::sort(input.begin(), input.end());
                cells = std::move(input);
                map.assign(static_cast<size_t>(size) * static_cast<size_t>(size),
                    -1);
                for (size_t i = 0; i < cells.size(); ++i)
                {
                    Rect const& r = cells[i];
                    for (int32_t u = r.u0; u < r.u1; ++u)
                    {
                        for (int32_t v = r.v0; v < r.v1; ++v)
                        {
                            map[static_cast<size_t>(u) * size + v] =
                                static_cast<int32_t>(i);
                        }
                    }
                }
            }

            bool operator==(Partition2 const& other) const
            {
                return size == other.size && map == other.map;
            }

            bool operator!=(Partition2 const& other) const
            {
                return !(*this == other);
            }

            // True when the rectangle is contained in one cell.
            bool Contains(int32_t u0, int32_t v0, int32_t u1, int32_t v1) const
            {
                if (u0 >= u1 || v0 >= v1 || cells.empty())
                {
                    return true;
                }
                if (u0 < 0 || v0 < 0 || u1 > size || v1 > size)
                {
                    return false;
                }
                int32_t id = map[static_cast<size_t>(u0) * size + v0];
                if (id < 0)
                {
                    return false;
                }
                Rect const& r = cells[id];
                return r.u0 <= u0 && r.v0 <= v0 && r.u1 >= u1 && r.v1 >= v1;
            }
        };

        Partition2 CommonRefinement(Partition2 const& p0, Partition2 const& p1)
        {
            std::vector<Rect> cells;
            cells.reserve(p0.cells.size() + p1.cells.size());
            for (Rect const& r0 : p0.cells)
            {
                for (Rect const& r1 : p1.cells)
                {
                    int32_t u0 = std::max(r0.u0, r1.u0);
                    int32_t v0 = std::max(r0.v0, r1.v0);
                    int32_t u1 = std::min(r0.u1, r1.u1);
                    int32_t v1 = std::min(r0.v1, r1.v1);
                    if (u0 < u1 && v0 < v1)
                    {
                        cells.push_back(Rect{ u0, v0, u1, v1 });
                    }
                }
            }
            Partition2 result;
            result.size = p0.size;
            result.Build(std::move(cells));
            return result;
        }

        // The two tangent axes of a face, in increasing axis order.
        void FaceTangents(int32_t axis, int32_t& tu, int32_t& tv)
        {
            tu = (axis + 1) % 3;
            tv = (axis + 2) % 3;
            if (tu > tv)
            {
                std::swap(tu, tv);
            }
        }

        int32_t Low(Extractor::Box const& b, int32_t axis)
        {
            return (axis == 0) ? b.x0 : (axis == 1) ? b.y0 : b.z0;
        }

        int32_t High(Extractor::Box const& b, int32_t axis)
        {
            return (axis == 0) ? b.x1 : (axis == 1) ? b.y1 : b.z1;
        }

        // A key identifying a shared face: the normal axis, the world plane
        // of the face, and the world origins along the two tangent axes.
        struct FaceKey
        {
            int32_t axis{ 0 };
            int64_t plane{ 0 };
            int64_t originU{ 0 };
            int64_t originV{ 0 };

            bool operator<(FaceKey const& other) const
            {
                return std::tie(axis, plane, originU, originV) <
                    std::tie(other.axis, other.plane, other.originU,
                        other.originV);
            }

            bool operator==(FaceKey const& other) const
            {
                return std::tie(axis, plane, originU, originV) ==
                    std::tie(other.axis, other.plane, other.originU,
                        other.originV);
            }
        };

        // A key identifying a shared edge: the direction of the edge, the
        // world coordinate where the edge begins along it, and the world
        // planes of the two fixed axes, in increasing axis order.
        struct EdgeKey
        {
            int32_t axis{ 0 };
            int64_t start{ 0 };
            int64_t fixed0{ 0 };
            int64_t fixed1{ 0 };

            bool operator<(EdgeKey const& other) const
            {
                return std::tie(axis, start, fixed0, fixed1) <
                    std::tie(other.axis, other.start, other.fixed0,
                        other.fixed1);
            }

            bool operator==(EdgeKey const& other) const
            {
                return std::tie(axis, start, fixed0, fixed1) ==
                    std::tie(other.axis, other.start, other.fixed0,
                        other.fixed1);
            }
        };

        EdgeKey MakeEdgeKey(int32_t axis, int64_t start,
            std::array<int64_t, 3> const& fixed)
        {
            int32_t fa, fb;
            FaceTangents(axis, fa, fb);
            return EdgeKey{ axis, start, fixed[fa], fixed[fb] };
        }

        // The 2D partition induced by the boxes on one outer face of a
        // block.  'dir' is 2*axis + side, side 0 for the minimum plane.
        Partition2 FacePartition(std::vector<Extractor::Box> const& boxes,
            int32_t size, int32_t dir)
        {
            int32_t axis = dir / 2;
            int32_t side = dir % 2;
            int32_t tu, tv;
            FaceTangents(axis, tu, tv);

            std::vector<Rect> cells;
            for (Extractor::Box const& b : boxes)
            {
                bool touches = (side == 0) ? (Low(b, axis) == 0) :
                    (High(b, axis) == size);
                if (!touches)
                {
                    continue;
                }
                cells.push_back(Rect{ Low(b, tu), Low(b, tv),
                    High(b, tu), High(b, tv) });
            }

            Partition2 result;
            result.size = size;
            result.Build(std::move(cells));
            return result;
        }

        // Refine a partition along one of its four sides.  'side' is 0 for
        // u-min, 1 for u-max, 2 for v-min, 3 for v-max.  'cuts' are in
        // face-local coordinates of the coordinate running along the side.
        Partition2 RefineSide(Partition2 const& partition, int32_t side,
            std::vector<int32_t> const& cuts)
        {
            if (cuts.empty())
            {
                return partition;
            }
            std::vector<Rect> cells;
            cells.reserve(partition.cells.size() + cuts.size());
            for (Rect const& r : partition.cells)
            {
                bool touches = (side == 0) ? (r.u0 == 0) :
                    (side == 1) ? (r.u1 == partition.size) :
                    (side == 2) ? (r.v0 == 0) : (r.v1 == partition.size);
                if (!touches)
                {
                    cells.push_back(r);
                    continue;
                }

                bool splitAlongV = (side < 2);
                int32_t lo = splitAlongV ? r.v0 : r.u0;
                int32_t hi = splitAlongV ? r.v1 : r.u1;
                std::vector<int32_t> points{ lo };
                for (int32_t c : cuts)
                {
                    if (c > lo && c < hi)
                    {
                        points.push_back(c);
                    }
                }
                points.push_back(hi);
                std::sort(points.begin(), points.end());
                points.erase(std::unique(points.begin(), points.end()),
                    points.end());
                for (size_t i = 0; i + 1 < points.size(); ++i)
                {
                    if (splitAlongV)
                    {
                        cells.push_back(
                            Rect{ r.u0, points[i], r.u1, points[i + 1] });
                    }
                    else
                    {
                        cells.push_back(
                            Rect{ points[i], r.v0, points[i + 1], r.v1 });
                    }
                }
            }
            Partition2 result;
            result.size = partition.size;
            result.Build(std::move(cells));
            return result;
        }

        // The cuts that a partition imposes on one of its sides, in
        // face-local coordinates along that side.
        std::vector<int32_t> SideCuts(Partition2 const& partition, int32_t side)
        {
            std::set<int32_t> cuts;
            for (Rect const& r : partition.cells)
            {
                bool touches = (side == 0) ? (r.u0 == 0) :
                    (side == 1) ? (r.u1 == partition.size) :
                    (side == 2) ? (r.v0 == 0) : (r.v1 == partition.size);
                if (touches)
                {
                    cuts.insert((side < 2) ? r.v0 : r.u0);
                    cuts.insert((side < 2) ? r.v1 : r.u1);
                }
            }
            return std::vector<int32_t>(cuts.begin(), cuts.end());
        }

        // The edge key of one side of a face.
        EdgeKey FaceSideEdge(FaceKey const& face, int32_t side, int32_t size)
        {
            int32_t tu, tv;
            FaceTangents(face.axis, tu, tv);
            std::array<int64_t, 3> fixed = { 0, 0, 0 };
            fixed[face.axis] = face.plane;
            int32_t edgeAxis;
            int64_t start;
            if (side == 0)
            {
                edgeAxis = tv;
                fixed[tu] = face.originU;
                start = face.originV;
            }
            else if (side == 1)
            {
                edgeAxis = tv;
                fixed[tu] = face.originU + size;
                start = face.originV;
            }
            else if (side == 2)
            {
                edgeAxis = tu;
                fixed[tv] = face.originV;
                start = face.originU;
            }
            else
            {
                edgeAxis = tu;
                fixed[tv] = face.originV + size;
                start = face.originU;
            }
            return MakeEdgeKey(edgeAxis, start, fixed);
        }

        // The local cuts on one edge of a block, from its box partition,
        // in block-local coordinates along the edge direction.
        std::vector<int32_t> BlockEdgeCuts(
            std::vector<Extractor::Box> const& boxes, int32_t size,
            std::array<int64_t, 3> const& origin, int32_t axis,
            std::array<int64_t, 3> const& fixedPlane)
        {
            int32_t fa, fb;
            FaceTangents(axis, fa, fb);
            int32_t localA = static_cast<int32_t>(fixedPlane[fa] - origin[fa]);
            int32_t localB = static_cast<int32_t>(fixedPlane[fb] - origin[fb]);
            if ((localA != 0 && localA != size) ||
                (localB != 0 && localB != size))
            {
                return {};
            }

            std::set<int32_t> cuts{ 0, size };
            for (Extractor::Box const& b : boxes)
            {
                bool onA = (localA == 0) ? (Low(b, fa) == 0) :
                    (High(b, fa) == size);
                bool onB = (localB == 0) ? (Low(b, fb) == 0) :
                    (High(b, fb) == size);
                if (onA && onB)
                {
                    cuts.insert(Low(b, axis));
                    cuts.insert(High(b, axis));
                }
            }
            return std::vector<int32_t>(cuts.begin(), cuts.end());
        }

        // Constraint passed into the production extractor.  A candidate box
        // is rejected when a footprint on a shared face crosses a negotiated
        // cell boundary, or when a box touching a shared edge crosses a
        // negotiated edge cut.  Boxes that touch no shared feature are never
        // rejected, so the interior partition of a block is unchanged and an
        // exposed face is not refined just because it has no neighbour.
        class BlockConstraint : public Extractor::MergeConstraint
        {
        public:
            BlockConstraint(int32_t size,
                std::array<Partition2 const*, 6> const& faces,
                std::array<std::vector<int32_t> const*, 12> const& edges)
                :
                mSize(size),
                mFaces(faces),
                mEdges(edges)
            {
            }

            bool AllowBox(int32_t x0, int32_t y0, int32_t z0,
                int32_t x1, int32_t y1, int32_t z1) const override
            {
                int32_t lo[3] = { x0, y0, z0 };
                int32_t hi[3] = { x1, y1, z1 };

                // A box that touches a shared face must lie in the unit slab
                // normal to that face and inside one negotiated cell of the
                // face.  The unit slab makes the crossing on the
                // perpendicular box edges a function of the plane samples
                // alone, so the two sides choose the same seam vertex; the
                // tangential extent stays adaptive at the negotiated cell
                // size.
                for (int32_t axis = 0; axis < 3; ++axis)
                {
                    for (int32_t side = 0; side < 2; ++side)
                    {
                        int32_t dir = axis * 2 + side;
                        if (mFaces[dir] == nullptr)
                        {
                            continue;
                        }
                        bool touches = (side == 0) ? (lo[axis] == 0) :
                            (hi[axis] == mSize);
                        if (!touches)
                        {
                            continue;
                        }
                        if (hi[axis] - lo[axis] != 1)
                        {
                            return false;
                        }
                        int32_t tu, tv;
                        FaceTangents(axis, tu, tv);
                        if (!mFaces[dir]->Contains(lo[tu], lo[tv], hi[tu],
                            hi[tv]))
                        {
                            return false;
                        }
                    }
                }

                // Shared-edge constraints.  A box that touches a shared edge
                // (two unit-slab faces meeting) must not cross a negotiated
                // edge cut.
                for (int32_t axis = 0; axis < 3; ++axis)
                {
                    int32_t fa, fb;
                    FaceTangents(axis, fa, fb);
                    for (int32_t sideA = 0; sideA < 2; ++sideA)
                    {
                        if ((sideA == 0) ? (lo[fa] != 0) : (hi[fa] != mSize))
                        {
                            continue;
                        }
                        for (int32_t sideB = 0; sideB < 2; ++sideB)
                        {
                            if ((sideB == 0) ? (lo[fb] != 0) : (hi[fb] != mSize))
                            {
                                continue;
                            }
                            int32_t slot = axis * 4 + sideA * 2 + sideB;
                            std::vector<int32_t> const* cuts = mEdges[slot];
                            if (cuts != nullptr &&
                                !AllowedSpan(lo[axis], hi[axis], *cuts))
                            {
                                return false;
                            }
                        }
                    }
                }
                return true;
            }

        private:
            static bool AllowedSpan(int32_t lo, int32_t hi,
                std::vector<int32_t> const& cuts)
            {
                for (int32_t c : cuts)
                {
                    if (lo < c && c < hi)
                    {
                        return false;
                    }
                }
                return true;
            }

            int32_t mSize;
            std::array<Partition2 const*, 6> mFaces;
            std::array<std::vector<int32_t> const*, 12> mEdges;
        };

        struct Workspace
        {
            int32_t N{ 0 };
            int32_t size{ 1 };
            std::vector<ASCBlockMeshBlock> const* blocks{ nullptr };
            std::map<std::tuple<int64_t, int64_t, int64_t>, int32_t> indexOfOrigin;
            std::map<FaceKey, std::pair<int32_t, int32_t>> sharedFaces;
            std::vector<std::array<bool, 6>> faceIsShared;
            std::vector<std::array<FaceKey, 6>> faceKeys;
            std::vector<std::array<EdgeKey, 12>> blockEdges;
            std::map<FaceKey, int32_t> firstDir, secondDir;
        };

        void BuildWorkspace(Workspace& ws)
        {
            auto const& blocks = *ws.blocks;
            int32_t S = ws.size;
            size_t numBlocks = blocks.size();
            ws.faceIsShared.assign(numBlocks, std::array<bool, 6>{});
            ws.faceKeys.resize(numBlocks);
            ws.blockEdges.resize(numBlocks);

            for (size_t i = 0; i < numBlocks; ++i)
            {
                auto const& o = blocks[i].origin;
                ws.indexOfOrigin[std::make_tuple(o[0], o[1], o[2])] =
                    static_cast<int32_t>(i);
            }

            for (size_t i = 0; i < numBlocks; ++i)
            {
                auto const& o = blocks[i].origin;
                for (int32_t dir = 0; dir < 6; ++dir)
                {
                    int32_t axis = dir / 2;
                    int32_t side = dir % 2;
                    int32_t tu, tv;
                    FaceTangents(axis, tu, tv);

                    std::array<int64_t, 3> neighbor = { o[0], o[1], o[2] };
                    neighbor[axis] += (side == 0) ? -S : S;
                    auto iter = ws.indexOfOrigin.find(
                        std::make_tuple(neighbor[0], neighbor[1], neighbor[2]));
                    bool shared = (iter != ws.indexOfOrigin.end());
                    if (shared)
                    {
                        neighbor = blocks[iter->second].origin;
                        (void)neighbor;
                    }

                    FaceKey key;
                    key.axis = axis;
                    key.plane = o[axis] + ((side == 0) ? 0 : S);
                    key.originU = o[tu];
                    key.originV = o[tv];
                    ws.faceKeys[i][dir] = key;
                    ws.faceIsShared[i][dir] = shared;
                }

                size_t next = 0;
                for (int32_t axis = 0; axis < 3; ++axis)
                {
                    int32_t fa, fb;
                    FaceTangents(axis, fa, fb);
                    for (int32_t sideA = 0; sideA < 2; ++sideA)
                    {
                        for (int32_t sideB = 0; sideB < 2; ++sideB)
                        {
                            std::array<int64_t, 3> fixed = { o[0], o[1], o[2] };
                            fixed[fa] += sideA * S;
                            fixed[fb] += sideB * S;
                            ws.blockEdges[i][next++] =
                                MakeEdgeKey(axis, o[axis], fixed);
                        }
                    }
                }
            }

            for (size_t i = 0; i < numBlocks; ++i)
            {
                for (int32_t dir = 0; dir < 6; ++dir)
                {
                    if (!ws.faceIsShared[i][dir])
                    {
                        continue;
                    }
                    FaceKey const& key = ws.faceKeys[i][dir];
                    std::array<int64_t, 3> origin = blocks[i].origin;
                    int32_t axis = dir / 2;
                    int32_t side = dir % 2;
                    origin[axis] += (side == 0) ? -S : S;
                    auto iter = ws.indexOfOrigin.find(
                        std::make_tuple(origin[0], origin[1], origin[2]));
                    int32_t j = iter->second;
                    auto pair = std::make_pair(
                        std::min(static_cast<int32_t>(i), j),
                        std::max(static_cast<int32_t>(i), j));

                    // Record the direction of this face in each of the two
                    // blocks.  The same FaceKey is seen from both sides, so
                    // the direction of the numerically smaller block must
                    // not be overwritten by the larger one.
                    int32_t opposite = axis * 2 + (1 - side);
                    if (!ws.sharedFaces.count(key))
                    {
                        ws.sharedFaces[key] = pair;
                        if (pair.first == static_cast<int32_t>(i))
                        {
                            ws.firstDir[key] = dir;
                            ws.secondDir[key] = opposite;
                        }
                        else
                        {
                            ws.firstDir[key] = opposite;
                            ws.secondDir[key] = dir;
                        }
                    }
                }
            }
        }

        // Recompute the edge cuts as the union over all blocks incident to
        // the edge.  An edge touched by only one block is exposed and is
        // left unconstrained.
        void ComputeEdgeTargets(Workspace const& ws,
            std::vector<std::vector<Extractor::Box>> const& boxes,
            std::map<EdgeKey, std::vector<int32_t>>& targets)
        {
            std::map<EdgeKey, std::set<int32_t>> merged;
            std::map<EdgeKey, std::set<int32_t>> contributors;

            for (size_t i = 0; i < boxes.size(); ++i)
            {
                auto const& origin = ws.blocks->at(i).origin;
                for (size_t e = 0; e < 12; ++e)
                {
                    EdgeKey const& key = ws.blockEdges[i][e];
                    std::array<int64_t, 3> fixed = { 0, 0, 0 };
                    int32_t fa, fb;
                    FaceTangents(key.axis, fa, fb);
                    fixed[fa] = key.fixed0;
                    fixed[fb] = key.fixed1;

                    std::vector<int32_t> cuts = BlockEdgeCuts(boxes[i],
                        ws.size, origin, key.axis, fixed);
                    if (!cuts.empty() && contributors[key].insert(
                        static_cast<int32_t>(i)).second)
                    {
                        merged[key].insert(cuts.begin(), cuts.end());
                    }
                }
            }

            targets.clear();
            for (auto const& element : merged)
            {
                if (contributors[element.first].size() >= 2)
                {
                    targets[element.first] = std::vector<int32_t>(
                        element.second.begin(), element.second.end());
                }
            }
        }

        std::array<double, 3> ToWorld(std::array<double, 3> const& local,
            std::array<int64_t, 3> const& origin)
        {
            return std::array<double, 3>{
                local[0] + static_cast<double>(origin[0]),
                local[1] + static_cast<double>(origin[1]),
                local[2] + static_cast<double>(origin[2]) };
        }

        // The coarsest common refinement of the two 2D partitions that the
        // two blocks realize on every shared face.
        void BuildFaceTargets(Workspace const& ws,
            std::vector<std::vector<Extractor::Box>> const& boxes, int32_t S,
            std::map<FaceKey, Partition2>& targets)
        {
            targets.clear();
            for (auto const& element : ws.sharedFaces)
            {
                FaceKey const& key = element.first;
                int32_t first = element.second.first;
                int32_t second = element.second.second;
                Partition2 p0 = FacePartition(boxes[first], S,
                    ws.firstDir.at(key));
                Partition2 p1 = FacePartition(boxes[second], S,
                    ws.secondDir.at(key));
                targets[key] = CommonRefinement(p0, p1);
            }
        }

        // Propagate the shared-edge cuts into the faces that border the edge
        // and then to the other sides of those faces, until neither changes.
        // Both targets only become finer, so this terminates.
        void PropagateEdges(std::map<FaceKey, Partition2>& faces,
            std::map<EdgeKey, std::vector<int32_t>>& edges, int32_t S)
        {
            for (int32_t pass = 0; pass < 4096; ++pass)
            {
                bool changed = false;

                for (auto& element : faces)
                {
                    Partition2 before = element.second;
                    FaceKey const& key = element.first;
                    for (int32_t side = 0; side < 4; ++side)
                    {
                        EdgeKey edge = FaceSideEdge(key, side, S);
                        auto iter = edges.find(edge);
                        if (iter == edges.end())
                        {
                            continue;
                        }
                        int64_t base = (side == 0 || side == 1) ?
                            key.originV : key.originU;
                        std::vector<int32_t> local;
                        local.reserve(iter->second.size());
                        for (int32_t cut : iter->second)
                        {
                            local.push_back(static_cast<int32_t>(cut - base));
                        }
                        element.second = RefineSide(element.second, side,
                            local);
                    }
                    if (element.second != before)
                    {
                        changed = true;
                    }
                }

                std::map<EdgeKey, std::set<int32_t>> added;
                for (auto const& element : faces)
                {
                    FaceKey const& key = element.first;
                    for (int32_t side = 0; side < 4; ++side)
                    {
                        EdgeKey edge = FaceSideEdge(key, side, S);
                        int64_t base = (side == 0 || side == 1) ?
                            key.originV : key.originU;
                        for (int32_t cut : SideCuts(element.second, side))
                        {
                            added[edge].insert(
                                static_cast<int32_t>(cut + base));
                        }
                    }
                }
                for (auto const& element : added)
                {
                    auto& target = edges[element.first];
                    std::set<int32_t> merged(target.begin(), target.end());
                    size_t before = merged.size();
                    merged.insert(element.second.begin(), element.second.end());
                    if (merged.size() != before)
                    {
                        target.assign(merged.begin(), merged.end());
                        changed = true;
                    }
                }

                if (!changed)
                {
                    break;
                }
            }
        }

    }  // anonymous namespace

    ASCBlockMeshOutput ComputeASCBlockMesh(int32_t N, double level,
        std::vector<ASCBlockMeshBlock> const& blocks)
    {
        if (N < 0 || N > 10)
        {
            throw std::invalid_argument("ASCBlockMesh: N must be in [0,10].");
        }
        if (!std::isfinite(level))
        {
            throw std::invalid_argument(
                "ASCBlockMesh: the level must be finite.");
        }
        if (blocks.empty())
        {
            throw std::invalid_argument(
                "ASCBlockMesh: at least one block is required.");
        }

        int64_t size64 = static_cast<int64_t>(1) << N;
        int32_t S = static_cast<int32_t>(size64);
        size_t samplesPerBlock = static_cast<size_t>(S + 1) *
            static_cast<size_t>(S + 1) * static_cast<size_t>(S + 1);

        // Validate the input.  Nothing is written to it.
        std::set<std::tuple<int64_t, int64_t, int64_t>> origins;
        for (ASCBlockMeshBlock const& block : blocks)
        {
            if (block.samples.size() != samplesPerBlock)
            {
                throw std::invalid_argument(
                    "ASCBlockMesh: a block has the wrong number of samples.");
            }
            if (block.depth < -1 || block.depth > N)
            {
                throw std::invalid_argument(
                    "ASCBlockMesh: a block depth is outside [-1,N].");
            }
            for (int32_t a = 0; a < 3; ++a)
            {
                int64_t value = block.origin[a];
                if (value % size64 != 0)
                {
                    throw std::invalid_argument(
                        "ASCBlockMesh: a block origin is not a multiple of "
                        "2^N.");
                }
                if (value > std::numeric_limits<int64_t>::max() - size64 ||
                    value < std::numeric_limits<int64_t>::min() + size64)
                {
                    throw std::invalid_argument(
                        "ASCBlockMesh: a block origin overflows.");
                }
            }
            if (!origins.insert(std::make_tuple(block.origin[0],
                block.origin[1], block.origin[2])).second)
            {
                throw std::invalid_argument(
                    "ASCBlockMesh: two blocks have the same origin.");
            }
            for (int32_t value : block.samples)
            {
                if (static_cast<double>(value) == level)
                {
                    throw std::invalid_argument(
                        "ASCBlockMesh: the level equals a sample value.");
                }
            }
        }

        // Samples on shared features must be equal.
        for (size_t i = 0; i < blocks.size(); ++i)
        {
            for (size_t j = i + 1; j < blocks.size(); ++j)
            {
                auto const& oi = blocks[i].origin;
                auto const& oj = blocks[j].origin;
                std::array<int32_t, 3> delta{};
                std::array<int32_t, 3> count{};
                bool touching = true;
                for (int32_t a = 0; a < 3; ++a)
                {
                    int64_t d = oj[a] - oi[a];
                    if (d == 0)
                    {
                        delta[a] = 0;
                        count[a] = S + 1;
                    }
                    else if (d == size64)
                    {
                        delta[a] = S;
                        count[a] = 1;
                    }
                    else if (d == -size64)
                    {
                        delta[a] = -S;
                        count[a] = 1;
                    }
                    else
                    {
                        touching = false;
                        break;
                    }
                }
                if (!touching)
                {
                    continue;
                }

                int32_t stride = S + 1;
                for (int32_t dz = 0; dz < count[2]; ++dz)
                {
                    int32_t zj = (delta[2] == S) ? 0 : (delta[2] == -S) ? S : dz;
                    int32_t zi = zj + delta[2];
                    for (int32_t dy = 0; dy < count[1]; ++dy)
                    {
                        int32_t yj = (delta[1] == S) ? 0 : (delta[1] == -S) ? S : dy;
                        int32_t yi = yj + delta[1];
                        for (int32_t dx = 0; dx < count[0]; ++dx)
                        {
                            int32_t xj = (delta[0] == S) ? 0 :
                                (delta[0] == -S) ? S : dx;
                            int32_t xi = xj + delta[0];
                            int32_t ij = xj + stride * (yj + stride * zj);
                            int32_t ii = xi + stride * (yi + stride * zi);
                            if (blocks[i].samples[ii] != blocks[j].samples[ij])
                            {
                                throw std::invalid_argument(
                                    "ASCBlockMesh: blocks disagree on a "
                                    "shared sample.");
                            }
                        }
                    }
                }
            }
        }

        size_t numBlocks = blocks.size();

        Workspace ws;
        ws.N = N;
        ws.size = S;
        ws.blocks = &blocks;
        BuildWorkspace(ws);

        // The negotiation is a fixed point over the partitions that each
        // block actually realizes.  A block's realized partition depends on
        // the face and edge targets, which in turn are the coarsest common
        // refinements of the realized partitions.  Because the merge is a
        // deterministic function of the constraints and the targets only
        // become finer, the iteration terminates.
        //
        // A target partition is stored as the union of the cuts that the
        // blocks on both sides realize on the shared feature.  The two
        // blocks must agree exactly on that feature, which is enforced by
        // refining the target to the common refinement of the realized
        // partitions at every iteration.
        std::vector<std::vector<Extractor::Box>> boxes(numBlocks);
        std::vector<std::unique_ptr<Extractor>> extractors(numBlocks);
        std::vector<std::unique_ptr<BlockConstraint>> constraints(numBlocks);

        std::map<FaceKey, Partition2> faceTargets;
        std::map<EdgeKey, std::vector<int32_t>> edgeTargets;

        // Start from the unconstrained partitions: each block at its own
        // depth.  This is the "candidate box partition" of the description.
        for (size_t i = 0; i < numBlocks; ++i)
        {
            Extractor candidate(N, blocks[i].samples.data(), false, N == 0,
                true);
            candidate.MergeBoxes(level, blocks[i].depth);
            boxes[i] = candidate.GetBoxes();
        }
        BuildFaceTargets(ws, boxes, S, faceTargets);
        ComputeEdgeTargets(ws, boxes, edgeTargets);
        PropagateEdges(faceTargets, edgeTargets, S);

        bool converged = false;
        for (int32_t iteration = 0; iteration < 4096; ++iteration)
        {
            for (size_t i = 0; i < numBlocks; ++i)
            {
                std::array<Partition2 const*, 6> facePointers;
                facePointers.fill(nullptr);
                for (int32_t dir = 0; dir < 6; ++dir)
                {
                    if (ws.faceIsShared[i][dir])
                    {
                        auto iter = faceTargets.find(ws.faceKeys[i][dir]);
                        if (iter != faceTargets.end())
                        {
                            facePointers[dir] = &iter->second;
                        }
                    }
                }

                std::array<std::vector<int32_t> const*, 12> edgePointers;
                edgePointers.fill(nullptr);
                for (size_t e = 0; e < 12; ++e)
                {
                    auto iter = edgeTargets.find(ws.blockEdges[i][e]);
                    if (iter != edgeTargets.end())
                    {
                        edgePointers[e] = &iter->second;
                    }
                }

                extractors[i] = std::make_unique<Extractor>(
                    N, blocks[i].samples.data(), false, N == 0, true);
                if (faceTargets.empty() && edgeTargets.empty())
                {
                    extractors[i]->SetMergeConstraint(nullptr);
                }
                else
                {
                    constraints[i] = std::make_unique<BlockConstraint>(
                        S, facePointers, edgePointers);
                    extractors[i]->SetMergeConstraint(constraints[i].get());
                }
                extractors[i]->MergeBoxes(level, blocks[i].depth);
                boxes[i] = extractors[i]->GetBoxes();
            }

            std::map<FaceKey, Partition2> newFaceTargets;
            BuildFaceTargets(ws, boxes, S, newFaceTargets);
            std::map<EdgeKey, std::vector<int32_t>> newEdgeTargets;
            ComputeEdgeTargets(ws, boxes, newEdgeTargets);
            PropagateEdges(newFaceTargets, newEdgeTargets, S);

            bool stable = (newFaceTargets == faceTargets) &&
                (newEdgeTargets == edgeTargets);
            faceTargets.swap(newFaceTargets);
            edgeTargets.swap(newEdgeTargets);
            if (stable)
            {
                converged = true;
                break;
            }
        }
        if (!converged)
        {
            throw std::runtime_error(
                "ASCBlockMesh: partition negotiation did not converge.");
        }

        // Phase 5: tessellate, orient against the global field, and merge
        // the vertices exactly.
        struct TriangleRecord
        {
            std::array<int32_t, 3> index;
            int32_t block;
        };

        ASCBlockMeshOutput output;
        std::vector<std::array<double, 3>> allVertices;
        std::map<std::array<double, 3>, int32_t> vertexMap;
        std::vector<TriangleRecord> triangles;

        auto AddVertex = [&](std::array<double, 3> const& position) -> int32_t
        {
            auto iter = vertexMap.find(position);
            if (iter != vertexMap.end())
            {
                return iter->second;
            }
            int32_t index = static_cast<int32_t>(allVertices.size());
            allVertices.push_back(position);
            vertexMap.emplace(position, index);
            return index;
        };

        for (size_t i = 0; i < numBlocks; ++i)
        {
            std::vector<Extractor::Vertex> localVertices;
            std::vector<Extractor::Triangle> localTriangles;
            extractors[i]->TessellateBoxes(localVertices, localTriangles);
            // The orientation convention is the one of the production
            // extractor, evaluated on the block-local samples.  It does not
            // depend on the order of the blocks.
            extractors[i]->OrientTriangles(localVertices, localTriangles, true);

            std::vector<int32_t> global(localVertices.size());
            for (size_t v = 0; v < localVertices.size(); ++v)
            {
                global[v] = AddVertex(ToWorld(localVertices[v],
                    blocks[i].origin));
            }

            for (Extractor::Triangle const& tri : localTriangles)
            {
                std::array<int32_t, 3> index = { global[tri.V[0]],
                    global[tri.V[1]], global[tri.V[2]] };
                int32_t smallest = 0;
                for (int32_t k = 1; k < 3; ++k)
                {
                    if (index[k] < index[smallest])
                    {
                        smallest = k;
                    }
                }
                std::array<int32_t, 3> rotated = { index[smallest],
                    index[(smallest + 1) % 3], index[(smallest + 2) % 3] };
                triangles.push_back(
                    TriangleRecord{ rotated, static_cast<int32_t>(i) });
            }
        }

        // Sort the vertices by position so the indexing, and therefore the
        // output, does not depend on the order of the input blocks.
        std::vector<int32_t> order(allVertices.size());
        for (size_t v = 0; v < allVertices.size(); ++v)
        {
            order[v] = static_cast<int32_t>(v);
        }
        std::sort(order.begin(), order.end(), [&](int32_t a, int32_t b)
        {
            return allVertices[a] < allVertices[b];
        });
        std::vector<int32_t> remap(allVertices.size());
        output.vertices.resize(allVertices.size());
        for (size_t i = 0; i < order.size(); ++i)
        {
            remap[order[i]] = static_cast<int32_t>(i);
            output.vertices[i] = allVertices[order[i]];
        }

        for (TriangleRecord& record : triangles)
        {
            std::array<int32_t, 3> index = { remap[record.index[0]],
                remap[record.index[1]], remap[record.index[2]] };
            int32_t smallest = 0;
            for (int32_t k = 1; k < 3; ++k)
            {
                if (index[k] < index[smallest])
                {
                    smallest = k;
                }
            }
            record.index = { index[smallest], index[(smallest + 1) % 3],
                index[(smallest + 2) % 3] };
        }

        // The triangles were oriented block by block, which is correct
        // inside a block but can disagree across a shared feature: the
        // extractor zeroes the gradient on a block's maximum outer plane, so
        // the two sides of a seam can choose opposite orientations.  Reorient
        // against the gradient of the sample field of the union of the
        // blocks.  On a single block this reproduces the extractor's rule
        // exactly (same formula, same boundary test), so the single-block
        // result is unchanged.
        {
            auto floorDiv = [](int64_t value, int64_t divisor)
            {
                int64_t quotient = value / divisor;
                int64_t remainder = value % divisor;
                if (remainder < 0)
                {
                    --quotient;
                }
                return quotient;
            };

            auto sample = [&](int64_t x, int64_t y, int64_t z,
                int32_t& value) -> bool
            {
                int64_t sx = floorDiv(x, S);
                int64_t sy = floorDiv(y, S);
                int64_t sz = floorDiv(z, S);
                auto iter = ws.indexOfOrigin.find(
                    std::make_tuple(sx * S, sy * S, sz * S));
                if (iter == ws.indexOfOrigin.end())
                {
                    return false;
                }
                int32_t lx = static_cast<int32_t>(x - sx * S);
                int32_t ly = static_cast<int32_t>(y - sy * S);
                int32_t lz = static_cast<int32_t>(z - sz * S);
                if (lx < 0 || ly < 0 || lz < 0 ||
                    lx > S || ly > S || lz > S)
                {
                    return false;
                }
                value = blocks[iter->second].samples[
                    lx + (S + 1) * (ly + (S + 1) * lz)];
                return true;
            };

            auto gradient = [&](std::array<double, 3> const& position)
            {
                std::array<double, 3> zero = { 0.0, 0.0, 0.0 };
                int64_t x = static_cast<int64_t>(std::floor(position[0]));
                int64_t y = static_cast<int64_t>(std::floor(position[1]));
                int64_t z = static_cast<int64_t>(std::floor(position[2]));
                int32_t f000, f100, f010, f110, f001, f101, f011, f111;
                if (!sample(x, y, z, f000) || !sample(x + 1, y, z, f100) ||
                    !sample(x, y + 1, z, f010) ||
                    !sample(x + 1, y + 1, z, f110) ||
                    !sample(x, y, z + 1, f001) ||
                    !sample(x + 1, y, z + 1, f101) ||
                    !sample(x, y + 1, z + 1, f011) ||
                    !sample(x + 1, y + 1, z + 1, f111))
                {
                    return zero;
                }

                double fx = position[0] - static_cast<double>(x);
                double fy = position[1] - static_cast<double>(y);
                double fz = position[2] - static_cast<double>(z);
                double ox = 1.0 - fx, oy = 1.0 - fy, oz = 1.0 - fz;
                std::array<double, 3> g;
                double t0 = oy * (f100 - f000) + fy * (f110 - f010);
                double t1 = oy * (f101 - f001) + fy * (f111 - f011);
                g[0] = oz * t0 + fz * t1;
                t0 = ox * (f010 - f000) + fx * (f110 - f100);
                t1 = ox * (f011 - f001) + fx * (f111 - f101);
                g[1] = oz * t0 + fz * t1;
                t0 = ox * (f001 - f000) + fx * (f101 - f100);
                t1 = ox * (f011 - f010) + fx * (f111 - f110);
                g[2] = oy * t0 + fy * t1;
                return g;
            };

            for (TriangleRecord& record : triangles)
            {
                std::array<double, 3> const& v0 = output.vertices[record.index[0]];
                std::array<double, 3> const& v1 = output.vertices[record.index[1]];
                std::array<double, 3> const& v2 = output.vertices[record.index[2]];
                std::array<double, 3> edge1, edge2, normal;
                for (int32_t k = 0; k < 3; ++k)
                {
                    edge1[k] = v1[k] - v0[k];
                    edge2[k] = v2[k] - v0[k];
                }
                normal[0] = edge1[1] * edge2[2] - edge1[2] * edge2[1];
                normal[1] = edge1[2] * edge2[0] - edge1[0] * edge2[2];
                normal[2] = edge1[0] * edge2[1] - edge1[1] * edge2[0];

                std::array<double, 3> g0 = gradient(v0);
                std::array<double, 3> g1 = gradient(v1);
                std::array<double, 3> g2 = gradient(v2);
                double dot = 0.0;
                for (int32_t k = 0; k < 3; ++k)
                {
                    dot += (g0[k] + g1[k] + g2[k]) / 3.0 * normal[k];
                }
                if (dot < 0.0)
                {
                    std::swap(record.index[1], record.index[2]);
                    int32_t smallest = 0;
                    for (int32_t k = 1; k < 3; ++k)
                    {
                        if (record.index[k] < record.index[smallest])
                        {
                            smallest = k;
                        }
                    }
                    record.index = { record.index[smallest],
                        record.index[(smallest + 1) % 3],
                        record.index[(smallest + 2) % 3] };
                }
            }
        }

        std::sort(triangles.begin(), triangles.end(),
            [](TriangleRecord const& a, TriangleRecord const& b)
        {
            if (a.index != b.index)
            {
                return a.index < b.index;
            }
            return a.block < b.block;
        });

        output.blockOrigins.reserve(numBlocks);
        for (size_t i = 0; i < numBlocks; ++i)
        {
            output.blockOrigins.push_back(std::array<int64_t, 3>{
                blocks[i].origin[0], blocks[i].origin[1],
                blocks[i].origin[2] });
        }

        for (TriangleRecord const& record : triangles)
        {
            if (!output.triangles.empty() &&
                output.triangles.back() == record.index)
            {
                continue;
            }
            output.triangles.push_back(record.index);
            output.triangleBlock.push_back(record.block);
        }

        return output;
    }
}
