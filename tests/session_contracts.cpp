//
//   Copyright 2021 Pixar
//
//   Licensed under the terms set forth in the LICENSE.txt file available at
//   https://opensubdiv.org/license.
//

//  Contract tests for the incremental tessellation session and the
//  topology-free mesh delta/mirror production path.
//
//  Three groups are registered with the task driver: dependency,
//  retessellation and replay. Every geometric reference is rebuilt
//  directly from the existing Bfr classes at the final control points
//  and rates -- never from the two new entry points.

#include "task_support/check.h"

#include <opensubdiv/far/topologyDescriptor.h>
#include <opensubdiv/far/topologyLevel.h>
#include <opensubdiv/far/topologyRefinerFactory.h>
#include <opensubdiv/bfr/refinerSurfaceFactory.h>
#include <opensubdiv/bfr/surface.h>
#include <opensubdiv/bfr/tessellation.h>
#include <opensubdiv/bfr/tessellationSession.h>
#include <opensubdiv/bfr/meshDelta.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace OpenSubdiv;
using namespace OpenSubdiv::OPENSUBDIV_VERSION;
using namespace OpenSubdiv::OPENSUBDIV_VERSION::Bfr;

namespace session_contracts {

typedef std::array<double, 3> Vec3;
typedef std::array<double, 2> Vec2;

constexpr double PosEps = 1e-9;
constexpr double NrmEps = 1e-8;

Vec3 cross3(Vec3 const & a, Vec3 const & b) {
    return { a[1] * b[2] - a[2] * b[1],
             a[2] * b[0] - a[0] * b[2],
             a[0] * b[1] - a[1] * b[0] };
}
double len3(Vec3 const & a) {
    return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
}

//  Robust analytic normal identical in rule to the production session:
//  first-order cross product when well conditioned, otherwise the
//  second-derivative jet limit at a structural boundary corner.
Vec3 robustNormal(Bfr::Surface<double> const & surface,
                  double const uv[2], std::vector<double> const & patch,
                  bool structuralCorner) {
    Vec3 p, du, dv;
    surface.Evaluate(uv, patch.data(),
                     Bfr::Surface<double>::PointDescriptor(3),
                     p.data(), du.data(), dv.data());
    Vec3 c = cross3(du, dv);
    double lu = len3(du), lv = len3(dv), lc = len3(c);
    if (lu > 0.0 && lv > 0.0 && lc / (lu * lv) > 1e-12) {
        return { c[0] / lc, c[1] / lc, c[2] / lc };
    }
    if (!structuralCorner) {
        throw std::runtime_error("reference: non-corner tangent degeneracy");
    }
    Vec3 q, duu, duv, dvv;
    surface.Evaluate(uv, patch.data(),
                     Bfr::Surface<double>::PointDescriptor(3),
                     q.data(), du.data(), dv.data(),
                     duu.data(), duv.data(), dvv.data());
    Vec3 const cand[4] = { cross3(du, dvv), cross3(duu, dv),
                           cross3(du, duv), cross3(duv, dv) };
    Vec3 sum{0, 0, 0}, first{0, 0, 0};
    int viable = 0;
    for (Vec3 const & x : cand) {
        double m = len3(x);
        if (m == 0.0) continue;
        Vec3 u{ x[0] / m, x[1] / m, x[2] / m };
        if (viable == 0) first = u;
        else if (first[0] * u[0] + first[1] * u[1] + first[2] * u[2] < 0.0)
            u = { -u[0], -u[1], -u[2] };
        sum = { sum[0] + u[0], sum[1] + u[1], sum[2] + u[2] };
        ++viable;
    }
    if (viable == 0) throw std::runtime_error("reference: collapsed jet");
    double sm = len3(sum);
    return { sum[0] / sm, sum[1] / sm, sum[2] / sm };
}

struct Cage {
    std::vector<Vec3> positions;
    std::vector<int> faceVerts;   // 4 ints per face
    std::vector<int> faceCounts;
    int numVertices = 0;
    int numFaces = 0;
};

Cage makeTorus(int m, int n, double tube = 0.6, double major = 2.0) {
    Cage cage;
    cage.numVertices = m * n;
    cage.numFaces = m * n;
    cage.faceCounts.assign(cage.numFaces, 4);
    double pi = std::acos(-1.0);
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < m; ++i) {
            double u = 2.0 * pi * i / m;
            double v = 2.0 * pi * j / n;
            cage.positions.push_back({
                (major + tube * std::cos(v)) * std::cos(u),
                (major + tube * std::cos(v)) * std::sin(u),
                tube * std::sin(v) });
        }
    }
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < m; ++i) {
            cage.faceVerts.insert(cage.faceVerts.end(), {
                j * m + i,
                j * m + (i + 1) % m,
                ((j + 1) % n) * m + (i + 1) % m,
                ((j + 1) % n) * m + i });
        }
    }
    return cage;
}

//  A curved, open grid sheet: every interior vertex has valence 4 while
//  boundary vertices are open fans of one or two faces.
Cage makeOpenSheet(int m, int n) {
    Cage cage;
    cage.numVertices = m * n;
    cage.numFaces = (m - 1) * (n - 1);
    cage.faceCounts.assign(cage.numFaces, 4);
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < m; ++i) {
            double x = static_cast<double>(i) / (m - 1);
            double y = static_cast<double>(j) / (n - 1);
            cage.positions.push_back({
                x * 4.0 - 2.0,
                y * 4.0 - 2.0,
                0.7 * std::sin(2.0 * x) * std::cos(2.0 * y) });
        }
    }
    for (int j = 0; j < n - 1; ++j) {
        for (int i = 0; i < m - 1; ++i) {
            int a = j * m + i;
            cage.faceVerts.insert(cage.faceVerts.end(),
                { a, a + 1, a + 1 + m, a + m });
        }
    }
    return cage;
}

//  Two disconnected components merged into one refiner, exercising the
//  rule that independent components are never welded by coordinates.
Cage makeTwoComponents() {
    Cage a = makeTorus(5, 5);
    Cage b = makeOpenSheet(4, 4);
    Cage out;
    out.numVertices = a.numVertices + b.numVertices;
    out.numFaces = a.numFaces + b.numFaces;
    out.faceCounts.assign(out.numFaces, 4);
    out.positions = a.positions;
    for (auto p : b.positions) {
        p[0] += 9.0;  // separate the components spatially
        out.positions.push_back(p);
    }
    out.faceVerts = a.faceVerts;
    for (int idx : b.faceVerts) out.faceVerts.push_back(idx + a.numVertices);
    return out;
}

std::unique_ptr<Far::TopologyRefiner>
makeRefiner(Cage const & cage) {
    Far::TopologyDescriptor desc;
    desc.numVertices = cage.numVertices;
    desc.numFaces = cage.numFaces;
    desc.numVertsPerFace = cage.faceCounts.data();
    desc.vertIndicesPerFace = cage.faceVerts.data();

    auto options = Far::TopologyRefinerFactory<Far::TopologyDescriptor>::
        Options(Sdc::SCHEME_CATMARK);
    options.schemeOptions.SetVtxBoundaryInterpolation(
        Sdc::Options::VTX_BOUNDARY_EDGE_ONLY);
    return std::unique_ptr<Far::TopologyRefiner>(
        Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Create(
            desc, options));
}

