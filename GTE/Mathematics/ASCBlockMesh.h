// David Eberly, Geometric Tools, Redmond WA 98052
// Copyright (c) 1998-2026
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt
// https://www.geometrictools.com/License/Boost/LICENSE_1_0.txt
// File Version: 8.0.2026.11.16

#pragma once

// Multi-block adaptive skeleton climbing.
//
// The adaptive skeleton climbing extractor in AdaptiveSkeletonClimbing3.h
// operates on a single (2^N+1)-by-(2^N+1)-by-(2^N+1) image.  When an image
// is partitioned into blocks, adjacent blocks generally produce different
// box partitions and therefore different intersection segments on their
// shared faces.  Setting 'fixBoundary' to 'true' avoids the mismatch by
// forcing the unit-cell resolution on every block boundary, which defeats
// the adaptivity of the method.
//
// This class keeps the adaptive partition inside each block but negotiates
// the partition on the features shared by the blocks.  Let S = 2^N.  Every
// block is an S-by-S-by-S cube whose integer origin is a multiple of S and
// whose samples are (S+1)^3 values stored in lexicographic order (x fastest,
// then y, then z).  Blocks may be missing from the domain, but two blocks
// that touch must agree, sample for sample, on their common samples.
//
// Negotiation:
//   1. Run the ordinary merge phase of every block using its own depth.  This
//      is the candidate partition; interior boxes are never refined later.
//   2. For a shared face, restrict both blocks' candidate partitions to the
//      face and take the coarsest common refinement of the two 2D dyadic
//      partitions.
//   3. For a shared edge, all blocks incident to the edge contribute their
//      cuts along the edge.  The union is propagated into the faces that
//      border the edge and then to the other sides of those faces, so a
//      refinement discovered on a shared edge reaches every face and edge
//      that touches it.  This is what keeps four blocks around an edge and
//      eight blocks around a corner consistent.
//   4. Merge every block again under the negotiated constraints and recompute
//      the targets from the realized partitions.  Repeat until the targets no
//      longer change.  Using the realized partitions (not just the candidate
//      ones) is what makes the two sides of a shared feature agree exactly,
//      because a constrained merge can realize a finer partition than its
//      candidate.  Faces and edges are processed as maps keyed by their world
//      geometry, so the result does not depend on the block order.
//
// A box that touches a shared face is restricted to the unit slab normal to
// that face and to one negotiated cell of the face.  The unit slab makes the
// crossing on the perpendicular box edges a function of the samples on the
// face alone, so both blocks choose the same seam vertices and segments.  The
// tangential extent remains adaptive, so a coarse block still produces long
// segments along the seam.  A box that touches a shared edge may not cross a
// negotiated edge cut.  Boxes that touch no shared feature are never
// constrained, so the interior partition of each block and the partition of
// an exposed face are exactly those of the single-block merge at that block's
// depth.  Setting the depth of every block to N (or comparing against
// fixBoundary = true) is therefore strictly more expensive than this
// negotiation.
//
// The constraints are fed into the existing
// Merge/DoXMerge/DoYMerge/DoZMerge and Tessellate pipeline.  The original
// linear merge trees, the interpolation and the ambiguous-face handling are
// reused, so no second isosurface algorithm is introduced.
//
// The output is an indexed triangle mesh in world coordinates.  Vertices on
// shared features are computed identically by both incident blocks (the
// sample values, the interpolation and the negotiated partitions are
// identical), so the vertex deduplication is exact and a closed surface that
// crosses shared features carries no T-junctions.  'triangleBlock' contains,
// for every triangle, the index of the input block that produced it, and
// 'blockOrigins' maps that index to the block origin, so the face source of a
// triangle is known.

#include <array>
#include <cstdint>
#include <vector>

namespace gte
{
    // The input block.  'origin' is in sample units and must be a multiple of
    // 2^N in each component; the block covers [origin, origin + 2^N] in world
    // sample coordinates.  'samples' has (2^N+1)^3 elements ordered so that
    // x varies fastest, then y, then z.  'depth' is in [-1,N]; -1 and 0 both
    // request the ordinary unrestricted merge of the block.
    struct ASCBlockMeshBlock
    {
        std::array<int64_t, 3> origin{ 0, 0, 0 };
        std::vector<int32_t> samples;
        int32_t depth{ -1 };
    };

    // An indexed triangle mesh in world coordinates.  A triangle references
    // three entries of 'vertices' and is stored in the ordered form used by
    // TriangleKey<true>: the smallest index is first and the cyclic order is
    // preserved.  'triangleBlock' is parallel to 'triangles' and holds the
    // index of the input block that generated the triangle.  'blockOrigins'
    // holds the origin of each input block, so the face source of a triangle
    // is 'blockOrigins[triangleBlock[t]]'.  Triangles are sorted, and
    // vertices are sorted by position, so the result does not depend on the
    // order in which the blocks were supplied.
    struct ASCBlockMeshOutput
    {
        std::vector<std::array<double, 3>> vertices;
        std::vector<std::array<int32_t, 3>> triangles;
        std::vector<int32_t> triangleBlock;
        std::vector<std::array<int64_t, 3>> blockOrigins;
    };

    // Throws std::invalid_argument on malformed input:
    //   - 'blocks' is empty,
    //   - N is outside [0,10],
    //   - 'level' is not finite or equals one of the samples,
    //   - a block origin is not a multiple of 2^N or overflows,
    //   - a sample array does not have (2^N+1)^3 elements,
    //   - a depth is outside [-1,N],
    //   - two blocks have the same origin (the blocks overlap), or
    //   - two blocks that touch disagree on a shared sample.
    // The input is not modified.
    ASCBlockMeshOutput ComputeASCBlockMesh(int32_t N, double level,
        std::vector<ASCBlockMeshBlock> const& blocks);
}
