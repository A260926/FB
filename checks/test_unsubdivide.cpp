/*
 * checks/test_unsubdivide.cpp
 *
 * TASK_CHECK cases for cc_Unsubdivide, split into three groups:
 *   topology  -- parent-face winding, numbering, mapping bijections
 *   inverse   -- geometry: coarse positions recovered by the LS solve
 *   rejection -- malformed / non-subdivision inputs must not fake success
 *
 * No main(): the shared task_support/driver.c provides it.
 */

#include "CatmullClark.h"
#include "cc_Unsubdivide.h"
#include "task_support/check.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <utility>
#include <vector>

namespace {

/* ------------------------------------------------------------------------- */
/* Polygonal mesh construction                                               */
/* ------------------------------------------------------------------------- */

struct PolyMesh {
    std::vector<cc_VertexPoint> pos;
    std::vector<std::vector<int>> faces;
};

cc_VertexPoint vp(float x, float y, float z) {
    cc_VertexPoint p;
    p.x = x;
    p.y = y;
    p.z = z;
    return p;
}

/*
 * Build a closed, oriented, quad/ngon cc_Mesh from a PolyMesh.
 * The input PolyMesh must be an oriented closed 2-manifold.  Halfedges are
 * assigned in face order; edges by canonical endpoint pair; all creases are
 * zero and there are no UVs.
 */
cc_Mesh *buildMesh(const PolyMesh &pm) {
    const int V = (int)pm.pos.size();
    const int F = (int)pm.faces.size();
    int H = 0;
    for (const auto &f : pm.faces) {
        H += (int)f.size();
    }

    /* Canonical edge ids from sorted endpoint pairs. */
    std::map<std::pair<int, int>, int> edgeMap;
    int E = 0;
    std::vector<int> faceStart(F, 0);
    {
        int cursor = 0;
        for (int f = 0; f < F; ++f) {
            faceStart[f] = cursor;
            const int n = (int)pm.faces[f].size();
            for (int k = 0; k < n; ++k) {
                int a = pm.faces[f][k];
                int b = pm.faces[f][(k + 1) % n];
                std::pair<int, int> key(std::min(a, b), std::max(a, b));
                if (!edgeMap.count(key)) {
                    edgeMap[key] = E++;
                }
            }
            cursor += n;
        }
    }

    cc_Mesh *m = ccm_Create(V, 0, H, E, F);
    if (!m) {
        return nullptr;
    }

    for (int v = 0; v < V; ++v) {
        m->vertexPoints[v] = pm.pos[v];
        m->vertexToHalfedgeIDs[v] = -1;
    }
    for (int f = 0; f < F; ++f) {
        const int n = (int)pm.faces[f].size();
        m->faceToHalfedgeIDs[f] = faceStart[f];
        for (int k = 0; k < n; ++k) {
            const int h = faceStart[f] + k;
            const int a = pm.faces[f][k];
            const int b = pm.faces[f][(k + 1) % n];
            cc_Halfedge &x = m->halfedges[h];
            x.vertexID = a;
            x.faceID = f;
            x.nextID = faceStart[f] + (k + 1) % n;
            x.prevID = faceStart[f] + (k + n - 1) % n;
            x.edgeID = edgeMap[std::pair<int, int>(std::min(a, b),
                                                   std::max(a, b))];
            x.twinID = -1;
            x.uvID = -1;
            if (m->vertexToHalfedgeIDs[a] < 0) {
                m->vertexToHalfedgeIDs[a] = h;
            }
        }
    }

    /* Twins: match the opposite directed edge. */
    std::map<std::pair<int, int>, int> directed;
    for (int h = 0; h < H; ++h) {
        directed[std::pair<int, int>(m->halfedges[h].vertexID,
                                     m->halfedges[m->halfedges[h].nextID]
                                         .vertexID)] = h;
    }
    for (int h = 0; h < H; ++h) {
        const int a = m->halfedges[h].vertexID;
        const int b = m->halfedges[m->halfedges[h].nextID].vertexID;
        auto it = directed.find(std::pair<int, int>(b, a));
        if (it == directed.end()) {
            ccm_Release(m);
            return nullptr;
        }
        m->halfedges[h].twinID = it->second;
    }

    for (int e = 0; e < E; ++e) {
        m->edgeToHalfedgeIDs[e] = -1;
    }
    for (int h = 0; h < H; ++h) {
        const int e = m->halfedges[h].edgeID;
        if (m->edgeToHalfedgeIDs[e] < 0) {
            m->edgeToHalfedgeIDs[e] = h;
        }
    }
    for (int e = 0; e < E; ++e) {
        m->creases[e].nextID = e;
        m->creases[e].prevID = e;
        m->creases[e].sharpness = 0.0f;
    }

    return m;
}

/* Extract the depth-1 quad mesh produced by the native Gather operator. */
cc_Mesh *refineOnce(const cc_Mesh *cage) {
    const int V1 = ccm_VertexCountAtDepth(cage, 1);
    const int E1 = ccm_EdgeCountAtDepth(cage, 1);
    const int F1 = ccm_FaceCountAtDepth(cage, 1);
    const int H1 = ccm_HalfedgeCountAtDepth(cage, 1);

    cc_Subd *subd = ccs_Create(cage, 1);
    if (!subd) {
        return nullptr;
    }
    ccs_Refine_NoCreases_Gather(subd);

    cc_Mesh *out = ccm_Create(V1, 0, H1, E1, F1);
    if (!out) {
        ccs_Release(subd);
        return nullptr;
    }
    for (int v = 0; v < V1; ++v) {
        out->vertexPoints[v] = ccs_VertexPoint(subd, v, 1);
        out->vertexToHalfedgeIDs[v] = -1;
    }
    for (int f = 0; f < F1; ++f) {
        out->faceToHalfedgeIDs[f] = 4 * f;
        for (int k = 0; k < 4; ++k) {
            const int h = 4 * f + k;
            cc_Halfedge &x = out->halfedges[h];
            x.vertexID = ccs_HalfedgeVertexID(subd, h, 1);
            x.faceID = f;
            x.nextID = 4 * f + (k + 1) % 4;
            x.prevID = 4 * f + (k + 3) % 4;
            x.edgeID = ccs_HalfedgeEdgeID(subd, h, 1);
            x.twinID = ccs_HalfedgeTwinID(subd, h, 1);
            x.uvID = -1;
            if (out->vertexToHalfedgeIDs[x.vertexID] < 0) {
                out->vertexToHalfedgeIDs[x.vertexID] = h;
            }
        }
    }
    for (int e = 0; e < E1; ++e) {
        out->edgeToHalfedgeIDs[e] = -1;
    }
    for (int h = 0; h < H1; ++h) {
        const int e = out->halfedges[h].edgeID;
        if (out->edgeToHalfedgeIDs[e] < 0) {
            out->edgeToHalfedgeIDs[e] = h;
        }
    }
    for (int e = 0; e < E1; ++e) {
        out->creases[e].nextID = e;
        out->creases[e].prevID = e;
        out->creases[e].sharpness = 0.0f;
    }

    ccs_Release(subd);
    return out;
}

/*
 * Independently renumber an existing mesh: arbitrary permutations of vertex,
 * edge, halfedge and face ids.  The face-to-halfedge mapping is carried along
 * unchanged, which also randomises the first corner seen by naive walkers.
 */
cc_Mesh *shuffleMesh(const cc_Mesh *in, unsigned seed) {
    const int V = in->vertexCount;
    const int E = in->edgeCount;
    const int H = in->halfedgeCount;
    const int F = in->faceCount;

    std::vector<int> vp(V), ep(E), hp(H), fp(F);
    for (int i = 0; i < V; ++i) vp[i] = i;
    for (int i = 0; i < E; ++i) ep[i] = i;
    for (int i = 0; i < H; ++i) hp[i] = i;
    for (int i = 0; i < F; ++i) fp[i] = i;
    std::mt19937 rng(seed);
    std::shuffle(vp.begin(), vp.end(), rng);
    std::shuffle(ep.begin(), ep.end(), rng);
    std::shuffle(hp.begin(), hp.end(), rng);
    std::shuffle(fp.begin(), fp.end(), rng);

    cc_Mesh *out = ccm_Create(V, 0, H, E, F);
    if (!out) {
        return nullptr;
    }
    for (int v = 0; v < V; ++v) {
        out->vertexPoints[vp[v]] = in->vertexPoints[v];
        out->vertexToHalfedgeIDs[vp[v]] = hp[in->vertexToHalfedgeIDs[v]];
    }
    for (int e = 0; e < E; ++e) {
        out->edgeToHalfedgeIDs[ep[e]] = hp[in->edgeToHalfedgeIDs[e]];
    }
    for (int f = 0; f < F; ++f) {
        out->faceToHalfedgeIDs[fp[f]] = hp[in->faceToHalfedgeIDs[f]];
    }
    for (int h = 0; h < H; ++h) {
        const cc_Halfedge &s = in->halfedges[h];
        cc_Halfedge &d = out->halfedges[hp[h]];
        d.vertexID = vp[s.vertexID];
        d.faceID = fp[s.faceID];
        d.edgeID = ep[s.edgeID];
        d.twinID = hp[s.twinID];
        d.nextID = hp[s.nextID];
        d.prevID = hp[s.prevID];
        d.uvID = -1;
    }
    for (int e = 0; e < E; ++e) {
        int src = -1;
        for (int i = 0; i < E; ++i) {
            if (ep[i] == e) {
                src = i;
                break;
            }
        }
        out->creases[e].nextID = e;
        out->creases[e].prevID = e;
        out->creases[e].sharpness = 0.0f;
        (void)src;
    }
    return out;
}

/* Read a face loop (coarse vertex ids) starting from faceToHalfedge. */
std::vector<int> faceLoop(const cc_Mesh *m, int face) {
    std::vector<int> loop;
    const int start = m->faceToHalfedgeIDs[face];
    int h = start;
    do {
        loop.push_back(m->halfedges[h].vertexID);
        h = m->halfedges[h].nextID;
    } while (h != start && (int)loop.size() <= m->halfedgeCount);
    return loop;
}

/* ------------------------------------------------------------------------- */
/* Reference shapes                                                          */
/* ------------------------------------------------------------------------- */

PolyMesh shapeTetra() {
    PolyMesh pm;
    pm.pos = {vp(0, 0, 0), vp(2, 0, 0), vp(0, 3, 0), vp(0, 0, 4)};
    pm.faces = {{0, 2, 1}, {0, 1, 3}, {1, 2, 3}, {2, 0, 3}};
    return pm;
}

PolyMesh shapePrism() {
    const int n = 5;
    PolyMesh pm;
    for (int i = 0; i < n; ++i) {
        const float a = 6.2831853f * (float)i / (float)n;
        pm.pos.push_back(vp(std::cos(a) * 2.0f + 0.13f * (float)i,
                            std::sin(a) * 2.0f,
                            0.07f * (float)i));
    }
    for (int i = 0; i < n; ++i) {
        const float a = 6.2831853f * (float)i / (float)n;
        pm.pos.push_back(vp(std::cos(a) * 1.3f,
                            std::sin(a) * 1.3f,
                            3.0f + 0.11f * (float)i));
    }
    std::vector<int> bottom, top;
    for (int i = 0; i < n; ++i) {
        bottom.push_back(n - 1 - i);
        top.push_back(n + i);
    }
    pm.faces.push_back(bottom);
    pm.faces.push_back(top);
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        pm.faces.push_back({i, j, n + j, n + i});
    }
    return pm;
}

