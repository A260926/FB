// This file is part of libigl, a simple c++ geometry processing library.
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "qslim_operator.h"

#include "COLLAPSE_EDGE_NULL.h"
#include "collapse_edge.h"
#include "collapse_least_cost_edge.h"
#include "decimate_callback_types.h"
#include "edge_flaps.h"
#include "min_heap.h"
#include "per_vertex_point_to_plane_quadrics.h"
#include "quadric_binary_plus_operator.h"
#include "remove_unreferenced.h"

#include <Eigen/LU>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
  using Quadric = std::tuple<Eigen::MatrixXd,Eigen::RowVectorXd,double>;

  // Validate the mesh/operator contract. Throws std::invalid_argument on the
  // first violated requirement. Outputs are not touched by the caller until
  // this returns.
  void validate_inputs(
    const Eigen::MatrixXd & V,
    const Eigen::MatrixXi & F,
    const Eigen::SparseMatrix<double> & L0,
    const int max_m,
    const double lambda)
  {
    if(V.size()==0 || F.size()==0)
      throw std::invalid_argument("qslim_operator: empty mesh");
    if(V.cols()!=3)
      throw std::invalid_argument("qslim_operator: V must be #V by 3");
    if(F.cols()!=3)
      throw std::invalid_argument("qslim_operator: F must be #F by 3");
    if(!V.allFinite())
      throw std::invalid_argument("qslim_operator: V contains non-finite values");
    if(max_m < 4)
      throw std::invalid_argument("qslim_operator: max_m must be >= 4");
    if(!std::isfinite(lambda) || lambda < 0.0)
      throw std::invalid_argument("qslim_operator: lambda must be finite and >= 0");
    const int n = V.rows();
    const int m = F.rows();
    if(m < 4)
      throw std::invalid_argument("qslim_operator: need at least 4 faces");
    // Face validity: indices in range, distinct corners, positive area.
    Eigen::Array<bool,Eigen::Dynamic,1> referenced =
      Eigen::Array<bool,Eigen::Dynamic,1>::Zero(n);
    // Directed edge -> number of occurrences (a closed oriented manifold has
    // each directed edge exactly once).
    std::unordered_map<long,int> directed;
    directed.reserve(3*m*2);
    const auto edge_key = [](int a,int b) -> long
    {
      return (long(a)<<32) ^ long(unsigned(b));
    };
    for(int f = 0;f<m;f++)
    {
      const int a = F(f,0), b = F(f,1), c = F(f,2);
      if(a<0 || b<0 || c<0 || a>=n || b>=n || c>=n)
        throw std::invalid_argument("qslim_operator: face index out of range");
      if(a==b || b==c || c==a)
        throw std::invalid_argument("qslim_operator: degenerate face corner");
      referenced(a)=referenced(b)=referenced(c)=true;
      const Eigen::RowVector3d p = V.row(a), q = V.row(b), r = V.row(c);
      const double area2 = (q-p).cross(r-p).norm();
      if(!(area2 > 0.0) || !std::isfinite(area2))
        throw std::invalid_argument("qslim_operator: non-positive-area face");
      const int es[3][2] = {{a,b},{b,c},{c,a}};
      for(int k = 0;k<3;k++)
      {
        const long key = edge_key(es[k][0],es[k][1]);
        if(++directed[key] > 1)
          throw std::invalid_argument(
            "qslim_operator: mesh is not a closed oriented 2-manifold");
      }
    }
    for(int v = 0;v<n;v++)
      if(!referenced(v))
        throw std::invalid_argument("qslim_operator: isolated vertex");
    // Every undirected edge must occur exactly twice, in opposite directions.
    std::unordered_set<long> checked;
    checked.reserve(directed.size());
    for(const auto & kv : directed)
    {
      if(checked.count(kv.first)) continue;
      const int a = int(kv.first >> 32);
      const int b = int(kv.first & 0xffffffffL);
      const long rev = edge_key(b,a);
      const auto it = directed.find(rev);
      if(it==directed.end() || it->second!=1)
        throw std::invalid_argument(
          "qslim_operator: mesh is not a closed oriented 2-manifold");
      checked.insert(rev);
    }
    // Operator validity.
    if(L0.rows()!=n || L0.cols()!=n)
      throw std::invalid_argument("qslim_operator: L0 must be #V by #V");
    for(int k=0;k<L0.outerSize();k++)
      for(Eigen::SparseMatrix<double>::InnerIterator it(L0,k); it; ++it)
        if(!std::isfinite(it.value()))
          throw std::invalid_argument("qslim_operator: L0 contains non-finite values");
    // Geometric edge set, keyed by ordered (min,max).
    std::unordered_set<long> geom;
    geom.reserve(directed.size());
    for(const auto & kv : directed)
    {
      const int a = int(kv.first >> 32);
      const int b = int(kv.first & 0xffffffffL);
      const int lo = std::min(a,b), hi = std::max(a,b);
      geom.insert((long(lo)<<32) ^ long(unsigned(hi)));
    }
    const double sym_tol = 1e-9;
    const double sum_tol = 1e-8;
    Eigen::VectorXd row_sum = Eigen::VectorXd::Zero(n);
    for(int k=0;k<L0.outerSize();k++)
    {
      for(Eigen::SparseMatrix<double>::InnerIterator it(L0,k); it; ++it)
      {
        const int i = it.row(), j = it.col();
        const double v = it.value();
        row_sum(i) += v;
        if(i==j) continue;
        if(v >= 0.0)
          throw std::invalid_argument(
            "qslim_operator: L0 off-diagonal entries must be negative conductances");
        const double w = L0.coeff(j,i);
        if(!std::isfinite(w) || std::abs(w-v) > sym_tol)
          throw std::invalid_argument("qslim_operator: L0 must be symmetric");
        const int lo = std::min(i,j), hi = std::max(i,j);
        if(!geom.count((long(lo)<<32) ^ long(unsigned(hi))))
          throw std::invalid_argument(
            "qslim_operator: L0 has conductance on a non-geometric edge");
      }
    }
    if(row_sum.array().abs().maxCoeff() > sum_tol)
      throw std::invalid_argument(
        "qslim_operator: L0 diagonal must be the sum of conductances");
    for(int v = 0;v<n;v++)
      if(!(L0.coeff(v,v) > 0.0))
        throw std::invalid_argument(
          "qslim_operator: L0 diagonal conductances must be positive");
    // Every geometric edge carries a positive conductance.
    for(const long key : geom)
    {
      const int a = int(key >> 32);
      const int b = int(key & 0xffffffffL);
      if(!(L0.coeff(a,b) < 0.0))
        throw std::invalid_argument(
          "qslim_operator: every geometric edge needs a positive conductance");
    }
  }

  // One recorded sparse elimination: r was removed and its value is the
  // weighted combination of the vertices active at that time.
  struct Elimination
  {
    int r;
    std::vector<std::pair<int,double>> weights;
  };
}

