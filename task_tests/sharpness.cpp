//
//   Regression tests for in-place editing of coarse-level sharpness on
//   Far::TopologyRefiner (UpdateUniformSharpness).
//
//   Groups:
//     rules    -- crease classification and vertex rules, per level, after edits
//     geometry -- interpolation and stencil evaluation vs. independently built refiners
//     views    -- preservation of existing topology views / rejections
//
#include "task_support/check.h"

#include <opensubdiv/far/topologyDescriptor.h>
#include <opensubdiv/far/primvarRefiner.h>
#include <opensubdiv/far/stencilTableFactory.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

using namespace OpenSubdiv;

typedef Far::Index           Index;
typedef Far::ConstIndexArray ConstIndexArray;

namespace {

// ---------------------------------------------------------------------------
//  Minimal mesh description and helpers
// ---------------------------------------------------------------------------

struct Mesh {
    int                     numVertices;
    std::vector<int>        faceVertCounts;
    std::vector<int>        faceVerts;
    std::vector<double>     positions;      // 3 * numVertices
    std::vector<int>        holes;          // faces tagged as holes (optional)
};

typedef Far::TopologyRefiner::SharpnessEdit SharpnessEdit;

//  A point with double coordinates -- geometry is compared to 1e-9.
struct Value {
    double p[3];
    void Clear() { p[0] = p[1] = p[2] = 0.0; }
    void AddWithWeight(Value const & v, double w) {
        for (int k = 0; k < 3; ++k) p[k] += v.p[k] * w;
    }
};

double const GEOM_TOL = 1e-9;

//  A closed, asymmetric quad-dominant shell:  pentagonal prism.  The pentagonal
//  cap faces give the ring vertices an extraordinary valence of 3, and the
//  layout (radii and heights) is deliberately not symmetric.
Mesh makePentagonalPrism() {
    Mesh m;
    m.numVertices = 10;
    m.faceVertCounts = { 5, 5, 4, 4, 4, 4, 4 };
    // bottom cap (0..4), top cap (5..9), sides winding outward
    m.faceVerts = {
        0, 1, 2, 3, 4,
        9, 8, 7, 6, 5,
        0, 5, 6, 1,
        1, 6, 7, 2,
        2, 7, 8, 3,
        3, 8, 9, 4,
        4, 9, 5, 0
    };
    double const radius[5] = { 1.0, 1.13, 1.07, 0.91, 1.19 };
    for (int i = 0; i < 5; ++i) {
        double a = 0.4 + 2.0 * 3.14159265358979323846 * double(i) / 5.0;
        m.positions.push_back(radius[i] * std::cos(a));
        m.positions.push_back(0.37 * std::sin(a));
        m.positions.push_back(-1.0 + 0.21 * double(i));
        m.positions.push_back(0.83 * radius[i] * std::cos(a + 0.11));
        m.positions.push_back(0.53 * std::sin(a) - 0.07 * double(i));
        m.positions.push_back(1.0 + 0.17 * double(i));
    }
    return m;
}

//  A closed, asymmetric shell with a mix of face sizes and extraordinary
//  valences.  A cube with its +z face split into four quads by adding four
//  boundary midpoints and a center: the sides become pentagons, the corners
//  have valence 3, the midpoints valence 3 and the center valence 4.
Mesh makeMixedValenceShell() {
    Mesh m;
    m.numVertices = 13;
    m.faceVertCounts = { 4, 4, 4, 4, 5, 5, 5, 5, 4 };
    m.faceVerts = {
        // split +z face
        4, 8, 12, 11,
        8, 5, 9, 12,
        12, 9, 6, 10,
        11, 12, 10, 7,
        // sides, each a pentagon through the top midpoints
        0, 1, 5, 8, 4,
        1, 2, 6, 9, 5,
        2, 3, 7, 10, 6,
        3, 0, 4, 11, 7,
        // -z bottom
        0, 3, 2, 1
    };
    double const p[13][3] = {
        { -1.00, -1.00, -1.00 }, { 1.00, -0.93, -1.00 },
        { 1.00,  1.00, -0.97 }, { -0.96, 1.00, -1.00 },
        { -1.00, -1.00,  1.00 }, { 1.07, -1.00,  0.91 },
        { 1.00,  1.06,  1.00 }, { -1.00, 1.00,  1.05 },
        { 0.03, -1.00,  0.96 }, { 1.02,  0.02,  0.98 },
        { -0.04, 1.00,  0.99 }, { -0.98, 0.01,  1.03 },
        { 0.02,  0.03,  1.12 }
    };
    for (int i = 0; i < 13; ++i) {
        m.positions.push_back(p[i][0]);
        m.positions.push_back(p[i][1]);
        m.positions.push_back(p[i][2]);
    }
    return m;
}

//  An open boundary quad grid (3x3 vertices / 2x2 faces) with asymmetric
//  placement so that the limit surface is not accidentally symmetric.
Mesh makeOpenQuadGrid() {
    Mesh m;
    m.numVertices = 9;
    m.faceVertCounts = { 4, 4, 4, 4 };
    m.faceVerts = {
        0, 1, 4, 3,
        1, 2, 5, 4,
        3, 4, 7, 6,
        4, 5, 8, 7
    };
    for (int i = 0; i < 9; ++i) {
        int x = i % 3, y = i / 3;
        m.positions.push_back(double(x) + 0.13 * double(y));
        m.positions.push_back(double(y) * 0.87 + 0.09 * double(x * x));
        m.positions.push_back(0.31 * double(x * y) - 0.06 * double(i));
    }
    return m;
}

//  A closed triangle mesh (pentagonal bipyramid) for Loop.  Two valence-5
//  apexes and five ring vertices of valence 4.
Mesh makePentagonalBipyramid() {
    Mesh m;
    m.numVertices = 7;
    m.faceVertCounts = { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3 };
    for (int i = 0; i < 5; ++i) {
        int a = i, b = (i + 1) % 5;
        m.faceVerts.push_back(5); m.faceVerts.push_back(a); m.faceVerts.push_back(b);
    }
    for (int i = 0; i < 5; ++i) {
        int a = i, b = (i + 1) % 5;
        m.faceVerts.push_back(6); m.faceVerts.push_back(b); m.faceVerts.push_back(a);
    }
    double const radius[5] = { 1.0, 1.21, 0.93, 1.11, 1.05 };
    for (int i = 0; i < 5; ++i) {
        double a = 0.3 + 2.0 * 3.14159265358979323846 * double(i) / 5.0;
        m.positions.push_back(radius[i] * std::cos(a));
        m.positions.push_back(radius[i] * std::sin(a));
        m.positions.push_back(0.12 * double(i) - 0.2);
    }
    m.positions.push_back(0.0); m.positions.push_back(0.0); m.positions.push_back(1.17);
    m.positions.push_back(-0.1); m.positions.push_back(0.05); m.positions.push_back(-1.23);
    return m;
}

//  An open triangle grid (2x2 quads split into 8 triangles), for Loop with
//  boundary.
Mesh makeOpenTriGrid() {
    Mesh m;
    m.numVertices = 9;
    m.faceVertCounts.assign(8, 3);
    int const tris[8][3] = {
        {0,1,4}, {0,4,3}, {1,2,5}, {1,5,4},
        {3,4,7}, {3,7,6}, {4,5,8}, {4,8,7}
    };
    for (int t = 0; t < 8; ++t)
        for (int k = 0; k < 3; ++k) m.faceVerts.push_back(tris[t][k]);
    for (int i = 0; i < 9; ++i) {
        int x = i % 3, y = i / 3;
        m.positions.push_back(double(x) + 0.17 * double(y * y));
        m.positions.push_back(double(y) * 0.79 + 0.11 * double(x));
        m.positions.push_back(0.23 * double(x * y) + 0.05 * double(i));
    }
    return m;
}

struct Setup {
    Sdc::SchemeType      scheme;
    Sdc::Options::CreasingMethod method;
    Sdc::Options::VtxBoundaryInterpolation boundary;
    int                  levels;
    bool                 fullTopologyInLastLevel;
    bool                 adaptive;
    bool                 addHoles;
    bool                 addFVar;
    bool                 sharedBase;

