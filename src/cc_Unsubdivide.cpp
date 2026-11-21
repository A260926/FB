//
// cc_Unsubdivide.cpp -- one-level inverse Catmull-Clark subdivision.
//
// The pipeline is:
//   1. fully validate the input mesh (no unchecked native traversal);
//   2. recover vertex classes (vertex / edge / face successors) and the
//      coarse parent topology from adjacency alone;
//   3. build the sparse, crease-free one-level Gather operator A and solve
//      min ||A X - Y||^2 with conjugate gradients on the normal equations;
//   4. publish a candidate coarse cc_Mesh, re-run the production Gather path
//      and reject the candidate unless every point and child-face adjacency
//      is reproduced within the caller supplied tolerance.
//
#include "cc_Unsubdivide.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace {

using std::int32_t;

constexpr int32_t CLASS_VERTEX = 0;
constexpr int32_t CLASS_EDGE   = 1;
constexpr int32_t CLASS_FACE   = 2;

struct SparseMatrix {
    int32_t rowCount = 0;
    int32_t colCount = 0;
    std::vector<int32_t> rowPtr;
    std::vector<int32_t> colInd;
    std::vector<double>  value;
};

static bool FiniteDouble(double x)
{
    return std::isfinite(x);
}

// y = A x
static void Multiply(const SparseMatrix &a, const double *x, double *y)
{
    for (int32_t r = 0; r < a.rowCount; ++r) {
        double sum = 0.0;
        for (int32_t k = a.rowPtr[r]; k < a.rowPtr[r + 1]; ++k) {
            sum += a.value[k] * x[a.colInd[k]];
        }
        y[r] = sum;
    }
}

// y = A^T x
static void MultiplyTranspose(const SparseMatrix &a, const double *x, double *y)
{
    std::fill(y, y + a.colCount, 0.0);
    for (int32_t r = 0; r < a.rowCount; ++r) {
        const double xr = x[r];
        for (int32_t k = a.rowPtr[r]; k < a.rowPtr[r + 1]; ++k) {
            y[a.colInd[k]] += a.value[k] * xr;
        }
    }
}

static double InfNorm(const std::vector<double> &v)
{
    double m = 0.0;
    for (double x : v) {
        const double a = std::fabs(x);
        if (a > m) m = a;
    }
    return m;
}

// Merge (column, coefficient) pairs by summing duplicates.
static std::vector<std::pair<int32_t, double>>
MergeEntries(std::vector<std::pair<int32_t, double>> entries)
{
    std::sort(entries.begin(), entries.end(),
              [](const auto &p, const auto &q) { return p.first < q.first; });
    std::vector<std::pair<int32_t, double>> out;
    for (auto &e : entries) {
        if (!out.empty() && out.back().first == e.first) {
            out.back().second += e.second;
        } else {
            out.push_back(e);
        }
    }
    return out;
}

} // anonymous namespace

extern "C"
void cc_UnsubdivideResult_Release(cc_UnsubdivideResult *result)
{
    if (!result) {
        return;
    }
    delete[] result->vertexClass;
    delete[] result->vertexParent;
    delete[] result->childParentFace;
    delete[] result->childParentCorner;
    if (result->mesh) {
        ccm_Release(result->mesh);
    }
    delete result;
}

