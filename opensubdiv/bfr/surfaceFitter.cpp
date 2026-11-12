//
//   Copyright 2026 Pixar
//
//   Licensed under the terms set forth in the LICENSE.txt file available at
//   https://opensubdiv.org/license.
//

#include "../bfr/surfaceFitter.h"

#include "../bfr/parameterization.h"
#include "../bfr/refinerSurfaceFactory.h"
#include "../bfr/surface.h"
#include "../far/topologyLevel.h"
#include "../far/topologyRefiner.h"
#include "../sdc/options.h"
#include "../sdc/types.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace OpenSubdiv {
namespace OPENSUBDIV_VERSION {

namespace Bfr {

namespace {
    typedef LimitSurfaceFitter::Real Real;

    //
    //  Small 3-vector helpers -- deliberately local to this implementation
    //  so no external numerical library is introduced:
    //
    inline void setVec(Real v[3], Real x, Real y, Real z) {
        v[0] = x; v[1] = y; v[2] = z;
    }
    inline Real dot3(Real const a[3], Real const b[3]) {
        return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
    }
    inline void cross3(Real const a[3], Real const b[3], Real c[3]) {
        c[0] = a[1]*b[2] - a[2]*b[1];
        c[1] = a[2]*b[0] - a[0]*b[2];
        c[2] = a[0]*b[1] - a[1]*b[0];
    }
    inline Real norm3(Real const a[3]) {
        return std::sqrt(dot3(a,a));
    }
    inline Real maxAbs3(Real const a[3]) {
        return std::max(std::fabs(a[0]),
               std::max(std::fabs(a[1]), std::fabs(a[2])));
    }
    inline bool isFinite(Real x) {
        return std::isfinite(x);
    }
    inline bool isFinite3(Real const a[3]) {
        return isFinite(a[0]) && isFinite(a[1]) && isFinite(a[2]);
    }

    //
    //  3x3 matrix stored column-major as m[column][row]:
    //
    struct Mat3 {
        Real m[3][3];
        Mat3() { std::memset(m, 0, sizeof(m)); }

        void setIdentity(Real s) {
            std::memset(m, 0, sizeof(m));
            m[0][0] = m[1][1] = m[2][2] = s;
        }
        void addIdentity(Real s) {
            m[0][0] += s; m[1][1] += s; m[2][2] += s;
        }
        void multiply(Real const x[3], Real r[3]) const {
            for (int row = 0; row < 3; ++row) {
                r[row] = m[0][row]*x[0] + m[1][row]*x[1] + m[2][row]*x[2];
            }
        }
        //  r = M^T x, i.e. r[row] = sum_c m[row][c] * x[c]
        void multiplyTranspose(Real const x[3], Real r[3]) const {
            for (int row = 0; row < 3; ++row) {
                r[row] = m[row][0]*x[0] + m[row][1]*x[1] + m[row][2]*x[2];
            }
        }
        //  Diagonal-block contribution of J^T J for one Jacobian entry:
        //  m[l2][l1] += s * sum_r J[l1][r] * J[l2][r]
        void addNormalDiag(Real s, Mat3 const & M) {
            for (int l1 = 0; l1 < 3; ++l1) {
                for (int l2 = 0; l2 < 3; ++l2) {
                    Real sum = 0.0;
                    for (int r = 0; r < 3; ++r) {
                        sum += M.m[l1][r] * M.m[l2][r];
                    }
                    m[l2][l1] += s * sum;
                }
            }
        }
    };

    //
    //  Inverting a small SPD 3x3 block (used only by the Jacobi
    //  preconditioner) -- falls back to a scaled identity on failure:
    //
    bool invert3(Mat3 const & A, Mat3 & inv) {
        Real a = A.m[0][0], b = A.m[1][0], c = A.m[2][0];
        Real d = A.m[0][1], e = A.m[1][1], f = A.m[2][1];
        Real g = A.m[0][2], h = A.m[1][2], i = A.m[2][2];

        Real co00 =  (e*i - f*h);
        Real co01 = -(d*i - f*g);
        Real co02 =  (d*h - e*g);
        Real det = a*co00 + b*co01 + c*co02;

        if (!isFinite(det) || det <= 0.0) return false;

        Real invDet = 1.0 / det;
        //  Adjugate entries, stored column-major as inv.m[column][row] --
        //  this is the standard inverse for a general 3x3, and the result
        //  is symmetric for the symmetric blocks used here.
        inv.m[0][0] =  co00 * invDet;
        inv.m[0][1] = -co01 * invDet;
        inv.m[0][2] =  co02 * invDet;
        inv.m[1][0] = -(b*i - c*h) * invDet;
        inv.m[1][1] =  (a*i - c*g) * invDet;
        inv.m[1][2] = -(a*h - b*g) * invDet;
        inv.m[2][0] =  (b*f - c*e) * invDet;
        inv.m[2][1] = -(a*f - c*d) * invDet;
        inv.m[2][2] =  (a*e - b*d) * invDet;
        return true;
    }

} // end namespace


//
//  FitSample:
//
LimitSurfaceFitter::FitSample::FitSample() :
        faceIndex(-1), u(0.0), v(0.0),
        positionWeight(0.0), normalWeight(0.0) {
}
LimitSurfaceFitter::FitSample::FitSample(Index face, Real uArg, Real vArg,
                                         Real posWeight, Real nrmWeight) :
        faceIndex(face), u(uArg), v(vArg),
        positionWeight(posWeight), normalWeight(nrmWeight) {
}


//
//  Anchor:
//
LimitSurfaceFitter::Anchor::Anchor() :
        vertexIndex(-1), positionIsRest(true) {
    setVec(position, 0.0, 0.0, 0.0);
}
LimitSurfaceFitter::Anchor::Anchor(Index vertex) :
        vertexIndex(vertex), positionIsRest(true) {
    setVec(position, 0.0, 0.0, 0.0);
}
LimitSurfaceFitter::Anchor::Anchor(Index vertex, Real const pos[3]) :
        vertexIndex(vertex), positionIsRest(false) {
    position[0] = pos[0]; position[1] = pos[1]; position[2] = pos[2];
}
LimitSurfaceFitter::Anchor::Anchor(Index vertex, Real x, Real y, Real z) :
        vertexIndex(vertex), positionIsRest(false) {
    setVec(position, x, y, z);
}


//
//  Options and result records:
//
LimitSurfaceFitter::Options::Options() :
        _lambda(1.0e-3),
        _gradientTolerance(1.0e-7),
        _maxIterations(80),
        _maxLineSearchSteps(40) {
}
LimitSurfaceFitter::Options &
LimitSurfaceFitter::Options::SetLambda(Real lambda) {
    _lambda = lambda;
    return *this;
}
LimitSurfaceFitter::Options &
LimitSurfaceFitter::Options::SetGradientTolerance(Real tolerance) {
    _gradientTolerance = tolerance;
    return *this;
}
LimitSurfaceFitter::Options &
LimitSurfaceFitter::Options::SetMaxIterations(int iterations) {
    _maxIterations = iterations;
    return *this;
}
LimitSurfaceFitter::Options &
LimitSurfaceFitter::Options::SetMaxLineSearchSteps(int steps) {
    _maxLineSearchSteps = steps;
    return *this;
}

LimitSurfaceFitter::IterationRecord::IterationRecord() :
        energy(0.0), freeGradientNorm(0.0),
        minSampleArea(0.0), stepNorm(0.0) {
}
LimitSurfaceFitter::Result::Result() :
        status(STATUS_INVALID_INPUT), numTemplateBuilds(0) {
}


//
//  Internal state of the fitter.
//
//  The geometry templates (the position, Du and Dv stencil coefficients of
//  each sample) are immutable after construction. Everything that changes
//  between solves -- targets, anchors and the rest weight -- is held in a
//  SolveState passed explicitly to the evaluation methods, so that no
//  Solve() changes the fitter's geometry state.
//
struct LimitSurfaceFitter::Impl {

