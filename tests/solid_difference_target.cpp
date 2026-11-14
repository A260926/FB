#include "task_support/check.h"
#include "solid_difference.h"
#include "trimesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <set>
#include <vector>

using namespace trimesh;

namespace
{

typedef std::array< double, 3 > P3;
typedef std::array< double, 2 > P2;

static P3 sub( const P3& a, const P3& b )
{
    P3 r = {{ a[0] - b[0], a[1] - b[1], a[2] - b[2] }};
    return r;
}
static P3 add( const P3& a, const P3& b )
{
    P3 r = {{ a[0] + b[0], a[1] + b[1], a[2] + b[2] }};
    return r;
}
static P3 scale( const P3& a, double s )
{
    P3 r = {{ a[0] * s, a[1] * s, a[2] * s }};
    return r;
}
static double dot( const P3& a, const P3& b )
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
static P3 cross( const P3& a, const P3& b )
{
    P3 r = {{
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0] }};
    return r;
}
static double len( const P3& a ) { return std::sqrt( dot( a, a ) ); }
static P3 norm( const P3& a ) { return scale( a, 1.0 / len( a ) ); }

static triangle_t tri( long a, long b, long c )
{
    triangle_t t;
    t.v[0] = a; t.v[1] = b; t.v[2] = c;
    return t;
}

// Per-corner UVs that deliberately differ per source face, so any real seam
// keeps distinct UV values on its two sides.
static void assign_uvs( SolidMesh& m )
{
    m.corner_uv.resize( 3 * m.triangles.size() );
    for( std::size_t f = 0; f < m.triangles.size(); ++f )
    {
        for( int k = 0; k < 3; ++k )
        {
            const P3& p = m.positions[ static_cast< std::size_t >( m.triangles[f].v[k] ) ];
            P2 uv = {{
                0.25 * p[0] + 1.7 * static_cast< double >( f ) + 0.11 * k,
                0.25 * ( p[1] + p[2] ) + 0.9 * static_cast< double >( f ) - 0.07 * k }};
            m.corner_uv[ 3 * f + k ] = uv;
        }
    }
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
        { 0, 3, 2 }, { 0, 2, 1 },       // bottom, -z
        { 4, 5, 6 }, { 4, 6, 7 },       // top, +z
        { 0, 1, 5 }, { 0, 5, 4 },       // front, -y
        { 2, 3, 7 }, { 2, 7, 6 },       // back, +y
        { 3, 0, 4 }, { 3, 4, 7 },       // left, -x
        { 1, 2, 6 }, { 1, 6, 5 }        // right, +x
    };
    for( int k = 0; k < 12; ++k )
    {
        m.triangles.push_back( tri( idx[k][0], idx[k][1], idx[k][2] ) );
    }
    assign_uvs( m );
    return m;
}

// Extrude a counter-clockwise xy polygon into a closed prism.
static SolidMesh make_prism( const std::vector< P3 >& poly, double z0, double z1 )
{
    SolidMesh m;
    const std::size_t n = poly.size();
    for( std::size_t i = 0; i < n; ++i )
        m.positions.push_back( {{ poly[i][0], poly[i][1], z0 }} );
    for( std::size_t i = 0; i < n; ++i )
        m.positions.push_back( {{ poly[i][0], poly[i][1], z1 }} );
    const long cb = 0;
    const long ct = static_cast< long >( n );
    for( std::size_t i = 0; i < n; ++i )
    {
        const long a = static_cast< long >( i );
        const long b = static_cast< long >( ( i + 1 ) % n );
        const long c = ct + static_cast< long >( ( i + 1 ) % n );
        const long d = ct + static_cast< long >( i );
        m.triangles.push_back( tri( a, b, c ) );
        m.triangles.push_back( tri( a, c, d ) );
    }
    for( std::size_t i = 0; i + 2 < n; ++i )
    {
        m.triangles.push_back( tri( ct, ct + static_cast< long >( i + 1 ),
                                    ct + static_cast< long >( i + 2 ) ) );
        m.triangles.push_back( tri( cb, static_cast< long >( i + 2 ),
                                    static_cast< long >( i + 1 ) ) );
    }
    assign_uvs( m );
    return m;
}

static SolidMesh make_tetra( const P3& q0, const P3& q1, const P3& q2, const P3& q3 )
{
    SolidMesh m;
    m.positions.push_back( q0 );
    m.positions.push_back( q1 );
    m.positions.push_back( q2 );
    m.positions.push_back( q3 );
    // q0,q1,q2 is a counter-clockwise base; q3 is the apex.
    m.triangles.push_back( tri( 0, 1, 2 ) );
    m.triangles.push_back( tri( 0, 3, 1 ) );
    m.triangles.push_back( tri( 1, 3, 2 ) );
    m.triangles.push_back( tri( 2, 3, 0 ) );
    assign_uvs( m );
    return m;
}

static double mesh_volume( const SolidMesh& m )
{
    double v = 0.0;
    for( std::size_t f = 0; f < m.triangles.size(); ++f )
    {
        const triangle_t& t = m.triangles[f];
        v += dot( m.positions[ t.v[0] ],
                  cross( m.positions[ t.v[1] ], m.positions[ t.v[2] ] ) );
    }
    return v / 6.0;
}

