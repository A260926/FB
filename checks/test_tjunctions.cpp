/*
---------------------------------------------------------------------------
Open Asset Import Library (assimp)
---------------------------------------------------------------------------

Task checks for the AI_CONFIG_PP_TRI_RESOLVE_TJUNCTIONS feature.

The file is compiled together with task_support/driver.c (which provides
main and the TASK_CHECK reporting), so it only registers test cases.
---------------------------------------------------------------------------
*/

#include "PostProcessing/ResolveTJunctionsProcess.h"
#include "PostProcessing/TriangulateProcess.h"

#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/mesh.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "task_support/check.h"

using namespace Assimp;

namespace {

constexpr ai_real kEps = ai_real(1e-6);

aiMesh *MakeEmptyMesh(unsigned int numVertices) {
    aiMesh *mesh = new aiMesh();
    mesh->mNumVertices = numVertices;
    mesh->mVertices = new aiVector3D[numVertices]();
    return mesh;
}

void SetVertex(aiMesh *mesh, unsigned int i, ai_real x, ai_real y, ai_real z = 0.0) {
    mesh->mVertices[i] = aiVector3D(x, y, z);
}

void SetFace(aiMesh *mesh, unsigned int faceIndex, const std::vector<unsigned int> &indices) {
    aiFace &face = mesh->mFaces[faceIndex];
    face.mNumIndices = static_cast<unsigned int>(indices.size());
    face.mIndices = new unsigned int[indices.size()];
    for (size_t i = 0; i < indices.size(); ++i) {
        face.mIndices[i] = indices[i];
    }
}

aiMesh *MakeMesh(unsigned int numVertices, const std::vector<std::vector<unsigned int>> &faces) {
    aiMesh *mesh = MakeEmptyMesh(numVertices);
    mesh->mNumFaces = static_cast<unsigned int>(faces.size());
    mesh->mFaces = new aiFace[faces.size()]();
    for (size_t f = 0; f < faces.size(); ++f) {
        SetFace(mesh, static_cast<unsigned int>(f), faces[f]);
    }
    mesh->mPrimitiveTypes = aiPrimitiveType_POLYGON | aiPrimitiveType_TRIANGLE;
    return mesh;
}

void AddNormals(aiMesh *mesh, const std::vector<aiVector3D> &normals) {
    mesh->mNormals = new aiVector3D[mesh->mNumVertices]();
    for (size_t i = 0; i < normals.size(); ++i) {
        mesh->mNormals[i] = normals[i];
    }
}

void AddTangents(aiMesh *mesh, const std::vector<aiVector3D> &tangents,
        const std::vector<aiVector3D> &bitangents) {
    mesh->mTangents = new aiVector3D[mesh->mNumVertices]();
    mesh->mBitangents = new aiVector3D[mesh->mNumVertices]();
    for (size_t i = 0; i < tangents.size(); ++i) {
        mesh->mTangents[i] = tangents[i];
        mesh->mBitangents[i] = bitangents[i];
    }
}

void AddUV(aiMesh *mesh, unsigned int slot, const std::vector<aiVector3D> &uvs,
        unsigned int components = 2) {
    mesh->mTextureCoords[slot] = new aiVector3D[mesh->mNumVertices]();
    mesh->mNumUVComponents[slot] = components;
    for (size_t i = 0; i < uvs.size(); ++i) {
        mesh->mTextureCoords[slot][i] = uvs[i];
    }
}

void AddColors(aiMesh *mesh, unsigned int slot, const std::vector<aiColor4D> &colors) {
    mesh->mColors[slot] = new aiColor4D[mesh->mNumVertices]();
    for (size_t i = 0; i < colors.size(); ++i) {
        mesh->mColors[slot][i] = colors[i];
    }
}

ai_real TriangleArea(const aiVector3D &a, const aiVector3D &b, const aiVector3D &c) {
    return ai_real(0.5) * ((b - a) ^ (c - a)).Length();
}

ai_real RingSignedArea(const std::vector<aiVector3D> &ring) {
    ai_real sum = 0.0;
    for (size_t i = 0; i < ring.size(); ++i) {
        const aiVector3D &a = ring[i];
        const aiVector3D &b = ring[(i + 1) % ring.size()];
        sum += a.x * b.y - b.x * a.y;
    }
    return ai_real(0.5) * sum;
}

using Edge = std::pair<unsigned int, unsigned int>;

Edge MakeEdge(unsigned int a, unsigned int b) {
    return (a < b) ? Edge(a, b) : Edge(b, a);
}

std::set<Edge> CollectTriangleEdges(const aiMesh *mesh) {
    std::set<Edge> edges;
    for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
        const aiFace &face = mesh->mFaces[f];
        for (unsigned int k = 0; k < face.mNumIndices; ++k) {
            edges.insert(MakeEdge(face.mIndices[k],
                    face.mIndices[(k + 1) % face.mNumIndices]));
        }
    }
    return edges;
}

bool PointInPolygon2D(const aiVector3D &p, const std::vector<aiVector3D> &ring) {
    bool inside = false;
    for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
        const aiVector3D &a = ring[j];
        const aiVector3D &b = ring[i];
        if ((a.y > p.y) != (b.y > p.y) &&
                p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) {
            inside = !inside;
        }
    }
    return inside;
}

bool NearlyEqual(ai_real a, ai_real b, ai_real tol = ai_real(1e-5)) {
    return std::fabs(a - b) <= tol;
}

bool VecEqual(const aiVector3D &a, const aiVector3D &b, ai_real tol = ai_real(1e-5)) {
    return NearlyEqual(a.x, b.x, tol) && NearlyEqual(a.y, b.y, tol) &&
           NearlyEqual(a.z, b.z, tol);
}

