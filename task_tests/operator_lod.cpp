// Task tests for igl::qslim_operator. Registered through task_support/check.h;
// the test driver (task_support/driver.c) supplies main.
#include "task_support/check.h"

#include <igl/qslim_operator.h>
#include <igl/qslim.h>
#include <igl/decimate.h>
#include <igl/adjacency_matrix.h>
#include <igl/loop.h>
#include <igl/edge_flaps.h>
#include <igl/writeOBJ.h>

#include <Eigen/Core>
#include <Eigen/Sparse>
#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <vector>

namespace
{
  using Eigen::MatrixXd;
  using Eigen::MatrixXi;
  using Eigen::VectorXi;
  using Eigen::VectorXd;
  using Eigen::SparseMatrix;

  // Regular octahedron: 6 vertices, 8 oriented faces.
  MatrixXd octa_V()
  {
    MatrixXd V(6,3);
    V << 1,0,0, -1,0,0, 0,1,0, 0,-1,0, 0,0,1, 0,0,-1;
    return V;
  }
  MatrixXi octa_F()
  {
    MatrixXi F(8,3);
    F << 0,2,4, 2,1,4, 1,3,4, 3,0,4,
         2,0,5, 1,2,5, 3,1,5, 0,3,5;
    return F;
  }

  // Sparse Laplacian with conductance w_e on every geometric edge.
  SparseMatrix<double> make_laplacian(
    const MatrixXi & F, const int n,
    const std::function<double(int,int)> & conductance)
  {
    MatrixXi E,EF,EI; VectorXi EMAP;
    igl::edge_flaps(F,E,EMAP,EF,EI);
    std::vector<Eigen::Triplet<double>> trips;
    std::vector<double> diag(n,0.0);
    for(int e = 0;e<E.rows();e++)
    {
      const int a = E(e,0), b = E(e,1);
      const double w = conductance(a,b);
      trips.emplace_back(a,b,-w);
      trips.emplace_back(b,a,-w);
      diag[a] += w; diag[b] += w;
    }
    for(int v = 0;v<n;v++) trips.emplace_back(v,v,diag[v]);
    SparseMatrix<double> L(n,n);
    L.setFromTriplets(trips.begin(),trips.end());
    return L;
  }
  SparseMatrix<double> uniform_laplacian(const MatrixXi & F,const int n)
  {
    return make_laplacian(F,n,[](int,int){ return 1.0; });
  }

  // Dense, independent reference for one Schur elimination sequence.
  struct DenseSchur
  {
    MatrixXd S;
    std::vector<char> active;
    DenseSchur(const SparseMatrix<double> & L0):
      S(MatrixXd(L0)), active(L0.rows(),true) {}
    double penalty(const int r,const int s) const
    {
      const double Srr = S(r,r);
      double sumsq = 0.0;
      for(int k = 0;k<S.rows();k++)
      {
        if(!active[k] || k==r) continue;
        const double vk = S(r,k)/Srr + (k==s ? 1.0 : 0.0);
        sumsq += vk*vk;
      }
      return Srr*sumsq;
    }
    void eliminate(const int r)
    {
      const int n = S.rows();
      const double Srr = S(r,r);
      for(int i = 0;i<n;i++)
      {
        if(!active[i] || i==r) continue;
        for(int j = 0;j<n;j++)
        {
          if(!active[j] || j==r) continue;
          S(i,j) -= S(i,r)*S(r,j)/Srr;
        }
      }
      active[r] = false;
    }
  };