    Setup() : scheme(Sdc::SCHEME_CATMARK), method(Sdc::Options::CREASE_UNIFORM),
              boundary(Sdc::Options::VTX_BOUNDARY_EDGE_ONLY), levels(3),
              fullTopologyInLastLevel(true), adaptive(false), addHoles(false),
              addFVar(false), sharedBase(false) { }
};

Sdc::Options makeOptions(Setup const & s) {
    Sdc::Options opt;
    opt.SetVtxBoundaryInterpolation(s.boundary);
    opt.SetCreasingMethod(s.method);
    return opt;
}

//  Create a refiner for the given mesh.  When edgeSharpness/vertexSharpness are
//  non-null they are absolute level-0 sharpness values indexed by edge/vertex.
//  Edge indexing is obtained from a probe refiner so that the caller does not
//  have to know Vtr's edge ordering.
std::unique_ptr<Far::TopologyRefiner> createRefiner(
        Mesh const & mesh, Setup const & setup,
        float const * edgeSharpness = 0, float const * vertexSharpness = 0,
        std::unique_ptr<Far::TopologyRefiner> * baseOwner = 0) {

    Far::TopologyDescriptor desc;
    desc.numVertices = mesh.numVertices;
    desc.numFaces    = (int) mesh.faceVertCounts.size();
    desc.numVertsPerFace  = mesh.faceVertCounts.data();
    desc.vertIndicesPerFace = mesh.faceVerts.data();

    Sdc::Options opt = makeOptions(setup);

    std::vector<int> edgePairs;
    std::vector<float> edgeWeights;
    if (edgeSharpness) {
        // Probe: same construction without creases, to recover edge vertex pairs.
        Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Options po(setup.scheme, opt);
        std::unique_ptr<Far::TopologyRefiner> probe(
            Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Create(desc, po));
        Far::TopologyLevel const & probeLevel = probe->GetLevel(0);
        for (int e = 0; e < probeLevel.GetNumEdges(); ++e) {
            ConstIndexArray ev = probeLevel.GetEdgeVertices(e);
            edgePairs.push_back(ev[0]);
            edgePairs.push_back(ev[1]);
            edgeWeights.push_back(edgeSharpness[e]);
        }
        desc.numCreases             = (int) edgeWeights.size();
        desc.creaseVertexIndexPairs = edgePairs.data();
        desc.creaseWeights          = edgeWeights.data();
    }
    std::vector<int>   cornerIndices;
    std::vector<float> cornerWeights;
    if (vertexSharpness) {
        for (int v = 0; v < mesh.numVertices; ++v) {
            cornerIndices.push_back(v);
            cornerWeights.push_back(vertexSharpness[v]);
        }
        desc.numCorners        = (int) cornerIndices.size();
        desc.cornerVertexIndices = cornerIndices.data();
        desc.cornerWeights     = cornerWeights.data();
    }
    std::vector<int> holes;
    if (setup.addHoles) {
        holes = mesh.holes;
        if (holes.empty()) holes.push_back(0);
        desc.numHoles   = (int) holes.size();
        desc.holeIndices = holes.data();
    }
    std::vector<int> fvarIndices;
    Far::TopologyDescriptor::FVarChannel fvar;
    if (setup.addFVar) {
        fvarIndices.resize(mesh.faceVerts.size());
        for (size_t i = 0; i < fvarIndices.size(); ++i) fvarIndices[i] = mesh.faceVerts[i];
        fvar.numValues   = mesh.numVertices;
        fvar.valueIndices = fvarIndices.data();
        desc.numFVarChannels = 1;
        desc.fvarChannels    = &fvar;
    }

    Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Options fo(setup.scheme, opt);
    fo.validateFullTopology = true;

    std::unique_ptr<Far::TopologyRefiner> refiner(
        Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Create(desc, fo));

    if (setup.sharedBase) {
        //  The child only borrows the base level, so the owner must outlive it.
        //  Hand ownership of the owner back through the caller-provided slot.
        std::unique_ptr<Far::TopologyRefiner> child(
            Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Create(*refiner));
        if (baseOwner) *baseOwner = std::move(refiner);
        return child;
    }

    if (setup.adaptive) {
        Far::TopologyRefiner::AdaptiveOptions ao(setup.levels);
        refiner->RefineAdaptive(ao);
    } else {
        Far::TopologyRefiner::UniformOptions uo(setup.levels);
        uo.fullTopologyInLastLevel = setup.fullTopologyInLastLevel;
        refiner->RefineUniform(uo);
    }
    return refiner;
}

//  Snapshot the classification of every level (counts, sharpness, inf/semi and
//  vertex rules).  Used both for comparison and for "rejected edits change
//  nothing" checks.
struct Snapshot {
    struct LevelData {
        int numVertices, numEdges, numFaces;
        std::vector<float> edgeSharpness, vertSharpness;
        std::vector<int> edgeInf, edgeSemi, vertInf, vertSemi, rules;
    };
    std::vector<LevelData> levels;
};

Snapshot snapshot(Far::TopologyRefiner const & refiner) {
    Snapshot s;
    for (int l = 0; l <= refiner.GetMaxLevel(); ++l) {
        Far::TopologyLevel const & lv = refiner.GetLevel(l);
        Snapshot::LevelData d;
        d.numVertices = lv.GetNumVertices();
        d.numEdges    = lv.GetNumEdges();
        d.numFaces    = lv.GetNumFaces();
        for (int e = 0; e < d.numEdges; ++e) {
            d.edgeSharpness.push_back(lv.GetEdgeSharpness(e));
            d.edgeInf.push_back(lv.IsEdgeInfSharp(e) ? 1 : 0);
            d.edgeSemi.push_back(lv.IsEdgeSemiSharp(e) ? 1 : 0);
        }
        for (int v = 0; v < d.numVertices; ++v) {
            d.vertSharpness.push_back(lv.GetVertexSharpness(v));
            d.vertInf.push_back(lv.IsVertexInfSharp(v) ? 1 : 0);
            d.vertSemi.push_back(lv.IsVertexSemiSharp(v) ? 1 : 0);
            d.rules.push_back((int) lv.GetVertexRule(v));
        }
        s.levels.push_back(d);
    }
    return s;
}

bool snapshotsEqual(Snapshot const & a, Snapshot const & b) {
    if (a.levels.size() != b.levels.size()) return false;
    for (size_t l = 0; l < a.levels.size(); ++l) {
        Snapshot::LevelData const & x = a.levels[l];
        Snapshot::LevelData const & y = b.levels[l];
        if (x.numVertices != y.numVertices || x.numEdges != y.numEdges ||
            x.numFaces != y.numFaces) return false;
        if (x.edgeSharpness != y.edgeSharpness) return false;
        if (x.vertSharpness != y.vertSharpness) return false;
        if (x.edgeInf != y.edgeInf || x.edgeSemi != y.edgeSemi) return false;
        if (x.vertInf != y.vertInf || x.vertSemi != y.vertSemi) return false;
        if (x.rules != y.rules) return false;
    }
    return true;
}

//  Compare the classification of two refiners level by level.
bool classificationsEqual(Far::TopologyRefiner const & a,
                          Far::TopologyRefiner const & b) {
    return snapshotsEqual(snapshot(a), snapshot(b));
}

//  Build an independent refiner whose base-level sharpness matches the current
//  state of the given refiner.
std::unique_ptr<Far::TopologyRefiner> rebuildFromCurrent(
        Mesh const & mesh, Setup const & setup,
        Far::TopologyRefiner const & edited) {
    Far::TopologyLevel const & lv = edited.GetLevel(0);
    std::vector<float> es(lv.GetNumEdges());
    std::vector<float> vs(lv.GetNumVertices());
    for (int e = 0; e < lv.GetNumEdges(); ++e) es[e] = lv.GetEdgeSharpness(e);
    for (int v = 0; v < lv.GetNumVertices(); ++v) vs[v] = lv.GetVertexSharpness(v);
    return createRefiner(mesh, setup, es.data(), vs.data());
}

//  Interpolate positions from the base level through every level.
std::vector<std::vector<Value> > interpolateLevels(
        Far::TopologyRefiner const & refiner, Mesh const & mesh) {
    std::vector<std::vector<Value> > result;
    std::vector<Value> base(mesh.numVertices);
    for (int i = 0; i < mesh.numVertices; ++i) {
        base[i].p[0] = mesh.positions[3*i + 0];
        base[i].p[1] = mesh.positions[3*i + 1];
        base[i].p[2] = mesh.positions[3*i + 2];
    }
    std::vector<Value> current = base;
    result.push_back(current);
    Far::PrimvarRefinerReal<double> prim(refiner);
    for (int l = 1; l <= refiner.GetMaxLevel(); ++l) {
        std::vector<Value> next(refiner.GetLevel(l).GetNumVertices());
        prim.Interpolate(l, current, next);
        current.swap(next);
        result.push_back(current);
    }
    return result;
}

double maxGeometryDelta(std::vector<std::vector<Value> > const & a,
                        std::vector<std::vector<Value> > const & b) {
    double delta = 0.0;
    for (size_t l = 0; l < a.size() && l < b.size(); ++l) {
        if (a[l].size() != b[l].size()) return 1e30;
        for (size_t i = 0; i < a[l].size(); ++i)
            for (int k = 0; k < 3; ++k)
                delta = std::fmax(delta, std::fabs(a[l][i].p[k] - b[l][i].p[k]));
    }
    return delta;
}

//  A simple fingerprint of all positions of the final level.
std::vector<double> finalPositions(Far::TopologyRefiner const & refiner,
                                   Mesh const & mesh) {
    std::vector<std::vector<Value> > all = interpolateLevels(refiner, mesh);
    std::vector<double> out;
    for (size_t i = 0; i < all.back().size(); ++i)
        for (int k = 0; k < 3; ++k) out.push_back(all.back()[i].p[k]);
    return out;
}

bool sameFloats(std::vector<double> const & a, std::vector<double> const & b,
                double tol) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::fabs(a[i] - b[i]) > tol) return false;
    return true;
}

