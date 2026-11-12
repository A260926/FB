#include "qa.hpp"

#include <opensubdiv/far/topologyDescriptor.h>
#include <opensubdiv/far/topologyRefiner.h>
#include <opensubdiv/far/topologyRefinerFactory.h>
#include <opensubdiv/bfr/refinerSurfaceFactory.h>
#include <opensubdiv/bfr/surface.h>
#include <opensubdiv/bfr/surfaceFitter.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <vector>

using namespace OpenSubdiv;

namespace {

typedef Bfr::LimitSurfaceFitter Fitter;
typedef Fitter::FitSample FitSample;
typedef Fitter::Anchor Anchor;
typedef Fitter::Result Result;
typedef Fitter::Options FitOptions;

//------------------------------------------------------------------------------
//  Test meshes
//------------------------------------------------------------------------------
struct TestMesh {
    std::unique_ptr<Far::TopologyRefiner> refiner;
    std::vector<double> rest;
    int numVerts;
};

static std::unique_ptr<Far::TopologyRefiner>
createRefiner(Sdc::SchemeType scheme,
              std::vector<int> const & faceSizes,
              std::vector<int> const & faceVerts,
              int numVerts) {
    static std::vector<int> sizes, verts;
    sizes = faceSizes;
    verts = faceVerts;

    Far::TopologyDescriptor desc;
    desc.numVertices        = numVerts;
    desc.numFaces           = (int)sizes.size();
    desc.numVertsPerFace    = sizes.data();
    desc.vertIndicesPerFace = verts.data();

    Sdc::Options opts;
    opts.SetVtxBoundaryInterpolation(Sdc::Options::VTX_BOUNDARY_EDGE_ONLY);

    typedef Far::TopologyRefinerFactory<Far::TopologyDescriptor> Factory;
    return std::unique_ptr<Far::TopologyRefiner>(
        Factory::Create(desc, Factory::Options(scheme, opts)));
}

//  Asymmetric cube: all quads, catmark, no extraordinary vertices:
static TestMesh makeAsymmetricCube() {
    TestMesh m;
    m.numVerts = 8;
    m.rest = {
        -1.2, -1.0, -1.0,   1.3, -0.9, -0.8,
         1.1,  1.2, -0.7,  -0.9,  1.05, -1.1,
        -1.1, -0.8,  1.2,   1.2, -1.1,  1.0,
         1.05, 0.95, 1.15, -1.0,  1.2,   0.9
    };
    std::vector<int> sizes(6, 4);
    std::vector<int> faces = {
        0,3,2,1,  4,5,6,7,  0,1,5,4,
        1,2,6,5,  2,3,7,6,  3,0,4,7
    };
    m.refiner = createRefiner(Sdc::SCHEME_CATMARK, sizes, faces, m.numVerts);
    return m;
}

//  Pentagonal prism: two pentagons and five quads -- extraordinary vertices
//  and non-quad faces in an otherwise closed, oriented manifold:
static TestMesh makePentagonalPrism() {
    TestMesh m;
    m.numVerts = 10;

    double const r = 1.15;
    double const z = 1.0;
    std::vector<double> rest(3 * m.numVerts);
    double const angles[5] = { 0.0, 1.2566370614, 2.5132741229,
                               3.7699111843, 5.0265482457 };
    for (int i = 0; i < 5; ++i) {
        double x = r * std::cos(angles[i]);
        double y = r * std::sin(angles[i]);
        //  Slight z and radial asymmetry so no accidental symmetry remains:
        double w = 0.06 * (double)i;
        rest[3*i+0] = x + w;
        rest[3*i+1] = y - 0.03 * (double)i;
        rest[3*i+2] = -z;
        rest[3*(i+5)+0] = x - 0.04 * (double)i;
        rest[3*(i+5)+1] = y + 0.05 * (double)i;
        rest[3*(i+5)+2] = z + 0.02 * (double)i;
    }
    m.rest = rest;

    std::vector<int> sizes;
    std::vector<int> faces;

    //  Bottom pentagon (outward is -z), top pentagon (outward is +z):
    sizes.push_back(5);
    faces.insert(faces.end(), {0, 4, 3, 2, 1});
    sizes.push_back(5);
    faces.insert(faces.end(), {5, 6, 7, 8, 9});

    //  Five side quads:
    for (int i = 0; i < 5; ++i) {
        int j = (i + 1) % 5;
        sizes.push_back(4);
        faces.insert(faces.end(), { i, j, 5 + j, 5 + i });
    }

    m.refiner = createRefiner(Sdc::SCHEME_CATMARK, sizes, faces, m.numVerts);
    return m;
}

//  A 3x3 quad grid (4x4 vertices) with gentle non-planar displacement:
static TestMesh makeQuadGrid() {
    TestMesh m;
    int const N = 3;
    int const nv = (N + 1) * (N + 1);
    m.numVerts = nv;
    m.rest.resize(3 * nv);
    for (int j = 0; j <= N; ++j) {
        for (int i = 0; i <= N; ++i) {
            int v = j * (N + 1) + i;
            m.rest[3*v+0] = (double)i - 1.2;
            m.rest[3*v+1] = (double)j - 1.1;
            m.rest[3*v+2] = 0.03 * std::sin(1.4 * i + 0.9 * j);
        }
    }
    std::vector<int> sizes;
    std::vector<int> faces;
    for (int j = 0; j < N; ++j) {
        for (int i = 0; i < N; ++i) {
            int v00 = j * (N + 1) + i;
            int v10 = v00 + 1;
            int v11 = v00 + (N + 1) + 1;
            int v01 = v00 + (N + 1);
            sizes.push_back(4);
            faces.insert(faces.end(), { v00, v10, v11, v01 });
        }
    }
    m.refiner = createRefiner(Sdc::SCHEME_CATMARK, sizes, faces, m.numVerts);
    return m;
}

//  A closed Catmark mesh with an interior extraordinary vertex: a
//  pentagonal prism whose top pentagon is split into a triangle fan around
//  a center vertex (valence 5).
static TestMesh makeIrregularVertexMesh() {
    TestMesh m;
    int const N = 5;
    m.numVerts = 11;   // 5 bottom + 5 top rim + 1 top center

    m.rest.resize(3 * m.numVerts);
    double const r = 1.1;
    for (int i = 0; i < N; ++i) {
        double a = (2.0 * 3.14159265358979323846 * i) / N;
        double x = r * std::cos(a), y = r * std::sin(a);
        m.rest[3*i+0]     = x + 0.05 * i;
        m.rest[3*i+1]     = y - 0.02 * i;
        m.rest[3*i+2]     = -1.0;
        m.rest[3*(i+5)+0] = x - 0.04 * i;
        m.rest[3*(i+5)+1] = y + 0.03 * i;
        m.rest[3*(i+5)+2] =  1.0 + 0.01 * i;
    }
    m.rest[3*10+0] = 0.1;
    m.rest[3*10+1] = -0.05;
    m.rest[3*10+2] = 1.12;

    std::vector<int> sizes;
    std::vector<int> faces;
    //  Bottom pentagon (outward -z):
    sizes.push_back(5);
    faces.insert(faces.end(), { 0, 4, 3, 2, 1 });
    //  Five side quads:
    for (int i = 0; i < N; ++i) {
        int j = (i + 1) % N;
        sizes.push_back(4);
        faces.insert(faces.end(), { i, j, 5 + j, 5 + i });
    }
    //  Top pentagon split into a triangle fan around vertex 10. The rim
    //  edge of each side quad runs a -> b, so the fan triangle uses the
    //  same direction for a consistent (oriented) winding:
    for (int i = 0; i < N; ++i) {
        int a = 5 + i;
        int b = 5 + ((i + 1) % N);
        sizes.push_back(3);
        faces.insert(faces.end(), { a, b, 10 });
    }

    m.refiner = createRefiner(Sdc::SCHEME_CATMARK, sizes, faces, m.numVerts);
    return m;
}

//  Tetrahedron: all triangles, Loop scheme:
static TestMesh makeLoopTetrahedron() {
    TestMesh m;
    m.numVerts = 4;
    m.rest = {
         1.1,  1.0,  1.2,
         1.0, -1.1, -1.0,
        -1.2,  1.05, -0.9,
        -0.95,-1.0,  1.1
    };
    std::vector<int> sizes(4, 3);
    std::vector<int> faces = {
        0,2,1,  0,1,3,  0,3,2,  1,2,3
    };
    m.refiner = createRefiner(Sdc::SCHEME_LOOP, sizes, faces, m.numVerts);
    return m;
}

//------------------------------------------------------------------------------
//  Fit samples for specific meshes
//------------------------------------------------------------------------------
static std::vector<FitSample> simpleQuadSamples(int numFaces) {
    std::vector<FitSample> samples;
    for (int f = 0; f < numFaces; ++f) {
        samples.push_back(FitSample(f, 0.30, 0.34, 1.0, 0.0));
        samples.push_back(FitSample(f, 0.66, 0.61, 1.0, 1.0));
    }
    return samples;
}

static std::vector<FitSample> triangleSamples(int numFaces) {
    std::vector<FitSample> samples;
    for (int f = 0; f < numFaces; ++f) {
        samples.push_back(FitSample(f, 0.22, 0.31, 1.0, 0.0));
        samples.push_back(FitSample(f, 0.33, 0.20, 0.8, 1.0));
    }
    return samples;
}

//  Position-only triangle samples (used by the dense linear reference):
static std::vector<FitSample> trianglePositionSamples(int numFaces) {
    std::vector<FitSample> samples;
    for (int f = 0; f < numFaces; ++f) {
        samples.push_back(FitSample(f, 0.22, 0.31, 1.0, 0.0));
        samples.push_back(FitSample(f, 0.33, 0.20, 0.8, 0.0));
    }
    return samples;
}

//  Samples strictly inside a smooth sub-face of every face of the prism
//  (the pentagons are parameterized as quad sub-faces):
static std::vector<FitSample> prismSamples() {
    std::vector<FitSample> samples;
    //  Two pentagon faces: half-tile centers well inside sub-faces.
    samples.push_back(FitSample(0, 0.20, 0.20, 1.0, 0.0));
    samples.push_back(FitSample(1, 1.20, 0.20, 0.7, 1.0));
    //  Five quad faces:
    for (int f = 2; f < 7; ++f) {
        samples.push_back(FitSample(f, 0.32, 0.36, 1.0, 0.0));
        samples.push_back(FitSample(f, 0.64, 0.58, 0.6, 1.0));
    }
    return samples;
}

//------------------------------------------------------------------------------
//  Independent evaluation using the original Bfr direct point/first-derivative
//  evaluation (Surface::Evaluate on prepared patch points -- intentionally not
//  the fitter and not limit stencils).
//------------------------------------------------------------------------------
struct DirectEval {
    std::vector<double> P, N;   // 3 values per sample
};

static DirectEval evaluateDirectly(Far::TopologyRefiner const & mesh,
                                   std::vector<double> const & points,
                                   std::vector<FitSample> const & samples) {
    DirectEval out;
    out.P.assign(3 * samples.size(), 0.0);
    out.N.assign(3 * samples.size(), 0.0);

    Bfr::RefinerSurfaceFactory<> factory(mesh);

    //  Initialize each referenced face surface once:
    std::map<int, Bfr::Surface<double> > surfaces;
    for (size_t i = 0; i < samples.size(); ++i) {
        surfaces[samples[i].faceIndex] = Bfr::Surface<double>();
    }
    std::map<int, Bfr::Surface<double> >::iterator it;
    for (it = surfaces.begin(); it != surfaces.end(); ++it) {
        if (!factory.InitVertexSurface(it->first, &it->second)) {
            return out;
        }
    }

    for (size_t i = 0; i < samples.size(); ++i) {
        FitSample const & s = samples[i];
        Bfr::Surface<double> & surf = surfaces[s.faceIndex];

        std::vector<double> patchPoints(3 * surf.GetNumPatchPoints());
        surf.PreparePatchPoints(points.data(), 3,
                                patchPoints.data(), 3);

        double uv[2] = { s.u, s.v };
        double P[3], Du[3], Dv[3];
        surf.Evaluate(uv, patchPoints.data(), 3, P, Du, Dv);

        double cross[3];
        cross[0] = Du[1]*Dv[2] - Du[2]*Dv[1];
        cross[1] = Du[2]*Dv[0] - Du[0]*Dv[2];
        cross[2] = Du[0]*Dv[1] - Du[1]*Dv[0];
        double len = std::sqrt(cross[0]*cross[0] +
                               cross[1]*cross[1] +
                               cross[2]*cross[2]);
        for (int k = 0; k < 3; ++k) {
            out.P[3*i+k] = P[k];
            out.N[3*i+k] = (len > 0.0) ? cross[k] / len : 0.0;
        }
    }
    return out;
}

//------------------------------------------------------------------------------
//  Complete objective, computed independently from the direct evaluation:
//------------------------------------------------------------------------------
static double completeEnergy(std::vector<double> const & x,
                             std::vector<double> const & targetPos,
                             std::vector<double> const & targetNrm,
                             DirectEval const & eval,
                             std::vector<FitSample> const & samples,
                             std::vector<double> const & rest,
                             double lambda) {
    double E = 0.0;
    for (size_t i = 0; i < samples.size(); ++i) {
        double dp[3], dn[3];
        for (int k = 0; k < 3; ++k) {
            dp[k] = eval.P[3*i+k] - targetPos[3*i+k];
            dn[k] = eval.N[3*i+k] - targetNrm[3*i+k];
        }
        E += samples[i].positionWeight * (dp[0]*dp[0] + dp[1]*dp[1] +
                                          dp[2]*dp[2]);
        E += samples[i].normalWeight * (dn[0]*dn[0] + dn[1]*dn[1] +
                                        dn[2]*dn[2]);
    }
    int numVerts = (int)(rest.size() / 3);
    for (int v = 0; v < numVerts; ++v) {
        double d0 = x[3*v+0] - rest[3*v+0];
        double d1 = x[3*v+1] - rest[3*v+1];
        double d2 = x[3*v+2] - rest[3*v+2];
        E += lambda * (d0*d0 + d1*d1 + d2*d2);
    }
    return E;
}

//  Central-difference free gradient of the complete objective, using the
//  independent direct evaluation. Returns the infinity norm over free
//  variables (a passed-in per-component gradient is also filled).
static double freeGradientByFD(Far::TopologyRefiner const & mesh,
                               std::vector<double> const & x,
                               std::vector<FitSample> const & samples,
                               std::vector<double> const & rest,
                               std::vector<double> const & targetPos,
                               std::vector<double> const & targetNrm,
                               double lambda,
                               std::vector<unsigned char> const & anchored,
                               std::vector<double> * gradientOut = 0,
                               double h = 1.0e-6) {
    int numVerts = (int)(rest.size() / 3);
    if (gradientOut) gradientOut->assign(3 * numVerts, 0.0);

    double norm = 0.0;
    for (int v = 0; v < numVerts; ++v) {
        if (anchored[v]) continue;
        for (int k = 0; k < 3; ++k) {
            std::vector<double> xp = x, xm = x;
            xp[3*v+k] += h;
            xm[3*v+k] -= h;

            DirectEval ep = evaluateDirectly(mesh, xp, samples);
            DirectEval em = evaluateDirectly(mesh, xm, samples);
            double fp = completeEnergy(xp, targetPos, targetNrm, ep,
                                       samples, rest, lambda);
            double fm = completeEnergy(xm, targetPos, targetNrm, em,
                                       samples, rest, lambda);
            double g = (fp - fm) / (2.0 * h);
            if (gradientOut) (*gradientOut)[3*v+k] = g;
            if (std::fabs(g) > norm) norm = std::fabs(g);
        }
    }
    return norm;
}

//------------------------------------------------------------------------------
//  A small, independent dense solver (Gaussian elimination with partial
//  pivoting) used only for tiny test systems (<= 3V, e.g. 24x24). This is a
//  test-side reference implementation, not part of the fitter.
//------------------------------------------------------------------------------
static bool denseSolve(std::vector<double> & A, std::vector<double> & b,
                       int n, std::vector<double> & x) {
    //  A is n x n row-major, b is length n; x receives the solution:
    for (int col = 0; col < n; ++col) {
        int pivot = col;
        for (int row = col + 1; row < n; ++row) {
            if (std::fabs(A[row*n+col]) > std::fabs(A[pivot*n+col])) {
                pivot = row;
            }
        }
        if (std::fabs(A[pivot*n+col]) < 1.0e-300) return false;

        if (pivot != col) {
            for (int j = col; j < n; ++j) {
                std::swap(A[pivot*n+j], A[col*n+j]);
            }
            std::swap(b[pivot], b[col]);
        }
        double d = A[col*n+col];
        for (int row = col + 1; row < n; ++row) {
            double f = A[row*n+col] / d;
            if (f == 0.0) continue;
            for (int j = col; j < n; ++j) {
                A[row*n+j] -= f * A[col*n+j];
            }
            b[row] -= f * b[col];
        }
    }
    x.assign(n, 0.0);
    for (int row = n - 1; row >= 0; --row) {
        double s = b[row];
        for (int j = row + 1; j < n; ++j) {
            s -= A[row*n+j] * x[j];
        }
        x[row] = s / A[row*n+row];
    }
    return true;
}

//  Exact position-only minimizer of
//      sum_i wp_i |P_i(X) - p_i|^2 + lambda sum_v |X_v - R_v|^2
//  built from the independent direct Bfr position evaluation. The design
//  matrix A (3 samples per component, 3V unknowns) is formed column by
//  column with direct evaluation at the unit basis vectors.
static bool densePositionMinimizer(Far::TopologyRefiner const & mesh,
                                   std::vector<double> const & rest,
                                   std::vector<FitSample> const & samples,
                                   std::vector<double> const & targetPos,
                                   double lambda,
                                   std::vector<double> & solution) {
    int numVerts = (int)(rest.size() / 3);
    int n = 3 * numVerts;
    int m = 3 * (int)samples.size();

    std::vector<double> A((size_t)m * n, 0.0);

    //  Columns of A: P_i(X) = sum_v w_v X_v, so the column for unknown
    //  (v,l) is the position response to e_(v,l) at every sample:
    for (int col = 0; col < n; ++col) {
        std::vector<double> unit(3 * numVerts, 0.0);
        unit[col] = 1.0;
        DirectEval ev = evaluateDirectly(mesh, unit, samples);
        for (int row = 0; row < m; ++row) {
            A[(size_t)row * n + col] = ev.P[row];
        }
    }

    //  H = sum_i wp_i A_i^T A_i + lambda I;  b = sum_i wp_i A_i^T p_i
    std::vector<double> H((size_t)n * n, 0.0);
    std::vector<double> rhs(n, 0.0);

    for (int i = 0; i < (int)samples.size(); ++i) {
        double w = samples[i].positionWeight;
        if (w == 0.0) continue;
        for (int r0 = 0; r0 < 3; ++r0) {
            int row = 3*i + r0;
            double const * Arow = &A[(size_t)row * n];
            for (int c0 = 0; c0 < n; ++c0) {
                double ac = Arow[c0];
                if (ac == 0.0) continue;
                rhs[c0] += w * ac * targetPos[row];
                for (int c1 = 0; c1 < n; ++c1) {
                    H[(size_t)c0 * n + c1] += w * ac * Arow[c1];
                }
            }
        }
    }
    for (int v = 0; v < n; ++v) {
        H[(size_t)v * n + v] += lambda;
        rhs[v] += lambda * rest[v];
    }

    return denseSolve(H, rhs, n, solution);
}

//------------------------------------------------------------------------------
//  Small helpers
//------------------------------------------------------------------------------
static std::vector<double> perturbedRest(TestMesh const & m, double scale) {
    std::vector<double> x = m.rest;
    for (int v = 0; v < m.numVerts; ++v) {
        x[3*v+0] += scale * std::sin(1.0 + 1.5 * v);
        x[3*v+1] += scale * std::cos(2.0 + 1.1 * v);
        x[3*v+2] += scale * std::sin(0.4 + 2.0 * v);
    }
    return x;
}

static void normalize3(double v[3]) {
    double len = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (len > 0.0) { v[0] /= len; v[1] /= len; v[2] /= len; }
}

//  Build target positions/normals for a fitter from a perturbed rest state.
//  The normal target is computed from the perturbed limit surface (so the
//  fit is not rigid) and is unit length.
static void makeTargetsFromPerturbation(TestMesh const & m,
                                        std::vector<FitSample> const & samples,
                                        double scale,
                                        std::vector<double> & targetPos,
                                        std::vector<double> & targetNrm) {
    std::vector<double> xt = perturbedRest(m, scale);
    DirectEval e = evaluateDirectly(*m.refiner, xt, samples);
    targetPos = e.P;
    targetNrm = e.N;
    for (size_t i = 0; i < samples.size(); ++i) {
        normalize3(&targetNrm[3*i]);
    }
}

static double maxDiff(std::vector<double> const & a,
                      std::vector<double> const & b) {
    double d = 0.0;
    size_t n = std::min(a.size(), b.size());
    if (a.size() != b.size()) return 1.0e300;
    for (size_t i = 0; i < n; ++i) {
        d = std::max(d, std::fabs(a[i] - b[i]));
    }
    return d;
}

//  Rotate a vector about a normalized axis by the given angle (Rodrigues):
static void rotate3(double const axis[3], double angle, double v[3]) {
    double c = std::cos(angle), s = std::sin(angle);
    double ax[3] = { axis[0], axis[1], axis[2] };
    normalize3(ax);

    double dot = ax[0]*v[0] + ax[1]*v[1] + ax[2]*v[2];
    double cr[3];
    cr[0] = ax[1]*v[2] - ax[2]*v[1];
    cr[1] = ax[2]*v[0] - ax[0]*v[2];
    cr[2] = ax[0]*v[1] - ax[1]*v[0];

    for (int k = 0; k < 3; ++k) {
        v[k] = v[k]*c + cr[k]*s + ax[k]*dot*(1.0 - c);
    }
}

static void applyRigidTransform(double const axis[3], double angle,
                                double const translation[3],
                                std::vector<double> & positions) {
    for (size_t i = 0; i + 2 < positions.size(); i += 3) {
        double v[3] = { positions[i], positions[i+1], positions[i+2] };
        rotate3(axis, angle, v);
        positions[i+0] = v[0] + translation[0];
        positions[i+1] = v[1] + translation[1];
        positions[i+2] = v[2] + translation[2];
    }
}

static void rotateVectors(double const axis[3], double angle,
                          std::vector<double> & vectors) {
    for (size_t i = 0; i + 2 < vectors.size(); i += 3) {
        double v[3] = { vectors[i], vectors[i+1], vectors[i+2] };
        rotate3(axis, angle, v);
        vectors[i+0] = v[0];
        vectors[i+1] = v[1];
        vectors[i+2] = v[2];
    }
}

//  Return true if the reported records look consistent (finite and ordered):
static bool recordsAreFinite(Result const & r) {
    for (size_t i = 0; i < r.iterations.size(); ++i) {
        Fitter::IterationRecord const & rec = r.iterations[i];
        if (!std::isfinite(rec.energy) ||
            !std::isfinite(rec.freeGradientNorm) ||
            !std::isfinite(rec.minSampleArea) ||
            !std::isfinite(rec.stepNorm)) {
            return false;
        }
    }
    return !r.iterations.empty();
}

//==============================================================================
//  Group: linear-fit
//==============================================================================
int linearFitDenseCube(Check & check) {
    TestMesh mesh = makeAsymmetricCube();
    check.require(mesh.refiner.get() != 0, "cube refiner");

    //  Position-only samples (normalWeight == 0):
    std::vector<FitSample> samples;
    for (int f = 0; f < 6; ++f) {
        samples.push_back(FitSample(f, 0.28, 0.33, 1.0, 0.0));
        samples.push_back(FitSample(f, 0.62, 0.57, 0.7, 0.0));
    }

    Fitter fitter(*mesh.refiner, mesh.rest, samples.data(),
                  (int)samples.size());
    check.require(fitter.IsValid(), "cube fitter valid");

    std::vector<double> targetPos, targetNrm;
    makeTargetsFromPerturbation(mesh, samples, 0.08, targetPos, targetNrm);

    double lambda = 0.02;
    FitOptions options;
    options.SetLambda(lambda).SetGradientTolerance(1.0e-9)
           .SetMaxIterations(50);

    Result result = fitter.Solve(targetPos, targetNrm, options);
    check.require(result.HasSolution(), "cube position solve has solution");
    check.require(result.status == Fitter::STATUS_CONVERGED,
                  "cube position solve converged");
    check.require(result.iterations.back().freeGradientNorm <=
                  options.GetGradientTolerance(),
                  "cube position converged gradient within tolerance");

    //  The position-only problem is quadratic: an independent dense
    //  normal-equation solve must reproduce the fitter's answer.
    std::vector<double> dense;
    bool denseOk = densePositionMinimizer(*mesh.refiner, mesh.rest, samples,
                                          targetPos, lambda, dense);
    check.require(denseOk, "dense position minimizer solved");

    double diff = maxDiff(dense, result.controlPoints);
    check.require(diff < 2.0e-7, "fitter matches independent dense minimum");

    return 0;
}

int linearFitDenseLoop(Check & check) {
    TestMesh mesh = makeLoopTetrahedron();
    check.require(mesh.refiner.get() != 0, "loop refiner");

    //  Position-only samples so the problem is exactly quadratic:
    std::vector<FitSample> samples = trianglePositionSamples(4);
    Fitter fitter(*mesh.refiner, mesh.rest, samples.data(),
                  (int)samples.size());
    check.require(fitter.IsValid(), "loop fitter valid");

    std::vector<double> targetPos, targetNrm;
    makeTargetsFromPerturbation(mesh, samples, 0.07, targetPos, targetNrm);

    double lambda = 0.05;
    FitOptions options;
    options.SetLambda(lambda).SetGradientTolerance(1.0e-9)
           .SetMaxIterations(40);

    Result result = fitter.Solve(targetPos, targetNrm, options);
    check.require(result.HasSolution(), "loop position solve has solution");
    check.require(result.status == Fitter::STATUS_CONVERGED,
                  "loop position solve converged");

    std::vector<double> dense;
    bool denseOk = densePositionMinimizer(*mesh.refiner, mesh.rest, samples,
                                          targetPos, lambda, dense);
    check.require(denseOk, "loop dense minimizer solved");
    check.require(maxDiff(dense, result.controlPoints) < 2.0e-7,
                  "loop fitter matches independent dense minimum");

    //  Sanity: the rest term is active, so the solution is pulled off the
    //  exact interpolation of unweighted data:
    check.require(result.iterations.back().energy <=
                  result.iterations.front().energy + 1.0e-12,
                  "loop position energy decreased");

    return 0;
}

int linearFitSubsetSamples(Check & check) {
    //  Fewer samples than control points, with vertices that have no
    //  sample influence: the rest term must keep them at rest.
    TestMesh mesh = makeQuadGrid();
    check.require(mesh.refiner.get() != 0, "grid refiner");

    std::vector<FitSample> samples;
    samples.push_back(FitSample(0, 0.25, 0.35, 1.0, 0.0));
    samples.push_back(FitSample(0, 0.75, 0.6, 1.0, 0.0));

    Fitter fitter(*mesh.refiner, mesh.rest, samples.data(),
                  (int)samples.size());
    check.require(fitter.IsValid(), "subset fitter valid");

    //  Displace only one target position:
    std::vector<double> targetPos(3 * samples.size(), 0.0);
    std::vector<double> targetNrm(3 * samples.size(), 0.0);
    DirectEval base = evaluateDirectly(*mesh.refiner, mesh.rest, samples);
    targetPos = base.P;
    targetPos[0] += 0.05;
    targetPos[1] -= 0.03;

    FitOptions options;
    options.SetLambda(0.3).SetGradientTolerance(1.0e-10);

    Result result = fitter.Solve(targetPos, targetNrm, options);
    check.require(result.HasSolution(), "subset solve has solution");

    //  Identify affected coarse vertices from the sample stencils:
    std::vector<unsigned char> affected(mesh.numVerts, 0);
    {
        Bfr::RefinerSurfaceFactory<> factory(*mesh.refiner);
        Bfr::Surface<double> surf;
        for (size_t i = 0; i < samples.size(); ++i) {
            if (!factory.InitVertexSurface(samples[i].faceIndex, &surf)) {
                check.require(false, "subset sample surface");
            }
            std::vector<int> ids(surf.GetNumControlPoints());
            surf.GetControlPointIndices(ids.data());
            for (size_t k = 0; k < ids.size(); ++k) {
                affected[ids[k]] = 1;
            }
        }
    }
    int numAffected = 0;
    for (int v = 0; v < mesh.numVerts; ++v) {
        if (!affected[v]) {
            check.require(result.controlPoints[3*v+0] == mesh.rest[3*v+0],
                          "unaffected x stays at rest");
            check.require(result.controlPoints[3*v+1] == mesh.rest[3*v+1],
                          "unaffected y stays at rest");
            check.require(result.controlPoints[3*v+2] == mesh.rest[3*v+2],
                          "unaffected z stays at rest");
        } else {
            ++numAffected;
        }
    }
    check.require(numAffected > 0 && numAffected < mesh.numVerts,
                  "subset has both affected and unaffected vertices");
    return 0;
}

//==============================================================================
//  Group: normal-objective
//==============================================================================
int normalEnergyAndGradient(Check & check) {
    TestMesh mesh = makeAsymmetricCube();
    std::vector<FitSample> samples = simpleQuadSamples(6);
    Fitter fitter(*mesh.refiner, mesh.rest, samples.data(),
                  (int)samples.size());
    check.require(fitter.IsValid(), "normal fitter valid");

    std::vector<double> targetPos, targetNrm;
    makeTargetsFromPerturbation(mesh, samples, 0.05, targetPos, targetNrm);

    double lambda = 0.015;
    FitOptions options;
    options.SetLambda(lambda).SetGradientTolerance(1.0e-8)
           .SetMaxIterations(100);
    Result result = fitter.Solve(targetPos, targetNrm, options);

    check.require(result.HasSolution(), "normal solve has solution");
    check.require(result.status == Fitter::STATUS_CONVERGED,
                  "normal mixed solve converged");
    check.require(recordsAreFinite(result), "normal records finite");
    check.require(result.iterations.size() >= 1,
                  "normal has accepted records");

    //  Energy must have decreased (it cannot be required to be monotone
    //  beyond the tiny numerical allowance, but a net decrease is):
    check.require(result.iterations.back().energy <=
                  result.iterations.front().energy,
                  "normal energy decreased");
    check.require(result.iterations.back().minSampleArea > 1.0e-12,
                  "converged state has non-degenerate samples");

    //  Independent check of the complete objective at the returned state:
    DirectEval ev = evaluateDirectly(*mesh.refiner, result.controlPoints,
                                     samples);
    double E = completeEnergy(result.controlPoints, targetPos, targetNrm,
                              ev, samples, mesh.rest, lambda);
    double recorded = result.iterations.back().energy;
    check.require(std::fabs(E - recorded) <
                  1.0e-9 * std::max(1.0, std::fabs(recorded)),
                  "independent energy matches recorded energy");

    //  Central-difference free gradient (uses only independent direct
    //  evaluation, never the fitter's Jacobian):
    std::vector<unsigned char> anchored(mesh.numVerts, 0);
    double fdNorm = freeGradientByFD(*mesh.refiner, result.controlPoints,
                                     samples, mesh.rest,
                                     targetPos, targetNrm, lambda, anchored);
    double reported = result.iterations.back().freeGradientNorm;
    check.require(std::fabs(fdNorm - reported) <
                  1.0e-6 * std::max(1.0, reported),
                  "analytic free gradient matches central differences");
    check.require(reported <= options.GetGradientTolerance(),
                  "reported gradient meets tolerance at convergence");

    //  The gradient check must also hold away from the solution -- a zero
    //  gradient at convergence alone would not prove the Jacobian is right.
    //  A solve restricted to zero nonlinear iterations publishes the
    //  initial state record, whose gradient is deliberately non-zero:
    FitOptions initialOptions = options;
    initialOptions.SetMaxIterations(0);
    Result initial = fitter.Solve(targetPos, targetNrm, initialOptions);
    check.require(initial.iterations.size() == 1,
                  "zero-iteration solve publishes initial record");
    check.require(initial.iterations.front().freeGradientNorm > 1.0e-3,
                  "initial state has a non-trivial gradient");
    double fdInitial = freeGradientByFD(*mesh.refiner, initial.controlPoints,
                                        samples, mesh.rest, targetPos,
                                        targetNrm, lambda, anchored);
    check.require(std::fabs(fdInitial -
                            initial.iterations.front().freeGradientNorm) <
                  1.0e-6 * std::max(1.0, fdInitial),
                  "analytic gradient matches central differences at rest");

    //  Per-state records must carry recomputed energy, gradient and area:
    for (size_t i = 0; i < result.iterations.size(); ++i) {
        check.require(result.iterations[i].minSampleArea > 0.0,
                      "record has a positive minimum sample area");
    }

    return 0;
}

int normalChangesOptimum(Check & check) {
    TestMesh mesh = makeAsymmetricCube();

    //  Same samples, same position targets; normal weight toggled:
    std::vector<FitSample> samplesWithNormals, samplesWithout;
    for (int f = 0; f < 6; ++f) {
        samplesWithNormals.push_back(FitSample(f, 0.33, 0.37, 1.0, 1.0));
        samplesWithout.push_back(FitSample(f, 0.33, 0.37, 1.0, 0.0));
    }

    Fitter fitterA(*mesh.refiner, mesh.rest, samplesWithNormals.data(),
                   (int)samplesWithNormals.size());
    Fitter fitterB(*mesh.refiner, mesh.rest, samplesWithout.data(),
                   (int)samplesWithout.size());
    check.require(fitterA.IsValid() && fitterB.IsValid(),
                  "optimum comparison fitters valid");

    std::vector<double> targetPos, targetNrm;
    makeTargetsFromPerturbation(mesh, samplesWithNormals, 0.05,
                                targetPos, targetNrm);

    //  Deliberately bend the target normals away from the position-implied
    //  normals so the normal term has a distinct preference:
    for (size_t i = 0; i < samplesWithNormals.size(); ++i) {
        double tweak[3] = { 0.20, -0.15, 0.0 };
        targetNrm[3*i+0] += tweak[0];
        targetNrm[3*i+1] += tweak[1];
        normalize3(&targetNrm[3*i]);
    }

    FitOptions options;
    options.SetLambda(0.01).SetGradientTolerance(1.0e-8)
           .SetMaxIterations(100);

    Result ra = fitterA.Solve(targetPos, targetNrm, options);
    Result rb = fitterB.Solve(targetPos, targetNrm, options);
    check.require(ra.HasSolution() && rb.HasSolution(),
                  "both optimum comparison solves returned solutions");

    //  The normal term must genuinely change the optimal control net:
    double diff = maxDiff(ra.controlPoints, rb.controlPoints);
    check.require(diff > 1.0e-3, "normal term changes the optimal net");

    //  And both must reduce their own energy relative to the initial state:
    check.require(ra.iterations.back().energy <
                  ra.iterations.front().energy,
                  "normal-term fit reduced energy");

    return 0;
}

int normalTargetScaling(Check & check) {
    TestMesh mesh = makeAsymmetricCube();
    std::vector<FitSample> samples = simpleQuadSamples(6);
    Fitter fitter(*mesh.refiner, mesh.rest, samples.data(),
                  (int)samples.size());
    check.require(fitter.IsValid(), "scaled normal fitter valid");

    std::vector<double> targetPos, targetNrm;
    makeTargetsFromPerturbation(mesh, samples, 0.05, targetPos, targetNrm);

    FitOptions options;
    options.SetLambda(0.01).SetGradientTolerance(1.0e-9)
           .SetMaxIterations(80);

    Result r1 = fitter.Solve(targetPos, targetNrm, options);

    //  Positive scaling of target normals must not change the answer
    //  (target normals are normalized internally):
    std::vector<double> scaled = targetNrm;
    for (size_t i = 0; i < scaled.size(); ++i) scaled[i] *= 3.7;
    Result r2 = fitter.Solve(targetPos, scaled, options);

    check.require(r1.HasSolution() && r2.HasSolution(),
                  "normal scaling solves returned solutions");
    check.require(maxDiff(r1.controlPoints, r2.controlPoints) < 1.0e-10,
                  "normal direction scaling invariant");

    return 0;
}

int normalRigidTransform(Check & check) {
    TestMesh mesh = makeAsymmetricCube();
    std::vector<FitSample> samples = simpleQuadSamples(6);

    Fitter fitter(*mesh.refiner, mesh.rest, samples.data(),
                  (int)samples.size());
    check.require(fitter.IsValid(), "rigid fitter valid");

    std::vector<double> targetPos, targetNrm;
    makeTargetsFromPerturbation(mesh, samples, 0.05, targetPos, targetNrm);

    FitOptions options;
    options.SetLambda(0.01).SetGradientTolerance(1.0e-9)
           .SetMaxIterations(80);

    Result base = fitter.Solve(targetPos, targetNrm, options);
    check.require(base.HasSolution(), "rigid base solve");

    //  Rigidly transform mesh rest, targets and normals; the fitted control
    //  points must transform identically:
    double const axis[3] = { 0.4, -0.7, 0.55 };
    double const angle = 0.83;
    double const translation[3] = { 0.35, -1.1, 0.72 };

    std::vector<double> restT = mesh.rest;
    applyRigidTransform(axis, angle, translation, restT);

    std::vector<double> posT = targetPos;
    applyRigidTransform(axis, angle, translation, posT);

    std::vector<double> nrmT = targetNrm;
    rotateVectors(axis, angle, nrmT);

    Fitter transformed(*mesh.refiner, restT, samples.data(),
                       (int)samples.size());
    check.require(transformed.IsValid(), "transformed fitter valid");

    Result rt = transformed.Solve(posT, nrmT, options);
    check.require(rt.HasSolution(), "rigid transformed solve");

    std::vector<double> expected = base.controlPoints;
    applyRigidTransform(axis, angle, translation, expected);

    check.require(maxDiff(expected, rt.controlPoints) < 5.0e-8,
                  "fit is equivariant under rigid transforms");

    return 0;
}

int normalInvalidInitial(Check & check) {
    //  A degenerate initial state (all rest points collapsed) must be
    //  rejected before any iteration:
    TestMesh mesh = makeAsymmetricCube();
    std::vector<double> collapsed(3 * mesh.numVerts, 0.0);

    std::vector<FitSample> samples = simpleQuadSamples(6);
    Fitter fitter(*mesh.refiner, collapsed.data(), samples.data(),
                  (int)samples.size());
    check.require(fitter.IsValid(), "degenerate fitter constructed");

    std::vector<double> targetPos(3 * samples.size(), 0.0);
    std::vector<double> targetNrm(3 * samples.size(), 0.0);
    for (size_t i = 0; i < samples.size(); ++i) {
        targetNrm[3*i+2] = 1.0;
    }

    FitOptions options;
    options.SetLambda(0.01);
    Result result = fitter.Solve(targetPos, targetNrm, options);

    check.require(result.status == Fitter::STATUS_INVALID_INITIAL,
                  "degenerate initial state reported invalid_initial");
    check.require(result.iterations.empty(),
                  "invalid initial state publishes no accepted iterations");

    //  The opposite normals at a valid initial state are also invalid:
    Fitter valid(*mesh.refiner, mesh.rest, samples.data(),
                 (int)samples.size());
    DirectEval ev = evaluateDirectly(*mesh.refiner, mesh.rest, samples);
    std::vector<double> flipped = ev.N;
    for (size_t i = 0; i < flipped.size(); ++i) flipped[i] = -flipped[i];
    Result r2 = valid.Solve(targetPos, flipped, options);
    check.require(r2.status == Fitter::STATUS_INVALID_INITIAL,
                  "opposite target normals rejected as invalid_initial");

    return 0;
}

int normalNonQuadMesh(Check & check) {
    //  Non-quad (pentagon + quad) Catmark mesh with extraordinary vertices:
    TestMesh mesh = makePentagonalPrism();
    check.require(mesh.refiner.get() != 0, "prism refiner");

    std::vector<FitSample> samples = prismSamples();
    Fitter fitter(*mesh.refiner, mesh.rest, samples.data(),
                  (int)samples.size());
    check.require(fitter.IsValid(), "prism fitter valid");
    check.require(fitter.GetNumSamples() == (int)samples.size(),
                  "prism sample count");

    std::vector<double> targetPos, targetNrm;
    makeTargetsFromPerturbation(mesh, samples, 0.04, targetPos, targetNrm);

    FitOptions options;
    options.SetLambda(0.02).SetGradientTolerance(1.0e-8)
           .SetMaxIterations(100);
    Result result = fitter.Solve(targetPos, targetNrm, options);

    check.require(result.HasSolution(), "prism solve has solution");
    check.require(result.status == Fitter::STATUS_CONVERGED,
                  "prism solve converged");
    check.require(result.iterations.back().minSampleArea > 1.0e-12,
                  "prism samples remain non-degenerate");

    DirectEval ev = evaluateDirectly(*mesh.refiner, result.controlPoints,
                                     samples);
    double E = completeEnergy(result.controlPoints, targetPos, targetNrm,
                              ev, samples, mesh.rest, 0.02);
    check.require(std::fabs(E - result.iterations.back().energy) <
                  1.0e-9 * std::max(1.0, std::fabs(E)),
                  "prism independent energy matches");

    return 0;
}

int normalLoopMesh(Check & check) {
    TestMesh mesh = makeLoopTetrahedron();
    std::vector<FitSample> samples = triangleSamples(4);
    Fitter fitter(*mesh.refiner, mesh.rest, samples.data(),
                  (int)samples.size());
    check.require(fitter.IsValid(), "loop normal fitter valid");

    std::vector<double> targetPos, targetNrm;
    makeTargetsFromPerturbation(mesh, samples, 0.05, targetPos, targetNrm);

    FitOptions options;
    options.SetLambda(0.02).SetGradientTolerance(1.0e-8)
           .SetMaxIterations(100);
    Result result = fitter.Solve(targetPos, targetNrm, options);

    check.require(result.HasSolution(), "loop normal solve has solution");
    check.require(result.status == Fitter::STATUS_CONVERGED,
                  "loop normal solve converged");

    std::vector<unsigned char> anchored(mesh.numVerts, 0);
    double fdNorm = freeGradientByFD(*mesh.refiner, result.controlPoints,
                                     samples, mesh.rest, targetPos,
                                     targetNrm, 0.02, anchored);
    check.require(std::fabs(fdNorm -
                  result.iterations.back().freeGradientNorm) <
                  1.0e-6 * std::max(1.0, fdNorm),
                  "loop analytic gradient matches central differences");
    return 0;
}

int normalCreasedMesh(Check & check) {
    //  A cube with a fully sharpened edge loop: the crease changes the limit
    //  surface but samples remain inside smooth sub-patches away from it.
    TestMesh mesh = makeAsymmetricCube();
    //  Rebuild with two infinitely sharp edges (crease pairs (0,1) and (2,3)):
    std::vector<int> sizes(6, 4);
    std::vector<int> faces = {
        0,3,2,1,  4,5,6,7,  0,1,5,4,
        1,2,6,5,  2,3,7,6,  3,0,4,7
    };
    static std::vector<int> s3, f3;
    static std::vector<float> w3;
    s3 = sizes; f3 = faces;
    int creasePairs[4] = { 0,1, 2,3 };
    w3.assign(2, Sdc::Crease::SHARPNESS_INFINITE);

    Far::TopologyDescriptor desc;
    desc.numVertices = 8;
    desc.numFaces = 6;
    desc.numVertsPerFace = s3.data();
    desc.vertIndicesPerFace = f3.data();
    desc.numCreases = 2;
    desc.creaseVertexIndexPairs = creasePairs;
    desc.creaseWeights = w3.data();

    Sdc::Options opts;
    opts.SetVtxBoundaryInterpolation(Sdc::Options::VTX_BOUNDARY_EDGE_ONLY);
    typedef Far::TopologyRefinerFactory<Far::TopologyDescriptor> Factory;
    std::unique_ptr<Far::TopologyRefiner> creasedRefiner(
        Factory::Create(desc, Factory::Options(Sdc::SCHEME_CATMARK, opts)));
    check.require(creasedRefiner.get() != 0, "creased refiner created");

    std::vector<FitSample> samples = simpleQuadSamples(6);
    Fitter fitter(*creasedRefiner, mesh.rest, samples.data(),
                  (int)samples.size());
    check.require(fitter.IsValid(), "creased fitter valid");

    std::vector<double> targetPos, targetNrm;
    {
        //  Use the creased surface itself to construct mild targets:
        DirectEval e = evaluateDirectly(*creasedRefiner, mesh.rest, samples);
        targetPos = e.P;
        targetNrm = e.N;
        for (size_t i = 0; i < samples.size(); ++i) {
            targetPos[3*i+2] += 0.03 * std::sin(1.0 + (double)i);
        }
    }

    FitOptions options;
    options.SetLambda(0.01).SetGradientTolerance(1.0e-8)
           .SetMaxIterations(100);
    Result result = fitter.Solve(targetPos, targetNrm, options);
    check.require(result.HasSolution(), "creased solve has solution");
    check.require(result.status == Fitter::STATUS_CONVERGED,
                  "creased solve converged");

    std::vector<unsigned char> anchored(mesh.numVerts, 0);
    double fdNorm = freeGradientByFD(*creasedRefiner, result.controlPoints,
                                     samples, mesh.rest, targetPos,
                                     targetNrm, 0.01, anchored);
    check.require(std::fabs(fdNorm -
                  result.iterations.back().freeGradientNorm) <
                  1.0e-6 * std::max(1.0, fdNorm),
                  "creased analytic gradient matches central differences");
    return 0;
}

int normalOpenBoundary(Check & check) {
    //  An open quad grid: smooth boundary edges remain valid limit surface
    //  and samples defined on faces touching them must be fitted as usual.
    TestMesh mesh = makeQuadGrid();
    check.require(mesh.refiner.get() != 0, "open grid refiner");

    std::vector<FitSample> samples;
    int numFaces = mesh.refiner->GetLevel(0).GetNumFaces();
    for (int f = 0; f < numFaces; ++f) {
        samples.push_back(FitSample(f, 0.3, 0.32, 1.0, 0.0));
        samples.push_back(FitSample(f, 0.66, 0.62, 1.0, 1.0));
    }

    Fitter fitter(*mesh.refiner, mesh.rest, samples.data(),
                  (int)samples.size());
    check.require(fitter.IsValid(), "open boundary fitter valid");

    std::vector<double> targetPos, targetNrm;
    makeTargetsFromPerturbation(mesh, samples, 0.05, targetPos, targetNrm);

    FitOptions options;
    options.SetLambda(0.02).SetGradientTolerance(1.0e-8)
           .SetMaxIterations(120);
    Result result = fitter.Solve(targetPos, targetNrm, options);
    check.require(result.HasSolution(), "open boundary solve has solution");
    check.require(result.status == Fitter::STATUS_CONVERGED,
                  "open boundary solve converged");
    check.require(result.iterations.back().minSampleArea > 1.0e-12,
                  "open boundary samples non-degenerate");

    std::vector<unsigned char> anchored(mesh.numVerts, 0);
    double fdNorm = freeGradientByFD(*mesh.refiner, result.controlPoints,
                                     samples, mesh.rest, targetPos,
                                     targetNrm, 0.02, anchored);
    check.require(std::fabs(fdNorm -
                  result.iterations.back().freeGradientNorm) <
                  1.0e-6 * std::max(1.0, fdNorm),
                  "open boundary gradient matches central differences");
    return 0;
}

int normalExtraordinaryVertex(Check & check) {
    //  A closed Catmark mesh with an interior extraordinary vertex: the
    //  extraordinary neighborhood makes faces irregular, and the samples
    //  live inside smooth quad sub-patches away from the vertex itself.
    TestMesh mesh = makeIrregularVertexMesh();
    check.require(mesh.refiner.get() != 0, "irregular-vertex refiner");

    //  Samples on the side quads and on the two sub-patches of the bottom
    //  pentagon that do not touch the top center vertex:
    std::vector<FitSample> samples;
    for (int f = 1; f < 6; ++f) {   // side quads (faces 1..5)
        samples.push_back(FitSample(f, 0.32, 0.36, 1.0, 0.0));
        samples.push_back(FitSample(f, 0.64, 0.58, 0.7, 1.0));
    }
    samples.push_back(FitSample(0, 0.25, 0.20, 1.0, 0.0));

    Fitter fitter(*mesh.refiner, mesh.rest, samples.data(),
                  (int)samples.size());
    check.require(fitter.IsValid(), "irregular-vertex fitter valid");

    std::vector<double> targetPos, targetNrm;
    makeTargetsFromPerturbation(mesh, samples, 0.04, targetPos, targetNrm);

    FitOptions options;
    options.SetLambda(0.02).SetGradientTolerance(1.0e-8)
           .SetMaxIterations(120);
    Result result = fitter.Solve(targetPos, targetNrm, options);
    check.require(result.HasSolution(),
                  "irregular-vertex solve has solution");
    check.require(result.status == Fitter::STATUS_CONVERGED,
                  "irregular-vertex solve converged");
    check.require(result.iterations.back().minSampleArea > 1.0e-12,
                  "irregular-vertex samples non-degenerate");

    DirectEval ev = evaluateDirectly(*mesh.refiner, result.controlPoints,
                                     samples);
    double E = completeEnergy(result.controlPoints, targetPos, targetNrm,
                              ev, samples, mesh.rest, 0.02);
    check.require(std::fabs(E - result.iterations.back().energy) <
                  1.0e-9 * std::max(1.0, std::fabs(E)),
                  "irregular-vertex independent energy matches");
    return 0;
}

int normalPreventsFlips(Check & check) {
    //  A target normal field deliberately demanding a sharp turn toward
    //  another direction must not be met by folding the surface: every
    //  accepted state keeps a positive sample area and consistent normal
    //  orientation, even when the line search has to shorten the step.
    TestMesh mesh = makeAsymmetricCube();
    std::vector<FitSample> samples = simpleQuadSamples(6);
    Fitter fitter(*mesh.refiner, mesh.rest, samples.data(),
                  (int)samples.size());
    check.require(fitter.IsValid(), "flip fitter valid");

    std::vector<double> targetPos, targetNrm;
    DirectEval base = evaluateDirectly(*mesh.refiner, mesh.rest, samples);
    targetPos = base.P;
    targetNrm.assign(3 * samples.size(), 0.0);
    for (size_t i = 0; i < samples.size(); ++i) {
        //  Ask for normals tilted well away from the rest normals while
        //  remaining positively aligned at the rest state (so the initial
        //  state is valid). The solver must not satisfy the request by
        //  folding the surface.
        targetNrm[3*i+0] = base.N[3*i+0] + 0.4;
        targetNrm[3*i+1] = base.N[3*i+1] + 0.4;
        targetNrm[3*i+2] = base.N[3*i+2];
        normalize3(&targetNrm[3*i]);
    }

    FitOptions options;
    options.SetLambda(1.0e-3).SetGradientTolerance(1.0e-9)
           .SetMaxIterations(60);
    Result result = fitter.Solve(targetPos, targetNrm, options);
    //  The requested normal field may be unreachable without folding, so a
    //  solve that stalls (no_progress) or hits its iteration limit is an
    //  acceptable outcome -- what must never happen is convergence on a
    //  folded/degenerate state. Whenever a solution is published it must
    //  satisfy the non-degeneracy checks.
    check.require(result.status != Fitter::STATUS_INVALID_INPUT &&
                  result.status != Fitter::STATUS_INVALID_INITIAL,
                  "flip-prevention solve started from a valid state");

    if (result.HasSolution()) {
        for (size_t i = 0; i < result.iterations.size(); ++i) {
            check.require(result.iterations[i].minSampleArea > 1.0e-12,
                          "every accepted state keeps non-degenerate samples");
        }
        DirectEval ev = evaluateDirectly(*mesh.refiner,
                                         result.controlPoints, samples);
        for (size_t i = 0; i < samples.size(); ++i) {
            if (samples[i].normalWeight <= 0.0) continue;
            double dot = ev.N[3*i+0]*targetNrm[3*i+0] +
                         ev.N[3*i+1]*targetNrm[3*i+1] +
                         ev.N[3*i+2]*targetNrm[3*i+2];
            check.require(dot > 0.0, "accepted state has no normal flip");
        }
    }
    return 0;
}

//==============================================================================
//  Group: constraint-reuse
//==============================================================================
int constraintAnchorsExact(Check & check) {
    TestMesh mesh = makeAsymmetricCube();
    std::vector<FitSample> samples = simpleQuadSamples(6);
    Fitter fitter(*mesh.refiner, mesh.rest, samples.data(),
                  (int)samples.size());
    check.require(fitter.IsValid(), "anchor fitter valid");

    std::vector<double> targetPos, targetNrm;
    makeTargetsFromPerturbation(mesh, samples, 0.06, targetPos, targetNrm);

    //  Anchor two vertices, one at rest and one at an explicit position:
    std::vector<Anchor> anchors;
    anchors.push_back(Anchor(0));                     // fixed at rest
    double explicitPos[3] = { 0.9, -1.3, 0.4 };
    anchors.push_back(Anchor(3, explicitPos));

    FitOptions options;
    options.SetLambda(0.02).SetGradientTolerance(1.0e-8)
           .SetMaxIterations(100);
    Result result = fitter.Solve(targetPos, targetNrm, anchors, options);
    check.require(result.HasSolution(), "anchor solve has solution");

    //  Anchors must be exactly satisfied, not approximated:
    for (int k = 0; k < 3; ++k) {
        check.require(result.controlPoints[k] == mesh.rest[k],
                      "rest anchor exact");
    }
    for (int k = 0; k < 3; ++k) {
        check.require(result.controlPoints[9+k] == explicitPos[k],
                      "explicit anchor exact");
    }

    //  The free gradient (which must exclude anchored vertices) should be
    //  below the tolerance and match a central difference on free entries:
    check.require(result.iterations.back().freeGradientNorm <=
                  options.GetGradientTolerance(),
                  "free gradient converges with anchors");

    std::vector<unsigned char> anchored(mesh.numVerts, 0);
    anchored[0] = anchored[3] = 1;
    double fdNorm = freeGradientByFD(*mesh.refiner, result.controlPoints,
                                     samples, mesh.rest, targetPos,
                                     targetNrm, 0.02, anchored);
    check.require(fdNorm < 1.0e-6,
                  "anchored free gradient vanishes on free vertices");

    //  Any perturbed anchored output would break the exactness guarantee:
    check.require(result.controlPoints[3*3+0] - explicitPos[0] == 0.0,
                  "explicit anchor has no residual");

    return 0;
}

int constraintAllAnchored(Check & check) {
    TestMesh mesh = makeAsymmetricCube();
    std::vector<FitSample> samples = simpleQuadSamples(6);
    Fitter fitter(*mesh.refiner, mesh.rest, samples.data(),
                  (int)samples.size());
    check.require(fitter.IsValid(), "all-anchored fitter valid");

    std::vector<Anchor> anchors;
    for (int v = 0; v < mesh.numVerts; ++v) anchors.push_back(Anchor(v));

    std::vector<double> targetPos, targetNrm;
    makeTargetsFromPerturbation(mesh, samples, 0.06, targetPos, targetNrm);

    FitOptions options;
    options.SetLambda(0.02);
    Result result = fitter.Solve(targetPos, targetNrm, anchors, options);

    check.require(result.status == Fitter::STATUS_CONVERGED,
                  "fully anchored solve converged immediately");
    check.require(result.iterations.size() == 1,
                  "fully anchored solve has a single record");
    check.require(result.iterations.back().freeGradientNorm == 0.0,
                  "fully anchored free gradient is zero");
    check.require(maxDiff(result.controlPoints, mesh.rest) == 0.0,
                  "fully anchored output equals rest exactly");

    return 0;
}

int constraintNoInfluence(Check & check) {
    TestMesh mesh = makeQuadGrid();
    //  Samples confined to a single face: several coarse vertices have no
    //  sample influence at all and must stay exactly at rest.
    std::vector<FitSample> samples;
    samples.push_back(FitSample(0, 0.3, 0.35, 1.0, 1.0));
    samples.push_back(FitSample(0, 0.7, 0.6, 1.0, 1.0));

    Fitter fitter(*mesh.refiner, mesh.rest, samples.data(),
                  (int)samples.size());
    check.require(fitter.IsValid(), "no-influence fitter valid");

    std::vector<double> targetPos, targetNrm;
    makeTargetsFromPerturbation(mesh, samples, 0.06, targetPos, targetNrm);

    FitOptions options;
    options.SetLambda(0.05).SetGradientTolerance(1.0e-9);
    Result result = fitter.Solve(targetPos, targetNrm, options);
    check.require(result.HasSolution(), "no-influence solve");

    //  Vertices outside the coarse control set of face 0 cannot move:
    Bfr::RefinerSurfaceFactory<> factory(*mesh.refiner);
    Bfr::Surface<double> surf;
    check.require(factory.InitVertexSurface(0, &surf),
                  "no-influence face surface");
    std::vector<int> ids(surf.GetNumControlPoints());
    surf.GetControlPointIndices(ids.data());
    std::vector<unsigned char> used(mesh.numVerts, 0);
    for (size_t i = 0; i < ids.size(); ++i) used[ids[i]] = 1;

    int numUnused = 0;
    for (int v = 0; v < mesh.numVerts; ++v) {
        if (!used[v]) {
            ++numUnused;
            check.require(result.controlPoints[3*v+0] == mesh.rest[3*v+0],
                          "no-influence vertex x at rest");
            check.require(result.controlPoints[3*v+1] == mesh.rest[3*v+1],
                          "no-influence vertex y at rest");
            check.require(result.controlPoints[3*v+2] == mesh.rest[3*v+2],
                          "no-influence vertex z at rest");
        }
    }
    check.require(numUnused > 0, "mesh has unaffected vertices");

    return 0;
}

int constraintABAReuse(Check & check) {
    TestMesh mesh = makeAsymmetricCube();
    std::vector<FitSample> samples = simpleQuadSamples(6);

    Fitter fitter(*mesh.refiner, mesh.rest, samples.data(),
                  (int)samples.size());
    check.require(fitter.IsValid(), "ABA fitter valid");
    int buildsBefore = fitter.GetNumTemplateBuilds();
    check.require(buildsBefore == (int)samples.size(),
                  "template build count equals sample count");

    std::vector<double> posA, nrmA, posB, nrmB;
    makeTargetsFromPerturbation(mesh, samples, 0.05, posA, nrmA);
    makeTargetsFromPerturbation(mesh, samples, 0.12, posB, nrmB);

    FitOptions options;
    options.SetLambda(0.02).SetGradientTolerance(1.0e-9)
           .SetMaxIterations(100);

    Result ra1 = fitter.Solve(posA, nrmA, options);
    Result rb  = fitter.Solve(posB, nrmB, options);
    Result ra2 = fitter.Solve(posA, nrmA, options);

    check.require(ra1.HasSolution() && rb.HasSolution() && ra2.HasSolution(),
                  "ABA solves returned solutions");
    check.require(maxDiff(ra1.controlPoints, ra2.controlPoints) < 1.0e-13,
                  "A/B/A returns the A solution again");
    check.require(maxDiff(ra1.controlPoints, rb.controlPoints) > 1.0e-3,
                  "B differs from A");

    //  A fresh fitter on A must agree bit-for-bit with the repeated A:
    Fitter fresh(*mesh.refiner, mesh.rest, samples.data(),
                 (int)samples.size());
    check.require(fresh.IsValid(), "fresh fitter valid");
    Result freshA = fresh.Solve(posA, nrmA, options);
    check.require(maxDiff(ra2.controlPoints, freshA.controlPoints) == 0.0,
                  "repeated A equals new-object A exactly");

    //  Templates are built only at construction, never during solves:
    check.require(fitter.GetNumTemplateBuilds() == buildsBefore,
                  "Solve did not rebuild templates");
    check.require(ra1.numTemplateBuilds == buildsBefore &&
                  rb.numTemplateBuilds == buildsBefore &&
                  ra2.numTemplateBuilds == buildsBefore,
                  "results report the same template build count");

    return 0;
}

int constraintIterationLimit(Check & check) {
    TestMesh mesh = makeAsymmetricCube();
    std::vector<FitSample> samples = simpleQuadSamples(6);
    Fitter fitter(*mesh.refiner, mesh.rest, samples.data(),
                  (int)samples.size());
    check.require(fitter.IsValid(), "iteration limit fitter valid");

    std::vector<double> targetPos, targetNrm;
    makeTargetsFromPerturbation(mesh, samples, 0.2, targetPos, targetNrm);

    FitOptions options;
    options.SetLambda(0.001).SetGradientTolerance(1.0e-13)
           .SetMaxIterations(1)
           .SetMaxLineSearchSteps(20);

    Result result = fitter.Solve(targetPos, targetNrm, options);
    check.require(result.status == Fitter::STATUS_ITERATION_LIMIT ||
                  result.status == Fitter::STATUS_NO_PROGRESS,
                  "iteration limited solve reports limit/no-progress");
    check.require(result.HasSolution(), "iteration limited solve has solution");
    check.require(result.iterations.size() >= 1,
                  "iteration limited solve recorded accepted states");
    check.require(result.iterations.size() <= 2,
                  "iteration limit bounded the accepted states");
    check.require(result.iterations.back().freeGradientNorm >
                  options.GetGradientTolerance(),
                  "iteration limit did not falsely report convergence");

    return 0;
}

int constraintNoProgressIsNotConverged(Check & check) {
    //  A step that becomes numerically tiny but leaves the gradient above
    //  tolerance must be reported as no_progress, never converged.
    TestMesh mesh = makeAsymmetricCube();
    std::vector<FitSample> samples;
    for (int f = 0; f < 6; ++f) {
        samples.push_back(FitSample(f, 0.35, 0.4, 1.0, 1.0));
    }
    Fitter fitter(*mesh.refiner, mesh.rest, samples.data(),
                  (int)samples.size());
    check.require(fitter.IsValid(), "no-progress fitter valid");

    std::vector<double> targetPos, targetNrm;
    makeTargetsFromPerturbation(mesh, samples, 0.05, targetPos, targetNrm);

    FitOptions options;
    options.SetLambda(1.0e-8)
           .SetGradientTolerance(1.0e-14)
           .SetMaxIterations(200)
           .SetMaxLineSearchSteps(60);
    Result result = fitter.Solve(targetPos, targetNrm, options);
    check.require(result.HasSolution(), "no-progress solve has a solution");
    check.require(result.status == Fitter::STATUS_NO_PROGRESS ||
                  result.status == Fitter::STATUS_ITERATION_LIMIT,
                  "tiny steps do not report convergence");
    check.require(result.status != Fitter::STATUS_CONVERGED,
                  "never falsely converged");

    return 0;
}

int constraintRejectInvalidInputs(Check & check) {
    TestMesh mesh = makeAsymmetricCube();
    std::vector<FitSample> samples = simpleQuadSamples(6);

    //  Non-finite rest:
    {
        std::vector<double> badRest = mesh.rest;
        badRest[0] = std::numeric_limits<double>::quiet_NaN();
        Fitter f(*mesh.refiner, badRest.data(), samples.data(),
                 (int)samples.size());
        check.require(!f.IsValid(), "non-finite rest rejected");
        check.require(f.GetConstructionStatus() == Fitter::STATUS_INVALID_INPUT,
                      "non-finite rest status");
    }

    //  Out-of-range face index:
    {
        std::vector<FitSample> bad = samples;
        bad[0].faceIndex = 999;
        Fitter f(*mesh.refiner, mesh.rest, bad.data(), (int)bad.size());
        check.require(!f.IsValid(), "out-of-range face rejected");
    }

    //  Invalid UV (outside the unit square):
    {
        std::vector<FitSample> bad = samples;
        bad[0].u = 1.4;
        bad[0].v = 0.6;
        Fitter f(*mesh.refiner, mesh.rest, bad.data(), (int)bad.size());
        check.require(!f.IsValid(), "invalid uv rejected");
    }

    //  Negative weights:
    {
        std::vector<FitSample> bad = samples;
        bad[0].positionWeight = -1.0;
        Fitter f(*mesh.refiner, mesh.rest, bad.data(), (int)bad.size());
        check.require(!f.IsValid(), "negative position weight rejected");
    }
    {
        std::vector<FitSample> bad = samples;
        bad[0].normalWeight = -0.5;
        Fitter f(*mesh.refiner, mesh.rest, bad.data(), (int)bad.size());
        check.require(!f.IsValid(), "negative normal weight rejected");
    }

    //  Non-finite weight:
    {
        std::vector<FitSample> bad = samples;
        bad[0].normalWeight = std::numeric_limits<double>::infinity();
        Fitter f(*mesh.refiner, mesh.rest, bad.data(), (int)bad.size());
        check.require(!f.IsValid(), "non-finite weight rejected");
    }

    //  Bad rest length (vector overload):
    {
        std::vector<double> badRest(3 * mesh.numVerts + 1, 0.0);
        Fitter f(*mesh.refiner, badRest, samples);
        check.require(!f.IsValid(), "bad rest length rejected");
    }

    //  A vertex may not be anchored twice in one solve:
    {
        Fitter fitter(*mesh.refiner, mesh.rest, samples.data(),
                      (int)samples.size());
        check.require(fitter.IsValid(), "duplicate anchor fitter valid");
        std::vector<double> tp(3 * samples.size(), 0.0);
        std::vector<double> tn(3 * samples.size(), 0.0);
        for (size_t i = 0; i < samples.size(); ++i) tn[3*i+2] = 1.0;
        DirectEval ev = evaluateDirectly(*mesh.refiner, mesh.rest, samples);
        tp = ev.P;

        std::vector<Anchor> dup;
        dup.push_back(Anchor(1));
        dup.push_back(Anchor(1));
        FitOptions options;
        Result r = fitter.Solve(tp, tn, dup, options);
        check.require(r.status == Fitter::STATUS_INVALID_INPUT,
                      "duplicate anchors rejected");
        check.require(r.controlPoints.empty(),
                      "duplicate anchors publish no solution");
    }

    //  The rest weight must be strictly positive:
    {
        Fitter fitter(*mesh.refiner, mesh.rest, samples.data(),
                      (int)samples.size());
        std::vector<double> tp(3 * samples.size(), 0.0);
        std::vector<double> tn(3 * samples.size(), 0.0);
        FitOptions options;
        options.SetLambda(0.0);
        Result r = fitter.Solve(tp, tn, options);
        check.require(r.status == Fitter::STATUS_INVALID_INPUT,
                      "zero lambda rejected");
        options.SetLambda(-1.0);
        Result r2 = fitter.Solve(tp, tn, options);
        check.require(r2.status == Fitter::STATUS_INVALID_INPUT,
                      "negative lambda rejected");
    }

    //  Non-finite target values are rejected:
    {
        Fitter fitter(*mesh.refiner, mesh.rest, samples.data(),
                      (int)samples.size());
        std::vector<double> tp(3 * samples.size(), 0.0);
        std::vector<double> tn(3 * samples.size(), 0.0);
        for (size_t i = 0; i < samples.size(); ++i) tn[3*i+2] = 1.0;
        tp[0] = std::numeric_limits<double>::infinity();
        FitOptions options;
        Result r = fitter.Solve(tp, tn, options);
        check.require(r.status == Fitter::STATUS_INVALID_INPUT,
                      "non-finite target position rejected");

        std::vector<double> tp2(3 * samples.size(), 0.0);
        DirectEval ev = evaluateDirectly(*mesh.refiner, mesh.rest, samples);
        tp2 = ev.P;
        std::vector<double> tnGood = tn;
        Result rGood = fitter.Solve(tp2, tnGood, options);
        check.require(rGood.status != Fitter::STATUS_INVALID_INPUT,
                      "valid target normals accepted");
        std::vector<double> tnBad = tn;
        //  Sample 0 is position-only, so perturb a sample that uses normals:
        tnBad[3*1+0] = std::numeric_limits<double>::quiet_NaN();
        Result r2 = fitter.Solve(tp2, tnBad, options);
        check.require(r2.status == Fitter::STATUS_INVALID_INPUT,
                      "non-finite target normal rejected");
    }

    //  Wrong target-array lengths (vector overload) are rejected:
    {
        Fitter fitter(*mesh.refiner, mesh.rest, samples.data(),
                      (int)samples.size());
        std::vector<double> tp(3 * samples.size() - 1, 0.0);
        std::vector<double> tn(3 * samples.size(), 0.0);
        Result r = fitter.Solve(tp, tn);
        check.require(r.status == Fitter::STATUS_INVALID_INPUT,
                      "short target positions rejected");
    }

    //  A hole face is rejected (samples on it cannot be fit):
    {
        std::vector<int> sizes(6, 4);
        std::vector<int> faces = {
            0,3,2,1,  4,5,6,7,  0,1,5,4,
            1,2,6,5,  2,3,7,6,  3,0,4,7
        };
        static std::vector<int> s2, f2;
        s2 = sizes; f2 = faces;

        Far::TopologyDescriptor desc;
        desc.numVertices = 8;
        desc.numFaces = 6;
        desc.numVertsPerFace = s2.data();
        desc.vertIndicesPerFace = f2.data();
        int holeIndex = 0;
        desc.numHoles = 1;
        desc.holeIndices = &holeIndex;

        Sdc::Options opts;
        opts.SetVtxBoundaryInterpolation(
                        Sdc::Options::VTX_BOUNDARY_EDGE_ONLY);
        typedef Far::TopologyRefinerFactory<Far::TopologyDescriptor> Factory;
        std::unique_ptr<Far::TopologyRefiner> refiner(
            Factory::Create(desc, Factory::Options(Sdc::SCHEME_CATMARK,
                                                   opts)));
        check.require(refiner.get() != 0 && refiner->HasHoles(),
                      "hole mesh created");

        std::vector<FitSample> holeSamples;
        holeSamples.push_back(FitSample(0, 0.4, 0.4, 1.0, 0.0));
        Fitter f(*refiner, mesh.rest, holeSamples.data(),
                 (int)holeSamples.size());
        check.require(!f.IsValid(), "sample on hole face rejected");
    }

    return 0;
}

int constraintTemplateCounts(Check & check) {
    TestMesh mesh = makePentagonalPrism();
    std::vector<FitSample> samples = prismSamples();

    Fitter fitter(*mesh.refiner, mesh.rest, samples.data(),
                  (int)samples.size());
    check.require(fitter.IsValid(), "template count fitter valid");
    check.require(fitter.GetNumTemplateBuilds() == (int)samples.size(),
                  "builds counted during construction");
    check.require(fitter.GetNumVertices() == mesh.numVerts,
                  "vertex count reported");
    check.require(fitter.GetNumSamples() == (int)samples.size(),
                  "sample count reported");

    std::vector<double> tp, tn;
    makeTargetsFromPerturbation(mesh, samples, 0.04, tp, tn);

    FitOptions options;
    options.SetLambda(0.02).SetGradientTolerance(1.0e-8);
    Result r = fitter.Solve(tp, tn, options);
    check.require(r.HasSolution(), "template count solve");
    check.require(fitter.GetNumTemplateBuilds() == (int)samples.size(),
                  "Solve reused existing templates");
    check.require(r.numTemplateBuilds == (int)samples.size(),
                  "result reports template builds");

    //  The rest points exposed by the fitter are exactly those supplied at
    //  construction (never replaced by a solved state):
    std::vector<double> supplied(mesh.rest);
    std::vector<double> exposed(3 * mesh.numVerts);
    double const * restPtr = fitter.GetRestControlPoints();
    check.require(restPtr != 0, "rest points accessible");
    for (size_t i = 0; i < exposed.size(); ++i) exposed[i] = restPtr[i];
    check.require(maxDiff(supplied, exposed) == 0.0,
                  "rest exposed unchanged");
    return 0;
}

} // end anonymous namespace