  // Independent dense prolongation reference: solve the harmonic system
  // directly. With E the eliminated rows and B the kept rows,
  //   L_EE * P_EB = -L_EB,  P_BB = I.
  // This does not reuse any elimination bookkeeping from the implementation.
  MatrixXd harmonic_prolongation(
    const SparseMatrix<double> & L0,
    const VectorXi & kept)
  {
    const int n = L0.rows();
    const int nb = kept.size();
    std::vector<char> is_kept(n,false);
    std::vector<int> elim;
    for(int i = 0;i<nb;i++) is_kept[kept(i)] = true;
    for(int v = 0;v<n;v++) if(!is_kept[v]) elim.push_back(v);
    const int ne = int(elim.size());
    MatrixXd LEE = MatrixXd::Zero(ne,ne);
    MatrixXd LEB = MatrixXd::Zero(ne,nb);
    const MatrixXd Ld(L0);
    for(int a = 0;a<ne;a++)
      for(int b = 0;b<ne;b++)
        LEE(a,b) = Ld(elim[a],elim[b]);
    for(int a = 0;a<ne;a++)
      for(int b = 0;b<nb;b++)
        LEB(a,b) = Ld(elim[a],kept(b));
    MatrixXd PE = LEE.ldlt().solve(-LEB);
    MatrixXd Pd = MatrixXd::Zero(n,nb);
    for(int a = 0;a<ne;a++) Pd.row(elim[a]) = PE.row(a);
    for(int b = 0;b<nb;b++) Pd(kept(b),b) = 1.0;
    return Pd;
  }

  void subdivided_octa(MatrixXd & V,MatrixXi & F,const int steps)
  {
    igl::loop(octa_V(),octa_F(),V,F,steps);
  }

  constexpr double TOL = 1e-9;
}

// ---------------------------------------------------------------- schur group
static int schur_dense_penalties()
{
  MatrixXd V = octa_V(); MatrixXi F = octa_F();
  const int n = V.rows();
  SparseMatrix<double> L0 = uniform_laplacian(F,n);
  MatrixXd U; MatrixXi G; VectorXi J,I;
  SparseMatrix<double> K,P;
  igl::qslim_operator_trace trace;
  const double lambda = 3.7;
  const bool reached = igl::qslim_operator(V,F,L0,4,lambda,U,G,J,I,K,P,&trace);
  TASK_CHECK(reached);
  TASK_CHECK(G.rows()==4);
  TASK_CHECK(trace.entries.size() >= 2);

  // Every popped candidate's penalty must match an independent dense Schur.
  DenseSchur ref(L0);
  int collapses = 0;
  for(const auto & te : trace.entries)
  {
    TASK_CHECK(te.r > te.s);
    const double expect_pen = lambda*ref.penalty(te.r,te.s);
    TASK_CHECK(std::abs(expect_pen-te.operator_penalty) <
      TOL*std::max(1.0,std::abs(expect_pen)));
    TASK_CHECK(std::abs((te.qem_cost+te.operator_penalty)-te.total_cost) <
      TOL*std::max(1.0,std::abs(te.total_cost)));
    if(te.success)
    {
      ref.eliminate(te.r);
      collapses++;
    }
  }
  TASK_CHECK(collapses == trace.num_collapsed);
  TASK_CHECK(collapses == n - U.rows());

  // K equals the dense active Schur block reordered to U's row order.
  VectorXi remap = VectorXi::Constant(n,-1);
  for(int c = 0;c<I.size();c++) remap(I(c)) = c;
  for(int a = 0;a<I.size();a++)
    for(int b = 0;b<I.size();b++)
    {
      TASK_CHECK(std::isfinite(K.coeff(a,b)));
      TASK_CHECK(std::abs(ref.S(I(a),I(b))-K.coeff(a,b)) <
        TOL*std::max(1.0,std::abs(ref.S(I(a),I(b)))));
    }

  // P matches the independently solved harmonic prolongation.
  const MatrixXd Pd = harmonic_prolongation(L0,I);
  const MatrixXd Pm(P);
  TASK_CHECK(Pm.rows()==n && Pm.cols()==I.size());
  TASK_CHECK((Pm-Pd).array().abs().maxCoeff() < TOL);
  return 0;
}

