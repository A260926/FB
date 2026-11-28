/*
 * cc_Unsubdivide.cpp
 *
 * Inverse Catmull-Clark: recover a coarse polygon cage from one step of the
 * upstream (crease-free) subdivision.  See cc_Unsubdivide.h for the contract.
 *
 * The implementation never calls the native traversal routines on unvalidated
 * caller data.  The input is first copied and structurally validated through
 * plain bounds-checked array reads; only the freshly built candidate cage is
 * handed to the upstream library.
 */

#include "cc_Unsubdivide.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace {

using i32 = int32_t;
using i64 = int64_t;

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

inline bool inRange(i32 value, i32 count) {
    return value >= 0 && value < count;
}

inline bool finiteInBounds(float value) {
    if (!std::isfinite((double)value)) {
        return false;
    }
    return value >= -10.0f && value <= 10.0f;
}

/* ------------------------------------------------------------------------- */
/* Validated, index-agnostic copy of the input mesh                          */
/* ------------------------------------------------------------------------- */

struct Raw {
    i32 V = 0;
    i32 H = 0;
    i32 E = 0;
    i32 F = 0;

    std::vector<i32> next;
    std::vector<i32> prev;
    std::vector<i32> twin;
    std::vector<i32> face;
    std::vector<i32> edge;
    std::vector<i32> vert;
    std::vector<i32> vHalf;
    std::vector<i32> eHalf;
    std::vector<i32> fHalf;
    std::vector<cc_VertexPoint> pos;

    /* Four halfedges of every face, in loop order, once validated. */
    std::vector<std::array<i32, 4>> quadH;
};

/* ------------------------------------------------------------------------- */
/* Input validation                                                          */
/* ------------------------------------------------------------------------- */