bool ColorEqual(const aiColor4D &a, const aiColor4D &b, ai_real tol = ai_real(1e-5)) {
    return NearlyEqual(a.r, b.r, tol) && NearlyEqual(a.g, b.g, tol) &&
           NearlyEqual(a.b, b.b, tol) && NearlyEqual(a.a, b.a, tol);
}

// Concave polygon with a split long edge and three neighbor faces.
aiMesh *MakeBoundaryMesh() {
    // 0:(0,0) 1:(4,0) 2:(4,2) 3:(2,1) 4:(0,2)   concave polygon P
    // 5:(1,0) 6:(3,0)                             T-junction candidates
    // 7:(0,-1) 8:(2,-1) 9:(4,-1)                 neighbor face vertices
    aiMesh *mesh = MakeMesh(10,
            { { 0, 1, 2, 3, 4 },        // concave polygon, base edge 0->1
              { 0, 7, 5 },               // neighbor triangle
              { 5, 8, 6 },               // neighbor triangle
              { 6, 9, 1 } });            // neighbor triangle
    const aiVector3D positions[10] = {
        { 0, 0, 0 }, { 4, 0, 0 }, { 4, 2, 0 }, { 2, 1, 0 }, { 0, 2, 0 },
        { 1, 0, 0 }, { 3, 0, 0 }, { 0, -1, 0 }, { 2, -1, 0 }, { 4, -1, 0 }
    };
    for (unsigned int i = 0; i < 10; ++i) {
        SetVertex(mesh, i, positions[i].x, positions[i].y);
    }
    return mesh;
}

} // namespace

// ===========================================================================
// boundary group: topology planning, sub-edges, coverage and winding
// ===========================================================================

static int BoundaryConcavePolygon() {
    std::unique_ptr<aiMesh> mesh(MakeBoundaryMesh());
    const unsigned int originalVertices = mesh->mNumVertices;

    const std::vector<aiVector3D> polygonRing = {
        mesh->mVertices[0], mesh->mVertices[1], mesh->mVertices[2],
        mesh->mVertices[3], mesh->mVertices[4]
    };
    const ai_real originalArea = std::fabs(RingSignedArea(polygonRing));
    const ai_real originalSign = RingSignedArea(polygonRing) > 0.0 ? 1.0 : -1.0;

    TASK_CHECK(ResolveTJunctionsProcess::ResolveMesh(mesh.get(), kEps));

    // The two candidates match the interpolated positions, so they are
    // reused: no vertex is added and no existing vertex moved.
    TASK_CHECK(mesh->mNumVertices == originalVertices);
    TASK_CHECK(VecEqual(mesh->mVertices[5], aiVector3D(1, 0, 0)));
    TASK_CHECK(VecEqual(mesh->mVertices[6], aiVector3D(3, 0, 0)));

    // Polygon ring of 5 with two extra boundary nodes -> 5 triangles,
    // plus the three untouched neighbor triangles.
    TASK_CHECK(mesh->mNumFaces == 8);
    for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
        TASK_CHECK(mesh->mFaces[f].mNumIndices == 3);
        TASK_CHECK(mesh->mFaces[f].mIndices[0] < mesh->mNumVertices);
        TASK_CHECK(mesh->mFaces[f].mIndices[1] < mesh->mNumVertices);
        TASK_CHECK(mesh->mFaces[f].mIndices[2] < mesh->mNumVertices);
    }

    const std::set<Edge> edges = CollectTriangleEdges(mesh.get());

    // Every sub-segment of the original long edge is a real triangle edge.
    TASK_CHECK(edges.count(MakeEdge(0, 5)) == 1);
    TASK_CHECK(edges.count(MakeEdge(5, 6)) == 1);
    TASK_CHECK(edges.count(MakeEdge(6, 1)) == 1);
    // The un-split long edge must not survive.
    TASK_CHECK(edges.count(MakeEdge(0, 1)) == 0);

    // Coverage: the five triangles of the polygon cover its area exactly
    // once, keep the winding and have no zero-area slivers.
    ai_real coveredArea = 0.0;
    unsigned int polygonTriangles = 0;
    for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
        const aiFace &face = mesh->mFaces[f];
        const aiVector3D &a = mesh->mVertices[face.mIndices[0]];
        const aiVector3D &b = mesh->mVertices[face.mIndices[1]];
        const aiVector3D &c = mesh->mVertices[face.mIndices[2]];
        const ai_real area = TriangleArea(a, b, c);
        TASK_CHECK(area > ai_real(1e-9)); // no zero-area triangles
        const aiVector3D centroid((a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3, 0);
        if (PointInPolygon2D(centroid, polygonRing)) {
            ++polygonTriangles;
            coveredArea += area;
            const ai_real sign = RingSignedArea({ a, b, c }) > 0.0 ? 1.0 : -1.0;
            TASK_CHECK(sign == originalSign); // winding preserved
        }
    }
    TASK_CHECK(polygonTriangles == 5);
    TASK_CHECK(NearlyEqual(coveredArea, originalArea, ai_real(1e-5)));
    return 0;
}

static int BoundaryIdempotent() {
    std::unique_ptr<aiMesh> mesh(MakeBoundaryMesh());
    TASK_CHECK(ResolveTJunctionsProcess::ResolveMesh(mesh.get(), kEps));
    const unsigned int verticesAfterFirst = mesh->mNumVertices;
    const unsigned int facesAfterFirst = mesh->mNumFaces;

    // A second run must find no remaining T-junction and change nothing.
    TASK_CHECK(!ResolveTJunctionsProcess::ResolveMesh(mesh.get(), kEps));
    TASK_CHECK(mesh->mNumVertices == verticesAfterFirst);
    TASK_CHECK(mesh->mNumFaces == facesAfterFirst);

    auto splits = ResolveTJunctionsProcess::CollectEdgeSplits(mesh.get(), kEps);
    TASK_CHECK(splits.empty());
    return 0;
}

