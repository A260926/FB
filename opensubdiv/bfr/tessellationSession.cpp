//
//   Copyright 2021 Pixar
//
//   Licensed under the terms set forth in the LICENSE.txt file available at
//   https://opensubdiv.org/license.
//

#include "tessellationSession.h"

#include "../far/topologyLevel.h"
#include "../far/topologyRefiner.h"
#include "refinerSurfaceFactory.h"
#include "surface.h"
#include "tessellation.h"
#include "../sdc/options.h"
#include "../sdc/types.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace OpenSubdiv {
namespace OPENSUBDIV_VERSION {

namespace Bfr {

namespace {

typedef std::array<double, 3> Vec3;

Vec3
cross3(Vec3 const & a, Vec3 const & b) {
    return { a[1] * b[2] - a[2] * b[1],
             a[2] * b[0] - a[0] * b[2],
             a[0] * b[1] - a[1] * b[0] };
}

double
dot3(Vec3 const & a, Vec3 const & b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

double
length3(Vec3 const & a) {
    return std::sqrt(dot3(a, a));
}

bool
finite3(Vec3 const & a) {
    return std::isfinite(a[0]) && std::isfinite(a[1]) &&
           std::isfinite(a[2]);
}

constexpr double SharedPositionTolerance = 1e-9;
constexpr double TangentDegeneracyLimit   = 1e-12;

} // unnamed namespace

//
//  All mutable, transactional mesh state lives in State. An Update() is
//  computed on a copy of State and only swapped in on success, so a
//  failed edit cannot consume IDs or advance the revision.
//
class TessellationSession::Impl {
public:
    struct EdgeState {
        int rate = 0;
        //  Canonical interior stations (slots 1 .. rate-1) directed from
        //  the smaller endpoint id toward the larger.
        std::vector<uint64_t> stations;
    };

    struct FaceState {
        int rates[5] = {0, 0, 0, 0, 0};
        //  Position IDs of interior tessellation coords, in coord order.
        std::vector<uint64_t> interior;
        //  Triangle IDs, in facet order.
        std::vector<uint64_t> triangles;
    };

    struct State {
        std::vector<double> positions;                 // 3 doubles per vertex
        std::vector<int> faceRequest;                 // per base face
        std::vector<int> edgeRate;                    // effective per edge
        std::vector<uint64_t> vertexId;               // one site per vertex
        std::vector<EdgeState> edges;
        std::vector<FaceState> faces;
        std::map<uint64_t, int> siteOwner;            // site id -> base face
        std::map<uint64_t, Vec3> livePositions;
        std::map<uint64_t, DeltaTriangle> liveTriangles;
        uint64_t nextPositionId = 1;
        uint64_t nextTriangleId = 1;
        uint64_t revision = 0;
    };

    Far::TopologyRefiner const & _refiner;
    Bfr::RefinerSurfaceFactory<> _factory;

    int _numVertices = 0;
    int _numFaces = 0;
    int _numEdges = 0;

    std::vector<std::array<int, 4>> _faceVertices;
    std::vector<std::array<int, 4>> _faceEdges;
    std::vector<std::array<int, 2>> _edgeVertices;
    std::vector<std::vector<int>> _edgeFaces;

    std::vector<Bfr::Surface<double>> _surfaces;
    std::vector<std::vector<int>> _faceControlPoints;
    //  Support dependency: every face that must be re-evaluated when a
    //  control point moves, including non-incident faces.
    std::vector<std::vector<int>> _vertexToFaces;
    //  Purely topological incident faces, used for site ownership.
    std::vector<std::vector<int>> _vertexIncidentFaces;
    //  True for a boundary corner vertex incident to exactly one face:
    //  such a vertex has a structurally singular first-order frame.
    std::vector<char> _isCornerVertex;

    State _state;
    MeshDelta _initialDelta;
    std::vector<int> _evaluatedFaces;
    std::vector<int> _remeshedFaces;

    Impl(Far::TopologyRefiner const & refiner,
         std::array<double, 3> const * controlPoints, size_t numControlPoints,
         int const * faceRates, size_t numFaceRates);

    uint64_t allocPosition(State & s) { return s.nextPositionId++; }
    uint64_t allocTriangle(State & s) { return s.nextTriangleId++; }

    void effectiveRates(State const & s, int face, int rates[5]) const {
        for (int k = 0; k < 4; ++k) {
            rates[k] = s.edgeRate[_faceEdges[face][k]];
        }
        rates[4] = s.faceRequest[face];
    }

