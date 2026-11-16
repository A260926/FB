// Tests for the continuous IQM skin surface.  No main(): the task driver in
// task_support/driver.c enumerates the registered task_cases.
//
// All surface math is recomputed here independently (double precision) instead
// of calling back into the production evaluator, so the checks can catch
// formula mistakes rather than mirror the implementation.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <array>
#include <map>
#include <set>
#include <algorithm>
#include <fstream>

#include "task_support/check.h"

#include "task_support/iqm_fixture.h"
#include "demo/skin_surface.h"

namespace skintest {

struct TVec
{
    double x, y, z;
    TVec(double x = 0, double y = 0, double z = 0) : x(x), y(y), z(z) {}
    TVec operator+(const TVec &o) const { return TVec(x+o.x, y+o.y, z+o.z); }
    TVec operator-(const TVec &o) const { return TVec(x-o.x, y-o.y, z-o.z); }
    TVec operator*(double k) const { return TVec(x*k, y*k, z*k); }
    double dot(const TVec &o) const { return x*o.x+y*o.y+z*o.z; }
    TVec cross(const TVec &o) const { return TVec(y*o.z-z*o.y, z*o.x-x*o.z, x*o.y-y*o.x); }
    double len() const { return std::sqrt(dot(*this)); }
};

// Independent 3x4 affine palette matrix (fourth row implicit 0,0,0,1).
struct TMat
{
    double m[3][4];
    TMat() { std::memset(m, 0, sizeof(m)); }
    TVec apply(const TVec &p) const
    {
        return TVec(m[0][0]*p.x+m[0][1]*p.y+m[0][2]*p.z+m[0][3],
                    m[1][0]*p.x+m[1][1]*p.y+m[1][2]*p.z+m[1][3],
                    m[2][0]*p.x+m[2][1]*p.y+m[2][2]*p.z+m[2][3]);
    }
    TVec applyVector(const TVec &p) const
    {
        return TVec(m[0][0]*p.x+m[0][1]*p.y+m[0][2]*p.z,
                    m[1][0]*p.x+m[1][1]*p.y+m[1][2]*p.z,
                    m[2][0]*p.x+m[2][1]*p.y+m[2][2]*p.z);
    }
    static TMat make(double m00,double m01,double m02,double m03,
                     double m10,double m11,double m12,double m13,
                     double m20,double m21,double m22,double m23)
    {
        TMat r;
        double v[12] = {m00,m01,m02,m03,m10,m11,m12,m13,m20,m21,m22,m23};
        for(int i = 0; i < 3; i++) for(int j = 0; j < 4; j++) r.m[i][j] = v[4*i+j];
        return r;
    }
};

Matrix3x4 toPalette(const TMat &t)
{
    return Matrix3x4(Vec4(float(t.m[0][0]),float(t.m[0][1]),float(t.m[0][2]),float(t.m[0][3])),
                     Vec4(float(t.m[1][0]),float(t.m[1][1]),float(t.m[1][2]),float(t.m[1][3])),
                     Vec4(float(t.m[2][0]),float(t.m[2][1]),float(t.m[2][2]),float(t.m[2][3])));
}

// ---- small bind-pose model builder ----------------------------------------

struct Model
{
    int numJoints = 0;
    std::vector<float> pos, uv;
    std::vector<uchar> bidx, bwgt;
    std::vector<iqmtriangle> tris;
    std::vector<iqmmesh> meshes;

    // weights as (joint, byte) pairs; bytes must sum to 255; remaining slots
    // are zero and may be placed deliberately for slot-order tests.
    int addVertex(const TVec &p, double u, double v,
                  const std::vector<std::pair<int,int>> &slots)
    {
        int id = (int)(pos.size()/3);
        pos.push_back(float(p.x)); pos.push_back(float(p.y)); pos.push_back(float(p.z));
        uv.push_back(float(u)); uv.push_back(float(v));
        uchar idx[4] = {0,0,0,0}, wgt[4] = {0,0,0,0};
        int sum = 0;
        for(size_t k = 0; k < slots.size() && k < 4; k++)
        {
            idx[k] = (uchar)slots[k].first;
            wgt[k] = (uchar)slots[k].second;
            sum += slots[k].second;
        }
        if(sum != 255) { std::fprintf(stderr, "test bug: weights sum %d\n", sum); std::exit(99); }
        for(int k = 0; k < 4; k++) { bidx.push_back(idx[k]); bwgt.push_back(wgt[k]); }
        return id;
    }
    int addTri(int a, int b, int c)
    {
        iqmtriangle t{ { (unsigned)a, (unsigned)b, (unsigned)c } };
        tris.push_back(t);
        return (int)tris.size()-1;
    }
    void addMesh(unsigned first, unsigned count)
    {
        iqmmesh m{};
        m.first_triangle = first;
        m.num_triangles = count;
        meshes.push_back(m);
    }
    SkinSurfaceBinding *bind() const
    {
        SkinSurfaceBinding *b = 0;
        bool ok = createSkinSurfaceBinding(b, pos.data(), uv.data(), bidx.data(), bwgt.data(),
                                           (int)(pos.size()/3), numJoints,
                                           tris.data(), (int)tris.size(),
                                           meshes.data(), (int)meshes.size());
        return ok ? b : 0;
    }
};

// Independent reference patch for one source triangle of a bound model.
struct RefPatch
{
    TVec P[3];
    double uv[3][2];
    std::map<int,double> w[3]; // merged weights per corner
    const TMat *pal;
    int nj;