static int schur_nonuniform_conductance()
{
  MatrixXd V = octa_V(); MatrixXi F = octa_F();
  const int n = V.rows();
  int edge_index = 0;
  SparseMatrix<double> L0 = make_laplacian(F,n,
    [&](int,int){ edge_index++; return 0.5 + 0.37*(edge_index%4); });
  MatrixXd U; MatrixXi G; VectorXi J,I;
  SparseMatrix<double> K,P;
  igl::qslim_operator_trace trace;
  const double lambda = 9.1;
  TASK_CHECK(igl::qslim_operator(V,F,L0,4,lambda,U,G,J,I,K,P,&trace));
  DenseSchur ref(L0);
  for(const auto & te : trace.entries)
  {
    const double expect = lambda*ref.penalty(te.r,te.s);
    TASK_CHECK(std::abs(expect-te.operator_penalty) <
      TOL*std::max(1.0,std::abs(expect)));
    if(te.success) ref.eliminate(te.r);
  }
  const MatrixXd Pd = harmonic_prolongation(L0,I);
  TASK_CHECK((MatrixXd(P)-Pd).array().abs().maxCoeff() < TOL);
  return 0;
}

static int schur_quadratic_energy_preserved()
{
  MatrixXd V; MatrixXi F;
  subdivided_octa(V,F,1); // 26 vertices, 32 faces
  const int n = V.rows();
  SparseMatrix<double> L0 = uniform_laplacian(F,n);
  MatrixXd U; MatrixXi G; VectorXi J,I;
  SparseMatrix<double> K,P;
  TASK_CHECK(igl::qslim_operator(V,F,L0,12,2.5,U,G,J,I,K,P,nullptr));
  // P^T L0 P = K
  const SparseMatrix<double> PtLP =
    (P.transpose()*L0*P).pruned(0.0);
  TASK_CHECK(PtLP.rows()==K.rows() && PtLP.nonZeros()>0);
  TASK_CHECK((MatrixXd(PtLP)-MatrixXd(K)).array().abs().maxCoeff() < 1e-8);

  // Random coarse scalars: harmonic extension energy equals coarse energy and
  // the eliminated rows of L0 P vanish.
  srand(12345);
  VectorXd x(U.rows());
  for(int i = 0;i<x.size();i++) x(i) = std::cos(0.37*i)+0.5*std::sin(1.9*i);
  const VectorXd fine = P*x;
  TASK_CHECK(fine.size()==n);
  const double e_coarse = x.dot(K*x);
  const double e_fine = fine.dot(L0*fine);
  TASK_CHECK(std::abs(e_coarse-e_fine) < 1e-8*std::max(1.0,std::abs(e_fine)));

  const MatrixXd LP = MatrixXd(L0*P);
  std::vector<char> kept(n,false);
  for(int i = 0;i<I.size();i++) kept[I(i)] = true;
  for(int v = 0;v<n;v++)
    if(!kept[v])
      TASK_CHECK(LP.row(v).array().abs().maxCoeff() < 1e-9);
  // Every prolongation row sums to one; kept rows are unit rows. Eigen's
  // sparse matrices are column-major, so inspect rows through P^T.
  const SparseMatrix<double> PT = P.transpose();
  for(int v = 0;v<n;v++)
  {
    double sum = 0.0;
    int nnz = 0;
    for(SparseMatrix<double>::InnerIterator it(PT,v); it; ++it)
    { sum += it.value(); nnz++; }
    TASK_CHECK(std::abs(sum-1.0) < 1e-10);
    if(kept[v])
    {
      TASK_CHECK(nnz==1);
      for(SparseMatrix<double>::InnerIterator it(PT,v); it; ++it)
        TASK_CHECK(std::abs(it.value()-1.0)<1e-12);
    }
  }
  return 0;
}

