/****************************************************************************
* Volume preserving QEM edge collapse - target tests.
*
* Every reference computation in this file is written from scratch against
* the live mesh (scalar triple products, KKT / SVD constrained solves, fan and
* shell topology walks). It never calls the production volume helper
* (vcg::tri::VolumePreservationHelper): the whole point is an independent
* oracle. Failures throw, checks/target_main.cpp turns that into a non-zero
* exit code.
****************************************************************************/

#include "checks/mesh.hpp"

#include <vcg/complex/algorithms/update/position.h>
#include <vcg/complex/append.h>

#include <Eigen/SVD>
#include <Eigen/LU>

#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace volume_tests {

using vcg::tri::Allocator;
using vcg::tri::EdgeCollapser;
using vcg::LocalOptimization;
using vcg::tri::UpdateFlags;
using vcg::tri::UpdateTopology;

namespace {

constexpr double kRankEps = 1e-11;

#define VT_REQUIRE(cond, msg) \
  do { if(!(cond)) throw std::runtime_error(std::string("volume_tests: ") + (msg)); } while(0)

// --------------------------------------------------------------------------
// Independent geometry / topology oracles.
// --------------------------------------------------------------------------

static double triple6(const Point3d &a, const Point3d &b, const Point3d &c)
{ return a.dot(b ^ c) / 6.0; }

static double shellVolume(const Mesh &m)
{
  double v = 0.0;
  for(const F &f : m.face) if(!f.IsD()) v += triple6(f.cP(0), f.cP(1), f.cP(2));
  return v;
}

// Per connected component (shell): directed volume and initial bbox diagonal.
struct ShellStats { double volume; double diag; size_t faces; };

static std::vector<ShellStats> perShellStats(const Mesh &m)
{
  std::map<const V*, int> comp;
  int ncomp = 0;
  for(const V &seed : m.vert)
  {
    if(seed.IsD() || comp.count(&seed)) continue;
    std::vector<const V*> stack(1, &seed);
    comp[&seed] = ncomp;
    while(!stack.empty())
    {
      const V *cur = stack.back(); stack.pop_back();
      for(vcg::face::VFIterator<F> x(const_cast<V*>(cur)); !x.End(); ++x)
        for(int j=0;j<3;++j)
        {
          const V *w = x.F()->cV(j);
          if(!comp.count(w)) { comp[w] = ncomp; stack.push_back(w); }
        }
    }
    ++ncomp;
  }

  std::vector<ShellStats> out(ncomp, {0.0, 0.0, 0});
  std::vector<Box3d> boxes(ncomp);
  std::vector<Point3d> centroid(ncomp, Point3d(0,0,0));
  std::vector<size_t> nv(ncomp, 0);
  for(auto &kv : comp)
  {
    const V *v = kv.first;
    centroid[kv.second] += v->cP();
    ++nv[kv.second];
  }
  for(int i=0;i<ncomp;++i) centroid[i] /= double(nv[i]);
  for(const F &f : m.face)
  {
    if(f.IsD()) continue;
    int c = comp[f.cV(0)];
    // Directed volume in a frame centered on this shell: keeps the correct
    // sign and magnitude even when the shell sits ~1e6 from the world origin.
    Point3d a = f.cP(0) - centroid[c], b = f.cP(1) - centroid[c], cc = f.cP(2) - centroid[c];
    out[c].volume += triple6(a, b, cc);
    ++out[c].faces;
    for(int j=0;j<3;++j) boxes[c].Add(f.cP(j));
  }
  for(int i=0;i<ncomp;++i) out[i].diag = boxes[i].Diag();
  return out;
}

// Every undirected edge must be incident on exactly two coherently oriented
// faces, and every vertex fan must be a single closed cycle.
static void assertClosedManifold(const Mesh &m)
{
  std::map<std::pair<const V*,const V*>, int> edgeCnt, edgeOri;
  for(const F &f : m.face)
  {
    if(f.IsD()) continue;
    for(int j=0;j<3;++j)
    {
      const V *a = f.cV(j), *b = f.cV1(j);
      auto key = a<b ? std::make_pair(a,b) : std::make_pair(b,a);
      ++edgeCnt[key];
      edgeOri[key] += (a<b) ? 1 : -1;
    }
  }
  VT_REQUIRE(!edgeCnt.empty(), "mesh has no edges");
  for(auto &kv : edgeCnt)
  {
    VT_REQUIRE(kv.second == 2, "edge is not two-manifold");
    VT_REQUIRE(edgeOri[kv.first] == 0, "edge is not coherently oriented");
  }
  for(const V &v : m.vert)
  {
    if(v.IsD()) continue;
    std::vector<const F*> fan;
    for(vcg::face::VFIterator<F> x(const_cast<V*>(&v)); !x.End(); ++x) fan.push_back(x.F());
    VT_REQUIRE(fan.size() >= 3, "vertex fan too small");
    std::map<const F*, std::vector<const F*> > links;
    for(const F *f : fan)
    {
      int k = 0; while(f->cV(k) != &v) ++k;
      for(const V *w : { f->cV1(k), f->cV2(k) })
      {
        std::vector<const F*> others;
        for(const F *g : fan)
          if(g != f)
            for(int j=0;j<3;++j)
              if((g->cV(j)==(&v) && g->cV1(j)==w) || (g->cV(j)==w && g->cV1(j)==(&v)))
                others.push_back(g);
        VT_REQUIRE(others.size() == 1, "non-manifold vertex fan");
        links[f].push_back(others.front());
      }
    }
    std::set<const F*> seen;
    std::vector<const F*> st(1, fan.front());
    while(!st.empty())
    {
      const F *cur = st.back(); st.pop_back();
      if(!seen.insert(cur).second) continue;
      for(const F *nb : links[cur]) if(!seen.count(nb)) st.push_back(nb);
    }
    VT_REQUIRE(seen.size() == fan.size(), "disconnected vertex fan");
  }
}

// Independent one-ring snapshot taken directly from surviving faces.
struct RingOracle
{
  Point3d o;
  std::vector<F*> shared, only0, only1;
  Eigen::Vector3d n = Eigen::Vector3d::Zero(); // sum (p x q) over surviving faces, local
  double d = 0.0;                              // sum triple products (x6), local
  double vOld = 0.0;                           // directed ring volume, world coords