    TVec position(const double b[3]) const
    {
        return P[0]*b[0] + P[1]*b[1] + P[2]*b[2];
    }
    TVec F(const double b[3]) const
    {
        TVec p = position(b);
        TVec r(0,0,0);
        for(int j = 0; j < nj; j++)
        {
            double wj = 0;
            for(int a = 0; a < 3; a++)
            {
                auto it = w[a].find(j);
                if(it != w[a].end()) wj += b[a]*it->second;
            }
            if(wj != 0.0) r = r + pal[j].apply(p)*wj;
        }
        return r;
    }
    TVec derivative(const double b[3], int k) const // dF/db_k, k in {1,2}, b0 eliminated
    {
        TVec p = position(b);
        TVec dp = P[k] - P[0]; // dp/db_k with b0 = 1-b1-b2
        TVec r(0,0,0);
        for(int j = 0; j < nj; j++)
        {
            double bk = 0, bb = 0;
            for(int a = 0; a < 3; a++)
            {
                auto it = w[a].find(j);
                double wa = it == w[a].end() ? 0.0 : it->second;
                bk += b[a]*wa;                 // w_j(b)
                bb += (a == k ? wa : (a == 0 ? -wa : 0.0)); // dw_j/db_k = w_kj - w_0j
            }
            // (dB_k)_j * p  (full affine apply, weight gradient carries translation)
            r = r + pal[j].apply(p)*bb;
            // B(b)_j * dp  (linear part only: dp is a direction)
            r = r + pal[j].applyVector(dp)*bk;
        }
        return r;
    }
    TVec normal(const double b[3]) const
    {
        // dF/db1 x dF/db2 exactly as the surface normal is specified.
        TVec d0 = derivative(b, 1), d1 = derivative(b, 2);
        TVec n = d0.cross(d1);
        return n * (1.0/n.len());
    }
    // Quadratic edge control-net deviation vectors d_ij computed straight from
    // the definition F(b) = sum w_j(b) M_j p(b).
    void edgeDev(int i, int j, TVec &d) const
    {
        double bi[3] = {0,0,0}, bj[3] = {0,0,0}, bm[3] = {0,0,0};
        bi[i] = 1; bj[j] = 1; bm[i] = bm[j] = 0.5;
        TVec qi = F(bi), qj = F(bj), qedge = F(bm);
        d = qedge*2.0 - qi - qj;
    }
};

RefPatch makeRef(const Model &m, const std::vector<TMat> &pal, int tri)
{
    RefPatch r;
    r.pal = pal.data();
    r.nj = (int)pal.size();
    const iqmtriangle &t = m.tris[tri];
    for(int a = 0; a < 3; a++)
    {
        int v = (int)t.vertex[a];
        r.P[a] = TVec(m.pos[3*v], m.pos[3*v+1], m.pos[3*v+2]);
        r.uv[a][0] = m.uv[2*v];
        r.uv[a][1] = m.uv[2*v+1];
        for(int s = 0; s < 4; s++)
        {
            uchar bw = m.bwgt[4*v+s];
            if(!bw) continue;
            r.w[a][m.bidx[4*v+s]] += double(bw)/255.0;
        }
    }
    return r;
}

bool nearVec(const TVec &a, const TVec &b, double tol = 1e-6)
{
    return (a-b).len() <= tol*(1.0 + std::max(a.len(), b.len()));
}

// Dense barycentric grid over the unit simplex (grid n per edge).
std::vector<std::array<double,3>> simplexGrid(int n)
{
    std::vector<std::array<double,3>> out;
    for(int i = 0; i <= n; i++)
        for(int j = 0; j <= n-i; j++)
        {
            int k = n-i-j;
            out.push_back({ double(i)/n, double(j)/n, double(k)/n });
        }
    return out;
}

// Independent degree-2 Bernstein coefficient maximum of g over a sub-triangle
// with root-barycentric corners q0,q1,q2.
double independentBound(const RefPatch &ref, const double q0[3], const double q1[3], const double q2[3])
{
    double cost[3];
    const int ei[3] = {0,1,2}, ej[3] = {1,2,2};
    for(int e = 0; e < 3; e++)
    {
        TVec d;
        ref.edgeDev(ei[e], ej[e], d);
        cost[e] = d.len();
    }
    auto prod = [&](int i, int j, const double q[3]) { return q[i]*q[j]; };
    double g[6];
    const double *q[3] = { q0, q1, q2 };
    for(int k = 0; k < 6; k++)
    {
        g[k] = 0;
        static const int mi[6][2] = { {0,0},{1,1},{2,2},{0,1},{0,2},{1,2} };
        for(int e = 0; e < 3; e++)
        {
            int i = ei[e], j = ej[e];
            double c;
            if(k < 3) c = prod(i, j, q[k]);
            else
            {
                int a = mi[k][0], bb = mi[k][1];
                c = 0.5*(q[a][i]*q[bb][j] + q[bb][i]*q[a][j]);
            }
            g[k] += cost[e]*c;
        }
    }
    double mx = g[0];
    for(int k = 1; k < 6; k++) mx = std::max(mx, g[k]);
    return mx;
}

// Verify one published mesh against independent math.  Returns the number of
// numeric checks performed (the caller wraps the TASK_CHECKs).
int verifyMeshNumeric(const SkinSurfaceMesh *mesh, const Model &m,
                      const std::vector<TMat> &pal, double tolerance)
{
    int checks = 0;
    TASK_CHECK(mesh != 0);
    TASK_CHECK(mesh->corners.size() == 3*mesh->triangles.size());
    TASK_CHECK(mesh->numSourceTriangles == m.tris.size());
    ++checks;

    // Every node position must equal the independently evaluated F at its
    // barycentric coordinates; normals must match analytic derivatives.
    for(size_t ti = 0; ti < mesh->triangles.size(); ti++)
    {
        const SkinSurfaceTriangle &st = mesh->triangles[ti];
        RefPatch ref = makeRef(m, pal, st.sourceTriangle);
        for(int k = 0; k < 3; k++)
        {
            const SkinSurfaceCorner &c = mesh->corners[3*ti+k];
            double b[3] = { c.barycentric[0], c.barycentric[1], c.barycentric[2] };
            TVec want = ref.F(b);
            Vec3 got = mesh->positions[st.vertex[k]];
            TASK_CHECK(nearVec(want, TVec(got.x, got.y, got.z), 2e-5));
            TVec wn = ref.normal(b);
            TVec gn(c.unitNormal[0], c.unitNormal[1], c.unitNormal[2]);
            TASK_CHECK(nearVec(wn, gn, 5e-5));
            TASK_CHECK(std::fabs(gn.len()-1.0) < 1e-5);
            double uuv[2] = {0,0};
            for(int a = 0; a < 3; a++)
            {
                uuv[0] += b[a]*ref.uv[a][0];
                uuv[1] += b[a]*ref.uv[a][1];
            }
            TASK_CHECK(std::fabs(uuv[0]-c.uv[0]) < 1e-5);
            TASK_CHECK(std::fabs(uuv[1]-c.uv[1]) < 1e-5);
            ++checks;
        }

        // Dense probes: the reported errorBound dominates the true deviation
        // of F from the linear interpolation of the output triangle, over the
        // whole closed parameter triangle.
        double q0[3] = { mesh->corners[3*ti].barycentric[0], mesh->corners[3*ti].barycentric[1], mesh->corners[3*ti].barycentric[2] };
        double q1[3] = { mesh->corners[3*ti+1].barycentric[0], mesh->corners[3*ti+1].barycentric[1], mesh->corners[3*ti+1].barycentric[2] };
        double q2[3] = { mesh->corners[3*ti+2].barycentric[0], mesh->corners[3*ti+2].barycentric[1], mesh->corners[3*ti+2].barycentric[2] };
        // Build the linear interpolant from the double-precision reference
        // corner values (not the float-stored nodes), so this measures the
        // genuine surface deviation rather than vertex float quantization.
        // Float-node accuracy against F is already checked corner by corner.
        TVec l0 = ref.F(q0), l1 = ref.F(q1), l2 = ref.F(q2);
        double worst = 0;
        for(const std::array<double,3> &s : simplexGrid(10))
        {
            double b[3] = { q0[0]*s[0]+q1[0]*s[1]+q2[0]*s[2],
                            q0[1]*s[0]+q1[1]*s[1]+q2[1]*s[2],
                            q0[2]*s[0]+q1[2]*s[1]+q2[2]*s[2] };
            TVec f = ref.F(b);
            TVec lin = l0*s[0] + l1*s[1] + l2*s[2];
            worst = std::max(worst, (f-lin).len());
        }
        TASK_CHECK(worst <= double(st.errorBound)*(1.0+1e-6) + 1e-11);
        TASK_CHECK(double(st.errorBound) <= tolerance*(1.0+1e-6));
        // Independent quadratic control-net bound, recomputed from F alone.
        // g = sum b_i b_j ||d_ij|| is an independent conservative upper bound
        // (triangle-inequality form), so it must also dominate the measured
        // deviation; production's Bernstein-remainder bound is allowed to be
        // tighter than it.
        double ib = independentBound(ref, q0, q1, q2);
        TASK_CHECK(ib + 1e-12 >= worst);
        ++checks;
    }
    return checks;
}

// ===========================================================================
// surface group
// ===========================================================================

int run_surface_quadratic()
{
    Model m;
    m.numJoints = 2;
    int v0 = m.addVertex(TVec(0,0,0), 0, 0, {{0,255}});
    int v1 = m.addVertex(TVec(2,0,0), 1, 0, {{0,128},{1,127}});
    int v2 = m.addVertex(TVec(0,2,0), 0, 1, {{1,255}});
    m.addTri(v0, v1, v2);
    m.addMesh(0, 1);

    // Non-uniform scale, rotations and translations: deliberately not a rigid
    // palette and not commuting with each other.
    std::vector<TMat> pal(2);
    pal[0] = TMat::make(1.5, 0.1, 0.0, 0.7,
                        0.0, 0.8, 0.2, -1.0,
                        0.1, 0.0, 1.2, 0.3);
    pal[1] = TMat::make(0.9, -0.3, 0.1, 1.1,
                        0.2, 1.1, 0.0, 0.4,
                        0.0, 0.1, 0.7, -0.6);
    Matrix3x4 pp[2] = { toPalette(pal[0]), toPalette(pal[1]) };

    SkinSurfaceBinding *b = m.bind();
    TASK_CHECK(b != 0);
    SkinSurfaceOptions opt;
    opt.chordTolerance = 1e-1;
    opt.maxDepth = 10;
    opt.maxTriangles = 200000;
    TASK_CHECK(evaluateSkinSurface(b, pp, 2, opt));
    const SkinSurfaceMesh *mesh = getSkinSurfaceDrawMesh(b);
    TASK_CHECK(mesh != 0 && mesh->triangles.size() >= 1);
    size_t looseTriCount = mesh->triangles.size();

    verifyMeshNumeric(mesh, m, pal, opt.chordTolerance);

    RefPatch ref = makeRef(m, pal, 0);

    // The surface is genuinely curved: midpoint F differs from the average of
    // the two skinned endpoints (weight cross term b0*b1).
    double bm[3] = {0.5,0.5,0.0}, b0[3] = {1,0,0}, b1[3] = {0,1,0};
    TVec fmid = ref.F(bm), f0 = ref.F(b0), f1 = ref.F(b1);
    TASK_CHECK((fmid - (f0+f1)*0.5).len() > 1e-4);
    TVec d01;
    ref.edgeDev(0, 1, d01);
    // d_01 = 2*F(mid) - F0 - F1, so F(mid) - (F0+F1)/2 = d_01/2.
    TASK_CHECK(nearVec(fmid - (f0+f1)*0.5, d01*0.5, 1e-10));

    // Finite-difference cross check of the analytic derivative (weight
    // gradient included): this would fail for a skinned-endpoint chord scheme.
    double h = 1e-6;
    double bp[3] = {0.3, 0.45, 0.25};
    double bu[3] = {0.3-h, 0.45+h, 0.25};
    double bv[3] = {0.3-h, 0.45, 0.25+h};
    TVec fd1 = (ref.F(bu) - ref.F(bp))*(1.0/h);
    TVec fd2 = (ref.F(bv) - ref.F(bp))*(1.0/h);
    TASK_CHECK(nearVec(fd1, ref.derivative(bp, 1), 2e-5));
    TASK_CHECK(nearVec(fd2, ref.derivative(bp, 2), 2e-5));

    // Tighter tolerance must refine this curved patch across grid levels.
    SkinSurfaceOptions tight = opt;
    tight.chordTolerance = 1e-4;
    TASK_CHECK(evaluateSkinSurface(b, pp, 2, tight));
    TASK_CHECK(getSkinSurfaceDrawMesh(b)->triangles.size() > looseTriCount);
    verifyMeshNumeric(getSkinSurfaceDrawMesh(b), m, pal, tight.chordTolerance);

    destroySkinSurfaceBinding(b);
    TASK_CHECK(b == 0);
    return 0;
}

int run_surface_slot_reorder()
{
    // Two one-triangle models with identical merged weights, but the second
    // permutes slot order, inserts a zero slot and merges two bones split over
    // separate slots in the first model.
    Model a, c;
    a.numJoints = c.numJoints = 3;
    int a0 = a.addVertex(TVec(0,0,0), 0, 0, {{0,100},{1,100},{2,55}});
    int a1 = a.addVertex(TVec(1,0,0), 1, 0, {{2,255}});
    int a2 = a.addVertex(TVec(0,1,0), 0, 1, {{1,200},{0,55}});
    a.addTri(a0, a1, a2); a.addMesh(0, 1);

    int c0 = c.addVertex(TVec(0,0,0), 0, 0, {{2,55},{0,0},{1,100},{0,100}});
    int c1 = c.addVertex(TVec(1,0,0), 1, 0, {{2,255},{0,0},{1,0},{0,0}});
    int c2 = c.addVertex(TVec(0,1,0), 0, 1, {{0,55},{2,0},{1,200},{0,0}});
    c.addTri(c0, c1, c2); c.addMesh(0, 1);

    std::vector<TMat> pal(3);
    pal[0] = TMat::make(1.2,.1,.2,.3, .0,.9,.1,-.2, .2,.1,1.1,.5);
    pal[1] = TMat::make(.8,.2,.0,.9, .1,1.3,.2,.1, .0,.2,.7,-.4);
    pal[2] = TMat::make(1.0,.3,.1,-.6, .2,.7,.2,.4, .1,.0,1.2,.2);
    Matrix3x4 pp[3] = { toPalette(pal[0]), toPalette(pal[1]), toPalette(pal[2]) };

    SkinSurfaceBinding *ba = a.bind(), *bc = c.bind();
    TASK_CHECK(ba && bc);
    SkinSurfaceOptions opt; opt.chordTolerance = 1e-3; opt.maxDepth = 8;
    TASK_CHECK(evaluateSkinSurface(ba, pp, 3, opt));
    TASK_CHECK(evaluateSkinSurface(bc, pp, 3, opt));
    const SkinSurfaceMesh *ma = getSkinSurfaceDrawMesh(ba), *mc = getSkinSurfaceDrawMesh(bc);
    TASK_CHECK(ma->triangles.size() == mc->triangles.size());
    TASK_CHECK(ma->positions.size() == mc->positions.size());

    // Same merged weights and palette must produce the same surface nodes.
    for(size_t i = 0; i < ma->positions.size(); i++)
        TASK_CHECK(nearVec(TVec(ma->positions[i].x,ma->positions[i].y,ma->positions[i].z),
                           TVec(mc->positions[i].x,mc->positions[i].y,mc->positions[i].z), 1e-6));

    // Single-bone vertex: F is exactly one affine transform, and the corner
    // matches it directly.  Locate the (b0,b1,b2)=(0,1,0) root corner node by
    // its recorded barycentric rather than assuming a leaf layout.
    RefPatch ref = makeRef(a, pal, 0);
    double be[3] = {0,1,0};
    int cornerNode = -1;
    for(size_t ti = 0; ti < ma->triangles.size() && cornerNode < 0; ti++)
        for(int k = 0; k < 3; k++)
        {
            const SkinSurfaceCorner &cc = ma->corners[3*ti+k];
            if(ma->triangles[ti].sourceTriangle == 0
               && std::fabs(cc.barycentric[1]-1.0) < 1e-12)
            {
                cornerNode = (int)ma->triangles[ti].vertex[k];
                break;
            }
        }
    TASK_CHECK(cornerNode >= 0);
    Vec3 got = ma->positions[cornerNode];
    TASK_CHECK(nearVec(ref.F(be), TVec(got.x,got.y,got.z), 1e-6));
    TASK_CHECK(nearVec(pal[2].apply(TVec(1,0,0)), TVec(got.x,got.y,got.z), 1e-6));

    destroySkinSurfaceBinding(ba);
    destroySkinSurfaceBinding(bc);
    return 0;
}

int run_surface_hierarchy()
{
    // Non-commuting parent/child style matrices, non-uniform scale and an
    // arbitrary inverse-bind contribution (folded into the palette exactly as
    // animateiqm's outframe already is).  Cross-check against the fixture's
    // independently generated palette as well.
    Model m;
    m.numJoints = 2;
    int v0 = m.addVertex(TVec(0.5,-0.5,0.25), 0, 0, {{0,255}});
    int v1 = m.addVertex(TVec(1.5,-0.5,0.25), 1, 0, {{0,153},{1,102}});
    int v2 = m.addVertex(TVec(0.5,0.75,0.25), 0, 1, {{1,255}});
    m.addTri(v0, v1, v2);
    m.addMesh(0, 1);

    std::vector<TMat> pal(2);
    pal[0] = TMat::make(2.0,0.0,0.0, 1.0,  0.0,0.5,0.0,-2.0,  0.0,0.0,1.5,0.5);
    // rotation-ish matrix so M0*M1 != M1*M0
    pal[1] = TMat::make(0.0,-1.0,0.0, 3.0,  1.0,0.0,0.0, 1.0,  0.0,0.0,0.7,0.0);
    Matrix3x4 pp[2] = { toPalette(pal[0]), toPalette(pal[1]) };
    {
        // demonstrate non-commutativity numerically with independent math
        TVec p(0.3, -0.2, 0.7);
        TVec ab = pal[0].apply(pal[1].apply(p));
        TVec ba = pal[1].apply(pal[0].apply(p));
        TASK_CHECK((ab-ba).len() > 1e-3);
    }

    SkinSurfaceBinding *b = m.bind();
    TASK_CHECK(b);
    SkinSurfaceOptions opt; opt.chordTolerance = 1e-5; opt.maxDepth = 9;
    TASK_CHECK(evaluateSkinSurface(b, pp, 2, opt));
    verifyMeshNumeric(getSkinSurfaceDrawMesh(b), m, pal, opt.chordTolerance);

    // Repeat against the real fixture palette (non-identity inverse base pose,
    // parent/child chain and non-uniform animated scaling).
    fixture::Model fx;
    std::array<fixture::A,2> fpal = fx.palette(1.625);
    std::vector<TMat> fp(2);
    for(int j = 0; j < 2; j++)
        for(int r = 0; r < 3; r++)
            for(int cc = 0; cc < 4; cc++) fp[j].m[r][cc] = fpal[j][r][cc];
    Matrix3x4 fpp[2] = { toPalette(fp[0]), toPalette(fp[1]) };
    Model fm;
    fm.numJoints = 2;
    int f0 = fm.addVertex(TVec(fx.positions[0],fx.positions[1],fx.positions[2]), 0, 0,
                          {{fx.indices[0],fx.weights[0]},{fx.indices[1],fx.weights[1]},
                           {fx.indices[2],fx.weights[2]},{fx.indices[3],fx.weights[3]}});
    int f1 = fm.addVertex(TVec(fx.positions[3],fx.positions[4],fx.positions[5]), 1, 0,
                          {{fx.indices[4],fx.weights[4]},{fx.indices[5],fx.weights[5]},
                           {fx.indices[6],fx.weights[6]},{fx.indices[7],fx.weights[7]}});
    int f2 = fm.addVertex(TVec(fx.positions[6],fx.positions[7],fx.positions[8]), 0, 1,
                          {{fx.indices[8],fx.weights[8]},{fx.indices[9],fx.weights[9]},
                           {fx.indices[10],fx.weights[10]},{fx.indices[11],fx.weights[11]}});
    fm.addTri(f0, f1, f2); fm.addMesh(0, 1);
    SkinSurfaceBinding *fb = fm.bind();
    TASK_CHECK(fb);
    TASK_CHECK(evaluateSkinSurface(fb, fpp, 2, opt));
    verifyMeshNumeric(getSkinSurfaceDrawMesh(fb), fm, fp, opt.chordTolerance);

    destroySkinSurfaceBinding(b);
    destroySkinSurfaceBinding(fb);
    return 0;
}

int run_surface_failures()
{
    Model m;
    m.numJoints = 2;
    int v0 = m.addVertex(TVec(0,0,0), 0, 0, {{0,255}});
    int v1 = m.addVertex(TVec(1,0,0), 1, 0, {{0,128},{1,127}});
    int v2 = m.addVertex(TVec(0,1,0), 0, 1, {{1,255}});
    m.addTri(v0, v1, v2);
    m.addMesh(0, 1);

    // Binding validation failures.
    SkinSurfaceBinding *bad = 0;
    Model badW = m;
    badW.bwgt[1] = 127; // sum no longer 255
    TASK_CHECK(!createSkinSurfaceBinding(bad, badW.pos.data(), badW.uv.data(), badW.bidx.data(), badW.bwgt.data(),
                                         3, 2, badW.tris.data(), 1, badW.meshes.data(), 1));
    TASK_CHECK(bad == 0);
    Model badBone = m;
    badBone.bidx[4] = 7; // out of range on a weighted slot
    TASK_CHECK(!createSkinSurfaceBinding(bad, badBone.pos.data(), badBone.uv.data(), badBone.bidx.data(), badBone.bwgt.data(),
                                         3, 2, badBone.tris.data(), 1, badBone.meshes.data(), 1));
    Model deg = m;
    deg.tris[0].vertex[2] = v0; // repeated corner
    TASK_CHECK(!createSkinSurfaceBinding(bad, deg.pos.data(), deg.uv.data(), deg.bidx.data(), deg.bwgt.data(),
                                         3, 2, deg.tris.data(), 1, deg.meshes.data(), 1));
    iqmmesh badRange[1]{};
    badRange[0].first_triangle = 0; badRange[0].num_triangles = 2; // exceeds triangle count
    TASK_CHECK(!createSkinSurfaceBinding(bad, m.pos.data(), m.uv.data(), m.bidx.data(), m.bwgt.data(),
                                         3, 2, m.tris.data(), 1, badRange, 1));
    TASK_CHECK(!createSkinSurfaceBinding(bad, 0, m.uv.data(), m.bidx.data(), m.bwgt.data(),
                                         3, 2, m.tris.data(), 1, m.meshes.data(), 1));
    // Inputs untouched by failed calls.
    TASK_CHECK(m.pos[0] == 0.0f && m.bwgt[0] == 255 && m.tris[0].vertex[1] == (unsigned)v1);

    std::vector<TMat> pal(2);
    pal[0] = TMat::make(1,0,0,0, 0,1,0,0, 0,0,1,0);
    pal[1] = TMat::make(.9,.1,0,.2, .1,1.1,0,.1, 0,0,.8,0);
    Matrix3x4 pp[2] = { toPalette(pal[0]), toPalette(pal[1]) };

    SkinSurfaceBinding *b = m.bind();
    TASK_CHECK(b);
    SkinSurfaceOptions opt; opt.chordTolerance = 1e-4; opt.maxDepth = 6;
    TASK_CHECK(evaluateSkinSurface(b, pp, 2, opt));
    const SkinSurfaceMesh *good = getSkinSurfaceDrawMesh(b);
    unsigned long long seq = good->sequence;
    size_t npos = good->positions.size(), ntri = good->triangles.size();

    // Illegal options fail and keep the previous snapshot.
    SkinSurfaceOptions bad1 = opt; bad1.chordTolerance = 0.0;
    TASK_CHECK(!evaluateSkinSurface(b, pp, 2, bad1));
    bad1.chordTolerance = std::nan("");
    TASK_CHECK(!evaluateSkinSurface(b, pp, 2, bad1));
    SkinSurfaceOptions bad2 = opt; bad2.maxTriangles = 0;
    TASK_CHECK(!evaluateSkinSurface(b, pp, 2, bad2));
    SkinSurfaceOptions bad3 = opt; bad3.maxDepth = 63;
    TASK_CHECK(!evaluateSkinSurface(b, pp, 2, bad3));

    // Singular and non-finite palette matrices fail.
    TMat singular = TMat::make(1,0,0,0, 0,1,0,0, 0,0,0,0);
    Matrix3x4 ps = toPalette(singular);
    TASK_CHECK(!evaluateSkinSurface(b, &ps, 1, opt));
    TMat nf = pal[0]; nf.m[1][2] = std::nan("");
    Matrix3x4 pn = toPalette(nf);
    TASK_CHECK(!evaluateSkinSurface(b, &pn, 1, opt));
    TASK_CHECK(!evaluateSkinSurface(0, pp, 2, opt));
    TASK_CHECK(!evaluateSkinSurface(b, pp, 1, opt)); // joint count mismatch

    // Previous complete output is unchanged.
    const SkinSurfaceMesh *after = getSkinSurfaceDrawMesh(b);
    TASK_CHECK(after == good && after->sequence == seq);
    TASK_CHECK(after->positions.size() == npos && after->triangles.size() == ntri);

    destroySkinSurfaceBinding(b);
    return 0;
}

int run_surface_degenerate_derivative()
{
    // Two invertible palettes whose 50/50 blend is singular at the edge
    // midpoint, with the midpoint bind position at the origin so both
    // derivative terms vanish there.  Each corner is still well behaved.
    Model m;
    m.numJoints = 2;
    int v0 = m.addVertex(TVec(-1,0,0), 0, 0, {{0,255}});
    int v1 = m.addVertex(TVec( 1,0,0), 1, 0, {{1,255}});
    int v2 = m.addVertex(TVec( 0,1,0), 0, 1, {{0,255}});
    m.addTri(v0, v1, v2);
    m.addMesh(0, 1);

    std::vector<TMat> pal(2);
    pal[0] = TMat::make(2,0,0, 0.5,  0,2,0,-0.5,  0,0,2,0.25);
    pal[1] = TMat::make(-2,0,0, 0.5,  0,-2,0,-0.5,  0,0,-2,0.25);
    Matrix3x4 pp[2] = { toPalette(pal[0]), toPalette(pal[1]) };
    TASK_CHECK(std::fabs(pal[0].m[0][0]*pal[0].m[1][1]*pal[0].m[2][2]) > 0.1);
    TASK_CHECK(std::fabs(pal[1].m[0][0]*pal[1].m[1][1]*pal[1].m[2][2]) > 0.1);

    SkinSurfaceBinding *b = m.bind();
    TASK_CHECK(b);
    // A tolerance above the root control-net bound lets depth 0 emit the
    // single root triangle without ever sampling the degenerate edge midpoint.
    SkinSurfaceOptions opt; opt.chordTolerance = 10.0; opt.maxDepth = 0;
    TASK_CHECK(evaluateSkinSurface(b, pp, 2, opt));
    TASK_CHECK(getSkinSurfaceDrawMesh(b)->triangles.size() == 1);

    // Refinement samples the degenerate midpoint -> explicit failure, previous
    // snapshot retained.
    unsigned long long seq = getSkinSurfaceDrawMesh(b)->sequence;
    opt.maxDepth = 2;
    opt.chordTolerance = 0.6;
    TASK_CHECK(!evaluateSkinSurface(b, pp, 2, opt));
    TASK_CHECK(getSkinSurfaceDrawMesh(b)->sequence == seq);
    TASK_CHECK(std::strstr(getSkinSurfaceError(b), "derivative") != 0);

    destroySkinSurfaceBinding(b);
    return 0;
}

// ===========================================================================
// topology group
// ===========================================================================

// Collect, per source triangle, the output triangles and node barycentrics.
struct Topo
{
    std::vector<std::vector<unsigned>> leaves; // source tri -> output tri ids
    std::map<unsigned, std::array<double,3>> nodeBary; // node id -> bary (per source)
};

int run_topology_shared_edge()
{
    // Two triangles sharing edge (0,1) with opposite local orientation, in
    // different material segments.
    Model m;
    m.numJoints = 2;
    int v0 = m.addVertex(TVec(0,0,0), 0, 0, {{0,255}});
    int v1 = m.addVertex(TVec(2,0,0), 1, 0, {{0,128},{1,127}});
    int v2 = m.addVertex(TVec(0,2,0), 0, 1, {{1,255}});
    int v3 = m.addVertex(TVec(0,0,2), 1, 1, {{0,64},{1,191}});
    m.addTri(v0, v1, v2);
    m.addTri(v1, v0, v3);
    m.addMesh(0, 1);
    m.addMesh(1, 1);

    std::vector<TMat> pal(2);
    pal[0] = TMat::make(1.3,.1,0,.2, 0,.9,.2,-.3, .1,0,1.1,.4);
    pal[1] = TMat::make(.8,.2,.1,.5, .2,1.1,0,.1, 0,.1,.7,-.2);
    Matrix3x4 pp[2] = { toPalette(pal[0]), toPalette(pal[1]) };

    SkinSurfaceBinding *b = m.bind();
    TASK_CHECK(b);
    SkinSurfaceOptions opt; opt.chordTolerance = 2e-5; opt.maxDepth = 8;
    TASK_CHECK(evaluateSkinSurface(b, pp, 2, opt));
    const SkinSurfaceMesh *mesh = getSkinSurfaceDrawMesh(b);
    TASK_CHECK(mesh->materialRanges.size() == 2);
    TASK_CHECK(mesh->materialRanges[0].numTriangles >= 1);
    TASK_CHECK(mesh->materialRanges[1].numTriangles >= 1);
    TASK_CHECK(mesh->materialRanges[0].firstTriangle == 0);
    TASK_CHECK(mesh->materialRanges[1].firstTriangle
               == mesh->materialRanges[0].firstTriangle + mesh->materialRanges[0].numTriangles);

    // Map source triangle -> leaves and node -> barycentric.
    std::map<int, std::vector<unsigned>> leaves;
    std::map<int, std::map<unsigned, std::array<double,3>>> bary;
    for(unsigned ti = 0; ti < mesh->triangles.size(); ti++)
    {
        int src = mesh->triangles[ti].sourceTriangle;
        leaves[src].push_back(ti);
        for(int k = 0; k < 3; k++)
        {
            unsigned nid = mesh->triangles[ti].vertex[k];
            bary[src][nid] = { mesh->corners[3*ti+k].barycentric[0],
                               mesh->corners[3*ti+k].barycentric[1],
                               mesh->corners[3*ti+k].barycentric[2] };
        }
    }
    TASK_CHECK(leaves[0].size() >= 4 && leaves[1].size() >= 4);

    // Barycentric coverage: parametric areas sum to the root triangle area 1/2.
    for(int src = 0; src < 2; src++)
    {
        double area = 0;
        std::set<std::array<long long,3>> centroids;
        for(unsigned ti : leaves[src])
        {
            double q[3][3];
            for(int k = 0; k < 3; k++)
                for(int c = 0; c < 3; c++)
                    q[k][c] = mesh->corners[3*ti+k].barycentric[c];
            double du[3] = { q[1][0]-q[0][0], q[1][1]-q[0][1], q[1][2]-q[0][2] };
            double dv[3] = { q[2][0]-q[0][0], q[2][1]-q[0][1], q[2][2]-q[0][2] };
            double det = du[0]*dv[1]-du[1]*dv[0];
            area += std::fabs(det)*0.5;
            std::array<long long,3> key;
            for(int c = 0; c < 3; c++)
                key[c] = (long long)std::llround((q[0][c]+q[1][c]+q[2][c])/3.0*1e9);
            TASK_CHECK(centroids.insert(key).second); // no overlapping leaves
        }
        TASK_CHECK(std::fabs(area-0.5) < 1e-9);
    }

    // Shared edge (v0,v1): gather the ordered node chain from each side.
    // v0Local is the source-triangle corner index that v0 occupies, so the
    // map key is the canonical v0->v1 edge parameter on both sides.
    auto edgeChain = [&](int src, int v0Local)
    {
        std::map<double, unsigned> chain; // canonical v0->v1 parameter vs node id
        for(auto &kv : bary[src])
        {
            const std::array<double,3> &bb = kv.second;
            if(std::fabs(bb[2]) < 1e-12)
                chain[bb[v0Local]] = kv.first;
        }
        return chain;
    };
    std::map<double, unsigned> chain0 = edgeChain(0, 0); // tri0 corners (v0,v1,v2)
    std::map<double, unsigned> chain1 = edgeChain(1, 1); // tri1 corners (v1,v0,v3)
    TASK_CHECK(chain0.size() >= 3 && chain0.size() == chain1.size());
    // Same physical subdivision points and the same shared position indices.
    auto it0 = chain0.begin();
    auto it1 = chain1.begin();
    for(; it0 != chain0.end(); ++it0, ++it1)
    {
        TASK_CHECK(std::fabs(it0->first - it1->first) < 1e-12);
        TASK_CHECK(it0->second == it1->second);
    }
    // No T-junction: every leaf edge lying on the root edge joins two
    // consecutive fractions of that face's shared chain.  Each face uses its
    // own chain because the local corner ordering differs across the edge.
    std::map<double, unsigned> chains[2] = { chain0, chain1 };
    int v0Locals[2] = { 0, 1 };
    for(int src = 0; src < 2; src++)
    {
        const std::map<double, unsigned> &chain = chains[src];
        int v0Local = v0Locals[src];
        std::vector<double> fracs;
        for(auto &kv : chain) fracs.push_back(kv.first);
        for(unsigned ti : leaves[src])
        {
            for(int e = 0; e < 3; e++)
            {
                unsigned na = mesh->triangles[ti].vertex[e];
                unsigned nb = mesh->triangles[ti].vertex[(e+1)%3];
                auto pa = bary[src].find(na), pb = bary[src].find(nb);
                TASK_CHECK(pa != bary[src].end() && pb != bary[src].end());
                bool aOn = pa->second[2] < 1e-12, bOn = pb->second[2] < 1e-12;
                if(aOn && bOn)
                {
                    double fa = pa->second[v0Local], fb = pb->second[v0Local];
                    auto ia = chain.find(fa), ib = chain.find(fb);
                    TASK_CHECK(ia != chain.end() && ib != chain.end());
                    size_t ka = (size_t)std::distance(chain.begin(), ia);
                    size_t kb = (size_t)std::distance(chain.begin(), ib);
                    TASK_CHECK(ka < kb ? kb - ka == 1 : ka - kb == 1);
                }
            }
        }
        // The chain fractions are uniformly spaced (dyadic grid), so the
        // segment length equals 1/(N-1) with no gaps or duplicates.
        TASK_CHECK(fracs.size() >= 3);
        double step = fracs[1] - fracs[0];
        TASK_CHECK(step > 0);
        for(size_t k = 1; k < fracs.size(); k++)
            TASK_CHECK(std::fabs((fracs[k]-fracs[k-1]) - step) < 1e-12);
        (void)fracs;
    }

    // Winding: every leaf normal points to the same side as the analytic
    // normal at its parametric centroid.
    for(int src = 0; src < 2; src++)
    {
        RefPatch ref = makeRef(m, pal, src);
        for(unsigned ti : leaves[src])
        {
            Vec3 a = mesh->positions[mesh->triangles[ti].vertex[0]];
            Vec3 bb = mesh->positions[mesh->triangles[ti].vertex[1]];
            Vec3 c = mesh->positions[mesh->triangles[ti].vertex[2]];
            TVec geo = (TVec(bb.x,bb.y,bb.z)-TVec(a.x,a.y,a.z))
                       .cross(TVec(c.x,c.y,c.z)-TVec(a.x,a.y,a.z));
            double cb[3] = {0,0,0};
            for(int k = 0; k < 3; k++)
                for(int q = 0; q < 3; q++) cb[q] += mesh->corners[3*ti+k].barycentric[q]/3.0;
            TASK_CHECK(geo.dot(ref.normal(cb)) > 0.0);
        }
    }

    // Per-corner UVs remain face-specific even on the shared edge.
    for(unsigned ti : leaves[0])
        for(int k = 0; k < 3; k++)
        {
            const SkinSurfaceCorner &cc = mesh->corners[3*ti+k];
            TASK_CHECK(std::isfinite(cc.uv[0]) && std::isfinite(cc.uv[1]));
        }
    verifyMeshNumeric(mesh, m, pal, opt.chordTolerance);

    destroySkinSurfaceBinding(b);
    return 0;
}

int run_topology_seam_and_affine()
{
    // Vertices 4 and 5 occupy the same position but have distinct indices:
    // the unindexed seam must not be welded.  A separate single-bone triangle
    // is an affine, disconnected component that must stay one triangle.
    Model m;
    m.numJoints = 2;
    int v0 = m.addVertex(TVec(0,0,0), 0, 0, {{0,255}});
    int v1 = m.addVertex(TVec(1,0,0), 1, 0, {{0,128},{1,127}});
    int v2 = m.addVertex(TVec(0,1,0), 0, 1, {{1,255}});
    int v3 = m.addVertex(TVec(0,0,1), 1, 1, {{0,128},{1,127}});
    int v4 = m.addVertex(TVec(1,0,0), 7, 7, {{1,255}}); // same xyz as v1, own UV
    int v5 = m.addVertex(TVec(3,3,3), 0, 0, {{0,255}});
    int v6 = m.addVertex(TVec(4,3,3), 1, 0, {{0,255}});
    int v7 = m.addVertex(TVec(3,4,3), 0, 1, {{0,255}});
    m.addTri(v0, v1, v2);
    m.addTri(v0, v4, v3); // shares position (1,0,0) only through duplicate index
    m.addTri(v5, v6, v7); // affine disconnected component
    m.addMesh(0, 2);
    m.addMesh(2, 1);

    std::vector<TMat> pal(2);
    pal[0] = TMat::make(1.4,.2,0,.1, 0,.8,.1,.2, .2,0,1.2,-.3);
    pal[1] = TMat::make(.7,.3,.1,.4, .1,1.2,.2,-.1, .1,.1,.9,.2);
    Matrix3x4 pp[2] = { toPalette(pal[0]), toPalette(pal[1]) };

    SkinSurfaceBinding *b = m.bind();
    TASK_CHECK(b);
    SkinSurfaceOptions opt; opt.chordTolerance = 1e-3; opt.maxDepth = 9; opt.maxTriangles = 2000000;
    TASK_CHECK(evaluateSkinSurface(b, pp, 2, opt));
    const SkinSurfaceMesh *mesh = getSkinSurfaceDrawMesh(b);

    // The curved first two triangles refine; the affine component stays a
    // single triangle even at a very tight tolerance (no uniform max level).
    std::map<int,unsigned> perSource;
    for(const SkinSurfaceTriangle &t : mesh->triangles) perSource[t.sourceTriangle]++;
    TASK_CHECK(perSource[0] > 1);
    TASK_CHECK(perSource[1] > 1);
    TASK_CHECK(perSource[2] == 1);

    // The duplicate-position seam uses distinct node ids.
    std::set<unsigned> nodesTri0, nodesTri1;
    for(const SkinSurfaceTriangle &t : mesh->triangles)
    {
        if(t.sourceTriangle == 0)
            for(int k = 0; k < 3; k++) nodesTri0.insert(t.vertex[k]);
        if(t.sourceTriangle == 1)
            for(int k = 0; k < 3; k++) nodesTri1.insert(t.vertex[k]);
    }
    // The genuinely shared v0 corner is shared; the coincident v1/v4 seam is not.
    unsigned sharedCount = 0;
    for(unsigned n : nodesTri0) if(nodesTri1.count(n)) ++sharedCount;
    TASK_CHECK(sharedCount == 1);

    // The affine leaf's error bound is numerically zero.
    for(const SkinSurfaceTriangle &t : mesh->triangles)
        if(t.sourceTriangle == 2)
            TASK_CHECK(double(t.errorBound) <= 1e-11);

    verifyMeshNumeric(mesh, m, pal, opt.chordTolerance);
    destroySkinSurfaceBinding(b);
    return 0;
}

int run_topology_budget_retains()
{
    Model m;
    m.numJoints = 2;
    int v0 = m.addVertex(TVec(0,0,0), 0, 0, {{0,255}});
    int v1 = m.addVertex(TVec(1,0,0), 1, 0, {{0,128},{1,127}});
    int v2 = m.addVertex(TVec(0,1,0), 0, 1, {{1,255}});
    m.addTri(v0, v1, v2);
    m.addMesh(0, 1);

    std::vector<TMat> pal(2);
    pal[0] = TMat::make(1.5,.2,0,0, 0,.7,.2,0, .2,0,1.3,0);
    pal[1] = TMat::make(.6,.3,0,.5, .2,1.2,0,.2, 0,.1,.8,.1);
    Matrix3x4 pp[2] = { toPalette(pal[0]), toPalette(pal[1]) };

    SkinSurfaceBinding *b = m.bind();
    TASK_CHECK(b);
    SkinSurfaceOptions opt; opt.chordTolerance = 1e-3; opt.maxDepth = 8; opt.maxTriangles = 100000;
    TASK_CHECK(evaluateSkinSurface(b, pp, 2, opt));
    const SkinSurfaceMesh *good = getSkinSurfaceDrawMesh(b);
    TASK_CHECK(good->triangles.size() > 1);
    unsigned long long seq = good->sequence;
    std::vector<Vec3> savedPos = good->positions;
    std::vector<SkinSurfaceTriangle> savedTri = good->triangles;

    // Depth cap makes the tolerance unreachable: overall failure, old output.
    SkinSurfaceOptions shallow = opt; shallow.maxDepth = 0;
    TASK_CHECK(!evaluateSkinSurface(b, pp, 2, shallow));
    const SkinSurfaceMesh *after = getSkinSurfaceDrawMesh(b);
    TASK_CHECK(after->sequence == seq);
    TASK_CHECK(after->positions.size() == savedPos.size());
    TASK_CHECK(after->triangles.size() == savedTri.size());
    TASK_CHECK(std::memcmp(after->positions.data(), savedPos.data(),
                           savedPos.size()*sizeof(Vec3)) == 0);

    // Triangle budget failure likewise retains the previous output.
    SkinSurfaceOptions poor = opt; poor.maxTriangles = 2;
    TASK_CHECK(!evaluateSkinSurface(b, pp, 2, poor));
    TASK_CHECK(getSkinSurfaceDrawMesh(b)->sequence == seq);
    TASK_CHECK(getSkinSurfaceDrawMesh(b)->triangles.size() == savedTri.size());

    // A second binding has its own storage: evaluations never borrow buffers.
    SkinSurfaceBinding *b2 = m.bind();
    TASK_CHECK(b2 && b2 != b);
    TASK_CHECK(getSkinSurfaceDrawMesh(b2) == 0);
    SkinSurfaceOptions loose; loose.chordTolerance = 1e-2; loose.maxDepth = 4;
    TASK_CHECK(evaluateSkinSurface(b2, pp, 2, loose));
    TASK_CHECK(getSkinSurfaceDrawMesh(b2)->positions.data()
               != getSkinSurfaceDrawMesh(b)->positions.data());
    TASK_CHECK(getSkinSurfaceDrawMesh(b)->sequence == seq); // first binding untouched
    destroySkinSurfaceBinding(b2);
    destroySkinSurfaceBinding(b);
    return 0;
}

// ===========================================================================
// pipeline group (real loader + animateiqm + snapshot-driven rendering)
// ===========================================================================

std::string tmpPath(const char *name)
{
    const char *dir = std::getenv("TMPDIR");
    if(!dir || !*dir) dir = "/tmp";
    return std::string(dir) + "/" + name;
}

int run_pipeline_fixture_multiframe()
{
    fixture::Model fx;
    std::string path = tmpPath("skin_surface_fixture.iqm");
    fx.write(path.c_str());

    TASK_CHECK(loadiqm(path.c_str()));
    TASK_CHECK(numjoints == 2 && numverts == 4 && tris != 0);
    TASK_CHECK(!getSkinSurfaceEnabled()); // default off
    TASK_CHECK(getSkinSurfaceDrawMesh() == 0);

    setSkinSurfaceEnabled(true);
    SkinSurfaceOptions opt; opt.chordTolerance = 5e-4; opt.maxDepth = 9; opt.maxTriangles = 200000;
    setSkinSurfaceOptions(opt);

    // Find the published node position of an original vertex (b = unit axis)
    // of a source triangle; stable across poses even if topology changes.
    auto cornerPosition = [](const SkinSurfaceMesh *m, int srcTri, int axis) -> TVec
    {
        for(unsigned ti = 0; ti < m->triangles.size(); ti++)
            if((int)m->triangles[ti].sourceTriangle == srcTri)
                for(int k = 0; k < 3; k++)
                    if(std::fabs(m->corners[3*ti+k].barycentric[axis]-1.0) < 1e-12)
                    {
                        const Vec3 &p = m->positions[m->triangles[ti].vertex[k]];
                        return TVec(p.x, p.y, p.z);
                    }
        return TVec(1e30, 1e30, 1e30);
    };

    const double frames[] = {0.0, 0.375, 1.0, 1.625};
    // Corner positions captured by value each frame: the published snapshot is
    // replaced in place on the next evaluation, so its pointer cannot be held.
    std::vector<std::array<TVec,3>> prevCorners(4), curCorners(4);
    std::vector<unsigned long long> seqs;
    for(int fi = 0; fi < 4; fi++)
    {
        animateiqm(float(frames[fi]));
        const SkinSurfaceMesh *mesh = getSkinSurfaceDrawMesh();
        TASK_CHECK(mesh != 0);
        TASK_CHECK(mesh->numSourceTriangles == 4);
        TASK_CHECK(mesh->materialRanges.size() == 1);
        TASK_CHECK(mesh->materialRanges[0].numTriangles == mesh->triangles.size());
        TASK_CHECK(mesh->corners.size() == 3*mesh->triangles.size());
        for(const SkinSurfaceTriangle &t : mesh->triangles)
            TASK_CHECK(t.sourceTriangle < 4);
        for(int t = 0; t < 4; t++)
            for(int a = 0; a < 3; a++) curCorners[t][a] = cornerPosition(mesh, t, a);
        if(fi > 0)
        {
            // Animated snapshots actually move: compare original corners by
            // identity (topology may legitimately differ between poses).
            unsigned diff = 0;
            for(int t = 0; t < 4; t++)
                for(int a = 0; a < 3; a++)
                    if((curCorners[t][a] - prevCorners[t][a]).len() > 1e-7)
                        ++diff;
            TASK_CHECK(diff > 0);
            TASK_CHECK(seqs.back() < mesh->sequence);
        }
        seqs.push_back(mesh->sequence);
        prevCorners = curCorners;
    }

    // Corner nodes reproduce the classic four-attribute skinning positions.
    animateiqm(0.375f);
    const SkinSurfaceMesh *mesh = getSkinSurfaceDrawMesh();
    for(unsigned ti = 0; ti < mesh->triangles.size(); ti++)
        for(int k = 0; k < 3; k++)
        {
            const SkinSurfaceCorner &cc = mesh->corners[3*ti+k];
            int axis = -1;
            for(int a = 0; a < 3; a++) if(std::fabs(cc.barycentric[a]-1.0) < 1e-12) axis = a;
            if(axis < 0) continue; // interior or edge node, not an original corner
            int srcVert = tris[mesh->triangles[ti].sourceTriangle].vertex[axis];
            Vec3 p = mesh->positions[mesh->triangles[ti].vertex[k]];
            TASK_CHECK((TVec(p.x,p.y,p.z)-TVec(outposition[3*srcVert],
                                                outposition[3*srcVert+1],
                                                outposition[3*srcVert+2])).len() < 2e-5);
        }

    // Returning to an earlier pose reproduces the identical snapshot.  The
    // first pose's data is copied out because the next evaluation replaces the
    // published snapshot in place.
    animateiqm(0.0f);
    const SkinSurfaceMesh *again = getSkinSurfaceDrawMesh();
    TASK_CHECK(again != 0 && again->sequence != 0);
    std::vector<Vec3> againPos = again->positions;
    std::vector<SkinSurfaceTriangle> againTri = again->triangles;
    unsigned long long againSeq = again->sequence;
    animateiqm(float(frames[0]));
    const SkinSurfaceMesh *back = getSkinSurfaceDrawMesh();
    TASK_CHECK(back->positions.size() == againPos.size());
    TASK_CHECK(back->triangles.size() == againTri.size());
    TASK_CHECK(back->sequence > againSeq);
    for(size_t i = 0; i < back->positions.size(); i++)
    {
        TASK_CHECK(back->positions[i].x == againPos[i].x);
        TASK_CHECK(back->positions[i].y == againPos[i].y);
        TASK_CHECK(back->positions[i].z == againPos[i].z);
    }
    for(size_t i = 0; i < back->triangles.size(); i++)
    {
        for(int k = 0; k < 3; k++)
            TASK_CHECK(back->triangles[i].vertex[k] == againTri[i].vertex[k]);
        TASK_CHECK(back->triangles[i].sourceTriangle == againTri[i].sourceTriangle);
        TASK_CHECK(back->triangles[i].errorBound == againTri[i].errorBound);
    }

    setSkinSurfaceEnabled(false);
    TASK_CHECK(getSkinSurfaceDrawMesh() == 0);
    cleanupiqm();
    delete[] meshdata;
    meshdata = 0;
    return 0;
}

// Minimal software rasterizer that consumes the public draw snapshot directly.
struct RasterImage
{
    int w, h;
    std::vector<int> material;
    std::vector<double> depth;
    RasterImage(int w, int h) : w(w), h(h), material(w*h, -1), depth(w*h, 1e30) {}
    void draw(const SkinSurfaceMesh *mesh, double scale, double ox, double oy)
    {
        for(unsigned ti = 0; ti < mesh->triangles.size(); ti++)
        {
            int mat = -1;
            for(size_t r = 0; r < mesh->materialRanges.size(); r++)
            {
                const SkinSurfaceRange &rr = mesh->materialRanges[r];
                if(ti >= rr.firstTriangle && ti < rr.firstTriangle+rr.numTriangles) { mat = (int)r; break; }
            }
            const SkinSurfaceTriangle &t = mesh->triangles[ti];
            double sx[3], sy[3], sz[3];
            for(int k = 0; k < 3; k++)
            {
                const Vec3 &p = mesh->positions[t.vertex[k]];
                sx[k] = ox + scale*p.x;
                sy[k] = oy - scale*p.y;
                sz[k] = p.z;
            }
            int xmin = std::max(0, (int)std::floor(std::min({sx[0],sx[1],sx[2]})));
            int xmax = std::min(w-1, (int)std::ceil(std::max({sx[0],sx[1],sx[2]})));
            int ymin = std::max(0, (int)std::floor(std::min({sy[0],sy[1],sy[2]})));
            int ymax = std::min(h-1, (int)std::ceil(std::max({sy[0],sy[1],sy[2]})));
            double denom = (sy[1]-sy[2])*(sx[0]-sx[2]) + (sx[2]-sx[1])*(sy[0]-sy[2]);
            if(std::fabs(denom) < 1e-12) continue;
            for(int y = ymin; y <= ymax; y++)
                for(int x = xmin; x <= xmax; x++)
                {
                    double l0 = ((sy[1]-sy[2])*(x+0.5-sx[2]) + (sx[2]-sx[1])*(y+0.5-sy[2]))/denom;
                    double l1 = ((sy[2]-sy[0])*(x+0.5-sx[2]) + (sx[0]-sx[2])*(y+0.5-sy[2]))/denom;
                    double l2 = 1.0-l0-l1;
                    if(l0 < -1e-9 || l1 < -1e-9 || l2 < -1e-9) continue;
                    double z = l0*sz[0]+l1*sz[1]+l2*sz[2];
                    int idx = y*w+x;
                    if(z < depth[idx]) { depth[idx] = z; material[idx] = mat; }
                }
        }
    }
    bool writePPM(const std::string &path) const
    {
        std::ofstream f(path, std::ios::binary);
        if(!f) return false;
        f << "P3\n" << w << " " << h << "\n255\n";
        static const unsigned char cols[][3] = {{220,40,40},{40,180,70},{40,80,220}};
        for(int i = 0; i < w*h; i++)
        {
            int mat = material[i];
            unsigned char r = 0, g = 0, bl = 0;
            if(mat >= 0) { r = cols[mat%3][0]; g = cols[mat%3][1]; bl = cols[mat%3][2]; }
            f << int(r) << " " << int(g) << " " << int(bl) << "\n";
        }
        return bool(f);
    }
};

int run_pipeline_ppm_render()
{
    // Two material segments whose triangles overlap on screen at different
    // depths, so per-pixel material and depth ordering are both testable.
    Model m;
    m.numJoints = 1;
    int a0 = m.addVertex(TVec(-1,-1, 0.5f), 0, 0, {{0,255}});
    int a1 = m.addVertex(TVec( 1,-1, 0.5f), 1, 0, {{0,255}});
    int a2 = m.addVertex(TVec( 0, 1, 0.5f), 0, 1, {{0,255}});
    int b0 = m.addVertex(TVec(-0.8,-0.8,-0.5f), 0, 0, {{0,255}});
    int b1 = m.addVertex(TVec( 0.8,-0.8,-0.5f), 1, 0, {{0,255}});
    int b2 = m.addVertex(TVec( 0, 0.8,-0.5f), 0, 1, {{0,255}});
    m.addTri(a0, a1, a2);
    m.addTri(b0, b1, b2);
    m.addMesh(0, 1);
    m.addMesh(1, 1);

    TMat tm = TMat::make(1,0,0,0.1, 0,1,0,-0.2, 0,0,1,0.0);
    Matrix3x4 pp = toPalette(tm);
    SkinSurfaceBinding *b = m.bind();
    TASK_CHECK(b);
    SkinSurfaceOptions opt; opt.chordTolerance = 1e-3; opt.maxDepth = 4;
    TASK_CHECK(evaluateSkinSurface(b, &pp, 1, opt));
    const SkinSurfaceMesh *mesh = getSkinSurfaceDrawMesh(b);
    TASK_CHECK(mesh->materialRanges.size() == 2);
    TASK_CHECK(mesh->triangles.size() == 2); // both patches affine -> one tri each

    RasterImage img(48, 40);
    img.draw(mesh, 14.0, 24.0, 20.0);
    TASK_CHECK(img.writePPM(tmpPath("skin_surface_render.ppm")));

    int countMat[2] = {0,0}, background = 0, overlap = 0;
    for(int i = 0; i < img.w*img.h; i++)
    {
        if(img.material[i] < 0) ++background;
        else { TASK_CHECK(img.material[i] == 0 || img.material[i] == 1); ++countMat[img.material[i]]; }
    }
    TASK_CHECK(countMat[0] > 0 && countMat[1] > 0 && background > 0);

    // Independent point-in-triangle + nearest-depth verification per pixel.
    auto inside = [](double px, double py, const double x[3], const double y[3])
    {
        double d = (y[1]-y[2])*(x[0]-x[2]) + (x[2]-x[1])*(y[0]-y[2]);
        double l0 = ((y[1]-y[2])*(px-x[2]) + (x[2]-x[1])*(py-y[2]))/d;
        double l1 = ((y[2]-y[0])*(px-x[2]) + (x[0]-x[2])*(py-y[2]))/d;
        double l2 = 1-l0-l1;
        return l0 >= 0 && l1 >= 0 && l2 >= 0;
    };
    double sx[2][3], sy[2][3], sz[2][3];
    for(int tr = 0; tr < 2; tr++)
    {
        const SkinSurfaceTriangle &t = mesh->triangles[tr];
        for(int k = 0; k < 3; k++)
        {
            const Vec3 &p = mesh->positions[t.vertex[k]];
            sx[tr][k] = 24.0 + 14.0*p.x;
            sy[tr][k] = 20.0 - 14.0*p.y;
            sz[tr][k] = p.z;
        }
    }
    for(int y = 0; y < img.h; y++)
        for(int x = 0; x < img.w; x++)
        {
            bool in0 = inside(x+0.5, y+0.5, sx[0], sy[0]);
            bool in1 = inside(x+0.5, y+0.5, sx[1], sy[1]);
            int idx = y*img.w+x;
            if(!in0 && !in1) { TASK_CHECK(img.material[idx] == -1); continue; }
            if(in0 && in1)
            {
                ++overlap;
                TASK_CHECK(img.material[idx] == 1); // triangle 1 is nearer (z=-0.5)
                TASK_CHECK(std::fabs(img.depth[idx]+0.5) < 1e-9);
            }
            else if(in0)
            {
                TASK_CHECK(img.material[idx] == 0);
                TASK_CHECK(std::fabs(img.depth[idx]-0.5) < 1e-9);
            }
            else
            {
                TASK_CHECK(img.material[idx] == 1);
                TASK_CHECK(std::fabs(img.depth[idx]+0.5) < 1e-9);
            }
        }
    TASK_CHECK(overlap > 10);

    destroySkinSurfaceBinding(b);
    return 0;
}

// ===========================================================================

} // namespace skintest

const struct task_case task_cases[] =
{
    { "surface",  "quadratic_patch",                 skintest::run_surface_quadratic },
    { "surface",  "slot_reorder_zero_slots_merge",   skintest::run_surface_slot_reorder },
    { "surface",  "noncommuting_hierarchy_scaling",  skintest::run_surface_hierarchy },
    { "surface",  "invalid_inputs_and_retained_snap",skintest::run_surface_failures },
    { "surface",  "degenerate_derivative_reported",  skintest::run_surface_degenerate_derivative },
    { "topology", "shared_edge_conforming",          skintest::run_topology_shared_edge },
    { "topology", "seam_no_weld_affine_component",   skintest::run_topology_seam_and_affine },
    { "topology", "budget_failure_keeps_old_output", skintest::run_topology_budget_retains },
    { "pipeline", "fixture_multiframe_snapshot",     skintest::run_pipeline_fixture_multiframe },
    { "pipeline", "snapshot_ppm_render",             skintest::run_pipeline_ppm_render },
};
const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