// ---------------------------------------------------------------- queue group
static int queue_fill_refresh_changes_order()
{
  MatrixXd V; MatrixXi F;
  subdivided_octa(V,F,1);
  const int n = V.rows();
  SparseMatrix<double> L0 = uniform_laplacian(F,n);

  // lambda = 0 run gives the pure-QSlim order.
  MatrixXd U0,U1; MatrixXi G0,G1; VectorXi J0,J1,I0,I1;
  SparseMatrix<double> K0,P0,K1,P1;
  igl::qslim_operator_trace tr0,tr1;
  TASK_CHECK(igl::qslim_operator(V,F,L0,12,0.0,U0,G0,J0,I0,K0,P0,&tr0));
  TASK_CHECK(igl::qslim_operator(V,F,L0,12,1.0e6,U1,G1,J1,I1,K1,P1,&tr1));

  // A fill-driven refresh really happens: some collapse refreshes edges
  // outside the geometric one-ring.
  bool saw_extra = false;
  for(const auto & te : tr1.entries)
    if(te.success && te.num_extra_refreshed>0) saw_extra = true;
  TASK_CHECK(saw_extra);

  // The operator penalty changes which candidate is picked at some step.
  const size_t cnt = std::min(tr0.entries.size(),tr1.entries.size());
  bool order_differs = false;
  size_t k0 = 0,k1 = 0;
  for(size_t k = 0;k<cnt;k++)
  {
    while(k0<tr0.entries.size() && !tr0.entries[k0].success) k0++;
    while(k1<tr1.entries.size() && !tr1.entries[k1].success) k1++;
    TASK_CHECK(k0<tr0.entries.size() && k1<tr1.entries.size());
    if(tr0.entries[k0].r != tr1.entries[k1].r ||
       tr0.entries[k0].s != tr1.entries[k1].s)
      order_differs = true;
    k0++; k1++;
  }
  TASK_CHECK(order_differs);

  // Every popped cost agrees with a dense Schur state maintained in the exact
  // pop order. A stale heap entry (missed fill refresh) would fail this.
  DenseSchur ref(L0);
  for(const auto & te : tr1.entries)
  {
    const double expect = 1.0e6*ref.penalty(te.r,te.s);
    TASK_CHECK(std::isfinite(te.operator_penalty));
    TASK_CHECK(std::abs(expect-te.operator_penalty) <
      1e-6*std::max(1.0,std::abs(expect)));
    if(te.success) ref.eliminate(te.r);
  }
  return 0;
}

static int queue_failures_are_logged_and_continue()
{
  // A tiny tetrahedron component (its 6 edges are intrinsically
  // non-collapsible: every pair of vertices already shares two faces) plus
  // an octahedron that still has collapsible edges.
  MatrixXd V(10,3);
  V << 0,0,0, 0.01,0,0, 0,0.01,0, 0,0,0.01,
       5,0,0, 3,0,0, 4,1,0, 4,-1,0, 4,0,1, 4,0,-1;
  MatrixXi Ft(4,3);
  Ft << 0,1,2, 0,3,1, 1,3,2, 2,3,0;
  MatrixXi Fo(8,3);
  Fo << 4,6,8, 6,5,8, 5,7,8, 7,4,8,
        6,4,9, 5,6,9, 7,5,9, 4,7,9;
  MatrixXi F(12,3);
  F << Ft,Fo;
  const int n = V.rows();
  SparseMatrix<double> L0 = uniform_laplacian(F,n);
  MatrixXd U; MatrixXi G; VectorXi J,I;
  SparseMatrix<double> K,P;
  igl::qslim_operator_trace trace;
  const double lambda = 2.0;
  const bool reached = igl::qslim_operator(V,F,L0,8,lambda,U,G,J,I,K,P,&trace);
  TASK_CHECK(reached);
  int fails = 0, successes = 0;
  bool saw_success_after_failure = false;
  for(const auto & te : trace.entries)
  {
    if(!te.success)
    {
      fails++;
      TASK_CHECK(te.num_extra_refreshed==0);
    }else
    {
      successes++;
      if(fails>0) saw_success_after_failure = true;
    }
  }
  TASK_CHECK(fails==6);
  TASK_CHECK(successes>0);
  TASK_CHECK(saw_success_after_failure);
  // The tetrahedron survives intact; all its vertices remain.
  for(int v = 0;v<4;v++)
  {
    bool present = false;
    for(int i = 0;i<I.size();i++) if(I(i)==v) present = true;
    TASK_CHECK(present);
  }
  // A failed pop never eliminates: penalties stay consistent with a dense
  // Schur state advanced only on success.
  DenseSchur ref(L0);
  for(const auto & te : trace.entries)
  {
    const double expect = lambda*ref.penalty(te.r,te.s);
    TASK_CHECK(std::abs(expect-te.operator_penalty) <
      TOL*std::max(1.0,std::abs(expect)));
    if(te.success) ref.eliminate(te.r);
  }
  // Outputs remain valid despite the rejected candidates.
  TASK_CHECK((MatrixXd(P.transpose()*L0*P)-MatrixXd(K))
    .array().abs().maxCoeff() < 1e-8);
  return 0;
}