  static Eigen::Vector3d e(const Point3d &p) { return Eigen::Vector3d(p[0],p[1],p[2]); }
  Point3d loc(const Point3d &p) const { return p - o; }
};

static RingOracle collectRing(V *v0, V *v1)
{
  RingOracle r;
  r.o = (v0->cP() + v1->cP()) * 0.5;
  for(vcg::face::VFIterator<F> x(v0); !x.End(); ++x)
  {
    bool hasV1 = false;
    for(int j=0;j<3;++j) if(x.F()->V(j) == v1) hasV1 = true;
    (hasV1 ? r.shared : r.only0).push_back(x.F());
  }
  for(vcg::face::VFIterator<F> x(v1); !x.End(); ++x)
  {
    bool hasV0 = false;
    for(int j=0;j<3;++j) if(x.F()->V(j) == v0) hasV0 = true;
    if(!hasV0) r.only1.push_back(x.F());
  }

  auto addOld = [&](const F &f)
  {
    Point3d a = r.loc(f.cP(0)), b = r.loc(f.cP(1)), c = r.loc(f.cP(2));
    r.d += a.dot(b ^ c);
    r.vOld += triple6(f.cP(0), f.cP(1), f.cP(2));
  };
  for(F *f : r.shared) addOld(*f);
  for(F *f : r.only0) addOld(*f);
  for(F *f : r.only1) addOld(*f);

  auto addSurv = [&](const F &f, V *endpoint)
  {
    int k = 0; while(f.V(k) != endpoint) ++k;
    Point3d p = r.loc(f.cP((k+1)%3)), q = r.loc(f.cP((k+2)%3));
    r.n += RingOracle::e(p).cross(RingOracle::e(q));
  };
  for(F *f : r.only0) addSurv(*f, v0);
  for(F *f : r.only1) addSurv(*f, v1);
  return r;
}

// Ring volume after the collapse, independently re-summed: shared faces are
// gone, every other face contributes once with its endpoint moved to x.
static double ringVolumeAfter(const RingOracle &r, V *v0, V *v1, const Point3d &x)
{
  double v = 0.0;
  auto moved = [&](const F &f, V *endpoint)
  {
    int k = 0; while(f.V(k) != endpoint) ++k;
    Point3d p[3];
    for(int j=0;j<3;++j) p[j] = (f.V(j) == endpoint) ? x : f.cP(j);
    (void)k;
    v += triple6(p[0], p[1], p[2]);
  };
  for(F *f : r.only0) moved(*f, v0);
  for(F *f : r.only1) moved(*f, v1);
  return v;
}

static void quadricMatrix(const math::Quadric<double> &q, Eigen::Matrix3d &A, Eigen::Vector3d &b)
{
  A << q.a[0], q.a[1], q.a[2],
       q.a[1], q.a[3], q.a[4],
       q.a[2], q.a[4], q.a[5];
  b << q.b[0], q.b[1], q.b[2];
}

// Independent constrained QEM minimum. KKT stationarity
//   2 A u + b_local = lambda n,   n.u = d
// solved through an SVD null-space reduction; rank deficient A and/or H keep
// a well defined answer, ties are broken toward the midpoint (local origin).
struct RefPlacement { bool ok = false; Point3d x; };

// Same solve, given the independently collected ring (the mesh itself is only
// needed for the quadrics above).
static RefPlacement referenceSolve(V *v0, V *v1, const RingOracle &ring, double ringScale)
{
  Eigen::Matrix3d A0, A1; Eigen::Vector3d b0, b1;
  quadricMatrix(v0->Qd(), A0, b0);
  quadricMatrix(v1->Qd(), A1, b1);
  Eigen::Matrix3d A = A0 + A1;
  Eigen::Vector3d bw = b0 + b1;
  Eigen::Vector3d oo = RingOracle::e(ring.o);
  Eigen::Vector3d bloc = 2.0 * A * oo + bw;

  RefPlacement out;
  Eigen::Vector3d u;
  auto finish = [&](bool ok)
  {
    out.ok = ok;
    if(ok)
    {
      Eigen::Vector3d x = oo + u;
      out.x = Point3d(x[0], x[1], x[2]);
    }
    return out;
  };

  if(ring.n.norm() <= kRankEps * ringScale * ringScale)
  {
    if(std::abs(ring.d) > 1e-10 * ringScale * ringScale * ringScale) return finish(false);
    Eigen::JacobiSVD<Eigen::Matrix3d> svd(A, Eigen::ComputeFullU | Eigen::ComputeFullV);
    Eigen::Vector3d s = svd.singularValues();
    double tol = kRankEps * s(0);
    Eigen::Vector3d rhs = -0.5 * bloc;
    u.setZero();
    Eigen::Vector3d nullRes = Eigen::Vector3d::Zero();
    for(int i=0;i<3;++i)
    {
      if(s(i) > tol) u += (svd.matrixU().col(i).dot(rhs) / s(i)) * svd.matrixV().col(i);
      else           nullRes += svd.matrixU().col(i).dot(rhs) * svd.matrixU().col(i);
    }
    if(nullRes.norm() > 1e-9 * std::max(rhs.norm(), 1e-12)) return finish(false);
    return finish(true);
  }

  Eigen::Vector3d u0 = (ring.d / ring.n.squaredNorm()) * ring.n;
  Eigen::JacobiSVD<Eigen::Matrix<double,3,1> > svdN(ring.n, Eigen::ComputeFullU | Eigen::ComputeFullV);
  Eigen::Matrix<double,3,2> Z;
  Z.col(0) = svdN.matrixU().col(1);
  Z.col(1) = svdN.matrixU().col(2);

  Eigen::Matrix2d H = Z.transpose() * A * Z;
  Eigen::Vector2d g = Z.transpose() * (2.0 * A * u0 + bloc);
  Eigen::JacobiSVD<Eigen::Matrix2d> svdH(H, Eigen::ComputeFullU | Eigen::ComputeFullV);
  Eigen::Vector2d sh = svdH.singularValues();
  double tol = kRankEps * sh(0);
  Eigen::Vector2d rhs = -0.5 * g;
  Eigen::Vector2d z = Eigen::Vector2d::Zero();
  Eigen::Vector2d nullRes = Eigen::Vector2d::Zero();
  for(int i=0;i<2;++i)
  {
    if(sh(i) > tol) z += (svdH.matrixU().col(i).dot(rhs) / sh(i)) * svdH.matrixV().col(i);
    else            nullRes += svdH.matrixU().col(i).dot(rhs) * svdH.matrixU().col(i);
  }
  if(nullRes.norm() > 1e-9 * std::max(rhs.norm(), 1e-12)) return finish(false);
  for(int i=0;i<2;++i)
    if(sh(i) <= tol)
    {
      Eigen::Vector2d nd = svdH.matrixV().col(i);
      z += (-nd.dot(Z.transpose() * u0 + z)) * nd;
    }
  u = u0 + Z * z;
  return finish(true);
}

static double ringScaleOf(V *v0, V *v1, const Point3d &o)
{
  double s = 0.0;
  for(V *v : { v0, v1 })
    for(vcg::face::VFIterator<F> x(v); !x.End(); ++x)
      for(int j=0;j<3;++j) s = std::max(s, (x.F()->cP(j) - o).Norm());
  return s;
}

// Build the production candidate for an edge and run its placement only.
static Point3d productionPlacement(V *v0, V *v1, TriEdgeCollapseQuadricParameter &p)
{
  Pair pair(v0, v1);
  Collapse cand(pair, 0, &p);
  cand.ComputePosition(&p);
  return cand.optimalPos;
}

} // anonymous namespace