bool anyDifference(std::vector<double> const & a, std::vector<double> const & b) {
    if (a.size() != b.size()) return true;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::fabs(a[i] - b[i]) > 1e-6) return true;
    return false;
}

//  Verify the edited refiner against an independently rebuilt one: topology
//  inventory, classification and geometry.  All levels are inspected.
bool verifyAgainstRebuild(Mesh const & mesh, Setup const & setup,
                          Far::TopologyRefiner const & edited) {
    std::unique_ptr<Far::TopologyRefiner> reference(
        rebuildFromCurrent(mesh, setup, edited));

    // Level inventory and classification (sharpness, inf/semi flags, rules).
    if (edited.GetMaxLevel() != reference->GetMaxLevel()) return false;
    for (int l = 0; l <= edited.GetMaxLevel(); ++l) {
        if (edited.GetLevel(l).GetNumVertices() != reference->GetLevel(l).GetNumVertices()) return false;
        if (edited.GetLevel(l).GetNumEdges()    != reference->GetLevel(l).GetNumEdges())    return false;
        if (edited.GetLevel(l).GetNumFaces()    != reference->GetLevel(l).GetNumFaces())    return false;
    }
    if (!classificationsEqual(edited, *reference)) return false;

    // Geometry via PrimvarRefiner on every level.
    double delta = maxGeometryDelta(interpolateLevels(edited, mesh),
                                    interpolateLevels(*reference, mesh));
    if (delta > GEOM_TOL) {
        std::printf("geometry delta %.3g exceeds tolerance\n", delta);
        return false;
    }
    return true;
}

//  Build the stencil table matching the final level from a refiner.
std::unique_ptr<const Far::StencilTableReal<double> > makeFinalStencilTable(
        Far::TopologyRefiner const & refiner) {
    Far::StencilTableFactoryReal<double>::Options so;
    so.generateIntermediateLevels = false;
    so.generateOffsets = true;
    return std::unique_ptr<const Far::StencilTableReal<double> >(
        Far::StencilTableFactoryReal<double>::Create(refiner, so));
}

std::vector<Value> stencilEvaluate(
        Far::StencilTableReal<double> const & table, Mesh const & mesh) {
    std::vector<Value> base(mesh.numVertices);
    for (int i = 0; i < mesh.numVertices; ++i) {
        base[i].p[0] = mesh.positions[3*i + 0];
        base[i].p[1] = mesh.positions[3*i + 1];
        base[i].p[2] = mesh.positions[3*i + 2];
    }
    std::vector<Value> out(table.GetNumStencils());
    table.UpdateValues(base, out);
    return out;
}

// ---------------------------------------------------------------------------
//  Edits used by the tests
// ---------------------------------------------------------------------------

//  A pseudo-random but deterministic spread of absolute values including
//  smooth, fractional, integer and infinite sharpness.
float spreadValue(int i) {
    static const float values[9] = { 0.0f, 0.25f, 0.5f, 1.0f, 1.75f, 2.0f, 3.25f, 6.5f, 10.0f };
    return values[(i * 5 + 3) % 9];
}

void buildSpreadEdits(Far::TopologyRefiner const & refiner,
                      std::vector<SharpnessEdit> & edges,
                      std::vector<SharpnessEdit> & vertices) {
    Far::TopologyLevel const & lv = refiner.GetLevel(0);
    edges.clear();
    vertices.clear();
    for (int e = 0; e < lv.GetNumEdges(); ++e)
        edges.push_back(SharpnessEdit(e, spreadValue(e)));
    for (int v = 0; v < lv.GetNumVertices(); ++v)
        vertices.push_back(SharpnessEdit(v, spreadValue(v + 4)));
}

//  Assign strictly unequal strengths to the edges incident a vertex -- a
//  genuine chain of adjacent creases of differing strength.  Under Chaikin a
//  child edge depends on the average of the other semi-sharp edges around the
//  end vertex, so this distinguishes Chaikin from a per-edge decrement.
void buildUnevenChain(Mesh const & mesh, Far::TopologyLevel const & lv,
                      std::vector<SharpnessEdit> & edges,
                      std::vector<SharpnessEdit> & vertices) {
    edges.clear();
    vertices.clear();
    (void) mesh;

    //  Find an interior vertex with at least four incident edges.
    int center = -1;
    for (int v = 0; v < lv.GetNumVertices(); ++v) {
        ConstIndexArray ve = lv.GetVertexEdges(v);
        if (!lv.IsVertexBoundary(v) && ve.size() >= 4) { center = v; break; }
    }
    if (center < 0) {   // fall back to any vertex with enough edges
        for (int v = 0; v < lv.GetNumVertices(); ++v)
            if (lv.GetVertexEdges(v).size() >= 2) { center = v; break; }
    }
    ConstIndexArray fan = lv.GetVertexEdges(center);
    float const values[4] = { 1.0f, 2.0f, 4.0f, 10.0f };
    for (int i = 0; i < fan.size(); ++i)
        edges.push_back(SharpnessEdit(fan[i], values[i % 4]));
    vertices.push_back(SharpnessEdit(center, 0.0f));
}

// ---------------------------------------------------------------------------
//  Rules group
// ---------------------------------------------------------------------------

