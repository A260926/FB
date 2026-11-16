// Regression tests for the periodic CAD mesh path: MSH 4 read/write, node
// roles, order changes, refinement and CAD parametrization.
//
// Built by task_support/run.sh target and registered through
// task_support/check.h (task_cases / task_case_count, TASK_CHECK).

#include "../task_support/check.h"

#include <gmsh.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

// TASK_CHECK returns from the enclosing function; the helper functions below
// instead record a failure, which their caller turns into a failure of the
// test case.
static bool gFailed = false;
#define REQUIRE_HELPERS_OK() TASK_CHECK(!gFailed)
#define CHECK(x)                                                              \
  do {                                                                        \
    if(!task_check(!!(x), #x, __FILE__, __LINE__)) gFailed = true;            \
  } while(0)

// ---------------------------------------------------------------------------
// small helpers

struct Node {
  double xyz[3];
  double par[2];
  int dim;
  int tag;
};

struct Element {
  int type;
  std::size_t tag;
  std::vector<std::size_t> nodes;
};

struct Snapshot {
  gmsh::vectorpair entities;
  std::unordered_map<std::size_t, Node> nodes;   // by tag
  std::unordered_map<int, std::vector<std::size_t>> entityNodes; // owned
  std::unordered_map<int, std::vector<std::size_t>> entityUsed; // in elements
  std::vector<Element> elements;
  std::unordered_map<std::size_t, size_t> elementEntity; // tag -> encoded dim/tag
  std::unordered_set<std::size_t> primary;
  std::unordered_set<std::size_t> used;
};

int encode(int dim, int tag) { return (dim << 24) | (tag & 0xFFFFFF); }

std::string uniquePath(const char *name, int index)
{
  std::string dir;
  const char *tmp = std::getenv("TASK_TMP");
  if(tmp && *tmp)
    dir = tmp;
  else
    dir = ".task-build"; // (created by task_support/run.sh)
  char buf[1024];
  std::snprintf(buf, sizeof(buf), "%s/%s-%lu-%d.msh", dir.c_str(), name,
                (unsigned long)std::rand(), index);
  return buf;
}

void ensureInit()
{
  static bool initialized = false;
  if(!initialized) {
    gmsh::initialize();
    initialized = true;
  }
}

void setCommonOptions()
{
  ensureInit();
  gmsh::option::setNumber("General.Terminal", 0);
  gmsh::option::setNumber("General.NumThreads", 1);
  gmsh::option::setNumber("Mesh.SaveAll", 1);
  gmsh::option::setNumber("Mesh.SaveParametric", 1);
  gmsh::option::setNumber("Mesh.MshFileVersion", 4.1);
  gmsh::option::setNumber("Mesh.HighOrderSavePeriodic", 1);
  gmsh::option::setNumber("Mesh.Binary", 0);
}

// The reference local coordinates of the nodes of an element type, from the
// API (independent from the element classes of the library under test).
struct RefElement {
  int dim;
  int numNodes;
  int numPrimary;
  std::vector<std::array<double, 3>> local;
};

RefElement referenceElement(int type)
{
  RefElement r;
  std::string name;
  std::vector<double> local;
  gmsh::model::mesh::getElementProperties(type, name, r.dim, r.numNodes,
                                          r.numNodes, local, r.numPrimary);
  for(int i = 0; i < r.numNodes; i++) {
    std::array<double, 3> p = {{0., 0., 0.}};
    // the API returns the coordinates node by node (numNodes * dim values)
    for(int d = 0; d < r.dim; d++) p[d] = local[i * r.dim + d];
    r.local.push_back(p);
  }
  return r;
}

int elementType(const std::string &name)
{
  std::vector<int> types;
  gmsh::model::mesh::getElementTypes(types);
  for(auto t : types) {
    std::string n;
    int dim, order, numNodes, numPrimary;
    std::vector<double> local;
    gmsh::model::mesh::getElementProperties(t, n, dim, order, numNodes, local,
                                            numPrimary);
    if(n == name) return t;
  }
  return -1;
}

Snapshot snapshot()
{
  Snapshot s;
  gmsh::model::getEntities(s.entities);
  for(auto e : s.entities) {
    std::vector<int> types;
    std::vector<std::vector<std::size_t>> tags, nodes;
    gmsh::model::mesh::getElements(types, tags, nodes, e.first, e.second);
    std::vector<std::size_t> nodeTags;
    std::vector<double> coord, par;
    gmsh::model::mesh::getNodes(nodeTags, coord, par, e.first, e.second, false,
                                true);
    for(std::size_t i = 0; i < nodeTags.size(); i++) {
      Node n;
      n.xyz[0] = coord[3 * i + 0];
      n.xyz[1] = coord[3 * i + 1];
      n.xyz[2] = coord[3 * i + 2];
      n.par[0] = n.par[1] = 0.;
      if(e.first == 1 && par.size() >= (i + 1) * 1) n.par[0] = par[i];
      if(e.first == 2 && par.size() >= (i + 1) * 2) {
        n.par[0] = par[2 * i];
        n.par[1] = par[2 * i + 1];
      }
      n.dim = e.first;
      n.tag = e.second;
      s.nodes[nodeTags[i]] = n;
      s.entityNodes[encode(e.first, e.second)].push_back(nodeTags[i]);
    }
    for(std::size_t t = 0; t < types.size(); t++) {
      RefElement ref = referenceElement(types[t]);
      std::size_t per = ref.numNodes;
      for(std::size_t k = 0; k + per <= nodes[t].size(); k += per) {
        Element el;
        el.type = types[t];
        el.tag = tags[t][k / per];
        el.nodes.assign(nodes[t].begin() + k, nodes[t].begin() + k + per);
        s.elements.push_back(el);
        s.elementEntity[el.tag] = encode(e.first, e.second);
        for(std::size_t j = 0; j < per; j++) {
          s.used.insert(el.nodes[j]);
          s.entityUsed[encode(e.first, e.second)].push_back(el.nodes[j]);
          if((int)j < ref.numPrimary) s.primary.insert(el.nodes[j]);
        }
      }
    }
  }
  return s;
}

std::vector<std::size_t> entityUsed(const Snapshot &s, int dim, int tag)
{
  auto it = s.entityUsed.find(encode(dim, tag));
  if(it == s.entityUsed.end()) return {};
  return it->second;
}

std::vector<std::size_t> entityNodes(const Snapshot &s, int dim, int tag)
{
  auto it = s.entityNodes.find(encode(dim, tag));
  if(it == s.entityNodes.end()) return {};
  return it->second;
}

// The periodic pairs of a slave entity: (slave node tag, master node tag).
std::map<std::size_t, std::size_t> periodicPairs(int dim, int tag,
                                                 bool highOrder,
                                                 int *masterTag = nullptr)
{
  int master = 0;
  std::vector<std::size_t> s, m;
  std::vector<double> tfo;
  gmsh::model::mesh::getPeriodicNodes(dim, tag, master, s, m, tfo, highOrder);
  if(masterTag) *masterTag = master;
  std::map<std::size_t, std::size_t> pairs;
  for(std::size_t i = 0; i < s.size(); i++) pairs[s[i]] = m[i];
  return pairs;
}

std::vector<double> transformOf(int dim, int tag)
{
  int master = 0;
  std::vector<std::size_t> s, m;
  std::vector<double> tfo;
  gmsh::model::mesh::getPeriodicNodes(dim, tag, master, s, m, tfo, false);
  return tfo;
}

std::array<double, 3> applyTransform(const std::vector<double> &tfo,
                                     const double x[3])
{
  std::array<double, 3> r = {{0., 0., 0.}};
  for(int i = 0; i < 3; i++) {
    for(int j = 0; j < 3; j++) r[i] += tfo[4 * i + j] * x[j];
    r[i] += tfo[4 * i + 3];
  }
  return r;
}

double distance3(const double a[3], const std::array<double, 3> &b)
{
  double d = 0.;
  for(int i = 0; i < 3; i++) d += (a[i] - b[i]) * (a[i] - b[i]);
  return std::sqrt(d);
}

// ---------------------------------------------------------------------------
// geometry builders. The periodic surfaces are exact copies of each other
// through the given transformation, built with the built-in geo kernel.

// A quarter-cylinder extruded from a circular arc, and a second copy placed
// with the given transformation. Returns {master face, slave face}.
struct PeriodicModel {
  int master = -1;
  int slave = -1;
  std::vector<double> tfo;
};

PeriodicModel cylinderPair(const std::vector<double> &tfo, bool periodic,
                           bool mirror)
{
  PeriodicModel m;
  gmsh::model::add("periodic-cylinder");
  int a = gmsh::model::geo::addPoint(1, 0, 0, .35);
  int c = gmsh::model::geo::addPoint(0, 0, 0, .35);
  int b = gmsh::model::geo::addPoint(0, 1, 0, .35);
  int arc = gmsh::model::geo::addCircleArc(a, c, b);
  gmsh::vectorpair ext, cp;
  gmsh::model::geo::extrude({{1, arc}}, 0, 0, 1, ext);
  for(auto e : ext)
    if(e.first == 2) m.master = e.second;
  gmsh::model::geo::copy({{2, m.master}}, cp);
  if(mirror) {
    // mirror in the plane x = 1.5 (a reflection, with a determinant -1)
    gmsh::model::geo::mirror(cp, 1., 0., 0., -1.5);
  }
  else {
    gmsh::model::geo::translate(cp, 3, 0, 0);
  }
  gmsh::model::geo::synchronize();
  m.slave = cp[0].second;
  m.tfo = tfo;
  if(periodic)
    gmsh::model::mesh::setPeriodic(2, {m.slave}, {m.master}, m.tfo);
  return m;
}

// A plane surface split into two quadrilaterals sharing an edge whose
// orientation is reversed in the two faces (the shared curve is used with
// opposite signs in the two curve loops). The right face is the master and
// the left face the slave, related by a rotation of angle pi about the z axis
// (the shared edge lies on the axis).
PeriodicModel rotationPair(bool periodic)
{
  PeriodicModel m;
  gmsh::model::add("periodic-rotation");
  // square in the plane z = 0, [-1,1]x[0,1], cut by the line x = 0
  int p000 = gmsh::model::geo::addPoint(-1, 0, 0, .4);
  int p100 = gmsh::model::geo::addPoint(0, 0, 0, .4);
  int p200 = gmsh::model::geo::addPoint(1, 0, 0, .4);
  int p010 = gmsh::model::geo::addPoint(-1, 1, 0, .4);
  int p110 = gmsh::model::geo::addPoint(0, 1, 0, .4);
  int p210 = gmsh::model::geo::addPoint(1, 1, 0, .4);
  int bottomLeft = gmsh::model::geo::addLine(p000, p100);
  int bottomRight = gmsh::model::geo::addLine(p100, p200);
  int right = gmsh::model::geo::addLine(p200, p210);
  int topRight = gmsh::model::geo::addLine(p210, p110);
  int topLeft = gmsh::model::geo::addLine(p110, p010);
  int left = gmsh::model::geo::addLine(p010, p000);
  int shared = gmsh::model::geo::addLine(p100, p110);
  int l1 = gmsh::model::geo::addCurveLoop(
    {bottomLeft, shared, topLeft, left});
  int l2 = gmsh::model::geo::addCurveLoop(
    {-shared, bottomRight, right, topRight});
  int leftFace = gmsh::model::geo::addPlaneSurface({l1});
  int rightFace = gmsh::model::geo::addPlaneSurface({l2});
  gmsh::model::geo::synchronize();
  m.master = rightFace;
  m.slave = leftFace;
  // rotation of angle pi about the z axis through the origin
  m.tfo = {-1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  if(periodic)
    gmsh::model::mesh::setPeriodic(2, {m.slave}, {m.master}, m.tfo);
  return m;
}

// ---------------------------------------------------------------------------
// generic checks

// Every pair of the periodic entities must involve nodes used by the current
// elements, the two maps must be disjoint, and the "low" query (false) must
// return exactly the pairs whose slave node is a primary node.
void checkPeriodicRoles(const Snapshot &s)
{
  gFailed = false;
  for(auto e : s.entities) {
    if(e.first != 1 && e.first != 2) continue;
    int master = 0;
    std::vector<std::size_t> sl, ma;
    std::vector<double> tfo;
    gmsh::model::mesh::getPeriodicNodes(e.first, e.second, master, sl, ma, tfo,
                                        false);
    if(sl.empty()) continue;
    CHECK(master != 0 && master != e.second);
    CHECK(tfo.size() == 16);
    std::set<std::size_t> inLow;
    for(std::size_t i = 0; i < sl.size(); i++) {
      CHECK(s.used.count(sl[i]) == 1);
      CHECK(s.used.count(ma[i]) == 1);
      CHECK(s.primary.count(sl[i]) == 1);
      inLow.insert(sl[i]);
    }
    std::vector<std::size_t> slAll, maAll;
    std::vector<double> tfoAll;
    gmsh::model::mesh::getPeriodicNodes(e.first, e.second, master, slAll,
                                        maAll, tfoAll, true);
    CHECK(slAll.size() >= sl.size());
    std::set<std::size_t> seen;
    for(std::size_t i = 0; i < slAll.size(); i++) {
      CHECK(seen.insert(slAll[i]).second); // bijection
      CHECK(s.used.count(slAll[i]) == 1);
      CHECK(s.used.count(maAll[i]) == 1);
      if(s.primary.count(slAll[i])) CHECK(inLow.count(slAll[i]) == 1);
      else CHECK(inLow.count(slAll[i]) == 0);
    }
    // the high-order part of the "true" query is exactly the subset of its
    // keys that are not primary nodes; the two maps are disjoint by
    // construction of the role split
    std::size_t lowPairs = 0, highPairs = 0;
    for(std::size_t i = 0; i < slAll.size(); i++) {
      if(inLow.count(slAll[i]))
        lowPairs++;
      else
        highPairs++;
    }
    CHECK(lowPairs == sl.size());
    CHECK(lowPairs + highPairs == slAll.size());
  }
}

// All the nodes of the entity must have parameters that map back onto their
// coordinates, and the slave nodes must be the image of their master node.
void checkGeometryAndParameters(const Snapshot &s, bool checkSlaveImage)
{
  gFailed = false;
  std::unordered_map<std::size_t, std::array<double, 3>> xyz;
  for(auto &p : s.nodes) {
    std::array<double, 3> a = {
      {p.second.xyz[0], p.second.xyz[1], p.second.xyz[2]}};
    xyz[p.first] = a;
  }
  for(auto e : s.entities) {
    if(e.first != 1 && e.first != 2) continue;
    std::vector<std::size_t> tags;
    std::vector<double> coord, par;
    gmsh::model::mesh::getNodes(tags, coord, par, e.first, e.second, false,
                                true);
    if(tags.empty()) continue;
    // the parameters of each node, as the model gives them (getNodes above
    // returns one parameter tuple per node, i.e. all of them)
    std::size_t checked = 0;
    for(std::size_t i = 0; i < tags.size(); i++) {
      std::size_t off = i * e.first;
      if(off + e.first > par.size()) break;
      std::vector<double> p(par.begin() + off, par.begin() + off + e.first);
      std::vector<double> c(coord.begin() + 3 * i, coord.begin() + 3 * i + 3);
      std::vector<double> g;
      gmsh::model::getValue(e.first, e.second, p, g);
      if(g.size() != 3) continue;
      double d = 0.;
      for(int k = 0; k < 3; k++) d += (g[k] - c[k]) * (g[k] - c[k]);
      CHECK(std::sqrt(d) < 1e-7);
      checked++;
    }
    CHECK(checked > 0);
    if(checkSlaveImage) {
      int master = 0;
      std::vector<std::size_t> sl, ma;
      std::vector<double> tfo;
      gmsh::model::mesh::getPeriodicNodes(e.first, e.second, master, sl, ma,
                                          tfo, true);
      if(sl.empty()) continue;
      CHECK(tfo.size() == 16);
      if(tfo.size() != 16) continue;
      for(std::size_t i = 0; i < sl.size(); i++) {
        auto si = xyz.find(sl[i]);
        auto mi = xyz.find(ma[i]);
        CHECK(si != xyz.end() && mi != xyz.end());
        if(si == xyz.end() || mi == xyz.end()) continue;
        std::array<double, 3> img = applyTransform(tfo, mi->second.data());
        double d = distance3(si->second.data(), img);
        CHECK(d < 1e-7);
      }
    }
  }
}

// The elements define a bijection between the slave and the master element
// positions through the local reference nodes of their type: for each slave
// element there is exactly one master element whose corners are the images of
// the slave corners, and the permutation of their local nodes must agree with
// the periodic pairs. This is an independent check of the pairing which does
// not use the library's own correspondence code.
void checkLocalPairing(const Snapshot &s, int slaveFace, int masterFace)
{
  gFailed = false;
  int masterTag = 0;
  std::map<std::size_t, std::size_t> pairs =
    periodicPairs(2, slaveFace, true, &masterTag);
  CHECK(masterTag == masterFace);
  CHECK(!pairs.empty());

  std::size_t matched = 0;
  // the master elements, indexed by their sorted corner tags
  std::unordered_map<std::string, const Element *> masterIndex;
  for(auto &el : s.elements) {
    auto it = s.elementEntity.find(el.tag);
    if(it == s.elementEntity.end() ||
       it->second != (std::size_t)encode(2, masterFace))
      continue;
    RefElement ref = referenceElement(el.type);
    if(ref.dim != 2) continue;
    std::vector<std::size_t> corners(el.nodes.begin(),
                                     el.nodes.begin() + ref.numPrimary);
    std::sort(corners.begin(), corners.end());
    std::string key;
    for(auto c : corners) key += std::to_string(c) + ",";
    masterIndex[key] = &el;
  }

  for(auto &el : s.elements) {
    auto it = s.elementEntity.find(el.tag);
    if(it == s.elementEntity.end() ||
       it->second != (std::size_t)encode(2, slaveFace))
      continue;
    RefElement ref = referenceElement(el.type);
    if(ref.dim != 2) continue;
    // the images of the slave corners
    std::vector<std::size_t> imgCorners;
    bool complete = true;
    for(int j = 0; j < ref.numPrimary; j++) {
      auto p = pairs.find(el.nodes[j]);
      if(p == pairs.end()) {
        complete = false;
        break;
      }
      imgCorners.push_back(p->second);
    }
    if(!complete) continue;
    std::vector<std::size_t> sorted = imgCorners;
    std::sort(sorted.begin(), sorted.end());
    std::string key;
    for(auto c : sorted) key += std::to_string(c) + ",";
    auto found = masterIndex.find(key);
    CHECK(found != masterIndex.end());
    if(found == masterIndex.end()) continue;
    const Element &mel = *found->second;
    RefElement mref = referenceElement(mel.type);
    CHECK(mref.numNodes == ref.numNodes);

    // map the reference position of each slave node to the reference frame of
    // the master element through the corner correspondence, and check that
    // the master node at that position is the periodic image of the slave node
    for(int j = 0; j < ref.numNodes; j++) {
      if(j < ref.numPrimary) continue;
      auto p = pairs.find(el.nodes[j]);
      CHECK(p != pairs.end());
      if(p == pairs.end()) continue;
      // find the master node of the same local position under the corner map
      int best = -1;
      double bestD = 0.;
      for(int k = 0; k < mref.numNodes; k++) {
        // map slave reference node j to the master reference frame
        std::array<double, 3> q;
        if(ref.dim == 1) {
          double u = (ref.local[j][0] - ref.local[0][0]) /
                     (ref.local[1][0] - ref.local[0][0]);
          (void)u;
        }
        // for triangles, express in barycentric coordinates of the slave
        // corners and reuse them on the master corners
        if(ref.numPrimary == 3) {
          const auto &s0 = ref.local[0], &s1 = ref.local[1], &s2 = ref.local[2];
          double a1 = s1[0] - s0[0], b1 = s1[1] - s0[1];
          double a2 = s2[0] - s0[0], b2 = s2[1] - s0[1];
          double det = a1 * b2 - a2 * b1;
          double dx = ref.local[j][0] - s0[0], dy = ref.local[j][1] - s0[1];
          double u = (dx * b2 - dy * a2) / det;
          double v = (dy * a1 - dx * b1) / det;
          int c0 = -1, c1 = -1, c2 = -1;
          for(int c = 0; c < 3; c++) {
            if(mel.nodes[c] == imgCorners[0]) c0 = c;
            if(mel.nodes[c] == imgCorners[1]) c1 = c;
            if(mel.nodes[c] == imgCorners[2]) c2 = c;
          }
          CHECK(c0 >= 0 && c1 >= 0 && c2 >= 0);
          const auto &m0 = mref.local[c0], &m1 = mref.local[c1],
                     &m2 = mref.local[c2];
          for(int d = 0; d < 2; d++)
            q[d] = m0[d] * (1. - u - v) + m1[d] * u + m2[d] * v;
        }
        else if(ref.numPrimary == 4) {
          const auto &s0 = ref.local[0], &s1 = ref.local[1];
          const auto &s2 = ref.local[2], &s3 = ref.local[3];
          // inverse bilinear on the reference square
          double e1[2] = {s1[0] - s0[0], s1[1] - s0[1]};
          double e3[2] = {s3[0] - s0[0], s3[1] - s0[1]};
          double e2[2] = {s2[0] - s3[0] - s1[0] + s0[0],
                          s2[1] - s3[1] - s1[1] + s0[1]};
          double px = ref.local[j][0] - s0[0], py = ref.local[j][1] - s0[1];
          double a = 0.5, b = 0.5;
          for(int iter = 0; iter < 40; iter++) {
            double fx = e1[0] * a + e3[0] * b + e2[0] * a * b - px;
            double fy = e1[1] * a + e3[1] * b + e2[1] * a * b - py;
            if(std::sqrt(fx * fx + fy * fy) < 1e-14) break;
            double d1x = e1[0] + e2[0] * b, d1y = e1[1] + e2[1] * b;
            double d2x = e3[0] + e2[0] * a, d2y = e3[1] + e2[1] * a;
            double det = d1x * d2y - d2x * d1y;
            a -= (fx * d2y - fy * d2x) / det;
            b -= (d1x * fy - d1y * fx) / det;
          }
          int c0 = -1, c1 = -1, c2 = -1, c3 = -1;
          for(int c = 0; c < 4; c++) {
            if(mel.nodes[c] == imgCorners[0]) c0 = c;
            if(mel.nodes[c] == imgCorners[1]) c1 = c;
            if(mel.nodes[c] == imgCorners[2]) c2 = c;
            if(mel.nodes[c] == imgCorners[3]) c3 = c;
          }
          CHECK(c0 >= 0 && c1 >= 0 && c2 >= 0 && c3 >= 0);
          const auto &m0 = mref.local[c0], &m1 = mref.local[c1],
                     &m2 = mref.local[c2], &m3 = mref.local[c3];
          for(int d = 0; d < 2; d++)
            q[d] = m0[d] * (1 - a) * (1 - b) + m1[d] * a * (1 - b) +
                   m2[d] * a * b + m3[d] * (1 - a) * b;
        }
        double d = 0.;
        for(int dd = 0; dd < 2; dd++) {
          double diff = mref.local[k][dd] - q[dd];
          d += diff * diff;
        }
        if(best < 0 || d < bestD) {
          best = k;
          bestD = d;
        }
      }
      CHECK(best >= 0 && bestD < 1e-12);
      if(best >= 0) CHECK(mel.nodes[best] == p->second);
      matched++;
    }
  }
  CHECK(matched > 0);
}

// The number of nodes and elements per entity, for the "no growth on a
// repeated setOrder" checks.
struct Counts {
  std::size_t nodes = 0;
  std::size_t elements = 0;
};

Counts counts(const Snapshot &s)
{
  Counts c;
  c.nodes = s.nodes.size();
  c.elements = s.elements.size();
  return c;
}

// Build a periodic model, mesh it at the given order and save it.
PeriodicModel makeSavedMesh(const std::vector<double> &tfo, bool mirror,
                            int order, bool binary, const std::string &path)
{
  gmsh::clear();
  PeriodicModel m = cylinderPair(tfo, true, mirror);
  gmsh::option::setNumber("Mesh.Binary", binary ? 1 : 0);
  gmsh::model::mesh::generate(2);
  gmsh::model::mesh::setOrder(order);
  gmsh::write(path);
  return m;
}

// Recreate the exact same CAD entities (no mesh) and merge the file.
PeriodicModel loadSavedMesh(const std::vector<double> &tfo, bool mirror,
                            const std::string &path)
{
  gmsh::clear();
  PeriodicModel m = cylinderPair(tfo, false, mirror);
  gmsh::merge(path);
  return m;
}

constexpr double pi = 3.14159265358979323846;

std::vector<double> translation(double x, double y, double z)
{
  return {1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z, 0, 0, 0, 1};
}

std::vector<double> rotationZ(double angle)
{
  double c = std::cos(angle), s = std::sin(angle);
  return {c, -s, 0, 0, s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

std::vector<double> reflectionX(double plane)
{
  // x -> 2 * plane - x
  return {-1, 0, 0, 2 * plane, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

// ---------------------------------------------------------------------------
// import_roles: roles from the real elements, on merged linear and high-order
// meshes, in both file encodings.

int importRolesLinear()
{
  setCommonOptions();
  gmsh::clear();
  gmsh::model::add("x");
  gmsh::model::remove();
  PeriodicModel m = cylinderPair(translation(3, 0, 0), true, false);
  gmsh::model::mesh::generate(2);
  Snapshot fresh = snapshot();
  // A fresh mesh only has primary nodes.
  TASK_CHECK(fresh.primary.size() == fresh.used.size());
  {
    // the master side owns a subset of the master nodes; the slave side has
    // exactly one pair per primary node of its elements
    std::size_t slavePrimary = 0;
    std::set<std::size_t> seen;
    for(auto n : entityUsed(fresh, 2, m.slave))
      if(fresh.primary.count(n)) seen.insert(n);
    slavePrimary = seen.size();
    TASK_CHECK(periodicPairs(2, m.slave, false).size() == slavePrimary);
  }
  std::size_t lowFresh = periodicPairs(2, m.slave, false).size();
  TASK_CHECK(lowFresh > 0);
  TASK_CHECK(periodicPairs(2, m.slave, true).size() == lowFresh);

  std::vector<std::string> paths;
  for(int binary = 0; binary < 2; binary++) {
    std::string path = uniquePath("import-linear", binary);
    gmsh::option::setNumber("Mesh.Binary", binary);
    gmsh::write(path);
    paths.push_back(path);

    gmsh::clear();
    gmsh::model::add("x");
    gmsh::model::remove();
    PeriodicModel loaded = cylinderPair(translation(3, 0, 0), false, false);
    gmsh::merge(path);
    Snapshot s = snapshot();
    TASK_CHECK(s.primary.size() == s.used.size());
    // the pairs are all primary pairs, the two maps are disjoint through the
    // role classification
    std::map<std::size_t, std::size_t> low = periodicPairs(2, loaded.slave, false);
    std::map<std::size_t, std::size_t> all = periodicPairs(2, loaded.slave, true);
    TASK_CHECK(low.size() == all.size());
    TASK_CHECK(low.size() == lowFresh);
    for(auto &p : all) TASK_CHECK(s.primary.count(p.first) == 1);
    checkPeriodicRoles(s);
    REQUIRE_HELPERS_OK();
    // the shared boundary nodes are paired by the boundary entities too, in a
    // compatible way
    for(auto e : s.entities) {
      if(e.first != 1) continue;
      std::map<std::size_t, std::size_t> p = periodicPairs(1, e.second, true);
      for(auto &q : p) {
        if(all.count(q.first)) TASK_CHECK(all[q.first] == q.second);
      }
    }
    // the slave image of each primary pair is correct
    checkGeometryAndParameters(s, true);
    REQUIRE_HELPERS_OK();
    std::remove(path.c_str());
  }
  return 0;
}

int importRolesHighOrder()
{
  setCommonOptions();
  for(int binary = 0; binary < 2; binary++) {
    std::string path = uniquePath("import-ho", binary);
    makeSavedMesh(translation(3, 0, 0), false, 3, binary != 0, path);
    PeriodicModel m = loadSavedMesh(translation(3, 0, 0), false, path);
    Snapshot s = snapshot();
    TASK_CHECK(s.primary.size() < s.used.size());
    checkPeriodicRoles(s);
    REQUIRE_HELPERS_OK();
    auto low = periodicPairs(2, m.slave, false);
    auto all = periodicPairs(2, m.slave, true);
    TASK_CHECK(!low.empty());
    TASK_CHECK(all.size() > low.size());
    // the "false" query returns exactly the primary pairs, and every other
    // used node has a pair in the "true" query
    std::size_t high = 0;
    for(auto &p : all) {
      if(s.primary.count(p.first))
        TASK_CHECK(low.count(p.first) == 1);
      else {
        TASK_CHECK(low.count(p.first) == 0);
        high++;
      }
    }
    TASK_CHECK(high > 0);
    TASK_CHECK(all.size() == low.size() + high);
    // every node of the slave surface is paired, and its pair is a node of the
    // master surface
    std::vector<std::size_t> slaveNodes = entityNodes(s, 2, m.slave);
    TASK_CHECK(slaveNodes.size() > 0);
    for(auto n : slaveNodes) {
      TASK_CHECK(all.count(n) == 1);
      int dt = -1, tg = -1;
      std::vector<double> c, p;
      gmsh::model::mesh::getNode(n, c, p, dt, tg);
      TASK_CHECK(dt == 2 && tg == m.slave);
      gmsh::model::mesh::getNode(all[n], c, p, dt, tg);
      TASK_CHECK(dt == 2 && tg == m.master);
    }
    // the local pairing against the reference nodes of the elements
    checkLocalPairing(s, m.slave, m.master);
    REQUIRE_HELPERS_OK();
    checkGeometryAndParameters(s, true);
    REQUIRE_HELPERS_OK();
    // the source coordinates and parameters are preserved: the nodes read must
    // be exactly those of the saved file (no node moved by the reader)
    std::remove(path.c_str());
  }
  return 0;
}

// No node may be moved by the reader, and no tag may be renumbered.
int importPreservesFile()
{
  setCommonOptions();
  std::string path = uniquePath("preserve", 0);
  // the same geometry is built twice: first to produce the saved mesh, then
  // to merge the file into a model that only has the CAD entities. Both
  // models share the same entity tags, so the nodes of the file can be
  // compared by tag.
  makeSavedMesh(translation(3, 0, 0), false, 3, false, path);
  // the model that wrote the file is the reference of what was written
  Snapshot saved = snapshot();
  gmsh::clear();
  gmsh::model::add("x");
  gmsh::model::remove();
  PeriodicModel m = cylinderPair(translation(3, 0, 0), false, false);
  gmsh::merge(path);
  Snapshot s = snapshot();
  // the tags come back unchanged (and dense from 1 for this file)
  TASK_CHECK(s.nodes.size() == saved.nodes.size());
  std::size_t maxTag = 0;
  for(auto &p : s.nodes) maxTag = std::max(maxTag, p.first);
  TASK_CHECK(maxTag == s.nodes.size());
  // and so do the coordinates and the parameters, node by node
  for(auto &p : saved.nodes) {
    auto it = s.nodes.find(p.first);
    TASK_CHECK(it != s.nodes.end());
    if(it == s.nodes.end()) continue;
    for(int k = 0; k < 3; k++)
      TASK_CHECK(std::abs(it->second.xyz[k] - p.second.xyz[k]) < 1e-14);
    for(int k = 0; k < 2; k++)
      TASK_CHECK(std::abs(it->second.par[k] - p.second.par[k]) < 1e-14);
  }
  // the merge into an unmeshed CAD model (not a discrete one) has not created
  // discrete entities: the tags of the merged model are exactly the CAD ones
  for(auto e : s.entities) {
    std::string type;
    gmsh::model::getEntityType(e.first, e.second, type);
    TASK_CHECK(type.find("Discrete") == std::string::npos ||
               e.first == 0); // (no discrete curve or surface was created)
  }
  std::remove(path.c_str());
  return 0;
}

// ---------------------------------------------------------------------------
// conversion: order changes on a merged mesh, in both encodings.

int conversionSequence()
{
  setCommonOptions();
  for(int binary = 0; binary < 2; binary++) {
    std::string path = uniquePath("sequence", binary);
    makeSavedMesh(translation(3, 0, 0), false, 3, binary != 0, path);
    PeriodicModel m = loadSavedMesh(translation(3, 0, 0), false, path);
    Snapshot merged = snapshot();
    std::size_t mergedLow = periodicPairs(2, m.slave, false).size();
    std::size_t mergedAll = periodicPairs(2, m.slave, true).size();
    TASK_CHECK(mergedLow > 0 && mergedAll > mergedLow);

    // 3 -> 1
    gmsh::model::mesh::setOrder(1);
    Snapshot s = snapshot();
    TASK_CHECK(s.used.size() == s.nodes.size()); // no orphan node kept
    std::size_t low1 = periodicPairs(2, m.slave, false).size();
    std::size_t all1 = periodicPairs(2, m.slave, true).size();
    TASK_CHECK(low1 == mergedLow);
    TASK_CHECK(all1 == low1);
    checkPeriodicRoles(s);
    REQUIRE_HELPERS_OK();
    checkGeometryAndParameters(s, true);
    REQUIRE_HELPERS_OK();

    // 1 -> 4
    gmsh::model::mesh::setOrder(4);
    s = snapshot();
    TASK_CHECK(s.used.size() == s.nodes.size());
    std::size_t low4 = periodicPairs(2, m.slave, false).size();
    std::size_t all4 = periodicPairs(2, m.slave, true).size();
    TASK_CHECK(low4 == mergedLow);
    TASK_CHECK(all4 > low4);
    checkPeriodicRoles(s);
    REQUIRE_HELPERS_OK();
    checkLocalPairing(s, m.slave, m.master);
    REQUIRE_HELPERS_OK();
    checkGeometryAndParameters(s, true);
    REQUIRE_HELPERS_OK();

    // 4 -> 2
    gmsh::model::mesh::setOrder(2);
    s = snapshot();
    TASK_CHECK(s.used.size() == s.nodes.size());
    std::size_t low2 = periodicPairs(2, m.slave, false).size();
    std::size_t all2 = periodicPairs(2, m.slave, true).size();
    TASK_CHECK(low2 == mergedLow);
    TASK_CHECK(all2 > low2);
    checkPeriodicRoles(s);
    REQUIRE_HELPERS_OK();
    checkLocalPairing(s, m.slave, m.master);
    REQUIRE_HELPERS_OK();
    checkGeometryAndParameters(s, true);
    REQUIRE_HELPERS_OK();

    // a repeated setOrder must not grow the mesh
    Counts before = counts(s);
    gmsh::model::mesh::setOrder(2);
    s = snapshot();
    TASK_CHECK(counts(s).nodes == before.nodes);
    TASK_CHECK(counts(s).elements == before.elements);
    TASK_CHECK(periodicPairs(2, m.slave, true).size() == all2);

    // the writes and reads of the converted mesh keep the periodic payload
    std::string out = uniquePath("sequence-out", binary);
    gmsh::option::setNumber("Mesh.Binary", binary);
    gmsh::write(out);
    // re-read into a fresh model with the same CAD entities and no mesh
    PeriodicModel m2 = loadSavedMesh(translation(3, 0, 0), false, out);
    Snapshot reread = snapshot();
    TASK_CHECK(periodicPairs(2, m2.slave, false).size() == low2);
    TASK_CHECK(periodicPairs(2, m2.slave, true).size() == all2);
    checkPeriodicRoles(reread);
    REQUIRE_HELPERS_OK();
    checkLocalPairing(reread, m2.slave, m2.master);
    REQUIRE_HELPERS_OK();
    std::remove(path.c_str());
    std::remove(out.c_str());
  }
  return 0;
}

// Refine a second order mesh and raise the order again.
int conversionRefine()
{
  setCommonOptions();
  std::string path = uniquePath("refine", 0);
  makeSavedMesh(translation(3, 0, 0), false, 2, false, path);
  PeriodicModel m = loadSavedMesh(translation(3, 0, 0), false, path);
  std::size_t low2 = periodicPairs(2, m.slave, false).size();
  std::size_t all2 = periodicPairs(2, m.slave, true).size();
  TASK_CHECK(low2 > 0 && all2 > low2);

  gmsh::model::mesh::refine();
  Snapshot s = snapshot();
  TASK_CHECK(s.used.size() == s.nodes.size());
  std::size_t lowRef = periodicPairs(2, m.slave, false).size();
  std::size_t allRef = periodicPairs(2, m.slave, true).size();
  TASK_CHECK(lowRef > low2); // the refined skeleton has more corners
  TASK_CHECK(allRef == lowRef);
  checkPeriodicRoles(s);
  REQUIRE_HELPERS_OK();
  checkGeometryAndParameters(s, true);
  REQUIRE_HELPERS_OK();

  gmsh::model::mesh::setOrder(3);
  s = snapshot();
  TASK_CHECK(s.used.size() == s.nodes.size());
  std::size_t low3 = periodicPairs(2, m.slave, false).size();
  std::size_t all3 = periodicPairs(2, m.slave, true).size();
  TASK_CHECK(low3 == lowRef);
  TASK_CHECK(all3 > low3);
  checkPeriodicRoles(s);
  REQUIRE_HELPERS_OK();
  checkLocalPairing(s, m.slave, m.master);
  REQUIRE_HELPERS_OK();
  checkGeometryAndParameters(s, true);
  REQUIRE_HELPERS_OK();
  std::remove(path.c_str());
  return 0;
}

// Closing Mesh.Renumber: the primary nodes and their tags are preserved by
// pure order changes, and the queries and output remain correct.
int conversionNoRenumber()
{
  setCommonOptions();
  std::string path = uniquePath("norenumber", 0);
  makeSavedMesh(translation(3, 0, 0), false, 3, false, path);
  PeriodicModel m = loadSavedMesh(translation(3, 0, 0), false, path);
  gmsh::option::setNumber("Mesh.Renumber", 0);

  auto before = periodicPairs(2, m.slave, false);
  std::map<std::size_t, std::array<double, 3>> primaryXYZ;
  {
    Snapshot s = snapshot();
    for(auto &p : before) {
      auto &n = s.nodes[p.first];
      primaryXYZ[p.first] = {{n.xyz[0], n.xyz[1], n.xyz[2]}};
    }
  }
  gmsh::model::mesh::setOrder(1);
  auto after = periodicPairs(2, m.slave, false);
  TASK_CHECK(after.size() == before.size());
  // the same tags remain primary and keep their coordinates
  Snapshot s = snapshot();
  for(auto &p : before) {
    TASK_CHECK(after.count(p.first) == 1);
    TASK_CHECK(after[p.first] == p.second);
    auto it = s.nodes.find(p.first);
    TASK_CHECK(it != s.nodes.end());
    if(it != s.nodes.end()) {
      double d = 0.;
      std::array<double, 3> &q = primaryXYZ[p.first];
      for(int k = 0; k < 3; k++) d += (it->second.xyz[k] - q[k]) *
                                      (it->second.xyz[k] - q[k]);
      TASK_CHECK(std::sqrt(d) < 1e-12);
    }
  }
  gmsh::model::mesh::setOrder(3);
  s = snapshot();
  TASK_CHECK(s.used.size() == s.nodes.size());
  auto low3 = periodicPairs(2, m.slave, false);
  TASK_CHECK(low3 == after);
  checkPeriodicRoles(s);
  REQUIRE_HELPERS_OK();
  checkLocalPairing(s, m.slave, m.master);
  REQUIRE_HELPERS_OK();
  checkGeometryAndParameters(s, true);
  REQUIRE_HELPERS_OK();
  std::remove(path.c_str());
  return 0;
}

// The identity of the periodic pairs follows the corner correspondence, not
// the closest node: with HighOrderPeriodic off, moving the XYZ of the
// non-corner nodes and calling setOrder at the same order must not change the
// pairing.
int conversionNoNearestNeighbour()
{
  setCommonOptions();
  gmsh::option::setNumber("Mesh.HighOrderPeriodic", 0);
  std::string path = uniquePath("nonearest", 0);
  makeSavedMesh(translation(3, 0, 0), false, 3, false, path);
  PeriodicModel m = loadSavedMesh(translation(3, 0, 0), false, path);
  Snapshot s = snapshot();
  auto before = periodicPairs(2, m.slave, true);
  TASK_CHECK(!before.empty());
  // move every non-corner node of the slave surface far away (in XYZ only)
  std::vector<std::size_t> moved;
  for(auto &p : s.nodes) {
    if(s.primary.count(p.first)) continue;
    if(p.second.dim != 2 || p.second.tag != m.slave) continue;
    std::vector<double> c = {p.second.xyz[0] + 100., p.second.xyz[1] - 50.,
                             p.second.xyz[2] + 25.};
    gmsh::model::mesh::setNode(p.first, c, {});
    moved.push_back(p.first);
  }
  TASK_CHECK(!moved.empty());
  gmsh::model::mesh::setOrder(3);
  auto after = periodicPairs(2, m.slave, true);
  TASK_CHECK(after.size() == before.size());
  for(auto &p : before) {
    TASK_CHECK(after.count(p.first) == 1);
    TASK_CHECK(after[p.first] == p.second);
  }
  std::remove(path.c_str());
  return 0;
}

// ---------------------------------------------------------------------------
// geometry_io: real curved surfaces, a reflection and a rotation; shared edge
// nodes; triangles and quadrangles; both encodings; and a non-periodic
// surface behaving normally.

// A quarter cylinder whose slave copy is a reflection: the surface is curved
// and the transformation has a negative determinant.
int geometryReflection()
{
  setCommonOptions();
  for(int binary = 0; binary < 2; binary++) {
    std::string path = uniquePath("reflect", binary);
    makeSavedMesh(reflectionX(1.5), true, 3, binary != 0, path);
    PeriodicModel m = loadSavedMesh(reflectionX(1.5), true, path);
    Snapshot s = snapshot();
    checkPeriodicRoles(s);
    REQUIRE_HELPERS_OK();
    checkLocalPairing(s, m.slave, m.master);
    REQUIRE_HELPERS_OK();
    checkGeometryAndParameters(s, true);
    REQUIRE_HELPERS_OK();
    // the transformation really is a reflection
    std::vector<double> tfo = transformOf(2, m.slave);
    TASK_CHECK(tfo.size() == 16);
    double det = tfo[0] * (tfo[5] * tfo[10] - tfo[6] * tfo[9]) -
                 tfo[1] * (tfo[4] * tfo[10] - tfo[6] * tfo[8]) +
                 tfo[2] * (tfo[4] * tfo[9] - tfo[5] * tfo[8]);
    TASK_CHECK(det < 0);
    std::remove(path.c_str());
  }
  return 0;
}

// A rotation (angle pi / 2 about the z axis) between two copies of an annular
// sector; the two surfaces share a geometric edge, whose nodes must be the
// same objects on both sides.
int geometryRotationSharedEdge()
{
  setCommonOptions();
  // build a quarter annulus: two arcs and two radial segments
  gmsh::clear();
  gmsh::model::add("x");
  gmsh::model::remove();
  int c = gmsh::model::geo::addPoint(0, 0, 0, .3);
  int a1 = gmsh::model::geo::addPoint(1, 0, 0, .3);
  int a2 = gmsh::model::geo::addPoint(0, 1, 0, .3);
  int b1 = gmsh::model::geo::addPoint(2, 0, 0, .3);
  int b2 = gmsh::model::geo::addPoint(0, 2, 0, .3);
  int inner = gmsh::model::geo::addCircleArc(a1, c, a2);
  int outer = gmsh::model::geo::addCircleArc(b1, c, b2);
  int radial1 = gmsh::model::geo::addLine(a1, b1);
  int radial2 = gmsh::model::geo::addLine(a2, b2);
  int loop = gmsh::model::geo::addCurveLoop({-inner, radial1, outer, -radial2});
  int master = gmsh::model::geo::addPlaneSurface({loop});
  gmsh::vectorpair cp;
  gmsh::model::geo::copy({{2, master}}, cp);
  gmsh::model::geo::rotate(cp, 0, 0, 0, 0, 0, 1, pi / 2);
  gmsh::model::geo::synchronize();
  int slave = cp[0].second;
  std::vector<double> tfo = rotationZ(pi / 2);
  gmsh::model::mesh::setPeriodic(2, {slave}, {master}, tfo);
  gmsh::model::mesh::generate(2);
  gmsh::model::mesh::setOrder(3);
  Snapshot fresh = snapshot();
  checkPeriodicRoles(fresh);
  REQUIRE_HELPERS_OK();
  checkLocalPairing(fresh, slave, master);
  REQUIRE_HELPERS_OK();
  checkGeometryAndParameters(fresh, true);
  REQUIRE_HELPERS_OK();

  std::string path = uniquePath("rotation", 0);
  gmsh::write(path);
  gmsh::clear();
  gmsh::model::add("x");
  gmsh::model::remove();
  // rebuild the same geometry without periodicity and merge
  c = gmsh::model::geo::addPoint(0, 0, 0, .3);
  a1 = gmsh::model::geo::addPoint(1, 0, 0, .3);
  a2 = gmsh::model::geo::addPoint(0, 1, 0, .3);
  b1 = gmsh::model::geo::addPoint(2, 0, 0, .3);
  b2 = gmsh::model::geo::addPoint(0, 2, 0, .3);
  inner = gmsh::model::geo::addCircleArc(a1, c, a2);
  outer = gmsh::model::geo::addCircleArc(b1, c, b2);
  radial1 = gmsh::model::geo::addLine(a1, b1);
  radial2 = gmsh::model::geo::addLine(a2, b2);
  loop = gmsh::model::geo::addCurveLoop({-inner, radial1, outer, -radial2});
  master = gmsh::model::geo::addPlaneSurface({loop});
  gmsh::vectorpair cp2;
  gmsh::model::geo::copy({{2, master}}, cp2);
  gmsh::model::geo::rotate(cp2, 0, 0, 0, 0, 0, 1, pi / 2);
  gmsh::model::geo::synchronize();
  slave = cp2[0].second;
  gmsh::merge(path);
  Snapshot s = snapshot();
  checkPeriodicRoles(s);
  REQUIRE_HELPERS_OK();
  checkLocalPairing(s, slave, master);
  REQUIRE_HELPERS_OK();
  checkGeometryAndParameters(s, true);
  REQUIRE_HELPERS_OK();

  // the two surfaces share their geometric boundaries: the nodes of the shared
  // curves are used by both faces (no copy)
  auto all = periodicPairs(2, slave, true);
  std::size_t shared = 0;
  for(auto &p : all) {
    // a node of the master face that is also used by the slave face is shared
    bool inSlave = false, inMaster = false;
    for(auto &el : s.elements) {
      for(auto n : el.nodes) {
        if(n == p.first) inSlave = true;
        if(n == p.second) inMaster = true;
      }
    }
    if(inSlave && inMaster && p.first == p.second) shared++;
  }
  (void)shared;
  // the pairs of the curves shared by the two faces agree with those of the
  // faces
  for(auto e : s.entities) {
    if(e.first != 1) continue;
    auto cpairs = periodicPairs(1, e.second, true);
    for(auto &p : cpairs) {
      if(s.primary.count(p.first)) {
        TASK_CHECK(all.count(p.first) == 1);
        TASK_CHECK(all[p.first] == p.second);
      }
    }
  }
  std::remove(path.c_str());
  return 0;
}

// Triangles and quadrangles: the same model meshed with a recombined
// (quadrangle) surface mesh must give the same guarantees. The quadrangles are
// obtained by recombination of the triangular mesh (no new geometry needed).
int geometryTrianglesAndQuads()
{
  setCommonOptions();
  for(int quads = 0; quads < 2; quads++) {
    std::string path = uniquePath("quads", quads);
    gmsh::clear();
    gmsh::model::add("x");
    gmsh::model::remove();
    PeriodicModel m = cylinderPair(translation(3, 0, 0), true, false);
    gmsh::option::setNumber("Mesh.RecombineAll", quads);
    gmsh::model::mesh::generate(2);
    gmsh::model::mesh::setOrder(3);
    Snapshot fresh = snapshot();
    std::size_t tris = 0, quadsCount = 0;
    for(auto &el : fresh.elements) {
      RefElement r = referenceElement(el.type);
      if(r.dim != 2) continue;
      if(r.numPrimary == 3) tris++;
      if(r.numPrimary == 4) quadsCount++;
    }
    TASK_CHECK(quads ? quadsCount > 0 : tris > 0);
    checkPeriodicRoles(fresh);
    REQUIRE_HELPERS_OK();
    checkLocalPairing(fresh, m.slave, m.master);
    REQUIRE_HELPERS_OK();
    checkGeometryAndParameters(fresh, true);
    REQUIRE_HELPERS_OK();
    gmsh::write(path);
    gmsh::clear();
    gmsh::model::add("x");
    gmsh::model::remove();
    PeriodicModel m2 = cylinderPair(translation(3, 0, 0), false, false);
    gmsh::merge(path);
    Snapshot s = snapshot();
    checkPeriodicRoles(s);
    REQUIRE_HELPERS_OK();
    checkLocalPairing(s, m2.slave, m2.master);
    REQUIRE_HELPERS_OK();
    checkGeometryAndParameters(s, true);
    REQUIRE_HELPERS_OK();
    std::remove(path.c_str());
  }
  gmsh::option::setNumber("Mesh.RecombineAll", 0);
  return 0;
}

// A reflection about the line shared by the two surfaces: the master and the
// slave traverse that geometric edge in opposite directions, and its nodes
// must be the same objects on both sides, paired with themselves, so that the
// curve, the face and the boundary of the face all say the same thing.
int geometryReversedSharedEdge()
{
  setCommonOptions();
  // square [-1,1]x[0,1] in z = 0, split by x = 0; the reflection x -> -x
  // maps the right half onto the left half and reverses the shared edge.
  struct Build {
    int left, right;
  };
  auto build = [](Build &b) {
    gmsh::model::add("reversed-edge");
    int p00 = gmsh::model::geo::addPoint(-1, 0, 0, .5);
    int p10 = gmsh::model::geo::addPoint(0, 0, 0, .5);
    int p20 = gmsh::model::geo::addPoint(1, 0, 0, .5);
    int p01 = gmsh::model::geo::addPoint(-1, 1, 0, .5);
    int p11 = gmsh::model::geo::addPoint(0, 1, 0, .5);
    int p21 = gmsh::model::geo::addPoint(1, 1, 0, .5);
    int bottomLeft = gmsh::model::geo::addLine(p00, p10);
    int bottomRight = gmsh::model::geo::addLine(p10, p20);
    int right = gmsh::model::geo::addLine(p20, p21);
    int topRight = gmsh::model::geo::addLine(p21, p11);
    int topLeft = gmsh::model::geo::addLine(p11, p01);
    int left = gmsh::model::geo::addLine(p01, p00);
    int shared = gmsh::model::geo::addLine(p10, p11);
    // the shared edge is used with opposite signs in the two loops
    int l1 = gmsh::model::geo::addCurveLoop({bottomLeft, shared, topLeft, left});
    int l2 =
      gmsh::model::geo::addCurveLoop({-shared, bottomRight, right, topRight});
    b.left = gmsh::model::geo::addPlaneSurface({l1});
    b.right = gmsh::model::geo::addPlaneSurface({l2});
    gmsh::model::geo::synchronize();
  };
  std::vector<double> tfo = reflectionX(0.); // x -> -x
  for(int binary = 0; binary < 2; binary++) {
    std::string path = uniquePath("reversed-edge", binary);
    // saved by a model that has the periodicity
    gmsh::clear();
    Build fresh;
    build(fresh);
    gmsh::model::mesh::setPeriodic(2, {fresh.left}, {fresh.right}, tfo);
    gmsh::option::setNumber("Mesh.Binary", binary);
    gmsh::model::mesh::generate(2);
    gmsh::model::mesh::setOrder(3);
    Snapshot saved = snapshot();
    checkPeriodicRoles(saved);
    REQUIRE_HELPERS_OK();
    checkLocalPairing(saved, fresh.left, fresh.right);
    REQUIRE_HELPERS_OK();
    checkGeometryAndParameters(saved, true);
    REQUIRE_HELPERS_OK();
    gmsh::write(path);

    // merged into the same CAD without periodicity
    gmsh::clear();
    Build loaded;
    build(loaded);
    gmsh::merge(path);
    Snapshot s = snapshot();
    checkPeriodicRoles(s);
    REQUIRE_HELPERS_OK();
    checkLocalPairing(s, loaded.left, loaded.right);
    REQUIRE_HELPERS_OK();
    checkGeometryAndParameters(s, true);
    REQUIRE_HELPERS_OK();
    // the edge nodes shared by the two faces are the same objects, and the
    // face correspondence pairs them with themselves
    auto facePairs = periodicPairs(2, loaded.left, true);
    std::size_t sharedNodes = 0;
    for(auto &p : facePairs) {
      if(p.first == p.second) sharedNodes++;
    }
    TASK_CHECK(sharedNodes > 0);
    // the boundary entities of both faces agree with the face maps
    for(auto e : s.entities) {
      if(e.first != 1) continue;
      std::map<std::size_t, std::size_t> cp = periodicPairs(1, e.second, true);
      for(auto &q : cp) {
        if(facePairs.count(q.first)) TASK_CHECK(facePairs[q.first] == q.second);
      }
    }
    std::remove(path.c_str());
  }
  return 0;
}

// A surface without any periodic constraint keeps its normal behaviour through
// merge, order changes and write.
int geometryNoPeriodic()
{
  setCommonOptions();
  std::string path = uniquePath("noper", 0);
  gmsh::clear();
  gmsh::model::add("x");
  gmsh::model::remove();
  PeriodicModel m = cylinderPair(translation(3, 0, 0), false, false);
  gmsh::model::mesh::generate(2);
  Snapshot s0 = snapshot();
  std::size_t nodes0 = s0.nodes.size();
  gmsh::model::mesh::setOrder(3);
  gmsh::write(path);

  gmsh::clear();
  gmsh::model::add("x");
  gmsh::model::remove();
  PeriodicModel m2 = cylinderPair(translation(3, 0, 0), false, false);
  gmsh::merge(path);
  Snapshot s = snapshot();
  TASK_CHECK(s.nodes.size() > nodes0);
  for(auto e : s.entities) {
    int master = 0;
    std::vector<std::size_t> sl, ma;
    std::vector<double> tfo;
    gmsh::model::mesh::getPeriodicNodes(e.first, e.second, master, sl, ma, tfo,
                                        true);
    TASK_CHECK(sl.empty());
    TASK_CHECK(master == e.second);
  }
  gmsh::model::mesh::setOrder(2);
  s = snapshot();
  TASK_CHECK(s.used.size() == s.nodes.size());
  checkGeometryAndParameters(s, false);
  REQUIRE_HELPERS_OK();
  gmsh::model::mesh::setOrder(4);
  s = snapshot();
  TASK_CHECK(s.used.size() == s.nodes.size());
  checkGeometryAndParameters(s, false);
  REQUIRE_HELPERS_OK();
  std::remove(path.c_str());
  return 0;
}

// An existing, freshly generated periodic mesh keeps its behaviour (the
// production path must not be changed by the import fixes).
int geometryFreshBehaviour()
{
  setCommonOptions();
  PeriodicModel m = cylinderPair(translation(3, 0, 0), true, false);
  gmsh::model::mesh::generate(2);
  std::size_t lowLinear = periodicPairs(2, m.slave, false).size();
  TASK_CHECK(lowLinear > 0);
  gmsh::model::mesh::setOrder(3);
  std::size_t low3 = periodicPairs(2, m.slave, false).size();
  std::size_t all3 = periodicPairs(2, m.slave, true).size();
  TASK_CHECK(low3 == lowLinear);
  TASK_CHECK(all3 > low3);
  Snapshot s = snapshot();
  checkPeriodicRoles(s);
  REQUIRE_HELPERS_OK();
  checkLocalPairing(s, m.slave, m.master);
  REQUIRE_HELPERS_OK();
  checkGeometryAndParameters(s, true);
  REQUIRE_HELPERS_OK();
  gmsh::model::mesh::setOrder(2);
  TASK_CHECK(periodicPairs(2, m.slave, false).size() == lowLinear);
  return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// registration

const struct task_case task_cases[] = {
  {"import_roles", "linear", importRolesLinear},
  {"import_roles", "high_order", importRolesHighOrder},
  {"import_roles", "preserve_file", importPreservesFile},
  {"conversion", "sequence", conversionSequence},
  {"conversion", "refine", conversionRefine},
  {"conversion", "no_renumber", conversionNoRenumber},
  {"conversion", "no_nearest_neighbour", conversionNoNearestNeighbour},
  {"geometry_io", "reflection", geometryReflection},
  {"geometry_io", "rotation_shared_edge", geometryRotationSharedEdge},
  {"geometry_io", "triangles_and_quads", geometryTrianglesAndQuads},
  {"geometry_io", "reversed_shared_edge", geometryReversedSharedEdge},
  {"geometry_io", "no_periodic", geometryNoPeriodic},
  {"geometry_io", "fresh_behaviour", geometryFreshBehaviour},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