// ==========================================================================
// 1. constrained_optimum: asymmetric one-ring, independently built equality,
//    KKT optimality of the production constrained minimum.
// ==========================================================================
void constrained_optimum()
{
  Mesh m; make_fixture(m);
  TriEdgeCollapseQuadricParameter p;
  p.OptimalPlacement = p.PreserveTopology = p.PreserveVolume = true;
  LocalOptimization<Mesh> session(m, &p);
  session.Init<Collapse>();
  UpdateFlags<Mesh>::FaceBorderFromVF(m);
  UpdateBounding<Mesh>::Box(m);
  double D = m.bbox.Diag();

  int checked = 0;
  std::set<std::pair<V*,V*>> edges;
  for(F &f : m.face)
    for(int j=0;j<3;++j)
    {
      V *a = f.V(j), *b = f.V1(j);
      if(a > b) std::swap(a, b);
      edges.insert({a, b});
    }

  for(auto &e : edges)
  {
    V *v0 = e.first, *v1 = e.second;
    RingOracle ring = collectRing(v0, v1);
    if(ring.n.norm() <= kRankEps) continue; // full-rank constraint cases only
    double rs = ringScaleOf(v0, v1, ring.o);
    RefPlacement ref = referenceSolve(v0, v1, ring, rs);
    VT_REQUIRE(ref.ok, "reference constrained solve failed on a regular ring");

    Point3d x = productionPlacement(v0, v1, p);

    // Position matches the independent constrained minimizer.
    VT_REQUIRE(Distance(x, ref.x) <= 1e-8 * D, "constrained position differs from independent KKT solve");

    // KKT stationarity: gradient of Q is parallel to the constraint normal.
    Eigen::Matrix3d A0, A1; Eigen::Vector3d b0, b1;
    quadricMatrix(v0->Qd(), A0, b0); quadricMatrix(v1->Qd(), A1, b1);
    Eigen::Matrix3d A = A0 + A1;
    Eigen::Vector3d oo = RingOracle::e(ring.o);
    Eigen::Vector3d u = RingOracle::e(x) - oo;
    Eigen::Vector3d grad = 2.0 * A * u + (2.0 * A * oo + (b0 + b1));
    Eigen::Vector3d nhat = ring.n.normalized();
    double tang = (grad - grad.dot(nhat) * nhat).norm();
    VT_REQUIRE(tang <= 1e-7 * std::max(grad.norm(), 1.0), "KKT stationarity violated");

    // The affine equality itself, checked against the independently built n,d.
    VT_REQUIRE(std::abs(ring.n.dot(u) - ring.d) <= 1e-10 * D * D * D,
               "production point violates the volume equality");

    // And the actual ring volumes before/after the simulated collapse.
    double vAfter = ringVolumeAfter(ring, v0, v1, x);
    VT_REQUIRE(std::abs(vAfter - ring.vOld) <= 1e-10 * D * D * D,
               "simulated ring volume changed");

    // Optimality: the reference point beats random feasible perturbations.
    Eigen::Vector3d t1 = Eigen::Vector3d::UnitX().cross(nhat);
    if(t1.norm() < 1e-6) t1 = Eigen::Vector3d::UnitY().cross(nhat);
    t1.normalize();
    Eigen::Vector3d t2 = nhat.cross(t1);
    auto qAt = [&](const Eigen::Vector3d &xx)
    { return (xx.transpose() * A * xx)(0) + (b0+b1).dot(xx) + v0->Qd().c + v1->Qd().c; };
    double qRef = qAt(RingOracle::e(x));
    for(int k=-3;k<=3;++k) if(k != 0)
    {
      Eigen::Vector3d xx = RingOracle::e(x) + (1e-3 * k) * t1 + (2e-4 * k) * t2;
      VT_REQUIRE(qAt(xx) >= qRef - 1e-7 * std::max(1.0, std::abs(qRef)),
                 "constrained point is not the QEM minimum");
    }
    ++checked;
  }
  VT_REQUIRE(checked >= 20, "asymmetric fixture did not provide enough regular rings");
}

