//
//   Copyright 2024 DreamWorks Animation LLC.
//
//   Licensed under the terms set forth in the LICENSE.txt file available at
//   https://opensubdiv.org/license.
//
//  Task tests for Far::CreaseSensitivityTable.
//
#include "registry.hpp"

#include "opensubdiv/far/topologyDescriptor.h"
#include "opensubdiv/far/topologyRefinerFactory.h"
#include "opensubdiv/far/primvarRefiner.h"
#include "opensubdiv/far/creaseSensitivityTable.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <vector>

using namespace OpenSubdiv;

namespace {

// ---------------------------------------------------------------------------
// Mesh and refiner construction helpers.
// ---------------------------------------------------------------------------
struct Mesh {
    int                     nverts = 0;
    std::vector<int>        faceCounts;
    std::vector<int>        faceVerts;
    std::vector<int>        creasePairs;
    std::vector<float>      creaseWeights;
};

struct CreaseSpec { int a, b; float w; };

static Mesh makeCube() {
    Mesh m;
    m.nverts = 8;
    int counts[6] = {4, 4, 4, 4, 4, 4};
    int ids[24] = {0, 1, 3, 2,  2, 3, 5, 4,  4, 5, 7, 6,
                   6, 7, 1, 0,  1, 7, 5, 3,  6, 0, 2, 4};
    m.faceCounts.assign(counts, counts + 6);
    m.faceVerts.assign(ids, ids + 24);
    return m;
}

// Closed, asymmetric shell: the cube with one quad face split into two
// triangles (5 quads + 2 triangles, same eight vertices).
static Mesh makeAsymmetricShell() {
    Mesh m;
    m.nverts = 8;
    int ids[24] = {0, 1, 3, 2,  2, 3, 5, 4,  4, 5, 7, 6,
                   6, 7, 1, 0,  1, 7, 5, 3,  6, 0, 2, 4};
    m.faceCounts.push_back(3);
    m.faceVerts.insert(m.faceVerts.end(), {0, 1, 3});
    m.faceCounts.push_back(3);
    m.faceVerts.insert(m.faceVerts.end(), {0, 3, 2});
    for (int f = 1; f < 6; ++f) {
        m.faceCounts.push_back(4);
        m.faceVerts.insert(m.faceVerts.end(), ids + 4 * f, ids + 4 * f + 4);
    }
    return m;
}

// Open 4x4 grid of quads with a boundary (25 vertices).
static Mesh makeGrid44() {
    Mesh m;
    m.nverts = 25;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            int v = y * 5 + x;
            m.faceCounts.push_back(4);
            m.faceVerts.insert(m.faceVerts.end(), {v, v + 1, v + 6, v + 5});
        }
    }
    return m;
}

// Open disk with an interior valence-5 vertex: triangle fan plus a quad ring.
static Mesh makeValenceFiveFan() {
    Mesh m;
    m.nverts = 11;
    for (int i = 0; i < 5; ++i) {
        int j = (i + 1) % 5;
        m.faceCounts.push_back(3);
        m.faceVerts.insert(m.faceVerts.end(), {0, 1 + i, 1 + j});
        m.faceCounts.push_back(4);
        m.faceVerts.insert(m.faceVerts.end(), {1 + i, 6 + i, 6 + j, 1 + j});
    }
    return m;
}

static void addCrease(Mesh& m, int a, int b, float w) {
    m.creasePairs.push_back(a);
    m.creasePairs.push_back(b);
    m.creaseWeights.push_back(w);
}

static std::unique_ptr<Far::TopologyRefiner>
makeRefiner(Mesh const& mesh, int level, bool refine,
            std::vector<float> const* weightOverride = nullptr) {

    Far::TopologyDescriptor d;
    d.numVertices       = mesh.nverts;
    d.numFaces          = (int)mesh.faceCounts.size();
    d.numVertsPerFace   = mesh.faceCounts.data();
    d.vertIndicesPerFace = mesh.faceVerts.data();
    d.numCreases        = (int)mesh.creaseWeights.size();
    d.creaseVertexIndexPairs = mesh.creasePairs.data();
    d.creaseWeights     = weightOverride ? weightOverride->data()
                                         : mesh.creaseWeights.data();
    d.numCorners        = 0;
    d.numHoles          = 0;
    d.numFVarChannels   = 0;

    Sdc::Options options;
    options.SetVtxBoundaryInterpolation(Sdc::Options::VTX_BOUNDARY_EDGE_ONLY);

    std::unique_ptr<Far::TopologyRefiner> refiner(
        Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Create(
            d, {Sdc::SCHEME_CATMARK, options}));
    if (refine) {
        refiner->RefineUniform(Far::TopologyRefiner::UniformOptions(level));
    }
    return refiner;
}

