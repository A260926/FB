//
//   Copyright 2024 Pixar
//
//   Licensed under the terms set forth in the LICENSE.txt file available at
//   https://opensubdiv.org/license.
//

//  Regression tests for Far::HierarchicalDisplacement.  No main() is
//  defined here: task_support/driver.c drives the registered cases.

#include "task_support/check.h"

#include "opensubdiv/far/hierarchicalDisplacement.h"
#include "opensubdiv/far/topologyDescriptor.h"
#include "opensubdiv/far/topologyRefiner.h"
#include "opensubdiv/far/topologyRefinerFactory.h"
#include "opensubdiv/far/primvarRefiner.h"
#include "opensubdiv/far/stencilTable.h"
#include "opensubdiv/far/stencilTableFactory.h"
#include "opensubdiv/far/patchTableFactory.h"
#include "opensubdiv/sdc/options.h"
#include "opensubdiv/sdc/types.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>

using namespace OpenSubdiv;

namespace {

// --------------------------------------------------------------------------
// Small mesh containers and builders
// --------------------------------------------------------------------------

struct Mesh {
    Sdc::SchemeType scheme = Sdc::SCHEME_CATMARK;
    int numVerts = 0;
    std::vector<int> vertsPerFace;
    std::vector<int> faceVerts;
    std::vector<double> xyz;
};

std::unique_ptr<Far::TopologyRefiner>
makeRefiner(Mesh const & mesh) {
    Far::TopologyDescriptor desc;
    desc.numVertices = mesh.numVerts;
    desc.numFaces = (int)mesh.vertsPerFace.size();
    desc.numVertsPerFace = mesh.vertsPerFace.data();
    desc.vertIndicesPerFace = mesh.faceVerts.data();

    Sdc::Options options;
    options.SetVtxBoundaryInterpolation(
        Sdc::Options::VTX_BOUNDARY_EDGE_ONLY);
    Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Options opts(
        mesh.scheme, options);
    return std::unique_ptr<Far::TopologyRefiner>(
        Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Create(
            desc, opts));
}

// Asymmetric closed Catmark cube cage.
Mesh makeCubeMesh() {
    Mesh m;
    m.scheme = Sdc::SCHEME_CATMARK;
    m.numVerts = 8;
    m.vertsPerFace = {4, 4, 4, 4, 4, 4};
    m.faceVerts = {
        0, 3, 2, 1,
        4, 5, 6, 7,
        0, 1, 5, 4,
        1, 2, 6, 5,
        2, 3, 7, 6,
        3, 0, 4, 7
    };
    m.xyz = {
        -1.0, -1.0, -1.0,
         1.0, -1.0, -1.0,
         1.0,  1.0, -1.0,
        -1.0,  1.0, -1.0,
        -1.0, -1.0,  1.2,
         1.0, -1.0,  1.3,
         1.0,  1.0,  1.5,
        -1.0,  1.0,  1.1
    };
    return m;
}

// Open Catmark L-shaped patch: 3x3 grid with one corner quad removed.
// The interior vertex 4 has three incident quads (valence 3) and the mesh
// has open boundaries.
Mesh makeOpenPatchMesh() {
    Mesh m;
    m.scheme = Sdc::SCHEME_CATMARK;
    m.numVerts = 8;
    m.vertsPerFace = {4, 4, 4};
    m.faceVerts = {
        0, 1, 4, 3,
        1, 2, 5, 4,
        3, 4, 7, 6
    };
    m.xyz = {
        0.0, 0.0, 0.0,
        1.0, 0.0, 0.1,
        2.0, 0.0, 0.0,
        0.0, 1.0, -0.1,
        1.0, 1.0, 0.2,
        2.0, 1.0, 0.15,
        0.0, 2.0, 0.05,
        1.0, 2.0, -0.05
    };
    return m;
}

// Open Loop triangle fan around a valence-5 center vertex (Loop regular
// valence is 6, so the center is extraordinary).
Mesh makeLoopFanMesh() {
    Mesh m;
    m.scheme = Sdc::SCHEME_LOOP;
    m.numVerts = 6;
    m.vertsPerFace = {3, 3, 3, 3, 3};
    m.faceVerts = {
        0, 1, 2,
        0, 2, 3,
        0, 3, 4,
        0, 4, 5,
        0, 5, 1
    };
    m.xyz = {
        0.0, 0.0, 0.1,
        1.0, 0.0, 0.0,
        0.7, 0.8, 0.2,
        -0.2, 1.1, -0.1,
        -0.9, 0.5, 0.15,
        -0.6, -0.7, -0.2
    };
    return m;
}

// Two disconnected open quads: exercises the multiple-component case.
Mesh makeTwoComponentMesh() {
    Mesh m;
    m.scheme = Sdc::SCHEME_CATMARK;
    m.numVerts = 8;
    m.vertsPerFace = {4, 4};
    m.faceVerts = {
        0, 1, 2, 3,
        4, 5, 6, 7
    };
    m.xyz = {
        0.0, 0.0, 0.0,
        1.0, 0.0, 0.1,
        1.0, 1.0, 0.0,
        0.0, 1.0, -0.1,
        3.0, 0.0, 0.2,
        4.0, 0.0, 0.1,
        4.0, 1.0, 0.3,
        3.0, 1.0, 0.0
    };
    return m;
}


// --------------------------------------------------------------------------
// Vector helpers and the PrimvarRefiner element type
// --------------------------------------------------------------------------

struct V {
    double x[3] = {0.0, 0.0, 0.0};
    void Clear() { x[0] = x[1] = x[2] = 0.0; }
    void AddWithWeight(V const & v, double w) {
        x[0] += w * v.x[0];
        x[1] += w * v.x[1];
        x[2] += w * v.x[2];
    }
};

void cross3(double const * a, double const * b, double * r) {
    r[0] = a[1] * b[2] - a[2] * b[1];
    r[1] = a[2] * b[0] - a[0] * b[2];
    r[2] = a[0] * b[1] - a[1] * b[0];
}
void sub3(double const * a, double const * b, double * r) {
    r[0] = a[0] - b[0];
    r[1] = a[1] - b[1];
    r[2] = a[2] - b[2];
}
double dot3(double const * a, double const * b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
double norm3(double const * a) {
    return std::sqrt(dot3(a, a));
}
double relErr(double a, double b) {
    double denom = std::max(std::fabs(a), std::fabs(b));
    if (denom < 1e-12) {
        return std::fabs(a - b);
    }
    return std::fabs(a - b) / denom;
}

std::vector<Far::HierarchicalDisplacementSample>
makeSamples(Mesh const & mesh, int count) {
    std::vector<Far::HierarchicalDisplacementSample> samples;
    int ptexPerFace = (mesh.scheme == Sdc::SCHEME_LOOP) ? 3 : 1;
    int numPtex = (int)mesh.vertsPerFace.size() * ptexPerFace;
    for (int i = 0; i < count; ++i) {
        Far::HierarchicalDisplacementSample s;
        s.face = (2 * i + 1) % numPtex;
        s.s = 0.23 + 0.071 * i;
        s.t = 0.31 + 0.067 * i;
        if (s.s >= 1.0) { s.s -= 0.5; }
        if (s.t >= 1.0) { s.t -= 0.5; }
        samples.push_back(s);
    }
    return samples;
}

std::vector<double>
makeDisplacements(int count, int salt) {
    std::vector<double> d(count);
    for (int i = 0; i < count; ++i) {
        double v = std::sin(0.37 * (i + 1) + 0.6 * salt) * 0.3
                 + std::cos(0.21 * (i + 2)) * 0.12;
        if ((i + salt) % 11 == 0) {
            v = 0.0;
        }
        if ((i + 2 * salt) % 13 == 0) {
            v = -v;
        }
        d[i] = v;
    }
    return d;
}

// Independent area-normal computation written directly against the Far
// level topology (not sharing any code with the production implementation).
std::vector<V>
independentAreaNormals(Far::TopologyLevel const & level,
                       std::vector<V> const & points) {
    int n = level.GetNumVertices();
    std::vector<V> normals(n);
    for (int v = 0; v < n; ++v) {
        Far::ConstIndexArray faces = level.GetVertexFaces(v);
        Far::ConstLocalIndexArray local =
            level.GetVertexFaceLocalIndices(v);
        double area[3] = {0.0, 0.0, 0.0};
        for (int i = 0; i < faces.size(); ++i) {
            Far::ConstIndexArray fv =
                level.GetFaceVertices(faces[i]);
            int k = fv.size();
            int li = local[i];
            int prev = fv[(li - 1 + k) % k];
            int next = fv[(li + 1) % k];
            double dx[3], dy[3], cr[3];
            sub3(points[next].x, points[v].x, dx);
            sub3(points[prev].x, points[v].x, dy);
            cross3(dx, dy, cr);
            area[0] += cr[0];
            area[1] += cr[1];
            area[2] += cr[2];
        }
        double length = norm3(area);
        normals[v].x[0] = area[0] / length;
        normals[v].x[1] = area[1] / length;
        normals[v].x[2] = area[2] / length;
    }
    return normals;
}

// Full independent forward: PrimvarRefiner subdivision, independently
// coded area normals, scalar displacement, then Far limit evaluation on
// the final layer as a fresh control cage.
struct ReferenceForward {
    std::vector<std::vector<V>> subdivPoints;
    std::vector<std::vector<V>> layerPoints;
    std::vector<std::vector<V>> normals;
    std::vector<V> samplePositions;
    std::vector<V> samplePu;
    std::vector<V> samplePv;
    std::vector<V> sampleNormals;
};

// Builds a TopologyDescriptor for every face of a Far level.
struct LevelDesc {
    std::vector<int> vpf;
    std::vector<int> fv;
    Far::TopologyDescriptor desc;
    explicit LevelDesc(Far::TopologyLevel const & level) {
        desc.numVertices = level.GetNumVertices();
        desc.numFaces = level.GetNumFaces();
        vpf.reserve(desc.numFaces);
        fv.reserve(level.GetNumFaceVertices());
        for (int f = 0; f < desc.numFaces; ++f) {
            Far::ConstIndexArray face = level.GetFaceVertices(f);
            vpf.push_back(face.size());
            for (int k = 0; k < face.size(); ++k) {
                fv.push_back(face[k]);
            }
        }
        desc.numVertsPerFace = vpf.data();
        desc.vertIndicesPerFace = fv.data();
    }
};

ReferenceForward
runReference(Mesh const & mesh, int numLayers,
             std::vector<Far::HierarchicalDisplacementSample> const & samples,
             std::vector<std::vector<double> > const & displacements) {
    ReferenceForward ref;

    // Match the production numbering by chaining one-level refiners; the
    // geometry itself still comes from PrimvarRefiner plus an independently
    // coded area-normal formula.
    std::vector<V> previous(mesh.numVerts);
    for (int v = 0; v < mesh.numVerts; ++v) {
        for (int k = 0; k < 3; ++k) {
            previous[v].x[k] = mesh.xyz[3 * v + k];
        }
    }

    ref.subdivPoints.resize(numLayers);
    ref.layerPoints.resize(numLayers);
    ref.normals.resize(numLayers);

    std::unique_ptr<Far::TopologyRefiner> stepRefiner = makeRefiner(mesh);
    for (int l = 1; l <= numLayers; ++l) {
        Far::TopologyRefiner::UniformOptions uo(1);
        uo.fullTopologyInLastLevel = true;
        stepRefiner->RefineUniform(uo);

        Far::TopologyLevel const & level = stepRefiner->GetLevel(1);
        int nv = level.GetNumVertices();
        Far::PrimvarRefinerReal<double> primvar(*stepRefiner);
        std::vector<V> subdiv(nv);
        primvar.Interpolate(1, previous, subdiv);

        std::vector<V> normals = independentAreaNormals(level, subdiv);
        std::vector<V> sculpted(nv);
        for (int v = 0; v < nv; ++v) {
            double d = displacements[l - 1][v];
            for (int k = 0; k < 3; ++k) {
                sculpted[v].x[k] =
                    subdiv[v].x[k] + d * normals[v].x[k];
            }
        }
        ref.subdivPoints[l - 1] = subdiv;
        ref.normals[l - 1] = normals;
        ref.layerPoints[l - 1] = sculpted;
        previous = std::move(sculpted);

        // Next step refines this child level as a fresh base cage.
        if (l < numLayers) {
            LevelDesc childDesc(level);
            Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Options
                nextOpts(mesh.scheme,
                         stepRefiner->GetSchemeOptions());
            stepRefiner.reset(
                Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Create(
                    childDesc.desc, nextOpts));
        }
    }

    // Build a fresh refiner on the final sculpted layer cage and evaluate
    // the limit samples through the regular Far machinery.
    Far::TopologyLevel const & finalLevel = stepRefiner->GetLevel(1);
    LevelDesc finalDesc(finalLevel);
    Far::TopologyDescriptor const & desc = finalDesc.desc;

    Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Options opts(
        mesh.scheme, stepRefiner->GetSchemeOptions());
    std::unique_ptr<Far::TopologyRefiner> limitRefiner(
        Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Create(
            desc, opts));

    Far::PatchTableFactory::Options po(5);
    po.SetPatchPrecision<double>();
    po.SetEndCapType(
        Far::PatchTableFactory::Options::ENDCAP_GREGORY_BASIS);
    limitRefiner->RefineAdaptive(po.GetRefineAdaptiveOptions());
    std::unique_ptr<Far::PatchTable> patches(
        Far::PatchTableFactory::Create(*limitRefiner, po));

    // Reorder limit outputs back to the requested sample order by
    // evaluating one single-sample limit table per sample.
    ref.samplePositions.resize(samples.size());
    ref.samplePu.resize(samples.size());
    ref.samplePv.resize(samples.size());
    ref.sampleNormals.resize(samples.size());
    for (std::size_t si = 0; si < samples.size(); ++si) {
        Far::LimitStencilTableFactoryReal<double>::LocationArray one;
        one.ptexIdx = samples[si].face;
        one.numLocations = 1;
        double s = samples[si].s;
        double t = samples[si].t;
        one.s = &s;
        one.t = &t;
        std::vector<Far::LimitStencilTableFactoryReal<double>::LocationArray>
            oneVec(1, one);
        std::unique_ptr<Far::LimitStencilTableReal<double> const> single(
            Far::LimitStencilTableFactoryReal<double>::Create(
                *limitRefiner, oneVec, nullptr, patches.get()));
        auto row = single->GetLimitStencil(0);
        V p, pu, pv;
        for (int j = 0; j < row.GetSize(); ++j) {
            int idx = row.GetVertexIndices()[j];
            V const & q = previous[idx];
            p.AddWithWeight(q, row.GetWeights()[j]);
            pu.AddWithWeight(q, row.GetDuWeights()[j]);
            pv.AddWithWeight(q, row.GetDvWeights()[j]);
        }
        double cr[3];
        cross3(pu.x, pv.x, cr);
        double length = norm3(cr);
        ref.samplePositions[si] = p;
        ref.samplePu[si] = pu;
        ref.samplePv[si] = pv;
        for (int k = 0; k < 3; ++k) {
            ref.sampleNormals[si].x[k] = cr[k] / length;
        }
    }
    return ref;
}

// --------------------------------------------------------------------------
// Objective and analytic gradient helpers
// --------------------------------------------------------------------------

struct ObjectiveAdjoints {
    std::vector<double> bP;
    std::vector<double> bN;
};

ObjectiveAdjoints makeAdjoints(int numSamples) {
    ObjectiveAdjoints a;
    a.bP.resize(3 * numSamples);
    a.bN.resize(3 * numSamples);
    for (int i = 0; i < numSamples; ++i) {
        a.bP[3 * i]     = 0.55 + 0.07 * i;
        a.bP[3 * i + 1] = -0.37;
        a.bP[3 * i + 2] = 0.21 - 0.03 * i;
        a.bN[3 * i]     = 0.31;
        a.bN[3 * i + 1] = 0.47 - 0.11 * i;
        a.bN[3 * i + 2] = -0.63;
    }
    return a;
}

double objectiveValue(Far::HierarchicalDisplacementResult const & r,
                      ObjectiveAdjoints const & a) {
    double f = 0.0;
    for (std::size_t i = 0; i < a.bP.size(); ++i) {
        f += a.bP[i] * r.samplePositions[i]
           + a.bN[i] * r.sampleNormals[i];
    }
    return f;
}

struct AnalyticGradient {
    std::vector<double> coarseGrad;
    std::vector<std::vector<double> > dispGrad;
};

AnalyticGradient
analyticPullback(Far::HierarchicalDisplacement const & plan,
                 std::vector<double> const & coarseXYZ,
                 std::vector<std::vector<double> > const & disps,
                 ObjectiveAdjoints const & a) {
    Far::HierarchicalDisplacementResult result;
    std::shared_ptr<Far::HierarchicalDisplacement::Tape> tape =
        plan.Forward(coarseXYZ, disps, result);
    AnalyticGradient g;
    g.coarseGrad.assign(3 * plan.GetNumCoarseVertices(), 0.0);
    g.dispGrad.resize(plan.GetNumLayers());
    std::vector<std::vector<double> *> pointers(plan.GetNumLayers());
    for (int l = 0; l < plan.GetNumLayers(); ++l) {
        g.dispGrad[l].assign(plan.GetNumLayerVertices(l + 1), 0.0);
        pointers[l] = &g.dispGrad[l];
    }
    tape->Pullback(a.bP, a.bN, g.coarseGrad, pointers);
    return g;
}

// Run a complete forward with the vector-based parameter containers.
double forwardObjective(Far::HierarchicalDisplacement const & plan,
                        std::vector<double> const & coarseXYZ,
                        std::vector<std::vector<double> > const & disps,
                        ObjectiveAdjoints const & a) {
    Far::HierarchicalDisplacementResult result;
    plan.Forward(coarseXYZ, disps, result);
    return objectiveValue(result, a);
}

// Central-difference gradient of every parameter (coarse XYZ and every
// layer scalar) on the small test grids.
struct FiniteDifferenceGradient {
    std::vector<double> coarseGrad;
    std::vector<std::vector<double> > dispGrad;
};

FiniteDifferenceGradient
finiteDifferenceGradient(Far::HierarchicalDisplacement const & plan,
                         std::vector<double> coarseXYZ,
                         std::vector<std::vector<double> > disps,
                         ObjectiveAdjoints const & a,
                         double h) {
    FiniteDifferenceGradient g;
    g.coarseGrad.assign(coarseXYZ.size(), 0.0);
    for (std::size_t i = 0; i < coarseXYZ.size(); ++i) {
        double saved = coarseXYZ[i];
        coarseXYZ[i] = saved + h;
        double fp = forwardObjective(plan, coarseXYZ, disps, a);
        coarseXYZ[i] = saved - h;
        double fm = forwardObjective(plan, coarseXYZ, disps, a);
        coarseXYZ[i] = saved;
        g.coarseGrad[i] = (fp - fm) / (2.0 * h);
    }
    g.dispGrad.resize(disps.size());
    for (std::size_t l = 0; l < disps.size(); ++l) {
        g.dispGrad[l].assign(disps[l].size(), 0.0);
        for (std::size_t i = 0; i < disps[l].size(); ++i) {
            double saved = disps[l][i];
            disps[l][i] = saved + h;
            double fp = forwardObjective(plan, coarseXYZ, disps, a);
            disps[l][i] = saved - h;
            double fm = forwardObjective(plan, coarseXYZ, disps, a);
            disps[l][i] = saved;
            g.dispGrad[l][i] = (fp - fm) / (2.0 * h);
        }
    }
    return g;
}

// ==========================================================================
// forward group
// ==========================================================================

int checkForwardAgainstReference(Mesh const & mesh, int numLayers,
                                 int numSamples, bool zeroDisplacement) {
    auto refiner = makeRefiner(mesh);
    auto samples = makeSamples(mesh, numSamples);
    Far::HierarchicalDisplacement plan(*refiner, numLayers, samples);

    std::vector<std::vector<double> > disps(numLayers);
    for (int l = 0; l < numLayers; ++l) {
        disps[l] = makeDisplacements(
            plan.GetNumLayerVertices(l + 1), l + 3);
        if (zeroDisplacement) {
            std::fill(disps[l].begin(), disps[l].end(), 0.0);
        }
    }

    Far::HierarchicalDisplacementResult result;
    auto tape = plan.Forward(mesh.xyz, disps, result);

    TASK_CHECK((int)result.finalLayerVertices.size()
               == 3 * plan.GetNumLayerVertices(numLayers));
    TASK_CHECK((int)result.samplePositions.size() == 3 * numSamples);
    TASK_CHECK((int)result.sampleNormals.size() == 3 * numSamples);
    TASK_CHECK((int)plan.GetFinalFaceVertexIndices().size()
               == plan.GetFinalNumFaces() * plan.GetFinalFaceSize());

    ReferenceForward ref =
        runReference(mesh, numLayers, samples, disps);

    for (int l = 0; l < numLayers; ++l) {
        int nv = plan.GetNumLayerVertices(l + 1);
        TASK_CHECK((int)tape->GetSubdivPoints(l + 1).size() == 3 * nv);
        TASK_CHECK((int)tape->GetLayerPoints(l + 1).size() == 3 * nv);
        for (int v = 0; v < nv; ++v) {
            for (int k = 0; k < 3; ++k) {
                TASK_CHECK(relErr(tape->GetSubdivPoints(l + 1)[3 * v + k],
                                  ref.subdivPoints[l][v].x[k]) < 1e-11);
                TASK_CHECK(relErr(tape->GetLayerPoints(l + 1)[3 * v + k],
                                  ref.layerPoints[l][v].x[k]) < 1e-11);
                TASK_CHECK(relErr(tape->GetUnitNormals(l + 1)[3 * v + k],
                                  ref.normals[l][v].x[k]) < 1e-11);
            }
        }
    }

    for (int i = 0; i < numSamples; ++i) {
        for (int k = 0; k < 3; ++k) {
            TASK_CHECK(relErr(result.samplePositions[3 * i + k],
                              ref.samplePositions[i].x[k]) < 1e-10);
            TASK_CHECK(relErr(result.sampleNormals[3 * i + k],
                              ref.sampleNormals[i].x[k]) < 1e-10);
        }
        double normalLength = norm3(&result.sampleNormals[3 * i]);
        TASK_CHECK(std::fabs(normalLength - 1.0) < 1e-12);
    }
    return 0;
}

int forwardClosedCube() {
    return checkForwardAgainstReference(makeCubeMesh(), 2, 4, false);
}

int forwardOpenPatch() {
    return checkForwardAgainstReference(makeOpenPatchMesh(), 2, 3, false);
}

int forwardLoopFan() {
    return checkForwardAgainstReference(makeLoopFanMesh(), 2, 4, false);
}

int forwardThreeLayers() {
    // Exercises the maximum supported layer count end to end.
    return checkForwardAgainstReference(makeCubeMesh(), 3, 3, false);
}

int forwardTwoComponents() {
    // Disconnected components must be handled independently.
    int rc = checkForwardAgainstReference(
        makeTwoComponentMesh(), 2, 2, false);
    if (rc) {
        return rc;
    }
    Mesh mesh = makeTwoComponentMesh();
    auto refiner = makeRefiner(mesh);
    // One sample strictly inside the first component's ptex face.
    std::vector<Far::HierarchicalDisplacementSample> oneSample = {
        {0, 0.35, 0.55}
    };
    Far::HierarchicalDisplacement plan(*refiner, 1, oneSample);

    // Adjoints on the first component's sample must not leak into the
    // coarse vertices of the second component.
    std::vector<std::vector<double> > disps(1);
    disps[0] = makeDisplacements(plan.GetNumLayerVertices(1), 1);
    ObjectiveAdjoints a;
    a.bP.assign(3, 0.0);
    a.bN.assign(3, 0.0);
    a.bP[0] = 1.0; // x position only
    AnalyticGradient g =
        analyticPullback(plan, mesh.xyz, disps, a);
    bool secondComponentTouched = false;
    for (int v = 4; v < 8; ++v) {
        for (int k = 0; k < 3; ++k) {
            if (g.coarseGrad[3 * v + k] != 0.0) {
                secondComponentTouched = true;
            }
        }
    }
    TASK_CHECK(!secondComponentTouched);
    bool firstComponentTouched = false;
    for (int v = 0; v < 4; ++v) {
        for (int k = 0; k < 3; ++k) {
            if (g.coarseGrad[3 * v + k] != 0.0) {
                firstComponentTouched = true;
            }
        }
    }
    TASK_CHECK(firstComponentTouched);
    return 0;
}

int forwardZeroDisplacement() {
    int rc = checkForwardAgainstReference(makeCubeMesh(), 1, 3, true);
    if (rc) {
        return rc;
    }
    // Zero displacement must leave the final cage equal to plain
    // subdivision of the coarse points, layer after layer.
    Mesh mesh = makeOpenPatchMesh();
    auto refiner = makeRefiner(mesh);
    auto samples = makeSamples(mesh, 2);
    Far::HierarchicalDisplacement plan(*refiner, 2, samples);
    std::vector<std::vector<double> > disps(2);
    for (int l = 0; l < 2; ++l) {
        disps[l].assign(plan.GetNumLayerVertices(l + 1), 0.0);
    }
    Far::HierarchicalDisplacementResult result;
    auto tape = plan.Forward(mesh.xyz, disps, result);
    for (int l = 1; l <= 2; ++l) {
        auto const & q = tape->GetSubdivPoints(l);
        auto const & p = tape->GetLayerPoints(l);
        TASK_CHECK(q.size() == p.size());
        for (std::size_t i = 0; i < q.size(); ++i) {
            TASK_CHECK(std::fabs(q[i] - p[i]) < 1e-14);
        }
    }
    // Gradients remain finite and well defined at zero displacement.
    ObjectiveAdjoints a = makeAdjoints(2);
    AnalyticGradient g = analyticPullback(plan, mesh.xyz, disps, a);
    for (double v : g.coarseGrad) {
        TASK_CHECK(std::isfinite(v));
    }
    return 0;
}

int forwardArgumentValidation() {
    Mesh mesh = makeCubeMesh();
    auto refiner = makeRefiner(mesh);
    auto samples = makeSamples(mesh, 2);

    bool threw = false;
    try {
        Far::HierarchicalDisplacement bad(*refiner, 4, samples);
    } catch (std::invalid_argument const &) {
        threw = true;
    }
    TASK_CHECK(threw);

    threw = false;
    try {
        std::vector<Far::HierarchicalDisplacementSample> empty;
        Far::HierarchicalDisplacement bad(*refiner, 1, empty);
    } catch (std::invalid_argument const &) {
        threw = true;
    }
    TASK_CHECK(threw);

    threw = false;
    try {
        auto badSamples = samples;
        badSamples[0].s = 0.0; // on the ptex boundary
        Far::HierarchicalDisplacement bad(*refiner, 1, badSamples);
    } catch (std::invalid_argument const &) {
        threw = true;
    }
    TASK_CHECK(threw);

    Far::HierarchicalDisplacement plan(*refiner, 2, samples);
    std::vector<double> d1(plan.GetNumLayerVertices(1), 0.1);
    std::vector<double> d2(plan.GetNumLayerVertices(2), 0.1);
    std::vector<std::vector<double> > ok = {d1, d2};

    threw = false;
    try {
        std::vector<double> shortD(1, 0.1);
        std::vector<std::vector<double> > bad = {shortD, d2};
        Far::HierarchicalDisplacementResult result;
        plan.Forward(mesh.xyz, bad, result);
    } catch (std::invalid_argument const &) {
        threw = true;
    }
    TASK_CHECK(threw);

    threw = false;
    try {
        std::vector<double> nanXYZ = mesh.xyz;
        nanXYZ[0] = std::numeric_limits<double>::quiet_NaN();
        Far::HierarchicalDisplacementResult result;
        plan.Forward(nanXYZ, ok, result);
    } catch (std::invalid_argument const &) {
        threw = true;
    }
    TASK_CHECK(threw);

    threw = false;
    try {
        std::vector<double> nanD = d1;
        nanD[0] = std::numeric_limits<double>::quiet_NaN();
        std::vector<std::vector<double> > bad = {nanD, d2};
        Far::HierarchicalDisplacementResult result;
        plan.Forward(mesh.xyz, bad, result);
    } catch (std::invalid_argument const &) {
        threw = true;
    }
    TASK_CHECK(threw);

    threw = false;
    try {
        std::vector<std::vector<double> > bad = {d1};
        Far::HierarchicalDisplacementResult result;
        plan.Forward(mesh.xyz, bad, result);
    } catch (std::invalid_argument const &) {
        threw = true;
    }
    TASK_CHECK(threw);

    threw = false;
    try {
        std::vector<double> shortXYZ(3, 0.0);
        Far::HierarchicalDisplacementResult result;
        plan.Forward(shortXYZ, ok, result);
    } catch (std::invalid_argument const &) {
        threw = true;
    }
    TASK_CHECK(threw);
    return 0;
}

// ==========================================================================
// adjoint group
// ==========================================================================

int checkFiniteDifferenceConvergence(Mesh const & mesh, int numLayers) {
    auto refiner = makeRefiner(mesh);
    auto samples = makeSamples(mesh, 3);
    Far::HierarchicalDisplacement plan(*refiner, numLayers, samples);

    std::vector<std::vector<double> > disps(numLayers);
    for (int l = 0; l < numLayers; ++l) {
        disps[l] = makeDisplacements(
            plan.GetNumLayerVertices(l + 1), l + 5);
    }
    ObjectiveAdjoints a = makeAdjoints(3);
    AnalyticGradient analytic =
        analyticPullback(plan, mesh.xyz, disps, a);

    // Three step sizes: the analytic/FD relative error must stay under the
    // required tolerance and must not grow as the step is refined (the
    // second-order scheme converges until it reaches the round-off floor).
    FiniteDifferenceGradient fd1 = finiteDifferenceGradient(
        plan, mesh.xyz, disps, a, 1e-4);
    FiniteDifferenceGradient fd2 = finiteDifferenceGradient(
        plan, mesh.xyz, disps, a, 5e-5);
    FiniteDifferenceGradient fd3 = finiteDifferenceGradient(
        plan, mesh.xyz, disps, a, 2.5e-5);

    auto checkPair = [](double an, double f1, double f2, double f3) {
        double e1 = relErr(an, f1);
        double e2 = relErr(an, f2);
        double e3 = relErr(an, f3);
        if (std::fabs(an) < 1e-11 && std::fabs(f3) < 1e-11) {
            return std::fabs(an - f3) < 1e-9;
        }
        // Hard accuracy requirement from the task specification.
        if (!(e2 < 2e-5 && e1 < 1.0)) {
            return false;
        }
        // Step-convergence is only meaningful above the double-precision
        // round-off floor; below ~1e-7 relative error the central
        // differences are dominated by cancellation noise.
        if (e2 > 1e-7) {
            return e3 <= e2 * 1.5 + 1e-12;
        }
        return true;
    };

    for (std::size_t i = 0; i < analytic.coarseGrad.size(); ++i) {
        TASK_CHECK(checkPair(analytic.coarseGrad[i],
                             fd1.coarseGrad[i],
                             fd2.coarseGrad[i],
                             fd3.coarseGrad[i]));
    }
    for (int l = 0; l < numLayers; ++l) {
        TASK_CHECK(analytic.dispGrad[l].size()
                   == fd1.dispGrad[l].size());
        for (std::size_t i = 0; i < analytic.dispGrad[l].size(); ++i) {
            TASK_CHECK(checkPair(analytic.dispGrad[l][i],
                                 fd1.dispGrad[l][i],
                                 fd2.dispGrad[l][i],
                                 fd3.dispGrad[l][i]));
        }
    }
    return 0;
}

int adjointFiniteDifferencesQuad() {
    // Small open quad grid: exhaustive parameter coverage.
    return checkFiniteDifferenceConvergence(makeOpenPatchMesh(), 2);
}

int adjointFiniteDifferencesLoop() {
    // One Loop layer on the small irregular fan.
    return checkFiniteDifferenceConvergence(makeLoopFanMesh(), 1);
}

int adjointTransposeIdentity() {
    Mesh mesh = makeOpenPatchMesh();
    auto refiner = makeRefiner(mesh);
    auto samples = makeSamples(mesh, 3);
    Far::HierarchicalDisplacement plan(*refiner, 2, samples);

    std::vector<std::vector<double> > disps(2);
    for (int l = 0; l < 2; ++l) {
        disps[l] = makeDisplacements(
            plan.GetNumLayerVertices(l + 1), l + 2);
    }
    ObjectiveAdjoints a = makeAdjoints(3);
    AnalyticGradient analytic =
        analyticPullback(plan, mesh.xyz, disps, a);

    // Random tangent directions.
    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> uni(-0.7, 0.7);
    std::vector<double> dx(mesh.xyz.size());
    for (double & v : dx) { v = uni(rng); }
    std::vector<std::vector<double> > dd(2);
    for (int l = 0; l < 2; ++l) {
        dd[l].resize(disps[l].size());
        for (double & v : dd[l]) { v = uni(rng); }
    }

    double const h = 1e-4;
    auto perturbedObjective = [&](double sign) {
        std::vector<double> xyz = mesh.xyz;
        for (std::size_t i = 0; i < xyz.size(); ++i) {
            xyz[i] += sign * h * dx[i];
        }
        std::vector<std::vector<double> > dp = disps;
        for (int l = 0; l < 2; ++l) {
            for (std::size_t i = 0; i < dp[l].size(); ++i) {
                dp[l][i] += sign * h * dd[l][i];
            }
        }
        return forwardObjective(plan, xyz, dp, a);
    };
    double directionalOutput =
        (perturbedObjective(1.0) - perturbedObjective(-1.0)) / (2.0 * h);

    double directionalGradient = 0.0;
    for (std::size_t i = 0; i < dx.size(); ++i) {
        directionalGradient += analytic.coarseGrad[i] * dx[i];
    }
    for (int l = 0; l < 2; ++l) {
        for (std::size_t i = 0; i < dd[l].size(); ++i) {
            directionalGradient += analytic.dispGrad[l][i] * dd[l][i];
        }
    }
    TASK_CHECK(relErr(directionalOutput, directionalGradient) < 1e-7);
    return 0;
}

int adjointAccumulationAndStencilTranspose() {
    // Pullback accumulates onto prefilled destination buffers.
    Mesh mesh = makeCubeMesh();
    auto refiner = makeRefiner(mesh);
    auto samples = makeSamples(mesh, 2);
    Far::HierarchicalDisplacement plan(*refiner, 2, samples);

    std::vector<std::vector<double> > disps(2);
    for (int l = 0; l < 2; ++l) {
        disps[l] = makeDisplacements(
            plan.GetNumLayerVertices(l + 1), l + 4);
    }
    ObjectiveAdjoints a = makeAdjoints(2);
    AnalyticGradient zero =
        analyticPullback(plan, mesh.xyz, disps, a);

    AnalyticGradient prefilled;
    prefilled.coarseGrad.assign(3 * plan.GetNumCoarseVertices(), 0.31);
    prefilled.dispGrad.resize(2);
    std::vector<std::vector<double> *> pp(2);
    for (int l = 0; l < 2; ++l) {
        prefilled.dispGrad[l].assign(
            plan.GetNumLayerVertices(l + 1), 0.17 + l);
        pp[l] = &prefilled.dispGrad[l];
    }
    Far::HierarchicalDisplacementResult result;
    auto tape = plan.Forward(mesh.xyz, disps, result);
    tape->Pullback(a.bP, a.bN, prefilled.coarseGrad, pp);
    for (std::size_t i = 0; i < zero.coarseGrad.size(); ++i) {
        TASK_CHECK(relErr(prefilled.coarseGrad[i],
                          zero.coarseGrad[i] + 0.31) < 1e-12);
    }
    for (int l = 0; l < 2; ++l) {
        double base = 0.17 + l;
        for (std::size_t i = 0; i < zero.dispGrad[l].size(); ++i) {
            TASK_CHECK(relErr(prefilled.dispGrad[l][i],
                              zero.dispGrad[l][i] + base) < 1e-12);
        }
    }

    // Direct exercise of StencilTableReal::SparseTransposeAccumulate and
    // its alternate-weight entry point.
    auto stencilRefiner = makeRefiner(mesh);
    stencilRefiner->RefineUniform(
        Far::TopologyRefiner::UniformOptions(1));
    Far::StencilTableFactoryReal<double>::Options so;
    so.generateIntermediateLevels = false;
    so.generateControlVerts = false;
    so.generateOffsets = true;
    std::unique_ptr<Far::StencilTableReal<double> const> table(
        Far::StencilTableFactoryReal<double>::Create(
            *stencilRefiner, so));
    int rows = table->GetNumStencils();
    int controls = table->GetNumControlVertices();
    std::vector<double> src(rows);
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> uni(-0.5, 0.5);
    for (double & v : src) { v = uni(rng); }

    std::vector<double> expected(controls, 1.0);
    std::vector<double> expectedDu(controls, 2.0);
    for (int i = 0; i < rows; ++i) {
        auto row = table->GetStencil(i);
        for (int j = 0; j < row.GetSize(); ++j) {
            int idx = row.GetVertexIndices()[j];
            expected[idx] += row.GetWeights()[j] * src[i];
        }
    }
    std::vector<double> dst(controls, 1.0);
    table->SparseTransposeAccumulate(src.data(), dst.data());
    for (int i = 0; i < controls; ++i) {
        TASK_CHECK(relErr(dst[i], expected[i]) < 1e-14);
    }
    // A second accumulation must add the same increment again.
    table->SparseTransposeAccumulate(src.data(), dst.data());
    for (int i = 0; i < controls; ++i) {
        TASK_CHECK(relErr(dst[i], 2.0 * expected[i] - 1.0) < 1e-14);
    }

    // Limit stencil table derivative weights through the same entry point.
    Far::PatchTableFactory::Options po(5);
    po.SetPatchPrecision<double>();
    po.SetEndCapType(
        Far::PatchTableFactory::Options::ENDCAP_GREGORY_BASIS);
    auto limitRefiner = makeRefiner(mesh);
    limitRefiner->RefineAdaptive(po.GetRefineAdaptiveOptions());
    std::unique_ptr<Far::PatchTable> patches(
        Far::PatchTableFactory::Create(*limitRefiner, po));
    Far::LimitStencilTableFactoryReal<double>::LocationArray loc;
    loc.ptexIdx = 0;
    loc.numLocations = 1;
    double us = 0.33, vt = 0.41;
    loc.s = &us;
    loc.t = &vt;
    std::vector<Far::LimitStencilTableFactoryReal<double>::LocationArray>
        locVec(1, loc);
    std::unique_ptr<Far::LimitStencilTableReal<double> const> limit(
        Far::LimitStencilTableFactoryReal<double>::Create(
            *limitRefiner, locVec, nullptr, patches.get()));
    std::vector<double> lsrc(limit->GetNumStencils(), 1.25);
    std::vector<double> ldu(limit->GetNumControlVertices(), 2.0);
    std::vector<double> manual(limit->GetNumControlVertices(), 2.0);
    auto lrow = limit->GetLimitStencil(0);
    for (int j = 0; j < lrow.GetSize(); ++j) {
        manual[lrow.GetVertexIndices()[j]] +=
            lrow.GetDuWeights()[j] * 1.25;
    }
    limit->SparseTransposeAccumulate(
        lsrc.data(), ldu.data(), limit->GetDuWeights().data());
    for (int i = 0; i < limit->GetNumControlVertices(); ++i) {
        TASK_CHECK(relErr(ldu[i], manual[i]) < 1e-14);
    }
    return 0;
}

int adjointDuplicateSamples() {
    Mesh mesh = makeOpenPatchMesh();
    auto refiner = makeRefiner(mesh);
    auto baseSamples = makeSamples(mesh, 2);

    std::vector<Far::HierarchicalDisplacementSample> dup =
        baseSamples;
    dup.push_back(baseSamples[0]); // s0 appears twice
    Far::HierarchicalDisplacement dupPlan(*refiner, 2, dup);

    std::vector<std::vector<double> > disps(2);
    for (int l = 0; l < 2; ++l) {
        disps[l] = makeDisplacements(
            dupPlan.GetNumLayerVertices(l + 1), l + 6);
    }

    Far::HierarchicalDisplacementResult dupResult;
    auto dupTape = dupPlan.Forward(mesh.xyz, disps, dupResult);
    TASK_CHECK(std::fabs(
        dupResult.samplePositions[0]
        - dupResult.samplePositions[6]) < 1e-16);
    TASK_CHECK(std::fabs(
        dupResult.sampleNormals[1]
        - dupResult.sampleNormals[7]) < 1e-16);

    // Adjoints on the duplicated sample must sum against one sample.
    ObjectiveAdjoints dupA = makeAdjoints(3);
    AnalyticGradient dupG =
        analyticPullback(dupPlan, mesh.xyz, disps, dupA);

    ObjectiveAdjoints singleA0 = makeAdjoints(1);
    for (int k = 0; k < 3; ++k) {
        singleA0.bP[k] = dupA.bP[k] + dupA.bP[6 + k];
        singleA0.bN[k] = dupA.bN[k] + dupA.bN[6 + k];
    }
    std::vector<Far::HierarchicalDisplacementSample> one0 =
        {baseSamples[0]};
    Far::HierarchicalDisplacement plan0(*refiner, 2, one0);
    AnalyticGradient g0 =
        analyticPullback(plan0, mesh.xyz, disps, singleA0);

    ObjectiveAdjoints singleA1 = makeAdjoints(1);
    for (int k = 0; k < 3; ++k) {
        singleA1.bP[k] = dupA.bP[3 + k];
        singleA1.bN[k] = dupA.bN[3 + k];
    }
    std::vector<Far::HierarchicalDisplacementSample> one1 =
        {baseSamples[1]};
    Far::HierarchicalDisplacement plan1(*refiner, 2, one1);
    AnalyticGradient g1 =
        analyticPullback(plan1, mesh.xyz, disps, singleA1);

    for (std::size_t i = 0; i < dupG.coarseGrad.size(); ++i) {
        TASK_CHECK(relErr(dupG.coarseGrad[i],
                          g0.coarseGrad[i] + g1.coarseGrad[i]) < 1e-10);
    }
    for (int l = 0; l < 2; ++l) {
        for (std::size_t i = 0; i < dupG.dispGrad[l].size(); ++i) {
            TASK_CHECK(relErr(dupG.dispGrad[l][i],
                              g0.dispGrad[l][i] + g1.dispGrad[l][i])
                       < 1e-10);
        }
    }
    return 0;
}

int adjointRigidTransform() {
    Mesh mesh = makeCubeMesh();
    auto refiner = makeRefiner(mesh);
    auto samples = makeSamples(mesh, 3);
    Far::HierarchicalDisplacement plan(*refiner, 2, samples);

    std::vector<std::vector<double> > disps(2);
    for (int l = 0; l < 2; ++l) {
        disps[l] = makeDisplacements(
            plan.GetNumLayerVertices(l + 1), l + 1);
    }
    ObjectiveAdjoints a = makeAdjoints(3);
    AnalyticGradient g =
        analyticPullback(plan, mesh.xyz, disps, a);

    // Translation: net coarse force equals the summed position adjoint;
    // the normal adjoint carries no translational component.
    double sumForce[3] = {0.0, 0.0, 0.0};
    for (int v = 0; v < plan.GetNumCoarseVertices(); ++v) {
        for (int k = 0; k < 3; ++k) {
            sumForce[k] += g.coarseGrad[3 * v + k];
        }
    }
    double sumBP[3] = {0.0, 0.0, 0.0};
    for (int i = 0; i < plan.GetNumSamples(); ++i) {
        for (int k = 0; k < 3; ++k) {
            sumBP[k] += a.bP[3 * i + k];
        }
    }
    for (int k = 0; k < 3; ++k) {
        TASK_CHECK(relErr(sumForce[k], sumBP[k]) < 1e-9);
    }

    // Rotation about the origin by angle theta around a tilted axis.
    double const theta = 0.37;
    double axis[3] = {0.3, -0.8, 0.52};
    double axisLen = norm3(axis);
    axis[0] /= axisLen; axis[1] /= axisLen; axis[2] /= axisLen;
    double R[3][3];
    double c = std::cos(theta), s = std::sin(theta), t = 1.0 - c;
    R[0][0] = c + axis[0] * axis[0] * t;
    R[0][1] = axis[0] * axis[1] * t - axis[2] * s;
    R[0][2] = axis[1] * s + axis[0] * axis[2] * t;
    R[1][0] = axis[2] * s + axis[0] * axis[1] * t;
    R[1][1] = c + axis[1] * axis[1] * t;
    R[1][2] = -axis[0] * s + axis[1] * axis[2] * t;
    R[2][0] = -axis[1] * s + axis[0] * axis[2] * t;
    R[2][1] = axis[0] * s + axis[1] * axis[2] * t;
    R[2][2] = c + axis[2] * axis[2] * t;
    auto rotate = [&](double const * in, double * out) {
        for (int i = 0; i < 3; ++i) {
            out[i] = R[i][0] * in[0] + R[i][1] * in[1]
                   + R[i][2] * in[2];
        }
    };

    std::vector<double> rotatedXYZ(mesh.xyz.size());
    for (int v = 0; v < plan.GetNumCoarseVertices(); ++v) {
        rotate(&mesh.xyz[3 * v], &rotatedXYZ[3 * v]);
    }
    ObjectiveAdjoints rotatedA = a;
    for (int i = 0; i < plan.GetNumSamples(); ++i) {
        rotate(&a.bP[3 * i], &rotatedA.bP[3 * i]);
        rotate(&a.bN[3 * i], &rotatedA.bN[3 * i]);
    }
    AnalyticGradient gr =
        analyticPullback(plan, rotatedXYZ, disps, rotatedA);

    for (int v = 0; v < plan.GetNumCoarseVertices(); ++v) {
        double expected[3];
        rotate(&g.coarseGrad[3 * v], expected);
        for (int k = 0; k < 3; ++k) {
            TASK_CHECK(relErr(gr.coarseGrad[3 * v + k], expected[k])
                       < 1e-9);
        }
    }
    // Scalar displacement gradients are rotation invariant.
    for (int l = 0; l < 2; ++l) {
        for (std::size_t i = 0; i < g.dispGrad[l].size(); ++i) {
            TASK_CHECK(relErr(gr.dispGrad[l][i], g.dispGrad[l][i])
                       < 1e-10);
        }
    }
    return 0;
}

// ==========================================================================
// ownership group
// ==========================================================================

int ownershipABAForwards() {
    Mesh mesh = makeCubeMesh();
    auto refiner = makeRefiner(mesh);
    auto samples = makeSamples(mesh, 3);
    Far::HierarchicalDisplacement plan(*refiner, 2, samples);

    std::vector<std::vector<double> > pA(2), pB(2);
    auto fillDisplacements = [&](std::vector<std::vector<double> > & store,
                                 int salt) {
        for (int l = 0; l < 2; ++l) {
            store[l] = makeDisplacements(
                plan.GetNumLayerVertices(l + 1), salt + l);
        }
    };
    fillDisplacements(pA, 1);
    fillDisplacements(pB, 9);

    Far::HierarchicalDisplacementResult rA1, rB, rA2;
    auto tA1 = plan.Forward(mesh.xyz, pA, rA1);
    auto tB  = plan.Forward(mesh.xyz, pB, rB);
    auto tA2 = plan.Forward(mesh.xyz, pA, rA2);

    TASK_CHECK(rA1.samplePositions.size() == rA2.samplePositions.size());
    for (std::size_t i = 0; i < rA1.samplePositions.size(); ++i) {
        TASK_CHECK(rA1.samplePositions[i] == rA2.samplePositions[i]);
        TASK_CHECK(rA1.sampleNormals[i] == rA2.sampleNormals[i]);
    }
    bool differs = false;
    for (std::size_t i = 0; i < rA1.samplePositions.size(); ++i) {
        if (rA1.samplePositions[i] != rB.samplePositions[i]) {
            differs = true;
        }
    }
    TASK_CHECK(differs);
    TASK_CHECK(tA1 != tA2);
    return 0;
}

int ownershipInterleavedTapes() {
    Mesh mesh = makeOpenPatchMesh();
    auto refiner = makeRefiner(mesh);
    auto samples = makeSamples(mesh, 2);
    Far::HierarchicalDisplacement plan(*refiner, 2, samples);

    std::vector<std::vector<double> > pA(2), pB(2);
    for (int l = 0; l < 2; ++l) {
        pA[l] = makeDisplacements(plan.GetNumLayerVertices(l + 1), l + 1);
        pB[l] = makeDisplacements(plan.GetNumLayerVertices(l + 1), l + 7);
    }
    ObjectiveAdjoints a = makeAdjoints(2);
    Far::HierarchicalDisplacementResult rA, rB;
    auto tA = plan.Forward(mesh.xyz, pA, rA);
    auto tB = plan.Forward(mesh.xyz, pB, rB);

    AnalyticGradient gA = analyticPullback(plan, mesh.xyz, pA, a);
    AnalyticGradient gB = analyticPullback(plan, mesh.xyz, pB, a);

    std::vector<double> gradA(3 * plan.GetNumCoarseVertices(), 0.0);
    std::vector<double> gradB(3 * plan.GetNumCoarseVertices(), 0.0);
    std::vector<std::vector<double> > dgradA(2), dgradB(2);
    std::vector<std::vector<double> *> ppA(2), ppB(2);
    for (int l = 0; l < 2; ++l) {
        dgradA[l].assign(plan.GetNumLayerVertices(l + 1), 0.0);
        dgradB[l].assign(plan.GetNumLayerVertices(l + 1), 0.0);
        ppA[l] = &dgradA[l];
        ppB[l] = &dgradB[l];
    }
    // Interleave the two pullbacks on the old tapes.
    tB->Pullback(a.bP, a.bN, gradB, ppB);
    tA->Pullback(a.bP, a.bN, gradA, ppA);
    tB->Pullback(a.bP, a.bN, gradB, ppB);
    tA->Pullback(a.bP, a.bN, gradA, ppA);

    for (std::size_t i = 0; i < gradA.size(); ++i) {
        TASK_CHECK(relErr(gradA[i], 2.0 * gA.coarseGrad[i]) < 1e-12);
        TASK_CHECK(relErr(gradB[i], 2.0 * gB.coarseGrad[i]) < 1e-12);
    }
    for (int l = 0; l < 2; ++l) {
        for (std::size_t i = 0; i < dgradA[l].size(); ++i) {
            TASK_CHECK(relErr(dgradA[l][i],
                              2.0 * gA.dispGrad[l][i]) < 1e-12);
            TASK_CHECK(relErr(dgradB[l][i],
                              2.0 * gB.dispGrad[l][i]) < 1e-12);
        }
    }
    return 0;
}

int ownershipSourceRelease() {
    Mesh mesh = makeCubeMesh();
    auto refiner = makeRefiner(mesh);
    auto samples = makeSamples(mesh, 2);
    Far::HierarchicalDisplacement plan(*refiner, 2, samples);

    std::vector<double> xyz = mesh.xyz;
    std::vector<std::vector<double> > disps(2);
    for (int l = 0; l < 2; ++l) {
        disps[l] = makeDisplacements(
            plan.GetNumLayerVertices(l + 1), l + 3);
    }
    ObjectiveAdjoints a = makeAdjoints(2);
    AnalyticGradient expected =
        analyticPullback(plan, xyz, disps, a);

    Far::HierarchicalDisplacementResult result;
    auto tape = plan.Forward(xyz, disps, result);

    // Destroy every caller-side input after the successful forward.
    std::fill(xyz.begin(), xyz.end(), 999.0);
    for (auto & v : disps) {
        std::fill(v.begin(), v.end(), 999.0);
    }

    AnalyticGradient actual;
    actual.coarseGrad.assign(3 * plan.GetNumCoarseVertices(), 0.0);
    actual.dispGrad.resize(2);
    std::vector<std::vector<double> *> pp(2);
    for (int l = 0; l < 2; ++l) {
        actual.dispGrad[l].assign(
            plan.GetNumLayerVertices(l + 1), 0.0);
        pp[l] = &actual.dispGrad[l];
    }
    tape->Pullback(a.bP, a.bN, actual.coarseGrad, pp);
    for (std::size_t i = 0; i < expected.coarseGrad.size(); ++i) {
        TASK_CHECK(actual.coarseGrad[i] == expected.coarseGrad[i]);
    }
    for (int l = 0; l < 2; ++l) {
        for (std::size_t i = 0; i < expected.dispGrad[l].size(); ++i) {
            TASK_CHECK(actual.dispGrad[l][i]
                       == expected.dispGrad[l][i]);
        }
    }
    return 0;
}

int ownershipPlanRelease() {
    Mesh mesh = makeOpenPatchMesh();
    auto samples = makeSamples(mesh, 2);
    ObjectiveAdjoints a = makeAdjoints(2);

    std::shared_ptr<Far::HierarchicalDisplacement::Tape> tape;
    AnalyticGradient expected;
    int numCoarse = 0;
    int numLayers = 0;
    std::vector<int> layerVerts;

    {
        auto refiner = makeRefiner(mesh);
        Far::HierarchicalDisplacement plan(*refiner, 2, samples);
        numCoarse = plan.GetNumCoarseVertices();
        numLayers = plan.GetNumLayers();
        layerVerts.resize(numLayers);
        std::vector<std::vector<double> > disps(numLayers);
        for (int l = 0; l < numLayers; ++l) {
            layerVerts[l] = plan.GetNumLayerVertices(l + 1);
            disps[l] = makeDisplacements(layerVerts[l], l + 2);
        }
        expected = analyticPullback(plan, mesh.xyz, disps, a);
        Far::HierarchicalDisplacementResult result;
        tape = plan.Forward(mesh.xyz, disps, result);
        // refiner and plan go out of scope here.
    }

    AnalyticGradient actual;
    actual.coarseGrad.assign(3 * numCoarse, 0.0);
    actual.dispGrad.resize(numLayers);
    std::vector<std::vector<double> *> pp(numLayers);
    for (int l = 0; l < numLayers; ++l) {
        actual.dispGrad[l].assign(layerVerts[l], 0.0);
        pp[l] = &actual.dispGrad[l];
    }
    tape->Pullback(a.bP, a.bN, actual.coarseGrad, pp);
    for (std::size_t i = 0; i < expected.coarseGrad.size(); ++i) {
        TASK_CHECK(relErr(actual.coarseGrad[i],
                          expected.coarseGrad[i]) < 1e-14);
    }
    for (int l = 0; l < numLayers; ++l) {
        for (std::size_t i = 0; i < expected.dispGrad[l].size(); ++i) {
            TASK_CHECK(relErr(actual.dispGrad[l][i],
                              expected.dispGrad[l][i]) < 1e-14);
        }
    }
    return 0;
}

int ownershipFailureKeepsOldTape() {
    Mesh mesh = makeCubeMesh();
    auto refiner = makeRefiner(mesh);
    auto samples = makeSamples(mesh, 2);
    Far::HierarchicalDisplacement plan(*refiner, 2, samples);

    std::vector<std::vector<double> > disps(2);
    for (int l = 0; l < 2; ++l) {
        disps[l] = makeDisplacements(
            plan.GetNumLayerVertices(l + 1), l + 2);
    }
    ObjectiveAdjoints a = makeAdjoints(2);
    AnalyticGradient expected =
        analyticPullback(plan, mesh.xyz, disps, a);
    Far::HierarchicalDisplacementResult goodResult;
    auto oldTape = plan.Forward(mesh.xyz, disps, goodResult);

    // All coarse points collapsed: every area vector vanishes.
    std::vector<double> collapsed(3 * plan.GetNumCoarseVertices(), 0.0);
    bool threwRuntime = false;
    try {
        Far::HierarchicalDisplacementResult bad;
        plan.Forward(collapsed, disps, bad);
    } catch (std::runtime_error const &) {
        threwRuntime = true;
    }
    TASK_CHECK(threwRuntime);

    // Non-finite input is an invalid_argument, distinct from the
    // geometric failure.
    bool threwInvalid = false;
    try {
        std::vector<double> badXYZ = mesh.xyz;
        badXYZ[2] = std::numeric_limits<double>::infinity();
        Far::HierarchicalDisplacementResult bad;
        plan.Forward(badXYZ, disps, bad);
    } catch (std::invalid_argument const &) {
        threwInvalid = true;
    }
    TASK_CHECK(threwInvalid);

    // The old tape must still pull back exactly as before.
    AnalyticGradient after;
    after.coarseGrad.assign(3 * plan.GetNumCoarseVertices(), 0.0);
    after.dispGrad.resize(2);
    std::vector<std::vector<double> *> pp(2);
    for (int l = 0; l < 2; ++l) {
        after.dispGrad[l].assign(
            plan.GetNumLayerVertices(l + 1), 0.0);
        pp[l] = &after.dispGrad[l];
    }
    oldTape->Pullback(a.bP, a.bN, after.coarseGrad, pp);
    for (std::size_t i = 0; i < expected.coarseGrad.size(); ++i) {
        TASK_CHECK(after.coarseGrad[i] == expected.coarseGrad[i]);
    }
    for (int l = 0; l < 2; ++l) {
        for (std::size_t i = 0; i < expected.dispGrad[l].size(); ++i) {
            TASK_CHECK(after.dispGrad[l][i]
                       == expected.dispGrad[l][i]);
        }
    }

    // A follow-up healthy forward must also work.
    Far::HierarchicalDisplacementResult again;
    auto newTape = plan.Forward(mesh.xyz, disps, again);
    for (std::size_t i = 0; i < again.samplePositions.size(); ++i) {
        TASK_CHECK(again.samplePositions[i]
                   == goodResult.samplePositions[i]);
    }
    return 0;
}

// ==========================================================================
// Registration (external linkage required by task_support/driver.c)
// ==========================================================================

} // anonymous namespace

const struct task_case task_cases[] = {
    {"forward",   "closed_cube_reference",      forwardClosedCube},
    {"forward",   "open_patch_reference",       forwardOpenPatch},
    {"forward",   "loop_fan_reference",         forwardLoopFan},
    {"forward",   "three_layers",               forwardThreeLayers},
    {"forward",   "two_components",             forwardTwoComponents},
    {"forward",   "zero_displacement",          forwardZeroDisplacement},
    {"forward",   "argument_validation",        forwardArgumentValidation},
    {"adjoint",   "finite_difference_quad",     adjointFiniteDifferencesQuad},
    {"adjoint",   "finite_difference_loop",     adjointFiniteDifferencesLoop},
    {"adjoint",   "transpose_inner_product",    adjointTransposeIdentity},
    {"adjoint",   "accumulation_and_transpose", adjointAccumulationAndStencilTranspose},
    {"adjoint",   "duplicate_samples",          adjointDuplicateSamples},
    {"adjoint",   "rigid_transform",            adjointRigidTransform},
    {"ownership", "aba_forwards",               ownershipABAForwards},
    {"ownership", "interleaved_tapes",          ownershipInterleavedTapes},
    {"ownership", "source_release",             ownershipSourceRelease},
    {"ownership", "plan_release",               ownershipPlanRelease},
    {"ownership", "failure_keeps_old_tape",     ownershipFailureKeepsOldTape}
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
