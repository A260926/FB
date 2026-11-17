#pragma once

#include <string>
#include <vector>

namespace polymesh
{
namespace soup
{
/// A single corner of a face in an indexed face soup.
/// Indices follow OBJ conventions: position indices are 1-based and positive,
/// texture/normal indices are 1-based with 0 meaning "not present".
struct corner
{
    int vertex = 0;
    int tex = 0;
    int normal = 0;
};

/// A polygon of an indexed face soup.
struct face
{
    std::vector<corner> corners;
};

/// Parses one OBJ face-corner token ("v", "v/vt", "v//vn" or "v/vt/vn")
/// into an index triple. Zero in a slot means the index was not present.
/// Shared by the legacy and the repairing OBJ reader paths.
corner parse_obj_corner_token(std::string token);

/// One corner of a repaired face.
struct repaired_corner
{
    int out_vertex = 0;    ///< output vertex slot
    int source_vertex = 0; ///< 0-based source position index
    int source_corner = 0; ///< global 0-based source corner index (face-local offset + local index)
    int tex = 0;           ///< original 1-based texture index (0 if absent)
    int normal = 0;        ///< original 1-based normal index (0 if absent)
};

/// A face of the repaired soup, already in consistent winding.
/// A flipped face still starts with its original first corner.
struct repaired_face
{
    std::vector<repaired_corner> corners;
};

/// Result of analyzing and repairing a face soup.
///
/// The output contains exactly as many faces as the input. Vertices are only
/// duplicated where disconnected face fans share a source position index;
/// positions that coincide but have different indices are never welded.
struct repaired_soup
{
    std::vector<repaired_face> faces;
    int vertex_count = 0;

    /// output vertex slot -> 0-based source position index
    std::vector<int> vertex_source;
    /// output face slot -> 0-based source face index
    std::vector<int> face_source;
};

/// Analyzes the connectivity of a polygon soup and makes it manifold and
/// orientable without dropping any face.
///
/// Performed steps:
///  * validate indices, per-face distinctness, edge multiplicity (<= 2 faces)
///    and reject duplicate faces (equal up to rotation or reversal)
///  * propagate winding constraints through each edge-connected component,
///    keeping the smallest source face of each component in its original
///    orientation
///  * split vertices that carry several disconnected face fans into one
///    output vertex per fan; unreferenced source positions survive as isolated
///    output vertices
///
/// @throws std::invalid_argument on any of the validation failures above or
///         when an edge-connected component has contradictory winding.
repaired_soup repair_face_soup(std::vector<face> const& faces, int vertex_count);
} // namespace soup
} // namespace polymesh