static int BoundaryMultipleEdgesMultipleSplits() {
    // Quad whose bottom and right edges each carry two split points.
    aiMesh *mesh = MakeMesh(10,
            { { 0, 1, 2, 3 },   // quad with edges 0->1 and 1->2 split
              { 4, 8, 5 },      // makes the bottom candidates part of a face
              { 6, 9, 7 } });   // makes the right candidates part of a face
    SetVertex(mesh, 0, 0, 0);
    SetVertex(mesh, 1, 6, 0);
    SetVertex(mesh, 2, 6, 4);
    SetVertex(mesh, 3, 0, 4);
    SetVertex(mesh, 4, 2, 0);  // bottom edge t = 1/3
    SetVertex(mesh, 5, 4, 0);  // bottom edge t = 2/3
    SetVertex(mesh, 6, 6, 1);  // right edge t = 0.25
    SetVertex(mesh, 7, 6, 3);  // right edge t = 0.75
    SetVertex(mesh, 8, 3, -2);
    SetVertex(mesh, 9, 8, 2);
    std::unique_ptr<aiMesh> keeper(mesh);

    const std::vector<aiVector3D> quad = {
        mesh->mVertices[0], mesh->mVertices[1], mesh->mVertices[2], mesh->mVertices[3]
    };
    const ai_real quadArea = std::fabs(RingSignedArea(quad));

    TASK_CHECK(ResolveTJunctionsProcess::ResolveMesh(mesh, kEps));
    TASK_CHECK(mesh->mNumVertices == 10); // every candidate is reused
    TASK_CHECK(mesh->mNumFaces == 8);     // quad -> 6 tris + 2 neighbor tris

    const std::set<Edge> edges = CollectTriangleEdges(mesh);
    // Bottom edge sub-segments.
    TASK_CHECK(edges.count(MakeEdge(0, 4)) == 1);
    TASK_CHECK(edges.count(MakeEdge(4, 5)) == 1);
    TASK_CHECK(edges.count(MakeEdge(5, 1)) == 1);
    TASK_CHECK(edges.count(MakeEdge(0, 1)) == 0);
    // Right edge sub-segments.
    TASK_CHECK(edges.count(MakeEdge(1, 6)) == 1);
    TASK_CHECK(edges.count(MakeEdge(6, 7)) == 1);
    TASK_CHECK(edges.count(MakeEdge(7, 2)) == 1);
    TASK_CHECK(edges.count(MakeEdge(1, 2)) == 0);

    ai_real covered = 0.0;
    unsigned int quadTriangles = 0;
    for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
        const aiFace &face = mesh->mFaces[f];
        const aiVector3D &a = mesh->mVertices[face.mIndices[0]];
        const aiVector3D &b = mesh->mVertices[face.mIndices[1]];
        const aiVector3D &c = mesh->mVertices[face.mIndices[2]];
        TASK_CHECK(TriangleArea(a, b, c) > ai_real(1e-9));
        const aiVector3D centroid((a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3, 0);
        if (PointInPolygon2D(centroid, quad)) {
            ++quadTriangles;
            covered += TriangleArea(a, b, c);
            TASK_CHECK(RingSignedArea({ a, b, c }) > 0.0); // CCW preserved
        }
    }
    TASK_CHECK(quadTriangles == 6);
    TASK_CHECK(NearlyEqual(covered, quadArea, ai_real(1e-5)));
    return 0;
}

static int BoundarySharedEdgeSameNodesBothSides() {
    // Two faces share edge (0,1) with opposite winding; one midpoint
    // candidate must become a single shared node on both sides.
    aiMesh *mesh = MakeMesh(7,
            { { 0, 1, 3 }, // CCW face above the edge
              { 1, 0, 4 }, // CW face below the edge
              { 2, 5, 6 } }); // references the midpoint candidate
    SetVertex(mesh, 0, 0, 0);
    SetVertex(mesh, 1, 4, 0);
    SetVertex(mesh, 2, 2, 0); // midpoint, t = 0.5
    SetVertex(mesh, 3, 0, 3);
    SetVertex(mesh, 4, 4, 3);
    SetVertex(mesh, 5, 2, -2);
    SetVertex(mesh, 6, 3, -1);
    std::unique_ptr<aiMesh> keeper(mesh);

    TASK_CHECK(ResolveTJunctionsProcess::ResolveMesh(mesh, kEps));
    TASK_CHECK(mesh->mNumVertices == 7); // one shared node, no duplicated insert
    TASK_CHECK(mesh->mNumFaces == 5);    // 2 + 2 expanded tris + the neighbor tri

    const std::set<Edge> edges = CollectTriangleEdges(mesh);
    TASK_CHECK(edges.count(MakeEdge(0, 2)) == 1);
    TASK_CHECK(edges.count(MakeEdge(2, 1)) == 1);
    TASK_CHECK(edges.count(MakeEdge(0, 1)) == 0);

    // The same candidate index is referenced on both sides, and each
    // face keeps its original winding (CCW vs CW).
    bool ccwUsesNode = false;
    bool cwUsesNode = false;
    for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
        const aiFace &face = mesh->mFaces[f];
        bool referencesNode = false;
        for (unsigned int k = 0; k < 3; ++k) {
            referencesNode = referencesNode || face.mIndices[k] == 2;
        }
        if (!referencesNode) {
            continue;
        }
        const ai_real sign = RingSignedArea({
            mesh->mVertices[face.mIndices[0]], mesh->mVertices[face.mIndices[1]],
            mesh->mVertices[face.mIndices[2]] });
        if (sign > 0.0) {
            ccwUsesNode = true;
        } else if (sign < 0.0) {
            cwUsesNode = true;
        }
    }
    TASK_CHECK(ccwUsesNode);
    TASK_CHECK(cwUsesNode);
    return 0;
}