    //  Evaluate one coordinate, enforcing finiteness and a recoverable
    //  tangent frame; returns position and analytic unit normal.
    //
    //  At an open-boundary corner (a one-face vertex under
    //  VTX_BOUNDARY_EDGE_ONLY) the two first parametric tangents are
    //  legitimately collinear, so the first-order cross product vanishes.
    //  The limiting normal is then recovered analytically from the second
    //  derivatives. Elsewhere a first-order tangent-area ratio at or below
    //  1e-12 is a genuine degeneracy and is rejected. A surface whose entire
    //  jet collapses (every candidate cross product is zero) has no normal
    //  and is also rejected.
    void evaluateCoord(int face, double const uv[2],
                       std::vector<double> const & patchPoints,
                       bool structuralCorner,
                       Vec3 & position, Vec3 & normal) const {
        Vec3 du, dv;
        _surfaces[face].Evaluate(uv, patchPoints.data(),
                                 Bfr::Surface<double>::PointDescriptor(3),
                                 position.data(), du.data(), dv.data());
        if (!finite3(position) || !finite3(du) || !finite3(dv)) {
            throw std::runtime_error(
                "TessellationSession: non-finite surface evaluation");
        }
        double lu = length3(du);
        double lv = length3(dv);
        Vec3 cross = cross3(du, dv);
        double lc = length3(cross);
        double tangentArea = (lu > 0.0 && lv > 0.0)
                                 ? lc / (lu * lv)
                                 : 0.0;
        if (tangentArea > TangentDegeneracyLimit) {
            normal = { cross[0] / lc, cross[1] / lc, cross[2] / lc };
            return;
        }
        if (!structuralCorner) {
            throw std::runtime_error(
                "TessellationSession: degenerate tangent frame");
        }

        //  First-order frame is singular: recover the limiting unit normal
        //  from the analytic second derivative jet.
        Vec3 ignored, duu, duv, dvv;
        _surfaces[face].Evaluate(uv, patchPoints.data(),
                                 Bfr::Surface<double>::PointDescriptor(3),
                                 ignored.data(), du.data(), dv.data(),
                                 duu.data(), duv.data(), dvv.data());
        if (!finite3(duu) || !finite3(duv) || !finite3(dvv)) {
            throw std::runtime_error(
                "TessellationSession: non-finite surface evaluation");
        }

        Vec3 const candidates[4] = {
            cross3(du, dvv), cross3(duu, dv),
            cross3(du, duv), cross3(duv, dv)
        };

        Vec3 sum = {0.0, 0.0, 0.0};
        Vec3 first = {0.0, 0.0, 0.0};
        int viable = 0;
        for (Vec3 const & candidate : candidates) {
            double magnitude = length3(candidate);
            if (magnitude == 0.0) continue;
            Vec3 unit = { candidate[0] / magnitude,
                          candidate[1] / magnitude,
                          candidate[2] / magnitude };
            if (viable == 0) {
                first = unit;
            } else if (dot3(first, unit) < 0.0) {
                unit = { -unit[0], -unit[1], -unit[2] };
            }
            sum[0] += unit[0];
            sum[1] += unit[1];
            sum[2] += unit[2];
            ++viable;
        }
        if (viable == 0 || length3(sum) == 0.0) {
            throw std::runtime_error(
                "TessellationSession: degenerate tangent frame");
        }
        double sumLength = length3(sum);
        normal = { sum[0] / sumLength, sum[1] / sumLength,
                   sum[2] / sumLength };
    }

    //  Resolve the canonical (low endpoint -> high endpoint) station of a
    //  base edge reached through local edge @p edgeOfFace of @p face.
    uint64_t resolveBoundarySite(State & s, int face, int edgeOfFace,
                                 int localIndex, int rate) const {
        int edge = _faceEdges[face][edgeOfFace];
        int a = _faceVertices[face][edgeOfFace];
        int b = _faceVertices[face][(edgeOfFace + 1) % 4];
        int v0 = _edgeVertices[edge][0];
        int v1 = _edgeVertices[edge][1];
        if (!((a == v0 && b == v1) || (a == v1 && b == v0))) {
            throw std::runtime_error(
                "TessellationSession: face/edge topology mismatch");
        }

        int canonical;
        if (a == v0) {
            canonical = localIndex;
        } else {
            canonical = rate - localIndex;
        }
        if (canonical == 0) {
            return s.vertexId[v0];
        }
        if (canonical == rate) {
            return s.vertexId[v1];
        }
        uint64_t id = s.edges[edge].stations[canonical - 1];
        if (id == 0) {
            throw std::runtime_error(
                "TessellationSession: missing edge station");
        }
        return id;
    }