std::vector<double> flatten(Cage const & cage) {
    std::vector<double> flat;
    flat.reserve(static_cast<size_t>(cage.numVertices) * 3);
    for (Vec3 const & p : cage.positions) {
        flat.insert(flat.end(), p.begin(), p.end());
    }
    return flat;
}

// ---------------------------------------------------------------------
//  Independent reference tessellation built only from stock Bfr.
// ---------------------------------------------------------------------
struct ReferenceFace {
    std::vector<Vec2> uv;          // one per tessellation coord
    std::vector<Vec3> position;
    std::vector<Vec3> normal;
    std::vector<std::array<int, 3>> facets;
};

struct ReferenceMesh {
    std::vector<ReferenceFace> faces;
};

ReferenceMesh
buildReference(Far::TopologyRefiner const & refiner,
               std::vector<Vec3> const & positions,
               std::vector<int> const & requests) {
    Far::TopologyLevel const & level = refiner.GetLevel(0);
    int numFaces = level.GetNumFaces();

    std::vector<int> edgeRate(level.GetNumEdges(), 1);
    for (int e = 0; e < level.GetNumEdges(); ++e) {
        Far::ConstIndexArray ef = level.GetEdgeFaces(e);
        for (int i = 0; i < ef.size(); ++i) {
            edgeRate[e] = std::max(edgeRate[e], requests[ef[i]]);
        }
    }

    Bfr::RefinerSurfaceFactory<> factory(refiner);
    ReferenceMesh ref;
    ref.faces.resize(numFaces);

    for (int f = 0; f < numFaces; ++f) {
        Bfr::Surface<double> surface;
        if (!factory.InitVertexSurface(f, &surface)) {
            throw std::runtime_error("reference: surface init failed");
        }
        std::vector<double> patch(surface.GetNumPatchPoints() * 3);
        surface.PreparePatchPoints(
            positions.data()->data(),
            Bfr::Surface<double>::PointDescriptor(3),
            patch.data(),
            Bfr::Surface<double>::PointDescriptor(3));

        Far::ConstIndexArray fe = level.GetFaceEdges(f);
        int rates[5];
        for (int k = 0; k < 4; ++k) rates[k] = edgeRate[fe[k]];
        rates[4] = requests[f];

        Bfr::Tessellation tess(surface.GetParameterization(), 5, rates);
        if (!tess.IsValid() || tess.GetFacetSize() != 3) {
            throw std::runtime_error("reference: invalid tessellation");
        }
        ReferenceFace & rf = ref.faces[f];
        int numCoords = tess.GetNumCoords();
        std::vector<double> coords(static_cast<size_t>(numCoords) * 2);
        tess.GetCoords(coords.data());
        rf.uv.resize(numCoords);
        rf.position.resize(numCoords);
        rf.normal.resize(numCoords);

        Far::ConstIndexArray fv = level.GetFaceVertices(f);
        std::array<int, 4> cornerCoord{};
        std::array<bool, 4> cornerStructural{};
        int scan = 0;
        for (int k = 0; k < 4; ++k) {
            cornerCoord[k] = scan;
            cornerStructural[k] =
                level.GetVertexFaces(fv[k]).size() == 1;
            scan += rates[k];
        }
        auto structuralAt = [&](int coordIndex) {
            for (int k = 0; k < 4; ++k) {
                if (coordIndex == cornerCoord[k] && cornerStructural[k]) {
                    return true;
                }
            }
            return false;
        };

        for (int c = 0; c < numCoords; ++c) {
            rf.uv[c] = { coords[2 * c], coords[2 * c + 1] };
            Vec3 p, du, dv;
            surface.Evaluate(coords.data() + 2 * c, patch.data(),
                             Bfr::Surface<double>::PointDescriptor(3),
                             p.data(), du.data(), dv.data());
            rf.position[c] = p;
            rf.normal[c] = robustNormal(surface, coords.data() + 2 * c,
                                        patch, structuralAt(c));
        }
        int numFacets = tess.GetNumFacets();
        std::vector<int> facetVals(static_cast<size_t>(numFacets) * 3);
        tess.GetFacets(facetVals.data());
        rf.facets.resize(numFacets);
        for (int t = 0; t < numFacets; ++t) {
            rf.facets[t] = { facetVals[3 * t],
                             facetVals[3 * t + 1],
                             facetVals[3 * t + 2] };
        }
    }
    return ref;
}

bool sameUV(Vec2 const & a, Vec2 const & b) {
    return a[0] == b[0] && a[1] == b[1];
}

//  Compare a session snapshot against the independent Bfr reference:
//  positions, per-corner UV/normals, facet coverage and winding.
bool compareSnapshot(Far::TopologyRefiner const & refiner,
                     std::vector<Vec3> const & positions,
                     std::vector<int> const & requests,
                     MeshSnapshot const & snapshot) {
    ReferenceMesh ref = buildReference(refiner, positions, requests);
    Far::TopologyLevel const & level = refiner.GetLevel(0);

    //  (face, uv) -> observed session position id, with consistency check.
    std::map<std::pair<int, Vec2>, uint64_t> cornerSite;
    auto uvKey = [](int f, Vec2 uv) {
        return std::make_pair(f, uv);
    };

    //  Reference facet UV-triple sets per face and session equivalents.
    std::vector<std::set<std::array<Vec2, 3>>> refFacets(level.GetNumFaces());
    std::vector<std::set<std::array<Vec2, 3>>> gotFacets(level.GetNumFaces());
    for (int f = 0; f < level.GetNumFaces(); ++f) {
        for (auto const & facet : ref.faces[f].facets) {
            refFacets[f].insert({
                ref.faces[f].uv[facet[0]],
                ref.faces[f].uv[facet[1]],
                ref.faces[f].uv[facet[2]] });
        }
    }

    //  Index reference coords by (face, exact uv).
    std::map<std::pair<int, Vec2>, int> refCoord;
    for (int f = 0; f < level.GetNumFaces(); ++f) {
        for (int c = 0; c < static_cast<int>(ref.faces[f].uv.size()); ++c) {
            refCoord[uvKey(f, ref.faces[f].uv[c])] = c;
        }
    }

    for (DeltaTriangle const & tri : snapshot.triangles) {
        int f = static_cast<int>(tri.face);
        if (f < 0 || f >= level.GetNumFaces()) return false;
        std::array<Vec2, 3> triUV;
        for (int q = 0; q < 3; ++q) {
            Vec2 uv = tri.cornerUV[q];
            triUV[q] = uv;
            auto it = refCoord.find(uvKey(f, uv));
            if (it == refCoord.end()) return false;
            int c = it->second;

            Vec3 const & refP = ref.faces[f].position[c];
            Vec3 const & refN = ref.faces[f].normal[c];
            for (int k = 0; k < 3; ++k) {
                if (std::abs(tri.cornerNormal[q][k] - refN[k]) > NrmEps) {
                    return false;
                }
            }
            //  Resolve the shared live position for this corner and check.
            auto posIt = std::lower_bound(
                snapshot.positions.begin(), snapshot.positions.end(),
                tri.positionIds[q],
                [](DeltaPosition const & rec, uint64_t id) {
                    return rec.id < id;
                });
            if (posIt == snapshot.positions.end() ||
                posIt->id != tri.positionIds[q]) {
                return false;
            }
            for (int k = 0; k < 3; ++k) {
                if (std::abs(posIt->position[k] - refP[k]) > PosEps) {
                    return false;
                }
            }
            auto seen = cornerSite.find(uvKey(f, uv));
            if (seen == cornerSite.end()) {
                cornerSite[uvKey(f, uv)] = tri.positionIds[q];
            } else if (seen->second != tri.positionIds[q]) {
                return false;  // inconsistent sharing inside one face
            }
        }
        gotFacets[f].insert(triUV);
    }

    for (int f = 0; f < level.GetNumFaces(); ++f) {
        if (gotFacets[f] != refFacets[f]) return false;  // coverage/winding
    }

    //  Every reference coord must be claimed by some session corner.
    for (int f = 0; f < level.GetNumFaces(); ++f) {
        for (Vec2 const & uv : ref.faces[f].uv) {
            if (cornerSite.find(uvKey(f, uv)) == cornerSite.end()) {
                return false;
            }
        }
    }
    return true;
}