static int BoundaryPointsAndLinesPreserved() {
    // A point-only and a line-only vertex must never become candidates.
    aiMesh *mesh = MakeMesh(6,
            { { 0, 1, 2 }, // triangle with long edge 0->1
              { 3 },       // point primitive
              { 4, 5 } });  // line primitive
    SetVertex(mesh, 0, 0, 0);
    SetVertex(mesh, 1, 4, 0);
    SetVertex(mesh, 2, 0, 2);
    SetVertex(mesh, 3, 2, 0); // lies on the long edge but is a point primitive
    SetVertex(mesh, 4, 9, 9);
    SetVertex(mesh, 5, 8, 8);
    mesh->mPrimitiveTypes |= aiPrimitiveType_POINT | aiPrimitiveType_LINE;
    std::unique_ptr<aiMesh> keeper(mesh);

    // The point-primitive vertex must not be picked up.
    auto splits = ResolveTJunctionsProcess::CollectEdgeSplits(mesh, kEps);
    TASK_CHECK(splits.empty());
    TASK_CHECK(!ResolveTJunctionsProcess::ResolveMesh(mesh, kEps));

    // Now make vertex 3 part of a real triangle as well.
    aiFace *grownFaces = new aiFace[4]();
    for (unsigned int f = 0; f < 3; ++f) {
        grownFaces[f].mNumIndices = mesh->mFaces[f].mNumIndices;
        grownFaces[f].mIndices = mesh->mFaces[f].mIndices;
        mesh->mFaces[f].mIndices = nullptr;
    }
    delete[] mesh->mFaces;
    mesh->mFaces = grownFaces;
    mesh->mNumFaces = 4;
    SetFace(mesh, 3, { 3, 5, 4 });

    TASK_CHECK(ResolveTJunctionsProcess::ResolveMesh(mesh, kEps));
    bool foundPoint = false;
    bool foundLine = false;
    for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
        const aiFace &face = mesh->mFaces[f];
        if (face.mNumIndices == 1) {
            foundPoint = true;
            TASK_CHECK(face.mIndices[0] == 3);
        }
        if (face.mNumIndices == 2) {
            foundLine = true;
            TASK_CHECK(face.mIndices[0] == 4 && face.mIndices[1] == 5);
        }
    }
    TASK_CHECK(foundPoint);
    TASK_CHECK(foundLine);
    return 0;
}

static int BoundaryNoCrossMeshPairing() {
    // Mesh A owns the long edge; mesh B merely contains a vertex at the
    // same coordinates. Processing must stay per-mesh.
    aiMesh *meshA = MakeMesh(3, { { 0, 1, 2 } });
    SetVertex(meshA, 0, 0, 0);
    SetVertex(meshA, 1, 2, 0);
    SetVertex(meshA, 2, 0, 2);
    aiMesh *meshB = MakeMesh(3, { { 0, 1, 2 } });
    SetVertex(meshB, 0, 1, 0); // same physical position as the T point
    SetVertex(meshB, 1, 1, -1);
    SetVertex(meshB, 2, 2, -1);
    std::unique_ptr<aiMesh> keepA(meshA);
    std::unique_ptr<aiMesh> keepB(meshB);

    TASK_CHECK(!ResolveTJunctionsProcess::ResolveMesh(meshA, kEps));
    TASK_CHECK(meshA->mNumFaces == 1);
    TASK_CHECK(meshA->mFaces[0].mNumIndices == 3);
    TASK_CHECK(meshA->mFaces[0].mIndices[0] == 0);
    TASK_CHECK(meshA->mFaces[0].mIndices[1] == 1);
    TASK_CHECK(meshA->mFaces[0].mIndices[2] == 2);
    TASK_CHECK(meshA->mNumVertices == 3);
    return 0;
}

static int BoundaryEpsilonValidation() {
    std::unique_ptr<aiMesh> mesh(MakeBoundaryMesh());
    const unsigned int vertices = mesh->mNumVertices;
    const unsigned int faces = mesh->mNumFaces;

    for (ai_real bad : { ai_real(0.0), ai_real(-1e-6),
                 std::numeric_limits<ai_real>::infinity(),
                 -std::numeric_limits<ai_real>::infinity(),
                 std::numeric_limits<ai_real>::quiet_NaN() }) {
        TASK_CHECK(!ResolveTJunctionsProcess::IsValidEpsilon(bad));
        TASK_CHECK(!ResolveTJunctionsProcess::ResolveMesh(mesh.get(), bad));
        // Nothing was modified before the rejection.
        TASK_CHECK(mesh->mNumVertices == vertices);
        TASK_CHECK(mesh->mNumFaces == faces);
    }
    TASK_CHECK(ResolveTJunctionsProcess::IsValidEpsilon(kEps));
    return 0;
}

// ===========================================================================
// streams group: attribute interpolation, reuse rules, anim and bone data
// ===========================================================================

namespace {

// Long edge 0->1 (length 4) with one midpoint candidate vertex 3
// (t = 0.5); vertex 4 gives the candidate a face to live in.
aiMesh *MakeSeamMesh() {
    aiMesh *mesh = MakeMesh(5, { { 0, 1, 2 }, { 3, 4, 0 } });
    SetVertex(mesh, 0, 0, 0);
    SetVertex(mesh, 1, 4, 0);
    SetVertex(mesh, 2, 0, 3);
    SetVertex(mesh, 3, 2, 0); // t = 0.5
    SetVertex(mesh, 4, 2, -2);
    return mesh;
}

} // namespace