    //  Rebuild an edge's station table for a new rate, preserving every
    //  rational station that survives (e.g. the midpoint when 2 -> 4).
    void rebuildEdgeStations(State & s, int edge, int newRate,
                             std::set<uint64_t> & deletedPositions) {
        EdgeState & es = s.edges[edge];
        int oldRate = es.rate;
        std::vector<uint64_t> oldStations = std::move(es.stations);

        es.rate = newRate;
        es.stations.assign(std::max(0, newRate - 1), 0);

        std::set<uint64_t> reused;
        for (int slot = 1; slot < newRate; ++slot) {
            uint64_t kept = 0;
            for (int oldSlot = 1; oldSlot < oldRate; ++oldSlot) {
                if (slot * oldRate == oldSlot * newRate) {
                    kept = oldStations[oldSlot - 1];
                    break;
                }
            }
            if (kept == 0) {
                kept = allocPosition(s);
                s.siteOwner[kept] = edgeOwner(edge);
            } else {
                reused.insert(kept);
            }
            es.stations[slot - 1] = kept;
        }
        for (uint64_t id : oldStations) {
            if (id != 0 && reused.find(id) == reused.end()) {
                deletedPositions.insert(id);
                s.livePositions.erase(id);
                s.siteOwner.erase(id);
            }
        }
    }

    int edgeOwner(int edge) const {
        int owner = _edgeFaces[edge].front();
        for (int f : _edgeFaces[edge]) owner = std::min(owner, f);
        return owner;
    }

