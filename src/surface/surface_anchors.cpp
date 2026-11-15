#include "geometrycentral/surface/surface_anchors.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <unordered_set>

namespace geometrycentral {
namespace surface {

namespace {

// Independent closest-point-on-triangle routine (Ericson, Real-Time Collision Detection),
// returning barycentric weights for (a, b, c) of the Euclidean closest point to p.
Vector3 closestPointOnTriangleBary(const Vector3& p, const Vector3& a, const Vector3& b, const Vector3& c) {
  Vector3 ab = b - a;
  Vector3 ac = c - a;
  Vector3 ap = p - a;
  double d1 = dot(ab, ap);
  double d2 = dot(ac, ap);
  if (d1 <= 0. && d2 <= 0.) return Vector3{1., 0., 0.};

  Vector3 bp = p - b;
  double d3 = dot(ab, bp);
  double d4 = dot(ac, bp);
  if (d3 >= 0. && d4 <= d3) return Vector3{0., 1., 0.};

  double vc = d1 * d4 - d3 * d2;
  if (vc <= 0. && d1 >= 0. && d3 <= 0.) {
    double v = d1 / (d1 - d3);
    return Vector3{1. - v, v, 0.};
  }

  Vector3 cp = p - c;
  double d5 = dot(ab, cp);
  double d6 = dot(ac, cp);
  if (d6 >= 0. && d5 <= d6) return Vector3{0., 0., 1.};

  double vb = d5 * d2 - d1 * d6;
  if (vb <= 0. && d2 >= 0. && d6 <= 0.) {
    double w = d2 / (d2 - d6);
    return Vector3{1. - w, 0., w};
  }

  double va = d3 * d6 - d5 * d4;
  if (va <= 0. && (d4 - d3) >= 0. && (d5 - d6) >= 0.) {
    double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    return Vector3{0., 1. - w, w};
  }

  double denom = 1. / (va + vb + vc);
  Vector3 bary{va * denom, vb * denom, vc * denom};
  // Clean up rounding noise at the simplex boundary and renormalize.
  if (bary.x < 0.) bary.x = 0.;
  if (bary.y < 0.) bary.y = 0.;
  if (bary.z < 0.) bary.z = 0.;
  double s = bary.x + bary.y + bary.z;
  return bary / s;
}

bool finiteVector(const Vector3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

} // namespace

SurfaceAnchorSet::SurfaceAnchorSet(MutationManager& manager_, const std::vector<SurfaceAnchorInput>& inputs)
    : manager(manager_), mesh(manager_.mesh), geometry(manager_.geometry) {

  if (geometry == nullptr) {
    throw std::invalid_argument("SurfaceAnchorSet requires a MutationManager with geometry");
  }
  if (manager_.surfaceAnchorSet != nullptr) {
    throw std::invalid_argument("MutationManager already has a SurfaceAnchorSet bound");
  }

  // === Phase 1: validate everything before registering any callback. ===
  std::unordered_set<uint64_t> usedIds;
  usedIds.reserve(inputs.size());

  // A face is chosen by smallest current face index; remember the choice so vertex/edge markers
  // with several adjacent faces resolve deterministically.
  auto pickFace = [&](const SurfacePoint& sp) -> Face {
    Face chosen;
    switch (sp.type) {
    case SurfacePointType::Vertex: {
      for (Face f : sp.vertex.adjacentFaces()) {
        if (chosen == Face() || f.getIndex() < chosen.getIndex()) chosen = f;
      }
      break;
    }
    case SurfacePointType::Edge: {
      for (Face f : sp.edge.adjacentFaces()) {
        if (chosen == Face() || f.getIndex() < chosen.getIndex()) chosen = f;
      }
      break;
    }
    case SurfacePointType::Face: {
      chosen = sp.face;
      break;
    }
    }
    return chosen;
  };

  std::vector<Anchor> built;
  built.reserve(inputs.size());

  for (const SurfaceAnchorInput& input : inputs) {
    if (input.id == 0) throw std::invalid_argument("SurfaceAnchorInput id must be non-zero");
    if (!usedIds.insert(input.id).second) throw std::invalid_argument("duplicate SurfaceAnchorInput id");
    if (!std::isfinite(input.maxDistance) || input.maxDistance < 0.) {
      throw std::invalid_argument("SurfaceAnchorInput maxDistance must be finite and non-negative");
    }

    const SurfacePoint& sp = input.point;
    Face face;
    Vector3 bary = Vector3::zero();

    switch (sp.type) {
    case SurfacePointType::Vertex: {
      if (sp.vertex.getMesh() != &mesh || sp.vertex == Vertex() || sp.vertex.isDead()) {
        throw std::invalid_argument("SurfaceAnchorInput vertex is invalid or on another mesh");
      }
      face = pickFace(sp);
      if (face == Face()) throw std::invalid_argument("SurfaceAnchorInput vertex has no adjacent face");
      std::array<Vertex, 3> fv;
      size_t k = 0;
      for (Vertex v : face.adjacentVertices()) fv[k++] = v;
      for (k = 0; k < 3; k++) {
        if (fv[k] == sp.vertex) bary[k] = 1.;
      }
      break;
    }
    case SurfacePointType::Edge: {
      if (sp.edge.getMesh() != &mesh || sp.edge == Edge() || sp.edge.isDead() || !std::isfinite(sp.tEdge) ||
          sp.tEdge < 0. || sp.tEdge > 1.) {
        throw std::invalid_argument("SurfaceAnchorInput edge point is invalid");
      }
      face = pickFace(sp);
      if (face == Face()) throw std::invalid_argument("SurfaceAnchorInput edge has no adjacent face");
      Vertex edgeTail = sp.edge.halfedge().tailVertex();
      Vertex edgeTip = sp.edge.halfedge().tipVertex();
      std::array<Vertex, 3> fv;
      size_t k = 0;
      for (Vertex v : face.adjacentVertices()) fv[k++] = v;
      for (k = 0; k < 3; k++) {
        if (fv[k] == edgeTail) bary[k] = 1. - sp.tEdge;
        if (fv[k] == edgeTip) bary[k] = sp.tEdge;
      }
      break;
    }
    case SurfacePointType::Face: {
      if (sp.face.getMesh() != &mesh || sp.face == Face() || sp.face.isDead() || sp.face.isBoundaryLoop()) {
        throw std::invalid_argument("SurfaceAnchorInput face is invalid or on another mesh");
      }
      face = sp.face;
      bary = sp.faceCoords;
      if (!finiteVector(bary) || bary.x < 0. || bary.y < 0. || bary.z < 0.) {
        throw std::invalid_argument("SurfaceAnchorInput barycentric weights must be finite and non-negative");
      }
      double sum = bary.x + bary.y + bary.z;
      if (std::abs(sum - 1.) > 1e-12) {
        throw std::invalid_argument("SurfaceAnchorInput barycentric weights must sum to 1");
      }
      bary = bary / sum;
      break;
    }
    }

    std::array<Vertex, 3> faceVerts;
    size_t k = 0;
    for (Vertex v : face.adjacentVertices()) faceVerts[k++] = v;

    Vector3 target = Vector3::zero();
    for (k = 0; k < 3; k++) target += bary[k] * geometry->vertexPositions[faceVerts[k]];
    if (!finiteVector(target)) throw std::invalid_argument("SurfaceAnchorInput target position is not finite");

    Anchor anchor;
    anchor.id = input.id;
    anchor.target = target;
    anchor.maxDistance = input.maxDistance;
    anchor.face = face;
    anchor.bary = bary;
    anchor.verts = faceVerts;
    built.push_back(anchor);
  }

  std::sort(built.begin(), built.end(), [](const Anchor& a, const Anchor& b) { return a.id < b.id; });

  // === Phase 2: registration. Nothing below should throw after the manager pointer is set. ===
  anchors = std::move(built);

  faceIndex = FaceData<std::vector<size_t>>(mesh);
  faceOrigin = FaceData<size_t>(mesh);
  edgeDirty = EdgeData<unsigned char>(mesh);
  for (Face f : mesh.faces()) faceOrigin[f] = f.getIndex();
  for (size_t i = 0; i < anchors.size(); i++) {
    faceIndex[anchors[i].face].push_back(i);
  }

  // mesh.compress() rewrites index spaces; MeshData containers permute themselves, but the
  // face/vertex handles stored in the anchors need an explicit identity-based remap.
  facePermuteCallbackIt = mesh.facePermuteCallbackList.insert(
      mesh.facePermuteCallbackList.end(), [this](const std::vector<size_t>& perm) {
        std::vector<size_t> oldToNew;
        for (size_t newInd = 0; newInd < perm.size(); newInd++) {
          size_t oldInd = perm[newInd];
          if (oldInd >= oldToNew.size()) oldToNew.resize(oldInd + 1, INVALID_IND);
          oldToNew[oldInd] = newInd;
        }
        for (Anchor& a : anchors) {
          size_t newInd = oldToNew.at(a.face.getIndex());
          a.face = Face(&mesh, newInd);
        }
        pendingDirtyList.clear();
      });

  vertexPermuteCallbackIt = mesh.vertexPermuteCallbackList.insert(
      mesh.vertexPermuteCallbackList.end(), [this](const std::vector<size_t>& perm) {
        std::vector<size_t> oldToNew;
        for (size_t newInd = 0; newInd < perm.size(); newInd++) {
          size_t oldInd = perm[newInd];
          if (oldInd >= oldToNew.size()) oldToNew.resize(oldInd + 1, INVALID_IND);
          oldToNew[oldInd] = newInd;
        }
        for (Anchor& a : anchors) {
          for (size_t k = 0; k < 3; k++) {
            a.verts[k] = Vertex(&mesh, oldToNew.at(a.verts[k].getIndex()));
          }
        }
      });

  compressCallbackIt = mesh.compressCallbackList.insert(mesh.compressCallbackList.end(), [this]() {
    // The representative halfedge of a face may point at different halfedge indices after a
    // compress; re-express every barycentric triple in the face's current winding by matching
    // vertex identities, so weights can never silently shift to the wrong corner.
    for (Anchor& a : anchors) {
      std::array<Vertex, 3> currentVerts;
      size_t k = 0;
      for (Vertex v : a.face.adjacentVertices()) currentVerts[k++] = v;

      Vector3 rebuilt = Vector3::zero();
      for (size_t oldSlot = 0; oldSlot < 3; oldSlot++) {
        for (size_t newSlot = 0; newSlot < 3; newSlot++) {
          if (a.verts[oldSlot] == currentVerts[newSlot]) rebuilt[newSlot] = a.bary[oldSlot];
        }
      }
      a.verts = currentVerts;
      a.bary = rebuilt;
    }
    for (Edge e : mesh.edges()) edgeDirty[e] = 0;
    pendingDirtyList.clear();
  });

  manager.surfaceAnchorSet = this;
}

SurfaceAnchorSet::~SurfaceAnchorSet() {
  if (manager.surfaceAnchorSet == this) manager.surfaceAnchorSet = nullptr;
  mesh.facePermuteCallbackList.erase(facePermuteCallbackIt);
  mesh.vertexPermuteCallbackList.erase(vertexPermuteCallbackIt);
  mesh.compressCallbackList.erase(compressCallbackIt);
  // MeshData members detach themselves in their own destructors; caller constraints and
  // policies are untouched.
}

std::vector<SurfaceAnchorSnapshot> SurfaceAnchorSet::snapshot() const {
  std::vector<SurfaceAnchorSnapshot> out;
  out.reserve(anchors.size());
  for (const Anchor& a : anchors) {
    SurfaceAnchorSnapshot s;
    s.id = a.id;
    s.target = a.target;
    s.maxDistance = a.maxDistance;
    s.face = a.face;
    s.barycentric = a.bary;
    s.position = Vector3::zero();
    for (size_t k = 0; k < 3; k++) {
      s.position += a.bary[k] * geometry->vertexPositions[a.verts[k]];
    }
    out.push_back(s);
  }
  return out;
}

void SurfaceAnchorSet::resetCounters() {
  anchorVisitCount = 0;
  projectionTriangleTestCount = 0;
}

void SurfaceAnchorSet::markEdgeDirty(Edge e) {
  if (e == Edge() || e.isDead()) return;
  if (!edgeDirty[e]) {
    edgeDirty[e] = 1;
    pendingDirtyList.push_back(e);
  }
}

void SurfaceAnchorSet::takeDirtyEdges(std::vector<Edge>& out) {
  for (Edge e : pendingDirtyList) {
    if (e != Edge() && !e.isDead()) out.push_back(e);
    if (e != Edge()) edgeDirty[e] = 0;
  }
  pendingDirtyList.clear();
}

bool SurfaceAnchorSet::evaluateCollapse(Edge e, double tCollapse, const Vector3& newVertexPosition) {
  (void)tCollapse;
  tentative.clear();
  pendingSurvivor = Vertex();

  Vertex u = e.halfedge().tailVertex(); // deleted under an interior collapse
  Vertex v = e.halfedge().tipVertex();  // surviving under an interior collapse

  // Anchors keep boundary vertices fixed; no edge touching a boundary vertex may collapse.
  if (e.isBoundary() || u.isBoundary() || v.isBoundary()) return false;
  if (u.isDead() || v.isDead()) return false;

  // === Gather the patch: union of the faces incident on either endpoint. ===
  std::vector<Face> patch;
  {
    std::set<size_t> seen;
    auto gather = [&](Vertex vert) {
      for (Face f : vert.adjacentFaces()) {
        if (seen.insert(f.getIndex()).second) patch.push_back(f);
      }
    };
    gather(u);
    gather(v);
  }
  if (patch.empty()) return false;

  // === Link condition: common neighbors of u and v must be exactly the vertices opposite e. ===
  std::set<size_t> neighborsU;
  for (Vertex w : u.adjacentVertices()) neighborsU.insert(w.getIndex());
  std::set<size_t> neighborsV;
  for (Vertex w : v.adjacentVertices()) neighborsV.insert(w.getIndex());

  std::set<size_t> opposite;
  for (Face f : e.adjacentFaces()) {
    for (Vertex w : f.adjacentVertices()) {
      if (w != u && w != v) {
        if (!opposite.insert(w.getIndex()).second) return false; // duplicated opposite vertex
      }
    }
  }
  if (opposite.size() != 2) return false; // interior edge must have two distinct opposites
  for (size_t w : neighborsU) {
    if (neighborsV.count(w) && !opposite.count(w)) return false; // link violation / duplicate face
  }

  // === Simulate the topology: faces with both endpoints die, the rest replace an endpoint. ===
  std::vector<SimTriangle> sim;
  sim.reserve(patch.size());
  for (Face f : patch) {
    std::array<Vertex, 3> fv;
    size_t k = 0;
    for (Vertex w : f.adjacentVertices()) fv[k++] = w;

    bool hasU = false, hasV = false;
    for (k = 0; k < 3; k++) {
      if (fv[k] == u) hasU = true;
      if (fv[k] == v) hasV = true;
    }
    if (hasU && hasV) continue; // deleted face

    SimTriangle tri;
    tri.face = f;
    tri.origin = faceOrigin[f];
    for (k = 0; k < 3; k++) {
      Vertex w = fv[k];
      tri.verts[k] = (w == u || w == v) ? v : w;
      tri.pos[k] = (w == u || w == v) ? newVertexPosition : geometry->vertexPositions[w];
    }
    // Defensive: a collapsed triangle would be degenerate/non-manifold.
    if (tri.verts[0] == tri.verts[1] || tri.verts[1] == tri.verts[2] || tri.verts[0] == tri.verts[2]) {
      return false;
    }
    sim.push_back(tri);
  }
  if (sim.empty()) return false;

  // Two surviving triangles with the same vertex set would be a duplicated (non-manifold) face.
  for (size_t i = 0; i < sim.size(); i++) {
    std::set<size_t> si{sim[i].verts[0].getIndex(), sim[i].verts[1].getIndex(), sim[i].verts[2].getIndex()};
    if (si.size() != 3) return false;
    for (size_t j = i + 1; j < sim.size(); j++) {
      std::set<size_t> sj{sim[j].verts[0].getIndex(), sim[j].verts[1].getIndex(), sim[j].verts[2].getIndex()};
      if (si == sj) return false;
    }
  }

  // === Geometric validity: positive area and consistent oriented normal. ===
  for (const SimTriangle& tri : sim) {
    Vector3 oldA = geometry->vertexPositions[tri.face.halfedge().vertex()];
    Vector3 oldB = geometry->vertexPositions[tri.face.halfedge().next().vertex()];
    Vector3 oldC = geometry->vertexPositions[tri.face.halfedge().next().next().vertex()];
    Vector3 oldNormal = cross(oldB - oldA, oldC - oldA);
    Vector3 newNormal = cross(tri.pos[1] - tri.pos[0], tri.pos[2] - tri.pos[0]);
    if (newNormal.norm() <= 0.) return false;
    if (dot(oldNormal, newNormal) <= 0.) return false;
  }

  // === Re-project every anchor attached to the patch from its immutable target. ===
  std::vector<size_t> affected;
  for (Face f : patch) {
    for (size_t idx : faceIndex[f]) affected.push_back(idx);
  }
  anchorVisitCount += affected.size();

  for (size_t idx : affected) {
    const Anchor& a = anchors[idx];
    double bestDist = std::numeric_limits<double>::infinity();
    size_t bestTri = 0;
    Vector3 bestBary = Vector3::zero();

    for (size_t ti = 0; ti < sim.size(); ti++) {
      const SimTriangle& tri = sim[ti];
      projectionTriangleTestCount++;
      Vector3 bary = closestPointOnTriangleBary(a.target, tri.pos[0], tri.pos[1], tri.pos[2]);
      Vector3 closest = bary[0] * tri.pos[0] + bary[1] * tri.pos[1] + bary[2] * tri.pos[2];
      double dist = (a.target - closest).norm();
      if (!std::isfinite(bestDist)) {
        bestDist = dist;
        bestTri = ti;
        bestBary = bary;
        continue;
      }
      // Treat machine-noise-level equal distances as a tie, resolved by initial face origin.
      double tieEps = 1e-12 * std::max({1., dist, bestDist});
      bool better = dist < bestDist - tieEps;
      bool tie = std::abs(dist - bestDist) <= tieEps && tri.origin < sim[bestTri].origin;
      if (better || tie) {
        bestDist = dist;
        bestTri = ti;
        bestBary = bary;
      }
    }

    if (!(bestDist <= a.maxDistance)) return false; // budget failure (NaN also rejected)

    TentativeAnchor ta;
    ta.anchorIndex = idx;
    ta.face = sim[bestTri].face;
    ta.verts = sim[bestTri].verts;
    ta.bary = bestBary;
    ta.position = bestBary[0] * sim[bestTri].pos[0] + bestBary[1] * sim[bestTri].pos[1] +
                  bestBary[2] * sim[bestTri].pos[2];
    ta.distance = bestDist;
    tentative.push_back(ta);
  }

  pendingSurvivor = v;
  return true;
}

void SurfaceAnchorSet::commitCollapse() {
  // The real collapse already rewired the mesh; the simulated faces keep their handles, only
  // their vertices were redirected to the survivor. Publish anchor state before any observer runs.
  std::set<size_t> touchedFaces;
  for (const TentativeAnchor& ta : tentative) {
    touchedFaces.insert(anchors[ta.anchorIndex].face.getIndex());
    touchedFaces.insert(ta.face.getIndex());
  }
  for (size_t fi : touchedFaces) faceIndex[Face(&mesh, fi)].clear();

  for (const TentativeAnchor& ta : tentative) {
    Anchor& a = anchors[ta.anchorIndex];
    a.face = ta.face;
    a.verts = ta.verts;
    a.bary = ta.bary;

    // A surviving face can get a new representative halfedge during the collapse, rotating its
    // winding even without compress(). Re-express the weights in the face's current winding by
    // matching vertex identities; weights never move to a different geometric corner.
    std::array<Vertex, 3> currentVerts;
    size_t k = 0;
    for (Vertex w : a.face.adjacentVertices()) currentVerts[k++] = w;
    Vector3 rewound = Vector3::zero();
    for (size_t oldSlot = 0; oldSlot < 3; oldSlot++) {
      for (size_t newSlot = 0; newSlot < 3; newSlot++) {
        if (a.verts[oldSlot] == currentVerts[newSlot]) rewound[newSlot] = a.bary[oldSlot];
      }
    }
    a.verts = currentVerts;
    a.bary = rewound;

    faceIndex[a.face].push_back(ta.anchorIndex);
  }

  // The merged star (u's halfedges were rewired onto the survivor) holds every edge whose anchor
  // feasibility or quadric neighborhood may have changed. Report them to the constraint channel.
  if (pendingSurvivor != Vertex()) {
    for (Edge ne : pendingSurvivor.adjacentEdges()) markEdgeDirty(ne);
  }
  pendingSurvivor = Vertex();
  tentative.clear();

  if (geometry != nullptr) geometry->refreshQuantities();
}

} // namespace surface
} // namespace geometrycentral