//  Verify shared edges: both incident faces resolve the same ordered
//  sequence of position ids and the same number of sub-edges.
bool verifySharedEdges(Far::TopologyRefiner const & refiner,
                       std::vector<int> const & requests,
                       MeshSnapshot const & snapshot) {
    Far::TopologyLevel const & level = refiner.GetLevel(0);

    std::map<std::pair<int, Vec2>, uint64_t> cornerSite;
    for (DeltaTriangle const & tri : snapshot.triangles) {
        for (int q = 0; q < 3; ++q) {
            cornerSite[std::make_pair(static_cast<int>(tri.face),
                                      Vec2(tri.cornerUV[q]))] =
                tri.positionIds[q];
        }
    }

    Bfr::RefinerSurfaceFactory<> factory(refiner);

    for (int e = 0; e < level.GetNumEdges(); ++e) {
        Far::ConstIndexArray ev = level.GetEdgeVertices(e);
        Far::ConstIndexArray ef = level.GetEdgeFaces(e);
        int rate = 1;
        for (int i = 0; i < ef.size(); ++i) {
            rate = std::max(rate, requests[ef[i]]);
        }
        int v0 = std::min(ev[0], ev[1]);
        int v1 = std::max(ev[0], ev[1]);

        std::vector<std::vector<uint64_t>> faceSequences;
        for (int i = 0; i < ef.size(); ++i) {
            int f = ef[i];
            Far::ConstIndexArray fv = level.GetFaceVertices(f);
            int localEdge = -1;
            for (int k = 0; k < 4; ++k) {
                int a = fv[k];
                int b = fv[(k + 1) % 4];
                if (std::min(a, b) == v0 && std::max(a, b) == v1) {
                    localEdge = k;
                    break;
                }
            }
            if (localEdge < 0) return false;

            Bfr::Surface<double> surface;
            if (!factory.InitVertexSurface(f, &surface)) return false;
            Far::ConstIndexArray fe = level.GetFaceEdges(f);
            std::vector<int> rates5(5);
            for (int k = 0; k < 4; ++k) {
                int er = 1;
                Far::ConstIndexArray neigh = level.GetEdgeFaces(fe[k]);
                for (int q = 0; q < neigh.size(); ++q) {
                    er = std::max(er, requests[neigh[q]]);
                }
                rates5[k] = er;
            }
            rates5[4] = requests[f];

            Bfr::Tessellation tess(surface.GetParameterization(), 5,
                                   rates5.data());
            std::vector<double> coords(
                static_cast<size_t>(tess.GetNumCoords()) * 2);
            tess.GetCoords(coords.data());

            int boundaryCursor = 0;
            for (int k = 0; k < localEdge; ++k) boundaryCursor += rates5[k];
            int numBoundary = tess.GetNumBoundaryCoords();
            std::vector<uint64_t> seq(rate + 1);
            for (int s = 0; s <= rate; ++s) {
                //  Boundary coords form a closed loop, so the closing
                //  endpoint wraps to the first boundary coordinate.
                int idx = (boundaryCursor + s) % numBoundary;
                Vec2 uv = { coords[2 * idx], coords[2 * idx + 1] };
                auto it = cornerSite.find(std::make_pair(f, uv));
                if (it == cornerSite.end()) return false;
                seq[s] = it->second;
            }
            //  Re-orient to the canonical v0 -> v1 direction.
            int a = fv[localEdge];
            if (a == v1) std::reverse(seq.begin(), seq.end());
            if (seq.front() == 0 || seq.back() == 0) return false;
            faceSequences.push_back(seq);
        }
        for (size_t i = 1; i < faceSequences.size(); ++i) {
            if (faceSequences[i] != faceSequences[0]) return false;
        }
        if (static_cast<int>(faceSequences[0].size()) != rate + 1) {
            return false;
        }
    }

    //  Positive local winding: every triangle has strictly positive signed
    //  area in its face's (u, v) parameterization. Analytic normals are
    //  checked separately against the independent Bfr reference; facet
    //  geometric normals are intentionally not compared at structural
    //  boundary corners, where the first jet is singular.
    for (DeltaTriangle const & t : snapshot.triangles) {
        double area2 =
            (t.cornerUV[1][0] - t.cornerUV[0][0]) *
                (t.cornerUV[2][1] - t.cornerUV[0][1]) -
            (t.cornerUV[1][1] - t.cornerUV[0][1]) *
                (t.cornerUV[2][0] - t.cornerUV[0][0]);
        if (!(area2 > 0.0)) return false;
    }
    return true;
}

//  Independent support set via a fresh factory.
std::set<int>
supportFaces(Far::TopologyRefiner const & refiner, int vertex) {
    Bfr::RefinerSurfaceFactory<> factory(refiner);
    Far::TopologyLevel const & level = refiner.GetLevel(0);
    std::set<int> out;
    for (int f = 0; f < level.GetNumFaces(); ++f) {
        Bfr::Surface<double> surface;
        if (!factory.InitVertexSurface(f, &surface)) {
            throw std::runtime_error("support: surface init failed");
        }
        std::vector<int> cvs(surface.GetNumControlPoints());
        surface.GetControlPointIndices(cvs.data());
        for (int cv : cvs) {
            if (cv == vertex) { out.insert(f); break; }
        }
    }
    return out;
}