// Independent parity ray membership test.
static bool ray_parity( const SolidMesh& m, const P3& p, const P3& d, bool& degenerate )
{
    degenerate = false;
    long hits = 0;
    for( std::size_t f = 0; f < m.triangles.size(); ++f )
    {
        const triangle_t& t = m.triangles[f];
        const P3& a = m.positions[ t.v[0] ];
        const P3& b = m.positions[ t.v[1] ];
        const P3& c = m.positions[ t.v[2] ];
        const P3 e1 = sub( b, a );
        const P3 e2 = sub( c, a );
        const P3 pv = cross( d, e2 );
        const double det = dot( e1, pv );
        if( std::fabs( det ) < 1e-18 ) continue;
        const P3 tv = sub( p, a );
        const double u = dot( tv, pv ) / det;
        if( u < 0.0 || u > 1.0 ) continue;
        const P3 qv = cross( tv, e1 );
        const double v = dot( d, qv ) / det;
        if( v < 0.0 || u + v > 1.0 ) continue;
        const double tt = dot( e2, qv ) / det;
        if( tt <= 1e-12 ) continue;
        if( u <= 1e-9 || v <= 1e-9 || u + v >= 1.0 - 1e-9 ) degenerate = true;
        ++hits;
    }
    return ( hits & 1L ) != 0;
}

static bool inside( const SolidMesh& m, const P3& p )
{
    if( m.triangles.empty() ) return false;
    const double dirs[7][3] =
    {
        { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 },
        { 0.5773502691896258, 0.5773502691896258, 0.5773502691896258 },
        { 0.8164965809277260, 0.4082482904638630, -0.4082482904638630 },
        { 0.3015113445777636, -0.9045340337332909, 0.3015113445777636 },
        { -0.4542568122066114, 0.7712364807512393, 0.4460941038949457 }
    };
    for( int k = 0; k < 7; ++k )
    {
        P3 d = {{ dirs[k][0], dirs[k][1], dirs[k][2] }};
        d = norm( d );
        bool deg;
        const bool r = ray_parity( m, p, d, deg );
        if( !deg ) return r;
    }
    return false;
}

// Full manifold / orientation-independent structural validation.
static bool validate_structure( const SolidMesh& m )
{
    if( m.triangles.empty() )
    {
        return m.positions.empty() && m.corner_uv.empty();
    }
    if( m.corner_uv.size() != 3 * m.triangles.size() ) return false;
    for( std::size_t i = 0; i < m.positions.size(); ++i )
    {
        for( int k = 0; k < 3; ++k )
        {
            if( !std::isfinite( m.positions[i][k] ) ) return false;
        }
    }
    std::set< std::set< long > > faces;
    std::map< std::pair< long, long >, int > dc;
    std::vector< char > used( m.positions.size(), 0 );
    for( std::size_t f = 0; f < m.triangles.size(); ++f )
    {
        const triangle_t& t = m.triangles[f];
        std::set< long > fs;
        for( int k = 0; k < 3; ++k )
        {
            if( t.v[k] < 0 || static_cast< std::size_t >( t.v[k] ) >= m.positions.size() )
                return false;
            fs.insert( t.v[k] );
            used[ static_cast< std::size_t >( t.v[k] ) ] = 1;
            ++dc[ std::make_pair( t.v[k], t.v[ ( k + 1 ) % 3 ] ) ];
        }
        if( fs.size() != 3 || !faces.insert( fs ).second ) return false;
        const P3 n = cross( sub( m.positions[ t.v[1] ], m.positions[ t.v[0] ] ),
                            sub( m.positions[ t.v[2] ], m.positions[ t.v[0] ] ) );
        if( len( n ) <= 0.0 ) return false;
    }
    for( std::size_t v = 0; v < used.size(); ++v )
    {
        if( !used[v] ) return false;
    }
    for( std::map< std::pair< long, long >, int >::const_iterator it = dc.begin();
         it != dc.end(); ++it )
    {
        if( it->second != 1 ) return false;
        if( dc.find( std::make_pair( it->first.second, it->first.first ) ) == dc.end() )
            return false;
    }
    std::vector< edge_t > edges;
    unordered_edges_from_triangles(
        static_cast< unsigned long >( m.triangles.size() ), &m.triangles[0], edges );
    trimesh_t he;
    he.build( static_cast< unsigned long >( m.positions.size() ),
              static_cast< unsigned long >( m.triangles.size() ),
              &m.triangles[0],
              static_cast< unsigned long >( edges.size() ), &edges[0] );
    if( !he.boundary_edges().empty() || !he.boundary_vertices().empty() ) return false;
    for( std::size_t v = 0; v < m.positions.size(); ++v )
    {
        const std::vector< trimesh_t::index_t > nb =
            he.vertex_vertex_neighbors( static_cast< trimesh_t::index_t >( v ) );
        if( nb.empty() ) return false;
        trimesh_t::index_t h = he.directed_edge2he_index(
            static_cast< trimesh_t::index_t >( v ), nb[0] );
        const trimesh_t::index_t first = h;
        long walked = 0;
        do
        {
            if( he.halfedge( h ).face < 0 ) return false;
            ++walked;
            h = he.halfedge( he.halfedge( h ).opposite_he ).next_he;
        } while( h != first && walked <= static_cast< long >( nb.size() ) + 1 );
        if( h != first || walked != static_cast< long >( nb.size() ) ) return false;
    }
    return true;
}

