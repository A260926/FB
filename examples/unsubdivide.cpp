//
// examples/unsubdivide.cpp -- headless inverse Catmull-Clark demo.
//
// Usage: unsubdivide <output.ccm>
//
// Builds the one-level subdivision of an asymmetric pentagonal prism,
// renumbers the result so no exported ordering survives, recovers the
// control cage through cc_Unsubdivide, round-trips it through
// ccm_Save / ccm_Load, and finally re-subdivides the loaded cage to verify
// the mapping and the forward residual.  Any failed self-check makes the
// process return a non-zero exit code.
//
#include "cc_Unsubdivide.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

using Pt = std::array<double, 3>;

struct Cage {
    std::vector<Pt> points;
    std::vector<std::vector<int32_t>> faces;
};

static cc_Mesh *BuildMesh(const std::vector<Pt> &points,
                          const std::vector<std::vector<int32_t>> &faces)
{
    int32_t H = 0;
    for (const auto &f : faces) {
        H += static_cast<int32_t>(f.size());
    }
    const int32_t V = static_cast<int32_t>(points.size());
    const int32_t F = static_cast<int32_t>(faces.size());

    std::map<std::pair<int32_t, int32_t>, int32_t> directed;
    int32_t h = 0;
    for (int32_t f = 0; f < F; ++f) {
        const int32_t n = static_cast<int32_t>(faces[f].size());
        for (int32_t k = 0; k < n; ++k) {
            const int32_t a = faces[f][k];
            const int32_t b = faces[f][(k + 1) % n];
            if (!directed.emplace(std::make_pair(a, b), h).second) {
                return nullptr;
            }
            ++h;
        }
    }

    int32_t E = 0;
    for (const auto &kv : directed) {
        if (directed.find(std::make_pair(kv.first.second,
                                         kv.first.first)) != directed.end()) {
            ++E;
        }
    }
    E /= 2;
    if (H != 2 * E) {
        return nullptr;
    }

    cc_Mesh *m = ccm_Create(V, 0, H, E, F);
    if (!m) {
        return nullptr;
    }
    for (int32_t v = 0; v < V; ++v) {
        for (int32_t d = 0; d < 3; ++d) {
            m->vertexPoints[v].array[d] =
                static_cast<float>(points[v][d]);
        }
        m->vertexToHalfedgeIDs[v] = -1;
    }
    h = 0;
    for (int32_t f = 0; f < F; ++f) {
        const int32_t n = static_cast<int32_t>(faces[f].size());
        m->faceToHalfedgeIDs[f] = h;
        for (int32_t k = 0; k < n; ++k) {
            const int32_t id = h + k;
            const int32_t a = faces[f][k];
            m->halfedges[id].vertexID = a;
            m->halfedges[id].nextID = h + ((k + 1) % n);
            m->halfedges[id].prevID = h + ((k + n - 1) % n);
            m->halfedges[id].faceID = f;
            m->halfedges[id].edgeID = -1;
            m->halfedges[id].twinID = -1;
            m->halfedges[id].uvID = -1;
            m->vertexToHalfedgeIDs[a] = id;
        }
        h += n;
    }
    int32_t edgeCount = 0;
    for (const auto &kv : directed) {
        const int32_t ha = kv.second;
        if (m->halfedges[ha].edgeID >= 0) {
            continue;
        }
        const auto rev =
            directed.find(std::make_pair(kv.first.second, kv.first.first));
        if (rev == directed.end()) {
            ccm_Release(m);
            return nullptr;
        }
        const int32_t hb = rev->second;
        m->halfedges[ha].twinID = hb;
        m->halfedges[hb].twinID = ha;
        m->halfedges[ha].edgeID = edgeCount;
        m->halfedges[hb].edgeID = edgeCount;
        m->edgeToHalfedgeIDs[edgeCount] = ha;
        m->creases[edgeCount].nextID = edgeCount;
        m->creases[edgeCount].prevID = edgeCount;
        m->creases[edgeCount].sharpness = 0.0f;
        ++edgeCount;
    }
    if (edgeCount != E) {
        ccm_Release(m);
        return nullptr;
    }
    return m;
}

