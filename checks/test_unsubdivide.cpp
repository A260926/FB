//
// checks/test_unsubdivide.cpp -- registration unit for the inverse
// Catmull-Clark subdivision.  No main(): task_support/driver.c drives the
// cases through task_cases / task_case_count.
//
// Materials are built from known coarse cages, subdivided once with the
// native Gather path, then independently renumbered (vertices, faces,
// halfedges, edges and per-face loop starts) before being handed to the
// inverse API, so no reliance on exported ordering is possible.
//
#include "../task_support/check.h"
#include "cc_Unsubdivide.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <utility>
#include <vector>

namespace {

using Pt = std::array<double, 3>;

struct Cage {
    std::vector<Pt> points;
    std::vector<std::vector<int32_t>> faces;
};

struct ChildMesh {
    cc_Mesh *mesh = nullptr;
    std::vector<int32_t> markers; // child ids that are coarse-vertex successors
    int32_t coarseV = 0;
    int32_t coarseE = 0;
    int32_t coarseF = 0;
};

// Build a closed, crease-free, UV-free halfedge mesh from oriented polygons.
static cc_Mesh *BuildMesh(const std::vector<Pt> &points,
                          const std::vector<std::vector<int32_t>> &faces)
{
    int32_t H = 0;
    for (const auto &f : faces) {
        H += static_cast<int32_t>(f.size());
    }
    const int32_t V = static_cast<int32_t>(points.size());
    const int32_t F = static_cast<int32_t>(faces.size());

    // Directed halfedge lookup; each key is the (origin, destination) pair.
    std::map<std::pair<int32_t, int32_t>, int32_t> directed;
    int32_t h = 0;
    for (int32_t f = 0; f < F; ++f) {
        const int32_t n = static_cast<int32_t>(faces[f].size());
        for (int32_t k = 0; k < n; ++k) {
            const int32_t a = faces[f][k];
            const int32_t b = faces[f][(k + 1) % n];
            if (!directed.emplace(std::make_pair(a, b), h).second) {
                return nullptr; // repeated directed edge
            }
            ++h;
        }
    }

    // Count undirected edges so the kernel arrays are sized correctly.
    int32_t E = 0;
    for (const auto &kv : directed) {
        if (directed.find(std::make_pair(kv.first.second,
                                         kv.first.first)) != directed.end()) {
            ++E;
        }
    }
    E /= 2;
    if (H != 2 * E) {
        return nullptr; // open or non-manifold polygon soup
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

// One native crease-free Gather step, exported as a standalone cc_Mesh.
static ChildMesh SubdivideOnce(const Cage &cage)
{
    ChildMesh out;
    cc_Mesh *parent = BuildMesh(cage.points, cage.faces);
    if (!parent) {
        return out;
    }
    const int32_t V0 = ccm_VertexCount(parent);
    const int32_t E0 = ccm_EdgeCount(parent);
    const int32_t F0 = ccm_FaceCount(parent);

    cc_Subd *subd = ccs_Create(parent, 1);
    if (!subd) {
        ccm_Release(parent);
        return out;
    }
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
    ccs_Release(subd);
    ccm_Release(parent);

    out.mesh = BuildMesh(pts, quads);
    if (!out.mesh) {
        return out;
    }
    out.coarseV = V0;
    out.coarseE = E0;
    out.coarseF = F0;
    out.markers.resize(V0);
    for (int32_t i = 0; i < V0; ++i) {
        out.markers[i] = i;
    }
    return out;
}

// Independently renumber vertices / faces / halfedges / edges and rotate
// every face loop start.  Markers are rewritten to the new vertex ids.
static cc_Mesh *Renumber(ChildMesh &child, std::mt19937 &rng)
{
    const int32_t V = ccm_VertexCount(child.mesh);
    const int32_t F = ccm_FaceCount(child.mesh);

    std::vector<int32_t> pv(V), invPv(V);
    for (int32_t i = 0; i < V; ++i) {
        pv[i] = i;
    }
    std::shuffle(pv.begin(), pv.end(), rng);
    for (int32_t i = 0; i < V; ++i) {
        invPv[pv[i]] = i;
    }
    std::vector<int32_t> pf(F);
    for (int32_t i = 0; i < F; ++i) {
        pf[i] = i;
    }
    std::shuffle(pf.begin(), pf.end(), rng);

    std::vector<Pt> pts(V);
    for (int32_t i = 0; i < V; ++i) {
        for (int32_t d = 0; d < 3; ++d) {
            pts[i][d] = child.mesh->vertexPoints[pv[i]].array[d];
        }
    }
    std::vector<std::vector<int32_t>> faces(F, std::vector<int32_t>(4));
    std::uniform_int_distribution<int32_t> roll(0, 3);
    for (int32_t j = 0; j < F; ++j) {
        const int32_t oldFace = pf[j];
        const int32_t start = roll(rng);
        const int32_t base = ccm_FaceToHalfedgeID(child.mesh, oldFace);
        for (int32_t k = 0; k < 4; ++k) {
            const int32_t oldV =
                child.mesh->halfedges[base + (start + k) % 4].vertexID;
            faces[j][k] = invPv[oldV];
        }
    }

    std::vector<int32_t> newMarkers;
    newMarkers.reserve(child.markers.size());
    for (int32_t m : child.markers) {
        newMarkers.push_back(invPv[m]);
    }
    child.markers = std::move(newMarkers);

    cc_Mesh *renumbered = BuildMesh(pts, faces);
    ccm_Release(child.mesh);
    child.mesh = renumbered;
    return renumbered;
}

static bool ValidateManifold(const cc_Mesh *m)
{
    const int32_t V = ccm_VertexCount(m);
    const int32_t E = ccm_EdgeCount(m);
    const int32_t H = ccm_HalfedgeCount(m);
    const int32_t F = ccm_FaceCount(m);
    if (V <= 0 || E <= 0 || H <= 0 || F <= 0) {
        return false;
    }
    std::vector<int32_t> edgeUse(E, 0);
    std::map<std::pair<int32_t, int32_t>, int32_t> directed;
    for (int32_t h = 0; h < H; ++h) {
        const cc_Halfedge &he = m->halfedges[h];
        if (he.twinID < 0 || he.twinID >= H ||
            m->halfedges[he.twinID].twinID != h) {
            return false;
        }
        if (m->halfedges[he.nextID].prevID != h ||
            m->halfedges[he.prevID].nextID != h) {
            return false;
        }
        ++edgeUse[he.edgeID];
        const int32_t a = he.vertexID;
        const int32_t b = m->halfedges[he.nextID].vertexID;
        if (a == b || !directed.emplace(std::make_pair(a, b), h).second) {
            return false;
        }
    }
    for (int32_t e = 0; e < E; ++e) {
        if (edgeUse[e] != 2) {
            return false;
        }
    }
    for (int32_t f = 0; f < F; ++f) {
        const int32_t start = m->faceToHalfedgeIDs[f];
        int32_t h = start;
        int32_t count = 0;
        do {
            if (m->halfedges[h].faceID != f) {
                return false;
            }
            h = m->halfedges[h].nextID;
            ++count;
            if (count > H + 1) {
                return false;
            }
        } while (h != start);
        if (count < 3) {
            return false;
        }
    }
    for (int32_t v = 0; v < V; ++v) {
        const int32_t h0 = m->vertexToHalfedgeIDs[v];
        int32_t h = h0;
        int32_t count = 0;
        do {
            if (m->halfedges[h].vertexID != v) {
                return false;
            }
            h = m->halfedges[m->halfedges[h].twinID].nextID;
            ++count;
            if (count > V + 1) {
                return false;
            }
        } while (h != h0);
    }
    return true;
}

static std::vector<int32_t> FaceLoop(const cc_Mesh *m, int32_t f)
{
    std::vector<int32_t> loop;
    const int32_t start = m->faceToHalfedgeIDs[f];
    int32_t h = start;
    do {
        loop.push_back(m->halfedges[h].vertexID);
        h = m->halfedges[h].nextID;
    } while (h != start);
    return loop;
}

static std::vector<int32_t> Canonical(std::vector<int32_t> loop)
{
    const int32_t n = static_cast<int32_t>(loop.size());
    int32_t pos = 0;
    for (int32_t k = 1; k < n; ++k) {
        if (loop[k] < loop[pos]) {
            pos = k;
        }
    }
    std::rotate(loop.begin(), loop.begin() + pos, loop.end());
    return loop;
}

// Asymmetric tetrahedron (also used by the production baseline).
static Cage MakeTetra()
{
    Cage c;
    c.points = {
        {0.0, 0.0, 0.0}, {2.0, 0.0, 0.0},
        {0.0, 3.0, 0.0}, {0.0, 0.0, 4.0}
    };
    c.faces = {
        {0, 2, 1}, {0, 1, 3}, {1, 2, 3}, {2, 0, 3}
    };
    return c;
}

// Asymmetric pentagonal prism: two pentagon caps and five quad sides.
static Cage MakePrism()
{
    Cage c;
    const double theta[5] = {
        0.0, 1.17, 2.33, 3.61, 4.92
    };
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
    c.faces.push_back({0, 4, 3, 2, 1});             // bottom cap
    c.faces.push_back({5, 6, 7, 8, 9});             // top cap
    for (int32_t i = 0; i < 5; ++i) {
        const int32_t j = (i + 1) % 5;
        c.faces.push_back({i, j, 5 + j, 5 + i});    // side quad
    }
    return c;
}

// Quad torus with at least 32 x 32 quads (closed genus-1 grid).
static Cage MakeTorus(int32_t n, int32_t m)
{
    Cage c;
    const double R = 2.0;
    const double r = 0.7;
    for (int32_t i = 0; i < n; ++i) {
        for (int32_t j = 0; j < m; ++j) {
            const double u = 2.0 * M_PI * i / n;
            const double v = 2.0 * M_PI * j / m;
            c.points.push_back({
                (R + r * std::cos(v)) * std::cos(u),
                (R + r * std::cos(v)) * std::sin(u),
                r * std::sin(v)
            });
        }
    }
    for (int32_t i = 0; i < n; ++i) {
        for (int32_t j = 0; j < m; ++j) {
            const int32_t i2 = (i + 1) % n;
            const int32_t j2 = (j + 1) % m;
            c.faces.push_back({
                i * m + j, i2 * m + j, i2 * m + j2, i * m + j2
            });
        }
    }
    return c;
}

// Full inverse run plus structural / mapping / geometry verification.
static int RunInverseCheck(const Cage &cage, std::mt19937 &rng,
                           double tolerance, int32_t maxIterations,
                           cc_UnsubdivideResult **kept = nullptr)
{
    ChildMesh child = SubdivideOnce(cage);
    TASK_CHECK(child.mesh != nullptr);
    const int32_t childV = ccm_VertexCount(child.mesh);
    const int32_t childF = ccm_FaceCount(child.mesh);
    Renumber(child, rng);
    TASK_CHECK(child.mesh != nullptr);

    cc_UnsubdivideResult *result = nullptr;
    const cc_UnsubdivideStatus status = cc_Unsubdivide(
        child.mesh, child.markers.data(),
        static_cast<int32_t>(child.markers.size()),
        tolerance, maxIterations, &result);
    TASK_CHECK(status == CC_UNSUBDIVIDE_OK);
    TASK_CHECK(result != nullptr);
    TASK_CHECK(result->mesh != nullptr);

    // Counts match the known cage.
    TASK_CHECK(result->coarseVertexCount ==
               static_cast<int32_t>(cage.points.size()));
    TASK_CHECK(result->coarseFaceCount ==
               static_cast<int32_t>(cage.faces.size()));
    TASK_CHECK(ccm_VertexCount(result->mesh) == result->coarseVertexCount);
    TASK_CHECK(ccm_FaceCount(result->mesh) == result->coarseFaceCount);
    TASK_CHECK(ccm_HalfedgeCount(result->mesh) ==
               2 * result->coarseEdgeCount);
    TASK_CHECK(ValidateManifold(result->mesh));

    // Report fields are populated and storage is sparse (linear).
    TASK_CHECK(result->iterations > 0 && result->iterations <= maxIterations);
    TASK_CHECK(result->operatorProducts > 0);
    TASK_CHECK(result->nonZeroCount > 0);
    TASK_CHECK(result->nonZeroCount <= 20LL * 3 * childV);
    TASK_CHECK(result->maxForwardResidual >= 0.0);
    TASK_CHECK(result->maxForwardResidual <= tolerance);

    // Vertex classes and parent indices form a complete partition.
    int32_t countClass[3] = {0, 0, 0};
    std::vector<unsigned char> usedVertex(result->coarseVertexCount, 0);
    std::vector<unsigned char> usedEdge(result->coarseEdgeCount, 0);
    std::vector<unsigned char> usedFace(result->coarseFaceCount, 0);
    for (int32_t v = 0; v < childV; ++v) {
        const int32_t c = result->vertexClass[v];
        TASK_CHECK(c >= 0 && c <= 2);
        ++countClass[c];
        const int32_t p = result->vertexParent[v];
        if (c == 0) {
            TASK_CHECK(p >= 0 && p < result->coarseVertexCount);
            TASK_CHECK(!usedVertex[p]);
            usedVertex[p] = 1;
        } else if (c == 1) {
            TASK_CHECK(p >= 0 && p < result->coarseEdgeCount);
            TASK_CHECK(!usedEdge[p]);
            usedEdge[p] = 1;
        } else {
            TASK_CHECK(p >= 0 && p < result->coarseFaceCount);
            TASK_CHECK(!usedFace[p]);
            usedFace[p] = 1;
        }
    }
    TASK_CHECK(countClass[0] == result->coarseVertexCount);
    TASK_CHECK(countClass[1] == result->coarseEdgeCount);
    TASK_CHECK(countClass[2] == result->coarseFaceCount);
    TASK_CHECK(std::find(usedVertex.begin(), usedVertex.end(), 0) ==
               usedVertex.end());
    TASK_CHECK(std::find(usedEdge.begin(), usedEdge.end(), 0) ==
               usedEdge.end());
    TASK_CHECK(std::find(usedFace.begin(), usedFace.end(), 0) ==
               usedFace.end());

    // Child -> parent face / corner is a bijection onto every
    // (parent face, corner) pair.
    std::vector<std::vector<unsigned char>> cornerSeen(result->coarseFaceCount);
    for (int32_t f = 0; f < result->coarseFaceCount; ++f) {
        cornerSeen[f].assign(FaceLoop(result->mesh, f).size(), 0);
    }
    for (int32_t q = 0; q < childF; ++q) {
        const int32_t pf = result->childParentFace[q];
        const int32_t pc = result->childParentCorner[q];
        TASK_CHECK(pf >= 0 && pf < result->coarseFaceCount);
        TASK_CHECK(pc >= 0 &&
                   pc < static_cast<int32_t>(cornerSeen[pf].size()));
        TASK_CHECK(!cornerSeen[pf][pc]);
        cornerSeen[pf][pc] = 1;
    }
    for (const auto &row : cornerSeen) {
        TASK_CHECK(std::find(row.begin(), row.end(), 0) == row.end());
    }

    // Recovered coarse points correspond to the original cage points.
    const int32_t CV = result->coarseVertexCount;
    std::vector<int32_t> coarseToOriginal(CV, -1);
    for (int32_t i = 0; i < CV; ++i) {
        double best = 1e30;
        int32_t bestJ = -1;
        for (int32_t j = 0; j < static_cast<int32_t>(cage.points.size()); ++j) {
            double d2 = 0.0;
            for (int32_t d = 0; d < 3; ++d) {
                const double delta =
                    result->mesh->vertexPoints[i].array[d] -
                    cage.points[j][d];
                d2 += delta * delta;
            }
            if (d2 < best) {
                best = d2;
                bestJ = j;
            }
        }
        TASK_CHECK(bestJ >= 0 && std::sqrt(best) < 1e-4);
        coarseToOriginal[i] = bestJ;
    }
    std::vector<int32_t> origUsed(cage.points.size(), 0);
    for (int32_t i = 0; i < CV; ++i) {
        TASK_CHECK(!origUsed[coarseToOriginal[i]]);
        origUsed[coarseToOriginal[i]] = 1;
    }

    // Recovered face loops match the original oriented polygons, including
    // the deterministic minimum-vertex loop start.
    std::vector<std::vector<int32_t>> expected;
    for (const auto &f : cage.faces) {
        std::vector<int32_t> mapped;
        for (int32_t v : f) {
            const auto it =
                std::find(coarseToOriginal.begin(), coarseToOriginal.end(), v);
            TASK_CHECK(it != coarseToOriginal.end());
            mapped.push_back(static_cast<int32_t>(
                it - coarseToOriginal.begin()));
        }
        expected.push_back(Canonical(std::move(mapped)));
    }
    std::sort(expected.begin(), expected.end());
    std::vector<std::vector<int32_t>> recovered;
    for (int32_t f = 0; f < result->coarseFaceCount; ++f) {
        std::vector<int32_t> loop = FaceLoop(result->mesh, f);
        TASK_CHECK(loop.front() == *std::min_element(loop.begin(), loop.end()));
        recovered.push_back(Canonical(loop));
    }
    std::sort(recovered.begin(), recovered.end());
    TASK_CHECK(recovered == expected);

    // Deterministic numbering: coarse vertex i is the i-th marked child id.
    std::vector<int32_t> sortedMarkers = child.markers;
    std::sort(sortedMarkers.begin(), sortedMarkers.end());
    for (int32_t i = 0; i < CV; ++i) {
        TASK_CHECK(result->vertexClass[sortedMarkers[i]] == 0);
        TASK_CHECK(result->vertexParent[sortedMarkers[i]] == i);
    }

    ccm_Release(child.mesh);
    if (kept) {
        *kept = result;
    } else {
        cc_UnsubdivideResult_Release(result);
    }
    return 0;
}

// =====================================================================
// topology group -- adjacency-driven parent reconstruction only
// =====================================================================
int TopologyTetra()
{
    std::mt19937 rng(0xC0FFEEu);
    return RunInverseCheck(MakeTetra(), rng, 5e-5, 512);
}

int TopologyPrism()
{
    std::mt19937 rng(0x5151A5A5u);
    return RunInverseCheck(MakePrism(), rng, 5e-5, 512);
}

int TopologyTorus()
{
    std::mt19937 rng(0x70A2u);
    return RunInverseCheck(MakeTorus(32, 32), rng, 5e-5, 512);
}

// =====================================================================
// inverse group -- numerical recovery, sparse accounting and ownership
// =====================================================================
int InverseTetra()
{
    std::mt19937 rng(12345u);
    return RunInverseCheck(MakeTetra(), rng, 5e-5, 512);
}

int InversePrism()
{
    std::mt19937 rng(67890u);
    return RunInverseCheck(MakePrism(), rng, 5e-5, 512);
}

int InverseTorus()
{
    std::mt19937 rng(0xBEEFu);
    return RunInverseCheck(MakeTorus(32, 32), rng, 5e-5, 512);
}

// Two different inputs processed back to back; the first source is released
// while its recovered cage keeps being used.
int InverseSequentialLifetime()
{
    std::mt19937 rngA(1111u), rngB(2222u);
    cc_UnsubdivideResult *first = nullptr;
    TASK_CHECK(RunInverseCheck(MakePrism(), rngA, 5e-5, 512, &first) == 0);
    TASK_CHECK(first != nullptr && first->mesh != nullptr);

    cc_UnsubdivideResult *second = nullptr;
    TASK_CHECK(RunInverseCheck(MakeTetra(), rngB, 5e-5, 512, &second) == 0);
    TASK_CHECK(second != nullptr);

    // Keep subdividing the first result after the second run completed.
    cc_Subd *subd = ccs_Create(first->mesh, 1);
    TASK_CHECK(subd != nullptr);
    ccs_Refine_NoCreases_Gather(subd);
    TASK_CHECK(ccs_CumulativeVertexCount(subd) ==
               ccm_VertexCountAtDepth(first->mesh, 1));
    TASK_CHECK(ValidateManifold(second->mesh));
    ccs_Release(subd);
    cc_UnsubdivideResult_Release(first);
    cc_UnsubdivideResult_Release(second);
    return 0;
}

// =====================================================================
// rejection group -- nothing here may masquerade as a success
// =====================================================================
static ChildMesh MakeValidChild(std::mt19937 &rng)
{
    ChildMesh child = SubdivideOnce(MakePrism());
    Renumber(child, rng);
    return child;
}

int RejectBadTwin()
{
    std::mt19937 rng(1u);
    ChildMesh child = MakeValidChild(rng);
    const int32_t h0 = child.mesh->faceToHalfedgeIDs[0];
    child.mesh->halfedges[h0].twinID =
        (child.mesh->halfedges[h0].twinID + 1) %
        ccm_HalfedgeCount(child.mesh);
    cc_UnsubdivideResult *result = nullptr;
    const cc_UnsubdivideStatus status = cc_Unsubdivide(
        child.mesh, child.markers.data(),
        static_cast<int32_t>(child.markers.size()),
        5e-5, 512, &result);
    TASK_CHECK(status == CC_UNSUBDIVIDE_INVALID_INPUT);
    TASK_CHECK(result == nullptr);
    ccm_Release(child.mesh);
    return 0;
}

int RejectBoundary()
{
    std::mt19937 rng(2u);
    ChildMesh child = MakeValidChild(rng);
    const int32_t h0 = child.mesh->faceToHalfedgeIDs[0];
    child.mesh->halfedges[h0].twinID = -1;
    cc_UnsubdivideResult *result = nullptr;
    const cc_UnsubdivideStatus status = cc_Unsubdivide(
        child.mesh, child.markers.data(),
        static_cast<int32_t>(child.markers.size()),
        5e-5, 512, &result);
    TASK_CHECK(status == CC_UNSUBDIVIDE_INVALID_INPUT);
    TASK_CHECK(result == nullptr);
    ccm_Release(child.mesh);
    return 0;
}

int RejectBrokenFan()
{
    std::mt19937 rng(3u);
    ChildMesh child = MakeValidChild(rng);
    const int32_t victim = child.markers[0];
    const int32_t other = child.markers[1];
    child.mesh->vertexToHalfedgeIDs[victim] =
        child.mesh->vertexToHalfedgeIDs[other];
    cc_UnsubdivideResult *result = nullptr;
    const cc_UnsubdivideStatus status = cc_Unsubdivide(
        child.mesh, child.markers.data(),
        static_cast<int32_t>(child.markers.size()),
        5e-5, 512, &result);
    TASK_CHECK(status == CC_UNSUBDIVIDE_INVALID_INPUT);
    TASK_CHECK(result == nullptr);
    ccm_Release(child.mesh);
    return 0;
}

int RejectAllSkip()
{
    std::mt19937 rng(4u);
    ChildMesh child = MakeValidChild(rng);
    const int32_t dummy = 0;
    cc_UnsubdivideResult *result = nullptr;
    const cc_UnsubdivideStatus status = cc_Unsubdivide(
        child.mesh, &dummy, 0, 5e-5, 512, &result);
    TASK_CHECK(status == CC_UNSUBDIVIDE_INVALID_INPUT);
    TASK_CHECK(result == nullptr);
    ccm_Release(child.mesh);
    return 0;
}

int RejectBadMarker()
{
    std::mt19937 rng(5u);
    ChildMesh child = MakeValidChild(rng);
    // Claim an edge successor is an original vertex successor.
    int32_t edgePoint = -1;
    for (int32_t v = 0; v < ccm_VertexCount(child.mesh); ++v) {
        bool marked = false;
        for (int32_t m : child.markers) {
            if (m == v) {
                marked = true;
                break;
            }
        }
        if (!marked) {
            edgePoint = v;
            break;
        }
    }
    TASK_CHECK(edgePoint >= 0);
    std::vector<int32_t> bad = child.markers;
    bad.push_back(edgePoint);
    cc_UnsubdivideResult *result = nullptr;
    const cc_UnsubdivideStatus status = cc_Unsubdivide(
        child.mesh, bad.data(), static_cast<int32_t>(bad.size()),
        5e-5, 512, &result);
    TASK_CHECK(status == CC_UNSUBDIVIDE_NOT_SUBDIVISION);
    TASK_CHECK(result == nullptr);

    // Out-of-range and duplicate markers are plain invalid input.
    std::vector<int32_t> oor = child.markers;
    oor.push_back(ccm_VertexCount(child.mesh) + 5);
    result = nullptr;
    TASK_CHECK(cc_Unsubdivide(child.mesh, oor.data(),
                              static_cast<int32_t>(oor.size()),
                              5e-5, 512, &result) ==
               CC_UNSUBDIVIDE_INVALID_INPUT);
    std::vector<int32_t> dup = child.markers;
    dup.push_back(child.markers[0]);
    result = nullptr;
    TASK_CHECK(cc_Unsubdivide(child.mesh, dup.data(),
                              static_cast<int32_t>(dup.size()),
                              5e-5, 512, &result) ==
               CC_UNSUBDIVIDE_INVALID_INPUT);
    ccm_Release(child.mesh);
    return 0;
}

int RejectIllegalArguments()
{
    std::mt19937 rng(6u);
    ChildMesh child = MakeValidChild(rng);
    cc_UnsubdivideResult *result = nullptr;
    TASK_CHECK(cc_Unsubdivide(nullptr, child.markers.data(),
                              static_cast<int32_t>(child.markers.size()),
                              5e-5, 512, &result) ==
               CC_UNSUBDIVIDE_INVALID_INPUT);
    TASK_CHECK(cc_Unsubdivide(child.mesh, child.markers.data(),
                              static_cast<int32_t>(child.markers.size()),
                              0.0, 512, &result) ==
               CC_UNSUBDIVIDE_INVALID_INPUT);
    TASK_CHECK(cc_Unsubdivide(child.mesh, child.markers.data(),
                              static_cast<int32_t>(child.markers.size()),
                              -1.0, 512, &result) ==
               CC_UNSUBDIVIDE_INVALID_INPUT);
    TASK_CHECK(cc_Unsubdivide(child.mesh, child.markers.data(),
                              static_cast<int32_t>(child.markers.size()),
                              5e-5, 0, &result) ==
               CC_UNSUBDIVIDE_INVALID_INPUT);
    TASK_CHECK(cc_Unsubdivide(child.mesh, nullptr,
                              static_cast<int32_t>(child.markers.size()),
                              5e-5, 512, &result) ==
               CC_UNSUBDIVIDE_INVALID_INPUT);
    TASK_CHECK(result == nullptr);
    ccm_Release(child.mesh);
    return 0;
}

int RejectSolverLimit()
{
    std::mt19937 rng(7u);
    ChildMesh child = SubdivideOnce(MakeTorus(32, 32));
    Renumber(child, rng);
    cc_UnsubdivideResult *result = nullptr;
    const cc_UnsubdivideStatus status = cc_Unsubdivide(
        child.mesh, child.markers.data(),
        static_cast<int32_t>(child.markers.size()),
        5e-5, 1, &result);
    TASK_CHECK(status == CC_UNSUBDIVIDE_SOLVER_LIMIT);
    TASK_CHECK(result == nullptr);
    ccm_Release(child.mesh);
    return 0;
}

// Topology stays a valid subdivision, but one face-point successor is pushed
// out of the one-level image space: the real forward residual must reject it.
int RejectResidual()
{
    std::mt19937 rng(8u);
    ChildMesh child = SubdivideOnce(MakePrism());
    Renumber(child, rng);

    // A clean run first establishes the authoritative vertex classes.
    cc_UnsubdivideResult *clean = nullptr;
    TASK_CHECK(cc_Unsubdivide(
        child.mesh, child.markers.data(),
        static_cast<int32_t>(child.markers.size()),
        5e-5, 512, &clean) == CC_UNSUBDIVIDE_OK);
    TASK_CHECK(clean != nullptr);
    int32_t facePoint = -1;
    for (int32_t v = 0; v < ccm_VertexCount(child.mesh); ++v) {
        if (clean->vertexClass[v] == 2) {
            facePoint = v;
            break;
        }
    }
    cc_UnsubdivideResult_Release(clean);
    TASK_CHECK(facePoint >= 0);

    child.mesh->vertexPoints[facePoint].x += 1.0f;

    cc_UnsubdivideResult *result = nullptr;
    const cc_UnsubdivideStatus status = cc_Unsubdivide(
        child.mesh, child.markers.data(),
        static_cast<int32_t>(child.markers.size()),
        5e-5, 512, &result);
    TASK_CHECK(status == CC_UNSUBDIVIDE_RESIDUAL_EXCEEDED);
    TASK_CHECK(result == nullptr);
    ccm_Release(child.mesh);
    return 0;
}

int RejectNonQuad()
{
    // A triangle mesh cannot be the all-quad output of one subdivision.
    Cage tri;
    tri.points = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    tri.faces = {{0, 2, 1}, {0, 1, 3}, {1, 2, 3}, {2, 0, 3}};
    cc_Mesh *m = BuildMesh(tri.points, tri.faces);
    const int32_t markers[4] = {0, 1, 2, 3};
    cc_UnsubdivideResult *result = nullptr;
    const cc_UnsubdivideStatus status =
        cc_Unsubdivide(m, markers, 4, 5e-5, 512, &result);
    TASK_CHECK(status == CC_UNSUBDIVIDE_INVALID_INPUT);
    TASK_CHECK(result == nullptr);
    ccm_Release(m);
    return 0;
}

} // anonymous namespace

const struct task_case task_cases[] = {
    {"topology", "tetra_winding_and_bijection", TopologyTetra},
    {"topology", "pentagonal_prism_ngon_faces", TopologyPrism},
    {"topology", "torus_32x32_sparse_topology", TopologyTorus},
    {"inverse", "tetra_least_squares", InverseTetra},
    {"inverse", "prism_least_squares", InversePrism},
    {"inverse", "torus_least_squares", InverseTorus},
    {"inverse", "sequential_inputs_source_released", InverseSequentialLifetime},
    {"rejection", "bad_twin_halfedge", RejectBadTwin},
    {"rejection", "boundary_halfedge", RejectBoundary},
    {"rejection", "broken_vertex_fan", RejectBrokenFan},
    {"rejection", "all_skip_markers", RejectAllSkip},
    {"rejection", "bad_marker_set", RejectBadMarker},
    {"rejection", "illegal_arguments", RejectIllegalArguments},
    {"rejection", "solver_iteration_cap", RejectSolverLimit},
    {"rejection", "off_image_face_point", RejectResidual},
    {"rejection", "non_quad_input", RejectNonQuad}
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