    //  Full (re)build of one face's sites and triangles. Used for the
    //  initial build and for remeshed faces.
    void buildFace(int face, State & s,
                   std::map<uint64_t, std::vector<std::pair<int, Vec3>>> &
                       sharedCandidates,
                   std::map<uint64_t, DeltaTriangle> & pendingTriangles,
                   bool isRemesh) {
        Bfr::Surface<double> const & surface = _surfaces[face];

        std::vector<double> patchPoints(surface.GetNumPatchPoints() * 3);
        surface.PreparePatchPoints(
            s.positions.data(),
            Bfr::Surface<double>::PointDescriptor(3),
            patchPoints.data(),
            Bfr::Surface<double>::PointDescriptor(3));

        int rates[5];
        effectiveRates(s, face, rates);

        Bfr::Tessellation tess(surface.GetParameterization(), 5, rates);
        if (!tess.IsValid() || tess.GetFacetSize() != 3) {
            throw std::runtime_error(
                "TessellationSession: invalid tessellation pattern");
        }

        int numCoords = tess.GetNumCoords();
        int numBoundary = tess.GetNumBoundaryCoords();
        std::vector<double> coords(static_cast<size_t>(numCoords) * 2);
        tess.GetCoords(coords.data());

        FaceState & fs = s.faces[face];
        std::copy(rates, rates + 5, fs.rates);

        if (isRemesh) {
            fs.interior.clear();
            fs.triangles.clear();
        }
        size_t numInterior = static_cast<size_t>(numCoords - numBoundary);
        if (fs.interior.size() != numInterior) {
            fs.interior.resize(numInterior);
        }
        for (size_t i = 0; i < numInterior; ++i) {
            if (fs.interior[i] == 0) {
                uint64_t id = allocPosition(s);
                fs.interior[i] = id;
                s.siteOwner[id] = face;
            }
        }

        //  Coord index -> position site id.
        std::vector<uint64_t> coordSite(numCoords, 0);

        int boundaryCursor = 0;
        for (int k = 0; k < 4; ++k) {
            int r = rates[k];
            for (int i = 0; i < r; ++i) {
                int coordIndex = boundaryCursor + i;
                coordSite[coordIndex] =
                    resolveBoundarySite(s, face, k, i, r);
            }
            boundaryCursor += r;
        }
        for (int ci = numBoundary; ci < numCoords; ++ci) {
            coordSite[ci] = fs.interior[ci - numBoundary];
        }

        //  Identify the four corner coordinate indices and whether each is
        //  a one-face boundary corner vertex with a singular first jet.
        std::array<bool, 4> cornerIsStructural{};
        std::array<int, 4> cornerCoordIndex{};
        int cursor = 0;
        for (int k = 0; k < 4; ++k) {
            cornerCoordIndex[k] = cursor;       // first coord of local edge k
            cornerIsStructural[k] =
                _isCornerVertex[_faceVertices[face][k]];
            cursor += rates[k];
        }
        auto isStructuralCornerCoord = [&](int coordIndex) {
            for (int k = 0; k < 4; ++k) {
                if (coordIndex == cornerCoordIndex[k] &&
                    cornerIsStructural[k]) {
                    return true;
                }
            }
            return false;
        };

        //  Evaluate every coordinate: positions and analytic normals.
        std::vector<Vec3> coordPosition(numCoords);
        std::vector<Vec3> coordNormal(numCoords);
        for (int ci = 0; ci < numCoords; ++ci) {
            double uv[2] = { coords[2 * ci], coords[2 * ci + 1] };
            evaluateCoord(face, uv, patchPoints,
                          isStructuralCornerCoord(ci),
                          coordPosition[ci], coordNormal[ci]);
            sharedCandidates[coordSite[ci]].push_back(
                std::make_pair(face, coordPosition[ci]));
        }

        //  Facets -> triangle records.
        int numFacets = tess.GetNumFacets();
        std::vector<int> facets(static_cast<size_t>(numFacets) * 3);
        tess.GetFacets(facets.data());

        if (static_cast<int>(fs.triangles.size()) != numFacets) {
            fs.triangles.resize(numFacets);
        }
        for (int tri = 0; tri < numFacets; ++tri) {
            int c0 = facets[3 * tri];
            int c1 = facets[3 * tri + 1];
            int c2 = facets[3 * tri + 2];
            if (c0 < 0 || c1 < 0 || c2 < 0 ||
                c0 >= numCoords || c1 >= numCoords || c2 >= numCoords) {
                throw std::runtime_error(
                    "TessellationSession: facet coordinate out of range");
            }
            uint64_t id = fs.triangles[tri];
            if (id == 0) {
                id = allocTriangle(s);
                fs.triangles[tri] = id;
            }
            DeltaTriangle rec;
            rec.id = id;
            rec.face = static_cast<uint32_t>(face);
            rec.positionIds = { coordSite[c0], coordSite[c1], coordSite[c2] };
            int corners[3] = { c0, c1, c2 };
            for (int q = 0; q < 3; ++q) {
                int ci = corners[q];
                rec.cornerUV[q] = { coords[2 * ci], coords[2 * ci + 1] };
                rec.cornerNormal[q] = coordNormal[ci];
            }
            pendingTriangles[id] = rec;
        }
    }

    void buildInitial() {
        State & s = _state;
        s.revision = 0;

        //  Effective edge rates: maximum request over incident faces.
        s.edgeRate.assign(_numEdges, 1);
        for (int e = 0; e < _numEdges; ++e) {
            int rate = 1;
            for (int f : _edgeFaces[e]) {
                rate = std::max(rate, s.faceRequest[f]);
            }
            s.edgeRate[e] = rate;
        }

        //  Vertex sites.
        s.vertexId.assign(_numVertices, 0);
        for (int v = 0; v < _numVertices; ++v) {
            uint64_t id = allocPosition(s);
            s.vertexId[v] = id;
            int owner = _vertexIncidentFaces[v].front();
            for (int f : _vertexIncidentFaces[v]) owner = std::min(owner, f);
            s.siteOwner[id] = owner;
        }

        //  Edge sites at the initial rates.
        s.edges.resize(_numEdges);
        for (int e = 0; e < _numEdges; ++e) {
            int rate = s.edgeRate[e];
            s.edges[e].rate = rate;
            s.edges[e].stations.resize(std::max(0, rate - 1));
            int owner = edgeOwner(e);
            for (int slot = 1; slot < rate; ++slot) {
                uint64_t id = allocPosition(s);
                s.edges[e].stations[slot - 1] = id;
                s.siteOwner[id] = owner;
            }
        }

        s.faces.resize(_numFaces);
        std::map<uint64_t, std::vector<std::pair<int, Vec3>>> candidates;
        std::map<uint64_t, DeltaTriangle> pendingTriangles;
        for (int f = 0; f < _numFaces; ++f) {
            buildFace(f, s, candidates, pendingTriangles, false);
        }
        finalizePositions(s, candidates);
        commitTriangles(s, pendingTriangles, std::set<uint64_t>());

        s.revision = 1;
        _initialDelta.fromRevision = 0;
        _initialDelta.toRevision = 1;
        _initialDelta.deletedTriangles.clear();
        _initialDelta.deletedPositions.clear();
        _initialDelta.positionUpserts.clear();
        _initialDelta.triangleUpserts.clear();
        for (auto const & kv : s.livePositions) {
            DeltaPosition rec;
            rec.id = kv.first;
            rec.position = kv.second;
            _initialDelta.positionUpserts.push_back(rec);
        }
        for (auto const & kv : s.liveTriangles) {
            _initialDelta.triangleUpserts.push_back(kv.second);
        }
    }