static int face_adjacency_components( const SolidMesh& m )
{
    const std::size_t n = m.triangles.size();
    std::vector< long > parent( n );
    for( std::size_t i = 0; i < n; ++i ) parent[i] = static_cast< long >( i );
    struct Find
    {
        std::vector< long >& p;
        long operator()( long x )
        {
            while( p[ static_cast< std::size_t >( x ) ] != x )
            {
                p[ static_cast< std::size_t >( x ) ] =
                    p[ static_cast< std::size_t >( p[ static_cast< std::size_t >( x ) ] ) ];
                x = p[ static_cast< std::size_t >( x ) ];
            }
            return x;
        }
    };
    Find find = { parent };
    std::map< std::pair< long, long >, long > edge_face;
    for( std::size_t f = 0; f < n; ++f )
    {
        const triangle_t& t = m.triangles[f];
        for( int k = 0; k < 3; ++k )
        {
            const long a = t.v[k];
            const long b = t.v[ ( k + 1 ) % 3 ];
            const std::pair< long, long > key =
                std::make_pair( std::min( a, b ), std::max( a, b ) );
            std::map< std::pair< long, long >, long >::iterator it = edge_face.find( key );
            if( it == edge_face.end() )
            {
                edge_face[ key ] = static_cast< long >( f );
            }
            else
            {
                long ra = find( it->second );
                long rb = find( static_cast< long >( f ) );
                if( ra != rb ) parent[ static_cast< std::size_t >( ra ) ] = rb;
            }
        }
    }
    std::set< long > roots;
    for( std::size_t i = 0; i < n; ++i ) roots.insert( find( static_cast< long >( i ) ) );
    return static_cast< int >( roots.size() );
}

// Check every output triangle against its FaceOrigin: barycentric back
// substitution, UV interpolation, and cut orientation.
static bool validate_provenance( const SolidMesh& a, const SolidMesh& b,
                                 const DifferenceResult& r )
{
    if( r.origins.size() != r.mesh.triangles.size() ) return false;
    for( std::size_t f = 0; f < r.mesh.triangles.size(); ++f )
    {
        const FaceOrigin& o = r.origins[f];
        if( o.operand != 0 && o.operand != 1 ) return false;
        const SolidMesh& src = ( o.operand == 0 ) ? a : b;
        if( o.face >= src.triangles.size() ) return false;
        const triangle_t& st = src.triangles[o.face];
        const P3& sa = src.positions[ st.v[0] ];
        const P3& sb = src.positions[ st.v[1] ];
        const P3& sc = src.positions[ st.v[2] ];
        double wsum = 0.0;
        for( int k = 0; k < 3; ++k )
        {
            const std::array< double, 3 >& w = o.bary[k];
            if( w[0] < -1e-9 || w[1] < -1e-9 || w[2] < -1e-9 ) return false;
            const double s = w[0] + w[1] + w[2];
            if( std::fabs( s - 1.0 ) > 1e-9 ) return false;
            wsum += s;
            P3 x = add( add( scale( sa, w[0] ), scale( sb, w[1] ) ), scale( sc, w[2] ) );
            const P3& got = r.mesh.positions[
                static_cast< std::size_t >( r.mesh.triangles[f].v[k] ) ];
            if( len( sub( x, got ) ) > 1e-8 ) return false;
            const P2& ua = src.corner_uv[ 3 * o.face + 0 ];
            const P2& ub = src.corner_uv[ 3 * o.face + 1 ];
            const P2& uc = src.corner_uv[ 3 * o.face + 2 ];
            P2 uvi = {{
                w[0] * ua[0] + w[1] * ub[0] + w[2] * uc[0],
                w[0] * ua[1] + w[1] * ub[1] + w[2] * uc[1] }};
            const P2& got_uv = r.mesh.corner_uv[ 3 * f + k ];
            if( std::fabs( uvi[0] - got_uv[0] ) > 1e-8 ||
                std::fabs( uvi[1] - got_uv[1] ) > 1e-8 ) return false;
        }
        if( std::fabs( wsum - 3.0 ) > 1e-8 ) return false;
        // Orientation relative to the source triangle.
        const P3 ns = norm( cross( sub( sb, sa ), sub( sc, sa ) ) );
        const triangle_t& ot = r.mesh.triangles[f];
        const P3 no = norm( cross(
            sub( r.mesh.positions[ ot.v[1] ], r.mesh.positions[ ot.v[0] ] ),
            sub( r.mesh.positions[ ot.v[2] ], r.mesh.positions[ ot.v[0] ] ) ) );
        if( o.operand == 0 && dot( ns, no ) <= 0.999 ) return false;
        if( o.operand == 1 && dot( ns, no ) >= -0.999 ) return false;
    }
    return true;
}

// Independent occupancy agreement on a dense probe lattice.
static bool occupancy_matches( const SolidMesh& a, const SolidMesh& b,
                               const SolidMesh& result )
{
    int checked = 0;
    for( int i = -24; i <= 24; ++i )
    for( int j = -24; j <= 24; ++j )
    for( int k = -24; k <= 24; ++k )
    {
        P3 p = {{ 0.173 * i, 0.191 * j - 0.031, 0.211 * k + 0.017 }};
        if( std::fabs( p[0] ) > 2.4 || std::fabs( p[1] ) > 2.4 || std::fabs( p[2] ) > 2.4 )
            continue;
        const bool expect = inside( a, p ) && !inside( b, p );
        if( inside( result, p ) != expect ) return false;
        ++checked;
    }
    return checked > 1000;
}