struct ScalarValue {
    double v = 0.0;
    void Clear() { v = 0.0; }
    void AddWithWeight(ScalarValue const& src, double w) { v += w * src.v; }
};

static std::vector<double>
refinePositions(Far::TopologyRefiner const& refiner,
                std::vector<double> const& control) {
    Far::PrimvarRefinerReal<double> primvar(refiner);
    std::vector<ScalarValue> src(control.size()), dst;
    for (size_t i = 0; i < control.size(); ++i) src[i].v = control[i];
    for (int l = 1; l <= refiner.GetMaxLevel(); ++l) {
        dst.resize(refiner.GetLevel(l).GetNumVertices());
        primvar.Interpolate(l, src, dst);
        src.swap(dst);
    }
    std::vector<double> out(src.size());
    for (size_t i = 0; i < src.size(); ++i) out[i] = src[i].v;
    return out;
}

// Finite-difference reference: two float-quantized sharpness values, with the
// denominator taken as their actual float difference.
static double
finiteDifferenceError(Mesh const& mesh, int creaseIndex, float baseSharpness,
                      float step, int level,
                      std::vector<double> const& control,
                      std::vector<double> const& analyticDeriv) {
    std::vector<float> wPlus  = mesh.creaseWeights;
    std::vector<float> wMinus = mesh.creaseWeights;
    float sPlus  = (float)((double)baseSharpness + (double)step);
    float sMinus = (float)((double)baseSharpness - (double)step);
    wPlus[creaseIndex]  = sPlus;
    wMinus[creaseIndex] = sMinus;

    std::unique_ptr<Far::TopologyRefiner> rPlus  = makeRefiner(mesh, level, true, &wPlus);
    std::unique_ptr<Far::TopologyRefiner> rMinus = makeRefiner(mesh, level, true, &wMinus);

    std::vector<double> pPlus  = refinePositions(*rPlus, control);
    std::vector<double> pMinus = refinePositions(*rMinus, control);

    double denominator = (double)sPlus - (double)sMinus;
    double maxError = 0.0;
    for (size_t i = 0; i < analyticDeriv.size(); ++i) {
        double fd = (pPlus[i] - pMinus[i]) / denominator;
        maxError = std::max(maxError, std::fabs(fd - analyticDeriv[i]));
    }
    return maxError;
}

static std::vector<double> unitScaleControls(int count, uint64_t seed) {
    std::mt19937_64 engine(seed);
    std::uniform_real_distribution<double> uniform(-1.5, 1.5);
    std::vector<double> control(count);
    for (double& x : control) x = uniform(engine);
    return control;
}

// Look up a stencil entry (position and sensitivity) for a coarse column.
static void entryAt(Far::CreaseSensitivityTable const& table, int row, int column,
                    double& positionWeight, double& sharpnessWeight) {
    positionWeight = 0.0;
    sharpnessWeight = 0.0;
    for (int k = table.GetOffsets()[row]; k < table.GetOffsets()[row + 1]; ++k) {
        if (table.GetIndices()[k] == column) {
            positionWeight    = table.GetWeights()[k];
            sharpnessWeight   = table.GetSharpnessWeights()[k];
            return;
        }
    }
}

static int creaseIndexOf(Mesh const& mesh, int a, int b) {
    for (size_t i = 0; i < mesh.creaseWeights.size(); ++i) {
        if (mesh.creasePairs[2 * i] == a && mesh.creasePairs[2 * i + 1] == b) {
            return (int)i;
        }
    }
    return -1;
}

// Shared structural invariants used across every case.
static void checkTableStructure(Case& q,
                                Far::CreaseSensitivityTable const& table,
                                Far::TopologyRefiner const& refiner,
                                int level) {
    int rows = table.GetNumVertices();
    q.check(rows == refiner.GetLevel(level).GetNumVertices(),
            "row count matches RefineUniform vertex count");
    q.check((int)table.GetOffsets().size() == rows + 1, "CSR offset size");
    q.check(table.GetOffsets()[0] == 0, "CSR offsets start at zero");
    q.check(table.GetIndices().size() == table.GetWeights().size() &&
            table.GetIndices().size() == table.GetSharpnessWeights().size(),
            "indices/weights/sharpnessWeights are equal length");

    for (int row = 0; row < rows; ++row) {
        q.check(table.GetOffsets()[row + 1] >= table.GetOffsets()[row],
                "monotone CSR offsets");
        for (int k = table.GetOffsets()[row] + 1;
             k < table.GetOffsets()[row + 1]; ++k) {
            q.check(table.GetIndices()[k] > table.GetIndices()[k - 1],
                    "columns strictly increasing and merged");
        }
    }
}