static int StreamsSeamDoesNotOverwriteEdgeData() {
    std::unique_ptr<aiMesh> mesh(MakeSeamMesh());
    // Edge endpoints carry smooth UVs and normals.
    AddUV(mesh.get(), 0, { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 },
            { 0.9, 0.9, 0 }, // candidate is a seam: must NOT overwrite edge data
            { 0.7, 0.2, 0 } });
    AddNormals(mesh.get(), { { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 },
            { 1, 0, 0 }, // seam normal of the candidate
            { 0, 1, 0 } });

    const unsigned int originalCount = mesh->mNumVertices;
    TASK_CHECK(ResolveTJunctionsProcess::ResolveMesh(mesh.get(), kEps));

    // The mismatching seam candidate forces one appended vertex.
    TASK_CHECK(mesh->mNumVertices == originalCount + 1);
    const unsigned int appended = originalCount;

    // The new vertex interpolates the long edge, never the candidate's
    // own seam UV/normal.
    TASK_CHECK(VecEqual(mesh->mTextureCoords[0][appended], aiVector3D(0.5, 0, 0)));
    TASK_CHECK(VecEqual(mesh->mNormals[appended], aiVector3D(0, 0, 1)));
    // The seam candidate itself is untouched.
    TASK_CHECK(VecEqual(mesh->mTextureCoords[0][3], aiVector3D(0.9, 0.9, 0)));
    TASK_CHECK(VecEqual(mesh->mNormals[3], aiVector3D(1, 0, 0)));
    // Existing vertices were not moved.
    TASK_CHECK(VecEqual(mesh->mVertices[0], aiVector3D(0, 0, 0)));
    TASK_CHECK(VecEqual(mesh->mVertices[1], aiVector3D(4, 0, 0)));
    return 0;
}

static int StreamsSparseUVAndColorSlots() {
    std::unique_ptr<aiMesh> mesh(MakeSeamMesh());
    // Sparse UV slots: 0 and 2 populated, 1 empty. The candidate values
    // deliberately mismatch the edge interpolation so a vertex is added.
    AddUV(mesh.get(), 0, { { 0, 0, 0 }, { 10, 0, 0 }, { 0, 0, 0 },
            { 7.7f, 7.7f, 0 }, { 7, 0, 0 } });
    AddUV(mesh.get(), 2, { { 0, 0, 7 }, { 2, 4, 7 }, { 9, 9, 7 },
            { 3.3f, 3.3f, 7 }, { 3, 3, 7 } }, 3);
    // Sparse color slots: 1 populated, 0 empty.
    AddColors(mesh.get(), 1, { { 0, 0, 0, 1 }, { 1, 1, 1, 1 }, { 0.2f, 0.2f, 0.2f, 1 },
            { 0.8f, 0.1f, 0.3f, 1 }, { 0.7f, 0.7f, 0.7f, 1 } });

    TASK_CHECK(ResolveTJunctionsProcess::ResolveMesh(mesh.get(), kEps));
    const unsigned int appended = 5;

    // Empty slots stay empty, processing does not stop at the first gap.
    TASK_CHECK(mesh->mTextureCoords[0] != nullptr);
    TASK_CHECK(mesh->mTextureCoords[1] == nullptr);
    TASK_CHECK(mesh->mTextureCoords[2] != nullptr);
    TASK_CHECK(mesh->mColors[0] == nullptr);
    TASK_CHECK(mesh->mColors[1] != nullptr);

    TASK_CHECK(VecEqual(mesh->mTextureCoords[0][appended], aiVector3D(5, 0, 0)));
    TASK_CHECK(VecEqual(mesh->mTextureCoords[2][appended], aiVector3D(1, 2, 7)));
    TASK_CHECK(mesh->mNumUVComponents[2] == 3);
    TASK_CHECK(ColorEqual(mesh->mColors[1][appended], aiColor4D(0.5, 0.5, 0.5, 1)));
    return 0;
}

static int StreamsNormalizedVectorsAndZero() {
    std::unique_ptr<aiMesh> mesh(MakeSeamMesh());
    const aiVector3D n0(1, 0, 0);
    const aiVector3D n1(0, 1, 0);
    AddNormals(mesh.get(), { n0, n1, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 } });
    // Tangents: both endpoints zero on the long edge -> zero stays zero.
    AddTangents(mesh.get(),
            { { 0, 0, 0 }, { 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, 0 }, { 1, 0, 0 } },
            { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 } });

    TASK_CHECK(ResolveTJunctionsProcess::ResolveMesh(mesh.get(), kEps));
    const unsigned int appended = 5;

    // Normalized linear interpolation of the endpoint normals.
    const aiVector3D expectedNormal((n0 + n1).Normalize());
    TASK_CHECK(VecEqual(mesh->mNormals[appended], expectedNormal, ai_real(1e-5)));
    TASK_CHECK(NearlyEqual(mesh->mNormals[appended].Length(), ai_real(1.0)));
    // Zero tangent interpolates to zero and is not "normalized" into NaN.
    TASK_CHECK(VecEqual(mesh->mTangents[appended], aiVector3D(0, 0, 0)));
    // Bitangent interpolation is normalized as well.
    TASK_CHECK(NearlyEqual(mesh->mBitangents[appended].Length(), ai_real(1.0)));
    return 0;
}