static bool same_mesh_geometry( const SolidMesh& x, const SolidMesh& y )
{
    if( x.triangles.size() != y.triangles.size() ) return false;
    if( x.positions.size() != y.positions.size() ) return false;
    if( x.corner_uv.size() != y.corner_uv.size() ) return false;
    std::set< P3 > sx, sy;
    for( std::size_t i = 0; i < x.positions.size(); ++i )
    {
        sx.insert( x.positions[i] );
        sy.insert( y.positions[i] );
    }
    return sx == sy;
}

static SolidMesh rotated_l_prism( double angle, double z0, double z1,
                                  double ox, double oy )
{
    const double base[6][2] =
    {
        { 0.0, 0.0 }, { 4.0, 0.0 }, { 4.0, 1.0 },
        { 1.0, 1.0 }, { 1.0, 4.0 }, { 0.0, 4.0 }
    };
    const double c = std::cos( angle );
    const double s = std::sin( angle );
    std::vector< P3 > poly;
    for( int k = 0; k < 6; ++k )
    {
        const double x = base[k][0] - 2.0;
        const double y = base[k][1] - 2.0;
        P3 q = {{ c * x - s * y + ox, s * x + c * y + oy, 0.0 }};
        poly.push_back( q );
    }
    return make_prism( poly, z0, z1 );
}

// ---------------------------------------------------------------------------
// intersections group
// ---------------------------------------------------------------------------

static int case_through_hole()
{
    SolidMesh a = make_box( -2, 2, -2, 2, -2, 2 );
    SolidMesh b = make_box( -3, 3, -0.6, 0.2, -0.3, 0.5 );
    DifferenceResult r;
    subtract_solid( a, b, r );
    TASK_CHECK( validate_structure( r.mesh ) );
    TASK_CHECK( r.origins.size() == r.mesh.triangles.size() );
    std::size_t cut_faces = 0;
    for( std::size_t f = 0; f < r.origins.size(); ++f )
    {
        if( r.origins[f].operand == 1 ) ++cut_faces;
    }
    TASK_CHECK( cut_faces > 0 );
    TASK_CHECK( std::fabs( mesh_volume( r.mesh ) - ( 64.0 - 4.0 * 0.8 * 0.8 ) ) < 1e-8 );
    TASK_CHECK( occupancy_matches( a, b, r.mesh ) );
    TASK_CHECK( validate_provenance( a, b, r ) );
    // The tunnel really opens through two opposite, non-coplanar A faces.
    std::set< std::size_t > touched_a;
    for( std::size_t f = 0; f < r.origins.size(); ++f )
    {
        if( r.origins[f].operand == 0 ) touched_a.insert( r.origins[f].face );
    }
    TASK_CHECK( touched_a.size() >= 4 );
    return 0;
}

static int case_closed_loop_inside_triangle()
{
    SolidMesh a = make_box( -2, 2, -2, 2, -2, 2 );
    const double cx = 0.6, cy = -0.4, rr = 0.3;
    SolidMesh b = make_tetra(
        {{ cx - rr, cy - rr, 3.0 }},
        {{ cx + rr, cy - rr, 3.0 }},
        {{ cx, cy + rr, 3.0 }},
        {{ cx, cy, 1.0 }} );
    DifferenceResult r;
    subtract_solid( a, b, r );
    TASK_CHECK( validate_structure( r.mesh ) );
    TASK_CHECK( validate_provenance( a, b, r ) );

    // The three loop vertices lie on A's top plane and are strictly interior
    // to a single source triangle (the top face triangle over y <= x).
    int loop_vertices = 0;
    for( std::size_t f = 0; f < r.origins.size(); ++f )
    {
        if( r.origins[f].operand != 1 ) continue;
        for( int k = 0; k < 3; ++k )
        {
            const P3& p = r.mesh.positions[
                static_cast< std::size_t >( r.mesh.triangles[f].v[k] ) ];
            if( std::fabs( p[2] - 2.0 ) > 1e-8 ) continue;
            ++loop_vertices;
            // B side face 1,2,3 (index 2) and friends carry the loop; check
            // the point lands inside A top triangle (4,5,6) with margin.
            TASK_CHECK( p[0] > -2.0 && p[0] < 2.0 && p[1] > -2.0 && p[1] < 2.0 );
            TASK_CHECK( p[1] < p[0] - 1e-6 );
        }
    }
    TASK_CHECK( loop_vertices >= 6 ); // three shared points, seen on several corners

    // Removed material is the tetrahedron under the loop.
    P3 l0 = {{ cx - rr * 0.5, cy - rr * 0.5, 2.0 }};
    P3 l1 = {{ cx + rr * 0.5, cy - rr * 0.5, 2.0 }};
    P3 l2 = {{ cx, cy + rr * 0.5, 2.0 }};
    // Intersect the three tetra side edges with z = 2 exactly (edge midpoint
    // parameter: from z=3 to z=1, t = 0.5).
    const P3 q0 = {{ cx - rr, cy - rr, 3.0 }};
    const P3 q1 = {{ cx + rr, cy - rr, 3.0 }};
    const P3 q2 = {{ cx, cy + rr, 3.0 }};
    const P3 q3 = {{ cx, cy, 1.0 }};
    P3 m0 = add( scale( q0, 0.5 ), scale( q3, 0.5 ) );
    P3 m1 = add( scale( q1, 0.5 ), scale( q3, 0.5 ) );
    P3 m2 = add( scale( q2, 0.5 ), scale( q3, 0.5 ) );
    (void)l0; (void)l1; (void)l2;
    const double removed = std::fabs( dot( sub( m1, m0 ),
        cross( sub( m2, m0 ), sub( q3, m0 ) ) ) ) / 6.0;
    TASK_CHECK( std::fabs( mesh_volume( r.mesh ) - ( 64.0 - removed ) ) < 1e-7 );
    TASK_CHECK( occupancy_matches( a, b, r.mesh ) );
    return 0;
}