cc_UnsubdivideStatus validateInput(const cc_Mesh *mesh, Raw &raw) {
    if (mesh == nullptr) {
        return CC_UNSUBDIVIDE_INVALID_INPUT;
    }
    if (mesh->vertexCount <= 0 || mesh->faceCount <= 0 ||
        mesh->halfedgeCount <= 0 || mesh->edgeCount <= 0) {
        return CC_UNSUBDIVIDE_INVALID_INPUT;
    }
    if (mesh->uvCount != 0) {
        /* UV-carrying meshes are outside the supported input. */
        return CC_UNSUBDIVIDE_INVALID_INPUT;
    }
    if (mesh->vertexToHalfedgeIDs == nullptr ||
        mesh->edgeToHalfedgeIDs == nullptr ||
        mesh->faceToHalfedgeIDs == nullptr ||
        mesh->vertexPoints == nullptr ||
        mesh->halfedges == nullptr ||
        mesh->creases == nullptr) {
        return CC_UNSUBDIVIDE_INVALID_INPUT;
    }

    const i32 V = mesh->vertexCount;
    const i32 H = mesh->halfedgeCount;
    const i32 E = mesh->edgeCount;
    const i32 F = mesh->faceCount;

    if (H != 2 * E) {
        return CC_UNSUBDIVIDE_INVALID_INPUT;
    }
    if ((i64)4 * F != (i64)H) {
        /* The supported input is quad-only. */
        return CC_UNSUBDIVIDE_INVALID_INPUT;
    }

    raw.V = V;
    raw.H = H;
    raw.E = E;
    raw.F = F;
    raw.next.resize(H);
    raw.prev.resize(H);
    raw.twin.resize(H);
    raw.face.resize(H);
    raw.edge.resize(H);
    raw.vert.resize(H);
    raw.vHalf.resize(V);
    raw.eHalf.resize(E);
    raw.fHalf.resize(F);
    raw.pos.resize(V);

    /* Positions: finite and inside [-10, 10]. */
    for (i32 v = 0; v < V; ++v) {
        const cc_VertexPoint p = mesh->vertexPoints[v];
        if (!finiteInBounds(p.x) || !finiteInBounds(p.y) ||
            !finiteInBounds(p.z)) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
        raw.pos[v] = p;
    }

    /* Creases: none may be active. */
    for (i32 e = 0; e < E; ++e) {
        const float sharpness = mesh->creases[e].sharpness;
        if (!std::isfinite((double)sharpness) || sharpness != 0.0f) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
    }

    /* Halfedge fields and index ranges. */
    for (i32 h = 0; h < H; ++h) {
        const cc_Halfedge &src = mesh->halfedges[h];
        if (!inRange(src.twinID, H) || !inRange(src.nextID, H) ||
            !inRange(src.prevID, H) || !inRange(src.faceID, F) ||
            !inRange(src.edgeID, E) || !inRange(src.vertexID, V)) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
        raw.twin[h] = src.twinID;
        raw.next[h] = src.nextID;
        raw.prev[h] = src.prevID;
        raw.face[h] = src.faceID;
        raw.edge[h] = src.edgeID;
        raw.vert[h] = src.vertexID;
    }

    /* Mapping arrays. */
    for (i32 v = 0; v < V; ++v) {
        const i32 h = mesh->vertexToHalfedgeIDs[v];
        if (!inRange(h, H) || raw.vert[h] != v) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
        raw.vHalf[v] = h;
    }
    for (i32 e = 0; e < E; ++e) {
        const i32 h = mesh->edgeToHalfedgeIDs[e];
        if (!inRange(h, H) || raw.edge[h] != e) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
        raw.eHalf[e] = h;
    }
    for (i32 f = 0; f < F; ++f) {
        const i32 h = mesh->faceToHalfedgeIDs[f];
        if (!inRange(h, H) || raw.face[h] != f) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
        raw.fHalf[f] = h;
    }

    /* next / prev consistency. */
    for (i32 h = 0; h < H; ++h) {
        if (raw.prev[raw.next[h]] != h || raw.next[raw.prev[h]] != h) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
    }

    /* Twin consistency: bidirectional, same edge, opposite direction. */
    for (i32 h = 0; h < H; ++h) {
        const i32 t = raw.twin[h];
        if (t == h || raw.twin[t] != h || raw.edge[t] != raw.edge[h]) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
        if (raw.vert[t] != raw.vert[raw.next[h]] ||
            raw.vert[raw.next[t]] != raw.vert[h]) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
    }

    /* Faces: closed four-cycles with distinct vertices and edges. */
    std::vector<int8_t> seenHalfedge(H, 0);
    raw.quadH.resize(F);
    for (i32 f = 0; f < F; ++f) {
        const i32 start = raw.fHalf[f];
        i32 h = start;
        i32 count = 0;
        for (; count < 4; ++count) {
            if (!inRange(h, H) || raw.face[h] != f) {
                return CC_UNSUBDIVIDE_INVALID_INPUT;
            }
            raw.quadH[f][count] = h;
            h = raw.next[h];
        }
        if (h != start) {
            /* A face that is not a quad is outside the supported input. */
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }

        const std::array<i32, 4> &q = raw.quadH[f];
        for (i32 i = 0; i < 4; ++i) {
            if (seenHalfedge[q[i]] != 0) {
                return CC_UNSUBDIVIDE_INVALID_INPUT;
            }
            seenHalfedge[q[i]] = 1;
        }
        /* No repeated vertex or edge inside a face. */
        for (i32 i = 0; i < 4; ++i) {
            for (i32 j = i + 1; j < 4; ++j) {
                if (raw.vert[q[i]] == raw.vert[q[j]] ||
                    raw.edge[q[i]] == raw.edge[q[j]]) {
                    return CC_UNSUBDIVIDE_INVALID_INPUT;
                }
            }
        }
    }
    for (i32 h = 0; h < H; ++h) {
        if (seenHalfedge[h] == 0) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
    }

    /* Edges: exactly two halfedges each, and they are twins. */
    std::vector<i32> edgeCount(E, 0);
    std::vector<i32> edgeFirst(E, -1);
    for (i32 h = 0; h < H; ++h) {
        const i32 e = raw.edge[h];
        ++edgeCount[e];
        if (edgeFirst[e] < 0) {
            edgeFirst[e] = h;
        }
    }
    for (i32 e = 0; e < E; ++e) {
        if (edgeCount[e] != 2) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
        const i32 h = edgeFirst[e];
        if (raw.twin[h] == h) {
            return CC_UNSUBDIVIDE_INVALID_INPUT;
        }
    }

    /* Every vertex fan must be a single rotation cycle. */
    std::vector<i32> outCount(V, 0);
    for (i32 h = 0; h < H; ++h) {
        ++outCount[raw.vert[h]];
    }
    for (i32 v = 0; v < V; ++v) {
        if (outCount[v] < 3) {
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
        const i32 start = raw.vHalf[v];
        i32 h = start;
        i32 steps = 0;
        do {
            if (raw.vert[h] != v) {
                return CC_UNSUBDIVIDE_INVALID_INPUT;
            }
            h = raw.twin[raw.prev[h]];
            if (!inRange(h, H)) {
                return CC_UNSUBDIVIDE_INVALID_INPUT;
            }
            ++steps;
            if (steps > outCount[v]) {
                return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            }
        } while (h != start);
        if (steps != outCount[v]) {
            /* Several disjoint fans meet at this vertex. */
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
    }

    return CC_UNSUBDIVIDE_OK;
}

/* ------------------------------------------------------------------------- */
/* Vertex classification                                                     */
/* ------------------------------------------------------------------------- */

struct Classification {
    std::vector<i32> cls;      /* CLASS_ORIGIN / EDGE / FACE per input vertex */
    std::vector<i32> originId; /* coarse vertex id, or -1 */
    std::vector<i32> faceId;   /* coarse face id, or -1 */
    std::vector<i32> edgeId;   /* coarse edge id, or -1 */
};

/* ------------------------------------------------------------------------- */
/* Recovered coarse topology                                                 */
/* ------------------------------------------------------------------------- */

struct CoarseTopo {
    i32 V = 0; /* coarse vertices */
    i32 F = 0; /* coarse faces */
    i32 E = 0; /* coarse edges */
    i32 H = 0; /* coarse halfedges */

    std::vector<std::vector<i32>> loops; /* per face: coarse vertex ids */
    std::vector<i32> loopBase;           /* size F + 1, halfedge offsets */

    std::vector<i32> hVert;
    std::vector<i32> hNext;
    std::vector<i32> hPrev;
    std::vector<i32> hFace;
    std::vector<i32> hEdge;
    std::vector<i32> hTwin;

    std::vector<i32> vHalf;
    std::vector<i32> eHalf;
    std::vector<i32> fHalf;

    std::vector<std::pair<i32, i32>> edgeEnds; /* sorted endpoints */
    std::vector<std::array<i32, 2>> edgeFaces; /* two adjacent coarse faces */

    std::vector<std::vector<i32>> vertFaces;
    std::vector<std::vector<i32>> vertEdges;
};

/* ------------------------------------------------------------------------- */
/* Sparse one-step operator A                                                */
/* ------------------------------------------------------------------------- */

struct SparseRow {
    std::vector<std::pair<i32, double>> terms;
};

void normalizeRow(SparseRow &row) {
    std::sort(row.terms.begin(), row.terms.end(),
              [](const std::pair<i32, double> &a,
                 const std::pair<i32, double> &b) { return a.first < b.first; });
    std::vector<std::pair<i32, double>> merged;
    merged.reserve(row.terms.size());
    for (const auto &term : row.terms) {
        if (!merged.empty() && merged.back().first == term.first) {
            merged.back().second += term.second;
        } else {
            merged.push_back(term);
        }
    }
    row.terms.clear();
    for (const auto &term : merged) {
        if (term.second != 0.0) {
            row.terms.push_back(term);
        }
    }
}

void addTerm(SparseRow &row, i32 col, double weight) {
    row.terms.emplace_back(col, weight);
}

/* ------------------------------------------------------------------------- */
/* Least-squares solve of  min || A X - Y ||^2                               */
/* ------------------------------------------------------------------------- */

struct SolveReport {
    i64 iterations = 0;
    i64 operatorProducts = 0;
    i64 nonzeroCoefficients = 0;
    double maxForwardResidual = 0.0;
};

double dotProduct(const std::vector<double> &a, const std::vector<double> &b) {
    double sum = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        sum += a[i] * b[i];
    }
    return sum;
}

double infNorm(const std::vector<double> &a) {
    double m = 0.0;
    for (double x : a) {
        const double ax = std::fabs(x);
        if (ax > m) {
            m = ax;
        }
    }
    return m;
}

/* out = A * x, with three interleaved coordinates. */
void applyA(const std::vector<SparseRow> &rows,
            const std::vector<double> &x,
            std::vector<double> &out) {
    const size_t rowCount = rows.size();
    out.assign(rowCount * 3, 0.0);
    for (size_t i = 0; i < rowCount; ++i) {
        const SparseRow &row = rows[i];
        double acc[3] = {0.0, 0.0, 0.0};
        for (const auto &term : row.terms) {
            const double w = term.second;
            const size_t base = (size_t)term.first * 3;
            acc[0] += w * x[base + 0];
            acc[1] += w * x[base + 1];
            acc[2] += w * x[base + 2];
        }
        out[i * 3 + 0] = acc[0];
        out[i * 3 + 1] = acc[1];
        out[i * 3 + 2] = acc[2];
    }
}

/* out = A^T * x. */
void applyAT(const std::vector<SparseRow> &rows,
             const std::vector<double> &x,
             size_t unknownCount,
             std::vector<double> &out) {
    out.assign(unknownCount * 3, 0.0);
    for (size_t i = 0; i < rows.size(); ++i) {
        const SparseRow &row = rows[i];
        const double v0 = x[i * 3 + 0];
        const double v1 = x[i * 3 + 1];
        const double v2 = x[i * 3 + 2];
        for (const auto &term : row.terms) {
            const double w = term.second;
            const size_t base = (size_t)term.first * 3;
            out[base + 0] += w * v0;
            out[base + 1] += w * v1;
            out[base + 2] += w * v2;
        }
    }
}

/*
 * Solve the normal equations  A^T A X = A^T Y  with conjugate gradients,
 * matrix-free.  The documented stopping test is on the true residual
 *   || A^T (A X - Y) ||_inf <= threshold.
 * CG runs on its recursive residual, then the true residual is re-evaluated
 * at every iterate; once it passes we return.  If the recursive and true
 * residuals disagree we restart CG from the true residual.
 */
bool solveNormalEquations(const std::vector<SparseRow> &rows,
                          const std::vector<double> &Y,
                          i32 unknownCount,
                          i32 maxIterations,
                          double threshold,
                          std::vector<double> &X,
                          SolveReport &report) {
    const size_t unknown = (size_t)unknownCount * 3;
    const size_t rowCount = rows.size() * 3;

    std::vector<double> b(unknown, 0.0);
    applyAT(rows, Y, (size_t)unknownCount, b);
    report.operatorProducts += 1;

    X.assign(unknown, 0.0);
    std::vector<double> r = b;
    std::vector<double> p = r;
    double rr = dotProduct(r, r);

    std::vector<double> q;
    std::vector<double> atq(unknown, 0.0);
    std::vector<double> ax;
    std::vector<double> residual(rowCount, 0.0);
    std::vector<double> trueResidual(unknown, 0.0);

    /* Evaluate the true residual; return its inf norm. */
    auto trueInfNorm = [&](const std::vector<double> &x) -> double {
        applyA(rows, x, ax);
        report.operatorProducts += 1;
        for (size_t i = 0; i < rowCount; ++i) {
            residual[i] = ax[i] - Y[i];
        }
        applyAT(rows, residual, (size_t)unknownCount, trueResidual);
        report.operatorProducts += 1;
        return infNorm(trueResidual);
    };

    if (trueInfNorm(X) <= threshold) {
        return true;
    }

    while (report.iterations < (i64)maxIterations) {
        applyA(rows, p, q);
        report.operatorProducts += 1;
        const double qq = dotProduct(q, q);
        if (!(qq > 0.0) || !std::isfinite(qq)) {
            break;
        }
        const double alpha = rr / qq;
        for (size_t i = 0; i < unknown; ++i) {
            X[i] += alpha * p[i];
        }
        applyAT(rows, q, (size_t)unknownCount, atq);
        report.operatorProducts += 1;
        for (size_t i = 0; i < unknown; ++i) {
            r[i] -= alpha * atq[i];
        }
        ++report.iterations;

        /* Track the true residual at every iterate. */
        const double exact = trueInfNorm(X);
        if (exact <= threshold) {
            return true;
        }

        const double rrNew = dotProduct(r, r);
        if (!(rrNew > 0.0) || !std::isfinite(rrNew)) {
            break;
        }

        /*
         * If the recursive residual has collapsed far below the true
         * residual (loss of orthogonality), restart from the true one.
         */
        const double recInf = infNorm(r);
        if (recInf < exact * 1e-3) {
            r = trueResidual;
            rr = dotProduct(r, r);
            p = r;
            if (!(rr > 0.0)) {
                break;
            }
            /*
             * Restarted CG counts its own first iteration; the outer loop
             * budget still applies.
             */
            continue;
        }

        const double beta = rrNew / rr;
        for (size_t i = 0; i < unknown; ++i) {
            p[i] = r[i] + beta * p[i];
        }
        rr = rrNew;
    }

    return false;
}

} /* namespace */

