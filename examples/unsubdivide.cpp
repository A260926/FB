/*
 * examples/unsubdivide.cpp
 *
 * Build an asymmetric pentagonal prism, subdivide it once with the native
 * Catmull-Clark Gather operator, randomly renumber every entity, recover the
 * coarse cage through cc_Unsubdivide, round-trip through ccm_Save/ccm_Load,
 * subdivide the loaded cage again, and verify the parent/child mapping and
 * point residuals.  Exits non-zero on any self-check failure.
 *
 * Usage: unsubdivide <output.ccm>
 */

#include "CatmullClark.h"
#include "cc_Unsubdivide.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <utility>
#include <vector>

namespace {

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

int fail(const char *what) {
    std::fprintf(stderr, "unsubdivide example: %s\n", what);
    return 1;
}

/* Build an oriented closed manifold cc_Mesh from a PolyMesh. */
cc_Mesh *buildMesh(const PolyMesh &pm) {
    const int V = (int)pm.pos.size();
    const int F = (int)pm.faces.size();
    int H = 0;
    for (const auto &f : pm.faces) {
        H += (int)f.size();
    }

    std::map<std::pair<int, int>, int> edgeMap;
    int E = 0;
    std::vector<int> faceStart(F, 0);
    int cursor = 0;
    for (int f = 0; f < F; ++f) {
        faceStart[f] = cursor;
        const int n = (int)pm.faces[f].size();
        for (int k = 0; k < n; ++k) {
            const int a = pm.faces[f][k];
            const int b = pm.faces[f][(k + 1) % n];
            const std::pair<int, int> key(std::min(a, b), std::max(a, b));
            if (!edgeMap.count(key)) {
                edgeMap[key] = E++;
            }
        }
        cursor += n;
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
    std::map<std::pair<int, int>, int> directed;
    for (int h = 0; h < H; ++h) {
        directed[std::pair<int, int>(
            m->halfedges[h].vertexID,
            m->halfedges[m->halfedges[h].nextID].vertexID)] = h;
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

/* One native Gather step, exported as a standalone quad cc_Mesh. */
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
 * Renumber every entity of `in`.  Returns the new mesh and fills
 * `newOfNativeVertex` so callers can locate the image of native vertex v.
 */
cc_Mesh *shuffleMesh(const cc_Mesh *in, unsigned seed,
                     std::vector<int> &newOfNativeVertex) {
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
    newOfNativeVertex = vp;

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
        out->creases[e].nextID = e;
        out->creases[e].prevID = e;
        out->creases[e].sharpness = 0.0f;
    }
    return out;
}

/* Asymmetric pentagonal prism. */
PolyMesh shapePentagonalPrism() {
    const int n = 5;
    PolyMesh pm;
    for (int i = 0; i < n; ++i) {
        const float a = 6.2831853f * (float)i / (float)n;
        const float radius = 2.0f + 0.17f * (float)i;
        pm.pos.push_back(vp(radius * std::cos(a),
                            radius * std::sin(a),
                            0.09f * (float)i));
    }
    for (int i = 0; i < n; ++i) {
        const float a = 6.2831853f * (float)i / (float)n + 0.13f;
        const float radius = 1.25f + 0.11f * (float)i;
        pm.pos.push_back(vp(radius * std::cos(a),
                            radius * std::sin(a),
                            3.0f + 0.07f * (float)i));
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

} /* namespace */

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <output.ccm>\n", argv[0]);
        return 2;
    }
    const char *outputPath = argv[1];

    /* 1. Coarse prism and its one-step subdivision image. */
    PolyMesh poly = shapePentagonalPrism();
    cc_Mesh *cage = buildMesh(poly);
    if (!cage) {
        return fail("could not build the coarse prism");
    }
    cc_Mesh *input = refineOnce(cage);
    if (!input) {
        ccm_Release(cage);
        return fail("could not refine the prism");
    }

    /* 2. Renumber everything and locate a known original vertex point.
     * Native depth-1 vertex 0 is the first coarse vertex's successor. */
    std::vector<int> newOfNativeVertex;
    cc_Mesh *shuffled = shuffleMesh(input, 20261129u, newOfNativeVertex);
    ccm_Release(input);
    if (!shuffled) {
        ccm_Release(cage);
        return fail("could not renumber the input");
    }
    const int marker = newOfNativeVertex[0];

    /* 3. Recover the coarse cage. */
    cc_UnsubdivideResult res;
    const cc_UnsubdivideStatus status =
        ccu_Unsubdivide(shuffled, marker, 5e-5, 512, &res);
    if (status != CC_UNSUBDIVIDE_OK) {
        std::fprintf(stderr, "unsubdivide example: recovery failed: %s\n",
                     ccu_StatusName(status));
        ccm_Release(shuffled);
        ccm_Release(cage);
        return 1;
    }
    std::printf("recovered cage: V=%d F=%d E=%d H=%d "
                "iterations=%lld forward_residual=%.3e\n",
                res.coarseVertexCount, res.coarseFaceCount,
                res.coarseEdgeCount, res.coarseHalfedgeCount,
                (long long)res.iterations, res.maxForwardResidual);

    /* 4. The recovered coarse faces must match the known coarse winding. */
    {
        const int V0 = cage->vertexCount;
        /* Recovered vertex ids follow ascending shuffled input index. */
        std::vector<int> order(V0);
        for (int i = 0; i < V0; ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return newOfNativeVertex[a] < newOfNativeVertex[b];
        });
        std::vector<int> coarseOfNative(V0, -1);
        for (int r = 0; r < V0; ++r) coarseOfNative[order[r]] = r;

        for (size_t f = 0; f < poly.faces.size(); ++f) {
            std::vector<int> want;
            for (int v : poly.faces[f]) want.push_back(coarseOfNative[v]);
            int minPos = 0;
            for (int k = 1; k < (int)want.size(); ++k) {
                if (want[k] < want[minPos]) minPos = k;
            }
            std::rotate(want.begin(), want.begin() + minPos, want.end());

            /* Recovered face id follows ascending shuffled face point index,
             * i.e. native face id V0 + f. */
            std::vector<int> orderF(poly.faces.size());
            for (size_t i = 0; i < poly.faces.size(); ++i) orderF[i] = (int)i;
            std::sort(orderF.begin(), orderF.end(), [&](int a, int b) {
                return newOfNativeVertex[V0 + a] < newOfNativeVertex[V0 + b];
            });
            int rf = -1;
            for (size_t r = 0; r < orderF.size(); ++r) {
                if (orderF[r] == (int)f) rf = (int)r;
            }
            std::vector<int> got;
            const int start = res.cage->faceToHalfedgeIDs[rf];
            int h = start;
            do {
                got.push_back(res.cage->halfedges[h].vertexID);
                h = res.cage->halfedges[h].nextID;
            } while (h != start);
            if (got != want) {
                ccu_ReleaseResult(&res);
                ccm_Release(shuffled);
                ccm_Release(cage);
                return fail("recovered parent-face winding mismatch");
            }
        }
    }

    /* 5. Save, load, and verify the loaded cage refines to the input. */
    if (!ccm_Save(res.cage, outputPath)) {
        ccu_ReleaseResult(&res);
        ccm_Release(shuffled);
        ccm_Release(cage);
        return fail("ccm_Save failed");
    }
    cc_Mesh *loaded = ccm_Load(outputPath);
    if (!loaded) {
        ccu_ReleaseResult(&res);
        ccm_Release(shuffled);
        ccm_Release(cage);
        return fail("ccm_Load failed");
    }
    if (loaded->vertexCount != res.coarseVertexCount ||
        loaded->faceCount != res.coarseFaceCount ||
        loaded->edgeCount != res.coarseEdgeCount ||
        loaded->halfedgeCount != res.coarseHalfedgeCount) {
        ccm_Release(loaded);
        ccu_ReleaseResult(&res);
        ccm_Release(shuffled);
        ccm_Release(cage);
        return fail("ccm round-trip changed the cage counts");
    }
    {
        std::vector<int> counts(loaded->faceCount, 0);
        for (int f = 0; f < loaded->faceCount; ++f) {
            const int start = loaded->faceToHalfedgeIDs[f];
            int h = start;
            do {
                ++counts[f];
                h = loaded->halfedges[h].nextID;
            } while (h != start);
        }
        /* Sum of parent degrees must equal the halfedge count. */
        long total = 0;
        for (int c : counts) total += c;
        if (total != loaded->halfedgeCount) {
            ccm_Release(loaded);
            ccu_ReleaseResult(&res);
            ccm_Release(shuffled);
            ccm_Release(cage);
            return fail("loaded cage has inconsistent face loops");
        }
    }

    /* 6. Refine the loaded cage and compare against the shuffled input via
     * the parent/child map, using the recovered report arrays. */
    {
        cc_Subd *subd = ccs_Create(loaded, 1);
        if (!subd) {
            ccm_Release(loaded);
            ccu_ReleaseResult(&res);
            ccm_Release(shuffled);
            ccm_Release(cage);
            return fail("ccs_Create failed on the loaded cage");
        }
        ccs_Refine_NoCreases_Gather(subd);

        double worst = 0.0;
        const int V0 = loaded->vertexCount;
        const int F0 = loaded->faceCount;
        for (int i = 0; i < shuffled->vertexCount; ++i) {
            int subdId = -1;
            switch (res.vertexClass[i]) {
                case CC_UNSUBDIVIDE_CLASS_ORIGIN:
                    subdId = res.vertexParent[i];
                    break;
                case CC_UNSUBDIVIDE_CLASS_FACE:
                    subdId = V0 + res.vertexParent[i];
                    break;
                case CC_UNSUBDIVIDE_CLASS_EDGE:
                    subdId = V0 + F0 + res.vertexParent[i];
                    break;
                default:
                    break;
            }
            if (subdId < 0) {
                worst = 1e9;
                break;
            }
            const cc_VertexPoint p = ccs_VertexPoint(subd, subdId, 1);
            const cc_VertexPoint q = shuffled->vertexPoints[i];
            const double dx = (double)p.x - (double)q.x;
            const double dy = (double)p.y - (double)q.y;
            const double dz = (double)p.z - (double)q.z;
            worst = std::max(worst, std::sqrt(dx * dx + dy * dy + dz * dz));
        }

        /* Every loaded parent face must have exactly its degree children in
         * the subd, and the parent/child map must be a bijection. */
        std::vector<int> childCount(F0, 0);
        for (int q = 0; q < shuffled->faceCount; ++q) {
            const int pf = res.parentFaceOfSubface[q];
            if (pf < 0 || pf >= F0) {
                worst = 1e9;
                break;
            }
            ++childCount[pf];
        }
        for (int f = 0; f < F0; ++f) {
            int degree = 0;
            const int start = loaded->faceToHalfedgeIDs[f];
            int h = start;
            do {
                ++degree;
                h = loaded->halfedges[h].nextID;
            } while (h != start);
            if (childCount[f] != degree) {
                worst = 1e9;
                break;
            }
        }
        ccs_Release(subd);
        if (worst > 5e-5) {
            std::fprintf(stderr,
                         "unsubdivide example: forward residual %.3e "
                         "exceeds tolerance\n", worst);
            ccm_Release(loaded);
            ccu_ReleaseResult(&res);
            ccm_Release(shuffled);
            ccm_Release(cage);
            return 1;
        }
        std::printf("round-trip residual: %.3e (tolerance 5e-5)\n", worst);
    }

    ccm_Release(loaded);
    ccu_ReleaseResult(&res);
    ccm_Release(shuffled);
    ccm_Release(cage);
    std::printf("UNSUBDIVIDE_EXAMPLE_OK %s\n", outputPath);
    return 0;
}