static void checkRowSums(Case& q, Far::CreaseSensitivityTable const& table) {
    for (int row = 0; row < table.GetNumVertices(); ++row) {
        double positionSum = 0.0;
        double sharpnessSum = 0.0;
        for (int k = table.GetOffsets()[row]; k < table.GetOffsets()[row + 1]; ++k) {
            positionSum    += table.GetWeights()[k];
            sharpnessSum   += table.GetSharpnessWeights()[k];
        }
        q.check(std::fabs(positionSum - 1.0) < 1e-10, "position row sums to one");
        q.check(std::fabs(sharpnessSum) < 1e-10, "sharpness row sums to zero");
    }
}

} // anonymous namespace

// ===========================================================================
// Group "masks": single-round rules verified against independent formulas.
// ===========================================================================

// Ordinary interior edge point under a crease-to-smooth fractional blend.
static int masksEdgePoint(Case& q) {
    Mesh mesh = makeCube();
    const float sharpness = 0.35f;
    addCrease(mesh, 0, 1, sharpness);

    std::unique_ptr<Far::TopologyRefiner> base = makeRefiner(mesh, 1, false);
    Far::TopologyLevel const& level0 = base->GetLevel(0);
    int edge = level0.FindEdge(0, 1);

    std::unique_ptr<Far::CreaseSensitivityTable const> table(
        Far::CreaseSensitivityTable::Create(*base, 1, edge));

    // Refine a second copy to locate the edge child vertex (standard order).
    std::unique_ptr<Far::TopologyRefiner> refined = makeRefiner(mesh, 1, true);
    checkTableStructure(q, *table, *refined, 1);
    int edgeChild = refined->GetLevel(0).GetEdgeChildVertex(edge);

    // Independent Catmull-Clark formula for an interior edge:
    //   smooth endpoint weight 1/4, smooth face weight 1/4 (two faces)
    //   blended endpoint = s*1/2 + (1-s)*1/4
    //   blended face     = (1-s)*1/4
    double endpoint = (double)sharpness * 0.5 + (1.0 - sharpness) * 0.25;
    double face     = (1.0 - sharpness) * 0.25;

    std::map<int, std::pair<double, double>> expected; // column -> (w, dw/ds)
    expected[0].first  += endpoint;
    expected[1].first  += endpoint;
    expected[0].second += (0.5 - 0.25);
    expected[1].second += (0.5 - 0.25);

    Far::ConstIndexArray incidentFaces = level0.GetEdgeFaces(edge);
    for (int f = 0; f < incidentFaces.size(); ++f) {
        Far::ConstIndexArray faceVerts = level0.GetFaceVertices(incidentFaces[f]);
        for (int i = 0; i < faceVerts.size(); ++i) {
            expected[faceVerts[i]].first  += face / faceVerts.size();
            expected[faceVerts[i]].second += -0.25 / faceVerts.size();
        }
    }

    for (std::map<int, std::pair<double, double>>::const_iterator it = expected.begin();
         it != expected.end(); ++it) {
        double w = 0.0, sw = 0.0;
        entryAt(*table, edgeChild, it->first, w, sw);
        q.check(std::fabs(w - it->second.first) < 1e-12,
                "edge point position weight matches independent formula");
        q.check(std::fabs(sw - it->second.second) < 1e-12,
                "edge point sharpness derivative matches independent formula");
    }
    checkRowSums(q, *table);
    return 0;
}