// ==========================================================================
// 2. singular_constraint: planar ring, rank deficient quadric, identically
//    zero equality (feasible and incompatible), nearest-midpoint tie break.
// ==========================================================================
namespace {

void makeBipyramid(Mesh &m, double hA, double hB, bool flat = false)
{
  if(flat)
  {
    Allocator<Mesh>::AddVertex(m, Point3d(0,0,0));
    Allocator<Mesh>::AddVertex(m, Point3d(2,0,0));
    Allocator<Mesh>::AddVertex(m, Point3d(-1,1,0));
    Allocator<Mesh>::AddVertex(m, Point3d(-1,-1,0));
    Allocator<Mesh>::AddVertex(m, Point3d(4,0,0));
  }
  else
  {
    Allocator<Mesh>::AddVertex(m, Point3d(0,0,hA));
    Allocator<Mesh>::AddVertex(m, Point3d(0,0,hB));
    Allocator<Mesh>::AddVertex(m, Point3d(1,0,0));
    Allocator<Mesh>::AddVertex(m, Point3d(-0.5, 0.8660254037844386, 0));
    Allocator<Mesh>::AddVertex(m, Point3d(-0.5,-0.8660254037844386, 0));
  }
  V *A=&m.vert[0], *B=&m.vert[1], *p1=&m.vert[2], *p2=&m.vert[3], *p3=&m.vert[4];
  Allocator<Mesh>::AddFace(m, A, p1, p2);
  Allocator<Mesh>::AddFace(m, A, p2, p3);
  Allocator<Mesh>::AddFace(m, A, p3, p1);
  Allocator<Mesh>::AddFace(m, B, p2, p1);
  Allocator<Mesh>::AddFace(m, B, p3, p2);
  Allocator<Mesh>::AddFace(m, B, p1, p3);
  UpdateTopology<Mesh>::VertexFace(m);
}

void installQuadrics(Mesh &m, TriEdgeCollapseQuadricParameter &p)
{
  LocalOptimization<Mesh> session(m, &p);
  session.Init<Collapse>(); // builds Qd through the normal InitQuadric path
}

} // namespace

