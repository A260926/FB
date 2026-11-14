#include "solid_difference.h"
#include "trimesh.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace trimesh
{

namespace
{

typedef std::array< double, 3 > P3;
typedef std::array< double, 2 > P2;

// ---------------------------------------------------------------------------
// numerical tolerances
// ---------------------------------------------------------------------------

static const double EPS_FINITE = 1e-10;      // signed-distance sign decisions
static const double EPS_BARY = 1e-9;         // event must be strictly inside face
static const double EPS_2D = 1e-12;          // planar meshing predicates
static const double MIN_SIN_ANGLE = 1e-4;    // transverse face normals
static const double RAY_EPS = 1e-12;

// ---------------------------------------------------------------------------
// small vector helpers
// ---------------------------------------------------------------------------

static P3 vsub( const P3& a, const P3& b )
{
    P3 r = {{ a[0] - b[0], a[1] - b[1], a[2] - b[2] }};
    return r;
}
static P3 vadd( const P3& a, const P3& b )
{
    P3 r = {{ a[0] + b[0], a[1] + b[1], a[2] + b[2] }};
    return r;
}
static P3 vscale( const P3& a, double s )
{
    P3 r = {{ a[0] * s, a[1] * s, a[2] * s }};
    return r;
}
static double vdot( const P3& a, const P3& b )
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
static P3 vcross( const P3& a, const P3& b )
{
    P3 r = {{
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0] }};
    return r;
}
static double vlen( const P3& a ) { return std::sqrt( vdot( a, a ) ); }

static P3 bary_point( const P3& a, const P3& b, const P3& c,
                      const std::array< double, 3 >& w )
{
    P3 r = vadd( vadd( vscale( a, w[0] ), vscale( b, w[1] ) ), vscale( c, w[2] ) );
    return r;
}

static bool finite3( const P3& p )
{
    return std::isfinite( p[0] ) && std::isfinite( p[1] ) && std::isfinite( p[2] );
}
static bool finite2( const P2& p )
{
    return std::isfinite( p[0] ) && std::isfinite( p[1] );
}

// ---------------------------------------------------------------------------
// validated mesh view
// ---------------------------------------------------------------------------

struct MeshData
{
    const SolidMesh* src;
    std::size_t nV;
    std::size_t nF;
    // directed vertex pair -> (face index, local edge index 0..2)
    std::map< std::pair< long, long >, std::pair< std::size_t, int > > dir_face;

    const P3& pos( std::size_t v ) const { return src->positions[v]; }
};

static void validate_mesh( const SolidMesh& m, const char* tag )
{
    const std::size_t nV = m.positions.size();
    const std::size_t nF = m.triangles.size();

    if( nF == 0 )
    {
        // An empty mesh carries no isolated points and no UVs.
        if( !m.positions.empty() || !m.corner_uv.empty() )
        {
            throw std::invalid_argument( std::string( tag ) + ": empty mesh must have no points or uv" );
        }
        return;
    }
    if( m.corner_uv.size() != 3 * nF )
    {
        throw std::invalid_argument( std::string( tag ) + ": uv count must be three times face count" );
    }
    for( std::size_t i = 0; i < nV; ++i )
    {
        if( !finite3( m.positions[i] ) )
        {
            throw std::invalid_argument( std::string( tag ) + ": non-finite position" );
        }
        for( int k = 0; k < 3; ++k )
        {
            if( std::fabs( m.positions[i][k] ) > 100.0 + 1e-12 )
            {
                throw std::invalid_argument( std::string( tag ) + ": position out of supported range" );
            }
        }
    }
    for( std::size_t i = 0; i < m.corner_uv.size(); ++i )
    {
        if( !finite2( m.corner_uv[i] ) )
        {
            throw std::invalid_argument( std::string( tag ) + ": non-finite uv" );
        }
        if( std::fabs( m.corner_uv[i][0] ) > 100.0 + 1e-12 ||
            std::fabs( m.corner_uv[i][1] ) > 100.0 + 1e-12 )
        {
            throw std::invalid_argument( std::string( tag ) + ": uv out of supported range" );
        }
    }

    std::vector< bool > used( nV, false );
    std::set< std::set< long > > face_sets;
    typedef std::map< std::pair< long, long >, std::pair< std::size_t, int > > dir_map_t;
    dir_map_t dir_face;

    for( std::size_t f = 0; f < nF; ++f )
    {
        const triangle_t& t = m.triangles[f];
        for( int k = 0; k < 3; ++k )
        {
            if( t.v[k] < 0 || static_cast< std::size_t >( t.v[k] ) >= nV )
            {
                throw std::invalid_argument( std::string( tag ) + ": triangle index out of range" );
            }
        }
        if( t.v[0] == t.v[1] || t.v[1] == t.v[2] || t.v[2] == t.v[0] )
        {
            throw std::invalid_argument( std::string( tag ) + ": degenerate repeated-index face" );
        }
        std::set< long > fs;
        fs.insert( t.v[0] ); fs.insert( t.v[1] ); fs.insert( t.v[2] );
        if( !face_sets.insert( fs ).second )
        {
            throw std::invalid_argument( std::string( tag ) + ": duplicate face" );
        }
        for( int k = 0; k < 3; ++k )
        {
            used[ t.v[k] ] = true;
            const long a = t.v[k];
            const long b = t.v[ ( k + 1 ) % 3 ];
            const std::pair< long, long > key = std::make_pair( a, b );
            if( dir_face.count( key ) )
            {
                throw std::invalid_argument( std::string( tag ) + ": directed edge shared by two faces" );
            }
            dir_face[ key ] = std::make_pair( f, k );
        }
    }
    for( std::size_t v = 0; v < nV; ++v )
    {
        if( !used[v] )
        {
            throw std::invalid_argument( std::string( tag ) + ": isolated vertex" );
        }
    }

    // Every undirected edge must appear exactly twice, with opposite directions.
    std::set< std::pair< long, long > > seen_undirected;
    for( dir_map_t::const_iterator it = dir_face.begin(); it != dir_face.end(); ++it )
    {
        const long a = it->first.first;
        const long b = it->first.second;
        const std::pair< long, long > rev = std::make_pair( b, a );
        const std::pair< long, long > canon = std::make_pair( std::min( a, b ), std::max( a, b ) );
        if( !seen_undirected.insert( canon ).second ) continue;
        if( !dir_face.count( rev ) )
        {
            throw std::invalid_argument( std::string( tag ) + ": boundary or non-manifold edge" );
        }
    }

    // Geometric quality checks.
    for( std::size_t f = 0; f < nF; ++f )
    {
        const triangle_t& t = m.triangles[f];
        const P3& p0 = m.positions[ t.v[0] ];
        const P3& p1 = m.positions[ t.v[1] ];
        const P3& p2 = m.positions[ t.v[2] ];
        const P3 e01 = vsub( p1, p0 );
        const P3 e12 = vsub( p2, p1 );
        const P3 e20 = vsub( p0, p2 );
        const double l01 = vlen( e01 );
        const double l12 = vlen( e12 );
        const double l20 = vlen( e20 );
        if( l01 < 1e-3 - 1e-15 || l12 < 1e-3 - 1e-15 || l20 < 1e-3 - 1e-15 )
        {
            throw std::invalid_argument( std::string( tag ) + ": edge shorter than 1e-3" );
        }
        if( l01 > 100.0 + 1e-12 || l12 > 100.0 + 1e-12 || l20 > 100.0 + 1e-12 )
        {
            throw std::invalid_argument( std::string( tag ) + ": edge longer than 100" );
        }
        const P3 n = vcross( e01, vsub( p2, p0 ) );
        const double area2 = vlen( n );
        if( area2 <= 0.0 || !std::isfinite( area2 ) )
        {
            throw std::invalid_argument( std::string( tag ) + ": zero-area face" );
        }
        const double longest = std::max( l01, std::max( l12, l20 ) );
        const double shortest_altitude = area2 / longest; // 2*area / longest edge
        if( longest / shortest_altitude > 1000.0 + 1e-9 )
        {
            throw std::invalid_argument( std::string( tag ) + ": face aspect ratio exceeds 1000" );
        }
    }

    // The mesh is a closed edge-manifold at this point; build the real
    // half-edge structure and verify that every vertex is a single fan
    // (rejects shells glued only at a vertex).
    std::vector< edge_t > edges;
    unordered_edges_from_triangles( static_cast< unsigned long >( nF ), &m.triangles[0], edges );
    trimesh_t he;
    he.build( static_cast< unsigned long >( nV ), static_cast< unsigned long >( nF ),
              &m.triangles[0], static_cast< unsigned long >( edges.size() ), &edges[0] );
    if( !he.boundary_edges().empty() || !he.boundary_vertices().empty() )
    {
        throw std::invalid_argument( std::string( tag ) + ": mesh is not closed" );
    }
    std::vector< long > incident( nV, 0 );
    for( dir_map_t::const_iterator it = dir_face.begin(); it != dir_face.end(); ++it )
    {
        ++incident[ static_cast< std::size_t >( it->first.second ) ];
    }
    for( std::size_t v = 0; v < nV; ++v )
    {
        std::vector< trimesh_t::index_t > faces;
        he.vertex_face_neighbors( static_cast< trimesh_t::index_t >( v ), faces );
        if( static_cast< long >( faces.size() ) != incident[v] )
        {
            throw std::invalid_argument( std::string( tag ) + ": vertex has more than one face fan" );
        }
        // Walk the fan explicitly and require it to close on itself.
        const std::vector< trimesh_t::index_t > neigh =
            he.vertex_vertex_neighbors( static_cast< trimesh_t::index_t >( v ) );
        if( neigh.empty() )
        {
            throw std::invalid_argument( std::string( tag ) + ": vertex has no neighbors" );
        }
        long walked = 0;
        trimesh_t::index_t hei = he.directed_edge2he_index(
            static_cast< trimesh_t::index_t >( v ), neigh[0] );
        const trimesh_t::index_t first = hei;
        do
        {
            if( he.halfedge( hei ).face < 0 )
            {
                throw std::invalid_argument( std::string( tag ) + ": boundary encountered in fan" );
            }
            ++walked;
            hei = he.halfedge( he.halfedge( hei ).opposite_he ).next_he;
        } while( hei != first && walked <= incident[v] + 1 );
        if( hei != first || walked != incident[v] )
        {
            throw std::invalid_argument( std::string( tag ) + ": vertex fan does not close" );
        }
    }
}

// ---------------------------------------------------------------------------
// point-in-solid by parity ray casting (several deterministic directions,
// the first direction with no vertex/edge hits wins)
// ---------------------------------------------------------------------------

static bool ray_parity( const SolidMesh& m, const P3& p, const P3& dir )
{
    long crossings = 0;
    bool degenerate = false;
    for( std::size_t f = 0; f < m.triangles.size(); ++f )
    {
        const triangle_t& t = m.triangles[f];
        const P3& a = m.positions[ t.v[0] ];
        const P3& b = m.positions[ t.v[1] ];
        const P3& c = m.positions[ t.v[2] ];
        const P3 e1 = vsub( b, a );
        const P3 e2 = vsub( c, a );
        const P3 pvec = vcross( dir, e2 );
        const double det = vdot( e1, pvec );
        if( std::fabs( det ) < 1e-18 ) continue;
        const P3 tvec = vsub( p, a );
        const double u = vdot( tvec, pvec ) / det;
        const P3 qvec = vcross( tvec, e1 );
        const double v = vdot( dir, qvec ) / det;
        if( u < -EPS_2D || u > 1.0 + EPS_2D || v < -EPS_2D || u + v > 1.0 + EPS_2D ) continue;
        const double tt = vdot( e2, qvec ) / det;
        if( tt <= RAY_EPS ) continue;
        // A barycentric coordinate on (or numerically on) zero means the ray
        // runs through an edge or vertex: the parity for this direction is
        // unreliable, so reject the whole direction and retry another one.
        if( std::fabs( u ) <= EPS_2D || std::fabs( v ) <= EPS_2D ||
            std::fabs( 1.0 - u - v ) <= EPS_2D )
        {
            degenerate = true;
        }
        ++crossings;
    }
    if( degenerate ) throw std::logic_error( "degenerate ray" );
    return ( crossings & 1L ) != 0;
}

static bool point_inside( const SolidMesh& m, const P3& p )
{
    if( m.triangles.empty() ) return false;
    static const double coeffs[7][3] =
    {
        { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 }, { 0.0, 0.0, 1.0 },
        { 0.57735026918962576, 0.57735026918962576, 0.57735026918962576 },
        { 0.81649658092772603, 0.40824829046386302, -0.40824829046386302 },
        { 0.30151134457776363, -0.90453403373329089, 0.30151134457776363 },
        { -0.45425681220661135, 0.77123648075123929, 0.44609410389494574 }
    };
    for( int k = 0; k < 7; ++k )
    {
        P3 d = {{ coeffs[k][0], coeffs[k][1], coeffs[k][2] }};
        const double l = vlen( d );
        d = vscale( d, 1.0 / l );
        try
        {
            return ray_parity( m, p, d );
        }
        catch( const std::logic_error& )
        {
            // ray grazed a vertex or edge: try the next direction
        }
    }
    throw std::domain_error( "could not classify point: all probe rays degenerate" );
}

// ---------------------------------------------------------------------------
// intersection events
// ---------------------------------------------------------------------------

struct Event
{
    int edge_mesh;                 // mesh owning the crossing edge
    long ev0, ev1;                 // edge endpoints, canonical (ev0 < ev1)
    std::size_t other_face;        // face pierced in the other mesh
    double s;                      // parameter along ev0 -> ev1
    std::array< double, 3 > bary;  // barycentric coords inside other_face
    P3 pos;
};

struct Mark
{
    enum Kind { CORNER, EDGE_EVENT, INTERIOR_EVENT } kind;
    int corner;                    // source face corner, for CORNER
    long event;                    // event id, or -1
    std::array< double, 3 > bary;  // barycentric coords within the face
    P2 q;                         // planar coordinates within the face
};

// Barycentric coordinates of x within the plane of (a,b,c).  The result is
// a least-squares fit, which is exact when x is on the plane.
static void bary_in_triangle( const P3& x, const P3& a, const P3& b, const P3& c,
                              std::array< double, 3 >& w )
{
    const P3 v0 = vsub( b, a );
    const P3 v1 = vsub( c, a );
    const P3 v2 = vsub( x, a );
    const double dot00 = vdot( v0, v0 );
    const double dot01 = vdot( v0, v1 );
    const double dot11 = vdot( v1, v1 );
    const double dot20 = vdot( v2, v0 );
    const double dot21 = vdot( v2, v1 );
    const double denom = dot00 * dot11 - dot01 * dot01;
    const double w1 = ( dot11 * dot20 - dot01 * dot21 ) / denom;
    const double w2 = ( dot00 * dot21 - dot01 * dot20 ) / denom;
    w[1] = w1;
    w[2] = w2;
    w[0] = 1.0 - w1 - w2;
}

// True when coplanar segment (p,q) touches the interior or boundary of
// triangle (a,b,c).
static bool coplanar_segment_hits_triangle( const P3& p, const P3& q,
                                            const P3& a, const P3& b, const P3& c )
{
    const P3 e1r = vsub( b, a );
    const P3 nraw = vcross( e1r, vsub( c, a ) );
    const double nlen = vlen( nraw );
    if( nlen <= 0.0 ) return false;
    const P3 nhat = vscale( nraw, 1.0 / nlen );
    const P3 uax = vscale( e1r, 1.0 / vlen( e1r ) );
    const P3 vax = vcross( nhat, uax );
    struct LP { double x, y; };
    LP tp[2], tri[3];
    const P3 src[2] = { p, q };
    for( int k = 0; k < 2; ++k )
    {
        const P3 d = vsub( src[k], a );
        tp[k].x = vdot( d, uax );
        tp[k].y = vdot( d, vax );
    }
    const P3 cs[3] = { a, b, c };
    for( int k = 0; k < 3; ++k )
    {
        const P3 d = vsub( cs[k], a );
        tri[k].x = vdot( d, uax );
        tri[k].y = vdot( d, vax );
    }
    const double tol = 1e-10;
    auto inside = [&]( const LP& z ) -> bool
    {
        for( int k = 0; k < 3; ++k )
        {
            const LP& r0 = tri[k];
            const LP& r1 = tri[ ( k + 1 ) % 3 ];
            const double cross = ( r1.x - r0.x ) * ( z.y - r0.y ) -
                                 ( r1.y - r0.y ) * ( z.x - r0.x );
            if( cross < -tol ) return false;
        }
        return true;
    };
    if( inside( tp[0] ) || inside( tp[1] ) ) return true;
    auto cross2 = []( const LP& a0, const LP& b0, const LP& c0 ) -> double
    {
        return ( b0.x - a0.x ) * ( c0.y - a0.y ) - ( b0.y - a0.y ) * ( c0.x - a0.x );
    };
    auto on_segment = [&]( const LP& z, const LP& a0, const LP& b0 ) -> bool
    {
        if( std::fabs( cross2( a0, b0, z ) ) > tol ) return false;
        return std::fabs( z.x - a0.x ) <= std::fabs( b0.x - a0.x ) + tol &&
               std::fabs( z.y - a0.y ) <= std::fabs( b0.y - a0.y ) + tol;
    };
    for( int k = 0; k < 3; ++k )
    {
        const LP& r0 = tri[k];
        const LP& r1 = tri[ ( k + 1 ) % 3 ];
        if( on_segment( r0, tp[0], tp[1] ) ) return true;
        const double e = cross2( tp[0], tp[1], r0 );
        const double f = cross2( tp[0], tp[1], r1 );
        const double g = cross2( r0, r1, tp[0] );
        const double h = cross2( r0, r1, tp[1] );
        if( ( ( e > tol && f < -tol ) || ( e < -tol && f > tol ) ) &&
            ( ( g > tol && h < -tol ) || ( g < -tol && h > tol ) ) )
        {
            return true;
        }
    }
    return false;
}

// Strict transverse crossing of edge (p,q) from mesh em with triangle
// (a,b,c) from the other mesh.  Tangent or grazing contact is rejected.
static bool edge_triangle_cross( const P3& p, const P3& q,
                                 const P3& a, const P3& b, const P3& c,
                                 std::array< double, 3 >& bary_out,
                                 double& t_out )
{
    const P3 n = vcross( vsub( b, a ), vsub( c, a ) );
    const double nlen = vlen( n );
    if( nlen <= 0.0 ) return false; // validated elsewhere, stay defensive
    const double d0 = vdot( n, vsub( p, a ) ) / nlen;
    const double d1 = vdot( n, vsub( q, a ) ) / nlen;
    if( std::fabs( d0 ) <= EPS_FINITE && std::fabs( d1 ) <= EPS_FINITE )
    {
        // Coplanar edge: only reject it when it actually meets the triangle.
        if( coplanar_segment_hits_triangle( p, q, a, b, c ) )
        {
            throw std::domain_error( "coplanar edge/face contact" );
        }
        return false;
    }
    if( std::fabs( d0 ) <= EPS_FINITE || std::fabs( d1 ) <= EPS_FINITE )
    {
        // One endpoint lies on the plane: a vertex-on-face contact if it is
        // inside the triangle, otherwise the edge simply misses.
        const P3& onp = ( std::fabs( d0 ) <= EPS_FINITE ) ? p : q;
        std::array< double, 3 > w;
        bary_in_triangle( onp, a, b, c, w );
        if( w[0] >= -EPS_BARY && w[1] >= -EPS_BARY && w[2] >= -EPS_BARY )
        {
            throw std::domain_error( "intersection through an original vertex" );
        }
        return false;
    }
    if( d0 * d1 > 0.0 ) return false; // same side, no crossing
    const double t = d0 / ( d0 - d1 );
    const P3 x = vadd( p, vscale( vsub( q, p ), t ) );

    bary_in_triangle( x, a, b, c, bary_out );
    if( bary_out[0] < -EPS_BARY || bary_out[1] < -EPS_BARY || bary_out[2] < -EPS_BARY )
    {
        return false; // plane crossing is strictly outside the triangle
    }
    if( bary_out[0] <= EPS_BARY || bary_out[1] <= EPS_BARY || bary_out[2] <= EPS_BARY )
    {
        // Inside but on (or numerically on) a triangle edge: edge/edge contact.
        throw std::domain_error( "intersection grazing a triangle edge or vertex" );
    }
    const double sum = bary_out[0] + bary_out[1] + bary_out[2];
    bary_out[0] /= sum;
    bary_out[1] /= sum;
    bary_out[2] /= sum;
    t_out = t;
    return true;
}

static P3 face_normal( const SolidMesh& m, std::size_t f )
{
    const triangle_t& t = m.triangles[f];
    const P3 n = vcross( vsub( m.positions[ t.v[1] ], m.positions[ t.v[0] ] ),
                         vsub( m.positions[ t.v[2] ], m.positions[ t.v[0] ] ) );
    return vscale( n, 1.0 / vlen( n ) );
}

// ---------------------------------------------------------------------------
// constrained planar triangulation of one source face
// ---------------------------------------------------------------------------

struct PlanarMesher
{
    // Planar straight-line graph built from the split source-triangle
    // boundary plus the real intersection segments, then turned into a
    // conforming triangulation by minimal-cycle extraction and ear clipping.
    std::vector< P2 > pts;

    struct HE
    {
        int to;
        int twin;
        HE() : to( -1 ), twin( -1 ) {}
    };
    std::vector< HE > he;
    std::map< std::pair< int, int >, int > he_index; // directed (from,to) -> he

    static double orient2( const P2& a, const P2& b, const P2& c )
    {
        return ( b[0] - a[0] ) * ( c[1] - a[1] ) - ( b[1] - a[1] ) * ( c[0] - a[0] );
    }

    int add_edge( int a, int b )
    {
        const std::pair< int, int > key = std::make_pair( a, b );
        std::map< std::pair< int, int >, int >::const_iterator it = he_index.find( key );
        if( it != he_index.end() ) return it->second;
        const int hab = static_cast< int >( he.size() );
        he.push_back( HE() );
        const int hba = static_cast< int >( he.size() );
        he.push_back( HE() );
        he[hab].to = b; he[hab].twin = hba;
        he[hba].to = a; he[hba].twin = hab;
        he_index[ std::make_pair( a, b ) ] = hab;
        he_index[ std::make_pair( b, a ) ] = hba;
        return hab;
    }
    // All minimal edge cycles of the embedded graph, using an explicit
    // rotation system.  Outgoing halfedges are sorted by polar angle at every
    // vertex; arriving on h = (u->v), the face on the left continues along
    // the outgoing edge immediately clockwise from h's twin.  This stays
    // well defined when consecutive edges are collinear.  Bounded faces come
    // out counter-clockwise, exterior sides clockwise.
    std::vector< std::vector< int > > build_cycles() const
    {
        std::map< int, std::vector< int > > outgoing;
        for( std::map< std::pair< int, int >, int >::const_iterator it = he_index.begin();
             it != he_index.end(); ++it )
        {
            outgoing[ it->first.first ].push_back( it->second );
        }
        // Rotation table: for every directed halfedge, the next halfedge of
        // the face lying on its left.
        std::map< int, int > left_next;
        for( std::map< int, std::vector< int > >::iterator it = outgoing.begin();
             it != outgoing.end(); ++it )
        {
            const int v = it->first;
            std::vector< int >& outs = it->second;
            std::sort( outs.begin(), outs.end(),
                [&]( int ha, int hb )
                {
                    const P2& pa = pts[ static_cast< std::size_t >( he[ha].to ) ];
                    const P2& pb = pts[ static_cast< std::size_t >( he[hb].to ) ];
                    return std::atan2( pa[1] - pts[ static_cast< std::size_t >( v ) ][1],
                                       pa[0] - pts[ static_cast< std::size_t >( v ) ][0] ) <
                           std::atan2( pb[1] - pts[ static_cast< std::size_t >( v ) ][1],
                                       pb[0] - pts[ static_cast< std::size_t >( v ) ][0] );
                } );
            const std::size_t k = outs.size();
            for( std::size_t i = 0; i < k; ++i )
            {
                // An incoming edge's twin is outs[i]; the clockwise neighbor
                // outs[(i-1+k)%k] continues the left face.
                const int twin_h = outs[i];
                const int incoming = he[twin_h].twin;
                left_next[ incoming ] = outs[ ( i + k - 1 ) % k ];
            }
        }

        std::vector< char > visited( he.size(), 0 );
        std::vector< std::vector< int > > cycles;
        for( std::size_t start = 0; start < he.size(); ++start )
        {
            if( visited[start] ) continue;
            std::vector< int > verts;
            int cur = static_cast< int >( start );
            int guard = 0;
            do
            {
                visited[cur] = 1;
                const int from = he[ he[cur].twin ].to;
                verts.push_back( from );
                cur = left_next[ cur ];
                if( ++guard > static_cast< int >( he.size() ) + 2 )
                {
                    throw std::runtime_error( "planar cycle walk did not close" );
                }
            } while( cur != static_cast< int >( start ) );
            cycles.push_back( verts );
        }
        return cycles;
    }

    static double signed_area2( const std::vector< int >& poly, const std::vector< P2 >& p )
    {
        double a = 0.0;
        for( std::size_t i = 0; i < poly.size(); ++i )
        {
            const P2& u = p[ poly[i] ];
            const P2& v = p[ poly[ ( i + 1 ) % poly.size() ] ];
            a += u[0] * v[1] - v[0] * u[1];
        }
        return a;
    }

    static bool segments_cross_strict( const P2& a, const P2& b, const P2& c, const P2& d )
    {
        const double e = orient2( a, b, c );
        const double f = orient2( a, b, d );
        const double g = orient2( c, d, a );
        const double h = orient2( c, d, b );
        return ( ( e > EPS_2D && f < -EPS_2D ) || ( e < -EPS_2D && f > EPS_2D ) ) &&
               ( ( g > EPS_2D && h < -EPS_2D ) || ( g < -EPS_2D && h > EPS_2D ) );
    }

    // Connect disconnected graph components (e.g. a closed intersection loop
    // floating inside the source triangle) with non-crossing bridges.  Both
    // endpoints are existing vertices, so no Steiner points are introduced.
    void connect_components()
    {
        // Every successful bridge merges two components, so no more than
        // pts.size() rounds can ever be needed.
        for( std::size_t round = 0; round < pts.size() + 1; ++round )
        {
            // Label connected components through existing half edges.
            std::vector< int > comp( pts.size(), -1 );
            int ncomp = 0;
            for( std::size_t s = 0; s < pts.size(); ++s )
            {
                if( comp[s] >= 0 ) continue;
                std::vector< int > stack( 1, static_cast< int >( s ) );
                comp[s] = ncomp;
                while( !stack.empty() )
                {
                    const int x = stack.back();
                    stack.pop_back();
                    for( std::map< std::pair< int, int >, int >::const_iterator it = he_index.begin();
                         it != he_index.end(); ++it )
                    {
                        int y = -1;
                        if( it->first.first == x ) y = it->first.second;
                        else if( it->first.second == x ) y = it->first.first;
                        if( y >= 0 && comp[ static_cast< std::size_t >( y ) ] < 0 )
                        {
                            comp[ static_cast< std::size_t >( y ) ] = ncomp;
                            stack.push_back( y );
                        }
                    }
                }
                ++ncomp;
            }
            if( ncomp <= 1 ) return;

            bool bridged = false;
            for( std::size_t a = 0; a < pts.size() && !bridged; ++a )
            {
                for( std::size_t b = a + 1; b < pts.size() && !bridged; ++b )
                {
                    if( comp[a] == comp[b] ) continue;
                    bool clear = true;
                    for( std::map< std::pair< int, int >, int >::const_iterator it = he_index.begin();
                         it != he_index.end() && clear; ++it )
                    {
                        const int c = it->first.first;
                        const int d = it->first.second;
                        if( static_cast< std::size_t >( c ) == a || static_cast< std::size_t >( c ) == b ||
                            static_cast< std::size_t >( d ) == a || static_cast< std::size_t >( d ) == b )
                        {
                            continue;
                        }
                        if( segments_cross_strict( pts[a], pts[b], pts[c], pts[d] ) ) clear = false;
                    }
                    if( clear )
                    {
                        add_edge( static_cast< int >( a ), static_cast< int >( b ) );
                        bridged = true;
                    }
                }
            }
            if( !bridged ) throw std::runtime_error( "could not connect planar components" );
        }
        throw std::runtime_error( "planar component bridging did not converge" );
    }

    static bool point_in_tri_or_on_edge( const P2& x, const P2& a, const P2& b, const P2& c )
    {
        const double s0 = orient2( a, b, x );
        const double s1 = orient2( b, c, x );
        const double s2 = orient2( c, a, x );
        return s0 >= -EPS_2D && s1 >= -EPS_2D && s2 >= -EPS_2D;
    }

    void ear_clip( std::vector< int > poly, std::vector< std::array< int, 3 > >& out ) const
    {
        if( poly.size() < 3 ) throw std::runtime_error( "empty planar polygon" );
        // A simple polygon always has at least one ear; each clip removes one
        // vertex, so at most poly.size() iterations are required.
        std::size_t guard = 0;
        const std::size_t guard_limit = poly.size() + 1;
        while( poly.size() > 3 )
        {
            bool clipped = false;
            const std::size_t n = poly.size();
            for( std::size_t i = 0; i < n; ++i )
            {
                const int a = poly[ ( i + n - 1 ) % n ];
                const int b = poly[i];
                const int c = poly[ ( i + 1 ) % n ];
                if( orient2( pts[a], pts[b], pts[c] ) <= EPS_2D ) continue;
                bool blocked = false;
                for( std::size_t j = 0; j < n; ++j )
                {
                    const int q = poly[j];
                    if( q == a || q == b || q == c ) continue;
                    // Reject also when q sits on an ear edge: a diagonal must
                    // never jump over a collinear constraint vertex.
                    if( point_in_tri_or_on_edge( pts[q], pts[a], pts[b], pts[c] ) )
                    {
                        blocked = true;
                        break;
                    }
                }
                if( blocked ) continue;
                std::array< int, 3 > t = {{ a, b, c }};
                out.push_back( t );
                poly.erase( poly.begin() + static_cast< std::ptrdiff_t >( i ) );
                clipped = true;
                break;
            }
            if( !clipped || ++guard > guard_limit )
            {
                throw std::runtime_error( "planar ear clipping failed" );
            }
        }
        if( orient2( pts[poly[0]], pts[poly[1]], pts[poly[2]] ) > EPS_2D )
        {
            std::array< int, 3 > t = {{ poly[0], poly[1], poly[2] }};
            out.push_back( t );
        }
    }

    void triangulate( std::vector< std::array< int, 3 > >& out )
    {
        connect_components();
        std::vector< std::vector< int > > cycles = build_cycles();
        // Every positive-area (counter-clockwise) minimal cycle is a bounded
        // face; clockwise cycles are exterior sides and are discarded.
        for( std::size_t i = 0; i < cycles.size(); ++i )
        {
            if( signed_area2( cycles[i], pts ) > EPS_2D )
            {
                ear_clip( cycles[i], out );
            }
        }
        if( out.empty() ) throw std::runtime_error( "planar triangulation produced no faces" );
    }
};

// ---------------------------------------------------------------------------
// per-face working data
// ---------------------------------------------------------------------------

struct FaceWork
{
    std::vector< Mark > marks;
    std::vector< std::pair< int, int > > constraints;
    P3 e1, e2, origin;
};

static int find_mark( const FaceWork& fw, long event )
{
    for( std::size_t i = 0; i < fw.marks.size(); ++i )
    {
        if( fw.marks[i].event == event ) return static_cast< int >( i );
    }
    return -1;
}

// ---------------------------------------------------------------------------
// main driver
// ---------------------------------------------------------------------------

} // anonymous namespace