// Vertex where two fractional creases decay in the same round, transitioning
// from the crease rule to the smooth rule (joint transition weight is the
// average of the two decaying sharpness values).
static int masksJointVertexTransition(Case& q) {
    Mesh mesh = makeGrid44();
    const int center = 12;
    const float sharpness = 0.35f;
    addCrease(mesh, center, 13, sharpness); // selected crease end
    addCrease(mesh, center, 17, sharpness); // second crease, same decay round

    std::unique_ptr<Far::TopologyRefiner> base = makeRefiner(mesh, 1, false);
    Far::TopologyLevel const& level0 = base->GetLevel(0);
    int selectedEdge = level0.FindEdge(center, 13);

    std::unique_ptr<Far::CreaseSensitivityTable const> table(
        Far::CreaseSensitivityTable::Create(*base, 1, selectedEdge));

    std::unique_ptr<Far::TopologyRefiner> refined = makeRefiner(mesh, 1, true);
    checkTableStructure(q, *table, *refined, 1);
    int vertexChild = refined->GetLevel(0).GetVertexChildVertex(center);

    // Independent mask formulas (valence 4):
    //   crease: vertex 3/4, the two crease-end edges 1/8
    //   smooth: vertex 1/2, every edge and every face 1/16
    // The fractional weight is (s+s)/2 = s and its derivative with respect
    // to the selected edge is 1/2 (the neighboring crease does not move).
    const double frac = sharpness;
    const double dFrac = 0.5;
    std::map<int, std::pair<double, double>> expected;

    auto accumulate = [&](int column, double creaseMask, double smoothMask) {
        double w  = frac * creaseMask + (1.0 - frac) * smoothMask;
        double sw = dFrac * (creaseMask - smoothMask);
        expected[column].first  += w;
        expected[column].second += sw;
    };

    accumulate(center, 0.75, 0.5); // vertex slot

    Far::ConstIndexArray edges = level0.GetVertexEdges(center);
    for (int i = 0; i < edges.size(); ++i) {
        Far::ConstIndexArray edgeVerts = level0.GetEdgeVertices(edges[i]);
        int opposite = (edgeVerts[0] == center) ? edgeVerts[1] : edgeVerts[0];
        bool isCreaseEnd =
            (opposite == 13 || opposite == 17);
        accumulate(opposite, isCreaseEnd ? 0.125 : 0.0, 0.0625);
    }

    Far::ConstIndexArray faces = level0.GetVertexFaces(center);
    for (int i = 0; i < faces.size(); ++i) {
        int faceChild = refined->GetLevel(0).GetFaceChildVertex(faces[i]);
        (void)faceChild;
        Far::ConstIndexArray faceVerts = level0.GetFaceVertices(faces[i]);
        for (int k = 0; k < faceVerts.size(); ++k) {
            // Face-center mask weight 1/16, distributed evenly over the face.
            accumulate(faceVerts[k], 0.0, 0.0625 / faceVerts.size());
        }
    }

    for (std::map<int, std::pair<double, double>>::const_iterator it = expected.begin();
         it != expected.end(); ++it) {
        double w = 0.0, sw = 0.0;
        entryAt(*table, vertexChild, it->first, w, sw);
        q.check(std::fabs(w - it->second.first) < 1e-12,
                "joint transition position weight matches independent formula");
        q.check(std::fabs(sw - it->second.second) < 1e-12,
                "joint transition derivative matches independent formula");
    }
    checkRowSums(q, *table);
    return 0;
}

// ===========================================================================
// Group "propagated-sensitivity": multi-round analytic propagation checked
// against external PrimvarRefiner finite differences.
// ===========================================================================

struct PropagationScenario {
    const char* name;
    Mesh mesh;
    int edgeA, edgeB;
    float sharpness;
    int level;
    uint64_t seed;
};