static int case_nonconvex_oblique()
{
    SolidMesh a = make_box( -3, 3, -3, 3, -3, 3 );
    SolidMesh b = rotated_l_prism( 0.7853981633974483, -2.0, 2.5, 0.37, -0.29 );
    DifferenceResult r;
    subtract_solid( a, b, r );
    TASK_CHECK( validate_structure( r.mesh ) );
    TASK_CHECK( validate_provenance( a, b, r ) );
    TASK_CHECK( occupancy_matches( a, b, r.mesh ) );
    std::set< std::size_t > fa, fb;
    std::set< P3 > normals_a, normals_b;
    for( std::size_t f = 0; f < r.origins.size(); ++f )
    {
        const FaceOrigin& o = r.origins[f];
        if( o.operand == 0 )
        {
            fa.insert( o.face );
            const triangle_t& t = a.triangles[o.face];
            normals_a.insert( norm( cross( sub( a.positions[ t.v[1] ], a.positions[ t.v[0] ] ),
                                            sub( a.positions[ t.v[2] ], a.positions[ t.v[0] ] ) ) ) );
        }
        else
        {
            fb.insert( o.face );
            const triangle_t& t = b.triangles[o.face];
            normals_b.insert( norm( cross( sub( b.positions[ t.v[1] ], b.positions[ t.v[0] ] ),
                                            sub( b.positions[ t.v[2] ], b.positions[ t.v[0] ] ) ) ) );
        }
    }
    TASK_CHECK( fa.size() >= 3 && fb.size() >= 3 );
    TASK_CHECK( normals_a.size() >= 2 && normals_b.size() >= 3 );
    TASK_CHECK( mesh_volume( r.mesh ) > 0.0 );
    return 0;
}

static int case_cut_spans_non_coplanar_faces()
{
    SolidMesh a = make_box( -2, 2, -2, 2, -2, 2 );
    // A thin box rotated about z by 30 degrees, long in z, crossing the top
    // and one side face of A.
    SolidMesh rot = rotated_l_prism( 0.0, 0.0, 0.0, 0.0, 0.0 );
    (void)rot;
    std::vector< P3 > rect;
    const double ang = 0.5235987755982988;
    const double c = std::cos( ang ), s = std::sin( ang );
    const double local[4][2] = {{ -0.4, -0.3 }, { 0.4, -0.3 }, { 0.4, 0.3 }, { -0.4, 0.3 }};
    for( int k = 0; k < 4; ++k )
    {
        P3 q = {{
            c * local[k][0] - s * local[k][1] + 0.8,
            s * local[k][0] + c * local[k][1] - 0.7, 0.0 }};
        rect.push_back( q );
    }
    SolidMesh b = make_prism( rect, -3.0, 3.0 );
    DifferenceResult r;
    subtract_solid( a, b, r );
    TASK_CHECK( validate_structure( r.mesh ) );
    TASK_CHECK( validate_provenance( a, b, r ) );
    TASK_CHECK( occupancy_matches( a, b, r.mesh ) );
    std::set< P3 > cut_normals;
    for( std::size_t f = 0; f < r.origins.size(); ++f )
    {
        if( r.origins[f].operand != 1 ) continue;
        const triangle_t& t = b.triangles[ r.origins[f].face ];
        cut_normals.insert( norm( cross(
            sub( b.positions[ t.v[1] ], b.positions[ t.v[0] ] ),
            sub( b.positions[ t.v[2] ], b.positions[ t.v[0] ] ) ) ) );
    }
    TASK_CHECK( cut_normals.size() >= 3 );
    return 0;
}

// ---------------------------------------------------------------------------
// solids group
// ---------------------------------------------------------------------------

static int case_disjoint()
{
    SolidMesh a = make_box( -2, -1, -2, -1, -2, -1 );
    SolidMesh b = make_box( 1, 2, 1, 2, 1, 2 );
    DifferenceResult r;
    subtract_solid( a, b, r );
    TASK_CHECK( validate_structure( r.mesh ) );
    TASK_CHECK( same_mesh_geometry( r.mesh, a ) );
    TASK_CHECK( r.mesh.triangles.size() == 12 );
    for( std::size_t f = 0; f < r.origins.size(); ++f )
    {
        TASK_CHECK( r.origins[f].operand == 0 );
    }
    TASK_CHECK( std::fabs( mesh_volume( r.mesh ) - 1.0 ) < 1e-10 );
    return 0;
}

static int case_enclosed_cavity()
{
    SolidMesh a = make_box( -2, 2, -2, 2, -2, 2 );
    SolidMesh b = make_box( -0.5, 0.5, -0.5, 0.5, -0.5, 0.5 );
    DifferenceResult r;
    subtract_solid( a, b, r );
    TASK_CHECK( validate_structure( r.mesh ) );
    TASK_CHECK( r.mesh.triangles.size() == 24 );
    std::size_t inner = 0;
    for( std::size_t f = 0; f < r.origins.size(); ++f )
    {
        if( r.origins[f].operand == 1 ) ++inner;
    }
    TASK_CHECK( inner == 12 );
    TASK_CHECK( std::fabs( mesh_volume( r.mesh ) - 63.0 ) < 1e-8 );
    TASK_CHECK( validate_provenance( a, b, r ) );
    P3 probe = {{ 0.0, 0.0, 0.0 }};
    TASK_CHECK( !inside( r.mesh, probe ) );
    P3 wall = {{ 1.0, 0.0, 0.0 }};
    TASK_CHECK( inside( r.mesh, wall ) );
    TASK_CHECK( face_adjacency_components( r.mesh ) == 2 );
    return 0;
}