// -------------------------------------------------------------- transfer group
static int transfer_two_components_isolated()
{
  // Two disjoint octahedra in one mesh.
  MatrixXd Va = octa_V(), Vb = octa_V()*1.7;
  Vb.rowwise() += (Eigen::RowVector3d()<<5.0,0.0,0.0).finished();
  MatrixXi Fa = octa_F(), Fb = octa_F().array() + Va.rows();
  MatrixXd V(Va.rows()+Vb.rows(),3);
  V<<Va,Vb;
  MatrixXi F(Fa.rows()+Fb.rows(),3);
  F<<Fa,Fb;
  const int n = V.rows();
  SparseMatrix<double> L0 = uniform_laplacian(F,n);

  MatrixXd U; MatrixXi G; VectorXi J,I;
  SparseMatrix<double> K,P;
  igl::qslim_operator_trace trace;
  TASK_CHECK(igl::qslim_operator(V,F,L0,8,1.3,U,G,J,I,K,P,&trace));

  // Component membership of each original vertex.
  std::vector<int> comp(n,0);
  for(int v = Va.rows();v<n;v++) comp[v] = 1;
  // No prolongation weight crosses components (iterate rows via P^T).
  const SparseMatrix<double> PT = P.transpose();
  for(int v = 0;v<n;v++)
    for(SparseMatrix<double>::InnerIterator it(PT,v); it; ++it)
      TASK_CHECK(comp[I(it.row())] == comp[v]);
  // K is block diagonal across the two components.
  for(int a = 0;a<I.size();a++)
    for(int b = 0;b<I.size();b++)
      if(comp[I(a)] != comp[I(b)])
        TASK_CHECK(K.coeff(a,b)==0.0);
  // Energy identity still holds globally.
  TASK_CHECK((MatrixXd(P.transpose()*L0*P)-MatrixXd(K))
    .array().abs().maxCoeff() < 1e-8);
  return 0;
}

static int transfer_unreachable_target()
{
  // Two octahedra cannot go below 4 faces per component.
  MatrixXd Va = octa_V(), Vb = octa_V();
  Vb.rowwise() += (Eigen::RowVector3d()<<5.0,0.0,0.0).finished();
  MatrixXi Fa = octa_F(), Fb = octa_F().array() + Va.rows();
  MatrixXd V(Va.rows()+Vb.rows(),3); V<<Va,Vb;
  MatrixXi F(Fa.rows()+Fb.rows(),3); F<<Fa,Fb;
  const int n = V.rows();
  SparseMatrix<double> L0 = uniform_laplacian(F,n);

  MatrixXd U; MatrixXi G; VectorXi J,I;
  SparseMatrix<double> K,P;
  igl::qslim_operator_trace trace;
  const bool reached = igl::qslim_operator(V,F,L0,4,1.0,U,G,J,I,K,P,&trace);
  TASK_CHECK(!reached);
  TASK_CHECK(!trace.reached);
  TASK_CHECK(G.rows()==8);
  TASK_CHECK(U.allFinite());
  // Valid K and P are still returned.
  TASK_CHECK((MatrixXd(P.transpose()*L0*P)-MatrixXd(K))
    .array().abs().maxCoeff() < 1e-8);
  const SparseMatrix<double> PT2 = P.transpose();
  for(int v = 0;v<n;v++)
  {
    double sum = 0.0;
    for(SparseMatrix<double>::InnerIterator it(PT2,v); it; ++it)
      sum += it.value();
    TASK_CHECK(std::abs(sum-1.0)<1e-10);
  }
  return 0;
}

