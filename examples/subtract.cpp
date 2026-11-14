#if 0
c++ -std=c++11 -O1 -g -I. examples/subtract.cpp solid_difference.cpp trimesh.cpp -o subtract
./subtract subtract_output.obj
#endif

// Example: subtract one closed triangular solid from another and write the
// resulting closed boundary (including cut skin and per-corner UVs) as OBJ.
//
// Supported inputs: finite, oriented closed triangle 2-manifolds whose
// boundaries are either disjoint or meet transversely.  Tangent contact,
// intersection lines through original vertices, edge/edge crossings or
// overlaps, and coplanar triangles in the contact region are rejected with
// std::domain_error; malformed topology is rejected with
// std::invalid_argument.  No voxelization or third-party boolean library is
// involved; every output triangle lies on an input triangle and carries its
// provenance and interpolated UV.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "solid_difference.h"

using namespace trimesh;

namespace
{

typedef std::array< double, 3 > P3;

static triangle_t tri( long a, long b, long c )
{
    triangle_t t;
    t.v[0] = a;
    t.v[1] = b;
    t.v[2] = c;
    return t;
}

static SolidMesh make_box( double x0, double x1, double y0, double y1,
                           double z0, double z1 )
{
    SolidMesh m;
    const P3 p[8] =
    {
        {{ x0, y0, z0 }}, {{ x1, y0, z0 }}, {{ x1, y1, z0 }}, {{ x0, y1, z0 }},
        {{ x0, y0, z1 }}, {{ x1, y0, z1 }}, {{ x1, y1, z1 }}, {{ x0, y1, z1 }}
    };
    for( int k = 0; k < 8; ++k ) m.positions.push_back( p[k] );
    const long idx[12][3] =
    {
        { 0, 3, 2 }, { 0, 2, 1 },
        { 4, 5, 6 }, { 4, 6, 7 },
        { 0, 1, 5 }, { 0, 5, 4 },
        { 2, 3, 7 }, { 2, 7, 6 },
        { 3, 0, 4 }, { 3, 4, 7 },
        { 1, 2, 6 }, { 1, 6, 5 }
    };
    for( int k = 0; k < 12; ++k )
    {
        m.triangles.push_back( tri( idx[k][0], idx[k][1], idx[k][2] ) );
    }
    // Planar per-face UVs; seams are intentional and preserved by the cut.
    m.corner_uv.resize( 3 * m.triangles.size() );
    for( std::size_t f = 0; f < m.triangles.size(); ++f )
    {
        for( int k = 0; k < 3; ++k )
        {
            const P3& q = m.positions[
                static_cast< std::size_t >( m.triangles[f].v[k] ) ];
            m.corner_uv[ 3 * f + k ] = {{ 0.25 * ( q[0] + 2.0 ),
                                          0.25 * ( q[1] + q[2] + 4.0 ) }};
        }
    }
    return m;
}

static bool write_obj( const SolidMesh& m, const std::string& path )
{
    std::ofstream out( path.c_str() );
    if( !out ) return false;
    for( std::size_t i = 0; i < m.positions.size(); ++i )
    {
        out << "v " << m.positions[i][0] << ' ' << m.positions[i][1]
            << ' ' << m.positions[i][2] << '\n';
    }
    for( std::size_t i = 0; i < m.corner_uv.size(); ++i )
    {
        out << "vt " << m.corner_uv[i][0] << ' ' << m.corner_uv[i][1] << '\n';
    }
    for( std::size_t f = 0; f < m.triangles.size(); ++f )
    {
        out << "f";
        for( int k = 0; k < 3; ++k )
        {
            const long v = m.triangles[f].v[k] + 1;       // OBJ is 1-based
            const long vt = static_cast< long >( 3 * f + k ) + 1;
            out << ' ' << v << '/' << vt;
        }
        out << '\n';
    }
    return static_cast< bool >( out );
}

} // namespace

int main( int argc, char** argv )
{
    const std::string path = ( argc > 1 ) ? argv[1] : "subtract_output.obj";

    // A block with a transverse rectangular tunnel punched through it.
    SolidMesh a = make_box( -2.0, 2.0, -2.0, 2.0, -2.0, 2.0 );
    SolidMesh b = make_box( -3.0, 3.0, -0.6, 0.2, -0.3, 0.5 );

    DifferenceResult result;
    try
    {
        subtract_solid( a, b, result );
    }
    catch( const std::exception& e )
    {
        std::cerr << "subtract_solid failed: " << e.what() << '\n';
        return 1;
    }

    if( !write_obj( result.mesh, path ) )
    {
        std::cerr << "could not open " << path << " for writing\n";
        return 1;
    }

    std::size_t from_a = 0;
    std::size_t from_b = 0;
    for( std::size_t f = 0; f < result.origins.size(); ++f )
    {
        if( result.origins[f].operand == 0 ) ++from_a;
        else ++from_b;
    }
    std::cout << "wrote " << result.mesh.triangles.size()
              << " triangles (" << from_a << " outer skin from A, "
              << from_b << " reversed cut skin from B) and "
              << result.mesh.positions.size() << " vertices to " << path
              << '\n';
    return 0;
}