/* ========================================================================= */
/* Main entry point                                                          */
/* ========================================================================= */

extern "C" cc_UnsubdivideStatus
ccu_Unsubdivide(const cc_Mesh *mesh,
                int32_t originVertex,
                double tolerance,
                int32_t maxIterations,
                cc_UnsubdivideResult *outResult) {
    if (outResult == nullptr) {
        return CC_UNSUBDIVIDE_INVALID_INPUT;
    }
    std::memset(outResult, 0, sizeof(*outResult));
    outResult->status = CC_UNSUBDIVIDE_INVALID_INPUT;

    if (mesh == nullptr || !std::isfinite(tolerance) || tolerance <= 0.0 ||
        maxIterations <= 0) {
        return CC_UNSUBDIVIDE_INVALID_INPUT;
    }
    outResult->tolerance = tolerance;

    Raw raw;
    cc_UnsubdivideStatus status = validateInput(mesh, raw);
    if (status != CC_UNSUBDIVIDE_OK) {
        outResult->status = status;
        return status;
    }

    if (originVertex < 0 || originVertex >= raw.V) {
        outResult->status = CC_UNSUBDIVIDE_INVALID_INPUT;
        return CC_UNSUBDIVIDE_INVALID_INPUT;
    }

    /*
     * Vertex classes, decided from adjacency alone.
     *
     * A child quad is [P, E, F, E] up to rotation (P: original vertex
     * point, E: edge point, F: face point).  Around every child quad the
     * two E corners and the P/F corners alternate, so the vertex graph has a
     * unique (up to a global flip) checkerboard colouring: one colour is
     * the edge-point side, the other the original/face side.
     *
     * The two sides are told apart intrinsically:
     *   - an edge point belongs to exactly two child quads, one for each of
     *     the two coarse faces adjacent to its coarse edge;
     *   - an original point belongs to one child quad per incident coarse
     *     face, i.e. its coarse valence (>= 3 for a closed manifold);
     *   - a face point belongs to one child quad per parent-face corner,
     *     i.e. the coarse face degree (>= 3).
     * Within a quad, the two same-colour corners are diagonal, so the
     * diagonal-degree of a vertex equals its number of incident quads.
     * Hence the edge side is exactly the colour whose *every* vertex has
     * diagonal-degree 2; the other side has all degrees >= 3.
     *
     * This is marker-independent; the marker then chooses the bipartition of
     * the original/face side, i.e. which side is "original" and which is
     * "face".  The marker must lie on the original/face side.
     */
    const i32 V = raw.V;

    /* 1. Checkerboard colouring of the input vertex graph. */
    std::vector<i32> vcolor(V, -1);
    {
        std::vector<i32> queue;
        queue.reserve(V);
        for (i32 seed = 0; seed < V; ++seed) {
            if (vcolor[seed] >= 0) {
                continue;
            }
            if (seed != 0) {
                /* More than one connected component. */
                outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            }
            vcolor[seed] = 0;
            queue.clear();
            queue.push_back(seed);
            for (size_t qi = 0; qi < queue.size(); ++qi) {
                const i32 v = queue[qi];
                const i32 start = raw.vHalf[v];
                i32 h = start;
                do {
                    const i32 to = raw.vert[raw.next[h]];
                    if (vcolor[to] < 0) {
                        vcolor[to] = vcolor[v] ^ 1;
                        queue.push_back(to);
                    } else if (vcolor[to] == vcolor[v]) {
                        /* Odd cycle: not a quadrangulated subdivision image. */
                        outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                        return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                    }
                    h = raw.twin[raw.prev[h]];
                } while (h != start);
            }
        }
    }

    if (!inRange(originVertex, V)) {
        outResult->status = CC_UNSUBDIVIDE_INVALID_INPUT;
        return CC_UNSUBDIVIDE_INVALID_INPUT;
    }

    /* 2. Diagonal-degree of every vertex, per checkerboard colour. */
    std::vector<i32> quadCount(V, 0);
    for (i32 f = 0; f < raw.F; ++f) {
        const std::array<i32, 4> &q = raw.quadH[f];
        for (i32 j = 0; j < 4; ++j) {
            ++quadCount[raw.vert[q[j]]];
        }
    }

    /*
     * The edge-point side is always 4-regular: every edge point has exactly
     * four neighbours.  The original/face side is 4-regular only when the
     * coarse mesh is a regular quad mesh (all coarse valences and all coarse
     * face degrees equal 4, e.g. a regular torus).
     *
     *   - exactly one side 4-regular  -> that side is the edge points;
     *   - both sides 4-regular        -> the marker selects the topology, so
     *                                    the marker's side is original/face;
     *   - neither side 4-regular      -> not a subdivision image.
     */
    i32 edgeColor = -1;
    i32 pfColor = -1;
    {
        std::vector<i32> degree(V, 0);
        for (i32 v = 0; v < V; ++v) {
            const i32 start = raw.vHalf[v];
            i32 h = start;
            i32 n = 0;
            do {
                ++n;
                h = raw.twin[raw.prev[h]];
            } while (h != start);
            degree[v] = n;
        }
        bool allFour[2] = {true, true};
        i32 count[2] = {0, 0};
        for (i32 v = 0; v < V; ++v) {
            ++count[vcolor[v]];
            if (degree[v] != 4) {
                allFour[vcolor[v]] = false;
            }
        }
        if (count[0] == 0 || count[1] == 0) {
            outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
        if (allFour[0] && !allFour[1]) {
            edgeColor = 0;
            pfColor = 1;
        } else if (!allFour[0] && allFour[1]) {
            edgeColor = 1;
            pfColor = 0;
        } else if (allFour[0] && allFour[1]) {
            /* Regular mesh: both sides are 4-regular; marker decides. */
            pfColor = vcolor[originVertex];
            edgeColor = pfColor ^ 1;
        } else {
            outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
    }

    /* Marker must be an original/face-side vertex, never an edge point. */
    if (vcolor[originVertex] != pfColor) {
        outResult->status = CC_UNSUBDIVIDE_INVALID_INPUT;
        return CC_UNSUBDIVIDE_INVALID_INPUT;
    }

    std::vector<i32> cls(V, -1);
    for (i32 v = 0; v < V; ++v) {
        if (vcolor[v] == edgeColor) {
            cls[v] = CC_UNSUBDIVIDE_CLASS_EDGE;
        }
    }

    /*
     * 3. Bipartition the original/face side along child-quad diagonals.
     * Each child quad contributes exactly one diagonal original/face edge,
     * so that graph is the coarse vertex/face incidence graph.  The marker
     * fixes which bipartition side is "original".
     */
    std::vector<i32> ofColor(V, -1);
    {
        std::vector<std::vector<i32>> diag(V);
        for (i32 f = 0; f < raw.F; ++f) {
            const std::array<i32, 4> &q = raw.quadH[f];
            i32 a = -1;
            i32 b = -1;
            for (i32 j = 0; j < 4; ++j) {
                const i32 x = raw.vert[q[j]];
                if (vcolor[x] == pfColor) {
                    if (a < 0) {
                        a = x;
                    } else if (b < 0) {
                        b = x;
                    } else {
                        /* More than two original/face corners in a quad. */
                        outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                        return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                    }
                }
            }
            if (a < 0 || b < 0) {
                outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            }
            /* The two same-colour corners of a quad are diagonal. */
            i32 posA = -1, posB = -1;
            for (i32 j = 0; j < 4; ++j) {
                if (raw.vert[q[j]] == a) posA = j;
                if (raw.vert[q[j]] == b) posB = j;
            }
            if (posA < 0 || posB < 0 || ((posA - posB) & 1) != 0) {
                outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            }
            diag[a].push_back(b);
            diag[b].push_back(a);
        }

        std::vector<i32> queue;
        queue.reserve(V);
        ofColor[originVertex] = 0; /* 0 == original side */
        queue.push_back(originVertex);
        for (size_t qi = 0; qi < queue.size(); ++qi) {
            const i32 v = queue[qi];
            for (i32 n : diag[v]) {
                if (ofColor[n] < 0) {
                    ofColor[n] = ofColor[v] ^ 1;
                    queue.push_back(n);
                } else if (ofColor[n] == ofColor[v]) {
                    outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                    return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                }
            }
        }
        for (i32 v = 0; v < V; ++v) {
            if (vcolor[v] == pfColor && ofColor[v] < 0) {
                /* The original/face incidence graph must be connected. */
                outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            }
        }
    }

    for (i32 v = 0; v < V; ++v) {
        if (vcolor[v] == pfColor) {
            cls[v] = (ofColor[v] == 0) ? CC_UNSUBDIVIDE_CLASS_ORIGIN
                                       : CC_UNSUBDIVIDE_CLASS_FACE;
        }
    }

    /*
     * Local class pattern validation.
     *
     * Original vertex points see only edge points and have the coarse
     * valence (>= 3).  Face points also see only edge points and have the
     * parent face degree (>= 3; 4 for a quad-only coarse mesh, but 3, 5, ...
     * for a recovered triangular or pentagonal parent).  Edge points always
     * have degree 4 and see exactly two original and two face points.
     */
    for (i32 v = 0; v < V; ++v) {
        const i32 start = raw.vHalf[v];
        i32 h = start;
        i32 originNeighbors = 0;
        i32 faceNeighbors = 0;
        i32 edgeNeighbors = 0;
        i32 degree = 0;
        do {
            ++degree;
            const i32 to = raw.vert[raw.next[h]];
            switch (cls[to]) {
                case CC_UNSUBDIVIDE_CLASS_ORIGIN: ++originNeighbors; break;
                case CC_UNSUBDIVIDE_CLASS_EDGE:   ++edgeNeighbors;   break;
                case CC_UNSUBDIVIDE_CLASS_FACE:   ++faceNeighbors;   break;
                default:
                    outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                    return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            }
            h = raw.twin[raw.prev[h]];
        } while (h != start);

        if (cls[v] == CC_UNSUBDIVIDE_CLASS_ORIGIN) {
            if (degree < 3 || edgeNeighbors != degree || originNeighbors ||
                faceNeighbors) {
                outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            }
        } else if (cls[v] == CC_UNSUBDIVIDE_CLASS_FACE) {
            if (degree < 3 || edgeNeighbors != degree || originNeighbors ||
                faceNeighbors) {
                outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            }
        } else {
            if (degree != 4 || originNeighbors != 2 || faceNeighbors != 2 ||
                edgeNeighbors != 0) {
                outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            }
        }
    }

    /*
     * Quad-level pattern: exactly one original point, one face point and two
     * edge points, alternating around the quad (the original and face points
     * sit diagonally opposite each other).
     */
    for (i32 f = 0; f < raw.F; ++f) {
        const std::array<i32, 4> &q = raw.quadH[f];
        i32 seenOrigin = 0, seenFace = 0, seenEdge = 0;
        for (i32 j = 0; j < 4; ++j) {
            switch (cls[raw.vert[q[j]]]) {
                case CC_UNSUBDIVIDE_CLASS_ORIGIN: ++seenOrigin; break;
                case CC_UNSUBDIVIDE_CLASS_FACE: ++seenFace; break;
                case CC_UNSUBDIVIDE_CLASS_EDGE: ++seenEdge; break;
                default: break;
            }
        }
        if (seenOrigin != 1 || seenFace != 1 || seenEdge != 2) {
            outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
        i32 originPos = -1;
        i32 facePos = -1;
        for (i32 j = 0; j < 4; ++j) {
            if (cls[raw.vert[q[j]]] == CC_UNSUBDIVIDE_CLASS_ORIGIN) {
                originPos = j;
            } else if (cls[raw.vert[q[j]]] == CC_UNSUBDIVIDE_CLASS_FACE) {
                facePos = j;
            }
        }
        if (((originPos - facePos) & 1) != 0) {
            outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
    }

    /* Coarse vertex / face numbering is driven by ascending input indices. */
    Classification map;
    map.cls = cls;
    map.originId.assign(V, -1);
    map.faceId.assign(V, -1);
    map.edgeId.assign(V, -1);

    i32 coarseVertexCount = 0;
    i32 coarseFaceCount = 0;
    for (i32 v = 0; v < V; ++v) {
        if (cls[v] == CC_UNSUBDIVIDE_CLASS_ORIGIN) {
            map.originId[v] = coarseVertexCount++;
        } else if (cls[v] == CC_UNSUBDIVIDE_CLASS_FACE) {
            map.faceId[v] = coarseFaceCount++;
        }
    }
    if (coarseVertexCount < 4 || coarseFaceCount < 4) {
        outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
    }

    std::set<i32> edgeVertexInputs;
    for (i32 v = 0; v < V; ++v) {
        if (cls[v] == CC_UNSUBDIVIDE_CLASS_EDGE) {
            edgeVertexInputs.insert(v);
        }
    }

    /* Edge points -> coarse edge endpoints (two distinct coarse vertices). */
    struct EdgeCandidate {
        i32 a;
        i32 b;
        i32 inputVertex;
    };
    std::vector<EdgeCandidate> candidates;
    candidates.reserve(edgeVertexInputs.size());

    for (i32 v : edgeVertexInputs) {
        const i32 start = raw.vHalf[v];
        i32 h = start;
        std::set<i32> originCvs;
        std::set<i32> faceIds;
        do {
            const i32 to = raw.vert[raw.next[h]];
            if (cls[to] == CC_UNSUBDIVIDE_CLASS_ORIGIN) {
                originCvs.insert(map.originId[to]);
            } else if (cls[to] == CC_UNSUBDIVIDE_CLASS_FACE) {
                faceIds.insert(map.faceId[to]);
            }
            h = raw.twin[raw.prev[h]];
        } while (h != start);
        if (originCvs.size() != 2 || faceIds.size() != 2) {
            outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
        const i32 a = *originCvs.begin();
        const i32 b = *originCvs.rbegin();
        candidates.push_back({a, b, v});
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const EdgeCandidate &x, const EdgeCandidate &y) {
                  if (x.a != y.a) {
                      return x.a < y.a;
                  }
                  return x.b < y.b;
              });

    const i32 coarseEdgeCount = (i32)candidates.size();
    std::vector<std::pair<i32, i32>> edgeEnds(coarseEdgeCount);
    for (i32 e = 0; e < coarseEdgeCount; ++e) {
        if (e > 0 && candidates[e].a == candidates[e - 1].a &&
            candidates[e].b == candidates[e - 1].b) {
            /* Duplicate coarse edge: not a simple one-step image. */
            outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
        edgeEnds[e] = {candidates[e].a, candidates[e].b};
        map.edgeId[candidates[e].inputVertex] = e;
    }

    /* Rebuild parent face loops around every face point. */
    CoarseTopo topo;
    topo.V = coarseVertexCount;
    topo.F = coarseFaceCount;
    topo.E = coarseEdgeCount;
    topo.loops.resize(coarseFaceCount);

    std::map<std::pair<i32, i32>, i32> edgeByEnds;
    for (i32 e = 0; e < coarseEdgeCount; ++e) {
        edgeByEnds[{edgeEnds[e].first, edgeEnds[e].second}] = e;
    }

    for (i32 v = 0; v < V; ++v) {
        if (cls[v] != CC_UNSUBDIVIDE_CLASS_FACE) {
            continue;
        }
        const i32 cf = map.faceId[v];

        /* Collect incident child quads (each is one corner of the parent). */
        std::vector<i32> incidentQuads;
        {
            const i32 start = raw.vHalf[v];
            i32 h = start;
            do {
                const i32 q = raw.face[h];
                if (std::find(incidentQuads.begin(), incidentQuads.end(), q) ==
                    incidentQuads.end()) {
                    incidentQuads.push_back(q);
                }
                h = raw.twin[raw.prev[h]];
            } while (h != start);
        }
        if (incidentQuads.size() < 3) {
            outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }

        const i32 q0 = incidentQuads.front();
        i32 hF0 = -1;
        for (i32 j = 0; j < 4; ++j) {
            if (raw.vert[raw.quadH[q0][j]] == v) {
                hF0 = raw.quadH[q0][j];
            }
        }
        if (hF0 < 0) {
            outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }

        std::vector<i32> corners;
        std::vector<i32> betweenEdgePoint;
        i32 hF = hF0;
        bool ok = true;
        do {
            const i32 diagonal = raw.vert[raw.next[raw.next[hF]]];
            if (cls[diagonal] != CC_UNSUBDIVIDE_CLASS_ORIGIN) {
                ok = false;
                break;
            }
            const i32 edgePoint = raw.vert[raw.prev[hF]];
            if (cls[edgePoint] != CC_UNSUBDIVIDE_CLASS_EDGE) {
                ok = false;
                break;
            }
            corners.push_back(map.originId[diagonal]);
            betweenEdgePoint.push_back(edgePoint);
            hF = raw.twin[raw.prev[hF]];
            if (hF < 0) {
                ok = false;
                break;
            }
            if (corners.size() > incidentQuads.size() + 1) {
                ok = false;
                break;
            }
        } while (hF != hF0);

        if (!ok || corners.size() != incidentQuads.size() ||
            corners.size() < 3) {
            outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }

        /* Every corner must be distinct. */
        {
            std::set<i32> uniqueCorners(corners.begin(), corners.end());
            if ((i32)uniqueCorners.size() != (i32)corners.size()) {
                outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            }
        }

        /* Validate the edge point between each consecutive corner pair. */
        const i32 n = (i32)corners.size();
        for (i32 k = 0; k < n; ++k) {
            const i32 u = corners[k];
            const i32 w = corners[(k + 1) % n];
            const i32 lo = std::min(u, w);
            const i32 hi = std::max(u, w);
            const auto it = edgeByEnds.find({lo, hi});
            if (it == edgeByEnds.end()) {
                outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            }
            const i32 e = it->second;
            const i32 ep = betweenEdgePoint[k];
            if (map.edgeId[ep] != e) {
                outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            }
        }

        /* Rotate the loop so that it starts at the smallest coarse vertex. */
        i32 minPos = 0;
        for (i32 k = 1; k < n; ++k) {
            if (corners[k] < corners[minPos]) {
                minPos = k;
            }
        }
        std::rotate(corners.begin(), corners.begin() + minPos, corners.end());
        topo.loops[cf] = corners;
    }

    /* Halfedge layout: grouped by parent face, corners in loop order. */
    topo.loopBase.assign(topo.F + 1, 0);
    for (i32 f = 0; f < topo.F; ++f) {
        if (topo.loops[f].size() < 3) {
            outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
        topo.loopBase[f + 1] = topo.loopBase[f] + (i32)topo.loops[f].size();
    }
    topo.H = topo.loopBase[topo.F];
    if (topo.H != 2 * topo.E) {
        outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
    }

    topo.hVert.assign(topo.H, -1);
    topo.hNext.assign(topo.H, -1);
    topo.hPrev.assign(topo.H, -1);
    topo.hFace.assign(topo.H, -1);
    topo.hEdge.assign(topo.H, -1);
    topo.hTwin.assign(topo.H, -1);
    topo.vHalf.assign(topo.V, -1);
    topo.eHalf.assign(topo.E, -1);
    topo.fHalf.assign(topo.F, -1);
    topo.edgeFaces.assign(topo.E, {{-1, -1}});
    topo.vertFaces.assign(topo.V, {});
    topo.vertEdges.assign(topo.V, {});

    for (i32 f = 0; f < topo.F; ++f) {
        const std::vector<i32> &loop = topo.loops[f];
        const i32 n = (i32)loop.size();
        topo.fHalf[f] = topo.loopBase[f];
        for (i32 k = 0; k < n; ++k) {
            const i32 h = topo.loopBase[f] + k;
            topo.hVert[h] = loop[k];
            topo.hNext[h] = topo.loopBase[f] + (k + 1) % n;
            topo.hPrev[h] = topo.loopBase[f] + (k + n - 1) % n;
            topo.hFace[h] = f;
            topo.vertFaces[loop[k]].push_back(f);
        }
    }

    /* Assign coarse edges to halfedges and validate two-sided usage. */
    {
        std::vector<std::vector<i32>> halfedgesOfEdge(topo.E);
        for (i32 f = 0; f < topo.F; ++f) {
            const std::vector<i32> &loop = topo.loops[f];
            const i32 n = (i32)loop.size();
            for (i32 k = 0; k < n; ++k) {
                const i32 u = loop[k];
                const i32 w = loop[(k + 1) % n];
                const auto it = edgeByEnds.find({std::min(u, w),
                                                 std::max(u, w)});
                if (it == edgeByEnds.end()) {
                    outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                    return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                }
                const i32 e = it->second;
                const i32 h = topo.loopBase[f] + k;
                topo.hEdge[h] = e;
                halfedgesOfEdge[e].push_back(h);
            }
        }

        for (i32 e = 0; e < topo.E; ++e) {
            if (halfedgesOfEdge[e].size() != 2) {
                outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            }
            const i32 h0 = halfedgesOfEdge[e][0];
            const i32 h1 = halfedgesOfEdge[e][1];
            const i32 a0 = topo.hVert[h0];
            const i32 b0 = topo.hVert[topo.hNext[h0]];
            const i32 a1 = topo.hVert[h1];
            const i32 b1 = topo.hVert[topo.hNext[h1]];
            if (!(a0 == b1 && b0 == a1)) {
                outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            }
            if (edgeEnds[e].first != std::min(a0, b0) ||
                edgeEnds[e].second != std::max(a0, b0)) {
                outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            }
            topo.hTwin[h0] = h1;
            topo.hTwin[h1] = h0;
            topo.edgeFaces[e] = {{topo.hFace[h0], topo.hFace[h1]}};
            if (topo.edgeFaces[e][0] == topo.edgeFaces[e][1]) {
                outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            }
            topo.eHalf[e] = std::min(h0, h1);
            topo.vertEdges[a0].push_back(e);
            topo.vertEdges[b0].push_back(e);
        }
    }

    /* Vertex and face indices of the coarse mesh. */
    for (i32 v = 0; v < topo.V; ++v) {
        std::sort(topo.vertFaces[v].begin(), topo.vertFaces[v].end());
        topo.vertFaces[v].erase(
            std::unique(topo.vertFaces[v].begin(), topo.vertFaces[v].end()),
            topo.vertFaces[v].end());
        if (topo.vertFaces[v].empty() || topo.vertEdges[v].empty()) {
            outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
        if (topo.vertFaces[v].size() != topo.vertEdges[v].size()) {
            outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        }
        topo.vHalf[v] = topo.loopBase[topo.vertFaces[v].front()];
        const std::vector<i32> &loop = topo.loops[topo.vertFaces[v].front()];
        for (i32 h = topo.loopBase[topo.vertFaces[v].front()];
             h < topo.loopBase[topo.vertFaces[v].front()] + (i32)loop.size();
             ++h) {
            if (topo.hVert[h] == v) {
                topo.vHalf[v] = h;
                break;
            }
        }
    }

    /* --------------------------------------------------------------------- */
    /* Sparse operator A: one no-crease Gather step on the recovered topology  */
    /* --------------------------------------------------------------------- */

    std::vector<SparseRow> faceRows(topo.F);
    for (i32 f = 0; f < topo.F; ++f) {
        const std::vector<i32> &loop = topo.loops[f];
        const double w = 1.0 / (double)loop.size();
        for (i32 corner : loop) {
            addTerm(faceRows[f], corner, w);
        }
        normalizeRow(faceRows[f]);
    }

    std::vector<SparseRow> rows(raw.V);

    for (i32 i = 0; i < raw.V; ++i) {
        if (cls[i] == CC_UNSUBDIVIDE_CLASS_FACE) {
            rows[i] = faceRows[map.faceId[i]];
            normalizeRow(rows[i]);
        } else if (cls[i] == CC_UNSUBDIVIDE_CLASS_EDGE) {
            const i32 e = map.edgeId[i];
            const i32 a = edgeEnds[e].first;
            const i32 b = edgeEnds[e].second;
            SparseRow row;
            addTerm(row, a, 0.25);
            addTerm(row, b, 0.25);
            const i32 f0 = topo.edgeFaces[e][0];
            const i32 f1 = topo.edgeFaces[e][1];
            for (const auto &term : faceRows[f0].terms) {
                addTerm(row, term.first, 0.25 * term.second);
            }
            for (const auto &term : faceRows[f1].terms) {
                addTerm(row, term.first, 0.25 * term.second);
            }
            normalizeRow(row);
            rows[i] = row;
        } else {
            const i32 v = map.originId[i];
            const i32 n = (i32)topo.vertEdges[v].size();
            if (n < 3 || (i32)topo.vertFaces[v].size() != n) {
                outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
                return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
            }
            const double invN = 1.0 / (double)n;
            const double invN2 = invN * invN;
            SparseRow row;
            for (i32 f : topo.vertFaces[v]) {
                for (const auto &term : faceRows[f].terms) {
                    addTerm(row, term.first, invN2 * term.second);
                }
            }
            for (i32 e : topo.vertEdges[v]) {
                const i32 a = edgeEnds[e].first;
                const i32 b = edgeEnds[e].second;
                addTerm(row, a, invN2);
                addTerm(row, b, invN2);
            }
            addTerm(row, v, 1.0 - 3.0 * invN);
            normalizeRow(row);
            rows[i] = row;
        }
    }

    SolveReport report;
    for (const SparseRow &row : rows) {
        report.nonzeroCoefficients += (i64)row.terms.size();
    }

    /* Y: every input vertex position. */
    std::vector<double> Y((size_t)raw.V * 3);
    for (i32 i = 0; i < raw.V; ++i) {
        Y[i * 3 + 0] = (double)raw.pos[i].x;
        Y[i * 3 + 1] = (double)raw.pos[i].y;
        Y[i * 3 + 2] = (double)raw.pos[i].z;
    }

    /* RHS norm for the documented stopping criterion. */
    std::vector<double> normalRhs((size_t)topo.V * 3, 0.0);
    {
        /* applyAT needs an output sized like the unknown vector. */
        std::vector<double> tmp((size_t)topo.V * 3, 0.0);
        applyAT(rows, Y, (size_t)topo.V, tmp);
        normalRhs.swap(tmp);
    }
    const double rhsInf = infNorm(normalRhs);
    const double threshold = 1e-10 * std::max(1.0, rhsInf);

    std::vector<double> X;
    const bool converged = solveNormalEquations(rows, Y, topo.V, maxIterations,
                                                threshold, X, report);

    outResult->iterations = report.iterations;
    outResult->operatorProducts = report.operatorProducts;
    outResult->nonzeroCoefficients = report.nonzeroCoefficients;

    if (!converged) {
        outResult->status = CC_UNSUBDIVIDE_SOLVER_LIMIT;
        return CC_UNSUBDIVIDE_SOLVER_LIMIT;
    }

    /* --------------------------------------------------------------------- */
    /* Build and verify the candidate cage with the native forward operator.  */
    /* --------------------------------------------------------------------- */

    cc_Mesh *cage = ccm_Create(topo.V, 0, topo.H, topo.E, topo.F);
    if (cage == nullptr) {
        outResult->status = CC_UNSUBDIVIDE_SOLVER_LIMIT;
        return CC_UNSUBDIVIDE_SOLVER_LIMIT;
    }

    for (i32 v = 0; v < topo.V; ++v) {
        cage->vertexPoints[v].x = (float)X[(size_t)v * 3 + 0];
        cage->vertexPoints[v].y = (float)X[(size_t)v * 3 + 1];
        cage->vertexPoints[v].z = (float)X[(size_t)v * 3 + 2];
        cage->vertexToHalfedgeIDs[v] = topo.vHalf[v];
    }
    for (i32 h = 0; h < topo.H; ++h) {
        cc_Halfedge &dst = cage->halfedges[h];
        dst.twinID = topo.hTwin[h];
        dst.nextID = topo.hNext[h];
        dst.prevID = topo.hPrev[h];
        dst.faceID = topo.hFace[h];
        dst.edgeID = topo.hEdge[h];
        dst.vertexID = topo.hVert[h];
        dst.uvID = -1;
    }
    for (i32 e = 0; e < topo.E; ++e) {
        cage->edgeToHalfedgeIDs[e] = topo.eHalf[e];
        cage->creases[e].nextID = e;
        cage->creases[e].prevID = e;
        cage->creases[e].sharpness = 0.0f;
    }
    for (i32 f = 0; f < topo.F; ++f) {
        cage->faceToHalfedgeIDs[f] = topo.fHalf[f];
    }

    cc_Subd *subd = ccs_Create(cage, 1);
    if (subd == nullptr) {
        ccm_Release(cage);
        outResult->status = CC_UNSUBDIVIDE_SOLVER_LIMIT;
        return CC_UNSUBDIVIDE_SOLVER_LIMIT;
    }
    ccs_Refine_NoCreases_Gather(subd);

    const i32 coarseV = topo.V;
    const i32 coarseF = topo.F;

    /* Map every input vertex to the native depth-1 vertex id. */
    std::vector<i32> subdVertexOfInput(raw.V, -1);
    for (i32 i = 0; i < raw.V; ++i) {
        if (cls[i] == CC_UNSUBDIVIDE_CLASS_ORIGIN) {
            subdVertexOfInput[i] = map.originId[i];
        } else if (cls[i] == CC_UNSUBDIVIDE_CLASS_FACE) {
            subdVertexOfInput[i] = coarseV + map.faceId[i];
        } else {
            subdVertexOfInput[i] = coarseV + coarseF + map.edgeId[i];
        }
    }

    double maxForwardResidual = 0.0;
    for (i32 i = 0; i < raw.V; ++i) {
        const cc_VertexPoint p =
            ccs_VertexPoint(subd, subdVertexOfInput[i], 1);
        const double dx = (double)p.x - (double)raw.pos[i].x;
        const double dy = (double)p.y - (double)raw.pos[i].y;
        const double dz = (double)p.z - (double)raw.pos[i].z;
        const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance > maxForwardResidual) {
            maxForwardResidual = distance;
        }
    }
    report.maxForwardResidual = maxForwardResidual;
    outResult->maxForwardResidual = maxForwardResidual;

    /* Derive and check the per-subface parent mapping. */
    std::vector<i32> parentFace(raw.F, -1);
    std::vector<i32> parentCorner(raw.F, -1);
    std::vector<std::vector<int8_t>> cornerUsed(topo.F);
    for (i32 f = 0; f < topo.F; ++f) {
        cornerUsed[f].assign(topo.loops[f].size(), 0);
    }

    bool topologyOk = true;
    for (i32 q = 0; q < raw.F && topologyOk; ++q) {
        const std::array<i32, 4> &quad = raw.quadH[q];
        i32 originInput = -1;
        i32 faceInput = -1;
        i32 edgeInputs[2] = {-1, -1};
        i32 edgeCountLocal = 0;
        for (i32 j = 0; j < 4; ++j) {
            const i32 x = raw.vert[quad[j]];
            if (cls[x] == CC_UNSUBDIVIDE_CLASS_ORIGIN) {
                if (originInput >= 0) {
                    topologyOk = false;
                }
                originInput = x;
            } else if (cls[x] == CC_UNSUBDIVIDE_CLASS_FACE) {
                if (faceInput >= 0) {
                    topologyOk = false;
                }
                faceInput = x;
            } else {
                if (edgeCountLocal >= 2) {
                    topologyOk = false;
                } else {
                    edgeInputs[edgeCountLocal] = x;
                }
                ++edgeCountLocal;
            }
        }
        if (!topologyOk || originInput < 0 || faceInput < 0 ||
            edgeCountLocal != 2) {
            topologyOk = false;
            break;
        }

        const i32 cf = map.faceId[faceInput];
        const i32 cv = map.originId[originInput];
        const std::vector<i32> &loop = topo.loops[cf];
        i32 corner = -1;
        for (i32 k = 0; k < (i32)loop.size(); ++k) {
            if (loop[k] == cv) {
                corner = k;
                break;
            }
        }
        if (corner < 0) {
            topologyOk = false;
            break;
        }
        const i32 n = (i32)loop.size();
        const i32 prevCv = loop[(corner + n - 1) % n];
        const i32 nextCv = loop[(corner + 1) % n];

        /* The two edge points must be exactly the adjacent coarse edges. */
        std::set<i32> expectedEdges;
        expectedEdges.insert(edgeByEnds[{std::min(cv, nextCv),
                                         std::max(cv, nextCv)}]);
        expectedEdges.insert(edgeByEnds[{std::min(prevCv, cv),
                                         std::max(prevCv, cv)}]);
        std::set<i32> actualEdges;
        actualEdges.insert(map.edgeId[edgeInputs[0]]);
        actualEdges.insert(map.edgeId[edgeInputs[1]]);
        if (expectedEdges != actualEdges) {
            topologyOk = false;
            break;
        }

        /* Native child quad: face id equals the cage halfedge id. */
        const i32 child = topo.loopBase[cf] + corner;
        const i32 edgeNext = topo.hEdge[topo.loopBase[cf] + corner];
        const i32 edgePrev =
            topo.hEdge[topo.loopBase[cf] + (corner + n - 1) % n];
        const i32 expected[4] = {
            cv,
            coarseV + coarseF + edgeNext,
            coarseV + cf,
            coarseV + coarseF + edgePrev,
        };
        std::set<i32> actualSet;
        std::set<i32> expectedSet;
        for (i32 j = 0; j < 4; ++j) {
            actualSet.insert(ccs_HalfedgeVertexID(subd, 4 * child + j, 1));
            expectedSet.insert(expected[j]);
        }
        if (actualSet != expectedSet) {
            topologyOk = false;
            break;
        }

        /* The input face must map bijectively onto (cf, corner). */
        if (parentFace[q] >= 0) {
            topologyOk = false;
            break;
        }
        parentFace[q] = cf;
        parentCorner[q] = corner;
        if (cornerUsed[cf][corner] != 0) {
            topologyOk = false;
            break;
        }
        cornerUsed[cf][corner] = 1;
    }

    if (topologyOk) {
        for (i32 f = 0; f < topo.F && topologyOk; ++f) {
            for (int8_t used : cornerUsed[f]) {
                if (used == 0) {
                    topologyOk = false;
                }
            }
        }
    }

    ccs_Release(subd);

    if (!topologyOk) {
        ccm_Release(cage);
        outResult->status = CC_UNSUBDIVIDE_NOT_SUBDIVISION;
        return CC_UNSUBDIVIDE_NOT_SUBDIVISION;
    }

    if (maxForwardResidual > tolerance) {
        ccm_Release(cage);
        outResult->status = CC_UNSUBDIVIDE_RESIDUAL_EXCEEDED;
        return CC_UNSUBDIVIDE_RESIDUAL_EXCEEDED;
    }

    /* --------------------------------------------------------------------- */
    /* Publish the result.                                                    */
    /* --------------------------------------------------------------------- */

    int32_t *vertexClass = (int32_t *)std::malloc(sizeof(int32_t) * V);
    int32_t *vertexParent = (int32_t *)std::malloc(sizeof(int32_t) * V);
    int32_t *parentFaceOfSubface =
        (int32_t *)std::malloc(sizeof(int32_t) * raw.F);
    int32_t *subfaceCornerOfSubface =
        (int32_t *)std::malloc(sizeof(int32_t) * raw.F);
    if (!vertexClass || !vertexParent || !parentFaceOfSubface ||
        !subfaceCornerOfSubface) {
        std::free(vertexClass);
        std::free(vertexParent);
        std::free(parentFaceOfSubface);
        std::free(subfaceCornerOfSubface);
        ccm_Release(cage);
        outResult->status = CC_UNSUBDIVIDE_SOLVER_LIMIT;
        return CC_UNSUBDIVIDE_SOLVER_LIMIT;
    }

    for (i32 i = 0; i < V; ++i) {
        vertexClass[i] = cls[i];
        if (cls[i] == CC_UNSUBDIVIDE_CLASS_ORIGIN) {
            vertexParent[i] = map.originId[i];
        } else if (cls[i] == CC_UNSUBDIVIDE_CLASS_FACE) {
            vertexParent[i] = map.faceId[i];
        } else {
            vertexParent[i] = map.edgeId[i];
        }
    }
    for (i32 q = 0; q < raw.F; ++q) {
        parentFaceOfSubface[q] = parentFace[q];
        subfaceCornerOfSubface[q] = parentCorner[q];
    }

    outResult->status = CC_UNSUBDIVIDE_OK;
    outResult->cage = cage;
    outResult->inputVertexCount = raw.V;
    outResult->inputFaceCount = raw.F;
    outResult->inputHalfedgeCount = raw.H;
    outResult->coarseVertexCount = topo.V;
    outResult->coarseFaceCount = topo.F;
    outResult->coarseEdgeCount = topo.E;
    outResult->coarseHalfedgeCount = topo.H;
    outResult->vertexClass = vertexClass;
    outResult->vertexParent = vertexParent;
    outResult->parentFaceOfSubface = parentFaceOfSubface;
    outResult->subfaceCornerOfSubface = subfaceCornerOfSubface;
    outResult->iterations = report.iterations;
    outResult->operatorProducts = report.operatorProducts;
    outResult->nonzeroCoefficients = report.nonzeroCoefficients;
    outResult->maxForwardResidual = maxForwardResidual;

    return CC_UNSUBDIVIDE_OK;
}

/* ========================================================================= */
/* Lifetime helpers                                                          */
/* ========================================================================= */

extern "C" void ccu_ReleaseResult(cc_UnsubdivideResult *result) {
    if (result == nullptr) {
        return;
    }
    if (result->cage != nullptr) {
        ccm_Release(result->cage);
        result->cage = nullptr;
    }
    std::free(result->vertexClass);
    std::free(result->vertexParent);
    std::free(result->parentFaceOfSubface);
    std::free(result->subfaceCornerOfSubface);
    result->vertexClass = nullptr;
    result->vertexParent = nullptr;
    result->parentFaceOfSubface = nullptr;
    result->subfaceCornerOfSubface = nullptr;
}

extern "C" const char *ccu_StatusName(cc_UnsubdivideStatus status) {
    switch (status) {
        case CC_UNSUBDIVIDE_OK: return "ok";
        case CC_UNSUBDIVIDE_INVALID_INPUT: return "invalid_input";
        case CC_UNSUBDIVIDE_NOT_SUBDIVISION: return "not_subdivision";
        case CC_UNSUBDIVIDE_SOLVER_LIMIT: return "solver_limit";
        case CC_UNSUBDIVIDE_RESIDUAL_EXCEEDED: return "residual_exceeded";
    }
    return "unknown";
}