static int transfer_no_op_when_target_met()
{
  MatrixXd V = octa_V(); MatrixXi F = octa_F();
  const int n = V.rows();
  SparseMatrix<double> L0 = uniform_laplacian(F,n);
  MatrixXd U; MatrixXi G; VectorXi J,I;
  SparseMatrix<double> K,P;
  igl::qslim_operator_trace trace;
  TASK_CHECK(igl::qslim_operator(V,F,L0,F.rows(),4.0,U,G,J,I,K,P,&trace));
  TASK_CHECK(trace.num_collapsed==0);
  TASK_CHECK(trace.entries.empty());
  TASK_CHECK(U.rows()==n && G.rows()==F.rows());
  TASK_CHECK((U-V).array().abs().maxCoeff()==0.0);
  TASK_CHECK(MatrixXd(K).rows()==n);
  TASK_CHECK((MatrixXd(K)-MatrixXd(L0)).array().abs().maxCoeff() < 1e-12);
  const MatrixXd Pm(P);
  TASK_CHECK((Pm-MatrixXd::Identity(n,n)).array().abs().maxCoeff() < 1e-12);
  return 0;
}

static int transfer_random_field_residual()
{
  MatrixXd V; MatrixXi F;
  subdivided_octa(V,F,1);
  const int n = V.rows();
  int eidx = 0;
  SparseMatrix<double> L0 = make_laplacian(F,n,
    [&](int,int){ eidx++; return 1.0 + 0.23*(eidx%5); });
  MatrixXd U; MatrixXi G; VectorXi J,I;
  SparseMatrix<double> K,P;
  TASK_CHECK(igl::qslim_operator(V,F,L0,10,7.7,U,G,J,I,K,P,nullptr));
  srand(777);
  for(int trial = 0;trial<3;trial++)
  {
    VectorXd x(U.rows());
    for(int i = 0;i<x.size();i++) x(i) = double(rand())/RAND_MAX - 0.5;
    const VectorXd fine = P*x;
    const MatrixXd LP = MatrixXd(L0*P);
    std::vector<char> kept(n,false);
    for(int i = 0;i<I.size();i++) kept[I(i)] = true;
    double residual = 0.0;
    for(int v = 0;v<n;v++)
      if(!kept[v])
        residual = std::max(residual,LP.row(v).array().abs().maxCoeff());
    TASK_CHECK(residual < 1e-9);
    const double e_coarse = x.dot(K*x), e_fine = fine.dot(L0*fine);
    TASK_CHECK(std::abs(e_coarse-e_fine) <
      1e-8*std::max(1.0,std::abs(e_fine)));
    TASK_CHECK(e_fine >= -1e-10);
  }
  return 0;
}

// -------------------------------------------------------- compatibility group
static int compatibility_lambda_zero_matches_qslim()
{
  MatrixXd V; MatrixXi F;
  subdivided_octa(V,F,1);
  const int n = V.rows();
  SparseMatrix<double> L0 = uniform_laplacian(F,n);

  MatrixXd Uq,Uo; MatrixXi Gq,Go; VectorXi Jq,Jo,Iq,Io;
  SparseMatrix<double> Kq,Pq;
  TASK_CHECK(igl::qslim(V,F,12,false,Uq,Gq,Jq,Iq));
  igl::qslim_operator_trace trace;
  TASK_CHECK(igl::qslim_operator(V,F,L0,12,0.0,Uo,Go,Jo,Io,Kq,Pq,&trace));
  TASK_CHECK(Gq.rows()==Go.rows());
  TASK_CHECK(Uq.rows()==Uo.rows());
  // Same birth faces and birth vertices.
  std::vector<int> jq(Jq.data(),Jq.data()+Jq.size());
  std::vector<int> jo(Jo.data(),Jo.data()+Jo.size());
  std::sort(jq.begin(),jq.end()); std::sort(jo.begin(),jo.end());
  TASK_CHECK(jq==jo);
  std::vector<int> iq(Iq.data(),Iq.data()+Iq.size());
  std::vector<int> io(Io.data(),Io.data()+Io.size());
  std::sort(iq.begin(),iq.end()); std::sort(io.begin(),io.end());
  TASK_CHECK(iq==io);
  // Same surviving positions (matched through birth vertex).
  for(int a = 0;a<Io.size();a++)
  {
    const int b = std::lower_bound(iq.begin(),iq.end(),Io(a))-iq.begin();
    TASK_CHECK((Uo.row(a)-Uq.row(b)).norm() < 1e-10);
  }
  // Zero lambda means zero operator penalty throughout.
  for(const auto & te : trace.entries)
    TASK_CHECK(te.operator_penalty==0.0 && std::isfinite(te.qem_cost));
  return 0;
}