static int runPropagationScenario(Case& q, PropagationScenario const& scenario) {
    Mesh mesh = scenario.mesh;
    addCrease(mesh, scenario.edgeA, scenario.edgeB, scenario.sharpness);
    int creaseIndex = creaseIndexOf(mesh, scenario.edgeA, scenario.edgeB);

    std::unique_ptr<Far::TopologyRefiner> base = makeRefiner(mesh, scenario.level, false);
    int edge = base->GetLevel(0).FindEdge(scenario.edgeA, scenario.edgeB);

    std::unique_ptr<Far::CreaseSensitivityTable const> table(
        Far::CreaseSensitivityTable::Create(*base, scenario.level, edge));

    std::unique_ptr<Far::TopologyRefiner> refined =
        makeRefiner(mesh, scenario.level, true);
    checkTableStructure(q, *table, *refined, scenario.level);
    checkRowSums(q, *table);

    std::vector<double> control = unitScaleControls(mesh.nverts, scenario.seed);
    std::vector<double> values, derivatives;
    table->Evaluate(control, 1, values, derivatives);

    // Positions must agree with the existing double-precision primvar path.
    std::vector<double> reference = refinePositions(*refined, control);
    double positionError = 0.0;
    for (size_t i = 0; i < values.size(); ++i) {
        positionError = std::max(positionError, std::fabs(values[i] - reference[i]));
    }
    q.check(positionError < 1e-7, "positions match double primvar refinement");

    // External finite differences at two step sizes; float-quantized
    // denominator.  Both must meet the unit-scale tolerance, and the finer
    // step must not degrade relative to the coarser one (convergence check).
    double coarseError = finiteDifferenceError(
        mesh, creaseIndex, scenario.sharpness, 1.0f / 64.0f,
        scenario.level, control, derivatives);
    double fineError = finiteDifferenceError(
        mesh, creaseIndex, scenario.sharpness, 1.0f / 256.0f,
        scenario.level, control, derivatives);
    q.check(coarseError < 5e-4, "coarse-step finite difference tolerance");
    q.check(fineError < 5e-4, "fine-step finite difference tolerance");
    q.check(fineError <= coarseError + 1e-6, "finite differences converge with step");

    // A global translation changes values only, never derivatives.
    std::vector<double> translated = control;
    for (double& x : translated) x += 3.25;
    std::vector<double> translatedValues, translatedDerivatives;
    table->Evaluate(translated, 1, translatedValues, translatedDerivatives);
    double translationValueError = 0.0, translationDerivError = 0.0;
    for (size_t i = 0; i < values.size(); ++i) {
        translationValueError =
            std::max(translationValueError,
                     std::fabs(translatedValues[i] - (values[i] + 3.25)));
        translationDerivError =
            std::max(translationDerivError,
                     std::fabs(translatedDerivatives[i] - derivatives[i]));
    }
    q.check(translationValueError < 1e-10 && translationDerivError < 1e-10,
            "translation leaves derivatives unchanged and shifts values");

    // Arbitrary three-channel signal reuses the same stencil.
    std::vector<double> interleaved((size_t)mesh.nverts * 3);
    for (int v = 0; v < mesh.nverts; ++v) {
        interleaved[3 * v + 0] = control[v];
        interleaved[3 * v + 1] = 0.5 * control[v] - 1.0;
        interleaved[3 * v + 2] = -2.0 * control[v] + 0.75;
    }
    std::vector<double> channelValues, channelDerivatives;
    table->Evaluate(interleaved, 3, channelValues, channelDerivatives);
    q.check(channelValues.size() == (size_t)table->GetNumVertices() * 3 &&
            channelDerivatives.size() == channelValues.size(),
            "multi-channel output sizing");
    double channelError = 0.0;
    for (int row = 0; row < table->GetNumVertices(); ++row) {
        for (int c = 0; c < 3; ++c) {
            double expectedValue = 0.0, expectedDeriv = 0.0;
            for (int k = table->GetOffsets()[row]; k < table->GetOffsets()[row + 1]; ++k) {
                int column = table->GetIndices()[k];
                expectedValue += table->GetWeights()[k] * interleaved[3 * column + c];
                expectedDeriv += table->GetSharpnessWeights()[k] * interleaved[3 * column + c];
            }
            channelError = std::max(channelError,
                std::fabs(channelValues[3 * row + c] - expectedValue));
            channelError = std::max(channelError,
                std::fabs(channelDerivatives[3 * row + c] - expectedDeriv));
        }
    }
    q.check(channelError < 1e-10, "arbitrary multi-channel signal evaluates correctly");
    return 0;
}

static int propagatedClosedShell(Case& q) {
    PropagationScenario s;
    s.name = "closed-shell";
    s.mesh = makeAsymmetricShell();
    s.edgeA = 0; s.edgeB = 1; s.sharpness = 0.35f; s.level = 3; s.seed = 11;
    return runPropagationScenario(q, s);
}

static int propagatedOpenPatch(Case& q) {
    PropagationScenario s;
    s.name = "open-patch";
    s.mesh = makeGrid44();
    s.edgeA = 12; s.edgeB = 13; s.sharpness = 1.40f; s.level = 3; s.seed = 23;
    return runPropagationScenario(q, s);
}

static int propagatedIrregularValence(Case& q) {
    PropagationScenario s;
    s.name = "irregular-valence";
    s.mesh = makeValenceFiveFan();
    s.edgeA = 0; s.edgeB = 1; s.sharpness = 2.25f; s.level = 4; s.seed = 37;
    return runPropagationScenario(q, s);
}