static int case_engulfed()
{
    SolidMesh a = make_box( -1, 1, -1, 1, -1, 1 );
    SolidMesh b = make_box( -2, 2, -2, 2, -2, 2 );
    DifferenceResult r;
    subtract_solid( a, b, r );
    TASK_CHECK( r.mesh.triangles.empty() );
    TASK_CHECK( r.mesh.positions.empty() );
    TASK_CHECK( r.mesh.corner_uv.empty() );
    TASK_CHECK( r.origins.empty() );
    return 0;
}

static int case_empty_inputs()
{
    SolidMesh a = make_box( -1, 1, -1, 1, -1, 1 );
    SolidMesh empty;
    DifferenceResult r;
    subtract_solid( a, empty, r );
    TASK_CHECK( same_mesh_geometry( r.mesh, a ) );
    TASK_CHECK( r.mesh.corner_uv.size() == a.corner_uv.size() );
    DifferenceResult r2;
    subtract_solid( empty, a, r2 );
    TASK_CHECK( r2.mesh.triangles.empty() && r2.mesh.positions.empty() );
    DifferenceResult r3;
    subtract_solid( empty, empty, r3 );
    TASK_CHECK( r3.mesh.triangles.empty() );
    return 0;
}

static int case_split_into_remnants()
{
    SolidMesh a = make_box( -2, 2, -2, 2, -2, 2 );
    // A thin slab spanning A's whole y/z extent with its broad faces outside
    // A, so the cut splits A into two disconnected remnants.  The asymmetric
    // section keeps its edges off A's face diagonals (no edge/edge contact).
    SolidMesh b = make_box( -0.3, 0.35, -3.0, 2.8, -2.9, 3.1 );
    DifferenceResult r;
    subtract_solid( a, b, r );
    TASK_CHECK( validate_structure( r.mesh ) );
    TASK_CHECK( face_adjacency_components( r.mesh ) == 2 );
    TASK_CHECK( std::fabs( mesh_volume( r.mesh ) -
        ( 64.0 - ( 0.35 + 0.3 ) * 4.0 * 4.0 ) ) < 1e-8 );
    TASK_CHECK( validate_provenance( a, b, r ) );
    TASK_CHECK( occupancy_matches( a, b, r.mesh ) );
    P3 left = {{ -1.0, 0.0, 0.0 }};
    P3 right = {{ 1.0, 0.0, 0.0 }};
    P3 gap = {{ 0.0, 0.0, 0.0 }};
    TASK_CHECK( inside( r.mesh, left ) );
    TASK_CHECK( inside( r.mesh, right ) );
    TASK_CHECK( !inside( r.mesh, gap ) );
    return 0;
}

static SolidMesh append_shell( const SolidMesh& base, const SolidMesh& shell )
{
    SolidMesh out = base;
    const long off = static_cast< long >( out.positions.size() );
    for( std::size_t i = 0; i < shell.positions.size(); ++i )
        out.positions.push_back( shell.positions[i] );
    for( std::size_t f = 0; f < shell.triangles.size(); ++f )
    {
        triangle_t t;
        for( int k = 0; k < 3; ++k ) t.v[k] = shell.triangles[f].v[k] + off;
        out.triangles.push_back( t );
    }
    for( std::size_t i = 0; i < shell.corner_uv.size(); ++i )
        out.corner_uv.push_back( shell.corner_uv[i] );
    return out;
}

static int case_two_inner_shells()
{
    SolidMesh a = make_box( -3, 3, -3, 3, -3, 3 );
    SolidMesh b0 = make_box( -2.0, -1.2, -0.5, 0.5, -0.5, 0.5 );
    SolidMesh b1 = make_box( 1.2, 2.0, -0.5, 0.5, -0.5, 0.5 );
    SolidMesh b = append_shell( b0, b1 );
    DifferenceResult r;
    subtract_solid( a, b, r );
    TASK_CHECK( validate_structure( r.mesh ) );
    TASK_CHECK( face_adjacency_components( r.mesh ) == 3 );
    TASK_CHECK( std::fabs( mesh_volume( r.mesh ) -
        ( 216.0 - 2.0 * ( 0.8 * 1.0 * 1.0 ) ) ) < 1e-8 );
    TASK_CHECK( !inside( r.mesh, {{ -1.6, 0.0, 0.0 }} ) );
    TASK_CHECK( !inside( r.mesh, {{ 1.6, 0.0, 0.0 }} ) );
    TASK_CHECK( inside( r.mesh, {{ 0.0, 0.0, 0.0 }} ) );
    return 0;
}

// ---------------------------------------------------------------------------
// provenance group
// ---------------------------------------------------------------------------