int testRulesCatmarkClosedUniform() {
    Mesh mesh = makePentagonalPrism();
    Setup setup;
    setup.scheme = Sdc::SCHEME_CATMARK;
    setup.method = Sdc::Options::CREASE_UNIFORM;
    setup.boundary = Sdc::Options::VTX_BOUNDARY_EDGE_ONLY;
    setup.levels = 3;

    std::unique_ptr<Far::TopologyRefiner> refiner = createRefiner(mesh, setup);

    std::vector<SharpnessEdit> edges, vertices;
    buildSpreadEdits(*refiner, edges, vertices);
    TASK_CHECK(refiner->UpdateUniformSharpness(edges, vertices));
    TASK_CHECK(verifyAgainstRebuild(mesh, setup, *refiner));
    return 0;
}

int testRulesCatmarkOpenChaikin() {
    Mesh mesh = makeOpenQuadGrid();
    Setup setup;
    setup.scheme = Sdc::SCHEME_CATMARK;
    setup.method = Sdc::Options::CREASE_CHAIKIN;
    setup.boundary = Sdc::Options::VTX_BOUNDARY_EDGE_ONLY;
    setup.levels = 3;

    std::unique_ptr<Far::TopologyRefiner> refiner = createRefiner(mesh, setup);

    std::vector<SharpnessEdit> edges, vertices;
    buildUnevenChain(mesh, refiner->GetLevel(0), edges, vertices);
    TASK_CHECK(refiner->UpdateUniformSharpness(edges, vertices));
    TASK_CHECK(verifyAgainstRebuild(mesh, setup, *refiner));
    return 0;
}

int testRulesCatmarkOpenCornerChaikin() {
    Mesh mesh = makeOpenQuadGrid();
    Setup setup;
    setup.method = Sdc::Options::CREASE_CHAIKIN;
    setup.boundary = Sdc::Options::VTX_BOUNDARY_EDGE_AND_CORNER;
    setup.levels = 2;

    std::unique_ptr<Far::TopologyRefiner> refiner = createRefiner(mesh, setup);

    std::vector<SharpnessEdit> edges, vertices;
    buildSpreadEdits(*refiner, edges, vertices);
    TASK_CHECK(refiner->UpdateUniformSharpness(edges, vertices));
    TASK_CHECK(verifyAgainstRebuild(mesh, setup, *refiner));
    return 0;
}

int testRulesLoopClosedUniform() {
    Mesh mesh = makePentagonalBipyramid();
    Setup setup;
    setup.scheme = Sdc::SCHEME_LOOP;
    setup.method = Sdc::Options::CREASE_UNIFORM;
    setup.boundary = Sdc::Options::VTX_BOUNDARY_EDGE_ONLY;
    setup.levels = 3;

    std::unique_ptr<Far::TopologyRefiner> refiner = createRefiner(mesh, setup);

    std::vector<SharpnessEdit> edges, vertices;
    buildSpreadEdits(*refiner, edges, vertices);
    TASK_CHECK(refiner->UpdateUniformSharpness(edges, vertices));
    TASK_CHECK(verifyAgainstRebuild(mesh, setup, *refiner));
    return 0;
}

int testRulesLoopOpenCornerChaikin() {
    Mesh mesh = makeOpenTriGrid();
    Setup setup;
    setup.scheme = Sdc::SCHEME_LOOP;
    setup.method = Sdc::Options::CREASE_CHAIKIN;
    setup.boundary = Sdc::Options::VTX_BOUNDARY_EDGE_AND_CORNER;
    setup.levels = 2;

    std::unique_ptr<Far::TopologyRefiner> refiner = createRefiner(mesh, setup);

    std::vector<SharpnessEdit> edges, vertices;
    buildSpreadEdits(*refiner, edges, vertices);
    TASK_CHECK(refiner->UpdateUniformSharpness(edges, vertices));
    TASK_CHECK(verifyAgainstRebuild(mesh, setup, *refiner));
    return 0;
}

//  Several sharp edges of differing strength meeting at a vertex: the vertex
//  rule must change from smooth to crease/corner and back as strengths change.
int testRulesVertexConvergence() {
    for (int method = 0; method < 2; ++method) {
        Mesh mesh = makeOpenQuadGrid();
        Setup setup;
        setup.scheme = Sdc::SCHEME_CATMARK;
        setup.method = method ? Sdc::Options::CREASE_CHAIKIN
                              : Sdc::Options::CREASE_UNIFORM;
        setup.boundary = Sdc::Options::VTX_BOUNDARY_EDGE_ONLY;
        setup.levels = 3;

        std::unique_ptr<Far::TopologyRefiner> refiner = createRefiner(mesh, setup);
        Far::TopologyLevel const & lv = refiner->GetLevel(0);

        //  Vertex 4 (the center) has four incident interior edges.  Make two of
        //  them sharp (crease) then a third (corner) then none (smooth).
        int centerEdges[4] = { -1, -1, -1, -1 };
        ConstIndexArray vEdges = lv.GetVertexEdges(4);
        for (int i = 0; i < vEdges.size() && i < 4; ++i) centerEdges[i] = vEdges[i];

        Sdc::Crease::Rule expected[3];
        expected[0] = Sdc::Crease::RULE_SMOOTH;
        expected[1] = Sdc::Crease::RULE_CREASE;
        expected[2] = Sdc::Crease::RULE_SMOOTH;

        std::vector<SharpnessEdit> edges, vertices;
        vertices.push_back(SharpnessEdit(4, 0.0f));
        edges.push_back(SharpnessEdit(centerEdges[0], 2.0f));
        edges.push_back(SharpnessEdit(centerEdges[1], 3.0f));
        TASK_CHECK(refiner->UpdateUniformSharpness(edges, vertices));
        TASK_CHECK(refiner->GetLevel(0).GetVertexRule(4) == expected[1]);
        TASK_CHECK(verifyAgainstRebuild(mesh, setup, *refiner));

        edges.push_back(SharpnessEdit(centerEdges[2], 1.5f));
        TASK_CHECK(refiner->UpdateUniformSharpness(edges, vertices));
        TASK_CHECK(refiner->GetLevel(0).GetVertexRule(4) == Sdc::Crease::RULE_CORNER);
        TASK_CHECK(verifyAgainstRebuild(mesh, setup, *refiner));

        //  Clear all of them and return to smooth.
        for (size_t i = 0; i < edges.size(); ++i) edges[i].sharpness = 0.0f;
        TASK_CHECK(refiner->UpdateUniformSharpness(edges, vertices));
        TASK_CHECK(refiner->GetLevel(0).GetVertexRule(4) == expected[2]);
        TASK_CHECK(verifyAgainstRebuild(mesh, setup, *refiner));
    }
    return 0;
}