// Sharpness 2.25 is exhausted after three uniform rounds, but the geometric
// sensitivity must still be carried by the smooth subdivision at level four.
static int propagatedExhaustedPersistence(Case& q) {
    Mesh mesh = makeGrid44();
    addCrease(mesh, 12, 13, 2.25f);
    addCrease(mesh, 18, 13, 0.90f); // neighboring crease, different decay round

    std::unique_ptr<Far::TopologyRefiner> base = makeRefiner(mesh, 4, false);
    int edge = base->GetLevel(0).FindEdge(12, 13);
    std::unique_ptr<Far::CreaseSensitivityTable const> table(
        Far::CreaseSensitivityTable::Create(*base, 4, edge));

    double maxSensitivity = 0.0;
    for (size_t k = 0; k < table->GetSharpnessWeights().size(); ++k) {
        maxSensitivity = std::max(maxSensitivity,
                                  std::fabs(table->GetSharpnessWeights()[k]));
    }
    q.check(maxSensitivity > 1e-6,
            "sensitivity remains nonzero after the crease is exhausted");

    std::vector<double> control = unitScaleControls(mesh.nverts, 51);
    std::vector<double> values, derivatives;
    table->Evaluate(control, 1, values, derivatives);
    double fdError = finiteDifferenceError(
        mesh, creaseIndexOf(mesh, 12, 13), 2.25f, 1.0f / 128.0f, 4,
        control, derivatives);
    q.check(fdError < 5e-4, "post-exhaustion propagation matches finite differences");
    checkRowSums(q, *table);
    return 0;
}

// Two creases meeting at one vertex but decaying in different rounds
// (0.35 versus 2.25).  The round in which only the selected edge is
// fractional must not attribute the other edge's transition weight to it.
static int propagatedTwoCreaseRounds(Case& q) {
    Mesh mesh = makeGrid44();
    const int center = 12;
    addCrease(mesh, center, 13, 0.35f);  // selected, decays immediately
    addCrease(mesh, center, 17, 2.25f);  // still sharp for the first two rounds

    int level = 3;
    std::unique_ptr<Far::TopologyRefiner> base = makeRefiner(mesh, level, false);
    int edge = base->GetLevel(0).FindEdge(center, 13);
    std::unique_ptr<Far::CreaseSensitivityTable const> table(
        Far::CreaseSensitivityTable::Create(*base, level, edge));
    std::unique_ptr<Far::TopologyRefiner> refined = makeRefiner(mesh, level, true);

    checkTableStructure(q, *table, *refined, level);
    checkRowSums(q, *table);

    std::vector<double> control = unitScaleControls(mesh.nverts, 103);
    std::vector<double> values, derivatives;
    table->Evaluate(control, 1, values, derivatives);

    std::vector<double> reference = refinePositions(*refined, control);
    double positionError = 0.0;
    for (size_t i = 0; i < values.size(); ++i) {
        positionError = std::max(positionError, std::fabs(values[i] - reference[i]));
    }
    q.check(positionError < 1e-7, "two-crease positions match primvar refinement");

    double fdError = finiteDifferenceError(
        mesh, creaseIndexOf(mesh, center, 13), 0.35f, 1.0f / 128.0f,
        level, control, derivatives);
    q.check(fdError < 5e-4, "two-crease different-round transitions match finite differences");
    return 0;
}

// Three creases meeting at one interior vertex with the three required
// strengths (0.35, 1.40, 2.25), so their sharpness decays in different rounds.
// The selected edge is the weakest one; the corner/crease/dart rule transitions
// at the shared vertex must use the joint transition weight, not the selected
// edge's sharpness alone.
static int propagatedThreeCreaseRounds(Case& q) {
    Mesh mesh = makeGrid44();
    const int center = 12;
    addCrease(mesh, center, 13, 0.35f);  // selected, decays in round one
    addCrease(mesh, center, 17, 1.40f);  // crosses an integer, decays later
    addCrease(mesh, center, 11, 2.25f);  // decays last

    int level = 4;
    std::unique_ptr<Far::TopologyRefiner> base = makeRefiner(mesh, level, false);
    int edge = base->GetLevel(0).FindEdge(center, 13);
    std::unique_ptr<Far::CreaseSensitivityTable const> table(
        Far::CreaseSensitivityTable::Create(*base, level, edge));
    std::unique_ptr<Far::TopologyRefiner> refined = makeRefiner(mesh, level, true);

    checkTableStructure(q, *table, *refined, level);
    checkRowSums(q, *table);

    std::vector<double> control = unitScaleControls(mesh.nverts, 91);
    std::vector<double> values, derivatives;
    table->Evaluate(control, 1, values, derivatives);

    std::vector<double> reference = refinePositions(*refined, control);
    double positionError = 0.0;
    for (size_t i = 0; i < values.size(); ++i) {
        positionError = std::max(positionError, std::fabs(values[i] - reference[i]));
    }
    q.check(positionError < 1e-7, "three-crease positions match primvar refinement");

    double coarseError = finiteDifferenceError(
        mesh, creaseIndexOf(mesh, center, 13), 0.35f, 1.0f / 64.0f,
        level, control, derivatives);
    double fineError = finiteDifferenceError(
        mesh, creaseIndexOf(mesh, center, 13), 0.35f, 1.0f / 256.0f,
        level, control, derivatives);
    q.check(coarseError < 5e-4 && fineError < 5e-4,
            "three-crease joint transitions match finite differences");
    q.check(fineError <= coarseError + 1e-6, "three-crease step convergence");
    return 0;
}