static int case_uv_seams_and_corner_order()
{
    SolidMesh a = make_box( -2, 2, -2, 2, -2, 2 );
    SolidMesh b = make_box( -3, 3, -0.6, 0.2, -0.3, 0.5 );
    DifferenceResult r;
    subtract_solid( a, b, r );
    TASK_CHECK( validate_provenance( a, b, r ) );

    // A geometric event vertex shared by an A-skin and a B-skin output
    // triangle must keep the two sides' distinct UVs (no averaging).
    std::map< P3, std::set< P2 > > uv_at_point;
    for( std::size_t f = 0; f < r.mesh.triangles.size(); ++f )
    {
        for( int k = 0; k < 3; ++k )
        {
            const P3& p = r.mesh.positions[
                static_cast< std::size_t >( r.mesh.triangles[f].v[k] ) ];
            uv_at_point[ p ].insert( r.mesh.corner_uv[ 3 * f + k ] );
        }
    }
    int seam_points = 0;
    for( std::map< P3, std::set< P2 > >::const_iterator it = uv_at_point.begin();
         it != uv_at_point.end(); ++it )
    {
        if( it->second.size() >= 2 ) ++seam_points;
    }
    TASK_CHECK( seam_points >= 4 );

    // Every stored UV is finite and in the supported range.
    for( std::size_t i = 0; i < r.mesh.corner_uv.size(); ++i )
    {
        TASK_CHECK( std::isfinite( r.mesh.corner_uv[i][0] ) );
        TASK_CHECK( std::isfinite( r.mesh.corner_uv[i][1] ) );
        TASK_CHECK( std::fabs( r.mesh.corner_uv[i][0] ) <= 100.0 );
        TASK_CHECK( std::fabs( r.mesh.corner_uv[i][1] ) <= 100.0 );
    }
    return 0;
}

static int case_reuse_and_independence()
{
    SolidMesh a = make_box( -2, 2, -2, 2, -2, 2 );
    SolidMesh b = make_box( -3, 3, -0.6, 0.2, -0.3, 0.5 );
    DifferenceResult r1;
    subtract_solid( a, b, r1 );
    DifferenceResult r2;
    subtract_solid( a, b, r2 );
    TASK_CHECK( r1.mesh.triangles.size() == r2.mesh.triangles.size() );
    TASK_CHECK( r1.mesh.positions.size() == r2.mesh.positions.size() );
    TASK_CHECK( r1.origins.size() == r2.origins.size() );

    const SolidMesh snapshot = r1.mesh;
    // Mutate an input; the owned result must not change.
    a.positions[0][0] += 0.5;
    a.triangles[0].v[0] = 1;
    TASK_CHECK( r1.mesh.triangles.size() == snapshot.triangles.size() );
    TASK_CHECK( r1.mesh.positions.size() == snapshot.positions.size() );
    for( std::size_t i = 0; i < snapshot.positions.size(); ++i )
    {
        for( int k = 0; k < 3; ++k )
        {
            TASK_CHECK( r1.mesh.positions[i][k] == snapshot.positions[i][k] );
        }
    }

    // Reuse the same result object on another call.
    SolidMesh c = make_box( -1, 1, -1, 1, -1, 1 );
    SolidMesh empty;
    subtract_solid( c, empty, r1 );
    TASK_CHECK( r1.mesh.triangles.size() == 12 );
    TASK_CHECK( r1.origins.size() == 12 );
    TASK_CHECK( r2.mesh.triangles.size() == snapshot.triangles.size() );
    return 0;
}

static int case_inputs_unchanged()
{
    SolidMesh a = make_box( -2, 2, -2, 2, -2, 2 );
    SolidMesh b = make_box( -3, 3, -0.6, 0.2, -0.3, 0.5 );
    const SolidMesh sa = a, sb = b;
    DifferenceResult r;
    subtract_solid( a, b, r );
    TASK_CHECK( a.positions.size() == sa.positions.size() );
    TASK_CHECK( a.triangles.size() == sa.triangles.size() );
    TASK_CHECK( a.corner_uv.size() == sa.corner_uv.size() );
    for( std::size_t i = 0; i < sa.positions.size(); ++i )
    {
        for( int k = 0; k < 3; ++k )
        {
            TASK_CHECK( a.positions[i][k] == sa.positions[i][k] );
        }
    }
    for( std::size_t i = 0; i < sa.triangles.size(); ++i )
    {
        for( int k = 0; k < 3; ++k )
        {
            TASK_CHECK( a.triangles[i].v[k] == sa.triangles[i].v[k] );
            TASK_CHECK( b.triangles[i].v[k] == sb.triangles[i].v[k] );
        }
    }
    for( std::size_t i = 0; i < sa.corner_uv.size(); ++i )
    {
        TASK_CHECK( a.corner_uv[i][0] == sa.corner_uv[i][0] );
        TASK_CHECK( b.corner_uv[i][1] == sb.corner_uv[i][1] );
    }
    return 0;
}

// ---------------------------------------------------------------------------
// validation group
// ---------------------------------------------------------------------------

static DifferenceResult sentinel_result()
{
    DifferenceResult r;
    r.mesh.positions.push_back( {{ 7.0, 8.0, 9.0 }} );
    r.mesh.corner_uv.push_back( {{ 0.5, 0.25 }} );
    FaceOrigin o;
    o.operand = 0;
    o.face = 999;
    r.origins.push_back( o );
    return r;
}