void singular_constraint()
{
  TriEdgeCollapseQuadricParameter p;
  p.OptimalPlacement = p.PreserveTopology = p.PreserveVolume = true;

  // ---- (a) planar one-ring: n = 0 and d = 0, identity equality ------------
  {
    Mesh m; makeBipyramid(m, 0, 0, true);
    installQuadrics(m, p);
    V *v0 = &m.vert[2], *v1 = &m.vert[3];
    RingOracle ring = collectRing(v0, v1);
    VT_REQUIRE(ring.n.norm() < 1e-12, "planar ring should give a zero constraint normal");
    VT_REQUIRE(std::abs(ring.d) < 1e-12, "planar ring should give zero constraint rhs");

    double rs = ringScaleOf(v0, v1, ring.o);
    RefPlacement ref = referenceSolve(v0, v1, ring, rs);
    VT_REQUIRE(ref.ok, "identity equality must fall back to the unconstrained minimum");
    Point3d x = productionPlacement(v0, v1, p);
    VT_REQUIRE(Distance(x, ref.x) <= 1e-9, "identity equality did not return the unconstrained minimum");

    // The unconstrained stationarity 2 A u + b = 0 must hold.
    Eigen::Matrix3d A0, A1; Eigen::Vector3d b0, b1;
    quadricMatrix(v0->Qd(), A0, b0); quadricMatrix(v1->Qd(), A1, b1);
    Eigen::Matrix3d A = A0 + A1;
    Eigen::Vector3d u = RingOracle::e(x) - RingOracle::e(ring.o);
    Eigen::Vector3d res = 2.0 * A * u + (2.0 * A * RingOracle::e(ring.o) + (b0 + b1));
    VT_REQUIRE(res.norm() <= 1e-7 * std::max((b0+b1).norm(), 1.0),
               "identity-equality fallback is not the QEM stationary point");
  }

  // ---- (b) incompatible identity equality: n = 0, d != 0 ------------------
  {
    Mesh m; makeBipyramid(m, 2.0, -2.0);
    installQuadrics(m, p);
    V *v0 = &m.vert[2], *v1 = &m.vert[3]; // base edge: surviving faces cancel
    RingOracle ring = collectRing(v0, v1);
    VT_REQUIRE(ring.n.norm() < 1e-12, "bipyramid base edge should give n = 0");
    VT_REQUIRE(std::abs(ring.d) > 1.0, "non-degenerate bipyramid should give d != 0");
    double rs = ringScaleOf(v0, v1, ring.o);
    VT_REQUIRE(!referenceSolve(v0, v1, ring, rs).ok, "reference must flag the incompatible equality");

    // Snapshot the full state a trial could pollute: coordinates, Qd
    // coefficients, marks and the VF linked list of every incident face.
    struct Snap { Point3d p0, p1; double a[10]; int mark0, mark1;
                  std::vector<std::pair<F*, std::array<std::pair<F*,int>,3>>> vf; };
    auto snapshot = [&]()
    {
      Snap s; s.p0 = v0->P(); s.p1 = v1->P();
      const double aa[10] = { v0->Qd().a[0],v0->Qd().a[1],v0->Qd().a[2],v0->Qd().a[3],
                              v0->Qd().a[4],v0->Qd().a[5],
                              v1->Qd().a[0],v1->Qd().a[1],v1->Qd().a[2],v1->Qd().a[3] };
      std::copy(aa, aa+10, s.a);
      s.mark0 = v0->IMark(); s.mark1 = v1->IMark();
      for(V *v : { v0, v1 })
        for(vcg::face::VFIterator<F> x(v); !x.End(); ++x)
        {
          std::array<std::pair<F*,int>,3> links;
          for(int k=0;k<3;++k) links[k] = { x.F()->VFp(k), x.F()->VFi(k) };
          s.vf.push_back({ x.F(), links });
        }
      return s;
    };
    Snap before = snapshot();

    // Drive the real candidate path: ComputePosition marks the placement
    // infeasible, IsFeasible then rejects it.
    Pair pair(v0, v1);
    Collapse cand(pair, 0, &p);
    cand.ComputePosition(&p);
    VT_REQUIRE(!cand.IsFeasible(&p), "incompatible identity constraint must reject the candidate");

    Snap after = snapshot();
    VT_REQUIRE(v0->P() == before.p0 && v1->P() == before.p1,
               "rejected trial moved coordinates");
    VT_REQUIRE(after.mark0 == before.mark0 && after.mark1 == before.mark1,
               "rejected trial changed vertex marks");
    const double aaNow[10] = { v0->Qd().a[0],v0->Qd().a[1],v0->Qd().a[2],v0->Qd().a[3],
                               v0->Qd().a[4],v0->Qd().a[5],
                               v1->Qd().a[0],v1->Qd().a[1],v1->Qd().a[2],v1->Qd().a[3] };
    for(int i=0;i<10;++i) VT_REQUIRE(aaNow[i] == before.a[i], "rejected trial changed a quadric");
    VT_REQUIRE(after.vf.size() == before.vf.size(), "rejected trial changed the VF fan size");
    for(size_t i=0;i<before.vf.size();++i)
    {
      VT_REQUIRE(after.vf[i].first == before.vf[i].first, "VF fan face order changed");
      for(int k=0;k<3;++k)
        VT_REQUIRE(after.vf[i].second[k] == before.vf[i].second[k],
                   "rejected trial rewired a VF link");
    }
  }

  // ---- (c) rank deficient quadric under a regular, full rank constraint ---
  // A regular octahedron ring keeps n != 0. We deliberately replace the
  // vertex quadrics by rank-1 plane quadrics so the summed QEM matrix is
  // singular, and check the closest-to-midpoint selection on the minimizer
  // continuum rather than merely the preserved volume.
  {
    Mesh m; vcg::tri::Octahedron(m);
    UpdateTopology<Mesh>::VertexFace(m);
    installQuadrics(m, p);
    V *v0 = m.face[0].V(0), *v1 = m.face[0].V(1);
    RingOracle ring = collectRing(v0, v1);
    VT_REQUIRE(ring.n.norm() > 1.0, "octahedron ring should keep a full rank constraint");
    Eigen::Vector3d nhat = ring.n.normalized();

    // Rank-1 quadric whose null space contains the whole constraint plane:
    // the objective is then constant over the feasible plane, so every finite
    // feasible point is a minimizer and the midpoint rule must pick the
    // orthogonal projection of the midpoint onto the plane.
    Plane3d plane;
    plane.SetDirection(Point3d(nhat[0], nhat[1], nhat[2]));
    plane.Normalize();
    plane.SetOffset(plane.Direction().dot(v0->cP()));
    math::Quadric<double> qn; qn.ByPlane(plane);
    v0->Qd() = qn; v1->Qd() = qn;

    double rs = ringScaleOf(v0, v1, ring.o);
    RefPlacement ref = referenceSolve(v0, v1, ring, rs);
    VT_REQUIRE(ref.ok, "rank-1 quadric still has a constrained minimum continuum");
    Point3d x = productionPlacement(v0, v1, p);
    VT_REQUIRE(Distance(x, ref.x) <= 1e-9, "rank-deficient continuum placement mismatch");
    VT_REQUIRE(std::abs(ring.n.dot(RingOracle::e(x) - RingOracle::e(ring.o)) - ring.d)
               <= 1e-10 * rs * rs * rs, "rank-deficient point violates the equality");

    // Chosen point is the orthogonal projection of the midpoint: its local
    // offset must be parallel to the constraint normal.
    Eigen::Vector3d ux = RingOracle::e(x) - RingOracle::e(ring.o);
    double tang = (ux - ux.dot(nhat) * nhat).norm();
    VT_REQUIRE(tang <= 1e-9 * std::max(ux.norm(), 1.0),
               "midpoint projection is not normal to the constraint plane");

    // Any other feasible point (same plane, moved tangentially) is farther
    // from the midpoint and has the same QEM value: genuine tie handling.
    Eigen::Vector3d t1 = Eigen::Vector3d::UnitX().cross(nhat);
    if(t1.norm() < 1e-6) t1 = Eigen::Vector3d::UnitY().cross(nhat);
    t1.normalize();
    Eigen::Vector3d t2 = nhat.cross(t1);
    auto qem = [&](const Eigen::Vector3d &xx)
    {
      Eigen::Matrix3d M; Eigen::Vector3d bv;
      quadricMatrix(v0->Qd(), M, bv); // identical quadric on both vertices
      return (xx.transpose() * (2.0 * M) * xx)(0) + (2.0 * bv).dot(xx) + 2.0 * v0->Qd().c;
    };
    double qChosen = qem(RingOracle::e(x));
    double dMid = Distance(x, ring.o);
    for(Eigen::Vector3d t : { t1, t2 })
      for(double s : { -0.1, -0.02, 0.02, 0.1 })
      {
        Eigen::Vector3d xx = RingOracle::e(x) + s * t;
        Point3d xp(xx[0], xx[1], xx[2]);
        VT_REQUIRE(Distance(xp, ring.o) > dMid + 1e-9,
                   "a tangential feasible point beat the midpoint projection");
        VT_REQUIRE(std::abs(qem(xx) - qChosen) <= 1e-8 * std::max(1.0, std::abs(qChosen)),
                   "feasible continuum points must share the QEM minimum");
      }

    // Second rank pattern: rank-1 quadric normal to a tangent direction, so
    // the reduced 2x2 Hessian has exactly one zero singular value.
    Eigen::Vector3d tt = t1;
    Plane3d planeT;
    planeT.SetDirection(Point3d(tt[0], tt[1], tt[2]));
    planeT.Normalize();
    planeT.SetOffset(planeT.Direction().dot(v0->cP()));
    math::Quadric<double> qt; qt.ByPlane(planeT);
    v0->Qd() = qt; v1->Qd() = qt;
    RefPlacement refT = referenceSolve(v0, v1, ring, rs);
    VT_REQUIRE(refT.ok, "tangential rank-1 quadric must stay solvable");
    Point3d xT = productionPlacement(v0, v1, p);
    VT_REQUIRE(Distance(xT, refT.x) <= 1e-9, "rank-one reduced Hessian placement mismatch");
    VT_REQUIRE(std::abs(ring.n.dot(RingOracle::e(xT) - RingOracle::e(ring.o)) - ring.d)
               <= 1e-10 * rs * rs * rs, "tangential rank-1 point violates the equality");
    // Moving along the one remaining null direction cannot improve QEM and
    // must not land closer to the midpoint than the chosen point.
    Eigen::Matrix<double,3,2> Z; Z.col(0)=t1; Z.col(1)=t2;
    Eigen::Matrix3d MT; Eigen::Vector3d bT;
    quadricMatrix(qt, MT, bT);
    Eigen::Matrix2d H = Z.transpose() * (2.0 * MT) * Z;
    Eigen::JacobiSVD<Eigen::Matrix2d> svH(H, Eigen::ComputeFullV);
    VT_REQUIRE(svH.singularValues()(1) <= kRankEps * svH.singularValues()(0),
               "expected a single-null-direction reduced Hessian");
    Eigen::Vector3d nd3 = Z * svH.matrixV().col(1);
    double dMidT = Distance(xT, ring.o);
    for(double s : { -0.05, 0.05 })
    {
      Eigen::Vector3d xx = RingOracle::e(xT) + s * nd3;
      VT_REQUIRE(Distance(Point3d(xx[0],xx[1],xx[2]), ring.o) >= dMidT - 1e-10,
                 "rank-one null direction tie break not respected");
    }
  }

  // ---- (d) feasible identity equality n = 0, d = 0 with a rank def quadric
  {
    Mesh m; makeBipyramid(m, 1.0, 1.0); // equal apices: base ring cancels, d = 0
    UpdateTopology<Mesh>::VertexFace(m);
    installQuadrics(m, p);
    V *v0 = &m.vert[2], *v1 = &m.vert[3];
    RingOracle ring = collectRing(v0, v1);
    VT_REQUIRE(ring.n.norm() < 1e-12 && std::abs(ring.d) < 1e-10,
               "equal-height bipyramid should give the 0 = 0 identity");
    double rs = ringScaleOf(v0, v1, ring.o);
    RefPlacement ref = referenceSolve(v0, v1, ring, rs);
    VT_REQUIRE(ref.ok, "0 = 0 identity must be feasible");
    Point3d x = productionPlacement(v0, v1, p);
    VT_REQUIRE(Distance(x, ref.x) <= 1e-9, "0 = 0 identity placement mismatch");
  }
}

