// David Eberly, Geometric Tools, Redmond WA 98052
// Copyright (c) 1998-2026
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt
// https://www.geometrictools.com/License/Boost/LICENSE_1_0.txt
// File Version: 8.0.2026.11.16

// Tests for the multi-block adaptive skeleton climbing entry point.  The
// cases are registered in four groups.  Every property is checked from the
// output mesh alone (vertex positions, triangle indices and the per-triangle
// block source); no internal state of the implementation is inspected.

#include "task_support/check.h"
#include <Mathematics/ASCBlockMesh.h>
#include <Mathematics/AdaptiveSkeletonClimbing3.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace
{
    using gte::ASCBlockMeshBlock;
    using gte::ASCBlockMeshOutput;

    // Fixed-point quantization of a world position.  Vertices produced by
    // the two blocks on a shared feature are computed from identical sample
    // values and identical interpolation, so they are exactly equal as
    // doubles and a large scale is enough to compare them.
    using Position = std::array<int64_t, 3>;
    using EdgeKey = std::pair<Position, Position>;
    using TriangleKey = std::array<Position, 3>;
    using Field = std::function<int32_t(int64_t, int64_t, int64_t)>;

    int64_t constexpr kScale = 100000000LL;  // 1e8

    Position Quantize(std::array<double, 3> const& point)
    {
        return Position{ std::llround(point[0] * kScale),
            std::llround(point[1] * kScale),
            std::llround(point[2] * kScale) };
    }

    // Rotate a triangle so its smallest vertex is first, preserving the
    // cyclic order.  Two triangles describing the same directed face have
    // the same canonical form.
    TriangleKey CanonicalTriangle(ASCBlockMeshOutput const& mesh,
        std::array<int32_t, 3> const& triangle)
    {
        TriangleKey key = { Quantize(mesh.vertices[triangle[0]]),
            Quantize(mesh.vertices[triangle[1]]),
            Quantize(mesh.vertices[triangle[2]]) };
        int32_t smallest = 0;
        for (int32_t i = 1; i < 3; ++i)
        {
            if (key[i] < key[smallest])
            {
                smallest = i;
            }
        }
        return TriangleKey{ key[smallest], key[(smallest + 1) % 3],
            key[(smallest + 2) % 3] };
    }

    EdgeKey CanonicalEdge(Position const& a, Position const& b)
    {
        return (a < b) ? EdgeKey{ a, b } : EdgeKey{ b, a };
    }

    // Undirected edge incidence.  An interior edge of a closed manifold is
    // shared by exactly two triangles.
    std::map<EdgeKey, int32_t> EdgeHistogram(ASCBlockMeshOutput const& mesh)
    {
        std::map<EdgeKey, int32_t> histogram;
        for (auto const& triangle : mesh.triangles)
        {
            for (int32_t e = 0; e < 3; ++e)
            {
                Position a = Quantize(mesh.vertices[triangle[e]]);
                Position b = Quantize(mesh.vertices[triangle[(e + 1) % 3]]);
                if (a != b)
                {
                    ++histogram[CanonicalEdge(a, b)];
                }
            }
        }
        return histogram;
    }

    bool CollinearBetween(Position const& point, Position const& a,
        Position const& b)
    {
        int64_t cross[3];
        for (int32_t i = 0; i < 3; ++i)
        {
            int32_t j = (i + 1) % 3;
            int32_t k = (i + 2) % 3;
            cross[i] = (point[j] - a[j]) * (b[k] - a[k]) -
                (point[k] - a[k]) * (b[j] - a[j]);
        }
        if (cross[0] != 0 || cross[1] != 0 || cross[2] != 0)
        {
            return false;
        }
        for (int32_t i = 0; i < 3; ++i)
        {
            int64_t lo = std::min(a[i], b[i]);
            int64_t hi = std::max(a[i], b[i]);
            if (point[i] < lo || point[i] > hi)
            {
                return false;
            }
        }
        return true;
    }

    // True when a mesh vertex lies in the interior of a mesh edge.  That is
    // exactly a T-junction (a crack), not a closed manifold.
    bool HasTJunction(ASCBlockMeshOutput const& mesh)
    {
        std::set<Position> vertices;
        for (auto const& vertex : mesh.vertices)
        {
            vertices.insert(Quantize(vertex));
        }
        for (auto const& element : EdgeHistogram(mesh))
        {
            EdgeKey const& edge = element.first;
            for (auto const& vertex : vertices)
            {
                if (vertex == edge.first || vertex == edge.second)
                {
                    continue;
                }
                if (CollinearBetween(vertex, edge.first, edge.second))
                {
                    return true;
                }
            }
        }
        return false;
    }

    // The undirected cross-section segments of the surface on the plane
    // x = plane, in world coordinates.
    std::multiset<EdgeKey> PlaneSegments(ASCBlockMeshOutput const& mesh,
        double plane)
    {
        std::multiset<EdgeKey> segments;
        for (auto const& triangle : mesh.triangles)
        {
            for (int32_t e = 0; e < 3; ++e)
            {
                std::array<double, 3> const& p0 = mesh.vertices[triangle[e]];
                std::array<double, 3> const& p1 =
                    mesh.vertices[triangle[(e + 1) % 3]];
                if (std::abs(p0[0] - plane) > 1e-9 ||
                    std::abs(p1[0] - plane) > 1e-9)
                {
                    continue;
                }
                Position a = Quantize(p0);
                Position b = Quantize(p1);
                if (a != b)
                {
                    segments.insert(CanonicalEdge(a, b));
                }
            }
        }
        return segments;
    }

    double SegmentLength(Position const& a, Position const& b)
    {
        double dx = static_cast<double>(a[0] - b[0]);
        double dy = static_cast<double>(a[1] - b[1]);
        double dz = static_cast<double>(a[2] - b[2]);
        return std::sqrt(dx * dx + dy * dy + dz * dz) /
            static_cast<double>(kScale);
    }

    double LongestSegment(std::multiset<EdgeKey> const& segments)
    {
        double longest = 0.0;
        for (auto const& segment : segments)
        {
            longest = std::max(longest,
                SegmentLength(segment.first, segment.second));
        }
        return longest;
    }

    ASCBlockMeshBlock MakeBlock(std::array<int64_t, 3> origin, int32_t depth,
        int32_t N, Field const& field)
    {
        int32_t size = 1 << N;
        size_t samplesPerBlock = static_cast<size_t>(size + 1) *
            static_cast<size_t>(size + 1) * static_cast<size_t>(size + 1);
        ASCBlockMeshBlock block;
        block.origin = origin;
        block.depth = depth;
        block.samples.resize(samplesPerBlock);
        for (int32_t z = 0; z <= size; ++z)
        {
            for (int32_t y = 0; y <= size; ++y)
            {
                for (int32_t x = 0; x <= size; ++x)
                {
                    size_t index = static_cast<size_t>(x) +
                        static_cast<size_t>(size + 1) *
                        (static_cast<size_t>(y) +
                            static_cast<size_t>(size + 1) *
                            static_cast<size_t>(z));
                    block.samples[index] = field(x + origin[0],
                        y + origin[1], z + origin[2]);
                }
            }
        }
        return block;
    }

    // A sphere of radius 1.5 centred on the shared plane x = 8, with y and z
    // centred in the block range.  Level 0; no sample equals 0 because the
    // squared distance is an integer while the radius squared is 2.25.
    int32_t SeamSphereField(int64_t x, int64_t y, int64_t z)
    {
        double dx = static_cast<double>(x) - 8.0;
        double dy = static_cast<double>(y) - 4.0;
        double dz = static_cast<double>(z) - 4.0;
        return static_cast<int32_t>(std::llround(
            100.0 * (dx * dx + dy * dy + dz * dz - 2.25)));
    }

    // A saddle centred on the shared plane x = 8.  The surface crosses the
    // seam on both sides of the saddle point, which stresses the ambiguous
    // face handling at the seam.  The 100 factor makes the zero of the field
    // an integer value, so the level has to be offset.
    int32_t SaddleField(int64_t x, int64_t y, int64_t z)
    {
        double dx = static_cast<double>(x) - 8.0;
        double dy = static_cast<double>(y) - 4.0;
        double dz = static_cast<double>(z) - 4.0;
        return static_cast<int32_t>(std::llround(
            100.0 * (dx * dx - dy * dy + 0.5 * dz)));
    }

    // A sphere of radius 1.5 centred on the four-block shared edge
    // x = 4, y = 4 (the edge runs along z), with z centred.
    int32_t EdgeSphereField(int64_t x, int64_t y, int64_t z)
    {
        double dx = static_cast<double>(x) - 4.0;
        double dy = static_cast<double>(y) - 4.0;
        double dz = static_cast<double>(z) - 2.0;
        return static_cast<int32_t>(std::llround(
            100.0 * (dx * dx + dy * dy + dz * dz - 2.25)));
    }

    // A sphere of radius 1.5 centred on the eight-block shared corner.
    int32_t CornerSphereField(int64_t x, int64_t y, int64_t z)
    {
        double dx = static_cast<double>(x) - 4.0;
        double dy = static_cast<double>(y) - 4.0;
        double dz = static_cast<double>(z) - 4.0;
        return static_cast<int32_t>(std::llround(
            100.0 * (dx * dx + dy * dy + dz * dz - 2.25)));
    }

    // A plane, 1000z + 3x + 5y, whose zero set crosses every block.
    int32_t TiltedPlaneField(int64_t x, int64_t y, int64_t z)
    {
        return static_cast<int32_t>(1000 * z + 3 * x + 5 * y);
    }

    int32_t RampField(int64_t x, int64_t y, int64_t z)
    {
        return static_cast<int32_t>(3 * x + 5 * y + 7 * z);
    }

    bool AllIndicesValid(ASCBlockMeshOutput const& mesh)
    {
        int32_t count = static_cast<int32_t>(mesh.vertices.size());
        for (auto const& triangle : mesh.triangles)
        {
            for (int32_t index : triangle)
            {
                if (index < 0 || index >= count)
                {
                    return false;
                }
            }
        }
        return true;
    }

    // A directed triangle together with the origin of the block that
    // produced it.  Two outputs are compared after sorting on this key.
    struct SourcedTriangle
    {
        TriangleKey key;
        std::array<int64_t, 3> origin;

        bool operator<(SourcedTriangle const& other) const
        {
            return std::tie(key, origin) < std::tie(other.key, other.origin);
        }
    };

    std::vector<SourcedTriangle> SourcedTriangles(
        ASCBlockMeshOutput const& mesh)
    {
        std::vector<SourcedTriangle> result;
        result.reserve(mesh.triangles.size());
        for (size_t t = 0; t < mesh.triangles.size(); ++t)
        {
            int32_t block = mesh.triangleBlock[t];
            result.push_back(SourcedTriangle{
                CanonicalTriangle(mesh, mesh.triangles[t]),
                mesh.blockOrigins[block] });
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    bool SameSourcedGeometry(std::vector<SourcedTriangle> const& a,
        std::vector<SourcedTriangle> const& b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (a[i].key != b[i].key || a[i].origin != b[i].origin)
            {
                return false;
            }
        }
        return true;
    }

    // The canonical directed triangles of the original extractor for one
    // block, at one depth.
    std::multiset<TriangleKey> LegacyTriangles(int32_t N,
        std::vector<int32_t> const& samples, double level, int32_t depth)
    {
        using Extractor = gte::AdaptiveSkeletonClimbing3<int32_t, double>;
        Extractor legacy(N, samples.data(), false);
        std::vector<Extractor::Vertex> vertices;
        std::vector<Extractor::Triangle> triangles;
        legacy.Extract(level, depth, vertices, triangles);
        legacy.MakeUnique(vertices, triangles);
        legacy.OrientTriangles(vertices, triangles, true);

        std::multiset<TriangleKey> result;
        for (auto const& triangle : triangles)
        {
            TriangleKey key = { Quantize(vertices[triangle.V[0]]),
                Quantize(vertices[triangle.V[1]]),
                Quantize(vertices[triangle.V[2]]) };
            int32_t smallest = 0;
            for (int32_t i = 1; i < 3; ++i)
            {
                if (key[i] < key[smallest])
                {
                    smallest = i;
                }
            }
            result.insert(TriangleKey{ key[smallest],
                key[(smallest + 1) % 3], key[(smallest + 2) % 3] });
        }
        return result;
    }
}

// ---------------------------------------------------------------------------
// faces
// ---------------------------------------------------------------------------

static int faces_two_blocks_different_depth()
{
    int32_t N = 3;
    std::vector<ASCBlockMeshBlock> blocks;
    blocks.push_back(MakeBlock({ 0, 0, 0 }, 2, N, SeamSphereField));
    blocks.push_back(MakeBlock({ 8, 0, 0 }, 1, N, SeamSphereField));

    ASCBlockMeshOutput mesh = gte::ComputeASCBlockMesh(N, 0.0, blocks);
    TASK_CHECK(!mesh.triangles.empty());
    TASK_CHECK(AllIndicesValid(mesh));
    TASK_CHECK(mesh.triangleBlock.size() == mesh.triangles.size());

    // Both blocks contribute.
    std::set<int32_t> sources(mesh.triangleBlock.begin(),
        mesh.triangleBlock.end());
    TASK_CHECK(sources.size() == 2);

    // The seam has cross-section segments, and each one is emitted by both
    // blocks: the shared vertex indices make it one directed edge.
    std::multiset<EdgeKey> segments = PlaneSegments(mesh, 8.0);
    TASK_CHECK(!segments.empty());

    std::map<EdgeKey, std::set<int32_t>> edgeBlocks;
    for (size_t t = 0; t < mesh.triangles.size(); ++t)
    {
        auto const& triangle = mesh.triangles[t];
        for (int32_t e = 0; e < 3; ++e)
        {
            std::array<double, 3> const& p0 = mesh.vertices[triangle[e]];
            std::array<double, 3> const& p1 =
                mesh.vertices[triangle[(e + 1) % 3]];
            if (std::abs(p0[0] - 8.0) < 1e-9 && std::abs(p1[0] - 8.0) < 1e-9)
            {
                Position a = Quantize(p0);
                Position b = Quantize(p1);
                if (a != b)
                {
                    edgeBlocks[CanonicalEdge(a, b)].insert(
                        mesh.triangleBlock[t]);
                }
            }
        }
    }
    TASK_CHECK(!edgeBlocks.empty());
    for (auto const& element : edgeBlocks)
    {
        TASK_CHECK(element.second.size() == 2);
    }

    TASK_CHECK(!HasTJunction(mesh));
    return 0;
}

static int faces_plane_crossing_seam()
{
    int32_t N = 3;
    double level = 4500.5;
    std::vector<ASCBlockMeshBlock> blocks;
    blocks.push_back(MakeBlock({ 0, 0, 0 }, 2, N, TiltedPlaneField));
    blocks.push_back(MakeBlock({ 8, 0, 0 }, 3, N, TiltedPlaneField));

    ASCBlockMeshOutput mesh = gte::ComputeASCBlockMesh(N, level, blocks);
    TASK_CHECK(!mesh.triangles.empty());

    // The interpolated surface lies on the plane.
    double maxResidual = 0.0;
    for (auto const& vertex : mesh.vertices)
    {
        double value = 1000.0 * vertex[2] + 3.0 * vertex[0] +
            5.0 * vertex[1];
        maxResidual = std::max(maxResidual, std::abs(value - level));
    }
    TASK_CHECK(maxResidual < 1e-9);

    TASK_CHECK(!PlaneSegments(mesh, 8.0).empty());
    TASK_CHECK(!HasTJunction(mesh));
    return 0;
}

static int faces_saddle_on_seam()
{
    int32_t N = 3;
    // The saddle's surface crosses the seam x = 8 on both sides of the
    // saddle point, which exercises the ambiguous-face handling exactly at
    // the seam.
    double level = 50.5;
    std::vector<ASCBlockMeshBlock> blocks;
    blocks.push_back(MakeBlock({ 0, 0, 0 }, 1, N, SaddleField));
    blocks.push_back(MakeBlock({ 8, 0, 0 }, 2, N, SaddleField));

    ASCBlockMeshOutput mesh = gte::ComputeASCBlockMesh(N, level, blocks);
    TASK_CHECK(!mesh.triangles.empty());
    TASK_CHECK(AllIndicesValid(mesh));

    // Seam segments exist and every one is shared by both blocks (identical
    // endpoints and triangulation on the seam).
    std::map<EdgeKey, std::set<int32_t>> incidence;
    for (size_t t = 0; t < mesh.triangles.size(); ++t)
    {
        auto const& triangle = mesh.triangles[t];
        for (int32_t e = 0; e < 3; ++e)
        {
            std::array<double, 3> const& p0 = mesh.vertices[triangle[e]];
            std::array<double, 3> const& p1 =
                mesh.vertices[triangle[(e + 1) % 3]];
            if (std::abs(p0[0] - 8.0) < 1e-9 && std::abs(p1[0] - 8.0) < 1e-9)
            {
                Position a = Quantize(p0);
                Position b = Quantize(p1);
                if (a != b)
                {
                    if (p0 < p1)
                    {
                        incidence[EdgeKey{ a, b }].insert(mesh.triangleBlock[t]);
                    }
                    else
                    {
                        incidence[EdgeKey{ b, a }].insert(mesh.triangleBlock[t]);
                    }
                }
            }
        }
    }
    TASK_CHECK(!incidence.empty());
    for (auto const& element : incidence)
    {
        TASK_CHECK(element.second.size() == 2);
    }
    TASK_CHECK(!HasTJunction(mesh));
    return 0;
}

static int faces_closed_shell_across_blocks()
{
    int32_t N = 2;
    std::vector<ASCBlockMeshBlock> blocks;
    for (int32_t z = 0; z < 2; ++z)
    {
        for (int32_t y = 0; y < 2; ++y)
        {
            for (int32_t x = 0; x < 2; ++x)
            {
                blocks.push_back(MakeBlock({ 4LL * x, 4LL * y, 4LL * z },
                    1 + (x + y + z) % 2, N, CornerSphereField));
            }
        }
    }

    ASCBlockMeshOutput mesh = gte::ComputeASCBlockMesh(N, 0.0, blocks);
    TASK_CHECK(!mesh.triangles.empty());
    TASK_CHECK(AllIndicesValid(mesh));

    // The sphere is fully inside the union of the blocks, so the shell is
    // closed: every edge has exactly two incident triangles.
    auto histogram = EdgeHistogram(mesh);
    TASK_CHECK(!histogram.empty());
    for (auto const& element : histogram)
    {
        TASK_CHECK(element.second == 2);
    }

    std::set<int32_t> sources(mesh.triangleBlock.begin(),
        mesh.triangleBlock.end());
    TASK_CHECK(sources.size() == 8);
    TASK_CHECK(!HasTJunction(mesh));
    return 0;
}

static int faces_four_blocks_shared_edge()
{
    int32_t N = 2;
    std::vector<ASCBlockMeshBlock> blocks;
    blocks.push_back(MakeBlock({ 0, 0, 0 }, 2, N, EdgeSphereField));
    blocks.push_back(MakeBlock({ 4, 0, 0 }, 1, N, EdgeSphereField));
    blocks.push_back(MakeBlock({ 0, 4, 0 }, 1, N, EdgeSphereField));
    blocks.push_back(MakeBlock({ 4, 4, 0 }, 2, N, EdgeSphereField));

    ASCBlockMeshOutput mesh = gte::ComputeASCBlockMesh(N, 0.0, blocks);
    TASK_CHECK(!mesh.triangles.empty());
    TASK_CHECK(AllIndicesValid(mesh));
    TASK_CHECK(mesh.triangleBlock.size() == mesh.triangles.size());

    // The sphere crosses the shared edge x = 4, y = 4 at two points, one
    // near each end.  They are single mesh vertices common to all four
    // blocks, so the shared edge carries no duplicate or dangling vertex.
    int32_t onEdge = 0;
    for (auto const& vertex : mesh.vertices)
    {
        if (std::abs(vertex[0] - 4.0) < 1e-9 &&
            std::abs(vertex[1] - 4.0) < 1e-9)
        {
            ++onEdge;
            TASK_CHECK(vertex[2] > -1e-9 && vertex[2] < 4.0 + 1e-9);
        }
    }
    TASK_CHECK(onEdge == 2);

    // All four blocks contribute and no crack appears along the shared
    // edge.
    std::set<int32_t> sources(mesh.triangleBlock.begin(),
        mesh.triangleBlock.end());
    TASK_CHECK(sources.size() == 4);
    TASK_CHECK(!HasTJunction(mesh));
    return 0;
}

static int faces_eight_blocks_shared_corner()
{
    int32_t N = 2;
    std::vector<ASCBlockMeshBlock> blocks;
    for (int32_t z = 0; z < 2; ++z)
    {
        for (int32_t y = 0; y < 2; ++y)
        {
            for (int32_t x = 0; x < 2; ++x)
            {
                blocks.push_back(MakeBlock({ 4LL * x, 4LL * y, 4LL * z },
                    1 + (x + 2 * y + 3 * z) % 2, N, CornerSphereField));
            }
        }
    }

    ASCBlockMeshOutput mesh = gte::ComputeASCBlockMesh(N, 0.0, blocks);
    TASK_CHECK(!mesh.triangles.empty());
    TASK_CHECK(AllIndicesValid(mesh));

    std::set<int32_t> sources(mesh.triangleBlock.begin(),
        mesh.triangleBlock.end());
    TASK_CHECK(sources.size() == 8);
    TASK_CHECK(!HasTJunction(mesh));

    // The sphere is closed, so every mesh edge has two incident triangles.
    // All eight blocks contribute to the three shared planes, and every seam
    // segment on those planes is emitted by both adjacent blocks.  This can
    // only hold if the eight incident partitions agree on every shared edge
    // and on the shared corner.
    for (auto const& element : EdgeHistogram(mesh))
    {
        TASK_CHECK(element.second == 2);
    }

    for (double plane : { 4.0 })
    {
        std::map<EdgeKey, std::set<int32_t>> incidence;
        for (size_t t = 0; t < mesh.triangles.size(); ++t)
        {
            auto const& triangle = mesh.triangles[t];
            for (int32_t e = 0; e < 3; ++e)
            {
                std::array<double, 3> const& p0 =
                    mesh.vertices[triangle[e]];
                std::array<double, 3> const& p1 =
                    mesh.vertices[triangle[(e + 1) % 3]];
                for (int32_t axis = 0; axis < 3; ++axis)
                {
                    if (std::abs(p0[axis] - plane) < 1e-9 &&
                        std::abs(p1[axis] - plane) < 1e-9)
                    {
                        Position a = Quantize(p0);
                        Position b = Quantize(p1);
                        if (a != b)
                        {
                            incidence[CanonicalEdge(a, b)].insert(
                                mesh.triangleBlock[t]);
                        }
                    }
                }
            }
        }
        TASK_CHECK(!incidence.empty());
        for (auto const& element : incidence)
        {
            TASK_CHECK(element.second.size() == 2);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// junctions
// ---------------------------------------------------------------------------

static int junctions_missing_neighbour()
{
    int32_t N = 3;
    double level = 4500.5;
    // Three of the four blocks around the shared edge x = 8, y = 8; the
    // block at (8,8,0) is absent.
    std::vector<ASCBlockMeshBlock> blocks;
    blocks.push_back(MakeBlock({ 0, 0, 0 }, 2, N, TiltedPlaneField));
    blocks.push_back(MakeBlock({ 8, 0, 0 }, 1, N, TiltedPlaneField));
    blocks.push_back(MakeBlock({ 0, 8, 0 }, 1, N, TiltedPlaneField));

    ASCBlockMeshOutput mesh = gte::ComputeASCBlockMesh(N, level, blocks);
    TASK_CHECK(!mesh.triangles.empty());
    TASK_CHECK(AllIndicesValid(mesh));
    TASK_CHECK(mesh.triangleBlock.size() == mesh.triangles.size());
    TASK_CHECK(!HasTJunction(mesh));

    std::set<int32_t> sources(mesh.triangleBlock.begin(),
        mesh.triangleBlock.end());
    TASK_CHECK(sources.size() == 3);

    // The exposed face x = 16 belongs to no neighbour and must stay
    // adaptive, not forced to unit resolution: it carries a segment longer
    // than a unit cell.
    std::multiset<EdgeKey> exposed = PlaneSegments(mesh, 16.0);
    TASK_CHECK(!exposed.empty());
    TASK_CHECK(LongestSegment(exposed) > 1.0);

    // Forcing every block to the finest depth produces a strictly larger
    // mesh, so the exposed face was not resolved at unit level.
    std::vector<ASCBlockMeshBlock> fine;
    fine.push_back(MakeBlock({ 0, 0, 0 }, N, N, TiltedPlaneField));
    fine.push_back(MakeBlock({ 8, 0, 0 }, N, N, TiltedPlaneField));
    fine.push_back(MakeBlock({ 0, 8, 0 }, N, N, TiltedPlaneField));
    ASCBlockMeshOutput refined = gte::ComputeASCBlockMesh(N, level, fine);
    TASK_CHECK(refined.triangles.size() > mesh.triangles.size());
    return 0;
}

static int junctions_negative_origin()
{
    int32_t N = 2;
    auto field = [](int64_t x, int64_t y, int64_t z)
    {
        double dx = static_cast<double>(x);
        double dy = static_cast<double>(y) - 2.0;
        double dz = static_cast<double>(z) - 2.0;
        return static_cast<int32_t>(std::llround(
            100.0 * (dx * dx + dy * dy + dz * dz - 2.25)));
    };
    std::vector<ASCBlockMeshBlock> blocks;
    blocks.push_back(MakeBlock({ -4, 0, 0 }, 2, N, field));
    blocks.push_back(MakeBlock({ 0, 0, 0 }, 1, N, field));

    ASCBlockMeshOutput mesh = gte::ComputeASCBlockMesh(N, 0.0, blocks);
    TASK_CHECK(!mesh.triangles.empty());
    TASK_CHECK(AllIndicesValid(mesh));

    for (auto const& vertex : mesh.vertices)
    {
        TASK_CHECK(vertex[0] >= -4.0 - 1e-9 && vertex[0] <= 4.0 + 1e-9);
        TASK_CHECK(vertex[1] >= -1e-9 && vertex[1] <= 4.0 + 1e-9);
        TASK_CHECK(vertex[2] >= -1e-9 && vertex[2] <= 4.0 + 1e-9);
    }

    TASK_CHECK(!PlaneSegments(mesh, 0.0).empty());
    TASK_CHECK(!HasTJunction(mesh));

    // The surface is closed, so every edge has two incident triangles.
    for (auto const& element : EdgeHistogram(mesh))
    {
        TASK_CHECK(element.second == 2);
    }
    return 0;
}

static int junctions_two_block_seam_segments_match()
{
    int32_t N = 3;
    std::vector<ASCBlockMeshBlock> blocks;
    blocks.push_back(MakeBlock({ 0, 0, 0 }, 3, N, SeamSphereField));
    blocks.push_back(MakeBlock({ 8, 0, 0 }, 1, N, SeamSphereField));

    ASCBlockMeshOutput mesh = gte::ComputeASCBlockMesh(N, 0.0, blocks);
    TASK_CHECK(!mesh.triangles.empty());

    // The seam cross-section is a closed loop.  Every seam segment is shared
    // by both blocks and every seam vertex has exactly two distinct seam
    // segments, which is only possible when both sides subdivide the seam
    // identically.
    std::map<EdgeKey, std::set<int32_t>> incidence;
    for (size_t t = 0; t < mesh.triangles.size(); ++t)
    {
        auto const& triangle = mesh.triangles[t];
        for (int32_t e = 0; e < 3; ++e)
        {
            std::array<double, 3> const& p0 = mesh.vertices[triangle[e]];
            std::array<double, 3> const& p1 =
                mesh.vertices[triangle[(e + 1) % 3]];
            if (std::abs(p0[0] - 8.0) < 1e-9 && std::abs(p1[0] - 8.0) < 1e-9)
            {
                Position a = Quantize(p0);
                Position b = Quantize(p1);
                if (a != b)
                {
                    incidence[CanonicalEdge(a, b)].insert(
                        mesh.triangleBlock[t]);
                }
            }
        }
    }
    TASK_CHECK(!incidence.empty());
    std::map<Position, int32_t> degree;
    for (auto const& element : incidence)
    {
        TASK_CHECK(element.second.size() == 2);
        ++degree[element.first.first];
        ++degree[element.first.second];
    }
    for (auto const& element : degree)
    {
        TASK_CHECK(element.second == 2);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// adaptivity
// ---------------------------------------------------------------------------

static int adaptivity_coarse_depth_has_large_segments()
{
    int32_t N = 3;
    double level = 4500.5;
    std::vector<ASCBlockMeshBlock> blocks;
    blocks.push_back(MakeBlock({ 0, 0, 0 }, 1, N, TiltedPlaneField));
    blocks.push_back(MakeBlock({ 8, 0, 0 }, 1, N, TiltedPlaneField));
    ASCBlockMeshOutput adaptive = gte::ComputeASCBlockMesh(N, level, blocks);
    TASK_CHECK(!adaptive.triangles.empty());

    std::multiset<EdgeKey> segments = PlaneSegments(adaptive, 8.0);
    TASK_CHECK(!segments.empty());
    TASK_CHECK(LongestSegment(segments) > 1.0);

    // Forcing the finest depth on both blocks yields more triangles: the
    // adaptive scheme must not spend unit cells everywhere.
    std::vector<ASCBlockMeshBlock> fine;
    fine.push_back(MakeBlock({ 0, 0, 0 }, N, N, TiltedPlaneField));
    fine.push_back(MakeBlock({ 8, 0, 0 }, N, N, TiltedPlaneField));
    ASCBlockMeshOutput refined = gte::ComputeASCBlockMesh(N, level, fine);
    TASK_CHECK(refined.triangles.size() > adaptive.triangles.size());
    return 0;
}

static int adaptivity_interior_partition_unchanged()
{
    int32_t N = 3;
    double level = 107.5;
    // The two blocks of a nested pair have different depths.  The shared
    // face x = 8 is negotiated, but a box strictly inside the first block is
    // not touched by the negotiation: its interior vertices match the
    // single-block partition at the same depth.
    std::vector<ASCBlockMeshBlock> blocks;
    blocks.push_back(MakeBlock({ 0, 0, 0 }, 2, N, RampField));
    blocks.push_back(MakeBlock({ 8, 0, 0 }, 1, N, RampField));
    ASCBlockMeshOutput mesh = gte::ComputeASCBlockMesh(N, level, blocks);
    TASK_CHECK(!mesh.triangles.empty());

    // The partition of block 0 at its own depth is used as the reference.
    std::vector<ASCBlockMeshBlock> one;
    one.push_back(MakeBlock({ 0, 0, 0 }, 2, N, RampField));
    ASCBlockMeshOutput alone = gte::ComputeASCBlockMesh(N, level, one);
    TASK_CHECK(!alone.triangles.empty());

    // Every vertex of the reference that is at least one unit from the
    // shared face is present in the negotiated mesh.
    int32_t interior = 0;
    for (auto const& vertex : alone.vertices)
    {
        if (vertex[0] < 8.0 - 1.0 + 1e-9)
        {
            ++interior;
        }
    }
    TASK_CHECK(interior > 0);

    std::set<Position> negotiated;
    for (auto const& vertex : mesh.vertices)
    {
        negotiated.insert(Quantize(vertex));
    }
    for (auto const& vertex : alone.vertices)
    {
        if (vertex[0] < 8.0 - 1.0 + 1e-9)
        {
            TASK_CHECK(negotiated.count(Quantize(vertex)) == 1);
        }
    }
    return 0;
}

static int adaptivity_full_root_box_surface()
{
    int32_t N = 2;
    // The monotone field x + y + z lets the whole block merge into one root
    // box.  The surface inside that box must still be emitted.
    auto field = [](int64_t x, int64_t y, int64_t z)
    {
        return static_cast<int32_t>(x + y + z);
    };
    std::vector<ASCBlockMeshBlock> blocks;
    blocks.push_back(MakeBlock({ 0, 0, 0 }, 0, N, field));
    ASCBlockMeshOutput mesh = gte::ComputeASCBlockMesh(N, 2.5, blocks);
    TASK_CHECK(!mesh.triangles.empty());
    TASK_CHECK(AllIndicesValid(mesh));

    for (auto const& vertex : mesh.vertices)
    {
        TASK_CHECK(std::abs(vertex[0] + vertex[1] + vertex[2] - 2.5) < 1e-9);
    }
    return 0;
}

static int adaptivity_plane_counts_are_adaptive()
{
    int32_t N = 3;
    double level = 4500.5;
    std::vector<ASCBlockMeshBlock> coarse;
    coarse.push_back(MakeBlock({ 0, 0, 0 }, 1, N, TiltedPlaneField));
    coarse.push_back(MakeBlock({ 8, 0, 0 }, 1, N, TiltedPlaneField));
    ASCBlockMeshOutput adaptive = gte::ComputeASCBlockMesh(N, level, coarse);

    std::vector<ASCBlockMeshBlock> fine;
    fine.push_back(MakeBlock({ 0, 0, 0 }, N, N, TiltedPlaneField));
    fine.push_back(MakeBlock({ 8, 0, 0 }, N, N, TiltedPlaneField));
    ASCBlockMeshOutput refined = gte::ComputeASCBlockMesh(N, level, fine);

    TASK_CHECK(adaptive.triangles.size() < refined.triangles.size());
    TASK_CHECK(LongestSegment(PlaneSegments(adaptive, 8.0)) > 1.0);
    return 0;
}

// ---------------------------------------------------------------------------
// compatibility
// ---------------------------------------------------------------------------

static int compatibility_invalid_inputs_throw()
{
    int32_t N = 2;
    std::vector<ASCBlockMeshBlock> blocks;
    blocks.push_back(MakeBlock({ 0, 0, 0 }, 1, N, RampField));

    auto Throws = [](std::function<void()> const& call)
    {
        try
        {
            call();
        }
        catch (std::invalid_argument const&)
        {
            return true;
        }
        catch (...)
        {
            return false;
        }
        return false;
    };

    // Empty input.
    TASK_CHECK(Throws([&]()
    {
        gte::ComputeASCBlockMesh(N, 100.5,
            std::vector<ASCBlockMeshBlock>{});
    }));

    // A level that equals a sample value.
    TASK_CHECK(Throws([&]()
    {
        gte::ComputeASCBlockMesh(N,
            static_cast<double>(blocks[0].samples[0]), blocks);
    }));

    // A non-finite level.
    TASK_CHECK(Throws([&]()
    {
        gte::ComputeASCBlockMesh(N,
            std::numeric_limits<double>::infinity(), blocks);
    }));

    // A depth outside [-1,N].
    TASK_CHECK(Throws([&]()
    {
        std::vector<ASCBlockMeshBlock> bad = blocks;
        bad[0].depth = N + 1;
        gte::ComputeASCBlockMesh(N, 100.5, bad);
    }));

    // A wrong sample count.
    TASK_CHECK(Throws([&]()
    {
        std::vector<ASCBlockMeshBlock> bad = blocks;
        bad[0].samples.pop_back();
        gte::ComputeASCBlockMesh(N, 100.5, bad);
    }));

    // An origin that is not a multiple of 2^N.
    TASK_CHECK(Throws([&]()
    {
        std::vector<ASCBlockMeshBlock> bad = blocks;
        bad[0].origin[0] = 1;
        gte::ComputeASCBlockMesh(N, 100.5, bad);
    }));

    // A negative N.
    TASK_CHECK(Throws([&]()
    {
        gte::ComputeASCBlockMesh(-1, 100.5, blocks);
    }));
    return 0;
}

static int compatibility_duplicate_and_conflicting_blocks()
{
    int32_t N = 2;
    std::vector<ASCBlockMeshBlock> blocks;
    blocks.push_back(MakeBlock({ 0, 0, 0 }, 1, N, RampField));

    auto Throws = [](std::function<void()> const& call)
    {
        try
        {
            call();
        }
        catch (std::invalid_argument const&)
        {
            return true;
        }
        catch (...)
        {
            return false;
        }
        return false;
    };

    // A duplicate origin means the blocks overlap.
    TASK_CHECK(Throws([&]()
    {
        std::vector<ASCBlockMeshBlock> bad = blocks;
        bad.push_back(MakeBlock({ 0, 0, 0 }, 2, N, RampField));
        gte::ComputeASCBlockMesh(N, 100.5, bad);
    }));

    // A block placed on top of a diagonal neighbour shares only a corner
    // with the first block; it is not an overlap and must be accepted.
    TASK_CHECK(!Throws([&]()
    {
        std::vector<ASCBlockMeshBlock> good;
        good.push_back(MakeBlock({ 0, 0, 0 }, 1, N, RampField));
        good.push_back(MakeBlock({ 4, 4, 4 }, 1, N, RampField));
        gte::ComputeASCBlockMesh(N, 100.5, good);
    }));

    // Two touching blocks that disagree on a shared sample.
    TASK_CHECK(Throws([&]()
    {
        std::vector<ASCBlockMeshBlock> bad;
        bad.push_back(MakeBlock({ 0, 0, 0 }, 1, N, RampField));
        bad.push_back(MakeBlock({ 4, 0, 0 }, 1, N,
            [](int64_t x, int64_t y, int64_t z)
            {
                return static_cast<int32_t>(3 * x + 5 * y + 7 * z + 1);
            }));
        gte::ComputeASCBlockMesh(N, 100.5, bad);
    }));
    return 0;
}

static int compatibility_input_unchanged()
{
    int32_t N = 2;
    std::vector<ASCBlockMeshBlock> blocks;
    blocks.push_back(MakeBlock({ 0, 0, 0 }, 1, N, CornerSphereField));
    blocks.push_back(MakeBlock({ 4, 0, 0 }, 2, N, CornerSphereField));

    std::vector<int32_t> copy0 = blocks[0].samples;
    std::vector<int32_t> copy1 = blocks[1].samples;
    std::array<int64_t, 3> origin0 = blocks[0].origin;
    std::array<int64_t, 3> origin1 = blocks[1].origin;
    int32_t depth0 = blocks[0].depth;
    int32_t depth1 = blocks[1].depth;

    gte::ComputeASCBlockMesh(N, 0.0, blocks);

    TASK_CHECK(blocks.size() == 2);
    TASK_CHECK(blocks[0].samples == copy0);
    TASK_CHECK(blocks[1].samples == copy1);
    TASK_CHECK(blocks[0].origin == origin0);
    TASK_CHECK(blocks[1].origin == origin1);
    TASK_CHECK(blocks[0].depth == depth0);
    TASK_CHECK(blocks[1].depth == depth1);

    // A throwing call leaves the input untouched as well.
    std::vector<ASCBlockMeshBlock> bad = blocks;
    bad[0].samples[0] = 5;
    std::vector<int32_t> before = bad[0].samples;
    bool threw = false;
    try
    {
        gte::ComputeASCBlockMesh(N, 5.0, bad);
    }
    catch (std::invalid_argument const&)
    {
        threw = true;
    }
    TASK_CHECK(bad[0].samples == before);
    return 0;
}

static int compatibility_permutation_determinism()
{
    int32_t N = 2;
    double level = 4500.5;
    std::vector<std::array<int64_t, 3>> origins = {
        { 0, 0, 0 }, { 4, 0, 0 }, { 0, 4, 0 }, { 4, 4, 0 },
        { 0, 0, 4 }, { 4, 0, 4 }, { 0, 4, 4 }, { 4, 4, 4 } };
    std::vector<int32_t> depths = { 1, 2, 1, 2, 2, 1, 2, 1 };

    std::vector<ASCBlockMeshBlock> original;
    for (size_t i = 0; i < origins.size(); ++i)
    {
        original.push_back(MakeBlock(origins[i], depths[i], N,
            TiltedPlaneField));
    }

    ASCBlockMeshOutput first = gte::ComputeASCBlockMesh(N, level, original);
    TASK_CHECK(!first.triangles.empty());

    std::vector<std::vector<size_t>> permutations = {
        { 7, 6, 5, 4, 3, 2, 1, 0 },
        { 3, 0, 6, 1, 7, 2, 5, 4 },
        { 4, 2, 6, 0, 5, 1, 3, 7 } };
    for (auto const& permutation : permutations)
    {
        std::vector<ASCBlockMeshBlock> permuted;
        for (size_t index : permutation)
        {
            permuted.push_back(original[index]);
        }
        ASCBlockMeshOutput other = gte::ComputeASCBlockMesh(N, level,
            permuted);

        // The sorted vertex list is the same.
        TASK_CHECK(first.vertices == other.vertices);

        // The directed triangles, aligned by source origin, are the same.
        TASK_CHECK(first.triangles.size() == other.triangles.size());
        TASK_CHECK(SameSourcedGeometry(SourcedTriangles(first),
            SourcedTriangles(other)));
    }

    // A repeated call on the same input gives the same result.
    ASCBlockMeshOutput again = gte::ComputeASCBlockMesh(N, level, original);
    TASK_CHECK(first.vertices == again.vertices);
    TASK_CHECK(first.triangles == again.triangles);
    TASK_CHECK(first.triangleBlock == again.triangleBlock);
    return 0;
}

static int compatibility_single_block_matches_legacy()
{
    int32_t N = 3;
    std::vector<int32_t> samples(9 * 9 * 9);
    for (int32_t z = 0; z < 9; ++z)
    {
        for (int32_t y = 0; y < 9; ++y)
        {
            for (int32_t x = 0; x < 9; ++x)
            {
                double value = std::sin(0.9 * x) + std::cos(0.7 * y) +
                    0.5 * std::sin(0.5 * z) + 0.8 * std::cos(0.4 * x + z);
                samples[x + 9 * (y + 9 * z)] =
                    static_cast<int32_t>(std::llround(1000.0 * value));
            }
        }
    }

    // Depths 0..N never synthesize the full root box for this field, so the
    // result must equal the original extractor with fixBoundary = false at
    // the same depth, including the orientation convention.
    for (int32_t depth = 0; depth <= N; ++depth)
    {
        ASCBlockMeshBlock block;
        block.origin = { 0, 0, 0 };
        block.depth = depth;
        block.samples = samples;
        ASCBlockMeshOutput mesh = gte::ComputeASCBlockMesh(N, 0.5, { block });
        TASK_CHECK(!mesh.triangles.empty());

        std::multiset<TriangleKey> actual;
        for (auto const& triangle : mesh.triangles)
        {
            actual.insert(CanonicalTriangle(mesh, triangle));
        }
        TASK_CHECK(actual == LegacyTriangles(N, samples, 0.5, depth));
    }

    // Depth -1 is the unrestricted partition, the same as depth 0.
    ASCBlockMeshBlock minusOne;
    minusOne.origin = { 0, 0, 0 };
    minusOne.depth = -1;
    minusOne.samples = samples;
    ASCBlockMeshOutput mesh = gte::ComputeASCBlockMesh(N, 1234.5, { minusOne });
    std::multiset<TriangleKey> actual;
    for (auto const& triangle : mesh.triangles)
    {
        actual.insert(CanonicalTriangle(mesh, triangle));
    }
    TASK_CHECK(actual == LegacyTriangles(N, samples, 1234.5, 0));
    return 0;
}

static int compatibility_no_degenerate_or_duplicate_faces()
{
    int32_t N = 2;
    std::vector<ASCBlockMeshBlock> blocks;
    blocks.push_back(MakeBlock({ 0, 0, 0 }, 2, N, CornerSphereField));
    blocks.push_back(MakeBlock({ 4, 0, 0 }, 1, N, CornerSphereField));

    ASCBlockMeshOutput mesh = gte::ComputeASCBlockMesh(N, 0.0, blocks);
    TASK_CHECK(!mesh.triangles.empty());

    // No triangle appears twice.
    std::map<TriangleKey, int32_t> seen;
    for (auto const& triangle : mesh.triangles)
    {
        ++seen[CanonicalTriangle(mesh, triangle)];
    }
    for (auto const& element : seen)
    {
        TASK_CHECK(element.second == 1);
    }

    // No triangle is degenerate.
    for (auto const& triangle : mesh.triangles)
    {
        Position a = Quantize(mesh.vertices[triangle[0]]);
        Position b = Quantize(mesh.vertices[triangle[1]]);
        Position c = Quantize(mesh.vertices[triangle[2]]);
        TASK_CHECK(a != b);
        TASK_CHECK(b != c);
        TASK_CHECK(a != c);
    }

    // Triangle sources are valid and parallel to the triangle list.
    TASK_CHECK(mesh.triangleBlock.size() == mesh.triangles.size());
    for (int32_t block : mesh.triangleBlock)
    {
        TASK_CHECK(block >= 0 &&
            block < static_cast<int32_t>(mesh.blockOrigins.size()));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
    { "faces", "two_blocks_different_depth", faces_two_blocks_different_depth },
    { "faces", "plane_crossing_seam", faces_plane_crossing_seam },
    { "faces", "saddle_on_seam", faces_saddle_on_seam },
    { "faces", "closed_shell_across_blocks", faces_closed_shell_across_blocks },
    { "faces", "four_blocks_shared_edge", faces_four_blocks_shared_edge },
    { "faces", "eight_blocks_shared_corner", faces_eight_blocks_shared_corner },
    { "junctions", "missing_neighbour", junctions_missing_neighbour },
    { "junctions", "negative_origin", junctions_negative_origin },
    { "junctions", "two_block_seam_segments_match", junctions_two_block_seam_segments_match },
    { "adaptivity", "coarse_depth_has_large_segments", adaptivity_coarse_depth_has_large_segments },
    { "adaptivity", "interior_partition_unchanged", adaptivity_interior_partition_unchanged },
    { "adaptivity", "full_root_box_surface", adaptivity_full_root_box_surface },
    { "adaptivity", "plane_counts_are_adaptive", adaptivity_plane_counts_are_adaptive },
    { "compatibility", "invalid_inputs_throw", compatibility_invalid_inputs_throw },
    { "compatibility", "duplicate_and_conflicting_blocks", compatibility_duplicate_and_conflicting_blocks },
    { "compatibility", "input_unchanged", compatibility_input_unchanged },
    { "compatibility", "permutation_determinism", compatibility_permutation_determinism },
    { "compatibility", "single_block_matches_legacy", compatibility_single_block_matches_legacy },
    { "compatibility", "no_degenerate_or_duplicate_faces", compatibility_no_degenerate_or_duplicate_faces },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