static int StreamsComplementaryAnimMeshes() {
    std::unique_ptr<aiMesh> mesh(MakeSeamMesh());

    mesh->mNumAnimMeshes = 2;
    mesh->mAnimMeshes = new aiAnimMesh *[2]();
    // Anim A: positions only. The candidate's pose deliberately differs
    // from the edge interpolation, forcing a new vertex to be appended.
    aiAnimMesh *animA = new aiAnimMesh();
    animA->mName = aiString("poseA");
    animA->mWeight = 0.25f;
    animA->mNumVertices = mesh->mNumVertices;
    animA->mVertices = new aiVector3D[mesh->mNumVertices]();
    for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
        animA->mVertices[i] = mesh->mVertices[i] * ai_real(2.0);
    }
    animA->mVertices[3] = aiVector3D(99, 99, 99); // seam candidate mismatch
    // Anim B: normals only (complementary missing stream).
    aiAnimMesh *animB = new aiAnimMesh();
    animB->mName = aiString("poseB");
    animB->mWeight = 0.75f;
    animB->mNumVertices = mesh->mNumVertices;
    animB->mNormals = new aiVector3D[mesh->mNumVertices]();
    for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
        animB->mNormals[i] = aiVector3D(0, 0, 1);
    }
    mesh->mAnimMeshes[0] = animA;
    mesh->mAnimMeshes[1] = animB;

    TASK_CHECK(ResolveTJunctionsProcess::ResolveMesh(mesh.get(), kEps));
    const unsigned int appended = 5;

    TASK_CHECK(mesh->mNumAnimMeshes == 2);
    TASK_CHECK(std::strcmp(animA->mName.C_Str(), "poseA") == 0);
    TASK_CHECK(std::strcmp(animB->mName.C_Str(), "poseB") == 0);
    TASK_CHECK(NearlyEqual(animA->mWeight, 0.25f));
    TASK_CHECK(NearlyEqual(animB->mWeight, 0.75f));

    // Non-null streams grew, null streams stayed null.
    TASK_CHECK(animA->mVertices != nullptr && animA->mNormals == nullptr);
    TASK_CHECK(animB->mNormals != nullptr && animB->mVertices == nullptr);
    TASK_CHECK(animA->mNumVertices == mesh->mNumVertices);
    TASK_CHECK(animB->mNumVertices == mesh->mNumVertices);

    // Interpolation uses the same edge parameter; the seam candidate's
    // own anim value is never used.
    TASK_CHECK(VecEqual(animA->mVertices[appended], aiVector3D(4, 0, 0)));
    TASK_CHECK(VecEqual(animA->mVertices[3], aiVector3D(99, 99, 99)));
    TASK_CHECK(VecEqual(animB->mNormals[appended], aiVector3D(0, 0, 1)));

    // Independent ownership: touching one anim stream does not reach the other.
    animA->mVertices[appended] = aiVector3D(42, 42, 42);
    TASK_CHECK(VecEqual(animB->mNormals[appended], aiVector3D(0, 0, 1)));
    return 0;
}

static int StreamsBoneWeights() {
    // Edge 0->1 with a mismatching candidate 3 so an extra vertex is made.
    aiMesh *mesh = MakeMesh(4, { { 0, 1, 2 }, { 3, 2, 0 } });
    SetVertex(mesh, 0, 0, 0);
    SetVertex(mesh, 1, 4, 0);
    SetVertex(mesh, 2, 0, 3);
    SetVertex(mesh, 3, 2, 0); // t = 0.5, weights below deliberately differ
    std::unique_ptr<aiMesh> keeper(mesh);

    mesh->mNumBones = 3;
    mesh->mBones = new aiBone *[3]();
    aiBone *bone0 = new aiBone();
    bone0->mName = aiString("bone0");
    bone0->mNumWeights = 2;
    bone0->mWeights = new aiVertexWeight[2]{ { 0, 1.0f }, { 3, 0.1f } };
    aiBone *bone1 = new aiBone();
    bone1->mName = aiString("bone1");
    bone1->mNumWeights = 2;
    bone1->mWeights = new aiVertexWeight[2]{ { 1, 1.0f }, { 3, 0.1f } };
    aiBone *bone2 = new aiBone();
    bone2->mName = aiString("bone2");
    bone2->mNumWeights = 1;
    bone2->mWeights = new aiVertexWeight[1]{ { 2, 1.0f } };
    mesh->mBones[0] = bone0;
    mesh->mBones[1] = bone1;
    mesh->mBones[2] = bone2;

    TASK_CHECK(ResolveTJunctionsProcess::ResolveMesh(mesh, kEps));
    TASK_CHECK(mesh->mNumVertices == 5);
    const unsigned int appended = 4;

    // Midpoint of (bone0=1) and (bone1=1): half weights, no renormalization.
    const auto weightOf = [&](unsigned int bone, unsigned int vertex) -> ai_real {
        for (unsigned int w = 0; w < mesh->mBones[bone]->mNumWeights; ++w) {
            if (mesh->mBones[bone]->mWeights[w].mVertexId == vertex) {
                return mesh->mBones[bone]->mWeights[w].mWeight;
            }
        }
        return ai_real(-1.0);
    };
    TASK_CHECK(NearlyEqual(weightOf(0, appended), ai_real(0.5)));
    TASK_CHECK(NearlyEqual(weightOf(1, appended), ai_real(0.5)));
    // bone2 influences neither endpoint, so no zero entry is appended.
    TASK_CHECK(weightOf(2, appended) < ai_real(0.0));
    TASK_CHECK(bone0->mNumWeights == 3 && bone1->mNumWeights == 3);
    TASK_CHECK(bone2->mNumWeights == 1);

    // The existing candidate weights are untouched.
    TASK_CHECK(NearlyEqual(weightOf(0, 3), ai_real(0.1)));
    TASK_CHECK(NearlyEqual(weightOf(1, 3), ai_real(0.1)));
    return 0;
}

static int StreamsReuseWhenEverythingMatches() {
    // Candidate at the midpoint with perfectly matching position, UV,
    // normal and bone weights: the original index must be reused.
    aiMesh *mesh = MakeMesh(4, { { 0, 1, 2 }, { 3, 2, 0 } });
    SetVertex(mesh, 0, 0, 0);
    SetVertex(mesh, 1, 4, 0);
    SetVertex(mesh, 2, 0, 3);
    SetVertex(mesh, 3, 2, 0);
    AddUV(mesh, 0, { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0.5, 0, 0 } });
    AddNormals(mesh, { { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 } });

    mesh->mNumBones = 1;
    mesh->mBones = new aiBone *[1]();
    aiBone *bone = new aiBone();
    bone->mName = aiString("bone");
    bone->mNumWeights = 3;
    bone->mWeights = new aiVertexWeight[3]{ { 0, 1.0f }, { 1, 1.0f }, { 3, 1.0f } };
    mesh->mBones[0] = bone;
    std::unique_ptr<aiMesh> keeper(mesh);

    TASK_CHECK(ResolveTJunctionsProcess::ResolveMesh(mesh, kEps));
    TASK_CHECK(mesh->mNumVertices == 4); // reused, nothing appended
    const std::set<Edge> edges = CollectTriangleEdges(mesh);
    TASK_CHECK(edges.count(MakeEdge(0, 3)) == 1);
    TASK_CHECK(edges.count(MakeEdge(3, 1)) == 1);
    return 0;
}