// ==========================================================================
// 3. live_collapse: drive the real LocalOptimization, check every accepted
//    collapse against volumes and topology rebuilt from surviving faces.
// ==========================================================================
void live_collapse()
{
  Mesh m; make_fixture(m);
  UpdateBounding<Mesh>::Box(m);
  double D = m.bbox.Diag();
  double vInit = shellVolume(m);
  int fInit = m.fn;

  TriEdgeCollapseQuadricParameter p;
  p.OptimalPlacement = p.PreserveTopology = p.PreserveVolume = true;
  LocalOptimization<Mesh> session(m, &p);
  session.Init<Collapse>();

  int accepted = 0;
  double prevVol = vInit;
  while(accepted < 24 && !session.h.empty())
  {
    std::pop_heap(session.h.begin(), session.h.end());
    auto elem = session.h.back();
    session.h.pop_back();
    Collapse *cand = dynamic_cast<Collapse*>(elem.locModPtr);
    VT_REQUIRE(cand != nullptr, "heap holds an unexpected local operation");

    if(!cand->IsUpToDate()) { delete elem.locModPtr; continue; }
    if(!cand->IsFeasible(&p)) { delete elem.locModPtr; continue; }

    V *v0 = cand->CollapsePair().cV(0), *v1 = cand->CollapsePair().cV(1);
    Point3d x = cand->optimalPos;

    // Independent pre-check of the chosen position on the live ring.
    RingOracle ring = collectRing(v0, v1);
    double rs = ringScaleOf(v0, v1, ring.o);
    if(ring.n.norm() > kRankEps * rs * rs)
      VT_REQUIRE(std::abs(ring.n.dot(RingOracle::e(x) - RingOracle::e(ring.o)) - ring.d)
                 <= 1e-10 * D * D * D, "live candidate violates its freshly built equality");
    VT_REQUIRE(std::isfinite(x[0]) && std::isfinite(x[1]) && std::isfinite(x[2]),
               "non-finite collapse position");

    int fBefore = m.fn;
    cand->Execute(m, &p);
    cand->UpdateHeap(session.h, &p);
    delete elem.locModPtr;
    VT_REQUIRE(m.fn < fBefore, "collapse did not remove faces");
    ++accepted;

    // Volume and topology rebuilt from scratch over surviving faces.
    double vol = shellVolume(m);
    VT_REQUIRE(std::abs(vol - prevVol) <= 1e-10 * D * D * D,
               "per-collapse volume error exceeds 1e-10 D^3");
    VT_REQUIRE(std::abs(vol - vInit) <= 1e-8 * D * D * D,
               "cumulative volume error exceeds 1e-8 D^3");
    assertClosedManifold(m);
    prevVol = vol;

    // Re-check a freshly created neighbour candidate against the new geometry.
    bool neighbourChecked = false;
    for(auto &he : session.h)
    {
      Collapse *nb = dynamic_cast<Collapse*>(he.locModPtr);
      if(!nb) continue;
      if(nb->CollapsePair().cV(0) != v1 && nb->CollapsePair().cV(1) != v1) continue;
      if(!nb->IsUpToDate()) continue;
      RingOracle rr = collectRing(nb->CollapsePair().cV(0), nb->CollapsePair().cV(1));
      double rrs = ringScaleOf(nb->CollapsePair().cV(0), nb->CollapsePair().cV(1), rr.o);
      if(rr.n.norm() <= kRankEps * rrs * rrs) continue;
      VT_REQUIRE(std::abs(rr.n.dot(RingOracle::e(nb->optimalPos) - RingOracle::e(rr.o)) - rr.d)
                 <= 1e-10 * D * D * D, "neighbour candidate stale relative to live geometry");
      neighbourChecked = true;
      break;
    }
    VT_REQUIRE(neighbourChecked, "no up-to-date neighbour candidate to re-check");
  }

  VT_REQUIRE(accepted >= 10, "fewer than ten successful collapses");
  VT_REQUIRE(m.fn <= fInit - (fInit + 2) / 3, "face count did not drop by at least 30%");
  VT_REQUIRE(std::abs(shellVolume(m) - vInit) <= 1e-8 * D * D * D, "final cumulative volume drift");
}