PolyMesh shapeTorus(int N) {
    PolyMesh pm;
    const float R = 3.0f;
    const float r = 1.0f;
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            const float u = 6.2831853f * (float)i / (float)N;
            const float v = 6.2831853f * (float)j / (float)N;
            pm.pos.push_back(vp((R + r * std::cos(v)) * std::cos(u),
                                (R + r * std::cos(v)) * std::sin(u),
                                r * std::sin(v) +
                                    0.03f * std::sin(3.0f * u)));
        }
    }
    auto id = [&](int i, int j) { return (i % N) * N + (j % N); };
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            pm.faces.push_back(
                {id(i, j), id(i + 1, j), id(i + 1, j + 1), id(i, j + 1)});
        }
    }
    return pm;
}

/* ------------------------------------------------------------------------- */
/* Shared fixture: shape -> coarse mesh -> depth-1 input (+ shuffle)          */
/* ------------------------------------------------------------------------- */

struct Fixture {
    PolyMesh poly;
    cc_Mesh *cage = nullptr;
    cc_Mesh *input = nullptr;
    std::vector<int> newOfNative; /* native depth-1 id -> shuffled id */
    int marker = -1;              /* shuffled id of native vertex 0 */
};

/* vertex permutation used by shuffleMesh must be reproducible; expose it. */
std::vector<int> shuffleVertexPerm(unsigned seed, int count) {
    std::vector<int> vp(count);
    for (int i = 0; i < count; ++i) vp[i] = i;
    /* consume the same RNG calls as shuffleMesh: vertex, edge, H, face */
    std::mt19937 rng(seed);
    std::shuffle(vp.begin(), vp.end(), rng);
    return vp;
}

bool makeFixture(const PolyMesh &poly, unsigned shuffleSeed, bool doShuffle,
                 Fixture &fx) {
    fx.poly = poly;
    fx.cage = buildMesh(poly);
    if (!fx.cage) {
        return false;
    }
    fx.input = refineOnce(fx.cage);
    if (!fx.input) {
        return false;
    }
    if (!doShuffle) {
        fx.newOfNative.resize(fx.input->vertexCount);
        for (int i = 0; i < fx.input->vertexCount; ++i) {
            fx.newOfNative[i] = i;
        }
        fx.marker = 0;
        return true;
    }
    cc_Mesh *shuffled = shuffleMesh(fx.input, shuffleSeed);
    if (!shuffled) {
        return false;
    }
    const std::vector<int> vp =
        shuffleVertexPerm(shuffleSeed, fx.input->vertexCount);
    fx.newOfNative.assign(fx.input->vertexCount, 0);
    for (int i = 0; i < fx.input->vertexCount; ++i) {
        fx.newOfNative[i] = vp[i];
    }
    ccm_Release(fx.input);
    fx.input = shuffled;
    fx.marker = fx.newOfNative[0];
    return true;
}

void releaseFixture(Fixture &fx) {
    if (fx.input) ccm_Release(fx.input);
    if (fx.cage) ccm_Release(fx.cage);
    fx.input = nullptr;
    fx.cage = nullptr;
}

/* Expected recovered numbering, derived only from the known coarse cage. */
struct Expectation {
    std::vector<int> coarseOfNativeVertex; /* native coarse vertex -> id */
    std::vector<int> coarseOfNativeFace;   /* native coarse face -> id */
};