//  Sharpness chains that exhaust at different levels: 1.0 decays after one
//  level, 3.0 after three, infinity never.  Check per level against a rebuild.
int testRulesLevelExhaustion() {
    for (int method = 0; method < 2; ++method) {
        Mesh mesh = makeOpenQuadGrid();
        Setup setup;
        setup.method = method ? Sdc::Options::CREASE_CHAIKIN
                              : Sdc::Options::CREASE_UNIFORM;
        setup.boundary = Sdc::Options::VTX_BOUNDARY_EDGE_ONLY;
        setup.levels = 4;

        std::unique_ptr<Far::TopologyRefiner> refiner = createRefiner(mesh, setup);
        Far::TopologyLevel const & lv = refiner->GetLevel(0);

        //  Pick a chain of interior edges around the center: alternating values.
        float values[4] = { 1.0f, 3.0f, 10.0f, 2.0f };
        std::vector<SharpnessEdit> edges, vertices;
        ConstIndexArray centerEdges = lv.GetVertexEdges(4);
        for (int i = 0; i < centerEdges.size(); ++i)
            edges.push_back(SharpnessEdit(centerEdges[i], values[i % 4]));
        vertices.push_back(SharpnessEdit(4, 4.0f));

        TASK_CHECK(refiner->UpdateUniformSharpness(edges, vertices));
        TASK_CHECK(verifyAgainstRebuild(mesh, setup, *refiner));

        //  Verify that the infinite crease survives, while finite ones decay.
        bool sawInfiniteAtLast = false;
        for (int e = 0; e < refiner->GetLevel(setup.levels).GetNumEdges(); ++e)
            if (refiner->GetLevel(setup.levels).IsEdgeInfSharp(e)) sawInfiniteAtLast = true;
        TASK_CHECK(sawInfiniteAtLast);

        //  Now lower the infinite crease to smooth and the chains to zero.
        for (size_t i = 0; i < edges.size(); ++i) edges[i].sharpness = 0.0f;
        vertices[0].sharpness = 0.0f;
        TASK_CHECK(refiner->UpdateUniformSharpness(edges, vertices));
        TASK_CHECK(verifyAgainstRebuild(mesh, setup, *refiner));
        return 0;
    }
    return 0;
}

//  Repeated edits accumulate from the last successful state: smooth -> half ->
//  infinite -> smooth, checking each step.
int testRulesCumulativeEdits() {
    for (int method = 0; method < 2; ++method) {
        Mesh mesh = makePentagonalPrism();
        Setup setup;
        setup.method = method ? Sdc::Options::CREASE_CHAIKIN
                              : Sdc::Options::CREASE_UNIFORM;
        setup.levels = 2;

        std::unique_ptr<Far::TopologyRefiner> refiner = createRefiner(mesh, setup);
        Far::TopologyLevel const & lv = refiner->GetLevel(0);
        int edge = -1;
        for (int e = 0; e < lv.GetNumEdges(); ++e) {
            if (lv.GetEdgeFaces(e).size() == 2) { edge = e; break; }
        }
        TASK_CHECK(edge >= 0);

        std::vector<SharpnessEdit> edges, vertices;
        edges.push_back(SharpnessEdit(edge, 0.5f));
        TASK_CHECK(refiner->UpdateUniformSharpness(edges, vertices));
        TASK_CHECK(refiner->GetLevel(0).GetEdgeSharpness(edge) == 0.5f);
        TASK_CHECK(verifyAgainstRebuild(mesh, setup, *refiner));

        edges[0].sharpness = 10.0f;
        TASK_CHECK(refiner->UpdateUniformSharpness(edges, vertices));
        TASK_CHECK(refiner->GetLevel(0).IsEdgeInfSharp(edge));
        TASK_CHECK(verifyAgainstRebuild(mesh, setup, *refiner));

        edges[0].sharpness = 0.0f;
        TASK_CHECK(refiner->UpdateUniformSharpness(edges, vertices));
        TASK_CHECK(!refiner->GetLevel(0).IsEdgeInfSharp(edge));
        TASK_CHECK(refiner->GetLevel(0).GetEdgeSharpness(edge) == 0.0f);
        TASK_CHECK(verifyAgainstRebuild(mesh, setup, *refiner));
    }
    return 0;
}

int testRulesMixedValenceClosed() {
    for (int method = 0; method < 2; ++method) {
        Mesh mesh = makeMixedValenceShell();
        Setup setup;
        setup.method = method ? Sdc::Options::CREASE_CHAIKIN
                              : Sdc::Options::CREASE_UNIFORM;
        setup.boundary = Sdc::Options::VTX_BOUNDARY_EDGE_ONLY;
        setup.levels = 3;

        std::unique_ptr<Far::TopologyRefiner> refiner = createRefiner(mesh, setup);
        TASK_CHECK(refiner->GetLevel(0).GetNumEdges() > 0);

        std::vector<SharpnessEdit> edges, vertices;
        buildSpreadEdits(*refiner, edges, vertices);
        TASK_CHECK(refiner->UpdateUniformSharpness(edges, vertices));
        TASK_CHECK(verifyAgainstRebuild(mesh, setup, *refiner));
    }
    return 0;
}

//  Adjacent unequal creases must subdivide differently for the two creasing
//  methods, and each result must match a refiner built independently with the
//  same final strengths and method.
int testRulesChaikinNotPerEdgeDecrement() {
    Mesh mesh = makeMixedValenceShell();

    std::vector<double> results[2];
    for (int method = 0; method < 2; ++method) {
        Setup setup;
        setup.method = method ? Sdc::Options::CREASE_CHAIKIN
                              : Sdc::Options::CREASE_UNIFORM;
        setup.levels = 3;
        std::unique_ptr<Far::TopologyRefiner> refiner = createRefiner(mesh, setup);

        std::vector<SharpnessEdit> edges, vertices;
        buildUnevenChain(mesh, refiner->GetLevel(0), edges, vertices);
        TASK_CHECK(refiner->UpdateUniformSharpness(edges, vertices));
        TASK_CHECK(verifyAgainstRebuild(mesh, setup, *refiner));
        results[method] = finalPositions(*refiner, mesh);
    }
    TASK_CHECK(anyDifference(results[0], results[1]));
    return 0;
}

//  Empty lists succeed (and change nothing).
int testRulesEmptyLists() {
    Mesh mesh = makeOpenQuadGrid();
    Setup setup;
    setup.levels = 2;

    std::unique_ptr<Far::TopologyRefiner> refiner = createRefiner(mesh, setup);
    Snapshot before = snapshot(*refiner);

    std::vector<SharpnessEdit> empty;
    TASK_CHECK(refiner->UpdateUniformSharpness(empty, empty));
    TASK_CHECK(snapshotsEqual(before, snapshot(*refiner)));
    return 0;
}

// ---------------------------------------------------------------------------
//  Geometry group
// ---------------------------------------------------------------------------

int testGeometryCatmarkUniform() {
    Mesh mesh = makePentagonalPrism();
    Setup setup;
    setup.method = Sdc::Options::CREASE_UNIFORM;
    setup.levels = 3;

    std::unique_ptr<Far::TopologyRefiner> refiner = createRefiner(mesh, setup);
    std::vector<double> before = finalPositions(*refiner, mesh);

    std::vector<SharpnessEdit> edges, vertices;
    buildSpreadEdits(*refiner, edges, vertices);
    TASK_CHECK(refiner->UpdateUniformSharpness(edges, vertices));
    TASK_CHECK(verifyAgainstRebuild(mesh, setup, *refiner));
    TASK_CHECK(anyDifference(before, finalPositions(*refiner, mesh)));
    return 0;
}

int testGeometryCatmarkChaikinOpen() {
    Mesh mesh = makeOpenQuadGrid();
    Setup setup;
    setup.method = Sdc::Options::CREASE_CHAIKIN;
    setup.boundary = Sdc::Options::VTX_BOUNDARY_EDGE_AND_CORNER;
    setup.levels = 3;

    std::unique_ptr<Far::TopologyRefiner> refiner = createRefiner(mesh, setup);

    std::vector<SharpnessEdit> edges, vertices;
    buildUnevenChain(mesh, refiner->GetLevel(0), edges, vertices);
    TASK_CHECK(refiner->UpdateUniformSharpness(edges, vertices));
    TASK_CHECK(verifyAgainstRebuild(mesh, setup, *refiner));

    //  Build a stencil table after the edit and compare it with direct
    //  interpolation and with the reference refiner's table.
    std::unique_ptr<const Far::StencilTableReal<double> > table =
        makeFinalStencilTable(*refiner);
    TASK_CHECK(table && table->GetNumStencils() ==
        refiner->GetLevel(refiner->GetMaxLevel()).GetNumVertices());

    std::vector<Value> evaluated = stencilEvaluate(*table, mesh);
    std::vector<std::vector<Value> > direct = interpolateLevels(*refiner, mesh);
    std::vector<Value> const & last = direct.back();
    TASK_CHECK(evaluated.size() == last.size());

    double delta = 0.0;
    for (size_t i = 0; i < evaluated.size(); ++i)
        for (int k = 0; k < 3; ++k)
            delta = std::fmax(delta, std::fabs(evaluated[i].p[k] - last[i].p[k]));
    TASK_CHECK(delta <= GEOM_TOL);

    std::unique_ptr<Far::TopologyRefiner> reference(
        rebuildFromCurrent(mesh, setup, *refiner));
    std::unique_ptr<const Far::StencilTableReal<double> > refTable =
        makeFinalStencilTable(*reference);
    TASK_CHECK(refTable && refTable->GetNumStencils() == table->GetNumStencils());
    TASK_CHECK(refTable->GetSizes() == table->GetSizes());
    TASK_CHECK(refTable->GetControlIndices() == table->GetControlIndices());
    std::vector<double> a = refTable->GetWeights(), b = table->GetWeights();
    TASK_CHECK(sameFloats(a, b, 0.0));
    return 0;
}