// ==========================================================================
// 4. shell_invariance: two shells, globally inverted shell, far translation
//    and three scales of the same asymmetric geometry.
// ==========================================================================
namespace {

struct ShellModel { Mesh mesh; double D; };

static void makeScaledFixture(ShellModel &sm, double scale, const Point3d &translate, bool flipSecond)
{
  Mesh one; make_fixture(one);
  Mesh two; make_fixture(two);
  if(flipSecond)
    for(F &f : two.face) std::swap(f.V(1), f.V(2));

  vcg::tri::Append<Mesh, Mesh>::Mesh(sm.mesh, one);
  vcg::tri::Append<Mesh, Mesh>::Mesh(sm.mesh, two);
  vcg::tri::UpdatePosition<Mesh>::Scale(sm.mesh, scale);
  vcg::tri::UpdatePosition<Mesh>::Translate(sm.mesh, translate);
  UpdateTopology<Mesh>::VertexFace(sm.mesh);
  UpdateFlags<Mesh>::FaceBorderFromVF(sm.mesh);
  UpdateBounding<Mesh>::Box(sm.mesh);
  sm.D = sm.mesh.bbox.Diag();
}

} // namespace

void shell_invariance()
{
  const Point3d far(1.0e6, -1.0e6, 5.0e5);
  for(double scale : {1e-3, 1.0, 1e3})
  {
    for(int farMode = 0; farMode < 2; ++farMode)
    {
      Point3d tr = farMode ? far : Point3d(0,0,0);
      ShellModel sm; makeScaledFixture(sm, scale, tr, /*flipSecond=*/true);
      auto before = perShellStats(sm.mesh);
      VT_REQUIRE(before.size() == size_t(2), "expected two disconnected shells");
      VT_REQUIRE(before[0].volume * before[1].volume < 0, "second shell should be inverted");

      TriEdgeCollapseQuadricParameter p;
      p.OptimalPlacement = p.PreserveTopology = p.PreserveVolume = true;
      LocalOptimization<Mesh> session(sm.mesh, &p);
      session.Init<Collapse>();
      size_t target = sm.mesh.fn / 2;
      session.SetTargetSimplices(int(target));
      session.DoOptimization();
      session.Finalize<Collapse>();

      auto after = perShellStats(sm.mesh);
      VT_REQUIRE(after.size() == size_t(2), "shells merged during simplification");
      assertClosedManifold(sm.mesh);
      for(int i=0;i<2;++i)
      {
        double D3 = before[i].diag * before[i].diag * before[i].diag;
        VT_REQUIRE(std::abs(after[i].volume - before[i].volume) <= 1e-8 * D3,
                   "per-shell cumulative volume drift out of tolerance");
        VT_REQUIRE(after[i].faces < before[i].faces, "shell was not simplified");
        VT_REQUIRE(before[i].volume * after[i].volume > 0, "shell orientation flipped");
      }
    }
  }

  // Per-collapse budget on the far-from-origin instance: large-number
  // cancellation would blow this immediately without the local frame.
  ShellModel sm; makeScaledFixture(sm, 1.0, far, true);
  auto before = perShellStats(sm.mesh);
  TriEdgeCollapseQuadricParameter p;
  p.OptimalPlacement = p.PreserveTopology = p.PreserveVolume = true;
  LocalOptimization<Mesh> session(sm.mesh, &p);
  session.Init<Collapse>();
  std::vector<double> prevVol(2);
  for(auto &s : before) prevVol.push_back(s.volume);
  prevVol.clear(); for(auto &s : before) prevVol.push_back(s.volume);
  int done = 0;
  while(done < 12 && !session.h.empty())
  {
    std::pop_heap(session.h.begin(), session.h.end());
    auto elem = session.h.back(); session.h.pop_back();
    Collapse *cand = dynamic_cast<Collapse*>(elem.locModPtr);
    if(cand->IsUpToDate() && cand->IsFeasible(&p))
    {
      cand->Execute(sm.mesh, &p);
      cand->UpdateHeap(session.h, &p);
      ++done;
      auto now = perShellStats(sm.mesh);
      for(size_t i=0;i<now.size();++i)
      {
        double D3 = before[i].diag * before[i].diag * before[i].diag;
        VT_REQUIRE(std::abs(now[i].volume - prevVol[i]) <= 1e-10 * D3,
                   "far-origin per-collapse volume error");
        prevVol[i] = now[i].volume;
      }
    }
    delete elem.locModPtr;
  }
  VT_REQUIRE(done >= 10, "far-origin run did not perform enough collapses");
}