static bool throws_invalid( const SolidMesh& a, const SolidMesh& b )
{
    DifferenceResult r = sentinel_result();
    try
    {
        subtract_solid( a, b, r );
    }
    catch( const std::invalid_argument& )
    {
        // Sentinel content must survive the failed call.
        TASK_CHECK( r.mesh.positions.size() == 1 );
        TASK_CHECK( r.mesh.positions[0][0] == 7.0 );
        TASK_CHECK( r.origins.size() == 1 && r.origins[0].face == 999 );
        return true;
    }
    catch( ... ) {}
    return false;
}

static bool throws_domain( const SolidMesh& a, const SolidMesh& b )
{
    DifferenceResult r = sentinel_result();
    try
    {
        subtract_solid( a, b, r );
    }
    catch( const std::domain_error& )
    {
        TASK_CHECK( r.mesh.positions.size() == 1 );
        TASK_CHECK( r.origins[0].face == 999 );
        return true;
    }
    catch( ... ) {}
    return false;
}

static int case_malformed_inputs()
{
    SolidMesh good = make_box( -1, 1, -1, 1, -1, 1 );

    SolidMesh dup = good;
    dup.triangles.push_back( dup.triangles[0] );
    TASK_CHECK( throws_invalid( dup, good ) );

    SolidMesh hole = good;
    hole.triangles.resize( 10 );
    hole.corner_uv.resize( 30 );
    TASK_CHECK( throws_invalid( hole, good ) );

    SolidMesh nan_mesh = good;
    nan_mesh.positions[0][0] = std::nan( "" );
    TASK_CHECK( throws_invalid( nan_mesh, good ) );

    SolidMesh degen = good;
    degen.triangles[0].v[1] = degen.triangles[0].v[0];
    TASK_CHECK( throws_invalid( degen, good ) );

    SolidMesh bad_uv = good;
    bad_uv.corner_uv.pop_back();
    TASK_CHECK( throws_invalid( bad_uv, good ) );

    SolidMesh isolated = good;
    isolated.positions.push_back( {{ 0.0, 0.0, 0.0 }} );
    TASK_CHECK( throws_invalid( isolated, good ) );

    SolidMesh empty_with_points;
    empty_with_points.positions.push_back( {{ 0.0, 0.0, 0.0 }} );
    TASK_CHECK( throws_invalid( empty_with_points, good ) );

    return 0;
}

static int case_unsupported_contact()
{
    SolidMesh a = make_box( -2, 2, -2, 2, -2, 2 );

    // Face-to-face tangency with coplanar overlap.
    SolidMesh tangent = make_box( 2.0, 3.0, -1.0, 1.0, -1.0, 1.0 );
    TASK_CHECK( throws_domain( a, tangent ) );

    // An original vertex of B landing on a face interior of A.
    SolidMesh vertex_touch = make_box( 2.0, 3.0, -0.5, 0.5, 1.5, 2.5 );
    TASK_CHECK( throws_domain( a, vertex_touch ) );

    // Coplanar overlapping triangles with the same plane.
    SolidMesh coplanar = make_box( 2.0 - 1e-12, 3.0, -0.5, 0.5, -0.5, 0.5 );
    TASK_CHECK( throws_domain( a, coplanar ) );

    return 0;
}

static int case_failure_preserves_result()
{
    SolidMesh a = make_box( -2, 2, -2, 2, -2, 2 );
    SolidMesh b = make_box( -0.5, 0.5, -0.5, 0.5, -0.5, 0.5 );
    DifferenceResult r = sentinel_result();
    SolidMesh bad = a;
    bad.triangles.resize( 8 );
    bad.corner_uv.resize( 24 );
    bool thrown = false;
    try
    {
        subtract_solid( bad, b, r );
    }
    catch( const std::invalid_argument& )
    {
        thrown = true;
    }
    TASK_CHECK( thrown );
    TASK_CHECK( r.mesh.positions.size() == 1 );
    TASK_CHECK( r.mesh.positions[0][2] == 9.0 );
    TASK_CHECK( r.mesh.corner_uv.size() == 1 );
    TASK_CHECK( r.origins.size() == 1 && r.origins[0].face == 999 );

    // A successful call after the failed one works normally.
    subtract_solid( a, b, r );
    TASK_CHECK( r.mesh.triangles.size() == 24 );
    TASK_CHECK( validate_structure( r.mesh ) );
    return 0;
}

} // anonymous namespace

extern const size_t task_case_count;
const task_case task_cases[] =
{
    { "intersections", "through_hole", case_through_hole },
    { "intersections", "closed_loop_inside_triangle", case_closed_loop_inside_triangle },
    { "intersections", "nonconvex_oblique", case_nonconvex_oblique },
    { "intersections", "cut_spans_non_coplanar_faces", case_cut_spans_non_coplanar_faces },
    { "solids", "disjoint", case_disjoint },
    { "solids", "enclosed_cavity", case_enclosed_cavity },
    { "solids", "engulfed", case_engulfed },
    { "solids", "empty_inputs", case_empty_inputs },
    { "solids", "split_into_remnants", case_split_into_remnants },
    { "solids", "two_inner_shells", case_two_inner_shells },
    { "provenance", "uv_seams_and_corner_order", case_uv_seams_and_corner_order },
    { "provenance", "reuse_and_independence", case_reuse_and_independence },
    { "provenance", "inputs_unchanged", case_inputs_unchanged },
    { "validation", "malformed_inputs", case_malformed_inputs },
    { "validation", "unsupported_contact", case_unsupported_contact },
    { "validation", "failure_preserves_result", case_failure_preserves_result }
};
const size_t task_case_count = sizeof( task_cases ) / sizeof( task_cases[0] );