static int compatibility_legacy_interfaces_still_work()
{
  MatrixXd V = octa_V(); MatrixXi F = octa_F();
  MatrixXd U; MatrixXi G; VectorXi J,I;
  TASK_CHECK(igl::qslim(V,F,6,false,U,G,J,I));
  TASK_CHECK(G.rows()==6 && U.allFinite());
  TASK_CHECK(igl::decimate(V,F,6,false,U,G,J,I));
  TASK_CHECK(G.rows()==6);
  SparseMatrix<double> A;
  igl::adjacency_matrix(F,A);
  TASK_CHECK(A.rows()==6 && A.nonZeros()==24);
  return 0;
}

static int compatibility_invalid_inputs_atomic()
{
  MatrixXd V = octa_V(); MatrixXi F = octa_F();
  const int n = V.rows();
  SparseMatrix<double> L0 = uniform_laplacian(F,n);
  MatrixXd U(2,3); U.setConstant(42.0);
  MatrixXi G(2,3); G.setConstant(7);
  VectorXi J = VectorXi::Constant(2,11), I = VectorXi::Constant(2,13);
  SparseMatrix<double> K,P;
  // TASK_CHECK returns from the enclosing function, so inside helpers return
  // a bool and assert at each call site.
  const auto untouched = [&]() -> bool
  {
    return U.rows()==2 && G.rows()==2 && J(0)==11 && I(0)==13;
  };
  const auto must_throw = [&](const MatrixXd & Vv,const MatrixXi & Ff,
    const SparseMatrix<double> & Ll,int mm,double lam) -> bool
  {
    bool threw = false;
    try
    {
      igl::qslim_operator(Vv,Ff,Ll,mm,lam,U,G,J,I,K,P,nullptr);
    }catch(const std::invalid_argument &){ threw = true; }
    return threw && untouched();
  };
  TASK_CHECK(must_throw(V,F,L0,3,1.0));            // max_m < 4
  TASK_CHECK(must_throw(V,F,L0,8,-1.0));           // negative lambda
  TASK_CHECK(must_throw(V,F,L0,8,
    std::numeric_limits<double>::infinity()));     // non-finite lambda
  MatrixXd Vbad = V; Vbad(0,0) = std::numeric_limits<double>::quiet_NaN();
  TASK_CHECK(must_throw(Vbad,F,L0,8,1.0));         // non-finite geometry
  SparseMatrix<double> Lbad(n,n-1);
  TASK_CHECK(must_throw(V,F,Lbad,8,1.0));          // wrong operator size
  SparseMatrix<double> Lpos = L0;
  Lpos.coeffRef(0,2) = 1.5;            // positive off-diagonal
  TASK_CHECK(must_throw(V,F,Lpos,8,1.0));
  MatrixXi Fbad = F; Fbad(0,1) = Fbad(0,0); // degenerate face
  TASK_CHECK(must_throw(V,Fbad,L0,8,1.0));
  return 0;
}

extern const struct task_case task_cases[] = {
  {"schur","dense_penalties",schur_dense_penalties},
  {"schur","nonuniform_conductance",schur_nonuniform_conductance},
  {"schur","quadratic_energy_preserved",schur_quadratic_energy_preserved},
  {"queue","fill_refresh_changes_order",queue_fill_refresh_changes_order},
  {"queue","failures_logged_and_continue",queue_failures_are_logged_and_continue},
  {"transfer","two_components_isolated",transfer_two_components_isolated},
  {"transfer","unreachable_target",transfer_unreachable_target},
  {"transfer","no_op_when_target_met",transfer_no_op_when_target_met},
  {"transfer","random_field_residual",transfer_random_field_residual},
  {"compatibility","lambda_zero_matches_qslim",compatibility_lambda_zero_matches_qslim},
  {"compatibility","legacy_interfaces_still_work",compatibility_legacy_interfaces_still_work},
  {"compatibility","invalid_inputs_atomic",compatibility_invalid_inputs_atomic},
};
extern const size_t task_case_count =
  sizeof(task_cases)/sizeof(task_cases[0]);