    struct TemplateEntry {
        Index vertex;
        Real  wP, wDu, wDv;
    };
    struct Template {
        std::vector<TemplateEntry> entries;
    };

    struct SolveState {
        Real  lambda;
        std::vector<unsigned char> anchored;  // per coarse vertex
        std::vector<Index>         freeVertices;
        std::vector<Real>          targets;    // 3 values per sample
        std::vector<Real>          normals;    // 3 values per sample
    };

    //  Result of a full evaluation: the energy is always recomputed in
    //  full; the gradient (true gradient of the complete objective) and
    //  the Gauss-Newton normal blocks are optional.
    struct Evaluation {
        Evaluation() : energy(0.0), freeGradientNorm(0.0),
                       minSampleArea(0.0), normalsValid(true) { }

        Real energy;
        Real freeGradientNorm;
        Real minSampleArea;
        bool normalsValid;

        std::vector<Real> gradient;      // 3 values per vertex

        //  Per-sample Jacobian blocks of the unit normal with respect to
        //  the vertices of that sample's template. Each sample i occupies
        //  sampleBlockOffset[i] .. +templates[i].entries.size().
        std::vector<Mat3> sampleBlocks;
        std::vector<int>  sampleBlockOffset;
    };

    Impl() :
            mesh(0), numVertices(0), numSamples(0),
            constructionStatus(STATUS_INVALID_INPUT), numTemplateBuilds(0),
            valid(false), scheme(Sdc::SCHEME_CATMARK) {
    }

    //  Construction:
    bool initialize(Far::TopologyRefiner const & meshArg,
                    Real const restPoints[],
                    FitSample const samplesArg[], int sampleCount,
                    bool haveRest);
    bool validateTopology() const;
    bool validateSample(FitSample const & sample,
                        Parameterization const & param) const;
    bool buildTemplates();

    //  Evaluation and solve:
    void evaluate(Real const x[], SolveState const & state,
                  Evaluation & eval,
                  bool needGradient, bool needNormalBlocks) const;
    bool evaluateCandidate(Real const x[], SolveState const & state,
                           Real & energy, bool & normalsValid) const;
    void applyHessian(Real const d[], Real out[],
                      SolveState const & state,
                      Evaluation const & eval) const;
    void buildPreconditioner(SolveState const & state,
                             Evaluation const & eval, Mat3 diag[]) const;
    bool solveNewtonSystem(SolveState const & state, Evaluation const & eval,
                           Real rhs[], Real step[], Real & stepNorm) const;
    Result solve(Real const targetPositions[], Real const targetNormals[],
                 Anchor const anchors[], int numAnchors,
                 Options const & options, bool haveTargets,
                 bool haveNormals) const;

    //  Member variables:
    Far::TopologyRefiner const * mesh;
    std::unique_ptr<RefinerSurfaceFactory<> > factory;

    Sdc::SchemeType scheme;
    int  numVertices;
    int  numSamples;

    std::vector<Real>        rest;        // 3 values per coarse vertex
    std::vector<FitSample>   samples;     // fixed sample list
    std::vector<Template>    templates;   // fixed geometry templates

    Status constructionStatus;
    int    numTemplateBuilds;