// ==========================================================================
// 5. rejection: early invalid_argument, untouched state, candidate rejection
//    does not prevent legal simplification.
// ==========================================================================
void rejection()
{
  auto expectThrow = [](Mesh &m, const TriEdgeCollapseQuadricParameter &pp, const char *what)
  {
    TriEdgeCollapseQuadricParameter p = pp;
    std::vector<int> flags;
    std::vector<char> valid;
    for(V &v : m.vert) { flags.push_back(v.Flags()); valid.push_back(v.Qd().IsValid()); }
    bool threw = false;
    try
    {
      LocalOptimization<Mesh> session(m, &p);
      session.Init<Collapse>();
    }
    catch(const std::invalid_argument &) { threw = true; }
    VT_REQUIRE(threw, what);
    size_t i = 0;
    for(V &v : m.vert)
    {
      VT_REQUIRE(int(v.Flags()) == flags[i], "failed Init changed vertex flags");
      VT_REQUIRE(v.Qd().IsValid() == (valid[i] != 0), "failed Init created/modified a quadric");
      ++i;
    }
  };

  TriEdgeCollapseQuadricParameter good;
  good.OptimalPlacement = good.PreserveTopology = good.PreserveVolume = true;

  // Parameter combinations.
  {
    Mesh m; make_fixture(m);
    TriEdgeCollapseQuadricParameter p = good; p.OptimalPlacement = false;
    expectThrow(m, p, "OptimalPlacement=false must be rejected");
  }
  {
    Mesh m; make_fixture(m);
    TriEdgeCollapseQuadricParameter p = good; p.PreserveTopology = false;
    expectThrow(m, p, "PreserveTopology=false must be rejected");
  }

  // Open boundary: remove one face.
  {
    Mesh m; make_fixture(m);
    Allocator<Mesh>::DeleteFace(m, m.face[0]);
    UpdateTopology<Mesh>::VertexFace(m);
    expectThrow(m, good, "open mesh must be rejected");
  }

  // Degenerate (zero-area) face.
  {
    Mesh m; make_fixture(m);
    V *w = m.face[0].V(1);
    w->P() = m.face[0].V(2)->P();
    expectThrow(m, good, "degenerate face must be rejected");
  }

  // Non-finite coordinate.
  {
    Mesh m; make_fixture(m);
    m.vert[0].P()[0] = std::numeric_limits<double>::infinity();
    expectThrow(m, good, "non-finite coordinate must be rejected");
  }

  // Non-manifold: two closed tetrahedral shells pinched at one vertex.
  {
    Mesh m;
    {
      Mesh a; vcg::tri::Tetrahedron(a);
      Mesh b; vcg::tri::Tetrahedron(b);
      vcg::tri::Append<Mesh,Mesh>::Mesh(m, a);
      vcg::tri::Append<Mesh,Mesh>::Mesh(m, b);
    }
    V *pinch = &m.vert[0];
    V *other = &m.vert[4]; // first vertex of the second tetrahedron
    for(F &f : m.face)
      for(int j=0;j<3;++j)
        if(f.V(j) == other) f.V(j) = pinch;
    UpdateTopology<Mesh>::VertexFace(m);
    expectThrow(m, good, "pinched vertex must be rejected as non-manifold");
  }

  // Candidate-level rejection still leaves a usable simplifier. The
  // bipyramid base edges have n = 0, d != 0 (rejected), the apex edges are
  // regular and must collapse while preserving the shell volume.
  {
    Mesh m; makeBipyramid(m, 2.0, -2.0);
    UpdateBounding<Mesh>::Box(m);
    double D = m.bbox.Diag();
    double v0 = shellVolume(m);
    LocalOptimization<Mesh> session(m, &good);
    session.Init<Collapse>();
    int rejected = 0, accepted = 0;
    while(!session.h.empty() && accepted < 2)
    {
      std::pop_heap(session.h.begin(), session.h.end());
      auto elem = session.h.back(); session.h.pop_back();
      Collapse *cand = dynamic_cast<Collapse*>(elem.locModPtr);
      if(!cand->IsUpToDate()) { delete elem.locModPtr; continue; }
      if(!cand->IsFeasible(&good)) { ++rejected; delete elem.locModPtr; continue; }
      cand->Execute(m, &good);
      cand->UpdateHeap(session.h, &good);
      delete elem.locModPtr;
      ++accepted;
    }
    VT_REQUIRE(rejected >= 1, "incompatible ring candidates were not rejected");
    VT_REQUIRE(accepted >= 1, "legal candidates stopped running after rejections");
    VT_REQUIRE(std::abs(shellVolume(m) - v0) <= 1e-8 * D * D * D,
               "volume changed despite the preserving mode");
    assertClosedManifold(m);
  }
}

// ==========================================================================
// 6. compatibility: PreserveVolume defaults off and reproduces the recorded
//    stock library result byte for byte.
// ==========================================================================
void compatibility()
{
  VT_REQUIRE(TriEdgeCollapseQuadricParameter().PreserveVolume == false,
             "PreserveVolume must default to false");

  Mesh m; make_fixture(m);
  const double before = shellVolume(m);
  int nf = m.fn;
  TriEdgeCollapseQuadricParameter p;
  p.PreserveTopology = true; // matches checks/baseline.cpp, PreserveVolume stays false
  LocalOptimization<Mesh> session(m, &p);
  session.Init<Collapse>();
  session.SetTargetSimplices(40);
  session.DoOptimization();
  session.Finalize<Collapse>();
  double after = shellVolume(m);

  VT_REQUIRE(m.fn < nf, "default pipeline stopped reducing");
  VT_REQUIRE(std::abs(before - after) >= 1e-4, "default pipeline lost its QEM drift signature");

  // Compare against the shipped record of the original library behavior.
  std::ifstream in("checks/default-baseline.txt");
  VT_REQUIRE(in.good(), "cannot open checks/default-baseline.txt");
  std::map<int, Point3d> refV;
  std::map<int, std::array<int,3>> refF;
  std::string line;
  while(std::getline(in, line))
  {
    std::istringstream ss(line);
    std::string tag; ss >> tag;
    if(tag == "V")
    {
      int i; double x,y,z; ss >> i >> x >> y >> z;
      refV[i] = Point3d(x,y,z);
    }
    else if(tag == "F")
    {
      int i,a,b,c; ss >> i >> a >> b >> c;
      refF[i] = {a,b,c};
    }
  }
  VT_REQUIRE(!refV.empty() && !refF.empty(), "baseline record could not be parsed");

  size_t liveV = 0;
  for(size_t i=0;i<m.vert.size();++i)
  {
    if(m.vert[i].IsD()) continue;
    ++liveV;
    auto it = refV.find(int(i));
    VT_REQUIRE(it != refV.end(), "off-mode vertex set differs from baseline");
    VT_REQUIRE(Distance(m.vert[i].cP(), it->second) <= 1e-12,
               "off-mode vertex coordinate differs from baseline");
  }
  VT_REQUIRE(liveV == refV.size(), "off-mode vertex count differs");
  size_t liveF = 0;
  for(size_t i=0;i<m.face.size();++i)
  {
    if(m.face[i].IsD()) continue;
    ++liveF;
    auto it = refF.find(int(i));
    VT_REQUIRE(it != refF.end(), "off-mode face set differs from baseline");
    for(int j=0;j<3;++j)
      VT_REQUIRE(m.face[i].cV(j) == &m.vert[it->second[j]],
                 "off-mode face connectivity differs from baseline");
  }
  VT_REQUIRE(liveF == refF.size(), "off-mode face count differs");
}

} // namespace volume_tests