    //  Resolve shared-site candidates against the owning face, enforcing
    //  the 1e-9 agreement, then publish only positions that changed.
    void finalizePositions(
        State & s,
        std::map<uint64_t, std::vector<std::pair<int, Vec3>>> const &
            candidates) {

        for (auto const & kv : candidates) {
            uint64_t id = kv.first;
            auto ownerIt = s.siteOwner.find(id);
            if (ownerIt == s.siteOwner.end()) {
                throw std::runtime_error(
                    "TessellationSession: site without owner");
            }
            int owner = ownerIt->second;

            Vec3 chosen;
            bool haveChosen = false;
            for (auto const & faceValue : kv.second) {
                if (faceValue.first == owner) {
                    chosen = faceValue.second;
                    haveChosen = true;
                    break;
                }
            }
            if (!haveChosen) {
                auto existing = s.livePositions.find(id);
                if (existing == s.livePositions.end()) {
                    throw std::runtime_error(
                        "TessellationSession: owner did not evaluate site");
                }
                chosen = existing->second;
            }

            for (auto const & faceValue : kv.second) {
                Vec3 d = {
                    faceValue.second[0] - chosen[0],
                    faceValue.second[1] - chosen[1],
                    faceValue.second[2] - chosen[2]
                };
                if (length3(d) > SharedPositionTolerance) {
                    throw std::runtime_error(
                        "TessellationSession: shared positions disagree by "
                        "more than 1e-9");
                }
            }

            auto existing = s.livePositions.find(id);
            if (existing == s.livePositions.end()) {
                s.livePositions[id] = chosen;
            } else if (existing->second != chosen) {
                existing->second = chosen;
            }
        }
    }

    void commitTriangles(State & s,
                         std::map<uint64_t, DeltaTriangle> const & pending,
                         std::set<uint64_t> const & deletedTriangles) {
        for (auto const & kv : pending) {
            if (deletedTriangles.find(kv.first) != deletedTriangles.end()) {
                throw std::runtime_error(
                    "TessellationSession: internal ID lifecycle error");
            }
            s.liveTriangles[kv.first] = kv.second;
        }
    }