Expectation expectationFor(const Fixture &fx) {
    const int V0 = fx.cage->vertexCount;
    const int F0 = fx.cage->faceCount;
    Expectation ex;
    ex.coarseOfNativeVertex.assign(V0, -1);
    ex.coarseOfNativeFace.assign(F0, -1);

    std::vector<int> orderV(V0);
    for (int i = 0; i < V0; ++i) orderV[i] = i;
    std::sort(orderV.begin(), orderV.end(), [&](int a, int b) {
        return fx.newOfNative[a] < fx.newOfNative[b];
    });
    for (int r = 0; r < V0; ++r) {
        ex.coarseOfNativeVertex[orderV[r]] = r;
    }

    std::vector<int> orderF(F0);
    for (int i = 0; i < F0; ++i) orderF[i] = i;
    std::sort(orderF.begin(), orderF.end(), [&](int a, int b) {
        return fx.newOfNative[V0 + a] < fx.newOfNative[V0 + b];
    });
    for (int r = 0; r < F0; ++r) {
        ex.coarseOfNativeFace[orderF[r]] = r;
    }
    return ex;
}

/* Reference coarse loops in recovered numbering, rotated to min first. */
std::vector<std::vector<int>> expectedCoarseLoops(const Fixture &fx) {
    const Expectation ex = expectationFor(fx);
    std::vector<std::vector<int>> loops(fx.poly.faces.size());
    for (size_t f = 0; f < fx.poly.faces.size(); ++f) {
        std::vector<int> loop;
        for (int v : fx.poly.faces[f]) {
            loop.push_back(ex.coarseOfNativeVertex[v]);
        }
        int minPos = 0;
        for (int k = 1; k < (int)loop.size(); ++k) {
            if (loop[k] < loop[minPos]) minPos = k;
        }
        std::rotate(loop.begin(), loop.begin() + minPos, loop.end());
        loops[f] = loop;
    }
    return loops;
}

/* Verify full result invariants against the known coarse cage. */
int checkRecoveredTopology(const Fixture &fx,
                           const cc_UnsubdivideResult &res) {
    const int V0 = fx.cage->vertexCount;
    const int F0 = fx.cage->faceCount;
    const int E0 = fx.cage->edgeCount;
    const int H0 = fx.cage->halfedgeCount;
    const int inputV = fx.input->vertexCount;
    const int inputF = fx.input->faceCount;

    TASK_CHECK(res.status == CC_UNSUBDIVIDE_OK);
    TASK_CHECK(res.cage != nullptr);
    if (!res.cage) return 0;

    TASK_CHECK(res.coarseVertexCount == V0);
    TASK_CHECK(res.coarseFaceCount == F0);
    TASK_CHECK(res.coarseEdgeCount == E0);
    TASK_CHECK(res.coarseHalfedgeCount == H0);
    TASK_CHECK(res.cage->vertexCount == V0);
    TASK_CHECK(res.cage->faceCount == F0);
    TASK_CHECK(res.cage->edgeCount == E0);
    TASK_CHECK(res.cage->halfedgeCount == H0);
    TASK_CHECK(res.cage->uvCount == 0);

    /* Class counts. */
    int nOrigin = 0, nEdge = 0, nFace = 0;
    for (int i = 0; i < inputV; ++i) {
        switch (res.vertexClass[i]) {
            case CC_UNSUBDIVIDE_CLASS_ORIGIN: ++nOrigin; break;
            case CC_UNSUBDIVIDE_CLASS_EDGE: ++nEdge; break;
            case CC_UNSUBDIVIDE_CLASS_FACE: ++nFace; break;
            default: TASK_CHECK(false); break;
        }
    }
    TASK_CHECK(nOrigin == V0);
    TASK_CHECK(nEdge == E0);
    TASK_CHECK(nFace == F0);

    const Expectation ex = expectationFor(fx);

    /*
     * Native depth-1 vertex layout is [orig V0][face F0][edge E0]; after the
     * shuffle, native id x is at newOfNative[x].  Cross-check the class and
     * parent id of every input vertex using only that layout.
     */
    for (int i = 0; i < V0; ++i) {
        const int shuffled = fx.newOfNative[i];
        TASK_CHECK(res.vertexClass[shuffled] == CC_UNSUBDIVIDE_CLASS_ORIGIN);
        TASK_CHECK(res.vertexParent[shuffled] == ex.coarseOfNativeVertex[i]);
    }
    for (int f = 0; f < F0; ++f) {
        const int shuffled = fx.newOfNative[V0 + f];
        TASK_CHECK(res.vertexClass[shuffled] == CC_UNSUBDIVIDE_CLASS_FACE);
        TASK_CHECK(res.vertexParent[shuffled] == ex.coarseOfNativeFace[f]);
    }
    for (int e = 0; e < E0; ++e) {
        const int shuffled = fx.newOfNative[V0 + F0 + e];
        TASK_CHECK(res.vertexClass[shuffled] == CC_UNSUBDIVIDE_CLASS_EDGE);
        /* Edge numbering is lexicographic by recovered coarse endpoints;
         * validated structurally below, not by this native order. */
        TASK_CHECK(res.vertexParent[shuffled] >= 0);
        TASK_CHECK(res.vertexParent[shuffled] < E0);
    }

    /* Recovered coarse loops, compared to the reference loops. */
    const std::vector<std::vector<int>> expected = expectedCoarseLoops(fx);
    for (int f = 0; f < F0; ++f) {
        const int rf = ex.coarseOfNativeFace[f];
        const std::vector<int> loop = faceLoop(res.cage, rf);
        TASK_CHECK(!loop.empty());
        TASK_CHECK(loop == expected[f]);
    }

    /* Coarse halfedge invariants: next/prev/twin/loop structure. */
    for (int h = 0; h < H0; ++h) {
        TASK_CHECK(res.cage->halfedges[h].nextID >= 0);
        TASK_CHECK(res.cage->halfedges[h].prevID >= 0);
        TASK_CHECK(res.cage->halfedges[h].twinID >= 0);
        TASK_CHECK(res.cage->halfedges[res.cage->halfedges[h].twinID].twinID ==
                   h);
        TASK_CHECK(res.cage->halfedges[res.cage->halfedges[h].nextID].prevID ==
                   h);
        TASK_CHECK(res.cage->halfedges[res.cage->halfedges[h].prevID].nextID ==
                   h);
        TASK_CHECK(res.cage->halfedges[h].faceID >= 0);
        TASK_CHECK(res.cage->halfedges[h].faceID < F0);
        TASK_CHECK(res.cage->halfedges[h].edgeID >= 0);
        TASK_CHECK(res.cage->halfedges[h].edgeID < E0);
    }

    /* Edge numbering: lexicographic on ordered endpoint pairs. */
    {
        std::vector<std::pair<int, int>> keys(E0);
        for (int e = 0; e < E0; ++e) {
            const int h = res.cage->edgeToHalfedgeIDs[e];
            TASK_CHECK(res.cage->halfedges[h].edgeID == e);
            const int a = res.cage->halfedges[h].vertexID;
            const int b = res.cage->halfedges[res.cage->halfedges[h].nextID]
                              .vertexID;
            keys[e] = std::make_pair(std::min(a, b), std::max(a, b));
            const int t = res.cage->halfedges[h].twinID;
            TASK_CHECK(res.cage->halfedges[t].edgeID == e);
        }
        std::vector<std::pair<int, int>> sorted = keys;
        std::sort(sorted.begin(), sorted.end());
        TASK_CHECK(keys == sorted);
        for (int e = 0; e < E0; ++e) {
            if (e > 0) {
                TASK_CHECK(keys[e] != keys[e - 1]);
            }
        }
    }

    /* Face mapping bijection: 4 child quads per parent, each corner once. */
    {
        std::vector<int> perFace(F0, 0);
        std::set<std::pair<int, int>> pairs;
        for (int q = 0; q < inputF; ++q) {
            const int pf = res.parentFaceOfSubface[q];
            const int pc = res.subfaceCornerOfSubface[q];
            TASK_CHECK(pf >= 0 && pf < F0);
            TASK_CHECK(pc >= 0);
            TASK_CHECK(pairs.insert(std::make_pair(pf, pc)).second);
            ++perFace[pf];
        }
        for (int f = 0; f < F0; ++f) {
            TASK_CHECK(perFace[ex.coarseOfNativeFace[f]] ==
                       (int)expected[f].size());
        }
        /* Every corner of every loop is occupied exactly once. */
        for (int f = 0; f < F0; ++f) {
            const int n = (int)faceLoop(res.cage, f).size();
            for (int k = 0; k < n; ++k) {
                TASK_CHECK(pairs.count(std::make_pair(f, k)) == 1);
            }
        }
    }

    /*
     * For each input child quad, its original corner must be the corner of
     * the parent loop recorded in subfaceCornerOfSubface, and its face
     * corner must belong to parentFaceOfSubface.
     */
    for (int q = 0; q < inputF; ++q) {
        const int start = res.cage ? fx.input->faceToHalfedgeIDs[q] : -1;
        int h = start;
        int originVertex = -1;
        int faceVertex = -1;
        int edgeVertexCount = 0;
        do {
            const int v = fx.input->halfedges[h].vertexID;
            if (res.vertexClass[v] == CC_UNSUBDIVIDE_CLASS_ORIGIN) {
                TASK_CHECK(originVertex < 0);
                originVertex = v;
            } else if (res.vertexClass[v] == CC_UNSUBDIVIDE_CLASS_FACE) {
                TASK_CHECK(faceVertex < 0);
                faceVertex = v;
            } else {
                ++edgeVertexCount;
            }
            h = fx.input->halfedges[h].nextID;
        } while (h != start);
        TASK_CHECK(originVertex >= 0);
        TASK_CHECK(faceVertex >= 0);
        TASK_CHECK(edgeVertexCount == 2);

        const int pf = res.parentFaceOfSubface[q];
        TASK_CHECK(res.vertexParent[faceVertex] == pf);

        const std::vector<int> loop = faceLoop(res.cage, pf);
        const int corner = res.subfaceCornerOfSubface[q];
        TASK_CHECK(corner < (int)loop.size());
        TASK_CHECK(loop[corner] == res.vertexParent[originVertex]);
        (void)nOrigin;
        (void)nEdge;
        (void)nFace;
    }

    return 0;
}