void subtract_solid( const SolidMesh& a, const SolidMesh& b, DifferenceResult& result )
{
    validate_mesh( a, "operand A" );
    validate_mesh( b, "operand B" );

    const SolidMesh* meshes[2] = { &a, &b };
    MeshData md[2];
    for( int m = 0; m < 2; ++m )
    {
        md[m].src = meshes[m];
        md[m].nV = meshes[m]->positions.size();
        md[m].nF = meshes[m]->triangles.size();
        if( md[m].nF > 0 )
        {
            for( std::size_t f = 0; f < md[m].nF; ++f )
            {
                const triangle_t& t = meshes[m]->triangles[f];
                for( int k = 0; k < 3; ++k )
                {
                    md[m].dir_face[ std::make_pair( t.v[k], t.v[ ( k + 1 ) % 3 ] ) ] =
                        std::make_pair( f, k );
                }
            }
        }
    }

    // ---------------- collect transverse intersection events -------------
    std::vector< Event > events;
    typedef std::map< std::pair< long, std::pair< long, std::size_t > >, long > event_key_map;
    event_key_map event_index;

    for( int em = 0; em < 2; ++em )
    {
        const int om = 1 - em;
        const MeshData& emd = md[em];
        const MeshData& omd = md[om];
        if( emd.nF == 0 || omd.nF == 0 ) continue;

        // Unique undirected edges of mesh em.
        std::set< std::pair< long, long > > edges;
        for( std::map< std::pair< long, long >, std::pair< std::size_t, int > >::const_iterator
             it = emd.dir_face.begin(); it != emd.dir_face.end(); ++it )
        {
            const long x = it->first.first;
            const long y = it->first.second;
            edges.insert( std::make_pair( std::min( x, y ), std::max( x, y ) ) );
        }

        for( std::set< std::pair< long, long > >::const_iterator eit = edges.begin();
             eit != edges.end(); ++eit )
        {
            const long u = eit->first;
            const long w = eit->second;
            const P3& p = emd.pos( static_cast< std::size_t >( u ) );
            const P3& q = emd.pos( static_cast< std::size_t >( w ) );

            // The two faces adjacent to this edge (needed for the angle rule).
            std::vector< std::size_t > adj_faces;
            std::map< std::pair< long, long >, std::pair< std::size_t, int > >::const_iterator fit;
            fit = emd.dir_face.find( std::make_pair( u, w ) );
            if( fit != emd.dir_face.end() ) adj_faces.push_back( fit->second.first );
            fit = emd.dir_face.find( std::make_pair( w, u ) );
            if( fit != emd.dir_face.end() ) adj_faces.push_back( fit->second.first );
            if( adj_faces.size() != 2 )
            {
                throw std::invalid_argument( "edge does not have two adjacent faces" );
            }

            for( std::size_t f = 0; f < omd.nF; ++f )
            {
                const triangle_t& ot = omd.src->triangles[f];
                const P3& ta = omd.pos( ot.v[0] );
                const P3& tb = omd.pos( ot.v[1] );
                const P3& tc = omd.pos( ot.v[2] );
                std::array< double, 3 > bary;
                double tpar;
                if( !edge_triangle_cross( p, q, ta, tb, tc, bary, tpar ) ) continue;

                // Transversality: unit normals of each adjacent face and the
                // pierced face must differ by sin >= 1e-4.
                const P3 n_other = face_normal( *omd.src, f );
                for( std::size_t ai = 0; ai < adj_faces.size(); ++ai )
                {
                    const P3 n_adj = face_normal( *emd.src, adj_faces[ai] );
                    const double sinang = vlen( vcross( n_adj, n_other ) );
                    if( sinang < MIN_SIN_ANGLE )
                    {
                        throw std::domain_error( "intersection angle below 1e-4" );
                    }
                }

                const std::pair< long, std::pair< long, std::size_t > > key =
                    std::make_pair( u, std::make_pair( w, f ) );
                if( event_index.count( key ) ) continue;
                Event ev;
                ev.edge_mesh = em;
                ev.ev0 = u;
                ev.ev1 = w;
                ev.other_face = f;
                ev.s = tpar;
                ev.bary = bary;
                ev.pos = vadd( p, vscale( vsub( q, p ), tpar ) );
                if( !finite3( ev.pos ) )
                {
                    throw std::domain_error( "non-finite intersection point" );
                }
                event_index[ key ] = static_cast< long >( events.size() );
                events.push_back( ev );
            }
        }
    }

    // Reject coincident events: two events at the same point mean edge/edge
    // or vertex contact, which is outside the supported regime.
    for( std::size_t i = 0; i < events.size(); ++i )
    {
        for( std::size_t j = i + 1; j < events.size(); ++j )
        {
            if( vlen( vsub( events[i].pos, events[j].pos ) ) < 1e-9 )
            {
                throw std::domain_error( "coincident intersection events" );
            }
        }
    }

    // ---------------- build per-face marks --------------------------------
    std::vector< std::vector< FaceWork > > work( 2 );
    for( int m = 0; m < 2; ++m )
    {
        work[m].resize( md[m].nF );
        for( std::size_t f = 0; f < md[m].nF; ++f )
        {
            FaceWork& fw = work[m][f];
            const triangle_t& t = meshes[m]->triangles[f];
            for( int k = 0; k < 3; ++k )
            {
                Mark mk;
                mk.kind = Mark::CORNER;
                mk.corner = k;
                mk.event = -1;
                mk.bary.fill( 0.0 );
                mk.bary[k] = 1.0;
                fw.marks.push_back( mk );
            }
            const P3& p0 = meshes[m]->positions[ t.v[0] ];
            const P3& p1 = meshes[m]->positions[ t.v[1] ];
            const P3& p2 = meshes[m]->positions[ t.v[2] ];
            fw.origin = p0;
            fw.e1 = vscale( vsub( p1, p0 ), 1.0 / vlen( vsub( p1, p0 ) ) );
            const P3 nraw = vcross( vsub( p1, p0 ), vsub( p2, p0 ) );
            const P3 nhat = vscale( nraw, 1.0 / vlen( nraw ) );
            fw.e2 = vcross( nhat, fw.e1 );
        }
    }

    for( std::size_t ei = 0; ei < events.size(); ++ei )
    {
        const Event& ev = events[ei];
        // Interior mark in the pierced face of the other mesh.
        {
            const int om = 1 - ev.edge_mesh;
            FaceWork& fw = work[om][ ev.other_face ];
            Mark mk;
            mk.kind = Mark::INTERIOR_EVENT;
            mk.corner = -1;
            mk.event = static_cast< long >( ei );
            mk.bary = ev.bary;
            fw.marks.push_back( mk );
        }
        // Edge marks on the two adjacent faces of the edge mesh.
        {
            const int em = ev.edge_mesh;
            const MeshData& emd = md[em];
            std::map< std::pair< long, long >, std::pair< std::size_t, int > >::const_iterator fit;
            fit = emd.dir_face.find( std::make_pair( ev.ev0, ev.ev1 ) );
            if( fit != emd.dir_face.end() )
            {
                const std::size_t f = fit->second.first;
                FaceWork& fw = work[em][f];
                Mark mk;
                mk.kind = Mark::EDGE_EVENT;
                mk.corner = -1;
                mk.event = static_cast< long >( ei );
                mk.bary.fill( 0.0 );
                const triangle_t& t = meshes[em]->triangles[f];
                for( int k = 0; k < 3; ++k )
                {
                    if( t.v[k] == ev.ev0 ) mk.bary[k] = 1.0 - ev.s;
                    if( t.v[k] == ev.ev1 ) mk.bary[k] = ev.s;
                }
                fw.marks.push_back( mk );
            }
            fit = emd.dir_face.find( std::make_pair( ev.ev1, ev.ev0 ) );
            if( fit != emd.dir_face.end() )
            {
                const std::size_t f = fit->second.first;
                FaceWork& fw = work[em][f];
                Mark mk;
                mk.kind = Mark::EDGE_EVENT;
                mk.corner = -1;
                mk.event = static_cast< long >( ei );
                mk.bary.fill( 0.0 );
                const triangle_t& t = meshes[em]->triangles[f];
                for( int k = 0; k < 3; ++k )
                {
                    if( t.v[k] == ev.ev0 ) mk.bary[k] = 1.0 - ev.s;
                    if( t.v[k] == ev.ev1 ) mk.bary[k] = ev.s;
                }
                fw.marks.push_back( mk );
            }
        }
    }

    // ---------------- pair events into intersection segments -------------
    // Group events by the unordered face pair they belong to.
    std::map< std::pair< std::size_t, std::size_t >, std::vector< long > > pair_events;
    for( std::size_t ei = 0; ei < events.size(); ++ei )
    {
        const Event& ev = events[ei];
        const int em = ev.edge_mesh;
        const MeshData& emd = md[em];
        std::vector< std::size_t > adj;
        std::map< std::pair< long, long >, std::pair< std::size_t, int > >::const_iterator fit;
        fit = emd.dir_face.find( std::make_pair( ev.ev0, ev.ev1 ) );
        if( fit != emd.dir_face.end() ) adj.push_back( fit->second.first );
        fit = emd.dir_face.find( std::make_pair( ev.ev1, ev.ev0 ) );
        if( fit != emd.dir_face.end() ) adj.push_back( fit->second.first );
        for( std::size_t k = 0; k < adj.size(); ++k )
        {
            std::pair< std::size_t, std::size_t > fpair =
                ( em == 0 ) ? std::make_pair( adj[k], ev.other_face )
                            : std::make_pair( ev.other_face, adj[k] );
            pair_events[ fpair ].push_back( static_cast< long >( ei ) );
        }
    }

    for( std::map< std::pair< std::size_t, std::size_t >, std::vector< long > >::iterator
         it = pair_events.begin(); it != pair_events.end(); ++it )
    {
        std::vector< long >& evs = it->second;
        // Deduplicate (an event on an A edge can be listed once per adjacent
        // face, but within one face pair each event is unique).
        std::sort( evs.begin(), evs.end() );
        evs.erase( std::unique( evs.begin(), evs.end() ), evs.end() );
        if( evs.size() != 2 )
        {
            throw std::domain_error( "face pair does not meet in a single transverse segment" );
        }
        const Event& e0 = events[ static_cast< std::size_t >( evs[0] ) ];
        const Event& e1 = events[ static_cast< std::size_t >( evs[1] ) ];
        if( vlen( vsub( e0.pos, e1.pos ) ) < 1e-9 )
        {
            throw std::domain_error( "degenerate intersection segment" );
        }

        const std::size_t fa = it->first.first;
        const std::size_t fb = it->first.second;
        const int ma0 = find_mark( work[0][fa], evs[0] );
        const int ma1 = find_mark( work[0][fa], evs[1] );
        const int mb0 = find_mark( work[1][fb], evs[0] );
        const int mb1 = find_mark( work[1][fb], evs[1] );
        if( ma0 < 0 || ma1 < 0 || mb0 < 0 || mb1 < 0 )
        {
            throw std::runtime_error( "intersection mark missing on incident face" );
        }
        work[0][fa].constraints.push_back( std::make_pair( ma0, ma1 ) );
        work[1][fb].constraints.push_back( std::make_pair( mb0, mb1 ) );
    }

    // ---------------- triangulate every touched face ----------------------
    // Result of triangulation: triples of local mark indices.
    std::vector< std::vector< std::vector< std::array< int, 3 > > > > face_tris( 2 );
    for( int m = 0; m < 2; ++m )
    {
        face_tris[m].resize( md[m].nF );
        for( std::size_t f = 0; f < md[m].nF; ++f )
        {
            FaceWork& fw = work[m][f];
            const triangle_t& t = meshes[m]->triangles[f];
            // Planar coordinates for every mark.
            for( std::size_t k = 0; k < fw.marks.size(); ++k )
            {
                const P3 x = bary_point( meshes[m]->positions[ t.v[0] ],
                                         meshes[m]->positions[ t.v[1] ],
                                         meshes[m]->positions[ t.v[2] ],
                                         fw.marks[k].bary );
                const P3 d = vsub( x, fw.origin );
                fw.marks[k].q[0] = vdot( d, fw.e1 );
                fw.marks[k].q[1] = vdot( d, fw.e2 );
            }
            if( fw.marks.size() == 3 )
            {
                std::array< int, 3 > one = {{ 0, 1, 2 }};
                face_tris[m][f].push_back( one );
                continue;
            }

            PlanarMesher mesher;
            for( std::size_t k = 0; k < fw.marks.size(); ++k )
            {
                mesher.pts.push_back( fw.marks[k].q );
            }
            // PSLG boundary: each source edge, split at its edge events in
            // order from corner k to corner k+1.
            for( int k = 0; k < 3; ++k )
            {
                const int kn = ( k + 1 ) % 3;
                struct EM { int mark; double along; };
                std::vector< EM > on_edge;
                for( std::size_t mi = 3; mi < fw.marks.size(); ++mi )
                {
                    if( fw.marks[mi].kind != Mark::EDGE_EVENT ) continue;
                    const int third = ( k + 2 ) % 3;
                    if( fw.marks[mi].bary[third] > EPS_BARY ) continue;
                    const double off = fw.marks[mi].bary[kn];
                    EM rec;
                    rec.mark = static_cast< int >( mi );
                    rec.along = off;
                    on_edge.push_back( rec );
                }
                std::sort( on_edge.begin(), on_edge.end(),
                    []( const EM& x, const EM& y ) { return x.along < y.along; } );
                int prev = k;
                for( std::size_t z = 0; z < on_edge.size(); ++z )
                {
                    mesher.add_edge( prev, on_edge[z].mark );
                    prev = on_edge[z].mark;
                }
                mesher.add_edge( prev, kn );
            }
            // Real intersection segments are PSLG constraints and survive
            // the whole triangulation as actual edges.
            for( std::size_t k = 0; k < fw.constraints.size(); ++k )
            {
                mesher.add_edge( fw.constraints[k].first, fw.constraints[k].second );
            }
            std::vector< std::array< int, 3 > > built;
            mesher.triangulate( built );
            for( std::size_t k = 0; k < built.size(); ++k )
            {
                face_tris[m][f].push_back( built[k] );
            }
            if( face_tris[m][f].empty() )
            {
                throw std::runtime_error( "face produced no triangles" );
            }
        }
    }

    // ---------------- assemble the output solid ---------------------------
    DifferenceResult local;
    std::map< long long, std::size_t > out_vertex;
    // Key encoding: event eid -> -(eid+1); mesh m original vertex v -> 2*v+2+m.
    for( std::size_t ei = 0; ei < events.size(); ++ei )
    {
        const long long key = -static_cast< long long >( ei ) - 1;
        out_vertex[ key ] = local.mesh.positions.size();
        local.mesh.positions.push_back( events[ei].pos );
    }

    for( int m = 0; m < 2; ++m )
    {
        const SolidMesh& src = *meshes[m];
        for( std::size_t f = 0; f < md[m].nF; ++f )
        {
            const triangle_t& st = src.triangles[f];
            const P3& sa = src.positions[ st.v[0] ];
            const P3& sb = src.positions[ st.v[1] ];
            const P3& sc = src.positions[ st.v[2] ];
            const P2& uva = src.corner_uv[ 3 * f + 0 ];
            const P2& uvb = src.corner_uv[ 3 * f + 1 ];
            const P2& uvc = src.corner_uv[ 3 * f + 2 ];
            const FaceWork& fw = work[m][f];

            for( std::size_t ti = 0; ti < face_tris[m][f].size(); ++ti )
            {
                const std::array< int, 3 >& mt = face_tris[m][f][ti];
                // Order marks in source orientation; B skin gets reversed.
                int order[3];
                if( m == 0 )
                {
                    order[0] = mt[0]; order[1] = mt[1]; order[2] = mt[2];
                }
                else
                {
                    order[0] = mt[2]; order[1] = mt[1]; order[2] = mt[0];
                }

                // Occupancy test at the triangle centroid.
                P3 centroid = {{ 0.0, 0.0, 0.0 }};
                std::array< std::array< double, 3 >, 3 > corner_bary;
                for( int c = 0; c < 3; ++c )
                {
                    corner_bary[c] = fw.marks[ order[c] ].bary;
                    const P3 x = bary_point( sa, sb, sc, corner_bary[c] );
                    centroid = vadd( centroid, vscale( x, 1.0 / 3.0 ) );
                }
                const bool inside_other = point_inside( *meshes[1 - m], centroid );
                const bool keep = ( m == 0 ) ? !inside_other : inside_other;
                if( !keep ) continue;

                triangle_t out_t;
                FaceOrigin origin;
                origin.operand = m;
                origin.face = f;
                for( int c = 0; c < 3; ++c )
                {
                    const Mark& mk = fw.marks[ order[c] ];
                    long long key;
                    if( mk.kind == Mark::CORNER )
                    {
                        key = 2LL * st.v[ mk.corner ] + 2 + m;
                    }
                    else
                    {
                        key = -static_cast< long long >( mk.event ) - 1;
                    }
                    std::map< long long, std::size_t >::iterator vit = out_vertex.find( key );
                    std::size_t vidx;
                    if( vit == out_vertex.end() )
                    {
                        vidx = local.mesh.positions.size();
                        out_vertex[ key ] = vidx;
                        const P3 x = bary_point( sa, sb, sc, mk.bary );
                        local.mesh.positions.push_back( x );
                    }
                    else
                    {
                        vidx = vit->second;
                    }
                    out_t.v[c] = static_cast< long >( vidx );
                    origin.bary[c] = mk.bary;

                    P2 uv = {{
                        mk.bary[0] * uva[0] + mk.bary[1] * uvb[0] + mk.bary[2] * uvc[0],
                        mk.bary[0] * uva[1] + mk.bary[1] * uvb[1] + mk.bary[2] * uvc[1] }};
                    local.mesh.corner_uv.push_back( uv );
                }
                local.mesh.triangles.push_back( out_t );
                local.origins.push_back( origin );
            }
        }
    }

    // ---------------- final defensive manifold check ----------------------
    if( !local.mesh.triangles.empty() )
    {
        // Drop vertices no face references and renumber.
        std::vector< char > used_v( local.mesh.positions.size(), 0 );
        for( std::size_t f = 0; f < local.mesh.triangles.size(); ++f )
        {
            for( int k = 0; k < 3; ++k )
            {
                used_v[ static_cast< std::size_t >( local.mesh.triangles[f].v[k] ) ] = 1;
            }
        }
        std::vector< long > remap( local.mesh.positions.size(), -1 );
        std::vector< P3 > kept_pos;
        for( std::size_t v = 0; v < used_v.size(); ++v )
        {
            if( used_v[v] )
            {
                remap[v] = static_cast< long >( kept_pos.size() );
                kept_pos.push_back( local.mesh.positions[v] );
            }
        }
        for( std::size_t f = 0; f < local.mesh.triangles.size(); ++f )
        {
            for( int k = 0; k < 3; ++k )
            {
                local.mesh.triangles[f].v[k] =
                    remap[ static_cast< std::size_t >( local.mesh.triangles[f].v[k] ) ];
            }
        }
        local.mesh.positions.swap( kept_pos );

        // No duplicate faces and every directed edge has exactly one use.
        std::set< std::set< long > > face_seen;
        std::map< std::pair< long, long >, int > dir_count;
        for( std::size_t f = 0; f < local.mesh.triangles.size(); ++f )
        {
            const triangle_t& t = local.mesh.triangles[f];
            std::set< long > fs;
            fs.insert( t.v[0] ); fs.insert( t.v[1] ); fs.insert( t.v[2] );
            if( fs.size() != 3 || !face_seen.insert( fs ).second )
            {
                throw std::runtime_error( "result contains a duplicate or degenerate face" );
            }
            for( int k = 0; k < 3; ++k )
            {
                ++dir_count[ std::make_pair( t.v[k], t.v[ ( k + 1 ) % 3 ] ) ];
            }
        }
        for( std::map< std::pair< long, long >, int >::const_iterator it = dir_count.begin();
             it != dir_count.end(); ++it )
        {
            if( it->second != 1 )
            {
                throw std::runtime_error( "result edge is not shared by exactly two faces" );
            }
            const std::map< std::pair< long, long >, int >::const_iterator rev =
                dir_count.find( std::make_pair( it->first.second, it->first.first ) );
            if( rev == dir_count.end() || rev->second != 1 )
            {
                throw std::runtime_error( "result has a non-manifold or boundary edge" );
            }
        }

        std::vector< edge_t > edges;
        unordered_edges_from_triangles(
            static_cast< unsigned long >( local.mesh.triangles.size() ),
            &local.mesh.triangles[0], edges );
        trimesh_t he;
        he.build( static_cast< unsigned long >( local.mesh.positions.size() ),
                  static_cast< unsigned long >( local.mesh.triangles.size() ),
                  &local.mesh.triangles[0],
                  static_cast< unsigned long >( edges.size() ), &edges[0] );
        if( !he.boundary_edges().empty() || !he.boundary_vertices().empty() )
        {
            throw std::runtime_error( "result mesh is not a closed manifold" );
        }
        // Every vertex must still be a single closed fan.
        for( std::size_t v = 0; v < local.mesh.positions.size(); ++v )
        {
            const std::vector< trimesh_t::index_t > neigh =
                he.vertex_vertex_neighbors( static_cast< trimesh_t::index_t >( v ) );
            if( neigh.empty() )
            {
                throw std::runtime_error( "result vertex without a fan" );
            }
            trimesh_t::index_t hei = he.directed_edge2he_index(
                static_cast< trimesh_t::index_t >( v ), neigh[0] );
            const trimesh_t::index_t first = hei;
            long walked = 0;
            do
            {
                if( he.halfedge( hei ).face < 0 )
                {
                    throw std::runtime_error( "result fan hits a boundary" );
                }
                ++walked;
                hei = he.halfedge( he.halfedge( hei ).opposite_he ).next_he;
            } while( hei != first && walked <= static_cast< long >( neigh.size() ) + 1 );
            if( hei != first || walked != static_cast< long >( neigh.size() ) )
            {
                throw std::runtime_error( "result vertex has more than one fan" );
            }
        }
        for( std::size_t f = 0; f < local.mesh.triangles.size(); ++f )
        {
            const triangle_t& t = local.mesh.triangles[f];
            const P3 n = vcross(
                vsub( local.mesh.positions[ t.v[1] ], local.mesh.positions[ t.v[0] ] ),
                vsub( local.mesh.positions[ t.v[2] ], local.mesh.positions[ t.v[0] ] ) );
            if( vlen( n ) <= 0.0 )
            {
                throw std::runtime_error( "result contains a zero-area face" );
            }
        }
    }
    if( local.mesh.corner_uv.size() != 3 * local.mesh.triangles.size() )
    {
        throw std::runtime_error( "result uv count mismatch" );
    }

    // Commit only after the whole construction succeeded.
    result = DifferenceResult();
    result.mesh.positions.swap( local.mesh.positions );
    result.mesh.triangles.swap( local.mesh.triangles );
    result.mesh.corner_uv.swap( local.mesh.corner_uv );
    result.origins.swap( local.origins );
}

} // namespace trimesh