int testGeometryLoop() {
    Mesh mesh = makePentagonalBipyramid();
    Setup setup;
    setup.scheme = Sdc::SCHEME_LOOP;
    setup.levels = 3;

    std::unique_ptr<Far::TopologyRefiner> refiner = createRefiner(mesh, setup);
    std::vector<SharpnessEdit> edges, vertices;
    buildSpreadEdits(*refiner, edges, vertices);
    TASK_CHECK(refiner->UpdateUniformSharpness(edges, vertices));
    TASK_CHECK(verifyAgainstRebuild(mesh, setup, *refiner));
    return 0;
}

//  A StencilTable created before the edit keeps its meaning; a table created
//  afterwards reflects the new shape.
int testGeometryOldStencilPreserved() {
    Mesh mesh = makePentagonalPrism();
    Setup setup;
    setup.levels = 2;

    std::unique_ptr<Far::TopologyRefiner> refiner = createRefiner(mesh, setup);

    //  Create a table on the smooth refiner and snapshot its evaluation.
    std::unique_ptr<const Far::StencilTableReal<double> > oldTable =
        makeFinalStencilTable(*refiner);
    std::vector<Value> oldValues = stencilEvaluate(*oldTable, mesh);

    std::vector<SharpnessEdit> edges, vertices;
    Far::TopologyLevel const & lv = refiner->GetLevel(0);
    for (int e = 0; e < lv.GetNumEdges(); ++e)
        edges.push_back(SharpnessEdit(e, 6.0f));
    for (int v = 0; v < lv.GetNumVertices(); ++v)
        vertices.push_back(SharpnessEdit(v, 10.0f));
    TASK_CHECK(refiner->UpdateUniformSharpness(edges, vertices));

    //  The old table still produces exactly the old values.
    std::vector<Value> reevaluated = stencilEvaluate(*oldTable, mesh);
    bool same = reevaluated.size() == oldValues.size();
    for (size_t i = 0; same && i < reevaluated.size(); ++i)
        for (int k = 0; k < 3; ++k)
            same = std::fabs(reevaluated[i].p[k] - oldValues[i].p[k]) <= 0.0;
    TASK_CHECK(same);

    //  A new table matches the new direct interpolation and differs from old.
    std::unique_ptr<const Far::StencilTableReal<double> > newTable =
        makeFinalStencilTable(*refiner);
    std::vector<Value> newValues = stencilEvaluate(*newTable, mesh);
    std::vector<std::vector<Value> > all = interpolateLevels(*refiner, mesh);
    std::vector<Value> const & direct = all.back();
    double delta = 0.0;
    for (size_t i = 0; i < newValues.size(); ++i)
        for (int k = 0; k < 3; ++k)
            delta = std::fmax(delta, std::fabs(newValues[i].p[k] - direct[i].p[k]));
    TASK_CHECK(delta <= GEOM_TOL);

    double change = 0.0;
    for (size_t i = 0; i < newValues.size(); ++i)
        for (int k = 0; k < 3; ++k)
            change = std::fmax(change, std::fabs(newValues[i].p[k] - oldValues[i].p[k]));
    TASK_CHECK(change > 1e-3);
    return 0;
}

// ---------------------------------------------------------------------------
//  Views group
// ---------------------------------------------------------------------------

//  Capture topological relation arrays (contents and storage addresses), parent
//  and child mappings and level object identity before an edit.
struct ViewSnapshot {
    struct Level {
        int numVertices, numEdges, numFaces, numFaceVertices;
        Far::TopologyLevel const * levelPtr;
        std::vector<Index const *> arrayPointers;
        std::vector<std::vector<int> > arrays;
        std::vector<int> childFaceVerts, childEdgeVerts, childVertVerts;
        std::vector<int> faceChildFaces, faceChildEdges, edgeChildEdges;
        std::vector<int> faceParentFace;
    };
    std::vector<Level> levels;
};

void appendArray(ViewSnapshot::Level & lvl, ConstIndexArray a) {
    lvl.arrayPointers.push_back(a.size() > 0 ? a.begin() : (Index const *) 0);
    lvl.arrays.push_back(std::vector<int>(a.begin(), a.end()));
}

ViewSnapshot captureViews(Far::TopologyRefiner const & refiner) {
    ViewSnapshot s;
    for (int l = 0; l <= refiner.GetMaxLevel(); ++l) {
        Far::TopologyLevel const & lv = refiner.GetLevel(l);
        ViewSnapshot::Level d;
        d.levelPtr = &lv;
        d.numVertices = lv.GetNumVertices();
        d.numEdges = lv.GetNumEdges();
        d.numFaces = lv.GetNumFaces();
        d.numFaceVertices = lv.GetNumFaceVertices();
        for (int f = 0; f < d.numFaces; ++f) {
            appendArray(d, lv.GetFaceVertices(f));
            appendArray(d, lv.GetFaceEdges(f));
            if (l < refiner.GetMaxLevel()) {
                d.childFaceVerts.push_back(lv.GetFaceChildVertex(f));
                ConstIndexArray cf = lv.GetFaceChildFaces(f);
                for (int i = 0; i < cf.size(); ++i) d.faceChildFaces.push_back(cf[i]);
                ConstIndexArray ce = lv.GetFaceChildEdges(f);
                for (int i = 0; i < ce.size(); ++i) d.faceChildEdges.push_back(ce[i]);
            }
            if (l > 0) d.faceParentFace.push_back(lv.GetFaceParentFace(f));
        }
        for (int e = 0; e < d.numEdges; ++e) {
            appendArray(d, lv.GetEdgeVertices(e));
            appendArray(d, lv.GetEdgeFaces(e));
            if (l < refiner.GetMaxLevel()) {
                d.childEdgeVerts.push_back(lv.GetEdgeChildVertex(e));
                ConstIndexArray ec = lv.GetEdgeChildEdges(e);
                for (int i = 0; i < ec.size(); ++i) d.edgeChildEdges.push_back(ec[i]);
            }
        }
        for (int v = 0; v < d.numVertices; ++v) {
            appendArray(d, lv.GetVertexFaces(v));
            appendArray(d, lv.GetVertexEdges(v));
            if (l < refiner.GetMaxLevel())
                d.childVertVerts.push_back(lv.GetVertexChildVertex(v));
        }
        s.levels.push_back(d);
    }
    return s;
}

