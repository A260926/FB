// Gmsh - Copyright (C) 1997-2026 C. Geuzaine, J.-F. Remacle
//
// See the LICENSE.txt file in the Gmsh root directory for license information.
// Please report all issues on https://gitlab.onelab.info/gmsh/gmsh/issues.

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "GModelPeriodic.h"
#include "GmshMessage.h"
#include "GModel.h"
#include "GEntity.h"
#include "GVertex.h"
#include "GEdge.h"
#include "GFace.h"
#include "SPoint3.h"
#include "MVertex.h"
#include "MElement.h"
#include "MLine.h"
#include "MTriangle.h"
#include "MQuadrangle.h"
#include "MEdge.h"
#include "MFace.h"

namespace gmshPeriodic {

namespace {

typedef std::map<MVertex *, MVertex *> vertexMap;

// The nodes used by the elements of an entity, and those of them that are a
// corner (primary vertex) of one of these elements.
struct EntityRoles {
  std::unordered_set<MVertex *> used;
  std::unordered_set<MVertex *> primary;
};

void entityRoles(GEntity *ge, EntityRoles &roles)
{
  for(std::size_t i = 0; i < ge->getNumMeshElements(); i++) {
    MElement *e = ge->getMeshElement(i);
    const std::size_t n = e->getNumVertices();
    const std::size_t nc = e->getNumPrimaryVertices();
    for(std::size_t j = 0; j < n; j++) roles.used.insert(e->getVertex(j));
    for(std::size_t j = 0; j < nc; j++) roles.primary.insert(e->getVertex(j));
  }
}

MVertex *mappedIn(const vertexMap &lo, const vertexMap &hi, MVertex *v)
{
  auto it = lo.find(v);
  if(it != lo.end()) return it->second;
  it = hi.find(v);
  if(it != hi.end()) return it->second;
  return nullptr;
}

void insertPair(vertexMap &lo, vertexMap &hi,
                const std::unordered_set<MVertex *> &primary, MVertex *slave,
                MVertex *master)
{
  // the two maps must stay disjoint
  if(primary.count(slave) && primary.count(master)) {
    hi.erase(slave);
    lo[slave] = master;
  }
  else {
    lo.erase(slave);
    hi[slave] = master;
  }
}

SPoint3 lerp(const SPoint3 &a, const SPoint3 &b, double t)
{
  return SPoint3(a.x() + (b.x() - a.x()) * t, a.y() + (b.y() - a.y()) * t,
                 a.z() + (b.z() - a.z()) * t);
}

SPoint3 combine(const SPoint3 &a, double wa, const SPoint3 &b, double wb)
{
  return SPoint3(a.x() * wa + b.x() * wb, a.y() * wa + b.y() * wb,
                 a.z() * wa + b.z() * wb);
}

// The local reference coordinates of the nodes of an element, as given by its
// function space (never by the coordinates of the nodes in space).
void referenceCoordinates(MElement *e, std::vector<SPoint3> &pts)
{
  const std::size_t n = e->getNumVertices();
  pts.resize(n);
  for(std::size_t i = 0; i < n; i++) {
    double u = 0., v = 0., w = 0.;
    e->getNode((int)i, u, v, w);
    pts[i] = SPoint3(u, v, w);
  }
}

// Map a reference point of the slave element to the reference frame of the
// master element through the correspondence of their corners. The reference
// elements are the standard Gmsh ones (their corners are at the same
// reference positions whatever the order), so a line uses the 1D affine map,
// a triangle the barycentric one and a quadrangle the bilinear map of the
// square.
bool referenceImage(const std::vector<SPoint3> &slavePts,
                    const std::vector<SPoint3> &masterPts,
                    const std::vector<int> &cornerToMasterCorner,
                    const SPoint3 &p, SPoint3 &q, int dim, int nCorner)
{
  if(dim == 1) {
    const SPoint3 &s0 = slavePts[0], &s1 = slavePts[1];
    const SPoint3 &m0 = masterPts[cornerToMasterCorner[0]];
    const SPoint3 &m1 = masterPts[cornerToMasterCorner[1]];
    const double d = s1.x() - s0.x();
    if(std::abs(d) < 1e-14) return false;
    const double t = (p.x() - s0.x()) / d;
    q = lerp(m0, m1, t);
    return true;
  }
  if(nCorner == 3) {
    const SPoint3 &s0 = slavePts[0], &s1 = slavePts[1], &s2 = slavePts[2];
    const double a1 = s1.x() - s0.x(), b1 = s1.y() - s0.y();
    const double a2 = s2.x() - s0.x(), b2 = s2.y() - s0.y();
    const double det = a1 * b2 - a2 * b1;
    if(std::abs(det) < 1e-14) return false;
    const double dx = p.x() - s0.x(), dy = p.y() - s0.y();
    const double u = (dx * b2 - dy * a2) / det;
    const double v = (dy * a1 - dx * b1) / det;
    const SPoint3 &m0 = masterPts[cornerToMasterCorner[0]];
    const SPoint3 &m1 = masterPts[cornerToMasterCorner[1]];
    const SPoint3 &m2 = masterPts[cornerToMasterCorner[2]];
    const SPoint3 a = lerp(m0, m1, u);
    const SPoint3 b = lerp(m0, m2, v);
    q = SPoint3(a.x() + b.x() - m0.x(), a.y() + b.y() - m0.y(),
                a.z() + b.z() - m0.z());
    return true;
  }
  else if(nCorner == 4) {
    // inverse bilinear map on the reference square: find (a, b) such that
    // p = c0 + a (c1 - c0) + b (c3 - c0) + a b (c2 - c3 - c1 + c0)
    const SPoint3 &c0 = slavePts[0], &c1 = slavePts[1], &c2 = slavePts[2],
                  &c3 = slavePts[3];
    const SPoint3 e1 = c1 - c0, e3 = c3 - c0, e2 = c2 - c3 - c1 + c0;
    double a = 0.5, b = 0.5;
    for(int it = 0; it < 40; it++) {
      const SPoint3 ab = combine(e2, a * b, p, -1.);
      const SPoint3 f = SPoint3(c0.x() + e1.x() * a + e3.x() * b + ab.x(),
                                c0.y() + e1.y() * a + e3.y() * b + ab.y(),
                                c0.z() + e1.z() * a + e3.z() * b + ab.z());
      if(f.distance(SPoint3(0., 0., 0.)) < 1e-14) break;
      const SPoint3 d1 = combine(e2, b, e1, 1.);
      const SPoint3 d2 = combine(e2, a, e3, 1.);
      const double det = d1.x() * d2.y() - d2.x() * d1.y();
      if(std::abs(det) < 1e-14) return false;
      a -= (f.x() * d2.y() - f.y() * d2.x()) / det;
      b -= (d1.x() * f.y() - d1.y() * f.x()) / det;
    }
    const SPoint3 &m0 = masterPts[cornerToMasterCorner[0]];
    const SPoint3 &m1 = masterPts[cornerToMasterCorner[1]];
    const SPoint3 &m2 = masterPts[cornerToMasterCorner[2]];
    const SPoint3 &m3 = masterPts[cornerToMasterCorner[3]];
    const SPoint3 lo = lerp(m0, m1, a), hi = lerp(m3, m2, a);
    q = lerp(lo, hi, b);
    return true;
  }
  return false;
}

bool localPermutation(MElement *slave, MElement *master,
                      const std::vector<int> &cornerToMasterCorner,
                      std::vector<int> &perm)
{
  const int dim = slave->getDim();
  const int nCorner = (int)slave->getNumPrimaryVertices();
  std::vector<SPoint3> slavePts, masterPts;
  referenceCoordinates(slave, slavePts);
  referenceCoordinates(master, masterPts);
  const std::size_t n = slave->getNumVertices();
  if(master->getNumVertices() != n) return false;
  perm.assign(n, -1);
  std::vector<char> taken(n, 0);
  for(std::size_t i = 0; i < n; i++) {
    SPoint3 q;
    if(!referenceImage(slavePts, masterPts, cornerToMasterCorner, slavePts[i],
                       q, dim, nCorner))
      return false;
    int best = -1;
    double bestDist = 0.;
    for(std::size_t j = 0; j < n; j++) {
      const double d = masterPts[j].distance(q);
      if(best < 0 || d < bestDist) {
        best = (int)j;
        bestDist = d;
      }
    }
    // the reference nodes of the same basis are distinct: this only accepts
    // the exact node of the master reference element
    if(best < 0 || bestDist > 1e-8 || taken[best]) return false;
    taken[best] = 1;
    perm[i] = best;
  }
  return true;
}

// The master elements of a curve or a surface, by the corners of their
// elements (independent of their node ordering)
struct MasterElements {
  std::map<MEdge, MElement *, MEdgeLessThan> lines;
  std::map<MFace, MElement *, MFaceLessThan> faces;
};

void collectMasterElements(GEntity *master, MasterElements &m)
{
  for(std::size_t i = 0; i < master->getNumMeshElements(); i++) {
    MElement *e = master->getMeshElement(i);
    const int nc = (int)e->getNumPrimaryVertices();
    if(nc == 2)
      m.lines[MEdge(e->getVertex(0), e->getVertex(1))] = e;
    else if(nc == 3)
      m.faces[MFace(e->getVertex(0), e->getVertex(1), e->getVertex(2))] = e;
    else if(nc == 4)
      m.faces[MFace(e->getVertex(0), e->getVertex(1), e->getVertex(2),
                    e->getVertex(3))] = e;
  }
}

MElement *findMasterElement(MasterElements &masterElements, int nc,
                            const std::vector<MVertex *> &corners)
{
  if(nc == 2) {
    auto it = masterElements.lines.find(MEdge(corners[0], corners[1]));
    return it == masterElements.lines.end() ? nullptr : it->second;
  }
  if(nc == 3) {
    auto it =
      masterElements.faces.find(MFace(corners[0], corners[1], corners[2]));
    return it == masterElements.faces.end() ? nullptr : it->second;
  }
  if(nc == 4) {
    auto it = masterElements.faces.find(
      MFace(corners[0], corners[1], corners[2], corners[3]));
    return it == masterElements.faces.end() ? nullptr : it->second;
  }
  return nullptr;
}

// The master of a node: what its boundary entity says if that entity is
// itself periodic (a node shared by two entities must be paired the same way
// everywhere); a node that the master elements use as well (e.g. a node of a
// geometric edge shared by the two surfaces) is paired with itself; otherwise
// what the maps of the entity say.
MVertex *masterOf(GEntity *ge, MVertex *v,
                  const std::unordered_set<MVertex *> &masterUsed)
{
  GEntity *owner = v->onWhat();
  if(owner && owner != ge && owner->getMeshMaster() != owner) {
    MVertex *m =
      mappedIn(owner->correspondingVertices,
               owner->correspondingHighOrderVertices, v);
    if(m) return m;
  }
  MVertex *m = mappedIn(ge->correspondingVertices,
                        ge->correspondingHighOrderVertices, v);
  if(m) return m;
  // a node of a boundary entity shared geometrically by the two surfaces (the
  // same object, not a copy) is the image of itself. Only a boundary node can
  // be shared this way: an interior node of the slave belongs to the slave
  // surface, and pairing it with itself would hide a missing correspondence.
  if(masterUsed.count(v) && v->onWhat() && v->onWhat() != ge &&
     v->onWhat()->dim() < ge->dim())
    return v;
  return nullptr;
}

// Rebuild the correspondence of a periodic curve or surface from its known
// corner correspondence and the local reference positions of the nodes of its
// elements. Pairs whose nodes are not (or no longer) used by the elements of
// the model are dropped; no pair is added for a node the entity does not use.
void rebuildEntity(GEntity *ge, GEntity *master,
                   const std::unordered_set<MVertex *> &used,
                   const std::unordered_set<MVertex *> &primary,
                   const std::unordered_set<MVertex *> &owned,
                   const std::unordered_set<MVertex *> &localUsed,
                   const std::unordered_set<MVertex *> &masterUsed)
{
  vertexMap lo, hi;
  auto keep = [&](const vertexMap &src) {
    for(auto &p : src) {
      MVertex *s = p.first, *m = p.second;
      if(!s || !m) continue;
      // the key is a node of this model entity; both nodes must be owned by
      // the model and used by its elements
      if(!localUsed.count(s)) continue;
      if(!owned.count(s) || !owned.count(m)) continue;
      if(!used.count(s) || !used.count(m)) continue;
      insertPair(lo, hi, primary, s, m);
    }
  };
  keep(ge->correspondingVertices);
  keep(ge->correspondingHighOrderVertices);

  if(ge->getNumMeshElements() && master->getNumMeshElements()) {
    MasterElements masterElements;
    collectMasterElements(master, masterElements);
    for(std::size_t i = 0; i < ge->getNumMeshElements(); i++) {
      MElement *slaveEl = ge->getMeshElement(i);
      const int nc = (int)slaveEl->getNumPrimaryVertices();
      std::vector<MVertex *> mappedCorners(nc, nullptr);
      bool complete = true;
      for(int j = 0; j < nc; j++) {
        mappedCorners[j] = masterOf(ge, slaveEl->getVertex(j), masterUsed);
        if(!mappedCorners[j]) complete = false;
      }
      if(!complete) continue;
      MElement *masterEl = findMasterElement(masterElements, nc, mappedCorners);
      if(!masterEl ||
         (int)masterEl->getNumPrimaryVertices() != nc) continue;
      std::vector<int> cornerToMasterCorner(nc, -1);
      std::vector<char> usedCorner(nc, 0);
      bool cornersOK = true;
      for(int j = 0; j < nc; j++) {
        for(int k = 0; k < nc; k++) {
          if(masterEl->getVertex(k) == mappedCorners[j] && !usedCorner[k]) {
            cornerToMasterCorner[j] = k;
            usedCorner[k] = 1;
            break;
          }
        }
        if(cornerToMasterCorner[j] < 0) cornersOK = false;
      }
      if(!cornersOK) continue;
      std::vector<int> perm;
      if(!localPermutation(slaveEl, masterEl, cornerToMasterCorner, perm))
        continue;
      for(std::size_t j = 0; j < slaveEl->getNumVertices(); j++) {
        MVertex *s = slaveEl->getVertex((int)j);
        if(!localUsed.count(s)) continue;
        MVertex *m = masterEl->getVertex(perm[j]);
        // a node shared with a periodic boundary entity is paired as that
        // entity pairs it, so that both entities give the same answer
        GEntity *owner = s->onWhat();
        if(owner && owner != ge && owner->getMeshMaster() != owner) {
          MVertex *authoritative =
            mappedIn(owner->correspondingVertices,
                     owner->correspondingHighOrderVertices, s);
          if(authoritative) m = authoritative;
        }
        if(!m || !owned.count(m) || !used.count(m)) continue;
        insertPair(lo, hi, primary, s, m);
      }
    }
  }

  ge->correspondingVertices.swap(lo);
  ge->correspondingHighOrderVertices.swap(hi);
}

void collectRoles(GModel *model, std::unordered_set<MVertex *> &used,
                  std::unordered_set<MVertex *> &primary,
                  std::unordered_set<MVertex *> &owned,
                  std::unordered_map<GEntity *, EntityRoles> *perEntity)
{
  std::vector<GEntity *> entities;
  model->getEntities(entities);
  for(GEntity *ge : entities) {
    EntityRoles roles;
    entityRoles(ge, roles);
    used.insert(roles.used.begin(), roles.used.end());
    primary.insert(roles.primary.begin(), roles.primary.end());
    for(MVertex *v : ge->mesh_vertices) owned.insert(v);
    if(perEntity) (*perEntity)[ge] = std::move(roles);
  }
}

} // namespace

void getRoles(GModel *model, std::unordered_set<MVertex *> &used,
              std::unordered_set<MVertex *> &primary)
{
  std::unordered_set<MVertex *> owned;
  collectRoles(model, used, primary, owned, nullptr);
}

void getUsedNodes(GModel *model, std::unordered_set<MVertex *> &used)
{
  std::vector<GEntity *> entities;
  model->getEntities(entities);
  for(GEntity *ge : entities) {
    for(std::size_t i = 0; i < ge->getNumMeshElements(); i++) {
      MElement *e = ge->getMeshElement(i);
      for(std::size_t j = 0; j < e->getNumVertices(); j++)
        used.insert(e->getVertex(j));
    }
  }
}

void removeFromMaps(GModel *model, const std::unordered_set<MVertex *> &dead)
{
  std::vector<GEntity *> entities;
  model->getEntities(entities);
  for(GEntity *ge : entities) {
    auto prune = [&](vertexMap &m) {
      for(auto it = m.begin(); it != m.end();) {
        if(dead.count(it->first) || dead.count(it->second))
          it = m.erase(it);
        else
          ++it;
      }
    };
    prune(ge->correspondingVertices);
    prune(ge->correspondingHighOrderVertices);
  }
}

bool rebuildAll(GModel *model)
{
  std::unordered_set<MVertex *> used, primary, owned;
  std::unordered_map<GEntity *, EntityRoles> perEntity;
  collectRoles(model, used, primary, owned, &perEntity);

  std::vector<GEntity *> entities;
  model->getEntities(entities);
  // points, then curves, then surfaces: a boundary entity pairs its shared
  // nodes before the higher dimensional entities borrow them
  for(int dim = 0; dim <= 2; dim++) {
    for(GEntity *ge : entities) {
      if(ge->dim() != dim) continue;
      if(ge->getMeshMaster() == ge) continue;
      GEntity *master = ge->getMeshMaster();
      if(!master) continue;
      const std::unordered_set<MVertex *> &localUsed = perEntity[ge].used;
      if(dim != 1 && dim != 2) {
        // points: keep the known point-to-point correspondence, given by the
        // corner (mesh point) of the entity
        vertexMap oldLo, oldHi;
        oldLo.swap(ge->correspondingVertices);
        oldHi.swap(ge->correspondingHighOrderVertices);
        vertexMap lo, hi;
        auto add = [&](const vertexMap &src) {
          for(auto &p : src) {
            MVertex *s = p.first, *m = p.second;
            if(!s || !m) continue;
            if(!localUsed.count(s)) continue;
            if(!owned.count(s) || !owned.count(m)) continue;
            if(!used.count(m)) continue;
            // a mesh point is a corner of its point element
            lo[s] = m;
          }
        };
        add(oldLo);
        add(oldHi);
        ge->correspondingVertices.swap(lo);
        ge->correspondingHighOrderVertices.swap(hi);
      }
      else {
        const std::unordered_set<MVertex *> &masterUsed =
          perEntity[master].used;
        rebuildEntity(ge, master, used, primary, owned, localUsed, masterUsed);
      }
    }
  }
  return true;
}

bool localNodePermutation(MElement *slave, MElement *master,
                          const std::map<MVertex *, MVertex *> &cornerPairs,
                          std::vector<int> &perm)
{
  const int nc = (int)slave->getNumPrimaryVertices();
  if((int)master->getNumPrimaryVertices() != nc) return false;
  std::vector<int> cornerToMasterCorner(nc, -1);
  for(int j = 0; j < nc; j++) {
    auto it = cornerPairs.find(slave->getVertex(j));
    if(it == cornerPairs.end()) return false;
    for(int k = 0; k < nc; k++)
      if(master->getVertex(k) == it->second) {
        cornerToMasterCorner[j] = k;
        break;
      }
    if(cornerToMasterCorner[j] < 0) return false;
  }
  return localPermutation(slave, master, cornerToMasterCorner, perm);
}

} // namespace gmshPeriodic
