#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace polymesh
{
/// A single corner of a polygon soup face.
/// Indices are zero-based positions into the vertex/texcoord/normal arrays.
/// Negative texcoord/normal indices mean "not specified".
struct soup_corner
{
    int vertex = -1;
    int texcoord = -1;
    int normal = -1;
};

/// One polygon of the soup. At least three corners with distinct vertices.
struct soup_face
{
    std::vector<soup_corner> corners;
};

/// Result of analyzing and repairing a polygon soup.
///
/// Every input face is preserved. Faces may be flipped so that winding is
/// consistent along every shared edge; within an edge-connected component the
/// face with the smallest source index keeps its original winding. A flipped
/// face still starts with its original first corner.
///
/// Vertices are duplicated wherever disconnected face fans meet at one source
/// vertex, so every output vertex belongs to exactly one connected face fan.
/// Vertices with identical coordinates but different source indices are not
/// welded, and isolated source vertices are emitted once.
struct repaired_soup
{
    /// One entry per output vertex: zero-based source vertex position.
    std::vector<int> vertex_source;
    /// One entry per output face: zero-based source face index.
    std::vector<int> face_source;
    /// One entry per output face corner (flat over faces): zero-based source
    /// face corner, accumulated over the source faces in file order.
    std::vector<int> halfedge_source;

    /// Output faces as indices into vertex_source / the output vertex slots.
    std::vector<std::vector<int>> faces;
    /// Per output corner, the corresponding source corner attributes.
    std::vector<std::vector<soup_corner>> corners;
    /// Source vertex indices that carry no face at all.
    std::vector<int> isolated_vertices;
    /// True for every output face whose corner order was reversed.
    std::vector<bool> flipped;

    int vertex_count() const { return (int)vertex_source.size(); }
    int face_count() const { return (int)faces.size(); }
};

/// Analyzes the connectivity of a polygon soup and builds a repaired soup with
/// consistent winding and per-fan vertex duplication.
///
/// Throws std::invalid_argument when:
///  * a corner references a missing/zero vertex, texcoord or normal index
///  * a face contains a repeated vertex
///  * two faces are duplicates up to cyclic shift or reversal
///  * three faces share one undirected edge
///  * an edge-connected component has contradictory winding constraints
///
/// The input itself is never modified.
repaired_soup repair_polygon_soup(std::vector<soup_face> const& faces, int vertex_count, int texcoord_count = 0, int normal_count = 0);
} // namespace polymesh