// ===========================================================================
// pipeline group: importer entry point, repeat runs and OBJ round trips
// ===========================================================================

namespace {

const char *kObjData =
        "v 0 0 0\n"
        "v 2 0 0\n"
        "v 0 2 0\n"
        "v 1 0 0\n"
        "v 0 -1 0\n"
        "v 2 -1 0\n"
        "vt 0 0\nvt 1 0\nvt 0 1\nvt 0.5 0\nvt 0 -0.5\nvt 1 -0.5\n"
        "vn 0 0 1\n"
        "f 1/1/1 2/2/1 3/3/1\n"
        "f 2/2/1 4/4/1 6/6/1\n"
        "f 4/4/1 1/1/1 5/5/1\n";

bool SceneHasLongEdge(const aiScene *scene) {
    for (unsigned int m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh *mesh = scene->mMeshes[m];
        const std::set<Edge> edges = CollectTriangleEdges(mesh);
        if (edges.count(MakeEdge(0, 1)) == 1) {
            // Indexing differs per import, verify geometrically instead.
        }
        for (const Edge &edge : edges) {
            const aiVector3D &a = mesh->mVertices[edge.first];
            const aiVector3D &b = mesh->mVertices[edge.second];
            if (VecEqual(a, aiVector3D(0, 0, 0)) && VecEqual(b, aiVector3D(2, 0, 0))) {
                return true;
            }
        }
    }
    return false;
}

unsigned int TotalFaces(const aiScene *scene) {
    unsigned int total = 0;
    for (unsigned int m = 0; m < scene->mNumMeshes; ++m) {
        total += scene->mMeshes[m]->mNumFaces;
    }
    return total;
}

} // namespace

static int PipelineImporterOptionEntry() {
    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_PP_TRI_RESOLVE_TJUNCTIONS, true);
    const aiScene *scene = importer.ReadFileFromMemory(kObjData, std::strlen(kObjData),
            aiProcess_Triangulate | aiProcess_JoinIdenticalVertices, "obj");
    TASK_CHECK(scene != nullptr);
    TASK_CHECK(scene->mNumMeshes >= 1);

    const aiMesh *mesh = scene->mMeshes[0];
    TASK_CHECK(mesh->mNumFaces == 4); // 3 original faces, long edge split adds one
    TASK_CHECK(!SceneHasLongEdge(scene));
    TASK_CHECK(mesh->HasTextureCoords(0));
    TASK_CHECK(mesh->HasNormals());

    const std::set<Edge> edges = CollectTriangleEdges(mesh);
    bool hasHalfSegment = false;
    for (const Edge &edge : edges) {
        const aiVector3D &a = mesh->mVertices[edge.first];
        const aiVector3D &b = mesh->mVertices[edge.second];
        if (VecEqual(a, aiVector3D(0, 0, 0)) && VecEqual(b, aiVector3D(1, 0, 0))) {
            hasHalfSegment = true;
        }
    }
    TASK_CHECK(hasHalfSegment);
    return 0;
}

static int PipelineDisabledByDefault() {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFileFromMemory(kObjData, std::strlen(kObjData),
            aiProcess_Triangulate | aiProcess_JoinIdenticalVertices, "obj");
    TASK_CHECK(scene != nullptr);
    TASK_CHECK(TotalFaces(scene) == 3);
    TASK_CHECK(SceneHasLongEdge(scene));
    return 0;
}

static int PipelineRepeatProcessing() {
    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_PP_TRI_RESOLVE_TJUNCTIONS, true);
    const aiScene *scene = importer.ReadFileFromMemory(kObjData, std::strlen(kObjData),
            aiProcess_Triangulate | aiProcess_JoinIdenticalVertices, "obj");
    TASK_CHECK(scene != nullptr);
    const unsigned int faces = TotalFaces(scene);
    const unsigned int vertices = scene->mMeshes[0]->mNumVertices;
    TASK_CHECK(faces == 4);

    // Running the triangulation step a second time must not grow anything.
    TriangulateProcess process;
    process.SetupProperties(&importer);
    process.Execute(const_cast<aiScene *>(scene));
    TASK_CHECK(TotalFaces(scene) == faces);
    TASK_CHECK(scene->mMeshes[0]->mNumVertices == vertices);

    // A third pass through the reusable production logic is a no-op too.
    TASK_CHECK(!ResolveTJunctionsProcess::ResolveMesh(
            const_cast<aiMesh *>(scene->mMeshes[0]), kEps));
    return 0;
}