//  Independent, hand-written OSDM0001 packer (deliberately not using the
//  production EncodeMeshDelta), so external references do not all come
//  from the same encoder.
namespace handpack {

void putU64(std::vector<unsigned char> & b, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        b.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xFF));
    }
}
void putU32(std::vector<unsigned char> & b, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xFF));
    }
}
void putF64(std::vector<unsigned char> & b, double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    putU64(b, bits);
}

std::vector<unsigned char>
pack(MeshDelta const & d) {
    std::vector<unsigned char> b;
    static const char magic[8] = {'O','S','D','M','0','0','0','1'};
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<unsigned char>(magic[i]));
    putU64(b, d.fromRevision);
    putU64(b, d.toRevision);
    putU64(b, d.deletedTriangles.size());
    putU64(b, d.deletedPositions.size());
    putU64(b, d.positionUpserts.size());
    putU64(b, d.triangleUpserts.size());
    for (uint64_t id : d.deletedTriangles) putU64(b, id);
    for (uint64_t id : d.deletedPositions) putU64(b, id);
    for (DeltaPosition const & p : d.positionUpserts) {
        putU64(b, p.id);
        putF64(b, p.position[0]);
        putF64(b, p.position[1]);
        putF64(b, p.position[2]);
    }
    for (DeltaTriangle const & t : d.triangleUpserts) {
        putU64(b, t.id);
        putU32(b, t.face);
        putU64(b, t.positionIds[0]);
        putU64(b, t.positionIds[1]);
        putU64(b, t.positionIds[2]);
        for (auto const & uv : t.cornerUV) { putF64(b, uv[0]); putF64(b, uv[1]); }
        for (auto const & n : t.cornerNormal) {
            putF64(b, n[0]); putF64(b, n[1]); putF64(b, n[2]);
        }
    }
    return b;
}

} // namespace handpack

MeshDelta minimalValidDelta() {
    MeshDelta d;
    d.fromRevision = 0;
    d.toRevision = 1;
    for (uint64_t id = 1; id <= 3; ++id) {
        DeltaPosition p;
        p.id = id;
        p.position = { static_cast<double>(id), 0.0, 0.0 };
        d.positionUpserts.push_back(p);
    }
    DeltaTriangle t;
    t.id = 1;
    t.face = 0;
    t.positionIds = { 1, 2, 3 };
    t.cornerUV = { Vec2{0.0, 0.0}, Vec2{1.0, 0.0}, Vec2{0.0, 1.0} };
    t.cornerNormal = { Vec3{0.0, 0.0, 1.0},
                       Vec3{0.0, 0.0, 1.0},
                       Vec3{0.0, 0.0, 1.0} };
    d.triangleUpserts.push_back(t);
    return d;
}

// ---------------------------------------------------------------------
//  dependency group
// ---------------------------------------------------------------------
int testSupportPropagation() {
    Cage cage = makeTorus(6, 6);
    auto refiner = makeRefiner(cage);
    std::vector<int> rates(cage.numFaces, 2);
    Bfr::TessellationSession session(*refiner, cage.positions.data(),
                                     cage.positions.size(),
                                     rates.data(), rates.size());

    std::set<int> expected = supportFaces(*refiner, 0);
    Far::ConstIndexArray incident = refiner->GetLevel(0).GetVertexFaces(0);
    std::set<int> incidentSet(incident.begin(), incident.end());

    bool foundNonIncident = false;
    for (int f : expected) {
        if (incidentSet.find(f) == incidentSet.end()) {
            foundNonIncident = true;
            break;
        }
    }
    TASK_CHECK(foundNonIncident);
    TASK_CHECK(!expected.empty());
    TASK_CHECK(expected.size() < static_cast<size_t>(cage.numFaces));

    auto edited = cage.positions;
    edited[0][2] += 0.31;
    std::vector<Bfr::PositionEdit> edits{{0, edited[0]}};
    MeshDelta delta = session.Update(edits, {});
    TASK_CHECK(delta.toRevision == 2);

    std::set<int> got(session.EvaluatedFaces().begin(),
                      session.EvaluatedFaces().end());
    TASK_CHECK(got == expected);
    TASK_CHECK(session.RemeshedFaces().empty());

    //  A distant, unsupported face must not appear in any delta record.
    std::set<int> touchedFaces;
    for (DeltaTriangle const & t : delta.triangleUpserts) {
        touchedFaces.insert(static_cast<int>(t.face));
    }
    for (int f : touchedFaces) {
        TASK_CHECK(expected.find(f) != expected.end());
    }
    TASK_CHECK(delta.deletedTriangles.empty());
    TASK_CHECK(delta.deletedPositions.empty());
    return 0;
}