static Cage MakePrism()
{
    Cage c;
    const double theta[5] = {0.0, 1.17, 2.33, 3.61, 4.92};
    const double rb[5] = {1.0, 0.92, 1.08, 0.97, 1.05};
    const double rt[5] = {1.25, 1.18, 1.31, 1.22, 1.29};
    for (int32_t i = 0; i < 5; ++i) {
        c.points.push_back({rb[i] * std::cos(theta[i]),
                            rb[i] * std::sin(theta[i]), 0.0});
    }
    for (int32_t i = 0; i < 5; ++i) {
        c.points.push_back({rt[i] * std::cos(theta[i]) + 0.31,
                            rt[i] * std::sin(theta[i]) - 0.17,
                            2.0 + 0.07 * i});
    }
    c.faces.push_back({0, 4, 3, 2, 1});
    c.faces.push_back({5, 6, 7, 8, 9});
    for (int32_t i = 0; i < 5; ++i) {
        const int32_t j = (i + 1) % 5;
        c.faces.push_back({i, j, 5 + j, 5 + i});
    }
    return c;
}

struct Child {
    cc_Mesh *mesh = nullptr;
    std::vector<int32_t> markers;
};

static Child SubdivideAndRenumber(const Cage &cage, std::mt19937 &rng)
{
    cc_Mesh *parent = BuildMesh(cage.points, cage.faces);
    cc_Subd *subd = ccs_Create(parent, 1);
    ccs_Refine_NoCreases_Gather(subd);

    const int32_t V1 = ccm_VertexCountAtDepth(parent, 1);
    const int32_t F1 = ccm_FaceCountAtDepth(parent, 1);
    std::vector<Pt> pts(V1);
    std::vector<std::vector<int32_t>> quads(F1, std::vector<int32_t>(4));
    for (int32_t v = 0; v < V1; ++v) {
        const cc_VertexPoint p = ccs_VertexPoint(subd, v, 1);
        for (int32_t d = 0; d < 3; ++d) {
            pts[v][d] = p.array[d];
        }
    }
    for (int32_t f = 0; f < F1; ++f) {
        for (int32_t k = 0; k < 4; ++k) {
            quads[f][k] = ccs_HalfedgeVertexID(subd, 4 * f + k, 1);
        }
    }
    const int32_t V0 = ccm_VertexCount(parent);
    ccs_Release(subd);
    ccm_Release(parent);

    // Independent vertex / face permutation and per-face loop rotation.
    std::vector<int32_t> pv(V1), invPv(V1);
    for (int32_t i = 0; i < V1; ++i) {
        pv[i] = i;
    }
    std::shuffle(pv.begin(), pv.end(), rng);
    for (int32_t i = 0; i < V1; ++i) {
        invPv[pv[i]] = i;
    }
    std::vector<int32_t> pf(F1);
    for (int32_t i = 0; i < F1; ++i) {
        pf[i] = i;
    }
    std::shuffle(pf.begin(), pf.end(), rng);

    std::vector<Pt> renumPts(V1);
    for (int32_t i = 0; i < V1; ++i) {
        renumPts[i] = pts[pv[i]];
    }
    std::vector<std::vector<int32_t>> renumFaces(
        F1, std::vector<int32_t>(4));
    std::uniform_int_distribution<int32_t> roll(0, 3);
    for (int32_t j = 0; j < F1; ++j) {
        const int32_t start = roll(rng);
        for (int32_t k = 0; k < 4; ++k) {
            renumFaces[j][k] = invPv[quads[pf[j]][(start + k) % 4]];
        }
    }

    Child out;
    out.mesh = BuildMesh(renumPts, renumFaces);
    out.markers.resize(V0);
    for (int32_t i = 0; i < V0; ++i) {
        out.markers[i] = invPv[i];
    }
    return out;
}