    bool valid;
};


//
//  Topology validation: oriented manifold only, and only Catmark/Loop.
//
bool
LimitSurfaceFitter::Impl::validateTopology() const {

    if (!mesh) return false;

    Far::TopologyLevel const & level = mesh->GetLevel(0);

    int numEdges = level.GetNumEdges();
    for (int e = 0; e < numEdges; ++e) {
        if (level.IsEdgeNonManifold(e)) return false;

        Far::ConstIndexArray eFaces = level.GetEdgeFaces(e);
        if (eFaces.size() > 2) return false;

        if (eFaces.size() == 2) {
            Far::ConstIndexArray eVerts = level.GetEdgeVertices(e);
            Far::ConstLocalIndexArray eFaceLocals =
                    level.GetEdgeFaceLocalIndices(e);

            int dirs[2] = { 0, 0 };
            for (int i = 0; i < 2; ++i) {
                int f = eFaces[i];
                int n = level.GetFaceVertices(f).size();
                int local = eFaceLocals[i];
                int v0 = level.GetFaceVertices(f)[local];
                int v1 = level.GetFaceVertices(f)[(local + 1) % n];

                if (v0 == eVerts[0] && v1 == eVerts[1]) {
                    dirs[i] = 1;
                } else if (v0 == eVerts[1] && v1 == eVerts[0]) {
                    dirs[i] = -1;
                } else {
                    //  Unexpected edge/face relationship:
                    return false;
                }
            }
            if (dirs[0] == dirs[1]) return false;  // inconsistent winding
        }
    }

    int numVerts = level.GetNumVertices();
    for (int v = 0; v < numVerts; ++v) {
        if (level.IsVertexNonManifold(v)) return false;
    }
    return true;
}


//
//  Validate that the (u,v) of a sample lies strictly inside a smooth
//  sub-patch of the face's parameterization. This rejects coordinates in
//  the gaps between sub-faces (which the conversion clamps) as well as
//  coordinates on sub-face boundaries.
//
bool
LimitSurfaceFitter::Impl::validateSample(FitSample const & sample,
                                         Parameterization const & param) const {

    if (!param.IsValid()) return false;

    switch (param.GetType()) {
    case Parameterization::QUAD:
        //  Strictly interior to the unit square:
        return (sample.u > 0.0) && (sample.u < 1.0) &&
               (sample.v > 0.0) && (sample.v < 1.0);

    case Parameterization::TRI:
        //  Strictly interior to the triangular domain:
        return (sample.u > 0.0) && (sample.v > 0.0) &&
               (sample.u + sample.v < 1.0);

    case Parameterization::QUAD_SUBFACES: {
        //  Identify the sub-face and its normalized local coordinates.
        //  Because the conversion clamps coordinates in the gaps between
        //  sub-faces, a round-trip test is used to reject those.
        Real local[2] = { sample.u, sample.v };
        int subFace = param.ConvertCoordToNormalizedSubFace(local, local);
        if (subFace < 0 || subFace >= param.GetFaceSize()) return false;
        if (!isFinite(local[0]) || !isFinite(local[1])) return false;
        if (!(local[0] > 0.0) || !(local[0] < 1.0)) return false;
        if (!(local[1] > 0.0) || !(local[1] < 1.0)) return false;

        Real back[2] = { 0.0, 0.0 };
        param.ConvertNormalizedSubFaceToCoord(subFace, local, back);
        Real const tol = 1.0e-9 * (1.0 + std::fabs(sample.u) +
                                   std::fabs(sample.v));
        if (std::fabs(back[0] - sample.u) > tol) return false;
        if (std::fabs(back[1] - sample.v) > tol) return false;
        return true;
    }
    default:
        return false;
    }
}


//
//  Build the position/Du/Dv stencil for all samples. This is the only place
//  where limit-surface geometry is constructed; Solve() only reuses the
//  resulting coefficients.
//
bool
LimitSurfaceFitter::Impl::buildTemplates() {

    numTemplateBuilds = 0;
    templates.clear();
    templates.resize(numSamples);

    Surface<double> surface;
    std::vector<Index> cvIndices;
    std::vector<Real>  sP, sDu, sDv;

    for (int i = 0; i < numSamples; ++i) {
        FitSample const & sample = samples[i];
        Real uv[2] = { sample.u, sample.v };

        if (!factory->InitVertexSurface(sample.faceIndex, &surface)) {
            return false;
        }

        int numCVs = surface.GetNumControlPoints();
        cvIndices.resize(numCVs);
        surface.GetControlPointIndices(cvIndices.data());

        sP.resize(numCVs);
        sDu.resize(numCVs);
        sDv.resize(numCVs);
        if (surface.EvaluateStencil(uv, sP.data(), sDu.data(), sDv.data())
                != numCVs) {
            return false;
        }

        Template & t = templates[i];
        t.entries.clear();
        t.entries.reserve(numCVs);
        for (int k = 0; k < numCVs; ++k) {
            if (!isFinite(sP[k]) || !isFinite(sDu[k]) || !isFinite(sDv[k])) {
                return false;
            }
            if (cvIndices[k] < 0 || cvIndices[k] >= numVertices) return false;

            if (sP[k] == 0.0 && sDu[k] == 0.0 && sDv[k] == 0.0) continue;

            TemplateEntry entry;
            entry.vertex = cvIndices[k];
            entry.wP     = sP[k];
            entry.wDu    = sDu[k];
            entry.wDv    = sDv[k];
            t.entries.push_back(entry);
        }
        ++numTemplateBuilds;
    }
    return true;
}


//
//  Full construction:
//
bool
LimitSurfaceFitter::Impl::initialize(Far::TopologyRefiner const & meshArg,
                                     Real const restPoints[],
                                     FitSample const samplesArg[],
                                     int sampleCount, bool haveRest) {

    mesh = &meshArg;
    scheme = meshArg.GetSchemeType();
    numVertices = meshArg.GetLevel(0).GetNumVertices();
    numSamples = sampleCount;

    constructionStatus = STATUS_INVALID_INPUT;
    valid = false;
    numTemplateBuilds = 0;

    if (!haveRest) return false;
    if (numVertices <= 0) return false;
    if (sampleCount < 0) return false;
    if (!samplesArg && sampleCount > 0) return false;
    if (!restPoints) return false;

    rest.resize(3 * numVertices);
    for (int i = 0; i < 3 * numVertices; ++i) {
        if (!isFinite(restPoints[i])) return false;
        rest[i] = restPoints[i];
    }

    if (scheme != Sdc::SCHEME_CATMARK && scheme != Sdc::SCHEME_LOOP) {
        return false;
    }
    if (!validateTopology()) return false;

    //  Validate all samples before building anything:
    samples.resize(numSamples);
    for (int i = 0; i < numSamples; ++i) {
        FitSample const & s = samplesArg[i];

        if (!isFinite(s.u) || !isFinite(s.v) ||
            !isFinite(s.positionWeight) || !isFinite(s.normalWeight)) {
            return false;
        }
        if (!(s.positionWeight >= 0.0) || !(s.normalWeight >= 0.0)) {
            return false;
        }
        if (s.positionWeight == 0.0 && s.normalWeight == 0.0) return false;

        if (s.faceIndex < 0 ||
            s.faceIndex >= meshArg.GetLevel(0).GetNumFaces()) return false;

        if (meshArg.HasHoles() &&
            meshArg.GetLevel(0).IsFaceHole(s.faceIndex)) return false;

        int faceSize = meshArg.GetLevel(0)
                            .GetFaceVertices(s.faceIndex).size();
        Parameterization param(scheme, faceSize);
        if (!validateSample(s, param)) return false;

        samples[i] = s;
    }

    factory.reset(new RefinerSurfaceFactory<>(meshArg));

    if (!buildTemplates()) return false;

    constructionStatus = STATUS_OK;
    valid = true;
    return true;
}


//
//  Evaluate a state completely.  The energy is always recomputed in full.
//
void
LimitSurfaceFitter::Impl::evaluate(Real const x[], SolveState const & state,
                                   Evaluation & eval,
                                   bool needGradient,
                                   bool needNormalBlocks) const {

    int dimension = 3 * numVertices;

    eval.energy = 0.0;
    eval.freeGradientNorm = 0.0;
    eval.minSampleArea = (numSamples > 0)
                       ? std::numeric_limits<Real>::max() : Real(0.0);
    eval.normalsValid = true;

    if (needGradient) {
        eval.gradient.assign(dimension, 0.0);
    } else {
        eval.gradient.clear();
    }
    if (needNormalBlocks) {
        int totalEntries = 0;
        eval.sampleBlockOffset.resize(numSamples);
        for (int i = 0; i < numSamples; ++i) {
            eval.sampleBlockOffset[i] = totalEntries;
            totalEntries += (int)templates[i].entries.size();
        }
        eval.sampleBlocks.assign(totalEntries, Mat3());
    } else {
        eval.sampleBlocks.clear();
        eval.sampleBlockOffset.clear();
    }

    //
    //  Data terms:
    //
    for (int i = 0; i < numSamples; ++i) {
        Template const & t = templates[i];
        Real const * targetPos   = &state.targets[3*i];
        Real const * targetNrm   = &state.normals[3*i];

        Real P[3] = { 0.0, 0.0, 0.0 };
        Real Du[3] = { 0.0, 0.0, 0.0 };
        Real Dv[3] = { 0.0, 0.0, 0.0 };

        for (size_t k = 0; k < t.entries.size(); ++k) {
            TemplateEntry const & e = t.entries[k];
            Real const * xv = &x[3 * e.vertex];

            P[0]  += e.wP  * xv[0]; P[1]  += e.wP  * xv[1];
            P[2]  += e.wP  * xv[2];
            Du[0] += e.wDu * xv[0]; Du[1] += e.wDu * xv[1];
            Du[2] += e.wDu * xv[2];
            Dv[0] += e.wDv * xv[0]; Dv[1] += e.wDv * xv[1];
            Dv[2] += e.wDv * xv[2];
        }

        Real cross[3];
        cross3(Du, Dv, cross);
        Real area = norm3(cross);
        if (area < eval.minSampleArea) eval.minSampleArea = area;

        //  Position term:
        Real dp[3] = { P[0] - targetPos[0],
                       P[1] - targetPos[1],
                       P[2] - targetPos[2] };
        Real wp = samples[i].positionWeight;
        eval.energy += wp * dot3(dp, dp);

        if (needGradient && wp > 0.0) {
            //  gradient = 2 wp * dP/dX * (P - p)
            for (size_t k = 0; k < t.entries.size(); ++k) {
                TemplateEntry const & e = t.entries[k];
                Real s = 2.0 * wp * e.wP;
                eval.gradient[3 * e.vertex + 0] += s * dp[0];
                eval.gradient[3 * e.vertex + 1] += s * dp[1];
                eval.gradient[3 * e.vertex + 2] += s * dp[2];
            }
        }

        //  Normal term (only on samples with a positive normal weight):
        Real wn = samples[i].normalWeight;
        Real n[3] = { 0.0, 0.0, 0.0 };
        Real eperp[3] = { 0.0, 0.0, 0.0 };
        if (wn > 0.0) {
            bool ok = isFinite(area) && (area > 1.0e-12);
            if (ok) {
                n[0] = cross[0] / area;
                n[1] = cross[1] / area;
                n[2] = cross[2] / area;
                ok = isFinite3(n) && (dot3(n, targetNrm) > 0.0);
            }
            if (!ok) {
                eval.normalsValid = false;
            } else {
                Real en[3] = { n[0] - targetNrm[0],
                               n[1] - targetNrm[1],
                               n[2] - targetNrm[2] };
                eval.energy += wn * dot3(en, en);

                //  Tangential part of (n - n*), used for the analytic
                //  gradient below: (I - n n^T)(n - n*) = -n* - (n . n*) n
                Real enDotN = dot3(en, n);
                eperp[0] = en[0] - enDotN * n[0];
                eperp[1] = en[1] - enDotN * n[1];
                eperp[2] = en[2] - enDotN * n[2];
            }
        }

        //
        //  Analytic derivatives of the unit normal with respect to each
        //  coarse vertex of the sample's template. Since n = c/|c| with
        //  c = Du x Dv:
        //
        //      dn/dX(v,l) = (I - n n^T)/|c| * dc/dX(v,l)
        //      dc/dX(v,l) = wDu_v (e_l x Dv) + wDv_v (Du x e_l)
        //
        //  and, with the energy term  wn |n - n*|^2:
        //
        //      dE/dX(v,l) = 2 wn (dn/dX(v,l)) . (n - n*)
        //                 = 2 wn/|c| [ wDu_v (Dv x eperp)_l
        //                             + wDv_v (eperp x Du)_l ]
        //
        if (wn > 0.0 && eval.normalsValid &&
            (needGradient || needNormalBlocks)) {

            Real invArea = 1.0 / area;

            if (needGradient) {
                Real a[3], b[3];
                cross3(Dv, eperp, a);
                cross3(eperp, Du, b);
                for (size_t k = 0; k < t.entries.size(); ++k) {
                    TemplateEntry const & e = t.entries[k];
                    Real s = 2.0 * wn * invArea;
                    Real ga = s * e.wDu, gb = s * e.wDv;
                    eval.gradient[3 * e.vertex + 0] += ga*a[0] + gb*b[0];
                    eval.gradient[3 * e.vertex + 1] += ga*a[1] + gb*b[1];
                    eval.gradient[3 * e.vertex + 2] += ga*a[2] + gb*b[2];
                }
            }
            if (needNormalBlocks) {
                int base = eval.sampleBlockOffset[i];
                for (size_t k = 0; k < t.entries.size(); ++k) {
                    TemplateEntry const & e = t.entries[k];
                    Mat3 & block = eval.sampleBlocks[base + (int)k];
                    for (int l = 0; l < 3; ++l) {
                        Real el[3] = { 0.0, 0.0, 0.0 };
                        el[l] = 1.0;
                        Real t1[3], t2[3];
                        cross3(el, Dv, t1);
                        cross3(Du, el, t2);
                        Real dc[3];
                        for (int r = 0; r < 3; ++r) {
                            dc[r] = e.wDu * t1[r] + e.wDv * t2[r];
                        }
                        Real dcDotN = dot3(dc, n);
                        //  The block stores J = dn/dX column-major, i.e.
                        //  m[column=input][row=output].
                        for (int r = 0; r < 3; ++r) {
                            block.m[l][r] +=
                                invArea * (dc[r] - dcDotN * n[r]);
                        }
                    }
                }
            }
        }
    }

    //
    //  Rest term (includes anchored vertices; their values are fixed so
    //  this contributes a constant to the energy and nothing to the
    //  gradient of the free variables):
    //
    for (int v = 0; v < numVertices; ++v) {
        Real dr[3] = { x[3*v+0] - rest[3*v+0],
                       x[3*v+1] - rest[3*v+1],
                       x[3*v+2] - rest[3*v+2] };
        eval.energy += state.lambda * dot3(dr, dr);

        if (needGradient && !state.anchored[v]) {
            Real s = 2.0 * state.lambda;
            eval.gradient[3*v+0] += s * dr[0];
            eval.gradient[3*v+1] += s * dr[1];
            eval.gradient[3*v+2] += s * dr[2];
        }
    }

    if (needGradient) {
        Real norm = 0.0;
        for (size_t i = 0; i < state.freeVertices.size(); ++i) {
            Index v = state.freeVertices[i];
            norm = std::max(norm, maxAbs3(&eval.gradient[3*v]));
        }
        eval.freeGradientNorm = norm;
    }
}


//
//  Lightweight evaluation used by the line search (energy and validity only)
//
bool
LimitSurfaceFitter::Impl::evaluateCandidate(Real const x[],
                                            SolveState const & state,
                                            Real & energy,
                                            bool & normalsValid) const {

    energy = 0.0;
    normalsValid = true;

    for (int i = 0; i < numSamples; ++i) {
        Template const & t = templates[i];
        Real const * targetPos = &state.targets[3*i];
        Real const * targetNrm = &state.normals[3*i];

        Real P[3] = { 0.0, 0.0, 0.0 };
        Real Du[3] = { 0.0, 0.0, 0.0 };
        Real Dv[3] = { 0.0, 0.0, 0.0 };
        for (size_t k = 0; k < t.entries.size(); ++k) {
            TemplateEntry const & e = t.entries[k];
            Real const * xv = &x[3 * e.vertex];
            P[0]  += e.wP  * xv[0]; P[1]  += e.wP  * xv[1];
            P[2]  += e.wP  * xv[2];
            Du[0] += e.wDu * xv[0]; Du[1] += e.wDu * xv[1];
            Du[2] += e.wDu * xv[2];
            Dv[0] += e.wDv * xv[0]; Dv[1] += e.wDv * xv[1];
            Dv[2] += e.wDv * xv[2];
        }

        Real dp[3] = { P[0] - targetPos[0],
                       P[1] - targetPos[1],
                       P[2] - targetPos[2] };
        energy += samples[i].positionWeight * dot3(dp, dp);

        Real wn = samples[i].normalWeight;
        if (wn > 0.0) {
            Real cross[3];
            cross3(Du, Dv, cross);
            Real area = norm3(cross);
            if (!isFinite(area) || !(area > 1.0e-12)) {
                normalsValid = false;
                continue;
            }
            Real n[3] = { cross[0] / area, cross[1] / area,
                          cross[2] / area };
            if (!isFinite3(n) || !(dot3(n, targetNrm) > 0.0)) {
                normalsValid = false;
                continue;
            }
            Real en[3] = { n[0] - targetNrm[0],
                           n[1] - targetNrm[1],
                           n[2] - targetNrm[2] };
            energy += wn * dot3(en, en);
        }
    }

    for (int v = 0; v < numVertices; ++v) {
        Real dr[3] = { x[3*v+0] - rest[3*v+0],
                       x[3*v+1] - rest[3*v+1],
                       x[3*v+2] - rest[3*v+2] };
        energy += state.lambda * dot3(dr, dr);
    }
    return normalsValid;
}


//
//  Apply the Gauss-Newton Hessian to a free-vector.  The Hessian is
//  block sparse (only vertices sharing a sample are coupled) and is never
//  assembled into a dense (3V x 3V) matrix.
//
void
LimitSurfaceFitter::Impl::applyHessian(Real const d[], Real out[],
                                       SolveState const & state,
                                       Evaluation const & eval) const {

    int dimension = 3 * numVertices;
    for (int i = 0; i < dimension; ++i) out[i] = 0.0;

    for (int i = 0; i < numSamples; ++i) {
        Template const & t = templates[i];

        //  Position part of 2 * (J^T J) d:  2 wp * (sum_v wP_v d_v) * wP_v
        Real wp = samples[i].positionWeight;
        if (wp > 0.0) {
            Real s[3] = { 0.0, 0.0, 0.0 };
            for (size_t k = 0; k < t.entries.size(); ++k) {
                TemplateEntry const & e = t.entries[k];
                Real const * dv = &d[3 * e.vertex];
                s[0] += e.wP * dv[0];
                s[1] += e.wP * dv[1];
                s[2] += e.wP * dv[2];
            }
            Real sc = 2.0 * wp;
            for (size_t k = 0; k < t.entries.size(); ++k) {
                TemplateEntry const & e = t.entries[k];
                Real w = sc * e.wP;
                out[3*e.vertex+0] += w * s[0];
                out[3*e.vertex+1] += w * s[1];
                out[3*e.vertex+2] += w * s[2];
            }
        }

        //  Normal part of 2 * (J^T J) d:  2 wn * J^T (J d)
        Real wn = samples[i].normalWeight;
        if (wn > 0.0) {
            int base = eval.sampleBlockOffset[i];
            Real s[3] = { 0.0, 0.0, 0.0 };
            for (size_t k = 0; k < t.entries.size(); ++k) {
                TemplateEntry const & e = t.entries[k];
                Real const * dv = &d[3 * e.vertex];
                Real dn[3];
                eval.sampleBlocks[base + (int)k].multiply(dv, dn);
                s[0] += dn[0]; s[1] += dn[1]; s[2] += dn[2];
            }
            Real sc = 2.0 * wn;
            Real ssum[3] = { sc * s[0], sc * s[1], sc * s[2] };
            for (size_t k = 0; k < t.entries.size(); ++k) {
                TemplateEntry const & e = t.entries[k];
                Real acc[3];
                eval.sampleBlocks[base + (int)k].multiplyTranspose(ssum, acc);
                out[3*e.vertex+0] += acc[0];
                out[3*e.vertex+1] += acc[1];
                out[3*e.vertex+2] += acc[2];
            }
        }
    }

    //  Rest term of 2 * (lambda I):
    Real s = 2.0 * state.lambda;
    for (size_t i = 0; i < state.freeVertices.size(); ++i) {
        Index v = state.freeVertices[i];
        for (int r = 0; r < 3; ++r) {
            out[3*v+r] += s * d[3*v+r];
        }
    }
}


//
//  Build the block-diagonal Jacobi preconditioner (using the same 2x
//  scaling as the Hessian; the scale factor cancels in the solve).
//
void
LimitSurfaceFitter::Impl::buildPreconditioner(SolveState const & state,
                                              Evaluation const & eval,
                                              Mat3 diag[]) const {

    Real lambda2 = 2.0 * state.lambda;
    for (size_t i = 0; i < state.freeVertices.size(); ++i) {
        Index v = state.freeVertices[i];
        diag[v].setIdentity(lambda2);
    }

    for (int i = 0; i < numSamples; ++i) {
        Template const & t = templates[i];

        Real wp = samples[i].positionWeight;
        if (wp > 0.0) {
            for (size_t k = 0; k < t.entries.size(); ++k) {
                TemplateEntry const & e = t.entries[k];
                if (state.anchored[e.vertex]) continue;
                diag[e.vertex].addIdentity(2.0 * wp * e.wP * e.wP);
            }
        }
        Real wn = samples[i].normalWeight;
        if (wn > 0.0) {
            int base = eval.sampleBlockOffset[i];
            for (size_t k = 0; k < t.entries.size(); ++k) {
                TemplateEntry const & e = t.entries[k];
                if (state.anchored[e.vertex]) continue;
                diag[e.vertex].addNormalDiag(
                        2.0 * wn, eval.sampleBlocks[base + (int)k]);
            }
        }
    }
}


//
//  Solve the Gauss-Newton system H d = rhs with preconditioned conjugate
//  gradients.  Only the free variables participate; anchored components of
//  the vector remain zero.
//
bool
LimitSurfaceFitter::Impl::solveNewtonSystem(SolveState const & state,
                                            Evaluation const & eval,
                                            Real rhs[], Real step[],
                                            Real & stepNorm) const {

    int dimension = 3 * numVertices;

    std::vector<Mat3> precond(numVertices);
    buildPreconditioner(state, eval, precond.data());

    std::vector<Mat3> invDiag(numVertices);
    for (size_t i = 0; i < state.freeVertices.size(); ++i) {
        Index v = state.freeVertices[i];
        if (!invert3(precond[v], invDiag[v])) {
            Real scale = 0.0;
            for (int r = 0; r < 3; ++r) scale += precond[v].m[r][r];
            scale = (scale > 0.0) ? (3.0 / scale) : 1.0;
            invDiag[v].setIdentity(scale);
        }
    }

    std::vector<Real> r(dimension, 0.0), z(dimension, 0.0),
                      p(dimension, 0.0), Ap(dimension, 0.0);

    for (int i = 0; i < dimension; ++i) step[i] = 0.0;

    Real rhsNorm = 0.0;
    for (size_t i = 0; i < state.freeVertices.size(); ++i) {
        Index v = state.freeVertices[i];
        rhsNorm = std::max(rhsNorm, maxAbs3(&rhs[3*v]));
    }
    if (rhsNorm == 0.0) {
        stepNorm = 0.0;
        return true;
    }
    for (int i = 0; i < dimension; ++i) r[i] = rhs[i];

    //  Preconditioned residual and initial search direction:
    for (size_t i = 0; i < state.freeVertices.size(); ++i) {
        Index v = state.freeVertices[i];
        invDiag[v].multiply(&r[3*v], &z[3*v]);
    }
    for (size_t i = 0; i < state.freeVertices.size(); ++i) {
        Index v = state.freeVertices[i];
        p[3*v+0] = z[3*v+0]; p[3*v+1] = z[3*v+1]; p[3*v+2] = z[3*v+2];
    }

    Real rz = 0.0;
    for (size_t i = 0; i < state.freeVertices.size(); ++i) {
        Index v = state.freeVertices[i];
        rz += r[3*v+0]*z[3*v+0] + r[3*v+1]*z[3*v+1] +
              r[3*v+2]*z[3*v+2];
    }
    if (!isFinite(rz) || rz <= 0.0) {
        stepNorm = 0.0;
        return false;
    }

    int maxIterations = std::min(3 * (int)state.freeVertices.size() + 10,
                                 2000);
    if (maxIterations < 1) maxIterations = 1;

    Real tol = 1.0e-12 * std::max(Real(1.0), rhsNorm);

    bool ok = false;
    for (int it = 0; it < maxIterations; ++it) {
        //  Ap = H p  (only free parts matter; anchored entries are zero)
        applyHessian(p.data(), Ap.data(), state, eval);

        Real pAp = 0.0;
        for (size_t i = 0; i < state.freeVertices.size(); ++i) {
            Index v = state.freeVertices[i];
            pAp += p[3*v+0]*Ap[3*v+0] + p[3*v+1]*Ap[3*v+1] +
                   p[3*v+2]*Ap[3*v+2];
        }
        if (!isFinite(pAp) || pAp <= 0.0) break;

        Real alpha = rz / pAp;
        for (size_t i = 0; i < state.freeVertices.size(); ++i) {
            Index v = state.freeVertices[i];
            for (int row = 0; row < 3; ++row) {
                step[3*v+row] += alpha * p[3*v+row];
                r[3*v+row]    -= alpha * Ap[3*v+row];
            }
        }

        Real rNorm = 0.0;
        for (size_t i = 0; i < state.freeVertices.size(); ++i) {
            Index v = state.freeVertices[i];
            rNorm = std::max(rNorm, maxAbs3(&r[3*v]));
        }
        if (rNorm <= tol) { ok = true; break; }

        for (size_t i = 0; i < state.freeVertices.size(); ++i) {
            Index v = state.freeVertices[i];
            invDiag[v].multiply(&r[3*v], &z[3*v]);
        }

        Real rzNew = 0.0;
        for (size_t i = 0; i < state.freeVertices.size(); ++i) {
            Index v = state.freeVertices[i];
            rzNew += r[3*v+0]*z[3*v+0] + r[3*v+1]*z[3*v+1] +
                     r[3*v+2]*z[3*v+2];
        }
        if (!isFinite(rzNew) || rzNew < 0.0) break;

        Real beta = rzNew / rz;
        rz = rzNew;
        for (size_t i = 0; i < state.freeVertices.size(); ++i) {
            Index v = state.freeVertices[i];
            for (int row = 0; row < 3; ++row) {
                p[3*v+row] = z[3*v+row] + beta * p[3*v+row];
            }
        }
    }

    stepNorm = 0.0;
    for (size_t i = 0; i < state.freeVertices.size(); ++i) {
        Index v = state.freeVertices[i];
        stepNorm = std::max(stepNorm, maxAbs3(&step[3*v]));
    }
    return ok || (stepNorm > 0.0);
}


//
//  The actual solve.
//
LimitSurfaceFitter::Result
LimitSurfaceFitter::Impl::solve(Real const targetPositions[],
                                Real const targetNormals[],
                                Anchor const anchors[], int numAnchors,
                                Options const & options,
                                bool haveTargets, bool haveNormals) const {

    Result result;
    if (!valid) {
        result.status = STATUS_INVALID_INPUT;
        return result;
    }

    result.numTemplateBuilds = numTemplateBuilds;

    //  Validate options:
    Real lambda = options.GetLambda();
    Real gradTol = options.GetGradientTolerance();
    if (!isFinite(lambda) || !(lambda > 0.0)) {
        result.status = STATUS_INVALID_INPUT;
        return result;
    }
    if (!isFinite(gradTol) || !(gradTol > 0.0)) {
        result.status = STATUS_INVALID_INPUT;
        return result;
    }
    if (options.GetMaxIterations() < 0 ||
        options.GetMaxLineSearchSteps() < 0) {
        result.status = STATUS_INVALID_INPUT;
        return result;
    }
    if (numAnchors < 0 || (numAnchors > 0 && !anchors)) {
        result.status = STATUS_INVALID_INPUT;
        return result;
    }
    if (!haveTargets || !targetPositions) {
        result.status = STATUS_INVALID_INPUT;
        return result;
    }
    if (numSamples > 0 && !haveNormals) {
        for (int i = 0; i < numSamples; ++i) {
            if (samples[i].normalWeight > 0.0) {
                result.status = STATUS_INVALID_INPUT;
                return result;
            }
        }
    }

    //  Validate and normalize the target data:
    SolveState state;
    state.lambda = lambda;
    state.targets.resize(3 * numSamples);
    state.normals.assign(3 * numSamples, 0.0);

    for (int i = 0; i < 3 * numSamples; ++i) {
        if (!isFinite(targetPositions[i])) {
            result.status = STATUS_INVALID_INPUT;
            return result;
        }
        state.targets[i] = targetPositions[i];
    }
    for (int i = 0; i < numSamples; ++i) {
        if (samples[i].normalWeight <= 0.0) continue;
        Real n[3] = { targetNormals ? targetNormals[3*i+0] : 0.0,
                      targetNormals ? targetNormals[3*i+1] : 0.0,
                      targetNormals ? targetNormals[3*i+2] : 0.0 };
        if (!isFinite3(n)) {
            result.status = STATUS_INVALID_INPUT;
            return result;
        }
        Real len = norm3(n);
        if (!(len > 0.0)) {
            result.status = STATUS_INVALID_INPUT;
            return result;
        }
        state.normals[3*i+0] = n[0] / len;
        state.normals[3*i+1] = n[1] / len;
        state.normals[3*i+2] = n[2] / len;
    }

    //  Validate anchors and reject duplicates:
    state.anchored.assign(numVertices, 0);
    std::vector<Real> anchorPos(3 * numVertices, 0.0);
    for (int i = 0; i < numAnchors; ++i) {
        Index v = anchors[i].vertexIndex;
        if (v < 0 || v >= numVertices) {
            result.status = STATUS_INVALID_INPUT;
            return result;
        }
        if (state.anchored[v]) {
            result.status = STATUS_INVALID_INPUT;
            return result;
        }
        Real const * pos = anchors[i].position;
        if (anchors[i].positionIsRest) {
            pos = &rest[3*v];
        }
        if (!isFinite3(pos)) {
            result.status = STATUS_INVALID_INPUT;
            return result;
        }
        state.anchored[v] = 1;
        anchorPos[3*v+0] = pos[0];
        anchorPos[3*v+1] = pos[1];
        anchorPos[3*v+2] = pos[2];
    }

    state.freeVertices.clear();
    for (int v = 0; v < numVertices; ++v) {
        if (!state.anchored[v]) state.freeVertices.push_back(v);
    }

    //  Initial state: the construction-time rest with the anchors applied
    //  exactly. Every solve starts from this state -- no solved positions
    //  are ever treated as a new rest.
    int dimension = 3 * numVertices;
    std::vector<Real> x(rest);
    for (int i = 0; i < numAnchors; ++i) {
        Index v = anchors[i].vertexIndex;
        x[3*v+0] = anchorPos[3*v+0];
        x[3*v+1] = anchorPos[3*v+1];
        x[3*v+2] = anchorPos[3*v+2];
    }

    Evaluation eval;
    evaluate(x.data(), state, eval, true, true);

    if (!eval.normalsValid) {
        result.status = STATUS_INVALID_INITIAL;
        result.controlPoints = x;
        return result;
    }

    result.iterations.push_back(IterationRecord());
    result.iterations.back().energy           = eval.energy;
    result.iterations.back().freeGradientNorm = eval.freeGradientNorm;
    result.iterations.back().minSampleArea    = eval.minSampleArea;
    result.iterations.back().stepNorm         = 0.0;

    if (state.freeVertices.empty() || eval.freeGradientNorm <= gradTol) {
        result.status = STATUS_CONVERGED;
        result.controlPoints.swap(x);
        return result;
    }

    int maxIterations = options.GetMaxIterations();
    int maxLineSearch = options.GetMaxLineSearchSteps();
    Real energyAllowance = 1.0e-10;

    for (int iter = 0; iter < maxIterations; ++iter) {

        //  RHS = -free gradient:
        std::vector<Real> rhs(dimension, 0.0);
        for (size_t i = 0; i < state.freeVertices.size(); ++i) {
            Index v = state.freeVertices[i];
            rhs[3*v+0] = -eval.gradient[3*v+0];
            rhs[3*v+1] = -eval.gradient[3*v+1];
            rhs[3*v+2] = -eval.gradient[3*v+2];
        }

        std::vector<Real> step(dimension, 0.0);
        Real stepNorm = 0.0;
        if (!solveNewtonSystem(state, eval, rhs.data(), step.data(),
                               stepNorm)) {
            result.status = STATUS_NO_PROGRESS;
            result.controlPoints = x;
            return result;
        }
        if (!(stepNorm > 0.0) || !isFinite(stepNorm)) {
            result.status = STATUS_NO_PROGRESS;
            result.controlPoints = x;
            return result;
        }

        //  Backtracking line search. Candidate states must satisfy the
        //  normal validity checks and must not increase the energy beyond
        //  the tiny numerical allowance. Rejected trials are discarded.
        Real prevEnergy = eval.energy;
        Real allowIncrease = energyAllowance * std::max(Real(1.0),
                               std::max(std::fabs(prevEnergy), Real(0.0)));

        bool accepted = false;
        Real alpha = 1.0;
        Real candEnergy = 0.0;
        bool candNormalsValid = false;
        std::vector<Real> candidate(dimension, 0.0);

        for (int ls = 0; ls <= maxLineSearch; ++ls) {
            for (size_t i = 0; i < state.freeVertices.size(); ++i) {
                Index v = state.freeVertices[i];
                for (int r = 0; r < 3; ++r) {
                    candidate[3*v+r] = x[3*v+r] + alpha * step[3*v+r];
                }
            }
            for (int i = 0; i < numAnchors; ++i) {
                Index v = anchors[i].vertexIndex;
                candidate[3*v+0] = x[3*v+0];
                candidate[3*v+1] = x[3*v+1];
                candidate[3*v+2] = x[3*v+2];
            }

            evaluateCandidate(candidate.data(), state,
                              candEnergy, candNormalsValid);

            if (candNormalsValid &&
                candEnergy <= prevEnergy + allowIncrease) {
                accepted = true;
                break;
            }
            alpha *= 0.5;
            if (alpha < 1.0e-20) break;
        }

        if (!accepted) {
            result.status = STATUS_NO_PROGRESS;
            result.controlPoints = x;
            return result;
        }

        x.swap(candidate);

        Real acceptedStep = alpha * stepNorm;
        Evaluation next;
        evaluate(x.data(), state, next, true, true);

        result.iterations.push_back(IterationRecord());
        result.iterations.back().energy           = next.energy;
        result.iterations.back().freeGradientNorm = next.freeGradientNorm;
        result.iterations.back().minSampleArea    = next.minSampleArea;
        result.iterations.back().stepNorm         = acceptedStep;

        if (next.freeGradientNorm <= gradTol) {
            result.status = STATUS_CONVERGED;
            result.controlPoints.swap(x);
            return result;
        }

        //  A step that is numerically zero cannot make progress.  It is
        //  only a convergence if the gradient test above passed, so this
        //  is explicitly reported as no progress otherwise.
        Real xNorm = 0.0;
        for (size_t i = 0; i < state.freeVertices.size(); ++i) {
            Index v = state.freeVertices[i];
            xNorm = std::max(xNorm, maxAbs3(&x[3*v]));
        }
        if (!(acceptedStep > 1.0e-13 * std::max(Real(1.0), xNorm))) {
            result.status = STATUS_NO_PROGRESS;
            result.controlPoints.swap(x);
            return result;
        }

        eval = std::move(next);
    }

    result.status = STATUS_ITERATION_LIMIT;
    result.controlPoints.swap(x);
    return result;
}


//
//  Public limit-surface fitter methods:
//
LimitSurfaceFitter::LimitSurfaceFitter(
        Far::TopologyRefiner const & mesh,
        Real const restControlPoints[],
        FitSample const samples[], int numSamples) :
        _impl(new Impl()) {

    _impl->initialize(mesh, restControlPoints, samples, numSamples, true);
}

LimitSurfaceFitter::LimitSurfaceFitter(
        Far::TopologyRefiner const & mesh,
        std::vector<Real> const & restControlPoints,
        std::vector<FitSample> const & samples) :
        _impl(new Impl()) {

    bool haveRest = (restControlPoints.size() ==
                     (size_t)(3 * mesh.GetLevel(0).GetNumVertices()));
    Real const * restPtr = haveRest ? restControlPoints.data() : 0;

    _impl->initialize(mesh, restPtr,
                      samples.empty() ? 0 : samples.data(),
                      (int)samples.size(), haveRest);
}

LimitSurfaceFitter::LimitSurfaceFitter(
        Far::TopologyRefiner const & mesh,
        std::vector<Real> const & restControlPoints,
        FitSample const samples[], int numSamples) :
        _impl(new Impl()) {

    bool haveRest = (restControlPoints.size() ==
                     (size_t)(3 * mesh.GetLevel(0).GetNumVertices()));
    Real const * restPtr = haveRest ? restControlPoints.data() : 0;

    _impl->initialize(mesh, restPtr, samples, numSamples, haveRest);
}

LimitSurfaceFitter::~LimitSurfaceFitter() {
}

bool
LimitSurfaceFitter::IsValid() const {
    return _impl && _impl->valid;
}

LimitSurfaceFitter::Status
LimitSurfaceFitter::GetConstructionStatus() const {
    return _impl ? _impl->constructionStatus : STATUS_INVALID_INPUT;
}

int
LimitSurfaceFitter::GetNumVertices() const {
    return _impl ? _impl->numVertices : 0;
}

int
LimitSurfaceFitter::GetNumSamples() const {
    return _impl ? _impl->numSamples : 0;
}

int
LimitSurfaceFitter::GetNumTemplateBuilds() const {
    return _impl ? _impl->numTemplateBuilds : 0;
}

Far::TopologyRefiner const &
LimitSurfaceFitter::GetMesh() const {
    return *_impl->mesh;
}

LimitSurfaceFitter::Real const *
LimitSurfaceFitter::GetRestControlPoints() const {
    return _impl->rest.empty() ? 0 : _impl->rest.data();
}

LimitSurfaceFitter::Result
LimitSurfaceFitter::Solve(Real const targetPositions[],
                          Real const targetNormals[],
                          Anchor const anchors[], int numAnchors,
                          Options const & options) const {
    return _impl->solve(targetPositions, targetNormals, anchors, numAnchors,
                        options, true, targetNormals != 0);
}

LimitSurfaceFitter::Result
LimitSurfaceFitter::Solve(Real const targetPositions[],
                          Real const targetNormals[],
                          Options const & options) const {
    return _impl->solve(targetPositions, targetNormals, 0, 0, options,
                        true, targetNormals != 0);
}

LimitSurfaceFitter::Result
LimitSurfaceFitter::Solve(std::vector<Real> const & targetPositions,
                          std::vector<Real> const & targetNormals,
                          std::vector<Anchor> const & anchors,
                          Options const & options) const {
    if ((int)targetPositions.size() != 3 * _impl->numSamples) {
        Result result;
        result.status = STATUS_INVALID_INPUT;
        result.numTemplateBuilds = _impl->numTemplateBuilds;
        return result;
    }
    if (!targetNormals.empty() &&
        (int)targetNormals.size() != 3 * _impl->numSamples) {
        Result result;
        result.status = STATUS_INVALID_INPUT;
        result.numTemplateBuilds = _impl->numTemplateBuilds;
        return result;
    }
    return _impl->solve(targetPositions.data(),
                        targetNormals.empty() ? 0 : targetNormals.data(),
                        anchors.empty() ? 0 : anchors.data(),
                        (int)anchors.size(), options,
                        true, !targetNormals.empty());
}

LimitSurfaceFitter::Result
LimitSurfaceFitter::Solve(std::vector<Real> const & targetPositions,
                          std::vector<Real> const & targetNormals,
                          Options const & options) const {
    return Solve(targetPositions, targetNormals,
                 std::vector<Anchor>(), options);
}

} // end namespace Bfr

} // end namespace OPENSUBDIV_VERSION
} // end namespace OpenSubdiv
