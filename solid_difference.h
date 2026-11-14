#ifndef SOLID_DIFFERENCE_H
#define SOLID_DIFFERENCE_H

#include "trimesh_types.h"

#include <array>
#include <cstddef>
#include <vector>

namespace trimesh
{

// An oriented, closed triangular boundary with per-corner UVs.
// 'corner_uv' is indexed as 3 * face_index + local_corner_index.
struct SolidMesh
{
    std::vector< std::array< double, 3 > > positions;
    std::vector< triangle_t > triangles;
    std::vector< std::array< double, 2 > > corner_uv;

    void clear()
    {
        positions.clear();
        triangles.clear();
        corner_uv.clear();
    }
};

// Where an output triangle came from.
// 'operand' is 0 for mesh A, 1 for mesh B.
// 'face' is the source triangle index.
// 'bary[c]' are the barycentric coordinates (relative to the source triangle,
// in its original corner order) of output corner c.
struct FaceOrigin
{
    int operand;
    std::size_t face;
    std::array< std::array< double, 3 >, 3 > bary;

    FaceOrigin() : operand( -1 ), face( 0 )
    {
        for( int c = 0; c < 3; ++c )
        {
            bary[c][0] = bary[c][1] = bary[c][2] = 0.0;
        }
    }
};

struct DifferenceResult
{
    SolidMesh mesh;
    std::vector< FaceOrigin > origins;

    void clear()
    {
        mesh.clear();
        origins.clear();
    }
};

// Builds the closed triangular boundary of (a minus b), including through
// holes, enclosed cavities and disconnected remnants.  The result owns its
// storage; the inputs are read only.  On failure the exception is thrown and
// 'result' is left untouched.
//
// Throws std::invalid_argument for malformed input topology, non-finite
// coordinates/UVs or degenerate faces.
// Throws std::domain_error for unsupported contact (tangency, intersections
// through original vertices, edge/edge crossings or overlaps, coplanar
// triangles in the contact region, near-degenerate intersection geometry).
void subtract_solid( const SolidMesh& a, const SolidMesh& b, DifferenceResult& result );

} // namespace trimesh

#endif // SOLID_DIFFERENCE_H