int testFailureRollbackAndParity() {
    Cage cage = makeOpenSheet(5, 5);
    auto refiner = makeRefiner(cage);
    auto controlRefiner = makeRefiner(cage);
    std::vector<int> rates(cage.numFaces, 2);

    Bfr::TessellationSession session(*refiner, cage.positions.data(),
                                     cage.positions.size(),
                                     rates.data(), rates.size());
    Bfr::TessellationSession control(*controlRefiner, cage.positions.data(),
                                     cage.positions.size(),
                                     rates.data(), rates.size());

    //  First successful edit on both.
    auto moved = cage.positions;
    moved[3][0] += 0.2;
    std::vector<Bfr::PositionEdit> first{{3, moved[3]}};
    MeshDelta d1 = session.Update(first, {});
    MeshDelta c1 = control.Update(first, {});
    TASK_CHECK(d1.toRevision == 2 && c1.toRevision == 2);

    MeshSnapshot before = session.Snapshot();
    uint64_t revBefore = session.Revision();
    uint64_t nextPosBefore = session.NextPositionId();
    uint64_t nextTriBefore = session.NextTriangleId();

    //  A degenerate edit: every control point onto a single straight line.
    //  The limit surface collapses to a curve, so Pu and Pv are parallel
    //  (non-zero magnitude, cross product only roundoff) and the analytic
    //  tangent-area test ||Pu x Pv|| / (||Pu|| ||Pv||) <= 1e-12 fires.
    std::vector<Bfr::PositionEdit> collapse;
    for (int v = 0; v < cage.numVertices; ++v) {
        collapse.push_back(
            Bfr::PositionEdit{v, Vec3{0.25 * v, 0.0, 0.0}});
    }
    bool threw = false;
    try {
        session.Update(collapse, {});
    } catch (std::runtime_error const &) {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(session.Revision() == revBefore);
    TASK_CHECK(session.NextPositionId() == nextPosBefore);
    TASK_CHECK(session.NextTriangleId() == nextTriBefore);

    MeshSnapshot after = session.Snapshot();
    TASK_CHECK(after.revision == before.revision);
    TASK_CHECK(after.positions.size() == before.positions.size());
    TASK_CHECK(after.triangles.size() == before.triangles.size());
    for (size_t i = 0; i < before.positions.size(); ++i) {
        TASK_CHECK(after.positions[i].id == before.positions[i].id);
        TASK_CHECK(after.positions[i].position ==
                   before.positions[i].position);
    }
    for (size_t i = 0; i < before.triangles.size(); ++i) {
        TASK_CHECK(after.triangles[i].id == before.triangles[i].id);
    }

    //  Retry after failure must match a session that never failed.
    auto moved2 = cage.positions;
    moved2[7][1] -= 0.25;
    std::vector<Bfr::PositionEdit> second{{7, moved2[7]}};
    MeshDelta d2 = session.Update(second, {});
    MeshDelta c2 = control.Update(second, {});
    TASK_CHECK(d2.fromRevision == c2.fromRevision);
    TASK_CHECK(d2.toRevision == c2.toRevision);
    TASK_CHECK(d2.deletedTriangles == c2.deletedTriangles);
    TASK_CHECK(d2.deletedPositions == c2.deletedPositions);
    TASK_CHECK(d2.positionUpserts.size() == c2.positionUpserts.size());
    TASK_CHECK(d2.triangleUpserts.size() == c2.triangleUpserts.size());
    for (size_t i = 0; i < d2.positionUpserts.size(); ++i) {
        TASK_CHECK(d2.positionUpserts[i].id == c2.positionUpserts[i].id);
        TASK_CHECK(d2.positionUpserts[i].position ==
                   c2.positionUpserts[i].position);
    }
    MeshSnapshot sEnd = session.Snapshot();
    MeshSnapshot cEnd = control.Snapshot();
    TASK_CHECK(sEnd.positions.size() == cEnd.positions.size());
    TASK_CHECK(sEnd.triangles.size() == cEnd.triangles.size());
    for (size_t i = 0; i < sEnd.triangles.size(); ++i) {
        TASK_CHECK(sEnd.triangles[i].id == cEnd.triangles[i].id);
        TASK_CHECK(sEnd.triangles[i].positionIds ==
                   cEnd.triangles[i].positionIds);
    }
    return 0;
}

int testValidationErrors() {
    Cage cage = makeOpenSheet(4, 4);
    auto refiner = makeRefiner(cage);
    std::vector<int> rates(cage.numFaces, 2);

    bool threw = false;

    //  Construction length mismatches.
    try {
        Bfr::TessellationSession bad(*refiner, cage.positions.data(),
                                     cage.positions.size() - 1,
                                     rates.data(), rates.size());
    } catch (std::invalid_argument const &) { threw = true; }
    TASK_CHECK(threw); threw = false;

    std::vector<int> badRates = rates;
    badRates[0] = 0;
    try {
        Bfr::TessellationSession bad(*refiner, cage.positions.data(),
                                     cage.positions.size(),
                                     badRates.data(), badRates.size());
    } catch (std::invalid_argument const &) { threw = true; }
    TASK_CHECK(threw); threw = false;

    badRates[0] = 33;
    try {
        Bfr::TessellationSession bad(*refiner, cage.positions.data(),
                                     cage.positions.size(),
                                     badRates.data(), badRates.size());
    } catch (std::invalid_argument const &) { threw = true; }
    TASK_CHECK(threw); threw = false;

    std::vector<Vec3> badPoints = cage.positions;
    badPoints[1][0] = std::numeric_limits<double>::infinity();
    try {
        Bfr::TessellationSession bad(*refiner, badPoints.data(),
                                     badPoints.size(),
                                     rates.data(), rates.size());
    } catch (std::invalid_argument const &) { threw = true; }
    TASK_CHECK(threw); threw = false;

    Bfr::TessellationSession session(*refiner, cage.positions.data(),
                                     cage.positions.size(),
                                     rates.data(), rates.size());

    //  Out of range, duplicate, non-finite point edits and bad rates.
    try {
        std::vector<Bfr::PositionEdit> e{
            Bfr::PositionEdit{cage.numVertices, Vec3{0, 0, 0}}};
        session.Update(e, {});
    } catch (std::invalid_argument const &) { threw = true; }
    TASK_CHECK(threw); threw = false;

    try {
        std::vector<Bfr::PositionEdit> e{
            Bfr::PositionEdit{0, cage.positions[0]},
            Bfr::PositionEdit{0, cage.positions[0]}};
        session.Update(e, {});
    } catch (std::invalid_argument const &) { threw = true; }
    TASK_CHECK(threw); threw = false;

    try {
        std::vector<Bfr::PositionEdit> e{
            Bfr::PositionEdit{0, Vec3{std::nan(""), 0.0, 0.0}}};
        session.Update(e, {});
    } catch (std::invalid_argument const &) { threw = true; }
    TASK_CHECK(threw); threw = false;

    try {
        std::vector<Bfr::RateEdit> e{Bfr::RateEdit{0, 0}};
        session.Update({}, e);
    } catch (std::invalid_argument const &) { threw = true; }
    TASK_CHECK(threw); threw = false;

    try {
        std::vector<Bfr::RateEdit> e{
            Bfr::RateEdit{0, 1}, Bfr::RateEdit{0, 2}};
        session.Update({}, e);
    } catch (std::invalid_argument const &) { threw = true; }
    TASK_CHECK(threw);

    TASK_CHECK(session.Revision() == 1);
    return 0;
}

// ---------------------------------------------------------------------
//  retessellation group
// ---------------------------------------------------------------------
int testRateRoundTripIdentity() {
    Cage cage = makeTorus(6, 6);
    auto refiner = makeRefiner(cage);
    std::vector<int> rates(cage.numFaces, 2);
    Bfr::TessellationSession session(*refiner, cage.positions.data(),
                                     cage.positions.size(),
                                     rates.data(), rates.size());
    Far::TopologyLevel const & level = refiner->GetLevel(0);
    Bfr::RefinerSurfaceFactory<> factory(*refiner);

    const int focusFace = 8;
    int edge = level.GetFaceEdges(focusFace)[0];
    Far::ConstIndexArray ev = level.GetEdgeVertices(edge);
    int v0 = std::min(ev[0], ev[1]);
    int v1 = std::max(ev[0], ev[1]);

    //  Canonical v0 -> v1 ordered station ids of one base edge, derived
    //  directly from an independent Bfr tessellation and the snapshot.
    auto edgeStations = [&](MeshSnapshot const & snap,
                            std::vector<int> const & requests) {
        std::map<std::pair<int, Vec2>, uint64_t> cornerSite;
        for (DeltaTriangle const & t : snap.triangles) {
            for (int q = 0; q < 3; ++q) {
                cornerSite[std::make_pair(static_cast<int>(t.face),
                                          Vec2(t.cornerUV[q]))] =
                    t.positionIds[q];
            }
        }
        int f = level.GetEdgeFaces(edge)[0];
        Far::ConstIndexArray fv = level.GetFaceVertices(f);
        int localEdge = -1;
        for (int k = 0; k < 4; ++k) {
            int a = fv[k], b = fv[(k + 1) % 4];
            if (std::min(a, b) == v0 && std::max(a, b) == v1) localEdge = k;
        }
        Bfr::Surface<double> surface;
        if (!factory.InitVertexSurface(f, &surface)) {
            return std::vector<uint64_t>();
        }
        Far::ConstIndexArray fe = level.GetFaceEdges(f);
        std::vector<int> rates5(5);
        for (int k = 0; k < 4; ++k) {
            int er = 1;
            Far::ConstIndexArray neigh = level.GetEdgeFaces(fe[k]);
            for (int q = 0; q < neigh.size(); ++q) {
                er = std::max(er, requests[neigh[q]]);
            }
            rates5[k] = er;
        }
        rates5[4] = requests[f];
        Bfr::Tessellation tess(surface.GetParameterization(), 5,
                               rates5.data());
        std::vector<double> coords(
            static_cast<size_t>(tess.GetNumCoords()) * 2);
        tess.GetCoords(coords.data());
        int rate = rates5[localEdge];
        int cursor = 0;
        for (int k = 0; k < localEdge; ++k) cursor += rates5[k];
        int numBoundary = tess.GetNumBoundaryCoords();
        std::vector<uint64_t> seq(rate + 1);
        for (int s = 0; s <= rate; ++s) {
            int idx = (cursor + s) % numBoundary;
            Vec2 uv = { coords[2 * idx], coords[2 * idx + 1] };
            auto it = cornerSite.find(std::make_pair(f, uv));
            if (it == cornerSite.end()) return std::vector<uint64_t>();
            seq[s] = it->second;
        }
        if (fv[localEdge] == v1) std::reverse(seq.begin(), seq.end());
        return seq;
    };

    MeshSnapshot snap2 = session.Snapshot();
    std::vector<uint64_t> stations2 = edgeStations(snap2, rates);
    TASK_CHECK(stations2.size() == 3);          // rate 2 -> two sub-edges
    TASK_CHECK(stations2[0] != 0 && stations2[2] != 0);
    uint64_t midpointAt2 = stations2[1];

    std::vector<Bfr::RateEdit> up{Bfr::RateEdit{focusFace, 4}};
    MeshDelta dUp = session.Update({}, up);
    TASK_CHECK(dUp.toRevision == 2);

    //  Remeshed set must include the focus face and its edge neighbors,
    //  while the neighbors' own inner request stays at 2.
    std::set<int> remesh(session.RemeshedFaces().begin(),
                         session.RemeshedFaces().end());
    TASK_CHECK(remesh.find(focusFace) != remesh.end());
    Far::ConstIndexArray e8 = level.GetFaceEdges(focusFace);
    std::set<int> neighbors;
    for (int k = 0; k < 4; ++k) {
        Far::ConstIndexArray ef = level.GetEdgeFaces(e8[k]);
        for (int i = 0; i < ef.size(); ++i) neighbors.insert(ef[i]);
    }
    for (int f : neighbors) TASK_CHECK(remesh.find(f) != remesh.end());

    std::vector<int> rates4 = rates;
    rates4[focusFace] = 4;
    MeshSnapshot snap4 = session.Snapshot();
    std::vector<uint64_t> stations4 = edgeStations(snap4, rates4);
    TASK_CHECK(stations4.size() == 5);        // rate 4 -> four sub-edges

    //  Endpoints keep their vertex ids and the rational midpoint (station
    //  2 of 4) is the exact same id as station 1 of 2.
    TASK_CHECK(stations4[0] == stations2[0]);
    TASK_CHECK(stations4[4] == stations2[2]);
    TASK_CHECK(stations4[2] == midpointAt2);

    //  A pure rate increase never deletes a surviving edge/vertex station:
    //  every deleted position must be an interior site of a remeshed face.
    std::set<uint64_t> edgeAndVertexIds;
    for (uint64_t id : stations4) edgeAndVertexIds.insert(id);
    for (uint64_t id : stations2) edgeAndVertexIds.insert(id);
    for (uint64_t id : dUp.deletedPositions) {
        TASK_CHECK(edgeAndVertexIds.find(id) == edgeAndVertexIds.end());
    }

    std::vector<Bfr::RateEdit> down{Bfr::RateEdit{focusFace, 2}};
    MeshDelta dDown = session.Update({}, down);
    TASK_CHECK(dDown.toRevision == 3);
    MeshSnapshot snapBack = session.Snapshot();
    std::vector<uint64_t> stationsBack = edgeStations(snapBack, rates);
    TASK_CHECK(stationsBack == stations2);

    //  Deleted ids are never reused anywhere in the live mesh.
    std::set<uint64_t> liveIds;
    for (DeltaTriangle const & t : snapBack.triangles) {
        for (uint64_t id : t.positionIds) liveIds.insert(id);
    }
    std::set<uint64_t> allDeleted;
    for (uint64_t id : dUp.deletedPositions) allDeleted.insert(id);
    for (uint64_t id : dDown.deletedPositions) allDeleted.insert(id);
    for (uint64_t id : allDeleted) {
        TASK_CHECK(liveIds.find(id) == liveIds.end());
    }
    return 0;
}

int testWorkSetsAndReference() {
    Cage cage = makeOpenSheet(6, 5);
    auto refiner = makeRefiner(cage);
    std::vector<int> rates(cage.numFaces, 2);
    Bfr::TessellationSession session(*refiner, cage.positions.data(),
                                     cage.positions.size(),
                                     rates.data(), rates.size());
    Bfr::MeshMirror mirror;
    mirror.Apply(session.InitialDelta());

    MeshSnapshot initSnap = session.Snapshot();
    TASK_CHECK(compareSnapshot(*refiner, cage.positions, rates, initSnap));
    TASK_CHECK(verifySharedEdges(*refiner, rates, initSnap));

    //  Combined point + rate edit on material containing distant faces.
    auto moved = cage.positions;
    moved[2][2] += 0.4;
    std::vector<Bfr::PositionEdit> points{Bfr::PositionEdit{2, moved[2]}};
    std::vector<Bfr::RateEdit> rateEdits{Bfr::RateEdit{3, 4}};
    MeshDelta d = session.Update(points, rateEdits);
    mirror.Apply(d);

    std::set<int> expectedEval = supportFaces(*refiner, 2);
    Far::TopologyLevel const & level = refiner->GetLevel(0);
    Far::ConstIndexArray fe = level.GetFaceEdges(3);
    std::set<int> remeshCandidates{3};
    for (int k = 0; k < 4; ++k) {
        Far::ConstIndexArray ef = level.GetEdgeFaces(fe[k]);
        for (int i = 0; i < ef.size(); ++i) remeshCandidates.insert(ef[i]);
    }
    std::set<int> expectedRemesh;
    std::vector<int> newRates = rates;
    newRates[3] = 4;
    for (int f : remeshCandidates) {
        Far::ConstIndexArray edges = level.GetFaceEdges(f);
        bool changed = (newRates[f] != rates[f]);
        for (int k = 0; k < 4 && !changed; ++k) {
            int oldER = 1, newER = 1;
            Far::ConstIndexArray ef = level.GetEdgeFaces(edges[k]);
            for (int i = 0; i < ef.size(); ++i) {
                oldER = std::max(oldER, rates[ef[i]]);
                newER = std::max(newER, newRates[ef[i]]);
            }
            changed = oldER != newER;
        }
        if (changed) expectedRemesh.insert(f);
    }
    std::set<int> gotEval(session.EvaluatedFaces().begin(),
                          session.EvaluatedFaces().end());
    std::set<int> gotRemesh(session.RemeshedFaces().begin(),
                            session.RemeshedFaces().end());
    TASK_CHECK(gotRemesh == expectedRemesh);
    std::set<int> expectedUnion = expectedEval;
    for (int f : expectedRemesh) expectedUnion.insert(f);
    TASK_CHECK(gotEval == expectedUnion);

    //  No duplicate / strictly ordered work sets.
    TASK_CHECK(std::adjacent_find(session.EvaluatedFaces().begin(),
                                  session.EvaluatedFaces().end()) ==
               session.EvaluatedFaces().end());
    TASK_CHECK(std::adjacent_find(session.RemeshedFaces().begin(),
                                  session.RemeshedFaces().end()) ==
               session.RemeshedFaces().end());

    //  Records never mention an unrelated face.
    std::set<int> deltaFaces;
    for (DeltaTriangle const & t : d.triangleUpserts) {
        deltaFaces.insert(static_cast<int>(t.face));
    }
    for (int f : deltaFaces) {
        TASK_CHECK(expectedUnion.find(f) != expectedUnion.end());
    }

    MeshSnapshot midSnap = session.Snapshot();
    TASK_CHECK(compareSnapshot(*refiner, moved, newRates, midSnap));
    TASK_CHECK(verifySharedEdges(*refiner, newRates, midSnap));
    TASK_CHECK(mirror.Snapshot().triangles.size() ==
               midSnap.triangles.size());

    //  A no-op edit must not advance the revision.
    uint64_t rev = session.Revision();
    MeshDelta noop = session.Update(points, rateEdits);
    TASK_CHECK(noop.IsEmpty());
    TASK_CHECK(noop.fromRevision == rev && noop.toRevision == rev);
    TASK_CHECK(session.Revision() == rev);
    mirror.Apply(noop);

    //  Rate back to 2.
    MeshDelta d2 = session.Update(
        {}, std::vector<Bfr::RateEdit>{Bfr::RateEdit{3, 2}});
    mirror.Apply(d2);
    MeshSnapshot endSnap = session.Snapshot();
    TASK_CHECK(compareSnapshot(*refiner, moved, rates, endSnap));
    TASK_CHECK(verifySharedEdges(*refiner, rates, endSnap));
    TASK_CHECK(mirror.Revision() == session.Revision());
    return 0;
}

int testMultipleComponents() {
    Cage cage = makeTwoComponents();
    auto refiner = makeRefiner(cage);
    std::vector<int> rates(cage.numFaces, 2);
    Bfr::TessellationSession session(*refiner, cage.positions.data(),
                                     cage.positions.size(),
                                     rates.data(), rates.size());
    MeshSnapshot snap = session.Snapshot();
    TASK_CHECK(compareSnapshot(*refiner, cage.positions, rates, snap));
    TASK_CHECK(verifySharedEdges(*refiner, rates, snap));

    //  Editing a vertex in the torus must not touch sheet faces.
    Cage torus = makeTorus(5, 5);
    std::set<int> torusSupport = supportFaces(*refiner, 0);
    auto moved = cage.positions;
    moved[0][0] += 0.5;
    MeshDelta d = session.Update(
        std::vector<Bfr::PositionEdit>{Bfr::PositionEdit{0, moved[0]}}, {});
    for (DeltaTriangle const & t : d.triangleUpserts) {
        TASK_CHECK(torusSupport.find(static_cast<int>(t.face)) !=
                   torusSupport.end());
    }
    return 0;
}

// ---------------------------------------------------------------------
//  replay group
// ---------------------------------------------------------------------
int testStreamReplay() {
    Cage cage = makeTorus(5, 6);
    auto refiner = makeRefiner(cage);
    std::vector<int> rates(cage.numFaces, 2);
    Bfr::TessellationSession session(*refiner, cage.positions.data(),
                                     cage.positions.size(),
                                     rates.data(), rates.size());
    Bfr::MeshMirror mirror;

    std::vector<MeshDelta> stream;
    stream.push_back(session.InitialDelta());

    auto moved = cage.positions;
    moved[4][2] += 0.3;
    stream.push_back(session.Update(
        std::vector<Bfr::PositionEdit>{Bfr::PositionEdit{4, moved[4]}}, {}));
    stream.push_back(session.Update(
        {}, std::vector<Bfr::RateEdit>{Bfr::RateEdit{2, 4}}));
    stream.push_back(session.Update(
        std::vector<Bfr::PositionEdit>{Bfr::PositionEdit{9, moved[9]}},
        std::vector<Bfr::RateEdit>{Bfr::RateEdit{2, 2}}));
    stream.push_back(session.Update(
        std::vector<Bfr::PositionEdit>{Bfr::PositionEdit{4, moved[4]}}, {}));

    for (MeshDelta const & d : stream) mirror.Apply(d);

    TASK_CHECK(mirror.Revision() == session.Revision());
    MeshSnapshot sessionSnap = session.Snapshot();
    MeshSnapshot mirrorSnap = mirror.Snapshot();
    TASK_CHECK(sessionSnap.positions.size() == mirrorSnap.positions.size());
    TASK_CHECK(sessionSnap.triangles.size() == mirrorSnap.triangles.size());
    for (size_t i = 0; i < sessionSnap.positions.size(); ++i) {
        TASK_CHECK(sessionSnap.positions[i].id ==
                   mirrorSnap.positions[i].id);
        TASK_CHECK(sessionSnap.positions[i].position ==
                   mirrorSnap.positions[i].position);
    }
    for (size_t i = 0; i < sessionSnap.triangles.size(); ++i) {
        DeltaTriangle const & a = sessionSnap.triangles[i];
        DeltaTriangle const & b = mirrorSnap.triangles[i];
        TASK_CHECK(a.id == b.id && a.face == b.face);
        TASK_CHECK(a.positionIds == b.positionIds);
        TASK_CHECK(a.cornerUV == b.cornerUV);
        TASK_CHECK(a.cornerNormal == b.cornerNormal);
    }

    //  Deterministic encoding and byte-exact round trip.
    for (MeshDelta const & d : stream) {
        std::vector<unsigned char> a = Bfr::EncodeMeshDelta(d);
        std::vector<unsigned char> b = Bfr::EncodeMeshDelta(d);
        TASK_CHECK(a == b);
        MeshDelta decoded = Bfr::DecodeMeshDelta(a);
        TASK_CHECK(decoded.fromRevision == d.fromRevision);
        TASK_CHECK(decoded.toRevision == d.toRevision);
        TASK_CHECK(decoded.positionUpserts.size() ==
                   d.positionUpserts.size());
        TASK_CHECK(decoded.triangleUpserts.size() ==
                   d.triangleUpserts.size());
        TASK_CHECK(decoded.deletedPositions == d.deletedPositions);
        TASK_CHECK(decoded.deletedTriangles == d.deletedTriangles);
    }
    return 0;
}

int testHandBuiltProtocolAndAtomicReject() {
    MeshDelta d = minimalValidDelta();
    std::vector<unsigned char> bytes = handpack::pack(d);

    //  The independent packer must agree with the production decoder.
    MeshDelta decoded = Bfr::DecodeMeshDelta(bytes);
    TASK_CHECK(decoded.triangleUpserts.size() == 1);
    TASK_CHECK(decoded.positionUpserts.size() == 3);

    Bfr::MeshMirror mirror;
    mirror.Apply(decoded);
    TASK_CHECK(mirror.Revision() == 1);
    TASK_CHECK(mirror.NumTriangles() == 1);
    TASK_CHECK(mirror.NumPositions() == 3);
    MeshSnapshot goodSnap = mirror.Snapshot();

    auto expectThrow = [](std::function<void()> fn) {
        bool threw = false;
        try { fn(); }
        catch (std::exception const &) { threw = true; }
        return threw;
    };

    //  1. Version out of order on a populated mirror.
    {
        MeshDelta bad = d;
        bad.fromRevision = 5;
        bad.toRevision = 6;
        TASK_CHECK(expectThrow([&] { mirror.Apply(bad); }));
    }
    //  2. Truncated byte stream.
    {
        std::vector<unsigned char> cut = bytes;
        cut.pop_back();
        TASK_CHECK(expectThrow([&] { Bfr::DecodeMeshDelta(cut); }));
    }
    //  3. Bad magic.
    {
        std::vector<unsigned char> bad = bytes;
        bad[0] = 'X';
        TASK_CHECK(expectThrow([&] { Bfr::DecodeMeshDelta(bad); }));
    }
    //  4. Over-limit record count in the header.
    {
        std::vector<unsigned char> bad = bytes;
        uint64_t over = 1000001;
        for (int i = 0; i < 8; ++i) {
            bad[24 + i] =
                static_cast<unsigned char>((over >> (8 * i)) & 0xFF);
        }
        TASK_CHECK(expectThrow([&] { Bfr::DecodeMeshDelta(bad); }));
    }
    //  5. Dangling position reference rejected atomically.
    {
        MeshDelta bad = d;
        bad.fromRevision = 1;
        bad.toRevision = 2;
        bad.triangleUpserts[0].id = 7;
        bad.triangleUpserts[0].positionIds = { 1, 2, 999};
        TASK_CHECK(expectThrow([&] { mirror.Apply(bad); }));
    }
    //  6. Non-finite position.
    {
        MeshDelta bad = d;
        bad.positionUpserts[0].position[0] =
            std::numeric_limits<double>::quiet_NaN();
        TASK_CHECK(expectThrow([&] { Bfr::EncodeMeshDelta(bad); }));
    }
    //  7. Non-unit normal.
    {
        MeshDelta bad = d;
        bad.triangleUpserts[0].cornerNormal[0] = {1.0, 1.0, 1.0};
        TASK_CHECK(expectThrow([&] { Bfr::EncodeMeshDelta(bad); }));
    }
    //  8. Unsorted records.
    {
        MeshDelta bad = d;
        bad.deletedTriangles = {5, 3};
        TASK_CHECK(expectThrow([&] { Bfr::EncodeMeshDelta(bad); }));
    }
    //  9. Same id deleted and upserted.
    {
        MeshDelta bad = d;
        bad.fromRevision = 1;
        bad.toRevision = 2;
        bad.deletedTriangles = {1};
        DeltaTriangle t = bad.triangleUpserts[0];
        t.id = 1;  // id 1 already exists; upsert is legal, but combine with
        bad.triangleUpserts[0] = t;
        TASK_CHECK(expectThrow([&] { Bfr::EncodeMeshDelta(bad); }));
    }
    //  10. Empty file and oversize guard.
    {
        std::vector<unsigned char> empty;
        TASK_CHECK(expectThrow([&] { Bfr::DecodeMeshDelta(empty); }));
    }

    //  The mirror must be completely untouched by every rejection.
    TASK_CHECK(mirror.Revision() == 1);
    TASK_CHECK(mirror.NumTriangles() == 1);
    TASK_CHECK(mirror.NumPositions() == 3);
    MeshSnapshot afterSnap = mirror.Snapshot();
    TASK_CHECK(afterSnap.triangles.size() == goodSnap.triangles.size());
    TASK_CHECK(afterSnap.positions.size() == goodSnap.positions.size());
    return 0;
}

int testMirrorEmptyDeltaAndDeletes() {
    Bfr::MeshMirror mirror;
    MeshDelta d = minimalValidDelta();
    mirror.Apply(d);

    //  Empty delta at the same revision is legal and changes nothing.
    MeshDelta noop;
    noop.fromRevision = mirror.Revision();
    noop.toRevision = mirror.Revision();
    mirror.Apply(noop);
    TASK_CHECK(mirror.Revision() == 1);

    //  A delete followed by re-insert in the same delta is rejected when
    //  combined; a proper delete-only delta commits and frees the id.
    MeshDelta remove;
    remove.fromRevision = 1;
    remove.toRevision = 2;
    remove.deletedTriangles = {1};
    mirror.Apply(remove);
    TASK_CHECK(mirror.NumTriangles() == 0);
    TASK_CHECK(mirror.Revision() == 2);

    bool threw = false;
    try {
        MeshDelta again = remove;  // deleting a missing id
        again.fromRevision = 2;
        again.toRevision = 3;
        mirror.Apply(again);
    } catch (std::invalid_argument const &) {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(mirror.Revision() == 2);
    return 0;
}

} // namespace session_contracts

//  The task driver resolves these two symbols with C linkage, so the table
//  lives at global scope while the test bodies stay in a named namespace.
const struct task_case task_cases[] = {
    { "dependency", "support_propagation",
      session_contracts::testSupportPropagation },
    { "dependency", "failure_rollback_parity",
      session_contracts::testFailureRollbackAndParity },
    { "dependency", "validation_errors",
      session_contracts::testValidationErrors },

    { "retessellation", "rate_roundtrip_identity",
      session_contracts::testRateRoundTripIdentity },
    { "retessellation", "worksets_and_reference",
      session_contracts::testWorkSetsAndReference },
    { "retessellation", "multiple_components",
      session_contracts::testMultipleComponents },

    { "replay", "stream_replay",
      session_contracts::testStreamReplay },
    { "replay", "handbuilt_protocol_atomic",
      session_contracts::testHandBuiltProtocolAndAtomicReject },
    { "replay", "mirror_empty_and_deletes",
      session_contracts::testMirrorEmptyDeltaAndDeletes },
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
