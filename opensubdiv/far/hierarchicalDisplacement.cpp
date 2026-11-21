//
//   Copyright 2024 Pixar
//
//   Licensed under the terms set forth in the LICENSE.txt file available at
//   https://opensubdiv.org/license.
//

#include "../far/hierarchicalDisplacement.h"

#include "../far/topologyDescriptor.h"
#include "../far/topologyRefinerFactory.h"
#include "../far/stencilTable.h"
#include "../far/stencilTableFactory.h"
#include "../far/patchTableFactory.h"
#include "../far/ptexIndices.h"

#include "../sdc/options.h"
#include "../sdc/types.h"

#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace OpenSubdiv {
namespace OPENSUBDIV_VERSION {

namespace Far {

namespace {

typedef StencilTableReal<double>        DStencilTable;
typedef LimitStencilTableReal<double>   DLimitStencilTable;

// ---- small vector helpers (packed XYZ triples) ---------------------------

inline void vsub(double const * a, double const * b, double * r) {
    r[0] = a[0] - b[0];
    r[1] = a[1] - b[1];
    r[2] = a[2] - b[2];
}
inline void vcross(double const * a, double const * b, double * r) {
    r[0] = a[1] * b[2] - a[2] * b[1];
    r[1] = a[2] * b[0] - a[0] * b[2];
    r[2] = a[0] * b[1] - a[1] * b[0];
}
inline double vdot(double const * a, double const * b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline double vnorm(double const * a) {
    return std::sqrt(vdot(a, a));
}

bool allFinite(double const * data, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (!std::isfinite(data[i])) {
            return false;
        }
    }
    return true;
}

// Adapter exposing the Clear()/AddWithWeight() element interface required
// by StencilTableReal::UpdateValues over a packed XYZ double array.
struct XYZProxy {
    double * p;
    void Clear() const {
        p[0] = 0.0;
        p[1] = 0.0;
        p[2] = 0.0;
    }
    void AddWithWeight(XYZProxy const & src, double w) const {
        p[0] += w * src.p[0];
        p[1] += w * src.p[1];
        p[2] += w * src.p[2];
    }
};
struct XYZView {
    double * base;
    XYZProxy operator[](int i) const { return XYZProxy{base + 3 * i}; }
};
// Builds a raw topology descriptor (and its backing index arrays) from the
// face inventory of an existing Far level.
struct LevelDescriptor {
    std::vector<int> vertsPerFace;
    std::vector<int> faceVertIndices;
    TopologyDescriptor descriptor;

    LevelDescriptor(TopologyLevel const & level) {
        int numFaces = level.GetNumFaces();
        vertsPerFace.reserve(numFaces);
        for (int f = 0; f < numFaces; ++f) {
            ConstIndexArray fv = level.GetFaceVertices(f);
            vertsPerFace.push_back(fv.size());
            for (int k = 0; k < fv.size(); ++k) {
                faceVertIndices.push_back(fv[k]);
            }
        }
        descriptor.numVertices      = level.GetNumVertices();
        descriptor.numFaces         = numFaces;
        descriptor.numVertsPerFace  = vertsPerFace.data();
        descriptor.vertIndicesPerFace = faceVertIndices.data();
    }
};

TopologyRefiner *
createRefiner(TopologyDescriptor const & desc,
              Sdc::SchemeType scheme, Sdc::Options const & schemeOptions) {
    TopologyRefinerFactory<TopologyDescriptor>::Options opts(
        scheme, schemeOptions);
    TopologyRefiner * refiner =
        TopologyRefinerFactory<TopologyDescriptor>::Create(desc, opts);
    if (!refiner) {
        throw std::runtime_error(
            "HierarchicalDisplacement: failed to create internal refiner");
    }
    return refiner;
}

// One oriented corner of a vertex within one incident face: the two
// neighboring corners of that face.
struct NormalCorner {
    int prevVertex;
    int nextVertex;
};

// Per-vertex ring of face corners, stored in the (oriented) face order
// returned by the topology level.
struct NormalTopology {
    std::vector<std::vector<NormalCorner> > corners;
};

} // anonymous namespace

// ==========================================================================
// Immutable plan data, shared by all tapes of a plan.
// ==========================================================================

struct HierarchicalDisplacement::PlanData {
    Sdc::SchemeType scheme;
    Sdc::Options schemeOptions;

    int numLayers = 0;
    int numSamples = 0;
    int numCoarseVerts = 0;

    // Vertex count of every level, [0 .. L].
    std::vector<int> numVerts;

    // One subdivision stencil table per layer: rows map level-l vertices to
    // level-(l-1) vertices.
    std::vector<std::shared_ptr<DStencilTable const> > subdivTables;