    MeshDelta runUpdate(std::vector<PositionEdit> const & pointEdits,
                        std::vector<RateEdit> const & rateEdits) {
        //  Validate before touching any state.
        std::set<int> changedVertices;
        std::set<int> changedRateFaces;
        {
            std::set<int> seenPoints;
            for (PositionEdit const & edit : pointEdits) {
                if (edit.index < 0 || edit.index >= _numVertices) {
                    throw std::invalid_argument(
                        "TessellationSession: control point index out of range");
                }
                if (!seenPoints.insert(edit.index).second) {
                    throw std::invalid_argument(
                        "TessellationSession: duplicate control point edit");
                }
                if (!finite3(edit.position)) {
                    throw std::invalid_argument(
                        "TessellationSession: non-finite control point");
                }
                double const * current =
                    _state.positions.data() + 3 * edit.index;
                if (current[0] != edit.position[0] ||
                    current[1] != edit.position[1] ||
                    current[2] != edit.position[2]) {
                    changedVertices.insert(edit.index);
                }
            }
            std::set<int> seenFaces;
            for (RateEdit const & edit : rateEdits) {
                if (edit.face < 0 || edit.face >= _numFaces) {
                    throw std::invalid_argument(
                        "TessellationSession: face index out of range");
                }
                if (!seenFaces.insert(edit.face).second) {
                    throw std::invalid_argument(
                        "TessellationSession: duplicate rate edit");
                }
                if (edit.rate < 1 || edit.rate > 32) {
                    throw std::invalid_argument(
                        "TessellationSession: rate must be within [1, 32]");
                }
                if (_state.faceRequest[edit.face] != edit.rate) {
                    changedRateFaces.insert(edit.face);
                }
            }
        }

        MeshDelta noop;
        noop.fromRevision = _state.revision;
        noop.toRevision = _state.revision;
        if (changedVertices.empty() && changedRateFaces.empty()) {
            _evaluatedFaces.clear();
            _remeshedFaces.clear();
            return noop;
        }

        //  All work happens on a disposable copy.
        State scratch = _state;

        for (PositionEdit const & edit : pointEdits) {
            scratch.positions[3 * edit.index]     = edit.position[0];
            scratch.positions[3 * edit.index + 1] = edit.position[1];
            scratch.positions[3 * edit.index + 2] = edit.position[2];
        }
        for (RateEdit const & edit : rateEdits) {
            scratch.faceRequest[edit.face] = edit.rate;
        }

        //  Recompute effective rates only on edges touched by a rate edit.
        std::set<int> changedEdges;
        for (int f : changedRateFaces) {
            for (int k = 0; k < 4; ++k) {
                int edge = _faceEdges[f][k];
                int newRate = 1;
                for (int g : _edgeFaces[edge]) {
                    newRate = std::max(newRate, scratch.faceRequest[g]);
                }
                if (newRate != scratch.edgeRate[edge]) {
                    changedEdges.insert(edge);
                    scratch.edgeRate[edge] = newRate;
                }
            }
        }

        std::set<uint64_t> deletedPositions;
        for (int edge : changedEdges) {
            rebuildEdgeStations(scratch, edge, scratch.edgeRate[edge],
                                deletedPositions);
        }

        //  Remeshed faces: any face whose five effective rates changed.
        std::set<int> remeshedSet;
        std::set<int> rateCandidateFaces = changedRateFaces;
        for (int edge : changedEdges) {
            for (int f : _edgeFaces[edge]) {
                rateCandidateFaces.insert(f);
            }
        }
        for (int f : rateCandidateFaces) {
            int rates[5];
            effectiveRates(scratch, f, rates);
            FaceState const & fs = scratch.faces[f];
            for (int k = 0; k < 5; ++k) {
                if (fs.rates[k] != rates[k]) {
                    remeshedSet.insert(f);
                    break;
                }
            }
        }

        //  Evaluated faces: support set of changed points union remeshed.
        std::set<int> evaluatedSet = remeshedSet;
        for (int v : changedVertices) {
            for (int f : _vertexToFaces[v]) {
                evaluatedSet.insert(f);
            }
        }

        std::set<uint64_t> deletedTriangles;
        for (int f : remeshedSet) {
            FaceState & fs = scratch.faces[f];
            for (uint64_t id : fs.triangles) {
                if (id != 0) deletedTriangles.insert(id);
            }
            for (uint64_t id : fs.interior) {
                if (id != 0) deletedPositions.insert(id);
            }
            for (uint64_t id : fs.triangles) {
                if (id != 0) scratch.liveTriangles.erase(id);
            }
            for (uint64_t id : fs.interior) {
                if (id != 0) {
                    scratch.livePositions.erase(id);
                    scratch.siteOwner.erase(id);
                }
            }
            fs.triangles.clear();
            fs.interior.clear();
        }

        std::map<uint64_t, std::vector<std::pair<int, Vec3>>> candidates;
        std::map<uint64_t, DeltaTriangle> pendingTriangles;
        for (int f : evaluatedSet) {
            buildFace(f, scratch, candidates, pendingTriangles,
                      remeshedSet.find(f) != remeshedSet.end());
        }

        finalizePositions(scratch, candidates);

        //  Publish triangle upserts only when the record actually changed.
        std::map<uint64_t, DeltaTriangle> triangleUpserts;
        for (auto const & kv : pendingTriangles) {
            auto old = scratch.liveTriangles.find(kv.first);
            if (old == scratch.liveTriangles.end() ||
                old->second.face != kv.second.face ||
                old->second.positionIds != kv.second.positionIds ||
                old->second.cornerUV != kv.second.cornerUV ||
                old->second.cornerNormal != kv.second.cornerNormal) {
                triangleUpserts.insert(kv);
            }
        }
        commitTriangles(scratch, pendingTriangles, deletedTriangles);

        //  Position upserts: positions that are new or numerically changed.
        std::map<uint64_t, DeltaPosition> positionUpserts;
        for (auto const & kv : candidates) {
            uint64_t id = kv.first;
            if (deletedPositions.find(id) != deletedPositions.end()) {
                throw std::runtime_error(
                    "TessellationSession: internal position lifecycle error");
            }
            Vec3 const & now = scratch.livePositions.at(id);
            auto before = _state.livePositions.find(id);
            if (before == _state.livePositions.end() ||
                before->second != now) {
                DeltaPosition rec;
                rec.id = id;
                rec.position = now;
                positionUpserts[id] = rec;
            }
        }

        //  Assemble the strictly sorted delta.
        MeshDelta delta;
        delta.fromRevision = _state.revision;
        delta.toRevision = _state.revision + 1;
        delta.deletedTriangles.assign(deletedTriangles.begin(),
                                      deletedTriangles.end());
        delta.deletedPositions.assign(deletedPositions.begin(),
                                      deletedPositions.end());
        for (auto const & kv : positionUpserts) {
            delta.positionUpserts.push_back(kv.second);
        }
        for (auto const & kv : triangleUpserts) {
            delta.triangleUpserts.push_back(kv.second);
        }

        //  Commit the transaction.
        scratch.revision = _state.revision + 1;
        _state = std::move(scratch);

        _evaluatedFaces.assign(evaluatedSet.begin(), evaluatedSet.end());
        _remeshedFaces.assign(remeshedSet.begin(), remeshedSet.end());
        return delta;
    }