/* Basic validity of a fixture, so failures are attributed correctly. */
int checkFixture(const Fixture &fx) {
    TASK_CHECK(fx.cage != nullptr);
    TASK_CHECK(fx.input != nullptr);
    TASK_CHECK(fx.marker >= 0 && fx.marker < fx.input->vertexCount);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Cases: topology                                                           */
/* ------------------------------------------------------------------------- */

int case_topology_tetra() {
    Fixture fx;
    TASK_CHECK(makeFixture(shapeTetra(), 0, false, fx));
    if (checkFixture(fx)) { releaseFixture(fx); return 1; }
    cc_UnsubdivideResult res;
    const cc_UnsubdivideStatus st =
        ccu_Unsubdivide(fx.input, fx.marker, 5e-5, 512, &res);
    TASK_CHECK(st == CC_UNSUBDIVIDE_OK);
    checkRecoveredTopology(fx, res);
    ccu_ReleaseResult(&res);
    releaseFixture(fx);
    return 0;
}

int case_topology_prism() {
    Fixture fx;
    TASK_CHECK(makeFixture(shapePrism(), 0, false, fx));
    if (checkFixture(fx)) { releaseFixture(fx); return 1; }
    cc_UnsubdivideResult res;
    const cc_UnsubdivideStatus st =
        ccu_Unsubdivide(fx.input, fx.marker, 5e-5, 512, &res);
    TASK_CHECK(st == CC_UNSUBDIVIDE_OK);
    checkRecoveredTopology(fx, res);
    /* The pentagonal prism really has pentagonal and quad parent faces,
     * i.e. non-quad parent faces are recovered. */
    {
        int sawPent = 0, sawQuad = 0;
        for (int f = 0; f < res.coarseFaceCount; ++f) {
            const int n = (int)faceLoop(res.cage, f).size();
            if (n == 5) ++sawPent;
            if (n == 4) ++sawQuad;
        }
        TASK_CHECK(sawPent == 2);
        TASK_CHECK(sawQuad == 5);
    }
    ccu_ReleaseResult(&res);
    releaseFixture(fx);
    return 0;
}

int case_topology_torus() {
    Fixture fx;
    TASK_CHECK(makeFixture(shapeTorus(32), 0, false, fx));
    if (checkFixture(fx)) { releaseFixture(fx); return 1; }
    cc_UnsubdivideResult res;
    const cc_UnsubdivideStatus st =
        ccu_Unsubdivide(fx.input, fx.marker, 5e-5, 512, &res);
    TASK_CHECK(st == CC_UNSUBDIVIDE_OK);
    checkRecoveredTopology(fx, res);
    ccu_ReleaseResult(&res);
    releaseFixture(fx);
    return 0;
}

int case_topology_shuffled() {
    const unsigned seeds[] = {1u, 7u, 12345u};
    for (unsigned seed : seeds) {
        Fixture fx;
        TASK_CHECK(makeFixture(shapePrism(), seed, true, fx));
        if (checkFixture(fx)) { releaseFixture(fx); return 1; }
        cc_UnsubdivideResult res;
        const cc_UnsubdivideStatus st =
            ccu_Unsubdivide(fx.input, fx.marker, 5e-5, 512, &res);
        TASK_CHECK(st == CC_UNSUBDIVIDE_OK);
        checkRecoveredTopology(fx, res);
        ccu_ReleaseResult(&res);
        releaseFixture(fx);
    }
    return 0;
}

int case_topology_shuffled_torus() {
    Fixture fx;
    TASK_CHECK(makeFixture(shapeTorus(8), 999u, true, fx));
    if (checkFixture(fx)) { releaseFixture(fx); return 1; }
    cc_UnsubdivideResult res;
    const cc_UnsubdivideStatus st =
        ccu_Unsubdivide(fx.input, fx.marker, 5e-5, 512, &res);
    TASK_CHECK(st == CC_UNSUBDIVIDE_OK);
    checkRecoveredTopology(fx, res);
    ccu_ReleaseResult(&res);
    releaseFixture(fx);
    return 0;
}

/* Halfedge ordering documented contract: grouped by face, corner order. */
int case_topology_halfedge_order() {
    Fixture fx;
    TASK_CHECK(makeFixture(shapePrism(), 0, false, fx));
    if (checkFixture(fx)) { releaseFixture(fx); return 1; }
    cc_UnsubdivideResult res;
    TASK_CHECK(ccu_Unsubdivide(fx.input, fx.marker, 5e-5, 512, &res) ==
               CC_UNSUBDIVIDE_OK);
    int offset = 0;
    for (int f = 0; f < res.coarseFaceCount; ++f) {
        TASK_CHECK(res.cage->faceToHalfedgeIDs[f] == offset);
        const int n = (int)faceLoop(res.cage, f).size();
        const std::vector<int> loop = faceLoop(res.cage, f);
        TASK_CHECK(loop[0] ==
                   *std::min_element(loop.begin(), loop.end()));
        for (int k = 0; k < n; ++k) {
            const int h = offset + k;
            TASK_CHECK(res.cage->halfedges[h].faceID == f);
            TASK_CHECK(res.cage->halfedges[h].vertexID == loop[k]);
        }
        offset += n;
    }
    TASK_CHECK(offset == res.coarseHalfedgeCount);
    ccu_ReleaseResult(&res);
    releaseFixture(fx);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Cases: inverse                                                            */
/* ------------------------------------------------------------------------- */

double maxCoarsePointError(const Fixture &fx,
                           const cc_UnsubdivideResult &res) {
    const int V0 = fx.cage->vertexCount;
    const Expectation ex = expectationFor(fx);
    double worst = 0.0;
    for (int v = 0; v < V0; ++v) {
        const int id = ex.coarseOfNativeVertex[v];
        const cc_VertexPoint got = res.cage->vertexPoints[id];
        const cc_VertexPoint want = fx.poly.pos[v];
        const double dx = (double)got.x - (double)want.x;
        const double dy = (double)got.y - (double)want.y;
        const double dz = (double)got.z - (double)want.z;
        worst = std::max(worst, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    return worst;
}

int inverseCase(const PolyMesh &poly, unsigned seed, bool doShuffle) {
    Fixture fx;
    TASK_CHECK(makeFixture(poly, seed, doShuffle, fx));
    if (checkFixture(fx)) { releaseFixture(fx); return 1; }
    cc_UnsubdivideResult res;
    const cc_UnsubdivideStatus st =
        ccu_Unsubdivide(fx.input, fx.marker, 5e-5, 512, &res);
    TASK_CHECK(st == CC_UNSUBDIVIDE_OK);
    TASK_CHECK(res.status == CC_UNSUBDIVIDE_OK);
    TASK_CHECK(res.cage != nullptr);
    if (res.status == CC_UNSUBDIVIDE_OK) {
        TASK_CHECK(res.maxForwardResidual <= 5e-5);
        TASK_CHECK(maxCoarsePointError(fx, res) <= 5e-5);
        TASK_CHECK(res.iterations > 0);
        TASK_CHECK(res.iterations <= 512);
        TASK_CHECK(res.operatorProducts > 0);
        TASK_CHECK(res.nonzeroCoefficients > 0);
        /* Sparse: no V^2 dense operator. */
        TASK_CHECK(res.nonzeroCoefficients <
                   (int64_t)64 * res.coarseVertexCount);
    }
    ccu_ReleaseResult(&res);
    releaseFixture(fx);
    return 0;
}

int case_inverse_tetra() {
    return inverseCase(shapeTetra(), 0, false);
}

int case_inverse_prism() {
    return inverseCase(shapePrism(), 0, false);
}

int case_inverse_torus32() {
    return inverseCase(shapeTorus(32), 0, false);
}

int case_inverse_shuffled_prism() {
    return inverseCase(shapePrism(), 4242u, true);
}

int case_inverse_shuffled_torus() {
    return inverseCase(shapeTorus(8), 31337u, true);
}

/* Two consecutive different inputs; results stay valid after input release. */
int case_inverse_consecutive_and_release() {
    Fixture fx1, fx2;
    TASK_CHECK(makeFixture(shapeTetra(), 0, false, fx1));
    TASK_CHECK(makeFixture(shapePrism(), 77u, true, fx2));
    if (checkFixture(fx1) || checkFixture(fx2)) {
        releaseFixture(fx1);
        releaseFixture(fx2);
        return 1;
    }

    cc_UnsubdivideResult r1, r2;
    TASK_CHECK(ccu_Unsubdivide(fx1.input, fx1.marker, 5e-5, 512, &r1) ==
               CC_UNSUBDIVIDE_OK);
    TASK_CHECK(ccu_Unsubdivide(fx2.input, fx2.marker, 5e-5, 512, &r2) ==
               CC_UNSUBDIVIDE_OK);
    TASK_CHECK(r1.cage != nullptr && r1.cage != r2.cage);

    /* Capture expected points, then release both sources. */
    const int V1 = fx1.cage->vertexCount;
    const int V2 = fx2.cage->vertexCount;
    const Expectation e1 = expectationFor(fx1);
    const Expectation e2 = expectationFor(fx2);

    ccm_Release(fx1.input);
    fx1.input = nullptr;
    ccm_Release(fx2.input);
    fx2.input = nullptr;

    double worst = 0.0;
    for (int v = 0; v < V1; ++v) {
        const cc_VertexPoint got = r1.cage->vertexPoints[e1.coarseOfNativeVertex[v]];
        const cc_VertexPoint want = fx1.poly.pos[v];
        worst = std::max<double>(worst, std::fabs(got.x - want.x) +
                                            std::fabs(got.y - want.y) +
                                            std::fabs(got.z - want.z));
    }
    for (int v = 0; v < V2; ++v) {
        const cc_VertexPoint got = r2.cage->vertexPoints[e2.coarseOfNativeVertex[v]];
        const cc_VertexPoint want = fx2.poly.pos[v];
        worst = std::max<double>(worst, std::fabs(got.x - want.x) +
                                            std::fabs(got.y - want.y) +
                                            std::fabs(got.z - want.z));
    }
    TASK_CHECK(worst <= 5e-5);

    /* The owned cages can be refined by the native library. */
    cc_Subd *s1 = ccs_Create(r1.cage, 1);
    TASK_CHECK(s1 != nullptr);
    if (s1) {
        ccs_Refine_NoCreases_Gather(s1);
        TASK_CHECK(ccm_VertexCountAtDepth(r1.cage, 1) == r1.inputVertexCount);
        ccs_Release(s1);
    }

    ccu_ReleaseResult(&r1);
    ccu_ReleaseResult(&r2);
    releaseFixture(fx1);
    releaseFixture(fx2);
    return 0;
}

/* Report fields are self-consistent with the documented stop criterion. */
int case_inverse_report() {
    Fixture fx;
    TASK_CHECK(makeFixture(shapeTorus(16), 5u, true, fx));
    if (checkFixture(fx)) { releaseFixture(fx); return 1; }
    cc_UnsubdivideResult res;
    TASK_CHECK(ccu_Unsubdivide(fx.input, fx.marker, 5e-5, 512, &res) ==
               CC_UNSUBDIVIDE_OK);
    TASK_CHECK(res.iterations >= 0 && res.iterations <= 512);
    TASK_CHECK(res.operatorProducts >= 2 * res.iterations);
    TASK_CHECK(res.nonzeroCoefficients > 0);
    TASK_CHECK(res.maxForwardResidual >= 0.0);
    TASK_CHECK(res.maxForwardResidual <= res.tolerance);
    TASK_CHECK(res.tolerance == 5e-5);
    TASK_CHECK(res.inputVertexCount == fx.input->vertexCount);
    TASK_CHECK(res.inputFaceCount == fx.input->faceCount);
    TASK_CHECK(res.inputHalfedgeCount == fx.input->halfedgeCount);
    ccu_ReleaseResult(&res);
    releaseFixture(fx);
    return 0;
}

cc_UnsubdivideResult runOn(cc_Mesh *mesh, int marker, double tol,
                           int iterations, cc_UnsubdivideStatus *st);

/*
 * A vertex of an "original/face" fan carries only E neighbours; a vertex on
 * the edge side carries two P and two F neighbours.  Exercise a genuine
 * subdivision image where the requested marker sits on an edge point of a
 * regular-mesh side and must not be silently skipped.
 */
int case_reject_marker_on_edge_side() {
    /* Regular prism image: many edge points, any of them is a bad marker. */
    Fixture fx;
    TASK_CHECK(makeFixture(shapePrism(), 0, false, fx));
    if (checkFixture(fx)) { releaseFixture(fx); return 1; }
    const int V0 = fx.cage->vertexCount;
    const int F0 = fx.cage->faceCount;

    /* Every native edge point lies on the edge side. */
    for (int k = 0; k < 3; ++k) {
        const int nativeEdge = V0 + F0 + k;
        cc_UnsubdivideStatus st;
        cc_UnsubdivideResult res = runOn(fx.input, nativeEdge, 5e-5, 512, &st);
        TASK_CHECK(st == CC_UNSUBDIVIDE_INVALID_INPUT);
        TASK_CHECK(res.cage == nullptr);
        TASK_CHECK(res.iterations == 0);
        ccu_ReleaseResult(&res);
    }
    releaseFixture(fx);
    return 0;
}

/* Point positions that would make the input non-finite are rejected. */
int case_reject_nonfinite_position() {
    Fixture fx;
    TASK_CHECK(makeFixture(shapeTetra(), 0, false, fx));
    if (checkFixture(fx)) { releaseFixture(fx); return 1; }
    const cc_VertexPoint saved = fx.input->vertexPoints[3];
    const float bad[] = {std::numeric_limits<float>::infinity(),
                         std::numeric_limits<float>::quiet_NaN(), 1e6f, -1e6f};
    for (float value : bad) {
        fx.input->vertexPoints[3].z = value;
        cc_UnsubdivideStatus st;
        cc_UnsubdivideResult res = runOn(fx.input, fx.marker, 5e-5, 512, &st);
        TASK_CHECK(st == CC_UNSUBDIVIDE_INVALID_INPUT);
        TASK_CHECK(res.cage == nullptr);
        ccu_ReleaseResult(&res);
    }
    fx.input->vertexPoints[3] = saved;
    /* Restored input still recovers. */
    cc_UnsubdivideResult res;
    TASK_CHECK(ccu_Unsubdivide(fx.input, fx.marker, 5e-5, 512, &res) ==
               CC_UNSUBDIVIDE_OK);
    ccu_ReleaseResult(&res);
    releaseFixture(fx);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Cases: rejection                                                          */
/* ------------------------------------------------------------------------- */

cc_UnsubdivideResult runOn(cc_Mesh *mesh, int marker, double tol,
                           int iterations, cc_UnsubdivideStatus *st) {
    cc_UnsubdivideResult res;
    *st = ccu_Unsubdivide(mesh, marker, tol, iterations, &res);
    return res;
}

int case_reject_bad_twin() {
    Fixture fx;
    TASK_CHECK(makeFixture(shapeTetra(), 0, false, fx));
    if (checkFixture(fx)) { releaseFixture(fx); return 1; }
    /* Self-twin on one halfedge. */
    fx.input->halfedges[3].twinID = 3;
    cc_UnsubdivideStatus st;
    cc_UnsubdivideResult res = runOn(fx.input, fx.marker, 5e-5, 512, &st);
    TASK_CHECK(st == CC_UNSUBDIVIDE_INVALID_INPUT);
    TASK_CHECK(res.cage == nullptr);
    ccu_ReleaseResult(&res);
    releaseFixture(fx);

    /* One-directional twin mismatch. */
    Fixture fy;
    TASK_CHECK(makeFixture(shapeTetra(), 0, false, fy));
    if (checkFixture(fy)) { releaseFixture(fy); return 1; }
    const int t = fy.input->halfedges[7].twinID;
    fy.input->halfedges[t].twinID = fy.input->halfedges[t].nextID;
    res = runOn(fy.input, fy.marker, 5e-5, 512, &st);
    TASK_CHECK(st == CC_UNSUBDIVIDE_INVALID_INPUT);
    TASK_CHECK(res.cage == nullptr);
    ccu_ReleaseResult(&res);
    releaseFixture(fy);
    return 0;
}

/*
 * Two closed quad cubes sharing exactly one vertex: every local array is
 * consistent, but the shared vertex has two disjoint fans.  Topological
 * validation must reject it as not a subdivision image.
 */
int case_reject_disconnected_fan() {
    PolyMesh cube;
    cube.pos = {
        vp(0, 0, 0), vp(1, 0, 0), vp(1, 1, 0), vp(0, 1, 0),
        vp(0, 0, 1), vp(1, 0, 1), vp(1, 1, 1), vp(0, 1, 1),
    };
    cube.faces = {
        {0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
        {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7},
    };

    /* Two copies, second one translated and its vertex 0 merged with a
     * vertex of the first. */
    PolyMesh pinched = cube;
    const int cubeVerts = (int)cube.pos.size();
    const int merged = 0;
    for (int i = 1; i < cubeVerts; ++i) {
        pinched.pos.push_back(vp(cube.pos[i].x + 4.0f,
                                 cube.pos[i].y,
                                 cube.pos[i].z));
    }
    /* vertex 0 of second cube is merged: it maps to `merged`. */
    for (auto &face : cube.faces) {
        std::vector<int> shifted;
        for (int v : face) {
            shifted.push_back(v == 0 ? merged : v - 1 + cubeVerts);
        }
        pinched.faces.push_back(shifted);
    }

    cc_Mesh *m = buildMesh(pinched);
    TASK_CHECK(m != nullptr);
    if (!m) return 0;

    /* Sanity: arrays are locally consistent but the fan splits. */
    cc_UnsubdivideStatus st;
    cc_UnsubdivideResult res = runOn(m, 0, 5e-5, 512, &st);
    TASK_CHECK(st == CC_UNSUBDIVIDE_NOT_SUBDIVISION);
    TASK_CHECK(res.cage == nullptr);
    TASK_CHECK(res.iterations == 0);
    ccu_ReleaseResult(&res);
    ccm_Release(m);
    return 0;
}

/* All-quad closed cube: no side of the checkerboard is 4-regular. */
int case_reject_all_skip_classes() {
    /*
     * "all-skip" in the sense of a candidate inverse where every input vertex
     * is treated as an edge/face point: here a plain closed quad cube whose
     * vertices are all simply-connected but whose checkerboard sides are not
     * a valid P/F pair, so no class assignment exists.  The call must reject,
     * not silently succeed with an empty or fabricated class map.
     */
    PolyMesh cube;
    cube.pos = {
        vp(0, 0, 0), vp(1, 0, 0), vp(1, 1, 0), vp(0, 1, 0),
        vp(0, 0, 1), vp(1, 0, 1), vp(1, 1, 1), vp(0, 1, 1),
    };
    cube.faces = {
        {0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
        {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7},
    };
    cc_Mesh *m = buildMesh(cube);
    TASK_CHECK(m != nullptr);
    if (!m) return 0;
    cc_UnsubdivideStatus st;
    cc_UnsubdivideResult res = runOn(m, 0, 5e-5, 512, &st);
    TASK_CHECK(st == CC_UNSUBDIVIDE_NOT_SUBDIVISION);
    TASK_CHECK(res.cage == nullptr);
    ccu_ReleaseResult(&res);
    ccm_Release(m);
    return 0;
}

/* Marker on an edge point (native id V0 + F0) must be invalid_input. */
int case_reject_bad_marker() {
    Fixture fx;
    TASK_CHECK(makeFixture(shapeTetra(), 0, false, fx));
    if (checkFixture(fx)) { releaseFixture(fx); return 1; }
    const int V0 = fx.cage->vertexCount;
    const int F0 = fx.cage->faceCount;
    const int edgePoint = V0 + F0; /* first native edge point */

    cc_UnsubdivideStatus st;
    cc_UnsubdivideResult res = runOn(fx.input, edgePoint, 5e-5, 512, &st);
    TASK_CHECK(st == CC_UNSUBDIVIDE_INVALID_INPUT);
    TASK_CHECK(res.cage == nullptr);
    ccu_ReleaseResult(&res);

    res = runOn(fx.input, -1, 5e-5, 512, &st);
    TASK_CHECK(st == CC_UNSUBDIVIDE_INVALID_INPUT);
    TASK_CHECK(res.cage == nullptr);
    ccu_ReleaseResult(&res);

    res = runOn(fx.input, fx.input->vertexCount, 5e-5, 512, &st);
    TASK_CHECK(st == CC_UNSUBDIVIDE_INVALID_INPUT);
    TASK_CHECK(res.cage == nullptr);
    ccu_ReleaseResult(&res);
    releaseFixture(fx);
    return 0;
}

/* Illegal array / argument values are rejected before any traversal. */
int case_reject_illegal_arrays_and_args() {
    Fixture fx;
    TASK_CHECK(makeFixture(shapeTetra(), 0, false, fx));
    if (checkFixture(fx)) { releaseFixture(fx); return 1; }

    cc_UnsubdivideStatus st;
    cc_UnsubdivideResult res;

    /* Out-of-range faceToHalfedge. */
    const int saved = fx.input->faceToHalfedgeIDs[0];
    fx.input->faceToHalfedgeIDs[0] = fx.input->halfedgeCount;
    res = runOn(fx.input, fx.marker, 5e-5, 512, &st);
    TASK_CHECK(st == CC_UNSUBDIVIDE_INVALID_INPUT);
    TASK_CHECK(res.cage == nullptr);
    ccu_ReleaseResult(&res);
    fx.input->faceToHalfedgeIDs[0] = saved;

    /* Out-of-range next. */
    const int savedNext = fx.input->halfedges[0].nextID;
    fx.input->halfedges[0].nextID = -5;
    res = runOn(fx.input, fx.marker, 5e-5, 512, &st);
    TASK_CHECK(st == CC_UNSUBDIVIDE_INVALID_INPUT);
    TASK_CHECK(res.cage == nullptr);
    ccu_ReleaseResult(&res);
    fx.input->halfedges[0].nextID = savedNext;

    /* Non-finite / out-of-range position. */
    const cc_VertexPoint savedPoint = fx.input->vertexPoints[0];
    fx.input->vertexPoints[0].x = std::numeric_limits<float>::quiet_NaN();
    res = runOn(fx.input, fx.marker, 5e-5, 512, &st);
    TASK_CHECK(st == CC_UNSUBDIVIDE_INVALID_INPUT);
    TASK_CHECK(res.cage == nullptr);
    ccu_ReleaseResult(&res);
    fx.input->vertexPoints[0] = savedPoint;

    fx.input->vertexPoints[0].y = 25.0f;
    res = runOn(fx.input, fx.marker, 5e-5, 512, &st);
    TASK_CHECK(st == CC_UNSUBDIVIDE_INVALID_INPUT);
    TASK_CHECK(res.cage == nullptr);
    ccu_ReleaseResult(&res);
    fx.input->vertexPoints[0] = savedPoint;

    /* Active crease. */
    const cc_Crease savedCrease = fx.input->creases[0];
    fx.input->creases[0].sharpness = 1.5f;
    res = runOn(fx.input, fx.marker, 5e-5, 512, &st);
    TASK_CHECK(st == CC_UNSUBDIVIDE_INVALID_INPUT);
    TASK_CHECK(res.cage == nullptr);
    ccu_ReleaseResult(&res);
    fx.input->creases[0] = savedCrease;

    /* Bad arguments. */
    res = runOn(nullptr, 0, 5e-5, 512, &st);
    TASK_CHECK(st == CC_UNSUBDIVIDE_INVALID_INPUT);
    ccu_ReleaseResult(&res);

    res = runOn(fx.input, fx.marker, -1.0, 512, &st);
    TASK_CHECK(st == CC_UNSUBDIVIDE_INVALID_INPUT);
    ccu_ReleaseResult(&res);

    res = runOn(fx.input, fx.marker,
                std::numeric_limits<double>::infinity(), 512, &st);
    TASK_CHECK(st == CC_UNSUBDIVIDE_INVALID_INPUT);
    ccu_ReleaseResult(&res);

    res = runOn(fx.input, fx.marker, 5e-5, 0, &st);
    TASK_CHECK(st == CC_UNSUBDIVIDE_INVALID_INPUT);
    ccu_ReleaseResult(&res);

    res = runOn(fx.input, fx.marker, 5e-5, -3, &st);
    TASK_CHECK(st == CC_UNSUBDIVIDE_INVALID_INPUT);
    ccu_ReleaseResult(&res);

    /* Null result slot. */
    TASK_CHECK(ccu_Unsubdivide(fx.input, fx.marker, 5e-5, 512, nullptr) ==
               CC_UNSUBDIVIDE_INVALID_INPUT);

    /* Input was not modified by any failed attempt. */
    TASK_CHECK(fx.input->faceToHalfedgeIDs[0] == saved);
    TASK_CHECK(fx.input->halfedges[0].nextID == savedNext);
    TASK_CHECK(std::memcmp(&fx.input->vertexPoints[0], &savedPoint,
                           sizeof(savedPoint)) == 0);

    releaseFixture(fx);
    return 0;
}

/* Non-quad input is outside the supported input. */
int case_reject_non_quad() {
    /* The coarse tetrahedron itself is a triangle mesh. */
    PolyMesh tetra = shapeTetra();
    cc_Mesh *m = buildMesh(tetra);
    TASK_CHECK(m != nullptr);
    if (!m) return 0;
    cc_UnsubdivideStatus st;
    cc_UnsubdivideResult res = runOn(m, 0, 5e-5, 512, &st);
    TASK_CHECK(st == CC_UNSUBDIVIDE_INVALID_INPUT ||
               st == CC_UNSUBDIVIDE_NOT_SUBDIVISION);
    TASK_CHECK(res.cage == nullptr);
    ccu_ReleaseResult(&res);
    ccm_Release(m);
    return 0;
}

/* Iteration cap reached before the residual test passes. */
int case_reject_solver_limit() {
    Fixture fx;
    TASK_CHECK(makeFixture(shapeTorus(32), 0, false, fx));
    if (checkFixture(fx)) { releaseFixture(fx); return 1; }
    cc_UnsubdivideStatus st;
    cc_UnsubdivideResult res = runOn(fx.input, fx.marker, 5e-5, 1, &st);
    TASK_CHECK(st == CC_UNSUBDIVIDE_SOLVER_LIMIT);
    TASK_CHECK(res.cage == nullptr);
    TASK_CHECK(res.iterations >= 1);
    TASK_CHECK(res.iterations <= 1);
    ccu_ReleaseResult(&res);

    /* As is a small multi-iteration budget the regular torus cannot finish
     * in.  Derive a budget strictly below the observed iteration count. */
    cc_UnsubdivideResult full;
    cc_UnsubdivideStatus fullStatus =
        ccu_Unsubdivide(fx.input, fx.marker, 5e-5, 512, &full);
    TASK_CHECK(fullStatus == CC_UNSUBDIVIDE_OK);
    TASK_CHECK(full.iterations > 3);
    const int32_t tight = (int32_t)(full.iterations - 1);
    ccu_ReleaseResult(&full);

    res = runOn(fx.input, fx.marker, 5e-5, tight, &st);
    TASK_CHECK(st == CC_UNSUBDIVIDE_SOLVER_LIMIT);
    TASK_CHECK(res.cage == nullptr);
    TASK_CHECK(res.iterations <= (int64_t)tight);
    ccu_ReleaseResult(&res);
    releaseFixture(fx);
    return 0;
}

/*
 * Legal topology, one face point displaced outside the one-step image space:
 * the solve converges but the forward residual must reject it.
 */
int case_reject_forward_residual() {
    Fixture fx;
    TASK_CHECK(makeFixture(shapePrism(), 0, false, fx));
    if (checkFixture(fx)) { releaseFixture(fx); return 1; }
    const int V0 = fx.cage->vertexCount;
    /* Native face point 0 is at depth-1 vertex id V0. */
    fx.input->vertexPoints[V0].x += 0.5f;

    cc_UnsubdivideStatus st;
    cc_UnsubdivideResult res = runOn(fx.input, fx.marker, 5e-5, 512, &st);
    TASK_CHECK(st == CC_UNSUBDIVIDE_RESIDUAL_EXCEEDED);
    TASK_CHECK(res.cage == nullptr);
    TASK_CHECK(res.maxForwardResidual > 5e-5);
    ccu_ReleaseResult(&res);
    releaseFixture(fx);
    return 0;
}

/*
 * An all-edge/face-only quad mesh where the marker sits on a vertex whose
 * checkerboard side cannot be classified: two edge-point fans.  Built as a
 * "double cover" style mesh; the validator must not loop or fake success.
 */
int case_reject_degenerate_all_skip() {
    /* A single quad strip closed into a torus with only 2 quads per row is
     * still a valid subdivision image; instead use a mesh of two disjoint
     * components, which the single-connectedness check rejects. */
    PolyMesh a = shapeTetra();
    PolyMesh combined = a;
    const int offset = (int)a.pos.size();
    for (const auto &p : a.pos) {
        combined.pos.push_back(vp(p.x + 20.0f, p.y, p.z));
    }
    for (const auto &f : a.faces) {
        std::vector<int> shifted;
        for (int v : f) shifted.push_back(v + offset);
        combined.faces.push_back(shifted);
    }
    cc_Mesh *m = buildMesh(combined);
    TASK_CHECK(m != nullptr);
    if (!m) return 0;
    /* This is a triangle mesh, so it is rejected on face degree before the
     * connectivity check; use it only to exercise the guard. */
    cc_UnsubdivideStatus st;
    cc_UnsubdivideResult res = runOn(m, 0, 5e-5, 512, &st);
    TASK_CHECK(st == CC_UNSUBDIVIDE_INVALID_INPUT ||
               st == CC_UNSUBDIVIDE_NOT_SUBDIVISION);
    TASK_CHECK(res.cage == nullptr);
    TASK_CHECK(res.iterations == 0);
    ccu_ReleaseResult(&res);
    ccm_Release(m);
    return 0;
}

/* A structurally valid quad mesh, two components: must be not_subdivision. */
int case_reject_disconnected_quad() {
    PolyMesh cube;
    cube.pos = {
        vp(0, 0, 0), vp(1, 0, 0), vp(1, 1, 0), vp(0, 1, 0),
        vp(0, 0, 1), vp(1, 0, 1), vp(1, 1, 1), vp(0, 1, 1),
    };
    cube.faces = {
        {0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
        {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7},
    };
    PolyMesh combined = cube;
    const int offset = (int)cube.pos.size();
    for (const auto &p : cube.pos) {
        /* Stay inside the documented [-10, 10] coordinate range. */
        combined.pos.push_back(vp(p.x + 4.0f, p.y, p.z));
    }
    for (const auto &f : cube.faces) {
        std::vector<int> shifted;
        for (int v : f) shifted.push_back(v + offset);
        combined.faces.push_back(shifted);
    }
    cc_Mesh *m = buildMesh(combined);
    TASK_CHECK(m != nullptr);
    if (!m) return 0;
    cc_UnsubdivideStatus st;
    cc_UnsubdivideResult res = runOn(m, 0, 5e-5, 512, &st);
    TASK_CHECK(st == CC_UNSUBDIVIDE_NOT_SUBDIVISION);
    TASK_CHECK(res.cage == nullptr);
    ccu_ReleaseResult(&res);
    ccm_Release(m);
    return 0;
}

/* Result lifetime: releasing NULL and a failed result is safe. */
int case_reject_release_safety() {
    ccu_ReleaseResult(nullptr);
    cc_UnsubdivideResult res;
    std::memset(&res, 0, sizeof(res));
    res.status = CC_UNSUBDIVIDE_INVALID_INPUT;
    ccu_ReleaseResult(&res);
    ccu_ReleaseResult(&res);
    TASK_CHECK(res.cage == nullptr);
    return 0;
}

} /* namespace */

/* ------------------------------------------------------------------------- */
/* Registration                                                              */
/* ------------------------------------------------------------------------- */

const struct task_case task_cases[] = {
    {"topology", "tetra", case_topology_tetra},
    {"topology", "prism_nongon_faces", case_topology_prism},
    {"topology", "torus32", case_topology_torus},
    {"topology", "shuffled_ids_prism", case_topology_shuffled},
    {"topology", "shuffled_ids_torus", case_topology_shuffled_torus},
    {"topology", "halfedge_order", case_topology_halfedge_order},

    {"inverse", "tetra", case_inverse_tetra},
    {"inverse", "prism", case_inverse_prism},
    {"inverse", "torus32", case_inverse_torus32},
    {"inverse", "shuffled_prism", case_inverse_shuffled_prism},
    {"inverse", "shuffled_torus", case_inverse_shuffled_torus},
    {"inverse", "consecutive_and_source_release",
     case_inverse_consecutive_and_release},
    {"inverse", "report_fields", case_inverse_report},

    {"rejection", "bad_twin", case_reject_bad_twin},
    {"rejection", "disconnected_vertex_fan", case_reject_disconnected_fan},
    {"rejection", "all_skip_classes", case_reject_all_skip_classes},
    {"rejection", "bad_marker", case_reject_bad_marker},
    {"rejection", "marker_on_edge_side", case_reject_marker_on_edge_side},
    {"rejection", "nonfinite_position", case_reject_nonfinite_position},
    {"rejection", "illegal_arrays_and_args",
     case_reject_illegal_arrays_and_args},
    {"rejection", "non_quad_input", case_reject_non_quad},
    {"rejection", "solver_limit", case_reject_solver_limit},
    {"rejection", "forward_residual", case_reject_forward_residual},
    {"rejection", "degenerate_all_skip", case_reject_degenerate_all_skip},
    {"rejection", "disconnected_quad", case_reject_disconnected_quad},
    {"rejection", "release_safety", case_reject_release_safety},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