static int PipelineObjRoundTrip() {
    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_PP_TRI_RESOLVE_TJUNCTIONS, true);
    const aiScene *scene = importer.ReadFileFromMemory(kObjData, std::strlen(kObjData),
            aiProcess_Triangulate | aiProcess_JoinIdenticalVertices, "obj");
    TASK_CHECK(scene != nullptr);
    const aiMesh *before = scene->mMeshes[0];
    const ai_real uvAtSplit = before->mTextureCoords[0][3].x;

    Assimp::Exporter exporter;
    const aiExportDataBlob *blob = exporter.ExportToBlob(scene, "obj");
    TASK_CHECK(blob != nullptr && blob->size > 0);

    Assimp::Importer reimporter;
    const aiScene *reloaded = reimporter.ReadFileFromMemory(blob->data, blob->size,
            aiProcess_Triangulate, "obj");
    TASK_CHECK(reloaded != nullptr);
    TASK_CHECK(TotalFaces(reloaded) == 4);
    TASK_CHECK(!SceneHasLongEdge(reloaded));
    TASK_CHECK(reloaded->mMeshes[0]->HasTextureCoords(0));
    TASK_CHECK(reloaded->mMeshes[0]->HasNormals());

    // The interpolated UV0 of the midpoint survives the round trip.
    bool foundMidpointUv = false;
    const aiMesh *mesh = reloaded->mMeshes[0];
    for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
        if (VecEqual(mesh->mVertices[v], aiVector3D(1, 0, 0)) &&
                NearlyEqual(mesh->mTextureCoords[0][v].x, uvAtSplit, ai_real(1e-4))) {
            foundMidpointUv = true;
        }
    }
    TASK_CHECK(foundMidpointUv);
    return 0;
}

static int PipelineInvalidEpsilonRejected() {
    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_PP_TRI_RESOLVE_TJUNCTIONS, true);
    importer.SetPropertyFloat(AI_CONFIG_PP_TRI_TJUNCTION_EPSILON, ai_real(0.0));
    const aiScene *scene = importer.ReadFileFromMemory(kObjData, std::strlen(kObjData),
            aiProcess_Triangulate, "obj");
    // The step throws before touching anything, so the import fails.
    TASK_CHECK(scene == nullptr);
    TASK_CHECK(std::strlen(importer.GetErrorString()) > 0);
    return 0;
}

static int PipelineCompatibleWithJoinAndNormals() {
    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_PP_TRI_RESOLVE_TJUNCTIONS, true);
    const aiScene *scene = importer.ReadFileFromMemory(kObjData, std::strlen(kObjData),
            aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
                    aiProcess_GenSmoothNormals,
            "obj");
    TASK_CHECK(scene != nullptr);
    TASK_CHECK(TotalFaces(scene) == 4);
    TASK_CHECK(scene->mMeshes[0]->HasNormals());
    for (unsigned int f = 0; f < scene->mMeshes[0]->mNumFaces; ++f) {
        TASK_CHECK(scene->mMeshes[0]->mFaces[f].mNumIndices == 3);
    }
    return 0;
}

static int PipelineTaskSupportFile() {
    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_PP_TRI_RESOLVE_TJUNCTIONS, true);
    const aiScene *scene = importer.ReadFile("task_support/tjunction.obj",
            aiProcess_Triangulate | aiProcess_JoinIdenticalVertices);
    TASK_CHECK(scene != nullptr);
    TASK_CHECK(TotalFaces(scene) == 4);
    TASK_CHECK(!SceneHasLongEdge(scene));
    return 0;
}

static int PipelineVerboseFormatWithoutJoin() {
    // Without JoinIdenticalVertices the OBJ stays verbose: the same
    // physical midpoint is referenced by separate corner indices. The
    // equivalent-candidate merge must still produce exactly one split
    // node and no duplicated inserted vertex.
    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_PP_TRI_RESOLVE_TJUNCTIONS, true);
    const aiScene *scene = importer.ReadFile("task_support/tjunction.obj",
            aiProcess_Triangulate);
    TASK_CHECK(scene != nullptr);
    const aiMesh *mesh = scene->mMeshes[0];
    TASK_CHECK(mesh->mNumVertices == 9); // original verbose count, no growth
    TASK_CHECK(mesh->mNumFaces == 4);
    TASK_CHECK(!SceneHasLongEdge(scene));
    for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
        TASK_CHECK(mesh->mFaces[f].mNumIndices == 3);
    }
    return 0;
}

// ===========================================================================
// Registration
// ===========================================================================

const struct task_case task_cases[] = {
    // boundary
    { "boundary", "concave_polygon_subedges_coverage_winding", BoundaryConcavePolygon },
    { "boundary", "idempotent_repeat_run", BoundaryIdempotent },
    { "boundary", "multiple_edges_multiple_splits", BoundaryMultipleEdgesMultipleSplits },
    { "boundary", "shared_edge_same_nodes_both_sides", BoundarySharedEdgeSameNodesBothSides },
    { "boundary", "points_and_lines_preserved", BoundaryPointsAndLinesPreserved },
    { "boundary", "no_cross_mesh_pairing", BoundaryNoCrossMeshPairing },
    { "boundary", "epsilon_validation", BoundaryEpsilonValidation },
    // streams
    { "streams", "seam_attributes_interpolated_from_edge", StreamsSeamDoesNotOverwriteEdgeData },
    { "streams", "sparse_uv_color_slots", StreamsSparseUVAndColorSlots },
    { "streams", "normalized_vectors_and_zero", StreamsNormalizedVectorsAndZero },
    { "streams", "complementary_anim_mesh_streams", StreamsComplementaryAnimMeshes },
    { "streams", "sparse_bone_weight_interpolation", StreamsBoneWeights },
    { "streams", "reuse_original_index_when_identical", StreamsReuseWhenEverythingMatches },
    // pipeline
    { "pipeline", "importer_option_entry", PipelineImporterOptionEntry },
    { "pipeline", "disabled_by_default", PipelineDisabledByDefault },
    { "pipeline", "repeat_processing_is_stable", PipelineRepeatProcessing },
    { "pipeline", "obj_roundtrip_preserves_chain", PipelineObjRoundTrip },
    { "pipeline", "invalid_epsilon_rejected", PipelineInvalidEpsilonRejected },
    { "pipeline", "join_vertices_and_normals_compatible", PipelineCompatibleWithJoinAndNormals },
    { "pipeline", "task_support_obj_file", PipelineTaskSupportFile },
    { "pipeline", "verbose_format_without_join", PipelineVerboseFormatWithoutJoin },
};

const size_t task_case_count =
        sizeof(task_cases) / sizeof(task_cases[0]);