    // Normal adjacency of every sculpting layer, indexed by layer vertex.
    std::vector<NormalTopology> normalTopology;

    // Final layer cage inventory.
    int finalNumFaces = 0;
    int finalFaceSize = 0;
    std::vector<int> finalFaceVertsPerFace;
    std::vector<int> finalFaceVertIndices;

    // Limit stencils of the fixed samples, expressed over the final layer
    // cage vertices.
    std::shared_ptr<DLimitStencilTable const> limitTable;
    std::vector<HierarchicalDisplacementSample> samples;
};

// ==========================================================================
// Per-forward tape data.
// ==========================================================================

struct HierarchicalDisplacement::Tape::Data {
    std::shared_ptr<const PlanData> plan;

    // Copies of the forward inputs, making the tape fully self-contained.
    std::vector<double> coarseXYZ;
    std::vector<std::vector<double> > displacements;

    // Geometry of every sculpting layer.
    std::vector<std::vector<double> > subdivPoints;   // Q_l, pre-displacement
    std::vector<std::vector<double> > areaVectors;    // A_l
    std::vector<std::vector<double> > unitNormals;    // n_l
    std::vector<std::vector<double> > layerPoints;    // P_l, post-displacement
    std::vector<double> areaLengths;                  // ||A_l,v||, flattened