// A semi-sharp selected edge alongside an infinitely sharp neighbor.
static int propagatedInfiniteNeighbor(Case& q) {
    Mesh mesh = makeGrid44();
    addCrease(mesh, 12, 13, 0.35f); // selected semi-sharp edge
    addCrease(mesh, 12, 17, 10.0f); // infinitely sharp neighbor

    std::unique_ptr<Far::TopologyRefiner> base = makeRefiner(mesh, 3, false);
    int edge = base->GetLevel(0).FindEdge(12, 13);
    std::unique_ptr<Far::CreaseSensitivityTable const> table(
        Far::CreaseSensitivityTable::Create(*base, 3, edge));

    std::vector<double> control = unitScaleControls(mesh.nverts, 67);
    std::vector<double> values, derivatives;
    table->Evaluate(control, 1, values, derivatives);
    double fdError = finiteDifferenceError(
        mesh, creaseIndexOf(mesh, 12, 13), 0.35f, 1.0f / 128.0f, 3,
        control, derivatives);
    q.check(fdError < 5e-4, "finite differences with an infinitely sharp neighbor");

    // The infinitely sharp region is itself immovable under the perturbation
    // only where it is decoupled; at minimum the derivative field must stay
    // balanced (row sums zero) and remain finite.
    checkRowSums(q, *table);
    for (double s : derivatives) q.check(std::isfinite(s), "finite derivatives");
    return 0;
}

// Constant control signal: every value equals the constant and every
// sharpness derivative is exactly zero.
static int propagatedConstantSignal(Case& q) {
    Mesh mesh = makeAsymmetricShell();
    addCrease(mesh, 2, 3, 1.40f);
    std::unique_ptr<Far::TopologyRefiner> base = makeRefiner(mesh, 2, false);
    int edge = base->GetLevel(0).FindEdge(2, 3);
    std::unique_ptr<Far::CreaseSensitivityTable const> table(
        Far::CreaseSensitivityTable::Create(*base, 2, edge));

    std::vector<double> control(mesh.nverts, 4.0);
    std::vector<double> values, derivatives;
    table->Evaluate(control, 1, values, derivatives);
    for (double v : values) q.check(std::fabs(v - 4.0) < 1e-10, "constant values preserved");
    for (double d : derivatives) q.check(std::fabs(d) < 1e-12, "constant signal has zero derivative");
    return 0;
}

// ===========================================================================
// Group "ownership": self-contained arrays and argument validation.
// ===========================================================================

static int ownershipSourceRelease(Case& q) {
    Mesh mesh = makeValenceFiveFan();
    addCrease(mesh, 0, 1, 0.35f);
    addCrease(mesh, 1, 2, 0.80f);

    std::vector<double> scalarControl = unitScaleControls(mesh.nverts, 83);
    std::vector<double> control((size_t)mesh.nverts * 2);
    for (int v = 0; v < mesh.nverts; ++v) {
        control[2 * v]     = scalarControl[v];
        control[2 * v + 1] = -scalarControl[v];
    }
    std::unique_ptr<Far::CreaseSensitivityTable const> table;
    int level = 3;
    {
        std::unique_ptr<Far::TopologyRefiner> base = makeRefiner(mesh, level, false);
        int edge = base->GetLevel(0).FindEdge(0, 1);
        table.reset(Far::CreaseSensitivityTable::Create(*base, level, edge));
    } // base refiner released here; the table must stand on its own

    std::vector<double> values, derivatives;
    table->Evaluate(control, 2, values, derivatives);

    std::unique_ptr<Far::TopologyRefiner> refined = makeRefiner(mesh, level, true);
    double positionError = 0.0;
    for (int channel = 0; channel < 2; ++channel) {
        std::vector<double> channelControl(mesh.nverts);
        for (int v = 0; v < mesh.nverts; ++v) channelControl[v] = control[2 * v + channel];
        std::vector<double> channelReference = refinePositions(*refined, channelControl);
        for (int row = 0; row < table->GetNumVertices(); ++row) {
            positionError = std::max(positionError,
                std::fabs(values[2 * row + channel] - channelReference[row]));
        }
    }
    q.check(positionError < 1e-7, "released-source table still evaluates positions");
    q.check(table->GetNumControlVertices() == mesh.nverts, "control vertex count retained");
    checkRowSums(q, *table);
    return 0;
}