    MeshSnapshot snapshot() const {
        MeshSnapshot out;
        out.revision = _state.revision;
        for (auto const & kv : _state.livePositions) {
            DeltaPosition rec;
            rec.id = kv.first;
            rec.position = kv.second;
            out.positions.push_back(rec);
        }
        for (auto const & kv : _state.liveTriangles) {
            out.triangles.push_back(kv.second);
        }
        return out;
    }
};

TessellationSession::Impl::Impl(
    Far::TopologyRefiner const & refiner,
    std::array<double, 3> const * controlPoints, size_t numControlPoints,
    int const * faceRates, size_t numFaceRates)
        : _refiner(refiner), _factory(refiner) {

    if (controlPoints == nullptr || faceRates == nullptr) {
        throw std::invalid_argument(
            "TessellationSession: null input array");
    }
    if (refiner.GetSchemeType() != Sdc::SCHEME_CATMARK) {
        throw std::invalid_argument(
            "TessellationSession: only Catmull-Clark cages are supported");
    }

    Far::TopologyLevel const & level = refiner.GetLevel(0);
    _numVertices = level.GetNumVertices();
    _numFaces = level.GetNumFaces();
    _numEdges = level.GetNumEdges();

    if (static_cast<int>(numControlPoints) != _numVertices) {
        throw std::invalid_argument(
            "TessellationSession: control point array length mismatch");
    }
    if (static_cast<int>(numFaceRates) != _numFaces) {
        throw std::invalid_argument(
            "TessellationSession: face rate array length mismatch");
    }

    _state.positions.resize(static_cast<size_t>(_numVertices) * 3);
    for (int v = 0; v < _numVertices; ++v) {
        if (!finite3(controlPoints[v])) {
            throw std::invalid_argument(
                "TessellationSession: non-finite control point");
        }
        _state.positions[3 * v]     = controlPoints[v][0];
        _state.positions[3 * v + 1] = controlPoints[v][1];
        _state.positions[3 * v + 2] = controlPoints[v][2];
    }

    _state.faceRequest.resize(_numFaces);
    for (int f = 0; f < _numFaces; ++f) {
        if (faceRates[f] < 1 || faceRates[f] > 32) {
            throw std::invalid_argument(
                "TessellationSession: rate must be within [1, 32]");
        }
        _state.faceRequest[f] = faceRates[f];
    }

    //  Cache base topology relationships.
    _faceVertices.resize(_numFaces);
    _faceEdges.resize(_numFaces);
    for (int f = 0; f < _numFaces; ++f) {
        Far::ConstIndexArray fv = level.GetFaceVertices(f);
        Far::ConstIndexArray fe = level.GetFaceEdges(f);
        if (fv.size() != 4 || fe.size() != 4) {
            throw std::invalid_argument(
                "TessellationSession: only quad faces are supported");
        }
        for (int k = 0; k < 4; ++k) {
            _faceVertices[f][k] = fv[k];
            _faceEdges[f][k] = fe[k];
        }
    }
    _edgeVertices.resize(_numEdges);
    _edgeFaces.resize(_numEdges);
    for (int e = 0; e < _numEdges; ++e) {
        Far::ConstIndexArray ev = level.GetEdgeVertices(e);
        Far::ConstIndexArray ef = level.GetEdgeFaces(e);
        if (ev.size() != 2 || ef.empty() || ef.size() > 2) {
            throw std::invalid_argument(
                "TessellationSession: edges must be manifold with one or "
                "two incident faces");
        }
        int a = ev[0], b = ev[1];
        if (a > b) std::swap(a, b);
        _edgeVertices[e] = { a, b };
        for (int i = 0; i < ef.size(); ++i) {
            _edgeFaces[e].push_back(ef[i]);
        }
    }

    //  Purely topological incidence for site ownership.
    _vertexIncidentFaces.resize(_numVertices);
    _isCornerVertex.assign(_numVertices, 0);
    for (int v = 0; v < _numVertices; ++v) {
        Far::ConstIndexArray vf = level.GetVertexFaces(v);
        _vertexIncidentFaces[v].reserve(vf.size());
        for (int i = 0; i < vf.size(); ++i) {
            _vertexIncidentFaces[v].push_back(vf[i]);
        }
        std::sort(_vertexIncidentFaces[v].begin(),
                  _vertexIncidentFaces[v].end());
        if (vf.size() == 1) _isCornerVertex[v] = 1;
    }

    //  Compile one Surface per face once; topology never changes.
    _surfaces.resize(_numFaces);
    _faceControlPoints.resize(_numFaces);
    _vertexToFaces.resize(_numVertices);
    for (int f = 0; f < _numFaces; ++f) {
        if (!_factory.InitVertexSurface(f, &_surfaces[f]) ||
            !_surfaces[f].IsValid()) {
            throw std::runtime_error(
                "TessellationSession: failed to compile a face Surface");
        }
        int numCVs = _surfaces[f].GetNumControlPoints();
        _faceControlPoints[f].resize(numCVs);
        _surfaces[f].GetControlPointIndices(_faceControlPoints[f].data());
        for (int cv : _faceControlPoints[f]) {
            if (cv < 0 || cv >= _numVertices) {
                throw std::runtime_error(
                    "TessellationSession: invalid control point index");
            }
            _vertexToFaces[cv].push_back(f);
        }
    }
    for (int v = 0; v < _numVertices; ++v) {
        std::sort(_vertexToFaces[v].begin(), _vertexToFaces[v].end());
        _vertexToFaces[v].erase(
            std::unique(_vertexToFaces[v].begin(),
                        _vertexToFaces[v].end()),
            _vertexToFaces[v].end());
    }

    buildInitial();
}

//
//  Public forwarding interface.
//
TessellationSession::TessellationSession(
    Far::TopologyRefiner const & refiner,
    std::array<double, 3> const * controlPoints, size_t numControlPoints,
    int const * faceRates, size_t numFaceRates)
        : _impl(std::make_unique<Impl>(refiner, controlPoints,
                                       numControlPoints,
                                       faceRates, numFaceRates)) { }

TessellationSession::~TessellationSession() = default;

int
TessellationSession::GetNumFaces() const {
    return _impl->_numFaces;
}

int
TessellationSession::GetNumControlPoints() const {
    return _impl->_numVertices;
}

uint64_t
TessellationSession::Revision() const {
    return _impl->_state.revision;
}

MeshDelta const &
TessellationSession::InitialDelta() const {
    return _impl->_initialDelta;
}

MeshDelta
TessellationSession::Update(std::vector<PositionEdit> const & pointEdits,
                            std::vector<RateEdit> const & rateEdits) {
    return _impl->runUpdate(pointEdits, rateEdits);
}

std::vector<int> const &
TessellationSession::EvaluatedFaces() const {
    return _impl->_evaluatedFaces;
}

std::vector<int> const &
TessellationSession::RemeshedFaces() const {
    return _impl->_remeshedFaces;
}

uint64_t
TessellationSession::NextPositionId() const {
    return _impl->_state.nextPositionId;
}

uint64_t
TessellationSession::NextTriangleId() const {
    return _impl->_state.nextTriangleId;
}

MeshSnapshot
TessellationSession::Snapshot() const {
    return _impl->snapshot();
}

} // end namespace Bfr

} // end namespace OPENSUBDIV_VERSION
using namespace OPENSUBDIV_VERSION;
} // end namespace OpenSubdiv