bool compareViews(ViewSnapshot const & before, Far::TopologyRefiner const & refiner) {
    if ((int) before.levels.size() != refiner.GetNumLevels()) return false;
    for (int l = 0; l <= refiner.GetMaxLevel(); ++l) {
        Far::TopologyLevel const & lv = refiner.GetLevel(l);
        ViewSnapshot::Level const & d = before.levels[l];
        if (d.levelPtr != &lv) return false;
        if (d.numVertices != lv.GetNumVertices()) return false;
        if (d.numEdges != lv.GetNumEdges()) return false;
        if (d.numFaces != lv.GetNumFaces()) return false;
        if (d.numFaceVertices != lv.GetNumFaceVertices()) return false;

        std::vector<Index const *> pointers;
        std::vector<std::vector<int> > arrays;
        std::vector<int> childFaceVerts, childEdgeVerts, childVertVerts;
        std::vector<int> faceChildFaces, faceChildEdges, edgeChildEdges;
        std::vector<int> faceParentFace;
        for (int f = 0; f < d.numFaces; ++f) {
            ConstIndexArray a = lv.GetFaceVertices(f);
            pointers.push_back(a.size() > 0 ? a.begin() : (Index const *) 0);
            arrays.push_back(std::vector<int>(a.begin(), a.end()));
            ConstIndexArray b = lv.GetFaceEdges(f);
            pointers.push_back(b.size() > 0 ? b.begin() : (Index const *) 0);
            arrays.push_back(std::vector<int>(b.begin(), b.end()));
            if (l < refiner.GetMaxLevel()) {
                childFaceVerts.push_back(lv.GetFaceChildVertex(f));
                ConstIndexArray cf = lv.GetFaceChildFaces(f);
                for (int i = 0; i < cf.size(); ++i) faceChildFaces.push_back(cf[i]);
                ConstIndexArray ce = lv.GetFaceChildEdges(f);
                for (int i = 0; i < ce.size(); ++i) faceChildEdges.push_back(ce[i]);
            }
            if (l > 0) faceParentFace.push_back(lv.GetFaceParentFace(f));
        }
        for (int e = 0; e < d.numEdges; ++e) {
            ConstIndexArray a = lv.GetEdgeVertices(e);
            pointers.push_back(a.size() > 0 ? a.begin() : (Index const *) 0);
            arrays.push_back(std::vector<int>(a.begin(), a.end()));
            ConstIndexArray b = lv.GetEdgeFaces(e);
            pointers.push_back(b.size() > 0 ? b.begin() : (Index const *) 0);
            arrays.push_back(std::vector<int>(b.begin(), b.end()));
            if (l < refiner.GetMaxLevel()) {
                childEdgeVerts.push_back(lv.GetEdgeChildVertex(e));
                ConstIndexArray ec = lv.GetEdgeChildEdges(e);
                for (int i = 0; i < ec.size(); ++i) edgeChildEdges.push_back(ec[i]);
            }
        }
        for (int v = 0; v < d.numVertices; ++v) {
            ConstIndexArray a = lv.GetVertexFaces(v);
            pointers.push_back(a.size() > 0 ? a.begin() : (Index const *) 0);
            arrays.push_back(std::vector<int>(a.begin(), a.end()));
            ConstIndexArray b = lv.GetVertexEdges(v);
            pointers.push_back(b.size() > 0 ? b.begin() : (Index const *) 0);
            arrays.push_back(std::vector<int>(b.begin(), b.end()));
            if (l < refiner.GetMaxLevel())
                childVertVerts.push_back(lv.GetVertexChildVertex(v));
        }

        if (d.arrayPointers != pointers) return false;
        if (d.arrays != arrays) return false;
        if (d.childFaceVerts != childFaceVerts) return false;
        if (d.childEdgeVerts != childEdgeVerts) return false;
        if (d.childVertVerts != childVertVerts) return false;
        if (d.faceChildFaces != faceChildFaces) return false;
        if (d.faceChildEdges != faceChildEdges) return false;
        if (d.edgeChildEdges != edgeChildEdges) return false;
        if (d.faceParentFace != faceParentFace) return false;
    }
    return true;
}

int testViewsPreserved() {
    for (int method = 0; method < 2; ++method) {
        Mesh mesh = makeOpenQuadGrid();
        Setup setup;
        setup.method = method ? Sdc::Options::CREASE_CHAIKIN
                              : Sdc::Options::CREASE_UNIFORM;
        setup.levels = 3;

        std::unique_ptr<Far::TopologyRefiner> refiner = createRefiner(mesh, setup);
        ViewSnapshot before = captureViews(*refiner);

        std::vector<SharpnessEdit> edges, vertices;
        Far::TopologyLevel const & lv = refiner->GetLevel(0);
        for (int e = 0; e < lv.GetNumEdges(); ++e)
            edges.push_back(SharpnessEdit(e, (e % 3 == 0) ? 10.0f : 2.0f));
        for (int v = 0; v < lv.GetNumVertices(); ++v)
            vertices.push_back(SharpnessEdit(v, (v % 2) ? 0.0f : 4.0f));
        TASK_CHECK(refiner->UpdateUniformSharpness(edges, vertices));

        TASK_CHECK(compareViews(before, *refiner));

        //  The previously obtained level reference is still applicable.
        TASK_CHECK(lv.GetNumVertices() > 0);
        TASK_CHECK((int) before.levels.size() == refiner->GetNumLevels());
    }
    return 0;
}

//  Verify the refiner is unchanged after a rejected call -- both the
//  classification of every level and the cached topology views.
bool rejectedUnchanged(Far::TopologyRefiner & refiner,
                       std::vector<SharpnessEdit> const & edges,
                       std::vector<SharpnessEdit> const & vertices) {
    Snapshot before = snapshot(refiner);
    ViewSnapshot viewsBefore = captureViews(refiner);
    bool result = refiner.UpdateUniformSharpness(edges, vertices);
    return (!result) && snapshotsEqual(before, snapshot(refiner)) &&
           compareViews(viewsBefore, refiner);
}

int testViewsRejectInvalid() {
    Mesh mesh = makeOpenQuadGrid();
    Setup setup;
    setup.levels = 2;

    std::unique_ptr<Far::TopologyRefiner> refiner = createRefiner(mesh, setup);
    Far::TopologyLevel const & lv = refiner->GetLevel(0);
    int nEdges = lv.GetNumEdges();
    int nVerts = lv.GetNumVertices();

    {
        std::vector<SharpnessEdit> e, v;
        e.push_back(SharpnessEdit(-1, 1.0f));
        TASK_CHECK(rejectedUnchanged(*refiner, e, v));
    }
    {
        std::vector<SharpnessEdit> e, v;
        e.push_back(SharpnessEdit(nEdges, 1.0f));
        TASK_CHECK(rejectedUnchanged(*refiner, e, v));
    }
    {
        std::vector<SharpnessEdit> e, v;
        v.push_back(SharpnessEdit(-1, 1.0f));
        TASK_CHECK(rejectedUnchanged(*refiner, e, v));
    }
    {
        std::vector<SharpnessEdit> e, v;
        v.push_back(SharpnessEdit(nVerts, 1.0f));
        TASK_CHECK(rejectedUnchanged(*refiner, e, v));
    }
    {
        std::vector<SharpnessEdit> e, v;
        e.push_back(SharpnessEdit(0, -0.25f));
        TASK_CHECK(rejectedUnchanged(*refiner, e, v));
    }
    {
        std::vector<SharpnessEdit> e, v;
        e.push_back(SharpnessEdit(0, 10.5f));
        TASK_CHECK(rejectedUnchanged(*refiner, e, v));
    }
    {
        std::vector<SharpnessEdit> e, v;
        e.push_back(SharpnessEdit(0, std::numeric_limits<float>::quiet_NaN()));
        TASK_CHECK(rejectedUnchanged(*refiner, e, v));
    }
    {
        std::vector<SharpnessEdit> e, v;
        e.push_back(SharpnessEdit(0, std::numeric_limits<float>::infinity()));
        TASK_CHECK(rejectedUnchanged(*refiner, e, v));
    }
    {
        std::vector<SharpnessEdit> e, v;
        v.push_back(SharpnessEdit(0, -1.0f));
        TASK_CHECK(rejectedUnchanged(*refiner, e, v));
    }
    {
        std::vector<SharpnessEdit> e, v;
        v.push_back(SharpnessEdit(0, std::numeric_limits<float>::quiet_NaN()));
        TASK_CHECK(rejectedUnchanged(*refiner, e, v));
    }
    {
        //  duplicate edge index
        std::vector<SharpnessEdit> e, v;
        e.push_back(SharpnessEdit(0, 1.0f));
        e.push_back(SharpnessEdit(0, 2.0f));
        TASK_CHECK(rejectedUnchanged(*refiner, e, v));
    }
    {
        //  duplicate vertex index
        std::vector<SharpnessEdit> e, v;
        v.push_back(SharpnessEdit(0, 1.0f));
        v.push_back(SharpnessEdit(0, 2.0f));
        TASK_CHECK(rejectedUnchanged(*refiner, e, v));
    }
    {
        //  A valid edit followed by an invalid one: nothing changes again.
        std::vector<SharpnessEdit> e, v;
        e.push_back(SharpnessEdit(0, 3.0f));
        TASK_CHECK(refiner->UpdateUniformSharpness(e, v));
        std::vector<SharpnessEdit> bad;
        bad.push_back(SharpnessEdit(0, 3.0f));
        bad.push_back(SharpnessEdit(0, 5.0f));
        TASK_CHECK(rejectedUnchanged(*refiner, bad, v));
    }
    return 0;
}