IGL_INLINE bool igl::qslim_operator(
  const Eigen::MatrixXd & V,
  const Eigen::MatrixXi & F,
  const Eigen::SparseMatrix<double> & L0,
  const int max_m,
  const double lambda,
  Eigen::MatrixXd & U,
  Eigen::MatrixXi & G,
  Eigen::VectorXi & J,
  Eigen::VectorXi & I,
  Eigen::SparseMatrix<double> & K,
  Eigen::SparseMatrix<double> & P,
  qslim_operator_trace * trace)
{
  validate_inputs(V,F,L0,max_m,lambda);

  const int n = V.rows();
  // Working geometry (copies; the input mesh is never modified).
  Eigen::MatrixXd Vw = V;
  Eigen::MatrixXi Fw = F;
  Eigen::VectorXi EMAP;
  Eigen::MatrixXi E,EF,EI;
  edge_flaps(Fw,E,EMAP,EF,EI);
  const int ne = E.rows();

  // QSlim quadrics, identical to igl::qslim on a closed mesh.
  std::vector<Quadric> quadrics;
  per_vertex_point_to_plane_quadrics(Vw,Fw,EMAP,EF,EI,quadrics);

  // Active Schur operator, grown only through actual fill-in (never a dense
  // n*n allocation).
  Eigen::SparseMatrix<double> S = L0;
  std::vector<char> active(n,1);

  // Geometric edge incidence per vertex, maintained across collapses.
  std::vector<std::unordered_set<int>> v2e(n);
  for(int e = 0;e<ne;e++)
  {
    v2e[E(e,0)].insert(e);
    v2e[E(e,1)].insert(e);
  }

  // Sparse recovery relations for the eventual prolongation.
  std::vector<Elimination> eliminations;
  eliminations.reserve(n);

  // Per-pop bookkeeping shared with the callbacks.
  int pop_s = -1, pop_r = -1;
  std::vector<int> snap_edges;
  std::unordered_map<int,std::pair<int,int>> snap_endpoints;
  std::vector<int> last_affected;
  std::vector<double> qem_cache(ne,0.0), pen_cache(ne,0.0);

  // Original QEM placement/cost plus the Schur operator penalty. With
  // lambda == 0 the cost, placement and heap order match igl::qslim exactly.
  decimate_cost_and_placement_callback cost_and_placement =
    [&](const int e,
        const Eigen::MatrixXd &,
        const Eigen::MatrixXi &,
        const Eigen::MatrixXi & Ec,
        const Eigen::VectorXi &,
        const Eigen::MatrixXi &,
        const Eigen::MatrixXi &,
        double & cost,
        Eigen::RowVectorXd & p)
  {
    const int a = Ec(e,0), b = Ec(e,1);
    const Quadric qp = quadrics[a] + quadrics[b];
    const auto & A = std::get<0>(qp);
    const auto & bb = std::get<1>(qp);
    const double c = std::get<2>(qp);
    double qem;
    if(bb.array().isInf().any())
    {
      qem = std::numeric_limits<double>::infinity();
      p.resizeLike(bb);
      p.setConstant(std::numeric_limits<double>::quiet_NaN());
    }else
    {
      p = -bb*A.inverse();
      qem = p.dot(p*A) + 2*p.dot(bb) + c;
    }
    if(std::isinf(qem) || qem!=qem)
    {
      qem = std::numeric_limits<double>::infinity();
      p.setConstant(0);
    }
    double penalty = 0.0;
    if(lambda > 0.0 && std::isfinite(qem))
    {
      const int s = std::min(a,b), r = std::max(a,b);
      const double Srr = S.coeff(r,r);
      // v = e_s + S_rA / S_rr over the active set A. Fill-in entries in
      // column r (not only the geometric one-ring) are all visited.
      double sumsq = 0.0;
      bool found_s = false;
      for(Eigen::SparseMatrix<double>::InnerIterator it(S,r); it; ++it)
      {
        const int k = it.row();
        if(k==r || !active[k]) continue;
        double vk = it.value()/Srr;
        if(k==s){ vk += 1.0; found_s = true; }
        sumsq += vk*vk;
      }
      if(!found_s) sumsq += 1.0;
      penalty = lambda*Srr*sumsq;
      if(!std::isfinite(penalty))
        penalty = std::numeric_limits<double>::infinity();
    }
    qem_cache[e] = qem;
    pen_cache[e] = penalty;
    cost = std::isfinite(qem) ? qem + penalty :
      std::numeric_limits<double>::infinity();
  };

  decimate_pre_collapse_callback pre_collapse =
    [&](const Eigen::MatrixXd &,
        const Eigen::MatrixXi &,
        const Eigen::MatrixXi & Ec,
        const Eigen::VectorXi &,
        const Eigen::MatrixXi &,
        const Eigen::MatrixXi &,
        const igl::min_heap<std::tuple<double,int,int>> &,
        const Eigen::VectorXi &,
        const Eigen::MatrixXd &,
        const int e) -> bool
  {
    // Edge rows may be stored in either orientation; the collapse always
    // keeps the lesser index and removes the greater one.
    pop_s = std::min(Ec(e,0),Ec(e,1));
    pop_r = std::max(Ec(e,0),Ec(e,1));
    // Snapshot every geometric edge touching either endpoint so the
    // incidence structure can be repaired after the collapse.
    snap_edges.clear();
    snap_endpoints.clear();
    snap_edges.reserve(v2e[pop_s].size()+v2e[pop_r].size());
    for(const int x : v2e[pop_s]) snap_edges.push_back(x);
    for(const int x : v2e[pop_r]) snap_edges.push_back(x);
    std::sort(snap_edges.begin(),snap_edges.end());
    snap_edges.erase(std::unique(snap_edges.begin(),snap_edges.end()),
      snap_edges.end());
    for(const int x : snap_edges)
      snap_endpoints[x] = {Ec(x,0),Ec(x,1)};
    return true;
  };

  decimate_post_collapse_callback post_collapse =
    [&](const Eigen::MatrixXd &,
        const Eigen::MatrixXi &,
        const Eigen::MatrixXi &,
        const Eigen::VectorXi &,
        const Eigen::MatrixXi &,
        const Eigen::MatrixXi &,
        const igl::min_heap<std::tuple<double,int,int>> &,
        const Eigen::VectorXi &,
        const Eigen::MatrixXd &,
        const int e,
        const int e1,
        const int e2,
        const int,
        const int,
        const bool collapsed)
  {
    if(!collapsed) return;
    const int s = pop_s, r = pop_r;

    // Quadric merge, exactly as in qslim_optimal_collapse_edge_callbacks.
    quadrics[s] = quadrics[s] + quadrics[r];

    // Exact Schur elimination of r over the current active set. The active
    // column r is authoritative: it contains every fill edge produced by
    // earlier eliminations, not only the geometric one-ring.
    const double Srr = S.coeff(r,r);
    std::vector<int> nbr;
    std::vector<double> cr;
    for(Eigen::SparseMatrix<double>::InnerIterator it(S,r); it; ++it)
    {
      const int k = it.row();
      if(k==r || !active[k]) continue;
      nbr.push_back(k);
      cr.push_back(it.value());
    }
    Elimination rec;
    rec.r = r;
    rec.weights.reserve(nbr.size());
    for(size_t i = 0;i<nbr.size();i++)
      rec.weights.push_back({nbr[i], -cr[i]/Srr});
    for(size_t i = 0;i<nbr.size();i++)
    {
      for(size_t j = 0;j<nbr.size();j++)
      {
        S.coeffRef(nbr[i],nbr[j]) -= cr[i]*cr[j]/Srr;
      }
    }
    // Retire r (exact zeros are pruned once after the whole run).
    for(const int k : nbr)
    {
      S.coeffRef(k,r) = 0.0;
      S.coeffRef(r,k) = 0.0;
    }
    S.coeffRef(r,r) = 0.0;
    active[r] = 0;
    eliminations.push_back(std::move(rec));

    // Repair geometric edge incidence: drop killed edges, reassign r's
    // surviving edges to s.
    const auto is_killed = [e,e1,e2](const int x)
    {
      return x==e || x==e1 || x==e2;
    };
    for(const int x : snap_edges)
    {
      const auto ab = snap_endpoints[x];
      if(is_killed(x))
      {
        v2e[ab.first].erase(x);
        v2e[ab.second].erase(x);
      }else if(ab.first==r || ab.second==r)
      {
        v2e[r].erase(x);
        v2e[s].insert(x);
      }
    }

    // Rows touched by the elimination (includes s and every fill neighbor).
    last_affected = nbr;
  };

  // Geometrically distant candidate edges depend on the changed Schur rows
  // through fill-in: collect their edge ids and let the heap refresh them
  // together with the geometric one-ring.
  decimate_extra_refresh_edges_callback extra_refresh =
    [&](const int,const int,const int,const int,const int,
        const std::vector<int> &,
        std::vector<int> & extra_edges)
  {
    for(const int v : last_affected)
    {
      if(!active[v]) continue;
      for(const int x : v2e[v]) extra_edges.push_back(x);
    }
  };

  // Heap and timestamp machinery, mirroring igl::decimate.
  igl::min_heap<std::tuple<double,int,int>> Q;
  Eigen::VectorXi EQ = Eigen::VectorXi::Zero(ne);
  Eigen::MatrixXd C(ne,Vw.cols());
  for(int e = 0;e<ne;e++)
  {
    double cost;
    Eigen::RowVectorXd p(1,3);
    cost_and_placement(e,Vw,Fw,E,EMAP,EF,EI,cost,p);
    C.row(e) = p;
    Q.emplace(cost,e,0);
  }

  if(trace)
  {
    trace->entries.clear();
    trace->max_m = max_m;
  }
  int face_count = Fw.rows();
  bool reached = face_count <= max_m;
  int prev_e = -1;

  while(!reached)
  {
    int e,e1,e2,f1,f2;
    int num_extra = 0;
    const bool collapsed = collapse_least_cost_edge(
      cost_and_placement,pre_collapse,post_collapse,extra_refresh,
      Vw,Fw,E,EMAP,EF,EI,Q,EQ,C,e,e1,e2,f1,f2,&num_extra);
    if(e == -1) break;
    const bool finite_cost = std::isfinite(qem_cache[e]);
    if(trace)
    {
      qslim_operator_trace_entry te;
      te.s = pop_s;
      te.r = pop_r;
      te.qem_cost = qem_cache[e];
      te.operator_penalty = finite_cost ? pen_cache[e] : 0.0;
      te.total_cost = finite_cost ?
        te.qem_cost + te.operator_penalty :
        std::numeric_limits<double>::infinity();
      te.success = collapsed;
      te.num_extra_refreshed = collapsed ? num_extra : 0;
      trace->entries.push_back(te);
    }
    if(collapsed)
    {
      face_count -= 2;
      if(face_count <= max_m)
      {
        reached = true;
        break;
      }
      prev_e = -1;
    }else
    {
      // Same edge failing twice in a row means no progress is possible.
      if(prev_e == e) break;
      prev_e = e;
    }
  }

  // Compact the surviving faces. Face rows are never reordered, so the row
  // index is the birth face (J).
  Eigen::MatrixXi G0(face_count,3);
  Eigen::VectorXi J0(face_count);
  int gf = 0;
  for(int f = 0;f<Fw.rows();f++)
  {
    if(Fw(f,0)!=IGL_COLLAPSE_EDGE_NULL ||
       Fw(f,1)!=IGL_COLLAPSE_EDGE_NULL ||
       Fw(f,2)!=IGL_COLLAPSE_EDGE_NULL)
    {
      G0.row(gf) = Fw.row(f);
      J0(gf) = f;
      gf++;
    }
  }
  G0.conservativeResize(gf,3);
  J0.conservativeResize(gf);
  Eigen::VectorXi remap, kept;
  remove_unreferenced(Vw,G0,U,G,remap,kept);
  J = J0;
  I = kept;
  const int nu = kept.size();

  // Drop exact zeros left by retired rows so storage counts actual nonzeros.
  S.prune([](int,int,double v){ return v != 0.0; });
  S.makeCompressed();

  // K: active Schur block in the order of U's rows.
  std::vector<Eigen::Triplet<double>> kt;
  for(int a = 0;a<nu;a++)
  {
    const int ia = kept(a);
    for(Eigen::SparseMatrix<double>::InnerIterator it(S,ia); it; ++it)
    {
      const int ib = remap(it.row());
      if(ib < 0) continue;
      kt.emplace_back(ib,a,it.value());
    }
  }
  K.resize(nu,nu);
  K.setFromTriplets(kt.begin(),kt.end());

  // P: unfold the sparse recovery relations once, in reverse elimination
  // order. Rows are sparse vectors indexed by original vertices; no full P is
  // ever copied during elimination.
  std::vector<Eigen::SparseVector<double>> prows(
    n,Eigen::SparseVector<double>(n));
  for(int a = 0;a<nu;a++)
    prows[kept(a)].insert(kept(a)) = 1.0;
  for(auto it = eliminations.rbegin(); it!=eliminations.rend(); ++it)
  {
    auto & row = prows[it->r];
    for(const auto & kv : it->weights)
    {
      const int k = kv.first;
      const double w = kv.second;
      for(Eigen::SparseVector<double>::InnerIterator jt(prows[k]); jt; ++jt)
        row.coeffRef(jt.index()) += w*jt.value();
    }
  }
  std::vector<Eigen::Triplet<double>> pt;
  for(int v = 0;v<n;v++)
  {
    for(Eigen::SparseVector<double>::InnerIterator it(prows[v]); it; ++it)
    {
      const int c = remap(it.index());
      if(c >= 0) pt.emplace_back(v,c,it.value());
    }
  }
  P.resize(n,nu);
  P.setFromTriplets(pt.begin(),pt.end());

  if(trace)
  {
    trace->num_collapsed = int(eliminations.size());
    trace->final_m = gf;
    trace->reached = reached;
  }
  return reached;
}