    // Sample evaluation results.
    std::vector<double> samplePu;
    std::vector<double> samplePv;
    std::vector<double> sampleCross;
    std::vector<double> sampleCrossLength;
};

HierarchicalDisplacement::Tape::Tape() = default;
HierarchicalDisplacement::Tape::~Tape() = default;

int HierarchicalDisplacement::Tape::GetNumLayers() const {
    return (int)_data->plan->numLayers;
}
std::vector<double> const &
HierarchicalDisplacement::Tape::GetSubdivPoints(int layer) const {
    if (layer < 1 || layer > (int)_data->subdivPoints.size()) {
        throw std::invalid_argument(
            "Tape::GetSubdivPoints: layer out of range");
    }
    return _data->subdivPoints[layer - 1];
}
std::vector<double> const &
HierarchicalDisplacement::Tape::GetUnitNormals(int layer) const {
    if (layer < 1 || layer > (int)_data->unitNormals.size()) {
        throw std::invalid_argument(
            "Tape::GetUnitNormals: layer out of range");
    }
    return _data->unitNormals[layer - 1];
}
std::vector<double> const &
HierarchicalDisplacement::Tape::GetLayerPoints(int layer) const {
    if (layer < 1 || layer > (int)_data->layerPoints.size()) {
        throw std::invalid_argument(
            "Tape::GetLayerPoints: layer out of range");
    }
    return _data->layerPoints[layer - 1];
}
std::vector<double> const &
HierarchicalDisplacement::Tape::GetSamplePu() const {
    return _data->samplePu;
}
std::vector<double> const &
HierarchicalDisplacement::Tape::GetSamplePv() const {
    return _data->samplePv;
}
std::vector<double> const &
HierarchicalDisplacement::Tape::GetCoarseXYZ() const {
    return _data->coarseXYZ;
}
std::vector<double> const &
HierarchicalDisplacement::Tape::GetDisplacements(int layer) const {
    if (layer < 1 || layer > (int)_data->displacements.size()) {
        throw std::invalid_argument(
            "Tape::GetDisplacements: layer out of range");
    }
    return _data->displacements[layer - 1];
}

// ==========================================================================
// Plan construction
// ==========================================================================

HierarchicalDisplacement::HierarchicalDisplacement(
        TopologyRefiner const & coarseRefiner,
        int numLayers,
        std::vector<HierarchicalDisplacementSample> const & samples) {

    if (numLayers < 1 || numLayers > 3) {
        throw std::invalid_argument(
            "HierarchicalDisplacement: numLayers must be in [1, 3]");
    }
    if (samples.empty()) {
        throw std::invalid_argument(
            "HierarchicalDisplacement: at least one sample is required");
    }
    if (coarseRefiner.GetMaxLevel() != 0) {
        throw std::invalid_argument(
            "HierarchicalDisplacement: input refiner must contain only the "
            "coarse level");
    }

    Sdc::SchemeType scheme = coarseRefiner.GetSchemeType();
    if (scheme != Sdc::SCHEME_CATMARK && scheme != Sdc::SCHEME_LOOP) {
        throw std::invalid_argument(
            "HierarchicalDisplacement: only Catmark and Loop are supported");
    }
    Sdc::Options schemeOptions = coarseRefiner.GetSchemeOptions();
    if (schemeOptions.GetVtxBoundaryInterpolation()
            != Sdc::Options::VTX_BOUNDARY_EDGE_ONLY) {
        throw std::invalid_argument(
            "HierarchicalDisplacement: VTX_BOUNDARY_EDGE_ONLY is required");
    }

    TopologyLevel const & coarseLevel = coarseRefiner.GetLevel(0);
    int regularFaceSize =
        Sdc::SchemeTypeTraits::GetRegularFaceSize(scheme);
    int numCoarse = coarseLevel.GetNumVertices();
    int numCoarseFaces = coarseLevel.GetNumFaces();

    if (numCoarse <= 0 || numCoarseFaces <= 0) {
        throw std::invalid_argument(
            "HierarchicalDisplacement: empty coarse mesh");
    }
    if (coarseRefiner.HasHoles()) {
        throw std::invalid_argument(
            "HierarchicalDisplacement: holes are not supported");
    }

    // Domain validation over every coarse component relationship.
    for (int f = 0; f < numCoarseFaces; ++f) {
        ConstIndexArray fv = coarseLevel.GetFaceVertices(f);
        if (fv.size() != regularFaceSize) {
            throw std::invalid_argument(
                "HierarchicalDisplacement: all faces must have the scheme's "
                "regular size (quads for Catmark, triangles for Loop)");
        }
        if (coarseLevel.IsFaceHole(f)) {
            throw std::invalid_argument(
                "HierarchicalDisplacement: holes are not supported");
        }
    }
    for (int e = 0; e < coarseLevel.GetNumEdges(); ++e) {
        if (coarseLevel.IsEdgeNonManifold(e)) {
            throw std::invalid_argument(
                "HierarchicalDisplacement: non-manifold edges are not "
                "supported");
        }
        ConstIndexArray edgeFaces = coarseLevel.GetEdgeFaces(e);
        if (edgeFaces.size() != 1 && edgeFaces.size() != 2) {
            throw std::invalid_argument(
                "HierarchicalDisplacement: only oriented manifold meshes "
                "(with open boundaries) are supported");
        }
        // VTX_BOUNDARY_EDGE_ONLY sharpens boundary edges internally, so only
        // interior sharpness can represent an explicit user crease.
        if (edgeFaces.size() == 2 &&
            coarseLevel.GetEdgeSharpness(e) != 0.0f) {
            throw std::invalid_argument(
                "HierarchicalDisplacement: explicit edge creases are not "
                "supported");
        }
        // Verify consistent orientation across interior edges: the two
        // incident faces must traverse the shared edge in opposite
        // directions.
        if (edgeFaces.size() == 2) {
            int ordered[2][2];
            for (int side = 0; side < 2; ++side) {
                int face = edgeFaces[side];
                ConstIndexArray fEdges = coarseLevel.GetFaceEdges(face);
                ConstIndexArray fVerts = coarseLevel.GetFaceVertices(face);
                int localEdge = fEdges.FindIndex(e);
                int k = fVerts.size();
                ordered[side][0] = fVerts[localEdge];
                ordered[side][1] = fVerts[(localEdge + 1) % k];
            }
            if (ordered[0][0] != ordered[1][1] ||
                ordered[0][1] != ordered[1][0]) {
                throw std::invalid_argument(
                    "HierarchicalDisplacement: mesh is not consistently "
                    "oriented");
            }
        }
    }
    for (int v = 0; v < numCoarse; ++v) {
        if (coarseLevel.IsVertexNonManifold(v)) {
            throw std::invalid_argument(
                "HierarchicalDisplacement: non-manifold vertices are not "
                "supported");
        }
        // Boundary tags are expected for open cages; explicit user creases
        // are only possible on interior vertices under EDGE_ONLY.
        if (!coarseLevel.IsVertexBoundary(v) &&
            coarseLevel.GetVertexSharpness(v) != 0.0f) {
            throw std::invalid_argument(
                "HierarchicalDisplacement: explicit vertex creases are not "
                "supported");
        }
        if (coarseLevel.GetVertexFaces(v).size() == 0) {
            throw std::invalid_argument(
                "HierarchicalDisplacement: isolated vertices are not "
                "supported");
        }
    }
    for (HierarchicalDisplacementSample const & sample : samples) {
        if (!(sample.s > 0.0 && sample.s < 1.0 &&
              sample.t > 0.0 && sample.t < 1.0)) {
            throw std::invalid_argument(
                "HierarchicalDisplacement: sample parameters must be strict "
                "interior ptex coordinates in (0, 1)");
        }
        if (!std::isfinite(sample.s) || !std::isfinite(sample.t) ||
            sample.face < 0) {
            throw std::invalid_argument(
                "HierarchicalDisplacement: invalid sample descriptor");
        }
    }

    std::shared_ptr<PlanData> plan = std::make_shared<PlanData>();
    plan->scheme = scheme;
    plan->schemeOptions = schemeOptions;
    plan->numLayers = numLayers;
    plan->numSamples = (int)samples.size();
    plan->numCoarseVerts = numCoarse;
    plan->samples = samples;
    plan->numVerts.resize(numLayers + 1);
    plan->numVerts[0] = numCoarse;
    plan->normalTopology.resize(numLayers);

    // -- chained one-level refiners ----------------------------------------
    // Every layer's stencil rows, normal rings and the next layer's cage
    // descriptor are derived from the same one-step refiner, so the vertex
    // numbering stays consistent across the whole chain (a freshly built
    // multi-level refiner would order edge-derived vertices differently).
    LevelDescriptor chainDesc(coarseLevel);

    for (int l = 1; l <= numLayers; ++l) {
        std::unique_ptr<TopologyRefiner> stepRefiner(createRefiner(
            chainDesc.descriptor, scheme, schemeOptions));
        TopologyRefiner::UniformOptions stepOptions(1);
        stepOptions.fullTopologyInLastLevel = true;
        stepRefiner->RefineUniform(stepOptions);

        TopologyLevel const & parentLevel = stepRefiner->GetLevel(0);
        TopologyLevel const & childLevel  = stepRefiner->GetLevel(1);
        plan->numVerts[l] = childLevel.GetNumVertices();

        // Stencil rows indexed purely by this step's parent/child local
        // vertex numbering.
        StencilTableFactoryReal<double>::Options stencilOptions;
        stencilOptions.generateIntermediateLevels = false;
        stencilOptions.generateControlVerts = false;
        stencilOptions.generateOffsets = true;
        DStencilTable const * rawTable =
            StencilTableFactoryReal<double>::Create(
                *stepRefiner, stencilOptions);
        if (!rawTable || rawTable->GetNumStencils()
                            != childLevel.GetNumVertices()) {
            delete rawTable;
            throw std::runtime_error(
                "HierarchicalDisplacement: failed to build layer stencil "
                "table");
        }
        if (rawTable->GetNumControlVertices()
                != parentLevel.GetNumVertices()) {
            delete rawTable;
            throw std::runtime_error(
                "HierarchicalDisplacement: internal stencil index space "
                "mismatch");
        }
        plan->subdivTables.emplace_back(
            std::shared_ptr<DStencilTable const>(rawTable));

        // Normal adjacency: one oriented corner contribution per incident
        // face of each child-level vertex.
        NormalTopology & normal = plan->normalTopology[l - 1];
        normal.corners.resize(childLevel.GetNumVertices());
        for (int v = 0; v < childLevel.GetNumVertices(); ++v) {
            ConstIndexArray faces = childLevel.GetVertexFaces(v);
            ConstLocalIndexArray localIndices =
                childLevel.GetVertexFaceLocalIndices(v);
            normal.corners[v].reserve(faces.size());
            for (int i = 0; i < faces.size(); ++i) {
                ConstIndexArray fv =
                    childLevel.GetFaceVertices(faces[i]);
                int k = fv.size();
                int li = localIndices[i];
                NormalCorner corner;
                corner.prevVertex = fv[(li - 1 + k) % k];
                corner.nextVertex = fv[(li + 1) % k];
                normal.corners[v].push_back(corner);
            }
        }

        // The next layer's cage descriptor shares this child's face
        // inventory and therefore its vertex numbering.
        chainDesc = LevelDescriptor(childLevel);
    }

    // -- final layer cage as a brand new control cage ----------------------
    plan->finalNumFaces = (int)chainDesc.vertsPerFace.size();
    plan->finalFaceSize = regularFaceSize;
    plan->finalFaceVertsPerFace = chainDesc.vertsPerFace;
    plan->finalFaceVertIndices = chainDesc.faceVertIndices;

    std::unique_ptr<TopologyRefiner> limitRefiner(createRefiner(
        chainDesc.descriptor, scheme, schemeOptions));

    PatchTableFactory::Options patchOptions(5);
    patchOptions.SetPatchPrecision<double>();
    patchOptions.SetEndCapType(
        PatchTableFactory::Options::ENDCAP_GREGORY_BASIS);
    limitRefiner->RefineAdaptive(
        patchOptions.GetRefineAdaptiveOptions());
    std::unique_ptr<PatchTable> patchTable(
        PatchTableFactory::Create(*limitRefiner, patchOptions));
    if (!patchTable) {
        throw std::runtime_error(
            "HierarchicalDisplacement: failed to build final layer patch "
            "table");
    }

    PtexIndices ptexIndices(*limitRefiner);
    int numPtex = ptexIndices.GetNumFaces();

    // One single-location LocationArray per sample, in request order.  The
    // limit factory emits stencils by iterating the location arrays, so this
    // keeps the stencil row order identical to the requested sample order
    // even when samples share a ptex face (duplicate samples).  The s/t
    // pointers alias plan->samples, which is stable for the plan's life.
    for (HierarchicalDisplacementSample const & sample : plan->samples) {
        if (sample.face >= numPtex) {
            throw std::invalid_argument(
                "HierarchicalDisplacement: sample ptex face id out of range "
                "of the final layer cage");
        }
    }
    LimitStencilTableFactoryReal<double>::LocationArrayVec locations;
    locations.reserve(plan->samples.size());
    for (HierarchicalDisplacementSample & sample : plan->samples) {
        LimitStencilTableFactoryReal<double>::LocationArray array;
        array.ptexIdx = sample.face;
        array.numLocations = 1;
        array.s = &sample.s;
        array.t = &sample.t;
        locations.push_back(array);
    }

    DLimitStencilTable const * rawLimit =
        LimitStencilTableFactoryReal<double>::Create(
            *limitRefiner, locations, nullptr, patchTable.get());
    if (!rawLimit || rawLimit->GetNumStencils() != (int)samples.size()) {
        delete rawLimit;
        throw std::runtime_error(
            "HierarchicalDisplacement: failed to build limit stencils for "
            "the requested samples");
    }
    if (rawLimit->GetNumControlVertices()
            != plan->numVerts[numLayers]) {
        delete rawLimit;
        throw std::runtime_error(
            "HierarchicalDisplacement: limit stencil index space mismatch");
    }
    plan->limitTable.reset(rawLimit);

    _plan = std::move(plan);
}

HierarchicalDisplacement::~HierarchicalDisplacement() = default;

int HierarchicalDisplacement::GetNumLayers() const {
    return _plan->numLayers;
}
int HierarchicalDisplacement::GetNumSamples() const {
    return _plan->numSamples;
}
int HierarchicalDisplacement::GetNumCoarseVertices() const {
    return _plan->numCoarseVerts;
}
int HierarchicalDisplacement::GetNumLayerVertices(int layer) const {
    if (layer < 1 || layer > _plan->numLayers) {
        throw std::invalid_argument(
            "HierarchicalDisplacement::GetNumLayerVertices: layer out of "
            "range");
    }
    return _plan->numVerts[layer];
}
int HierarchicalDisplacement::GetFinalNumFaces() const {
    return _plan->finalNumFaces;
}
int HierarchicalDisplacement::GetFinalFaceSize() const {
    return _plan->finalFaceSize;
}
std::vector<int> const &
HierarchicalDisplacement::GetFinalFaceVertexIndices() const {
    return _plan->finalFaceVertIndices;
}

// ==========================================================================
// Forward
// ==========================================================================

std::shared_ptr<HierarchicalDisplacement::Tape>
HierarchicalDisplacement::Forward(
        std::vector<double> const & coarseXYZ,
        std::vector<std::vector<double> > const & displacements,
        HierarchicalDisplacementResult & result) const {

    PlanData const & plan = *_plan;
    int L = plan.numLayers;

    if ((int)coarseXYZ.size() != 3 * plan.numCoarseVerts) {
        throw std::invalid_argument(
            "HierarchicalDisplacement::Forward: coarse XYZ array has the "
            "wrong length");
    }
    if (!allFinite(coarseXYZ.data(), coarseXYZ.size())) {
        throw std::invalid_argument(
            "HierarchicalDisplacement::Forward: non-finite coarse position");
    }
    if ((int)displacements.size() != L) {
        throw std::invalid_argument(
            "HierarchicalDisplacement::Forward: expected one displacement "
            "array per layer");
    }
    for (int l = 0; l < L; ++l) {
        if ((int)displacements[l].size() != plan.numVerts[l + 1]) {
            throw std::invalid_argument(
                "HierarchicalDisplacement::Forward: displacement array has "
                "the wrong length");
        }
        if (!allFinite(displacements[l].data(),
                       displacements[l].size())) {
            throw std::invalid_argument(
                "HierarchicalDisplacement::Forward: non-finite displacement "
                "value");
        }
    }

    // The tape is fully populated before publication so a failure leaves no
    // half-built result and no observable side effect on existing tapes.
    std::shared_ptr<Tape> tape(new Tape());
    tape->_data = std::make_shared<Tape::Data>();
    Tape::Data & data = *tape->_data;
    data.plan = _plan;
    data.coarseXYZ = coarseXYZ;
    data.displacements = displacements;
    data.subdivPoints.resize(L);
    data.areaVectors.resize(L);
    data.unitNormals.resize(L);
    data.layerPoints.resize(L);

    std::vector<double> const * previous = &data.coarseXYZ;

    for (int l = 0; l < L; ++l) {
        int numChild = plan.numVerts[l + 1];

        // Q_{l+1} = S_{l+1} P_l
        std::vector<double> & subdiv = data.subdivPoints[l];
        subdiv.assign((std::size_t)3 * numChild, 0.0);
        XYZView srcView{const_cast<double *>(previous->data())};
        XYZView dstView{subdiv.data()};
        plan.subdivTables[l]->UpdateValues(srcView, dstView);

        // Area vectors from the oriented face corners.
        NormalTopology const & normal = plan.normalTopology[l];
        std::vector<double> & area = data.areaVectors[l];
        std::vector<double> & unit = data.unitNormals[l];
        area.assign((std::size_t)3 * numChild, 0.0);
        unit.resize((std::size_t)3 * numChild);

        for (int v = 0; v < numChild; ++v) {
            double const * qv = &subdiv[3 * v];
            double accum[3] = {0.0, 0.0, 0.0};
            for (NormalCorner const & corner : normal.corners[v]) {
                double dx[3], dy[3], cr[3];
                vsub(&subdiv[3 * corner.nextVertex], qv, dx);
                vsub(&subdiv[3 * corner.prevVertex], qv, dy);
                vcross(dx, dy, cr);
                accum[0] += cr[0];
                accum[1] += cr[1];
                accum[2] += cr[2];
            }
            area[3 * v]     = accum[0];
            area[3 * v + 1] = accum[1];
            area[3 * v + 2] = accum[2];

            double length = vnorm(accum);
            if (!(length > 1e-12)) {
                throw std::runtime_error(
                    "HierarchicalDisplacement::Forward: degenerate area "
                    "normal (||A_v|| <= 1e-12)");
            }
            data.areaLengths.push_back(length);
            double inv = 1.0 / length;
            unit[3 * v]     = accum[0] * inv;
            unit[3 * v + 1] = accum[1] * inv;
            unit[3 * v + 2] = accum[2] * inv;
        }

        // P_{l+1} = Q_{l+1} + d_{l+1} * n
        std::vector<double> & sculpted = data.layerPoints[l];
        sculpted.resize((std::size_t)3 * numChild);
        std::vector<double> const & disp = data.displacements[l];
        for (int v = 0; v < numChild; ++v) {
            double d = disp[v];
            sculpted[3 * v]     = subdiv[3 * v]     + d * unit[3 * v];
            sculpted[3 * v + 1] = subdiv[3 * v + 1] + d * unit[3 * v + 1];
            sculpted[3 * v + 2] = subdiv[3 * v + 2] + d * unit[3 * v + 2];
        }

        previous = &sculpted;
    }

    int numFinal = plan.numVerts[L];
    int numSamples = plan.numSamples;

    // Limit evaluation of the fixed samples through the final layer cage.
    DLimitStencilTable const & limit = *plan.limitTable;
    std::vector<double> const & finalPoints = data.layerPoints[L - 1];

    std::vector<double> positions((std::size_t)3 * numSamples, 0.0);
    data.samplePu.assign((std::size_t)3 * numSamples, 0.0);
    data.samplePv.assign((std::size_t)3 * numSamples, 0.0);
    XYZView finalView{const_cast<double *>(finalPoints.data())};
    XYZView posView{positions.data()};
    XYZView puView{data.samplePu.data()};
    XYZView pvView{data.samplePv.data()};
    limit.UpdateValues(finalView, posView);
    limit.UpdateDerivs(finalView, puView, pvView);

    data.sampleCross.resize((std::size_t)3 * numSamples);
    data.sampleCrossLength.resize(numSamples);
    std::vector<double> normals((std::size_t)3 * numSamples, 0.0);

    for (int i = 0; i < numSamples; ++i) {
        double const * pu = &data.samplePu[3 * i];
        double const * pv = &data.samplePv[3 * i];
        double * cr = &data.sampleCross[3 * i];
        vcross(pu, pv, cr);
        double length = vnorm(cr);
        if (!(length > 1e-12)) {
            throw std::runtime_error(
                "HierarchicalDisplacement::Forward: degenerate sample "
                "tangent frame (||Pu x Pv|| <= 1e-12)");
        }
        data.sampleCrossLength[i] = length;
        double inv = 1.0 / length;
        normals[3 * i]     = cr[0] * inv;
        normals[3 * i + 1] = cr[1] * inv;
        normals[3 * i + 2] = cr[2] * inv;
    }

    // Publish only once every computation above has succeeded.
    result.finalLayerVertices = finalPoints;
    result.samplePositions = std::move(positions);
    result.sampleNormals = std::move(normals);

    return tape;
}

// ==========================================================================
// Pullback (analytic adjoint)
// ==========================================================================

void
HierarchicalDisplacement::Tape::Pullback(
        std::vector<double> const & samplePositionAdjoint,
        std::vector<double> const & sampleNormalAdjoint,
        std::vector<double> & coarseXYZGradient,
        std::vector<std::vector<double> *> const & displacementGradients)
        const {

    Tape::Data const & data = *_data;
    PlanData const & plan = *data.plan;
    int L = plan.numLayers;
    int numSamples = plan.numSamples;

    if ((int)samplePositionAdjoint.size() != 3 * numSamples ||
        (int)sampleNormalAdjoint.size() != 3 * numSamples) {
        throw std::invalid_argument(
            "HierarchicalDisplacement::Pullback: sample adjoint array has "
            "the wrong length");
    }
    if ((int)coarseXYZGradient.size() != 3 * plan.numVerts[0]) {
        throw std::invalid_argument(
            "HierarchicalDisplacement::Pullback: coarse gradient array has "
            "the wrong length");
    }
    if (!allFinite(samplePositionAdjoint.data(),
                   samplePositionAdjoint.size()) ||
        !allFinite(sampleNormalAdjoint.data(),
                   sampleNormalAdjoint.size())) {
        throw std::invalid_argument(
            "HierarchicalDisplacement::Pullback: non-finite sample adjoint");
    }
    if ((int)displacementGradients.size() != L) {
        throw std::invalid_argument(
            "HierarchicalDisplacement::Pullback: expected one gradient "
            "buffer per layer");
    }
    for (int l = 0; l < L; ++l) {
        if (!displacementGradients[l] ||
            (int)displacementGradients[l]->size()
                != plan.numVerts[l + 1]) {
            throw std::invalid_argument(
                "HierarchicalDisplacement::Pullback: displacement gradient "
                "array has the wrong length");
        }
    }

    double const * bPos = samplePositionAdjoint.data();
    double const * bNrm = sampleNormalAdjoint.data();
    double * coarseGrad = coarseXYZGradient.data();

    // -- sample adjoints through the unit normal and cross product --------
    std::vector<double> puAdjoint((std::size_t)3 * numSamples, 0.0);
    std::vector<double> pvAdjoint((std::size_t)3 * numSamples, 0.0);
    std::vector<double> finalAdjoint(
        (std::size_t)3 * plan.numVerts[L], 0.0);

    for (int i = 0; i < numSamples; ++i) {
        double const * cross = &data.sampleCross[3 * i];
        double invLength = 1.0 / data.sampleCrossLength[i];
        double const * bN = &sampleNormalAdjoint[3 * i];

        // The unit normal is N = C / ||C|| where C = Pu x Pv.  The reverse
        // of the normalization projects the adjoint onto the tangent plane
        // and divides by the cross-product length.
        double unit[3];
        for (int k = 0; k < 3; ++k) {
            unit[k] = cross[k] * invLength;
        }
        double nn = vdot(unit, bN);
        double bC[3];
        for (int k = 0; k < 3; ++k) {
            bC[k] = (bN[k] - unit[k] * nn) * invLength;
        }

        // C = Pu x Pv  =>  bPu += Pv x bC,  bPv += bC x Pu.
        double const * pv = &data.samplePv[3 * i];
        double const * pu = &data.samplePu[3 * i];
        double t1[3], t2[3];
        vcross(pv, bC, t1);
        vcross(bC, pu, t2);
        for (int k = 0; k < 3; ++k) {
            puAdjoint[3 * i + k] = t1[k];
            pvAdjoint[3 * i + k] = t2[k];
        }
    }

    // Transpose of the three limit stencil weight matrices, accumulated
    // onto the same final-layer vertex adjoint.  The stencil table has one
    // row per sample, so the packed XYZ sample adjoints must be split into
    // one scalar-per-row call per Cartesian axis.
    DLimitStencilTable const & limit = *plan.limitTable;
    std::vector<double> axisSrc(numSamples);
    for (int axis = 0; axis < 3; ++axis) {
        for (int i = 0; i < numSamples; ++i) {
            axisSrc[i] = bPos[3 * i + axis];
        }
        limit.SparseTransposeAccumulate(axisSrc.data(),
                                        &finalAdjoint[axis],
                                        nullptr, 3);
        for (int i = 0; i < numSamples; ++i) {
            axisSrc[i] = puAdjoint[3 * i + axis];
        }
        limit.SparseTransposeAccumulate(axisSrc.data(),
                                        &finalAdjoint[axis],
                                        limit.GetDuWeights().data(), 3);
        for (int i = 0; i < numSamples; ++i) {
            axisSrc[i] = pvAdjoint[3 * i + axis];
        }
        limit.SparseTransposeAccumulate(axisSrc.data(),
                                        &finalAdjoint[axis],
                                        limit.GetDvWeights().data(), 3);
    }

    // -- reverse through the sculpting layers ------------------------------
    std::vector<double> childAdjoint = std::move(finalAdjoint);

    for (int l = L - 1; l >= 0; --l) {
        int numChild = plan.numVerts[l + 1];
        int numParent = plan.numVerts[l];
        NormalTopology const & normal = plan.normalTopology[l];

        std::vector<double> const & subdiv = data.subdivPoints[l];
        std::vector<double> const & unit = data.unitNormals[l];
        std::vector<double> const & disp = data.displacements[l];

        std::vector<double> subdivAdjoint(
            (std::size_t)3 * numChild, 0.0);

        // areaLengths are stored flat, one entry per (layer, vertex).
        std::size_t areaOffset = 0;
        for (int q = 0; q < l; ++q) {
            areaOffset += plan.numVerts[q + 1];
        }

        for (int v = 0; v < numChild; ++v) {
            double const * bP = &childAdjoint[3 * v];
            std::vector<double> & dGrad = *displacementGradients[l];

            // d_l gradient: bP . n_v.
            dGrad[v] += vdot(bP, &unit[3 * v]);

            // Direct Q_v contribution of P_v = Q_v + d_v n_v.
            subdivAdjoint[3 * v]     += bP[0];
            subdivAdjoint[3 * v + 1] += bP[1];
            subdivAdjoint[3 * v + 2] += bP[2];

            // Seed on n_v is d_v * bP; reverse the unit normalization
            // n_v = A_v / ||A_v|| onto the area vector A_v.
            double seedN[3] = {
                disp[v] * bP[0],
                disp[v] * bP[1],
                disp[v] * bP[2]
            };
            double length = data.areaLengths[areaOffset + v];
            double const * n = &unit[3 * v];
            double nn = vdot(n, seedN);
            double bA[3];
            for (int k = 0; k < 3; ++k) {
                bA[k] = (seedN[k] - n[k] * nn) / length;
            }

            // Reverse each cross product contribution:
            //   k = cross(Q_next - Q_v, Q_prev - Q_v) = cross(x, y)
            //   bX = y x bA ;  bY = bA x x
            //   bQ_next += bX, bQ_prev += bY, bQ_v -= bX + bY.
            double sumSelf[3] = {0.0, 0.0, 0.0};
            for (NormalCorner const & corner : normal.corners[v]) {
                double x[3], y[3], bX[3], bY[3];
                vsub(&subdiv[3 * corner.nextVertex], &subdiv[3 * v], x);
                vsub(&subdiv[3 * corner.prevVertex], &subdiv[3 * v], y);
                vcross(y, bA, bX);
                vcross(bA, x, bY);

                subdivAdjoint[3 * corner.nextVertex]     += bX[0];
                subdivAdjoint[3 * corner.nextVertex + 1] += bX[1];
                subdivAdjoint[3 * corner.nextVertex + 2] += bX[2];
                subdivAdjoint[3 * corner.prevVertex]     += bY[0];
                subdivAdjoint[3 * corner.prevVertex + 1] += bY[1];
                subdivAdjoint[3 * corner.prevVertex + 2] += bY[2];
                for (int k = 0; k < 3; ++k) {
                    sumSelf[k] += bX[k] + bY[k];
                }
            }
            subdivAdjoint[3 * v]     -= sumSelf[0];
            subdivAdjoint[3 * v + 1] -= sumSelf[1];
            subdivAdjoint[3 * v + 2] -= sumSelf[2];
        }

        // Transpose of the linear subdivision stencils.
        std::vector<double> parentAdjoint(
            (std::size_t)3 * numParent, 0.0);
        for (int axis = 0; axis < 3; ++axis) {
            std::vector<double> src(numChild);
            for (int v = 0; v < numChild; ++v) {
                src[v] = subdivAdjoint[3 * v + axis];
            }
            std::vector<double> dst(numParent, 0.0);
            plan.subdivTables[l]->SparseTransposeAccumulate(
                src.data(), dst.data());
            for (int v = 0; v < numParent; ++v) {
                parentAdjoint[3 * v + axis] = dst[v];
            }
        }

        childAdjoint = std::move(parentAdjoint);
    }

    // Accumulate onto the caller's buffers rather than overwriting them.
    for (std::size_t i = 0; i < childAdjoint.size(); ++i) {
        coarseGrad[i] += childAdjoint[i];
    }
}

} // end namespace Far

} // end namespace OPENSUBDIV_VERSION
using namespace OPENSUBDIV_VERSION;
} // end namespace OpenSubdiv