int testViewsRejectUnsupported() {
    Mesh mesh = makeOpenQuadGrid();

    //  Adaptive refinement.
    {
        Setup setup; setup.levels = 2; setup.adaptive = true;
        std::unique_ptr<Far::TopologyRefiner> r = createRefiner(mesh, setup);
        std::vector<SharpnessEdit> e, v;
        e.push_back(SharpnessEdit(0, 1.0f));
        TASK_CHECK(!r->UpdateUniformSharpness(e, v));
    }
    //  Holes.
    {
        Setup setup; setup.levels = 2; setup.addHoles = true;
        std::unique_ptr<Far::TopologyRefiner> r = createRefiner(mesh, setup);
        std::vector<SharpnessEdit> e, v;
        e.push_back(SharpnessEdit(0, 1.0f));
        TASK_CHECK(r->HasHoles());
        TASK_CHECK(!r->UpdateUniformSharpness(e, v));
    }
    //  Face-varying channels.
    {
        Setup setup; setup.levels = 2; setup.addFVar = true;
        std::unique_ptr<Far::TopologyRefiner> r = createRefiner(mesh, setup);
        TASK_CHECK(r->GetNumFVarChannels() == 1);
        std::vector<SharpnessEdit> e, v;
        e.push_back(SharpnessEdit(0, 1.0f));
        TASK_CHECK(!r->UpdateUniformSharpness(e, v));
    }
    //  Shared coarse level.
    {
        Setup setup; setup.levels = 2; setup.sharedBase = true;
        std::unique_ptr<Far::TopologyRefiner> owner;
        std::unique_ptr<Far::TopologyRefiner> r = createRefiner(mesh, setup, 0, 0, &owner);
        TASK_CHECK(owner.get() != 0);
        std::vector<SharpnessEdit> e, v;
        e.push_back(SharpnessEdit(0, 1.0f));
        TASK_CHECK(!r->UpdateUniformSharpness(e, v));
    }
    //  No refinement levels.
    {
        Setup setup; setup.levels = 0;
        std::unique_ptr<Far::TopologyRefiner> r = createRefiner(mesh, setup);
        TASK_CHECK(r->GetNumLevels() == 1);
        std::vector<SharpnessEdit> e, v;
        e.push_back(SharpnessEdit(0, 1.0f));
        TASK_CHECK(!r->UpdateUniformSharpness(e, v));
    }
    //  Incomplete topology in the last level.
    {
        Setup setup; setup.levels = 2; setup.fullTopologyInLastLevel = false;
        std::unique_ptr<Far::TopologyRefiner> r = createRefiner(mesh, setup);
        std::vector<SharpnessEdit> e, v;
        e.push_back(SharpnessEdit(0, 1.0f));
        TASK_CHECK(!r->UpdateUniformSharpness(e, v));
    }
    //  Unsupported boundary interpolation (VTX_BOUNDARY_NONE).
    {
        Setup setup; setup.levels = 2;
        setup.boundary = Sdc::Options::VTX_BOUNDARY_NONE;
        std::unique_ptr<Far::TopologyRefiner> r = createRefiner(mesh, setup);
        std::vector<SharpnessEdit> e, v;
        e.push_back(SharpnessEdit(0, 1.0f));
        TASK_CHECK(!r->UpdateUniformSharpness(e, v));
    }
    return 0;
}

//  Cumulative edits survive a long chain of changes, always matching a rebuild.
int testViewsCumulativeChain() {
    Mesh mesh = makePentagonalPrism();
    Setup setup;
    setup.method = Sdc::Options::CREASE_CHAIKIN;
    setup.levels = 3;

    std::unique_ptr<Far::TopologyRefiner> refiner = createRefiner(mesh, setup);
    ViewSnapshot views = captureViews(*refiner);

    float const values[6] = { 0.0f, 1.0f, 3.5f, 10.0f, 0.75f, 0.0f };
    std::vector<SharpnessEdit> edges, vertices;
    for (int pass = 0; pass < 6; ++pass) {
        Far::TopologyLevel const & lv = refiner->GetLevel(0);
        edges.clear();
        vertices.clear();
        for (int e = 0; e < lv.GetNumEdges(); ++e)
            edges.push_back(SharpnessEdit(e, values[(e + pass) % 6]));
        for (int v = 0; v < lv.GetNumVertices(); ++v)
            vertices.push_back(SharpnessEdit(v, values[(v + 2 * pass) % 6]));
        TASK_CHECK(refiner->UpdateUniformSharpness(edges, vertices));
        TASK_CHECK(verifyAgainstRebuild(mesh, setup, *refiner));
        TASK_CHECK(compareViews(views, *refiner));
    }
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
//  Registration
//
//  driver.c is compiled as C++ by the harness, so these definitions need
//  external (C++) linkage at global scope, matching its declarations.
// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
    { "rules", "catmark_closed_uniform",         testRulesCatmarkClosedUniform },
    { "rules", "catmark_open_chaikin",           testRulesCatmarkOpenChaikin },
    { "rules", "catmark_open_corner_chaikin",    testRulesCatmarkOpenCornerChaikin },
    { "rules", "loop_closed_uniform",            testRulesLoopClosedUniform },
    { "rules", "loop_open_corner_chaikin",       testRulesLoopOpenCornerChaikin },
    { "rules", "vertex_convergence",             testRulesVertexConvergence },
    { "rules", "level_exhaustion",               testRulesLevelExhaustion },
    { "rules", "cumulative_edits",               testRulesCumulativeEdits },
    { "rules", "mixed_valence_closed",           testRulesMixedValenceClosed },
    { "rules", "chaikin_vs_uniform",             testRulesChaikinNotPerEdgeDecrement },
    { "rules", "empty_lists",                    testRulesEmptyLists },

    { "geometry", "catmark_uniform",             testGeometryCatmarkUniform },
    { "geometry", "catmark_chaikin_open",        testGeometryCatmarkChaikinOpen },
    { "geometry", "loop",                        testGeometryLoop },
    { "geometry", "old_stencil_preserved",       testGeometryOldStencilPreserved },

    { "views", "preserved",                      testViewsPreserved },
    { "views", "reject_invalid",                 testViewsRejectInvalid },
    { "views", "reject_unsupported",             testViewsRejectUnsupported },
    { "views", "cumulative_chain",               testViewsCumulativeChain }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