static int ownershipArgumentValidation(Case& q) {
    Mesh mesh = makeGrid44();
    addCrease(mesh, 12, 13, 0.35f);
    std::unique_ptr<Far::TopologyRefiner> base = makeRefiner(mesh, 2, false);
    int edge = base->GetLevel(0).FindEdge(12, 13);
    std::unique_ptr<Far::CreaseSensitivityTable const> table(
        Far::CreaseSensitivityTable::Create(*base, 2, edge));

    std::vector<double> good((size_t)mesh.nverts, 0.5);
    std::vector<double> out1, out2;

    auto throwsInvalid = [&](std::vector<double> const& control, size_t channels,
                             std::vector<double>& a, std::vector<double>& b) {
        try {
            table->Evaluate(control, channels, a, b);
            return false;
        } catch (std::invalid_argument const&) {
            return true;
        }
    };

    q.check(throwsInvalid(good, 0, out1, out2), "zero channels rejected");
    std::vector<double> wrongLength((size_t)mesh.nverts - 1, 0.5);
    q.check(throwsInvalid(wrongLength, 1, out1, out2), "wrong control length rejected");
    std::vector<double> nonFinite = good;
    nonFinite[3] = std::numeric_limits<double>::infinity();
    q.check(throwsInvalid(nonFinite, 1, out1, out2), "non-finite control rejected");
    q.check(throwsInvalid(good, 1, out1, out1), "identical output vectors rejected");

    // Valid call still succeeds after the rejected attempts.
    table->Evaluate(good, 1, out1, out2);
    q.check(out1.size() == (size_t)table->GetNumVertices() &&
            out2.size() == out1.size(), "valid evaluation after rejected calls");

    // Unsupported construction inputs are refused with invalid_argument.
    auto rejects = [&](Far::TopologyRefiner const& refiner, int lvl, int e) {
        try {
            Far::CreaseSensitivityTable::Create(refiner, lvl, e);
            return false;
        } catch (std::invalid_argument const&) {
            return true;
        }
    };
    q.check(rejects(*base, 0, edge), "level zero rejected");
    q.check(rejects(*base, 5, edge), "level above four rejected");
    q.check(rejects(*base, 1, 99999), "out-of-range edge rejected");
    int boundaryEdge = base->GetLevel(0).FindEdge(0, 1);
    q.check(rejects(*base, 1, boundaryEdge), "boundary edge rejected");

    Mesh integerMesh = makeGrid44();
    addCrease(integerMesh, 12, 13, 1.0f);
    std::unique_ptr<Far::TopologyRefiner> integerBase = makeRefiner(integerMesh, 1, false);
    int integerEdge = integerBase->GetLevel(0).FindEdge(12, 13);
    q.check(rejects(*integerBase, 1, integerEdge), "integer sharpness rejected");

    std::unique_ptr<Far::TopologyRefiner> refined = makeRefiner(mesh, 1, true);
    q.check(rejects(*refined, 1, edge), "already-refined refiner rejected");
    return 0;
}

// ===========================================================================
// Registry.
// ===========================================================================
void register_task_tests(Registry& registry) {
    registry.add("masks", "edge-point-formula", masksEdgePoint);
    registry.add("masks", "joint-vertex-transition", masksJointVertexTransition);

    registry.add("propagated-sensitivity", "closed-shell", propagatedClosedShell);
    registry.add("propagated-sensitivity", "open-patch", propagatedOpenPatch);
    registry.add("propagated-sensitivity", "irregular-valence", propagatedIrregularValence);
    registry.add("propagated-sensitivity", "exhausted-persistence", propagatedExhaustedPersistence);
    registry.add("propagated-sensitivity", "two-crease-rounds", propagatedTwoCreaseRounds);
    registry.add("propagated-sensitivity", "three-crease-rounds", propagatedThreeCreaseRounds);
    registry.add("propagated-sensitivity", "infinite-neighbor", propagatedInfiniteNeighbor);
    registry.add("propagated-sensitivity", "constant-signal", propagatedConstantSignal);

    registry.add("ownership", "source-release", ownershipSourceRelease);
    registry.add("ownership", "argument-validation", ownershipArgumentValidation);
}