extern "C"
cc_UnsubdivideStatus cc_Unsubdivide(
    const cc_Mesh *mesh,
    const int32_t *markedVertex,
    int32_t markedVertexCount,
    double tolerance,
    int32_t maxIterations,
    cc_UnsubdivideResult **outResult)
{
    // ----------------------------------------------------------------------
    // 1. Argument validation
    // ----------------------------------------------------------------------
    if (!mesh || !outResult || !markedVertex) {
        return CC_UNSUBDIVIDE_INVALID_INPUT;
    }
    if (!FiniteDouble(tolerance) || tolerance <= 0.0) {
        return CC_UNSUBDIVIDE_INVALID_INPUT;
    }
    if (maxIterations <= 0) {
        return CC_UNSUBDIVIDE_INVALID_INPUT;
    }

    const int32_t V = mesh->vertexCount;
    const int32_t H = mesh->halfedgeCount;
    const int32_t E = mesh->edgeCount;
    const int32_t F = mesh->faceCount;

    constexpr int32_t kLimit = INT32_MAX / 8;
    if (V <= 0 || H <= 0 || E <= 0 || F <= 0 ||
        V > kLimit || H > kLimit || E > kLimit || F > kLimit) {
        return CC_UNSUBDIVIDE_INVALID_INPUT;
    }
    if (!mesh->vertexPoints || !mesh->halfedges || !mesh->creases ||
        !mesh->vertexToHalfedgeIDs || !mesh->edgeToHalfedgeIDs ||
        !mesh->faceToHalfedgeIDs) {
        return CC_UNSUBDIVIDE_INVALID_INPUT;
    }
    // UVs and creases are not supported.
    if (mesh->uvCount != 0) {
        return CC_UNSUBDIVIDE_INVALID_INPUT;
    }
    if (markedVertexCount <= 0 || markedVertexCount > V) {
        return CC_UNSUBDIVIDE_INVALID_INPUT;
    }

    // Coordinates: finite and within the documented box.
    for (int32_t v = 0; v < V; ++v) {
        for (int32_t d = 0; d < 3; ++d) {
            const float x = mesh->vertexPoints[v].array[d];
            if (!std::isfinite(x) || x < -10.0f || x > 10.0f) {
                return CC_UNSUBDIVIDE_INVALID_INPUT;
            }
        }
    }

    // Marker array: in-range, no duplicates.
    std::vector<int32_t> marked(markedVertex, markedVertex + markedVertexCount);
    {
        std::vector<int32_t> sorted = marked;
        std::sort(sorted.begin(), sorted.end());
        for (int32_t i = 0; i < markedVertexCount; ++i) {
            if (sorted[i] < 0 || sorted[i] >= V) {
                return CC_UNSUBDIVIDE_INVALID_INPUT;
            }
            if (i > 0 && sorted[i] == sorted[i - 1]) {
                return CC_UNSUBDIVIDE_INVALID_INPUT;
            }
        }
    }
    std::vector<unsigned char> isMarked(V, 0);
    for (int32_t v : marked) {
        isMarked[v] = 1;
    }

    // Crease sharpness must all be zero.
    for (int32_t e = 0; e < E; ++e) {
        if (mesh->creases[e].sharpness != 0.0f) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
    }

    // ----------------------------------------------------------------------
    // 2. Halfedge structure validation (all indices checked before traversal)
    // ----------------------------------------------------------------------
    const cc_Halfedge *he = mesh->halfedges;

    for (int32_t h = 0; h < H; ++h) {
        const int32_t t = he[h].twinID;
        const int32_t n = he[h].nextID;
        const int32_t p = he[h].prevID;
        if (t < -1 || t >= H || n < 0 || n >= H || p < 0 || p >= H) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
        if (he[h].faceID < 0 || he[h].faceID >= F ||
            he[h].edgeID < 0 || he[h].edgeID >= E ||
            he[h].vertexID < 0 || he[h].vertexID >= V) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
        if (he[h].uvID != -1) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
        if (t == h || n == h || p == h) {
            return CC_UNSUBDIVIDE_INVALID_INPUT; // self loops
        }
        if (t >= 0) {
            if (he[t].twinID != h) {
                return CC_UNSUBDIVIDE_INVALID_INPUT; // bad twin pairing
            }
        }
        if (he[n].prevID != h || he[p].nextID != h) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
    }

    // Back-pointer arrays.
    for (int32_t v = 0; v < V; ++v) {
        const int32_t h = mesh->vertexToHalfedgeIDs[v];
        if (h < 0 || h >= H || he[h].vertexID != v) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
    }
    for (int32_t e = 0; e < E; ++e) {
        const int32_t h = mesh->edgeToHalfedgeIDs[e];
        if (h < 0 || h >= H || he[h].edgeID != e) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
    }
    for (int32_t f = 0; f < F; ++f) {
        const int32_t h = mesh->faceToHalfedgeIDs[f];
        if (h < 0 || h >= H || he[h].faceID != f) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
    }

    // Every face is a quad and its halfedge loop is closed and consistent.
    std::vector<std::array<int32_t, 4>> faceVerts(F);
    for (int32_t f = 0; f < F; ++f) {
        const int32_t start = mesh->faceToHalfedgeIDs[f];
        int32_t h = start;
        for (int32_t k = 0; k < 4; ++k) {
            if (he[h].faceID != f) {
                return CC_UNSUBDIVIDE_INVALID_INPUT;
            }
            faceVerts[f][k] = he[h].vertexID;
            h = he[h].nextID;
        }
        if (h != start) {
            return CC_UNSUBDIVIDE_INVALID_INPUT; // not a quad
        }
        for (int32_t a = 0; a < 4; ++a) {
            for (int32_t b = a + 1; b < 4; ++b) {
                if (faceVerts[f][a] == faceVerts[f][b]) {
                    return CC_UNSUBDIVIDE_INVALID_INPUT;
                }
            }
        }
    }
    if (H != 4 * F) {
        return CC_UNSUBDIVIDE_INVALID_INPUT;
    }

    // Every edge is used by exactly two twin halfedges; closed manifold.
    std::vector<int32_t> edgeUse(E, 0);
    for (int32_t h = 0; h < H; ++h) {
        ++edgeUse[he[h].edgeID];
        if (he[h].twinID < 0) {
            return CC_UNSUBDIVIDE_INVALID_INPUT; // boundary unsupported
        }
        if (he[he[h].twinID].edgeID != he[h].edgeID) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
    }
    for (int32_t e = 0; e < E; ++e) {
        if (edgeUse[e] != 2) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
    }
    if (H != 2 * E) {
        return CC_UNSUBDIVIDE_INVALID_INPUT;
    }

    // No repeated directed edges (duplicate / non-manifold incidence).
    std::vector<std::pair<int32_t, int32_t>> directed;
    directed.reserve(H);
    for (int32_t h = 0; h < H; ++h) {
        const int32_t a = he[h].vertexID;
        const int32_t b = he[he[h].nextID].vertexID;
        if (a == b) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
        directed.emplace_back(a, b);
    }
    std::sort(directed.begin(), directed.end());
    for (int32_t i = 1; i < H; ++i) {
        if (directed[i] == directed[i - 1]) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
    }

    // Every vertex fan is a closed, consistent ring.
    std::vector<int32_t> incident(V, 0);
    for (int32_t h = 0; h < H; ++h) {
        ++incident[he[h].vertexID];
    }
    for (int32_t v = 0; v < V; ++v) {
        const int32_t h0 = mesh->vertexToHalfedgeIDs[v];
        int32_t h = h0;
        int32_t count = 0;
        do {
            if (he[h].vertexID != v) {
                return CC_UNSUBDIVIDE_INVALID_INPUT; // broken vertex fan
            }
            const int32_t t = he[h].twinID;
            if (t < 0) {
                return CC_UNSUBDIVIDE_INVALID_INPUT;
            }
            h = he[t].nextID;
            ++count;
            if (count > V + 1) {
                return CC_UNSUBDIVIDE_INVALID_INPUT; // fan does not close
            }
        } while (h != h0);
        // A single ring must cover every incident halfedge; a mismatch
        // indicates a non-manifold vertex with more than one link component.
        if (count != incident[v]) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
    }

    // Connectivity: one connected component over twin / next adjacency.
    {
        std::vector<unsigned char> seen(H, 0);
        std::vector<int32_t> stack;
        stack.push_back(0);
        seen[0] = 1;
        int32_t reached = 0;
        while (!stack.empty()) {
            const int32_t h = stack.back();
            stack.pop_back();
            ++reached;
            const int32_t nbr[3] = { he[h].twinID, he[h].nextID, he[h].prevID };
            for (int32_t q : nbr) {
                if (q >= 0 && !seen[q]) {
                    seen[q] = 1;
                    stack.push_back(q);
                }
            }
        }
        if (reached != H) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
    }

    // Euler characteristic of a connected closed orientable surface is
    // 2 - 2g (any genus is supported, e.g. a torus has characteristic 0).
    {
        const int64_t chi = static_cast<int64_t>(V) - E + F;
        if (chi > 2 || (chi & 1) != 0) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
    }

    // Directed halfedge lookup (a -> b) -> h.
    std::map<std::pair<int32_t, int32_t>, int32_t> directedMap;
    for (int32_t h = 0; h < H; ++h) {
        const int32_t a = he[h].vertexID;
        const int32_t b = he[he[h].nextID].vertexID;
        directedMap.emplace(std::make_pair(a, b), h);
    }

    // ----------------------------------------------------------------------
    // 3. Recover vertex classes from the marked original-vertex successors
    //
    // A child quad is anchored at one coarse vertex successor a.  Walking
    // its loop from a: [a, edge-point, face-point, edge-point].
    // ----------------------------------------------------------------------
    std::vector<int32_t> vertexClass(V, -1);

    auto AssignClass = [&](int32_t v, int32_t c) -> bool {
        if (vertexClass[v] == -1) {
            vertexClass[v] = c;
            return true;
        }
        return vertexClass[v] == c;
    };

    for (int32_t f = 0; f < F; ++f) {
        // Locate the unique marked corner of this quad; it is the anchor.
        int32_t anchor = -1;
        for (int32_t k = 0; k < 4; ++k) {
            if (isMarked[faceVerts[f][k]]) {
                if (anchor != -1) {
                    return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                }
                anchor = k;
            }
        }
        if (anchor == -1) {
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
        const int32_t a  = faceVerts[f][ anchor        ];
        const int32_t e1 = faceVerts[f][(anchor + 1) & 3];
        const int32_t fp = faceVerts[f][(anchor + 2) & 3];
        const int32_t e2 = faceVerts[f][(anchor + 3) & 3];
        if (!AssignClass(a,  CLASS_VERTEX) ||
            !AssignClass(e1, CLASS_EDGE)   ||
            !AssignClass(fp, CLASS_FACE)   ||
            !AssignClass(e2, CLASS_EDGE)) {
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
    }

    int32_t coarseVertexCount = 0;
    int32_t coarseFaceCount = 0;
    for (int32_t v = 0; v < V; ++v) {
        if (vertexClass[v] == -1) {
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
        // Marked set must be exactly the vertex-successor set.
        if (isMarked[v] != (vertexClass[v] == CLASS_VERTEX)) {
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
        if (vertexClass[v] == CLASS_VERTEX) {
            ++coarseVertexCount;
        } else if (vertexClass[v] == CLASS_FACE) {
            ++coarseFaceCount;
        }
    }
    if (coarseVertexCount != markedVertexCount) {
        return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
    }

    // Every quad must exhibit the [vertex, edge, face, edge] role pattern.
    for (int32_t f = 0; f < F; ++f) {
        int32_t facePos = -1;
        for (int32_t k = 0; k < 4; ++k) {
            if (vertexClass[faceVerts[f][k]] == CLASS_FACE) {
                if (facePos != -1) {
                    return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                }
                facePos = k;
            }
        }
        if (facePos == -1) {
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
        if (vertexClass[faceVerts[f][(facePos + 2) & 3]] != CLASS_VERTEX ||
            vertexClass[faceVerts[f][(facePos + 1) & 3]] != CLASS_EDGE   ||
            vertexClass[faceVerts[f][(facePos + 3) & 3]] != CLASS_EDGE) {
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
    }

    // ----------------------------------------------------------------------
    // 4. Coarse numbering (deterministic, index based but order independent)
    // ----------------------------------------------------------------------
    std::vector<int32_t> coarseVertices = marked;
    std::sort(coarseVertices.begin(), coarseVertices.end());
    std::vector<int32_t> childToCoarseVertex(V, -1);
    for (int32_t i = 0; i < coarseVertexCount; ++i) {
        childToCoarseVertex[coarseVertices[i]] = i;
    }

    std::vector<int32_t> facePoints;
    facePoints.reserve(coarseFaceCount);
    for (int32_t v = 0; v < V; ++v) {
        if (vertexClass[v] == CLASS_FACE) {
            facePoints.push_back(v);
        }
    }
    std::sort(facePoints.begin(), facePoints.end());
    std::vector<int32_t> childToCoarseFace(V, -1);
    for (int32_t i = 0; i < coarseFaceCount; ++i) {
        childToCoarseFace[facePoints[i]] = i;
    }

    // Child quads grouped around each face-point successor.
    std::vector<std::vector<int32_t>> quadsAtFacePoint(V);
    for (int32_t f = 0; f < F; ++f) {
        for (int32_t k = 0; k < 4; ++k) {
            const int32_t v = faceVerts[f][k];
            if (vertexClass[v] == CLASS_FACE) {
                quadsAtFacePoint[v].push_back(f);
                break;
            }
        }
    }

    // Walk the quads around every face point to recover the parent face loop.
    struct ChildQuadInfo {
        int32_t parentFace = -1;
        int32_t walkPosition = -1; // anchor position in the walked loop
    };
    std::vector<ChildQuadInfo> quadInfo(F);
    // Coarse face loops in the orientation of the input, before rotation.
    std::vector<std::vector<int32_t>> coarseLoops(coarseFaceCount);

    for (int32_t cf = 0; cf < coarseFaceCount; ++cf) {
        const int32_t fp = facePoints[cf];
        const std::vector<int32_t> &quads = quadsAtFacePoint[fp];
        if (quads.size() < 3) {
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
        std::vector<int32_t> loop;       // coarse vertex ids
        std::vector<int32_t> walkQuads;  // child quad at each walk position
        int32_t q = quads[0];
        int32_t guard = 0;
        do {
            int32_t facePos = -1;
            for (int32_t k = 0; k < 4; ++k) {
                if (faceVerts[q][k] == fp) {
                    facePos = k;
                    break;
                }
            }
            if (facePos == -1) {
                return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            }
            const int32_t anchorChild = faceVerts[q][(facePos + 2) & 3];
            if (vertexClass[anchorChild] != CLASS_VERTEX) {
                return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            }
            loop.push_back(childToCoarseVertex[anchorChild]);
            walkQuads.push_back(q);

            // Cross the halfedge from the previous edge-point to this face
            // point to reach the next quad around the face point.
            const int32_t prevEdgePoint = faceVerts[q][(facePos + 3) & 3];
            const auto it = directedMap.find(std::make_pair(prevEdgePoint, fp));
            if (it == directedMap.end()) {
                return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            }
            const int32_t twin = he[it->second].twinID;
            if (twin < 0) {
                return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            }
            q = he[twin].faceID;
            ++guard;
            if (guard > F + 1) {
                return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            }
        } while (q != quads[0]);

        const int32_t n = static_cast<int32_t>(loop.size());
        if (n != static_cast<int32_t>(quads.size())) {
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
        // Distinct corners: no collapsed parent face.
        std::vector<int32_t> tmp = loop;
        std::sort(tmp.begin(), tmp.end());
        for (int32_t i = 1; i < n; ++i) {
            if (tmp[i] == tmp[i - 1]) {
                return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            }
        }
        // Every quad incident to this face point must be visited once.
        std::vector<int32_t> visitedQuads = walkQuads;
        std::sort(visitedQuads.begin(), visitedQuads.end());
        if (visitedQuads != quads) {
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }

        // Crossing the (outgoing edge point -> face point) halfedge through
        // its twin lands in the quad anchored at the next corner in the
        // parent face's orientation, so the walk already follows the input
        // orientation and entry k holds the anchor corner at forward loop
        // index k.
        coarseLoops[cf] = loop;
        for (int32_t k = 0; k < n; ++k) {
            quadInfo[walkQuads[k]].parentFace = cf;
            quadInfo[walkQuads[k]].walkPosition = k;
        }
    }
    for (int32_t f = 0; f < F; ++f) {
        if (quadInfo[f].parentFace == -1) {
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
    }

    // Rotate every parent loop so its first coarse vertex is its smallest.
    // This is the frame in which coarse halfedges are emitted, so every
    // subsequent index (walkPosition, edges, parent corners) lives in this
    // single, final frame.
    for (int32_t cf = 0; cf < coarseFaceCount; ++cf) {
        auto &loop = coarseLoops[cf];
        const int32_t n = static_cast<int32_t>(loop.size());
        int32_t minPos = 0;
        for (int32_t k = 1; k < n; ++k) {
            if (loop[k] < loop[minPos]) {
                minPos = k;
            }
        }
        std::rotate(loop.begin(), loop.begin() + minPos, loop.end());
        const std::vector<int32_t> &quads = quadsAtFacePoint[facePoints[cf]];
        for (int32_t q : quads) {
            int32_t &pos = quadInfo[q].walkPosition;
            pos = (pos - minPos + n) % n;
        }
    }

    // ----------------------------------------------------------------------
    // 5. Coarse edges, numbered by first halfedge emission order.  The loops
    // already start at their smallest vertex, so this is exactly the order in
    // which the recovered halfedges are emitted below.
    // ----------------------------------------------------------------------
    struct EdgeKey {
        int32_t u, v;
        bool operator<(const EdgeKey &o) const {
            return u != o.u ? u < o.u : v < o.v;
        }
    };
    std::map<EdgeKey, int32_t> edgeIdByKey;
    std::vector<std::array<int32_t, 2>> edgeVerts;
    std::vector<std::vector<int32_t>> edgeFaces;
    // For every coarse face corner: its outgoing coarse edge id.
    std::vector<std::vector<int32_t>> faceCornerEdge(coarseFaceCount);
    int32_t coarseEdgeCount = 0;

    for (int32_t cf = 0; cf < coarseFaceCount; ++cf) {
        const auto &loop = coarseLoops[cf];
        const int32_t n = static_cast<int32_t>(loop.size());
        faceCornerEdge[cf].resize(n);
        for (int32_t k = 0; k < n; ++k) {
            const int32_t a = loop[k];
            const int32_t b = loop[(k + 1) % n];
            EdgeKey key{ std::min(a, b), std::max(a, b) };
            auto it = edgeIdByKey.find(key);
            if (it == edgeIdByKey.end()) {
                const int32_t e = coarseEdgeCount++;
                edgeIdByKey.emplace(key, e);
                edgeVerts.push_back({ key.u, key.v });
                edgeFaces.push_back({ cf });
                faceCornerEdge[cf][k] = e;
            } else {
                const int32_t e = it->second;
                edgeFaces[e].push_back(cf);
                faceCornerEdge[cf][k] = e;
            }
        }
    }
    // Closed manifold: every coarse edge borders exactly two faces.
    for (int32_t e = 0; e < coarseEdgeCount; ++e) {
        if (edgeFaces[e].size() != 2) {
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
    }

    // Coarse incidences.
    std::vector<std::vector<int32_t>> vertEdges(coarseVertexCount);
    std::vector<std::vector<int32_t>> vertFaces(coarseVertexCount);
    for (int32_t e = 0; e < coarseEdgeCount; ++e) {
        vertEdges[edgeVerts[e][0]].push_back(e);
        vertEdges[edgeVerts[e][1]].push_back(e);
    }
    for (int32_t cf = 0; cf < coarseFaceCount; ++cf) {
        for (int32_t v : coarseLoops[cf]) {
            vertFaces[v].push_back(cf);
        }
    }
    for (int32_t i = 0; i < coarseVertexCount; ++i) {
        if (vertEdges[i].empty() ||
            vertEdges[i].size() != vertFaces[i].size()) {
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
    }

    // Edge-successor -> parent coarse edge.  The anchor-relative quad order
    // is [anchor, outgoing edge point, face point, incoming edge point], so
    // the outgoing edge point (on the coarse edge leaving this corner) is the
    // stored vertex one slot before the face point, (facePos + 3) mod 4.
    // walkPosition already indexes the final, rotated loop.
    std::vector<int32_t> childToCoarseEdge(V, -1);
    for (int32_t cf = 0; cf < coarseFaceCount; ++cf) {
        const int32_t n = static_cast<int32_t>(coarseLoops[cf].size());
        const std::vector<int32_t> &quads = quadsAtFacePoint[facePoints[cf]];
        for (int32_t q : quads) {
            int32_t facePos = -1;
            for (int32_t k = 0; k < 4; ++k) {
                if (faceVerts[q][k] == facePoints[cf]) {
                    facePos = k;
                    break;
                }
            }
            if (facePos < 0) {
                return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            }
            const int32_t edgePoint = faceVerts[q][(facePos + 3) & 3];
            const int32_t corner = quadInfo[q].walkPosition;
            const int32_t e = faceCornerEdge[cf][corner % n];
            if (childToCoarseEdge[edgePoint] == -1) {
                childToCoarseEdge[edgePoint] = e;
            } else if (childToCoarseEdge[edgePoint] != e) {
                return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            }
        }
    }
    for (int32_t v = 0; v < V; ++v) {
        if (vertexClass[v] == CLASS_EDGE && childToCoarseEdge[v] == -1) {
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
    }

    // Subdivision preserves the Euler characteristic, so the recovered cage
    // must exhibit exactly the characteristic measured on the input.
    if (static_cast<int64_t>(coarseVertexCount) - coarseEdgeCount +
        coarseFaceCount !=
        static_cast<int64_t>(V) - E + F) {
        return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
    }

    // ----------------------------------------------------------------------
    // 6. Build the sparse one-level crease-free Gather operator A (in double)
    //
    // Rows follow input child vertices; columns follow coarse vertices.
    // Symbolic coefficients mirror ccs__Cage*Points_Gather exactly.
    // ----------------------------------------------------------------------
    std::vector<std::vector<std::pair<int32_t, double>>> rows(V);

    for (int32_t v = 0; v < V; ++v) {
        std::vector<std::pair<int32_t, double>> entries;
        if (vertexClass[v] == CLASS_VERTEX) {
            // Gather vertex rule, fully expanded into coarse corner weights:
            //   newV_i = (1/n^2) * (n V_i + sum_j V_j + sum_f F_f)
            //          + (1 - 3/n) V_i
            //          = (1 - 2/n) V_i
            //          + (1/n^2) * sum over edge neighbours V_j
            //          + (1/n^2) * sum over incident faces F_f,
            // with each F_f = (1/nf) sum over its corners.  Duplicate
            // columns (self / neighbour appearing through a face) are summed
            // by MergeEntries below.
            const int32_t i = childToCoarseVertex[v];
            const double n = static_cast<double>(vertEdges[i].size());
            entries.emplace_back(i, 1.0 - 2.0 / n);
            for (int32_t e : vertEdges[i]) {
                const int32_t j =
                    edgeVerts[e][0] == i ? edgeVerts[e][1] : edgeVerts[e][0];
                entries.emplace_back(j, 1.0 / (n * n));
            }
            for (int32_t cf : vertFaces[i]) {
                const double nf =
                    static_cast<double>(coarseLoops[cf].size());
                for (int32_t c : coarseLoops[cf]) {
                    entries.emplace_back(c, 1.0 / (n * n * nf));
                }
            }
        } else if (vertexClass[v] == CLASS_EDGE) {
            const int32_t e = childToCoarseEdge[v];
            const int32_t i = edgeVerts[e][0];
            const int32_t j = edgeVerts[e][1];
            entries.emplace_back(i, 0.25);
            entries.emplace_back(j, 0.25);
            for (int32_t cf : edgeFaces[e]) {
                const double nf =
                    static_cast<double>(coarseLoops[cf].size());
                for (int32_t c : coarseLoops[cf]) {
                    entries.emplace_back(c, 0.25 / nf);
                }
            }
        } else {
            const int32_t cf = childToCoarseFace[v];
            const double nf =
                static_cast<double>(coarseLoops[cf].size());
            for (int32_t c : coarseLoops[cf]) {
                entries.emplace_back(c, 1.0 / nf);
            }
        }
        rows[v] = MergeEntries(std::move(entries));
    }

    // Expand to three coordinate blocks and assemble CSR.
    const int32_t rowCount3 = 3 * V;
    const int32_t colCount3 = 3 * coarseVertexCount;
    SparseMatrix a;
    a.rowCount = rowCount3;
    a.colCount = colCount3;
    a.rowPtr.assign(rowCount3 + 1, 0);
    for (int32_t v = 0; v < V; ++v) {
        const int32_t nnz = static_cast<int32_t>(rows[v].size());
        a.rowPtr[3 * v + 1] = nnz;
        a.rowPtr[3 * v + 2] = nnz;
        a.rowPtr[3 * v + 3] = nnz;
    }
    for (int32_t r = 1; r <= rowCount3; ++r) {
        a.rowPtr[r] += a.rowPtr[r - 1];
    }
    const int32_t totalNnz = a.rowPtr.back();
    a.colInd.resize(totalNnz);
    a.value.resize(totalNnz);
    for (int32_t v = 0; v < V; ++v) {
        for (int32_t d = 0; d < 3; ++d) {
            int32_t dst = a.rowPtr[3 * v + d];
            for (const auto &e : rows[v]) {
                a.colInd[dst] = 3 * e.first + d;
                a.value[dst] = e.second;
                ++dst;
            }
        }
    }

    // Row-sum sanity: every Gather row is affine with coefficient sum 1.
    for (int32_t v = 0; v < V; ++v) {
        double sum = 0.0;
        for (const auto &e : rows[v]) {
            sum += e.second;
        }
        if (std::fabs(sum - 1.0) > 1e-12) {
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
    }

    // ----------------------------------------------------------------------
    // 7. Least squares min ||A X - Y||^2 via CG on the normal equations
    // ----------------------------------------------------------------------
    std::vector<double> rhs(rowCount3, 0.0);
    for (int32_t v = 0; v < V; ++v) {
        for (int32_t d = 0; d < 3; ++d) {
            rhs[3 * v + d] =
                static_cast<double>(mesh->vertexPoints[v].array[d]);
        }
    }

    std::vector<double> grad0(colCount3, 0.0);
    MultiplyTranspose(a, rhs.data(), grad0.data());
    int32_t operatorProducts = 1;
    const double stopScale = std::max(1.0, InfNorm(grad0));
    const double stopTolerance = 1e-10 * stopScale;

    // Jacobi preconditioner: diagonal of A^T A (same per coordinate block).
    std::vector<double> diag(coarseVertexCount, 0.0);
    for (int32_t v = 0; v < V; ++v) {
        for (const auto &e : rows[v]) {
            diag[e.first] += e.second * e.second;
        }
    }
    for (int32_t i = 0; i < coarseVertexCount; ++i) {
        if (!(diag[i] > 0.0)) {
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
    }

    std::vector<double> x(colCount3, 0.0);
    std::vector<double> r = grad0;   // r = A^T Y - A^T A x
    std::vector<double> z(colCount3, 0.0);
    std::vector<double> p(colCount3, 0.0);
    std::vector<double> ap(colCount3, 0.0);
    std::vector<double> tmp(rowCount3, 0.0);

    auto ApplyDiag = [&](const std::vector<double> &in,
                         std::vector<double> &out) {
        for (int32_t i = 0; i < coarseVertexCount; ++i) {
            const double inv = 1.0 / diag[i];
            for (int32_t d = 0; d < 3; ++d) {
                out[3 * i + d] = in[3 * i + d] * inv;
            }
        }
    };

    int32_t iterations = 0;
    bool converged = false;
    // With x = 0 the normal-equations residual is A^T Y itself; it can
    // already meet the stopping test for degenerate (e.g. all-zero) input.
    if (InfNorm(r) <= stopTolerance) {
        converged = true;
    }

    ApplyDiag(r, z);
    p = z;
    double rzOld = 0.0;
    for (int32_t k = 0; k < colCount3; ++k) {
        rzOld += r[k] * z[k];
    }
    if (!converged && (!(rzOld > 0.0) || !std::isfinite(rzOld))) {
        return CC_UNSUBDIVIDE_SOLVER_LIMIT;
    }

    for (int32_t it = 1; it <= maxIterations && !converged; ++it) {
        iterations = it;
        Multiply(a, p.data(), tmp.data());
        MultiplyTranspose(a, tmp.data(), ap.data());
        operatorProducts += 2;

        double pAp = 0.0;
        for (int32_t k = 0; k < colCount3; ++k) {
            pAp += p[k] * ap[k];
        }
        if (!(pAp > 0.0) || !std::isfinite(pAp)) {
            return CC_UNSUBDIVIDE_SOLVER_LIMIT;
        }
        const double alpha = rzOld / pAp;
        for (int32_t k = 0; k < colCount3; ++k) {
            x[k] += alpha * p[k];
            r[k] -= alpha * ap[k];
        }
        if (InfNorm(r) <= stopTolerance) {
            converged = true;
            break;
        }
        ApplyDiag(r, z);
        double rzNew = 0.0;
        for (int32_t k = 0; k < colCount3; ++k) {
            rzNew += r[k] * z[k];
        }
        if (!std::isfinite(rzNew) || rzNew <= 0.0) {
            return CC_UNSUBDIVIDE_SOLVER_LIMIT;
        }
        const double beta = rzNew / rzOld;
        for (int32_t k = 0; k < colCount3; ++k) {
            p[k] = z[k] + beta * p[k];
        }
        rzOld = rzNew;
    }
    if (!converged) {
        return CC_UNSUBDIVIDE_SOLVER_LIMIT;
    }

    // ----------------------------------------------------------------------
    // 8. Build the candidate coarse cage
    // ----------------------------------------------------------------------
    int32_t coarseHalfedgeCount = 0;
    for (const auto &loop : coarseLoops) {
        coarseHalfedgeCount += static_cast<int32_t>(loop.size());
    }
    if (coarseHalfedgeCount != 2 * coarseEdgeCount) {
        return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
    }

    cc_Mesh *cage = ccm_Create(coarseVertexCount, 0, coarseHalfedgeCount,
                               coarseEdgeCount, coarseFaceCount);
    if (!cage) {
        return CC_UNSUBDIVIDE_INVALID_INPUT;
    }

    std::vector<int32_t> faceBase(coarseFaceCount, 0);
    {
        int32_t base = 0;
        for (int32_t cf = 0; cf < coarseFaceCount; ++cf) {
            faceBase[cf] = base;
            base += static_cast<int32_t>(coarseLoops[cf].size());
        }
    }

    std::vector<std::array<int32_t, 2>> edgeHalfedges(coarseEdgeCount,
                                                      { -1, -1 });
    std::fill(cage->vertexToHalfedgeIDs,
              cage->vertexToHalfedgeIDs + coarseVertexCount, -1);

    for (int32_t cf = 0; cf < coarseFaceCount; ++cf) {
        const auto &loop = coarseLoops[cf];
        const int32_t n = static_cast<int32_t>(loop.size());
        const int32_t base = faceBase[cf];
        cage->faceToHalfedgeIDs[cf] = base;
        for (int32_t k = 0; k < n; ++k) {
            const int32_t h = base + k;
            const int32_t e = faceCornerEdge[cf][k];
            cc_Halfedge &out = cage->halfedges[h];
            out.twinID = -1;
            out.nextID = base + ((k + 1) % n);
            out.prevID = base + ((k + n - 1) % n);
            out.faceID = cf;
            out.edgeID = e;
            out.vertexID = loop[k];
            out.uvID = -1;
            if (cage->vertexToHalfedgeIDs[loop[k]] == -1) {
                cage->vertexToHalfedgeIDs[loop[k]] = h;
            }
            if (edgeHalfedges[e][0] == -1) {
                edgeHalfedges[e][0] = h;
            } else {
                edgeHalfedges[e][1] = h;
            }
        }
    }
    for (int32_t e = 0; e < coarseEdgeCount; ++e) {
        const int32_t h0 = edgeHalfedges[e][0];
        const int32_t h1 = edgeHalfedges[e][1];
        if (h0 < 0 || h1 < 0) {
            ccm_Release(cage);
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
        cage->halfedges[h0].twinID = h1;
        cage->halfedges[h1].twinID = h0;
        cage->edgeToHalfedgeIDs[e] = h0;
        cage->creases[e].nextID = e;
        cage->creases[e].prevID = e;
        cage->creases[e].sharpness = 0.0f;
    }
    for (int32_t i = 0; i < coarseVertexCount; ++i) {
        if (cage->vertexToHalfedgeIDs[i] == -1) {
            ccm_Release(cage);
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
        for (int32_t d = 0; d < 3; ++d) {
            cage->vertexPoints[i].array[d] =
                static_cast<float>(x[3 * i + d]);
        }
    }

    // ----------------------------------------------------------------------
    // 9. Forward verification with the native production Gather path
    // ----------------------------------------------------------------------
    cc_UnsubdivideStatus failure = CC_UNSUBDIVIDE_OK;
    cc_Subd *subd = ccs_Create(cage, 1);
    if (!subd) {
        ccm_Release(cage);
        return CC_UNSUBDIVIDE_INVALID_INPUT;
    }
    ccs_Refine_NoCreases_Gather(subd);

    const int32_t forwardV = ccm_VertexCountAtDepth(cage, 1);
    const int32_t forwardF = ccm_FaceCountAtDepth(cage, 1);
    const int32_t forwardH = ccm_HalfedgeCountAtDepth(cage, 1);
    if (forwardV != V || forwardF != F || forwardH != H) {
        failure = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
    }

    // Inverse role maps: depth-1 subd vertex id -> input child vertex.
    std::vector<int32_t> subdToChild(forwardV, -1);
    std::vector<int32_t> coarseEdgeToChild(coarseEdgeCount, -1);
    if (failure == CC_UNSUBDIVIDE_OK) {
        for (int32_t i = 0; i < coarseVertexCount; ++i) {
            subdToChild[i] = coarseVertices[i];
        }
        for (int32_t cf = 0; cf < coarseFaceCount; ++cf) {
            subdToChild[coarseVertexCount + cf] = facePoints[cf];
        }
        for (int32_t v = 0; v < V; ++v) {
            if (vertexClass[v] == CLASS_EDGE) {
                coarseEdgeToChild[childToCoarseEdge[v]] = v;
            }
        }
        for (int32_t e = 0; e < coarseEdgeCount; ++e) {
            if (coarseEdgeToChild[e] == -1) {
                failure = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                break;
            }
            subdToChild[coarseVertexCount + coarseFaceCount + e] =
                coarseEdgeToChild[e];
        }
    }

    // Check every point against the input child positions.
    double maxForwardResidual = 0.0;
    if (failure == CC_UNSUBDIVIDE_OK) {
        for (int32_t sv = 0; sv < forwardV; ++sv) {
            const int32_t cv = subdToChild[sv];
            if (cv < 0) {
                failure = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                break;
            }
            const cc_VertexPoint p = ccs_VertexPoint(subd, sv, 1);
            double dist2 = 0.0;
            for (int32_t d = 0; d < 3; ++d) {
                const double delta =
                    static_cast<double>(p.array[d]) -
                    static_cast<double>(mesh->vertexPoints[cv].array[d]);
                dist2 += delta * delta;
            }
            const double dist = std::sqrt(dist2);
            if (dist > maxForwardResidual) {
                maxForwardResidual = dist;
            }
        }
    }

    // Check every child face adjacency through the parent mapping.
    if (failure == CC_UNSUBDIVIDE_OK) {
        for (int32_t q = 0; q < F; ++q) {
            const int32_t cf = quadInfo[q].parentFace;
            const int32_t n =
                static_cast<int32_t>(coarseLoops[cf].size());
            const int32_t corner = quadInfo[q].walkPosition;
            // Child face id of (parent face cf, anchor corner) equals the
            // coarse halfedge base[cf] + corner (see ccs__RefineCageHalfedges).
            const int32_t childFace = faceBase[cf] + corner;
            if (childFace < 0 || childFace >= forwardF) {
                failure = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                break;
            }
            const int32_t subEdge0 =
                faceCornerEdge[cf][corner];
            const int32_t subEdge1 =
                faceCornerEdge[cf][(corner + n - 1) % n];
            // Subd vertex order inside a child quad starting at its anchor:
            // coarse vertex, outgoing edge point, parent face point,
            // incoming edge point.
            const int32_t subdIds[4] = {
                coarseLoops[cf][corner],
                coarseVertexCount + coarseFaceCount + subEdge0,
                coarseVertexCount + cf,
                coarseVertexCount + coarseFaceCount + subEdge1
            };
            // Anchor position inside the input child quad.
            int32_t anchorPos = -1;
            for (int32_t k = 0; k < 4; ++k) {
                if (faceVerts[q][k] ==
                    subdToChild[subdIds[0]]) {
                    anchorPos = k;
                    break;
                }
            }
            if (anchorPos == -1) {
                failure = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                break;
            }
            for (int32_t k = 0; k < 4; ++k) {
                const int32_t sv = ccs_HalfedgeVertexID(
                    subd, 4 * childFace + k, 1);
                if (sv < 0 || sv >= forwardV ||
                    subdToChild[sv] !=
                        faceVerts[q][(anchorPos + k) & 3]) {
                    failure = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                    break;
                }
            }
            if (failure != CC_UNSUBDIVIDE_OK) {
                break;
            }
        }
    }

    if (failure == CC_UNSUBDIVIDE_OK &&
        maxForwardResidual > tolerance) {
        failure = CC_UNSUBDIVIDE_RESIDUAL_EXCEEDED;
    }

    ccs_Release(subd);
    if (failure != CC_UNSUBDIVIDE_OK) {
        ccm_Release(cage);
        return failure;
    }

    // ----------------------------------------------------------------------
    // 10. Publish the result
    // ----------------------------------------------------------------------
    cc_UnsubdivideResult *result = new (std::nothrow) cc_UnsubdivideResult();
    if (!result) {
        ccm_Release(cage);
        return CC_UNSUBDIVIDE_INVALID_INPUT;
    }
    result->vertexClass = new (std::nothrow) int32_t[V];
    result->vertexParent = new (std::nothrow) int32_t[V];
    result->childParentFace = new (std::nothrow) int32_t[F];
    result->childParentCorner = new (std::nothrow) int32_t[F];
    if (!result->vertexClass || !result->vertexParent ||
        !result->childParentFace || !result->childParentCorner) {
        delete[] result->vertexClass;
        delete[] result->vertexParent;
        delete[] result->childParentFace;
        delete[] result->childParentCorner;
        delete result;
        ccm_Release(cage);
        return CC_UNSUBDIVIDE_INVALID_INPUT;
    }

    for (int32_t v = 0; v < V; ++v) {
        result->vertexClass[v] = vertexClass[v];
        if (vertexClass[v] == CLASS_VERTEX) {
            result->vertexParent[v] = childToCoarseVertex[v];
        } else if (vertexClass[v] == CLASS_EDGE) {
            result->vertexParent[v] = childToCoarseEdge[v];
        } else {
            result->vertexParent[v] = childToCoarseFace[v];
        }
    }
    for (int32_t q = 0; q < F; ++q) {
        result->childParentFace[q] = quadInfo[q].parentFace;
        result->childParentCorner[q] = quadInfo[q].walkPosition;
    }

    result->mesh = cage;
    result->coarseVertexCount = coarseVertexCount;
    result->coarseEdgeCount = coarseEdgeCount;
    result->coarseFaceCount = coarseFaceCount;
    result->iterations = iterations;
    result->operatorProducts = operatorProducts;
    result->nonZeroCount = static_cast<int64_t>(totalNnz);
    result->maxForwardResidual = maxForwardResidual;

    *outResult = result;
    return CC_UNSUBDIVIDE_OK;
}
