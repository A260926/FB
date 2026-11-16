#include "skin_surface.h"

#include <cmath>
#include <cstring>
#include <climits>
#include <string>
#include <map>
#include <tuple>
#include <algorithm>
#include <numeric>

namespace {

const double SKIN_EPS = 1e-12;      // slack multiplier on conservative bounds
const unsigned SKIN_HARDCAP = 30;   // deepest dyadic level (4^30 leaves cap)

struct DVec
{
    double x, y, z;
    DVec() : x(0), y(0), z(0) {}
    DVec(double x, double y, double z) : x(x), y(y), z(z) {}
    DVec operator+(const DVec &o) const { return DVec(x+o.x, y+o.y, z+o.z); }
    DVec operator-(const DVec &o) const { return DVec(x-o.x, y-o.y, z-o.z); }
    DVec operator*(double k) const { return DVec(x*k, y*k, z*k); }
    DVec &operator+=(const DVec &o) { x+=o.x; y+=o.y; z+=o.z; return *this; }
    double dot(const DVec &o) const { return x*o.x + y*o.y + z*o.z; }
    DVec cross(const DVec &o) const { return DVec(y*o.z-z*o.y, z*o.x-x*o.z, x*o.y-y*o.x); }
    double len2() const { return dot(*this); }
};

// Row-major 3x4 affine matrix; the fourth homogeneous row is implicit (0,0,0,1).
struct Mat
{
    double m[3][4];
    Mat() { std::memset(m, 0, sizeof(m)); }
    Mat operator+(const Mat &o) const
    {
        Mat r;
        for(int i = 0; i < 3; i++)
            for(int j = 0; j < 4; j++) r.m[i][j] = m[i][j] + o.m[i][j];
        return r;
    }
    Mat operator*(double k) const
    {
        Mat r;
        for(int i = 0; i < 3; i++)
            for(int j = 0; j < 4; j++) r.m[i][j] = m[i][j]*k;
        return r;
    }
    Mat &operator+=(const Mat &o)
    {
        for(int i = 0; i < 3; i++)
            for(int j = 0; j < 4; j++) m[i][j] += o.m[i][j];
        return *this;
    }
    DVec apply(const DVec &p) const
    {
        return DVec(m[0][0]*p.x + m[0][1]*p.y + m[0][2]*p.z + m[0][3],
                    m[1][0]*p.x + m[1][1]*p.y + m[1][2]*p.z + m[1][3],
                    m[2][0]*p.x + m[2][1]*p.y + m[2][2]*p.z + m[2][3]);
    }
    // Linear part only: a direction has homogeneous coordinate 0, so the
    // translation column must not be added.
    DVec applyVector(const DVec &p) const
    {
        return DVec(m[0][0]*p.x + m[0][1]*p.y + m[0][2]*p.z,
                    m[1][0]*p.x + m[1][1]*p.y + m[1][2]*p.z,
                    m[2][0]*p.x + m[2][1]*p.y + m[2][2]*p.z);
    }
    bool finite() const
    {
        for(int i = 0; i < 3; i++)
            for(int j = 0; j < 4; j++)
                if(!std::isfinite(m[i][j])) return false;
        return true;
    }
    double det3() const
    {
        return m[0][0]*(m[1][1]*m[2][2]-m[1][2]*m[2][1])
             - m[0][1]*(m[1][0]*m[2][2]-m[1][2]*m[2][0])
             + m[0][2]*(m[1][0]*m[2][1]-m[1][1]*m[2][0]);
    }
};

struct BindInfluence
{
    int joint;
    double weight;
};

struct BindVertex
{
    DVec position;
    double uv[2];
    std::vector<BindInfluence> influences; // merged by joint, joint order
};

bool finiteVec(const DVec &v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// Canonical dyadic barycentric coordinate: b_k = num[k] / 2^shift, sum num = 2^shift.
struct RationalBary
{
    long long num[3];
    unsigned shift;
    double asDouble(int k) const { return double(num[k]) / double(1LL << shift); }
};

using BaryKey = std::tuple<long long, long long, long long, unsigned>;
using EdgeKey = std::tuple<unsigned, unsigned, unsigned, unsigned>; // umin, umax, level, segment

struct SkinSurfaceBindingImpl
{
    int numJoints = 0;
    int numSourceTriangles = 0;
    std::vector<BindVertex> verts;
    std::vector<unsigned int> sourceTris; // 3 indices per source triangle
    std::vector<SkinSurfaceRange> ranges;
    SkinSurfaceMesh published;
    std::string error;
};

// Per-frame, per-source-triangle quadratic patch data.
struct Patch
{
    Mat cornerBlend[3];      // B_a = sum_j w_aj M_j
    DVec bindPos[3];         // P_a
};

struct Evaluator
{
    SkinSurfaceBindingImpl *b;
    const Mat *palette;
    SkinSurfaceOptions opt;
    std::vector<Patch> patches;
    std::string error;

    // candidate mesh storage; published only on full success
    std::vector<DVec> positions;
    std::map<unsigned, unsigned> cornerNode;
    std::map<EdgeKey, unsigned> rootEdgeNode;
    std::map<BaryKey, unsigned> interiorNode;
    unsigned int triangleBudget = 0;
    unsigned long long refineVisits = 0;

    void fail(const char *msg)
    {
        if(error.empty()) error = msg;
    }

    Mat blendedMatrix(int vertexIndex) const
    {
        Mat r;
        const BindVertex &v = b->verts[vertexIndex];
        for(const BindInfluence &inf : v.influences)
            r += palette[inf.joint] * inf.weight;
        return r;
    }

    // F(b) with analytic derivatives dF/db1, dF/db2 (b0 = 1-b1-b2).
    // The derivative includes the weight gradient (dB_k * pos) as well as the
    // bind-position gradient (B * dp_k); it is not a skinned-endpoint chord.
    DVec evaluate(const Patch &p, const double bary[3], DVec *d0 = 0, DVec *d1 = 0) const
    {
        DVec pos = p.bindPos[0]*bary[0] + p.bindPos[1]*bary[1] + p.bindPos[2]*bary[2];
        Mat bl = p.cornerBlend[0]*bary[0] + p.cornerBlend[1]*bary[1] + p.cornerBlend[2]*bary[2];
        DVec f = bl.apply(pos);
        if(d0 || d1)
        {
            DVec dp0 = p.bindPos[0] - p.bindPos[2];
            DVec dp1 = p.bindPos[1] - p.bindPos[2];
            Mat db0 = p.cornerBlend[0]; db0 += p.cornerBlend[2]*-1.0;
            Mat db1 = p.cornerBlend[1]; db1 += p.cornerBlend[2]*-1.0;
            if(d0) *d0 = db0.apply(pos) + bl.applyVector(dp0);
            if(d1) *d1 = db1.apply(pos) + bl.applyVector(dp1);
        }
        return f;
    }

    // Conservative sup over the closed sub-triangle (q0,q1,q2) of
    // ||F(b) - L(b)|| where L is the linear interpolation of F at the three
    // sub-triangle corners.  The remainder e = F - L is quadratic with
    // e(qk) = 0; in degree-2 Bernstein form on this sub-triangle its three
    // vertex control points are zero and the three edge control points equal
    // 2*e(edge midpoint).  Convex-hull containment of the Bernstein curve then
    // gives the whole-domain bound
    //   sup ||e|| <= 2 * max over edges ||F(mid) - (F(qi)+F(qj))/2||.
    // It comes from the quadratic control net, not from finite samples, and
    // shrinks by ~4x per uniform bisection level.
    double triangleBound(const Patch &p, const double q0[3], const double q1[3], const double q2[3]) const
    {
        DVec f0 = evaluate(p, q0), f1 = evaluate(p, q1), f2 = evaluate(p, q2);
        DVec corner[3] = { f0, f1, f2 };
        const double *q[3] = { q0, q1, q2 };
        double worst = 0;
        for(int i = 0; i < 3; i++)
            for(int j = i+1; j < 3; j++)
            {
                double mid[3] = { (q[i][0]+q[j][0])*0.5, (q[i][1]+q[j][1])*0.5, (q[i][2]+q[j][2])*0.5 };
                DVec eMid = evaluate(p, mid) - (corner[i] + corner[j])*0.5;
                worst = std::max(worst, eMid.len2());
            }
        return 2.0*std::sqrt(worst);
    }

    double storedBound(double raw) const
    {
        return raw + std::max(1e-30, raw*SKIN_EPS);
    }

    // Minimal uniform 4-split level at which every sub-triangle of (b0,b1,b2)
    // satisfies the chord tolerance.  The bound is monotone under refinement
    // because every child parameter triangle is contained in its parent.
    bool requiredDepth(const Patch &p, const double b0[3], const double b1[3], const double b2[3],
                       unsigned depth, unsigned &result)
    {
        // Bound the search itself: a full quadtree visit count above the
        // triangle budget can never produce an in-budget mesh, so stop early.
        if(++refineVisits > 4ULL*opt.maxTriangles + 64)
        {
            fail("triangle budget exhausted");
            return false;
        }
        if(storedBound(triangleBound(p, b0, b1, b2)) <= opt.chordTolerance)
        {
            result = depth;
            return true;
        }
        if(depth >= opt.maxDepth || depth >= SKIN_HARDCAP) return false;
        double m01[3] = { (b0[0]+b1[0])/2, (b0[1]+b1[1])/2, (b0[2]+b1[2])/2 };
        double m02[3] = { (b0[0]+b2[0])/2, (b0[1]+b2[1])/2, (b0[2]+b2[2])/2 };
        double m12[3] = { (b1[0]+b2[0])/2, (b1[1]+b2[1])/2, (b1[2]+b2[2])/2 };
        unsigned d1, d2, d3, d4;
        if(!requiredDepth(p, b0,  m01, m02, depth+1, d1)) return false;
        if(!requiredDepth(p, m01, b1,  m12, depth+1, d2)) return false;
        if(!requiredDepth(p, m02, m12, b2,  depth+1, d3)) return false;
        if(!requiredDepth(p, m01, m12, m02, depth+1, d4)) return false;
        result = std::max(std::max(d1, d2), std::max(d3, d4));
        return true;
    }

    // Union-find over source triangles connected by a shared original edge.
    struct DSU
    {
        std::vector<int> parent;
        explicit DSU(int n) : parent(n) { std::iota(parent.begin(), parent.end(), 0); }
        int find(int x) { while(parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; }
        void unite(int a, int b) { a = find(a); b = find(b); if(a != b) parent[b] = a; }
    };

    static RationalBary gridPoint(long long i, long long j, long long n)
    {
        RationalBary rb;
        rb.num[0] = n - i - j;
        rb.num[1] = i;
        rb.num[2] = j;
        rb.shift = 0;
        long long nn = n;
        while(nn > 1) { nn /= 2; ++rb.shift; }
        // Canonicalize by common powers of two so identical geometric points
        // share one key regardless of where they were generated.
        while(rb.shift > 0 && (rb.num[0] & 1) == 0 && (rb.num[1] & 1) == 0 && (rb.num[2] & 1) == 0)
        {
            for(int k = 0; k < 3; k++) rb.num[k] /= 2;
            --rb.shift;
        }
        return rb;
    }

    // Shared node for a dyadic point of one source triangle.
    unsigned nodeFor(const Patch &p, int sourceTri, const RationalBary &rb)
    {
        EdgeKey rk;
        const EdgeKey *edgeKeyPtr = 0;
        for(int k = 0; k < 3; k++)
        {
            if(rb.num[k] != 0) continue;
            int i = (k+1)%3, j = (k+2)%3;
            unsigned u = b->sourceTris[3*sourceTri+i];
            unsigned v = b->sourceTris[3*sourceTri+j];
            unsigned lo = std::min(u, v), hi = std::max(u, v);
            bool reversed = u > v;
            // Fraction along the canonical lo->hi direction: toward corner j
            // (weight b_j) when not reversed, toward corner i otherwise.
            long long seg = reversed ? rb.num[i] : rb.num[j];
            long long count = 1LL << rb.shift;
            if(seg <= 0 || seg >= count) continue; // pure corner
            rk = std::make_tuple(lo, hi, rb.shift, (unsigned)seg);
            edgeKeyPtr = &rk;
            break;
        }
        BaryKey bk = std::make_tuple(rb.num[0], rb.num[1], rb.num[2], rb.shift);
        if(edgeKeyPtr)
        {
            auto it = rootEdgeNode.find(*edgeKeyPtr);
            if(it != rootEdgeNode.end()) return it->second;
        }
        else
        {
            auto it = interiorNode.find(bk);
            if(it != interiorNode.end()) return it->second;
        }

        double bary[3] = { rb.asDouble(0), rb.asDouble(1), rb.asDouble(2) };
        DVec d0, d1, f = evaluate(p, bary, &d0, &d1);
        if(!finiteVec(f) || !finiteVec(d0) || !finiteVec(d1))
        {
            fail("non-finite surface evaluation");
            return UINT_MAX;
        }
        DVec n = d0.cross(d1);
        double n2 = n.len2(), scale = d0.len2()*d1.len2() + 1e-30;
        if(!std::isfinite(n2) || n2 <= 1e-18*scale)
        {
            fail("degenerate surface derivative at sampling point");
            return UINT_MAX;
        }
        if(positions.size() >= UINT_MAX-1)
        {
            fail("numerical limit: node index overflow");
            return UINT_MAX;
        }
        unsigned id = (unsigned)positions.size();
        positions.push_back(f);
        if(edgeKeyPtr) rootEdgeNode[*edgeKeyPtr] = id;
        else interiorNode[bk] = id;
        return id;
    }

    bool run()
    {
        const int nt = b->numSourceTriangles;
        patches.resize(nt);
        for(int t = 0; t < nt; t++)
        {
            Patch &p = patches[t];
            for(int a = 0; a < 3; a++)
            {
                unsigned vi = b->sourceTris[3*t+a];
                p.cornerBlend[a] = blendedMatrix(vi);
                p.bindPos[a] = b->verts[vi].position;
                DVec f = p.cornerBlend[a].apply(p.bindPos[a]);
                if(!finiteVec(f)) { fail("non-finite surface evaluation"); return false; }
            }
        }

        // Connected components via shared original edges.
        DSU dsu(nt);
        std::map<std::pair<unsigned,unsigned>, std::vector<int>> edgeOwners;
        for(int t = 0; t < nt; t++)
            for(int e = 0; e < 3; e++)
            {
                int i = e == 0 ? 0 : (e == 1 ? 0 : 1);
                int j = e == 0 ? 1 : (e == 1 ? 2 : 2);
                unsigned u = b->sourceTris[3*t+i], v = b->sourceTris[3*t+j];
                edgeOwners[std::make_pair(std::min(u,v), std::max(u,v))].push_back(t);
            }
        for(auto &kv : edgeOwners)
            for(size_t k = 1; k < kv.second.size(); k++)
                dsu.unite(kv.second[0], kv.second[k]);

        // Per-source-triangle required uniform level.
        refineVisits = 0;
        std::vector<unsigned> needDepth(nt);
        for(int t = 0; t < nt; t++)
        {
            double rootBary[3][3] = { {1,0,0}, {0,1,0}, {0,0,1} };
            unsigned d;
            if(!requiredDepth(patches[t], rootBary[0], rootBary[1], rootBary[2], 0, d))
            {
                fail("chord tolerance not achievable within maxDepth");
                return false;
            }
            needDepth[t] = d;
        }
        // A component uses one shared level so all its edges carry identical
        // point strings; disconnected affine components stay at level 0.
        std::map<int, unsigned> componentDepth;
        for(int t = 0; t < nt; t++)
        {
            int root = dsu.find(t);
            unsigned &d = componentDepth[root];
            d = std::max(d, needDepth[t]);
        }
        unsigned long long totalLeaves = 0;
        std::vector<unsigned> triLevel(nt);
        for(int t = 0; t < nt; t++)
        {
            unsigned d = componentDepth[dsu.find(t)];
            triLevel[t] = d;
            totalLeaves += 1ULL << (2*d);
        }
        if(totalLeaves > opt.maxTriangles)
        {
            fail("triangle budget exhausted");
            return false;
        }

        positions.clear();
        rootEdgeNode.clear();
        cornerNode.clear();
        triangleBudget = 0;

        SkinSurfaceMesh mesh;
        mesh.numSourceTriangles = nt;
        mesh.positions.reserve((size_t)std::min<unsigned long long>(totalLeaves*2, 1u<<28));

        for(int t = 0; t < nt; t++)
        {
            const Patch &p = patches[t];
            unsigned d = triLevel[t];
            long long n = 1LL << d;

            // Pass 1: evaluate and deduplicate every grid node.  Interior
            // nodes are per source triangle; root-edge and corner nodes are
            // globally shared, so adjacent faces get one identical point
            // string and one position index per shared edge point.
            interiorNode.clear();
            std::vector<std::vector<unsigned>> grid(n+1, std::vector<unsigned>(n+1, UINT_MAX));
            for(long long i = 0; i <= n; i++)
                for(long long j = 0; j <= n-i; j++)
                {
                    RationalBary rb = gridPoint(i, j, n);
                    bool isCorner = (rb.shift == 0);
                    unsigned id;
                    if(isCorner)
                    {
                        int axis = (rb.num[0] == 1) ? 0 : (rb.num[1] == 1 ? 1 : 2);
                        unsigned vi = b->sourceTris[3*t+axis];
                        auto it = cornerNode.find(vi);
                        if(it != cornerNode.end()) id = it->second;
                        else
                        {
                            id = nodeFor(p, t, rb);
                            if(id == UINT_MAX) return false;
                            cornerNode[vi] = id;
                        }
                    }
                    else
                    {
                        id = nodeFor(p, t, rb);
                        if(id == UINT_MAX) return false;
                    }
                    grid[i][j] = id;
                }

            // Pass 2: emit the 4^d parameter triangles, keeping original winding.
            for(long long i = 0; i < n; i++)
                for(long long j = 0; j < n-i; j++)
                {
                    RationalBary rbA = gridPoint(i, j, n);
                    RationalBary rbB = gridPoint(i+1, j, n);
                    RationalBary rbC = gridPoint(i, j+1, n);
                    if(!emitLeaf(mesh, p, t, grid[i][j], grid[i+1][j], grid[i][j+1], rbA, rbB, rbC))
                        return false;
                    if(i+j < n-1)
                    {
                        RationalBary rbD = gridPoint(i+1, j+1, n);
                        if(!emitLeaf(mesh, p, t, grid[i+1][j], grid[i+1][j+1], grid[i][j+1], rbB, rbD, rbC))
                            return false;
                    }
                }
        }

        // All shared nodes exist now; publish their positions.
        for(const DVec &p2 : positions)
        {
            if(!finiteVec(p2)) { fail("non-finite output position"); return false; }
            Vec3 fv(float(p2.x), float(p2.y), float(p2.z));
            if(!std::isfinite(fv.x) || !std::isfinite(fv.y) || !std::isfinite(fv.z))
            {
                fail("non-finite output position");
                return false;
            }
            mesh.positions.push_back(fv);
        }

        if(mesh.corners.size() != 3*mesh.triangles.size())
        {
            fail("internal output inconsistency");
            return false;
        }
        mesh.materialRanges = b->ranges;
        // Rebuild material runs from the emitted, ordered source triangle ids.
        unsigned cursor = 0;
        for(SkinSurfaceRange &r : mesh.materialRanges)
        {
            unsigned srcFirst = r.firstTriangle, srcCount = r.numTriangles;
            unsigned first = cursor, count = 0;
            while(cursor < (unsigned)mesh.triangles.size()
                  && mesh.triangles[cursor].sourceTriangle >= srcFirst
                  && mesh.triangles[cursor].sourceTriangle < srcFirst + srcCount)
            {
                ++cursor; ++count;
            }
            r.firstTriangle = first;
            r.numTriangles = count;
        }
        if(cursor != (unsigned)mesh.triangles.size())
        {
            fail("material range coverage error");
            return false;
        }

        mesh.sequence = b->published.sequence + 1;
        b->published = std::move(mesh);
        return true;
    }

    bool emitLeaf(SkinSurfaceMesh &mesh, const Patch &p, int sourceTri,
                  unsigned id0, unsigned id1, unsigned id2,
                  const RationalBary &rb0, const RationalBary &rb1, const RationalBary &rb2)
    {
        if(triangleBudget >= opt.maxTriangles)
        {
            fail("triangle budget exhausted");
            return false;
        }
        const RationalBary *rb[3] = { &rb0, &rb1, &rb2 };
        double q[3][3];
        for(int k = 0; k < 3; k++)
            for(int c = 0; c < 3; c++)
                q[k][c] = rb[k]->asDouble(c);
        float bound = float(storedBound(triangleBound(p, q[0], q[1], q[2])));
        if(!std::isfinite(bound) || bound > float(opt.chordTolerance))
        {
            fail("chord tolerance not achievable within maxDepth");
            return false;
        }
        SkinSurfaceTriangle st;
        st.vertex[0] = id0; st.vertex[1] = id1; st.vertex[2] = id2;
        st.sourceTriangle = sourceTri;
        st.errorBound = bound;
        mesh.triangles.push_back(st);

        for(int k = 0; k < 3; k++)
        {
            double bary[3] = { q[k][0], q[k][1], q[k][2] };
            DVec d0, d1;
            evaluate(p, bary, &d0, &d1);
            DVec n = d0.cross(d1);
            double n2l = n.len2();
            if(!std::isfinite(n2l) || n2l <= 1e-18*(d0.len2()*d1.len2()+1e-30))
            {
                fail("degenerate surface derivative at sampling point");
                return false;
            }
            n = n * (1.0/std::sqrt(n2l));
            SkinSurfaceCorner cc;
            cc.barycentric[0] = float(bary[0]);
            cc.barycentric[1] = float(bary[1]);
            cc.barycentric[2] = float(bary[2]);
            cc.unitNormal[0] = float(n.x);
            cc.unitNormal[1] = float(n.y);
            cc.unitNormal[2] = float(n.z);
            double uv[2] = { 0, 0 };
            for(int a = 0; a < 3; a++)
            {
                const BindVertex &bv = b->verts[b->sourceTris[3*sourceTri+a]];
                uv[0] += bary[a]*bv.uv[0];
                uv[1] += bary[a]*bv.uv[1];
            }
            cc.uv[0] = float(uv[0]);
            cc.uv[1] = float(uv[1]);
            for(int qq = 0; qq < 3; qq++)
                if(!std::isfinite(cc.unitNormal[qq]) || !std::isfinite(cc.barycentric[qq]))
                {
                    fail("non-finite corner data");
                    return false;
                }
            if(!std::isfinite(cc.uv[0]) || !std::isfinite(cc.uv[1]))
            {
                fail("non-finite corner data");
                return false;
            }
            mesh.corners.push_back(cc);
        }
        ++triangleBudget;
        return true;
    }
};

bool validateOptions(const SkinSurfaceOptions &o, std::string &err)
{
    if(!std::isfinite(o.chordTolerance) || o.chordTolerance <= 0.0)
    {
        err = "chordTolerance must be a positive finite value";
        return false;
    }
    if(o.maxTriangles == 0)
    {
        err = "maxTriangles must be positive";
        return false;
    }
    if(o.maxDepth > SKIN_HARDCAP)
    {
        err = "maxDepth exceeds supported subdivision limit";
        return false;
    }
    return true;
}

// ---- global demo integration state ----

SkinSurfaceBinding *g_binding = 0;
bool g_enabled = false;
SkinSurfaceOptions g_options;
std::string g_message;

} // namespace

bool createSkinSurfaceBinding(SkinSurfaceBinding *&out,
                              const float *positions, const float *texcoords,
                              const uchar *blendIndex, const uchar *blendWeight,
                              int numVerts, int numJoints,
                              const iqmtriangle *triangles, int numTriangles,
                              const iqmmesh *meshes, int numMeshes)
{
    out = 0;
    std::string err;
    if(numVerts <= 0 || numJoints <= 0 || numTriangles <= 0 || numMeshes < 0)
        err = "invalid binding dimensions";
    else if(!positions || !blendIndex || !blendWeight || !triangles || (numMeshes > 0 && !meshes))
        err = "missing required vertex/triangle arrays";
    if(!err.empty())
    {
        g_message = err;
        return false;
    }

    SkinSurfaceBindingImpl *b = new SkinSurfaceBindingImpl();
    b->numJoints = numJoints;
    b->numSourceTriangles = numTriangles;
    b->verts.resize(numVerts);
    for(int v = 0; v < numVerts; v++)
    {
        BindVertex &bv = b->verts[v];
        bv.position = DVec(positions[3*v], positions[3*v+1], positions[3*v+2]);
        if(!finiteVec(bv.position)) { err = "non-finite bind position"; goto fail; }
        if(texcoords)
        {
            bv.uv[0] = texcoords[2*v];
            bv.uv[1] = texcoords[2*v+1];
            if(!std::isfinite(bv.uv[0]) || !std::isfinite(bv.uv[1]))
            {
                err = "non-finite texcoord";
                goto fail;
            }
        }
        int merged[256];
        double sumw[256];
        int used = 0;
        unsigned bytesum = 0;
        for(int k = 0; k < 256; k++) { merged[k] = -1; sumw[k] = 0; }
        for(int s = 0; s < 4; s++)
        {
            uchar bw = blendWeight[4*v+s];
            uchar bi = blendIndex[4*v+s];
            bytesum += bw;
            if(bw == 0) continue; // zero slots do not participate
            if((int)bi >= numJoints) { err = "blend index out of range"; goto fail; }
            if(merged[bi] < 0) { merged[bi] = used++; sumw[bi] = 0; }
            sumw[bi] += double(bw)/255.0;
        }
        if(bytesum != 255) { err = "vertex blend weights do not sum to 255"; goto fail; }
        bv.influences.reserve(used);
        for(int j = 0; j < numJoints; j++)
            if(merged[j] >= 0) bv.influences.push_back({ j, sumw[j] });
        double wsum = 0;
        for(const BindInfluence &inf : bv.influences) wsum += inf.weight;
        if(!std::isfinite(wsum) || std::fabs(wsum - 1.0) > 1e-6)
        {
            err = "merged blend weights do not sum to 1";
            goto fail;
        }
    }
    b->sourceTris.resize(3*numTriangles);
    for(int t = 0; t < numTriangles; t++)
    {
        for(int a = 0; a < 3; a++)
        {
            unsigned vi = triangles[t].vertex[a];
            if(vi >= (unsigned)numVerts) { err = "triangle vertex out of range"; goto fail; }
            b->sourceTris[3*t+a] = vi;
        }
        if(b->sourceTris[3*t] == b->sourceTris[3*t+1]
           || b->sourceTris[3*t+1] == b->sourceTris[3*t+2]
           || b->sourceTris[3*t] == b->sourceTris[3*t+2])
        {
            err = "degenerate input triangle";
            goto fail;
        }
        DVec p0 = b->verts[b->sourceTris[3*t]].position;
        DVec p1 = b->verts[b->sourceTris[3*t+1]].position;
        DVec p2 = b->verts[b->sourceTris[3*t+2]].position;
        double area2 = (p1-p0).cross(p2-p0).len2();
        double scale = (p1-p0).len2() + (p2-p0).len2() + (p2-p1).len2() + 1e-30;
        if(!std::isfinite(area2) || area2 <= 1e-20*scale)
        {
            err = "degenerate input triangle";
            goto fail;
        }
    }
    // Material ranges must partition the source triangles in iqmmesh order.
    b->ranges.resize(numMeshes);
    {
        unsigned cursor = 0;
        for(int m = 0; m < numMeshes; m++)
        {
            if(meshes[m].num_triangles == 0
               || meshes[m].first_triangle != cursor
               || (unsigned long long)meshes[m].first_triangle + meshes[m].num_triangles > (unsigned)numTriangles)
            {
                err = "material triangle range out of range or not contiguous";
                goto fail;
            }
            b->ranges[m].firstTriangle = meshes[m].first_triangle;
            b->ranges[m].numTriangles = meshes[m].num_triangles;
            cursor += meshes[m].num_triangles;
        }
        if(cursor != (unsigned)numTriangles) { err = "material ranges do not cover all triangles"; goto fail; }
    }
    out = reinterpret_cast<SkinSurfaceBinding *>(b);
    return true;

fail:
    delete b;
    g_message = err;
    return false;
}

void destroySkinSurfaceBinding(SkinSurfaceBinding *&binding)
{
    if(!binding) return;
    delete reinterpret_cast<SkinSurfaceBindingImpl *>(binding);
    binding = 0;
}

bool evaluateSkinSurface(SkinSurfaceBinding *binding,
                         const Matrix3x4 *palette, int numJoints,
                         const SkinSurfaceOptions &options)
{
    SkinSurfaceBindingImpl *b = reinterpret_cast<SkinSurfaceBindingImpl *>(binding);
    if(!b || !palette || numJoints != b->numJoints)
    {
        std::string err = "invalid palette or binding";
        if(b) b->error = err;
        g_message = err;
        return false;
    }
    std::string err;
    if(!validateOptions(options, err))
    {
        b->error = err;
        g_message = err;
        return false;
    }
    std::vector<Mat> mats(numJoints);
    double frob = 1.0;
    for(int j = 0; j < numJoints; j++)
    {
        for(int r = 0; r < 3; r++)
        {
            const Vec4 &row = r == 0 ? palette[j].a : (r == 1 ? palette[j].b : palette[j].c);
            mats[j].m[r][0] = row.x;
            mats[j].m[r][1] = row.y;
            mats[j].m[r][2] = row.z;
            mats[j].m[r][3] = row.w;
        }
        if(!mats[j].finite())
        {
            b->error = "non-finite palette matrix";
            g_message = b->error;
            return false;
        }
        for(int r = 0; r < 3; r++)
            for(int c = 0; c < 3; c++) frob += mats[j].m[r][c]*mats[j].m[r][c];
        double d = mats[j].det3();
        if(!std::isfinite(d) || std::fabs(d) <= 1e-18*frob)
        {
            b->error = "palette matrix is not invertible";
            g_message = b->error;
            return false;
        }
    }
    Evaluator ev;
    ev.b = b;
    ev.palette = mats.data();
    ev.opt = options;
    if(!ev.run())
    {
        b->error = ev.error;
        g_message = ev.error;
        return false; // published snapshot is untouched
    }
    b->error.clear();
    g_message.clear();
    return true;
}

const SkinSurfaceMesh *getSkinSurfaceDrawMesh(const SkinSurfaceBinding *binding)
{
    const SkinSurfaceBindingImpl *b = reinterpret_cast<const SkinSurfaceBindingImpl *>(binding);
    if(!b || b->published.empty()) return 0;
    return &b->published;
}

const char *getSkinSurfaceError(const SkinSurfaceBinding *binding)
{
    const SkinSurfaceBindingImpl *b = reinterpret_cast<const SkinSurfaceBindingImpl *>(binding);
    return b ? b->error.c_str() : "null binding";
}

void setSkinSurfaceEnabled(bool enabled) { g_enabled = enabled; }
bool getSkinSurfaceEnabled() { return g_enabled; }
void setSkinSurfaceOptions(const SkinSurfaceOptions &options) { g_options = options; }
const SkinSurfaceOptions &getSkinSurfaceOptions() { return g_options; }

const SkinSurfaceMesh *getSkinSurfaceDrawMesh()
{
    if(!g_enabled) return 0;
    return getSkinSurfaceDrawMesh(g_binding);
}

const char *getSkinSurfaceMessage() { return g_message.c_str(); }

bool skinSurfaceLoad(const float *positions, const float *texcoords,
                     const uchar *blendIndex, const uchar *blendWeight,
                     int numVerts, int numJoints,
                     const iqmtriangle *triangles, int numTriangles,
                     const iqmmesh *meshes, int numMeshes)
{
    if(g_binding)
    {
        destroySkinSurfaceBinding(g_binding);
        g_binding = 0;
    }
    SkinSurfaceBinding *raw = 0;
    if(!createSkinSurfaceBinding(raw, positions, texcoords, blendIndex, blendWeight,
                                 numVerts, numJoints, triangles, numTriangles, meshes, numMeshes))
        return false;
    g_binding = raw;
    return true;
}

void skinSurfaceAnimate(const Matrix3x4 *palette, int numJoints)
{
    if(!g_enabled || !g_binding) return;
    // Failure keeps the previous complete snapshot; the renderer falls back
    // automatically when no snapshot is available.
    evaluateSkinSurface(g_binding, palette, numJoints, g_options);
}

void skinSurfaceUnload()
{
    if(g_binding)
    {
        destroySkinSurfaceBinding(g_binding);
        g_binding = 0;
    }
    g_message.clear();
}