static int Fail(const char *message)
{
    std::fprintf(stderr, "unsubdivide example FAILED: %s\n", message);
    return 1;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <output.ccm>\n",
                     argc > 0 ? argv[0] : "unsubdivide");
        return 2;
    }
    const std::string outputPath = argv[1];
    constexpr double kTolerance = 5e-5;
    constexpr int32_t kMaxIterations = 512;

    std::mt19937 rng(0x9E3779B9u);
    Child child = SubdivideAndRenumber(MakePrism(), rng);
    if (!child.mesh) {
        return Fail("child construction");
    }
    const int32_t childV = ccm_VertexCount(child.mesh);
    const int32_t childF = ccm_FaceCount(child.mesh);

    cc_UnsubdivideResult *result = nullptr;
    const cc_UnsubdivideStatus status = cc_Unsubdivide(
        child.mesh, child.markers.data(),
        static_cast<int32_t>(child.markers.size()),
        kTolerance, kMaxIterations, &result);
    if (status != CC_UNSUBDIVIDE_OK || !result || !result->mesh) {
        ccm_Release(child.mesh);
        return Fail("cc_Unsubdivide did not return OK");
    }
    std::printf("recovered cage: V=%d E=%d F=%d iterations=%d products=%d "
                "nnz=%lld forwardResidual=%.3e\n",
                result->coarseVertexCount, result->coarseEdgeCount,
                result->coarseFaceCount, result->iterations,
                result->operatorProducts,
                static_cast<long long>(result->nonZeroCount),
                result->maxForwardResidual);
    if (result->coarseVertexCount != 10 ||
        result->coarseEdgeCount != 15 ||
        result->coarseFaceCount != 7) {
        cc_UnsubdivideResult_Release(result);
        ccm_Release(child.mesh);
        return Fail("prism counts");
    }

    if (!ccm_Save(result->mesh, outputPath.c_str())) {
        cc_UnsubdivideResult_Release(result);
        ccm_Release(child.mesh);
        return Fail("ccm_Save");
    }
    cc_Mesh *loaded = ccm_Load(outputPath.c_str());
    if (!loaded) {
        cc_UnsubdivideResult_Release(result);
        ccm_Release(child.mesh);
        return Fail("ccm_Load");
    }
    if (ccm_VertexCount(loaded) != result->coarseVertexCount ||
        ccm_FaceCount(loaded) != result->coarseFaceCount) {
        ccm_Release(loaded);
        cc_UnsubdivideResult_Release(result);
        ccm_Release(child.mesh);
        return Fail("round-trip counts");
    }

    // Re-subdivide the loaded cage and verify every point against the
    // renumbered input through the published child -> parent mapping.
    cc_Subd *subd = ccs_Create(loaded, 1);
    if (!subd) {
        ccm_Release(loaded);
        cc_UnsubdivideResult_Release(result);
        ccm_Release(child.mesh);
        return Fail("ccs_Create");
    }
    ccs_Refine_NoCreases_Gather(subd);
    if (ccm_VertexCountAtDepth(loaded, 1) != childV ||
        ccm_FaceCountAtDepth(loaded, 1) != childF) {
        ccs_Release(subd);
        ccm_Release(loaded);
        cc_UnsubdivideResult_Release(result);
        ccm_Release(child.mesh);
        return Fail("forward counts");
    }

    std::vector<int32_t> subdToChild(childV, -1);
    for (int32_t i = 0; i < result->coarseVertexCount; ++i) {
        int32_t childId = -1;
        for (int32_t v = 0; v < childV; ++v) {
            if (result->vertexClass[v] == 0 &&
                result->vertexParent[v] == i) {
                childId = v;
                break;
            }
        }
        if (childId < 0) {
            ccs_Release(subd);
            ccm_Release(loaded);
            cc_UnsubdivideResult_Release(result);
            ccm_Release(child.mesh);
            return Fail("vertex mapping");
        }
        subdToChild[i] = childId;
    }
    for (int32_t cf = 0; cf < result->coarseFaceCount; ++cf) {
        int32_t childId = -1;
        for (int32_t v = 0; v < childV; ++v) {
            if (result->vertexClass[v] == 2 &&
                result->vertexParent[v] == cf) {
                childId = v;
                break;
            }
        }
        if (childId < 0) {
            ccs_Release(subd);
            ccm_Release(loaded);
            cc_UnsubdivideResult_Release(result);
            ccm_Release(child.mesh);
            return Fail("face mapping");
        }
        subdToChild[result->coarseVertexCount + cf] = childId;
    }
    for (int32_t e = 0; e < result->coarseEdgeCount; ++e) {
        int32_t childId = -1;
        for (int32_t v = 0; v < childV; ++v) {
            if (result->vertexClass[v] == 1 &&
                result->vertexParent[v] == e) {
                childId = v;
                break;
            }
        }
        if (childId < 0) {
            ccs_Release(subd);
            ccm_Release(loaded);
            cc_UnsubdivideResult_Release(result);
            ccm_Release(child.mesh);
            return Fail("edge mapping");
        }
        subdToChild[result->coarseVertexCount +
                    result->coarseFaceCount + e] = childId;
    }

    double maxResidual = 0.0;
    for (int32_t sv = 0; sv < childV; ++sv) {
        const cc_VertexPoint p = ccs_VertexPoint(subd, sv, 1);
        const int32_t cv = subdToChild[sv];
        if (cv < 0) {
            ccs_Release(subd);
            ccm_Release(loaded);
            cc_UnsubdivideResult_Release(result);
            ccm_Release(child.mesh);
            return Fail("incomplete point mapping");
        }
        double d2 = 0.0;
        for (int32_t d = 0; d < 3; ++d) {
            const double delta =
                static_cast<double>(p.array[d]) -
                static_cast<double>(child.mesh->vertexPoints[cv].array[d]);
            d2 += delta * delta;
        }
        maxResidual = std::max(maxResidual, std::sqrt(d2));
    }
    std::printf("round-trip re-subdivision max residual: %.3e\n",
                maxResidual);
    if (!(maxResidual <= kTolerance)) {
        ccs_Release(subd);
        ccm_Release(loaded);
        cc_UnsubdivideResult_Release(result);
        ccm_Release(child.mesh);
        return Fail("residual after round trip");
    }

    // Every child quad maps back to a unique (parent face, parent corner).
    std::vector<std::vector<unsigned char>> cornerSeen(
        result->coarseFaceCount);
    for (int32_t f = 0; f < result->coarseFaceCount; ++f) {
        const int32_t start = loaded->faceToHalfedgeIDs[f];
        int32_t h = start;
        int32_t n = 0;
        do {
            ++n;
            h = loaded->halfedges[h].nextID;
        } while (h != start);
        cornerSeen[f].assign(n, 0);
    }
    for (int32_t q = 0; q < childF; ++q) {
        const int32_t pf = result->childParentFace[q];
        const int32_t pc = result->childParentCorner[q];
        if (pf < 0 || pf >= result->coarseFaceCount ||
            pc < 0 || pc >= static_cast<int32_t>(cornerSeen[pf].size()) ||
            cornerSeen[pf][pc]) {
            ccs_Release(subd);
            ccm_Release(loaded);
            cc_UnsubdivideResult_Release(result);
            ccm_Release(child.mesh);
            return Fail("child face mapping");
        }
        cornerSeen[pf][pc] = 1;
    }

    ccs_Release(subd);
    ccm_Release(loaded);
    cc_UnsubdivideResult_Release(result);
    ccm_Release(child.mesh);
    std::printf("UNSUBDIVIDE_EXAMPLE_OK %s\n", outputPath.c_str());
    return 0;
}