//==============================================================================
//  Registration
//==============================================================================
void register_contracts(Registry & registry) {
    //  linear-fit
    registry.add("linear-fit", "dense-minimum-catmark", linearFitDenseCube);
    registry.add("linear-fit", "dense-minimum-loop", linearFitDenseLoop);
    registry.add("linear-fit", "subset-and-unaffected", linearFitSubsetSamples);

    //  normal-objective
    registry.add("normal-objective", "energy-and-gradient",
                 normalEnergyAndGradient);
    registry.add("normal-objective", "normal-changes-optimum",
                 normalChangesOptimum);
    registry.add("normal-objective", "normal-scale-invariance",
                 normalTargetScaling);
    registry.add("normal-objective", "rigid-transform", normalRigidTransform);
    registry.add("normal-objective", "invalid-initial", normalInvalidInitial);
    registry.add("normal-objective", "non-quad-mesh", normalNonQuadMesh);
    registry.add("normal-objective", "loop-mesh", normalLoopMesh);
    registry.add("normal-objective", "creased-mesh", normalCreasedMesh);
    registry.add("normal-objective", "open-boundary", normalOpenBoundary);
    registry.add("normal-objective", "extraordinary-vertex",
                 normalExtraordinaryVertex);
    registry.add("normal-objective", "prevents-flips", normalPreventsFlips);

    //  constraint-reuse
    registry.add("constraint-reuse", "anchors-exact", constraintAnchorsExact);
    registry.add("constraint-reuse", "all-anchored", constraintAllAnchored);
    registry.add("constraint-reuse", "no-influence", constraintNoInfluence);
    registry.add("constraint-reuse", "aba-reuse", constraintABAReuse);
    registry.add("constraint-reuse", "iteration-limit",
                 constraintIterationLimit);
    registry.add("constraint-reuse", "no-progress-not-converged",
                 constraintNoProgressIsNotConverged);
    registry.add("constraint-reuse", "reject-invalid-inputs",
                 constraintRejectInvalidInputs);
    registry.add("constraint-reuse", "template-build-counts",
                 constraintTemplateCounts);
}
