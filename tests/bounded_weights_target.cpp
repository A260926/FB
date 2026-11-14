// tests/bounded_weights_target.cpp
//
// Target tests for tinyobj::BoundedWeights. The optimality group contains a
// fully independent dense reference: it assembles L, M and Q in dense form,
// enumerates every active nonnegativity set, solves the resulting KKT linear
// systems with Gaussian elimination, and keeps the feasible point of minimum
// energy. That reference is compared against the sparse production solver.

#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "../task_support/check.h"

#include "bounded_weights.h"

namespace {

const double kWeightTol = 1e-6;
const double kEnergyTol = 1e-6;

struct TriMesh {
  int n;
  std::vector<double> pos;  // 3*n
  std::vector<int> tris;    // 3*F
};

tinyobj::attrib_t MakeAttrib(const TriMesh& m) {
  tinyobj::attrib_t a;
  a.vertices = m.pos;
  return a;
}

std::vector<tinyobj::shape_t> MakeShapes(const TriMesh& m,
                                         int shape_split = 0) {
  std::vector<tinyobj::shape_t> shapes(shape_split + 1);
  size_t faces = m.tris.size() / 3;
  for (size_t f = 0; f < faces; ++f) {
    int target = shape_split ? static_cast<int>(f % (shape_split + 1)) : 0;
    for (int c = 0; c < 3; ++c) {
      tinyobj::index_t idx;
      idx.vertex_index = m.tris[3 * f + c];
      idx.normal_index = 0;
      idx.texcoord_index = static_cast<int>(f + c);  // distinct UV corners
      shapes[target].mesh.indices.push_back(idx);
    }
    shapes[target].mesh.num_face_vertices.push_back(3);
  }
  return shapes;
}

// ---------------------------------------------------------------------------
// Independent dense reference: cotangent L, lumped masses M, Q = L^T M^-1 L.
// ---------------------------------------------------------------------------
struct DenseOperators {
  int n;
  std::vector<double> L;  // n*n row major
  std::vector<double> M;  // n diagonal entries
  std::vector<double> Q;  // n*n row major
};

DenseOperators BuildDense(const TriMesh& m) {
  DenseOperators d;
  d.n = m.n;
  d.L.assign(m.n * m.n, 0.0);
  d.M.assign(m.n, 0.0);
  for (size_t f = 0; f < m.tris.size() / 3; ++f) {
    const int* v = &m.tris[3 * f];
    double p[3][3];
    for (int c = 0; c < 3; ++c)
      for (int q = 0; q < 3; ++q) p[c][q] = m.pos[3 * v[c] + q];
    double e1[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1],
                    p[1][2] - p[0][2]};
    double e2[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1],
                    p[2][2] - p[0][2]};
    double cr[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                    e1[2] * e2[0] - e1[0] * e2[2],
                    e1[0] * e2[1] - e1[1] * e2[0]};
    double cn = std::sqrt(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]);
    double area = 0.5 * cn;
    for (int c = 0; c < 3; ++c) d.M[v[c]] += area / 3.0;
    for (int c = 0; c < 3; ++c) {
      int a = v[(c + 1) % 3], b = v[(c + 2) % 3];
      double u[3] = {p[(c + 1) % 3][0] - p[c][0],
                     p[(c + 1) % 3][1] - p[c][1],
                     p[(c + 1) % 3][2] - p[c][2]};
      double w[3] = {p[(c + 2) % 3][0] - p[c][0],
                     p[(c + 2) % 3][1] - p[c][1],
                     p[(c + 2) % 3][2] - p[c][2]};
      double dot = u[0] * w[0] + u[1] * w[1] + u[2] * w[2];
      double cot = dot / cn;  // negative for obtuse angles, kept as is
      d.L[a * m.n + b] += -0.5 * cot;
      d.L[b * m.n + a] += -0.5 * cot;
    }
  }
  for (int i = 0; i < m.n; ++i) {
    double s = 0.0;
    for (int j = 0; j < m.n; ++j)
      if (i != j) s += d.L[i * m.n + j];
    d.L[i * m.n + i] = -s;
  }
  d.Q.assign(m.n * m.n, 0.0);
  for (int i = 0; i < m.n; ++i)
    for (int k = 0; k < m.n; ++k) {
      double t = d.L[i * m.n + k] / d.M[k];
      for (int j = 0; j < m.n; ++j)
        d.Q[i * m.n + j] += t * d.L[k * m.n + j];
    }
  return d;
}

bool GaussianSolve(std::vector<std::vector<double> > A,
                   const std::vector<double>& b, std::vector<double>* x) {
  int sz = static_cast<int>(A.size());
  for (int i = 0; i < sz; ++i) A[i].push_back(b[i]);
  for (int col = 0; col < sz; ++col) {
    int piv = col;
    for (int r = col + 1; r < sz; ++r)
      if (std::fabs(A[r][col]) > std::fabs(A[piv][col])) piv = r;
    if (std::fabs(A[piv][col]) < 1e-14) return false;
    std::swap(A[piv], A[col]);
    for (int r = 0; r < sz; ++r) {
      if (r == col) continue;
      double f = A[r][col] / A[col][col];
      for (int c = col; c <= sz; ++c) A[r][c] -= f * A[col][c];
    }
  }
  x->resize(sz);
  for (int i = 0; i < sz; ++i) (*x)[i] = A[i][sz] / A[i][i];
  return true;
}

double DenseEnergy(const DenseOperators& d, const std::vector<double>& W,
                   int K) {
  int n = d.n;
  double e = 0.0;
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < n; ++j)
      for (int k = 0; k < K; ++k)
        e += W[i * K + k] * d.Q[i * n + j] * W[j * K + k];
  return 0.5 * e;
}

// Exact KKT solution by enumerating the active set of nonnegativity
// constraints over all free (vertex, handle) variables jointly.
bool EnumerateSolution(const TriMesh& m, const DenseOperators& d,
                       const std::vector<int>& fixed, int K,
                       std::vector<double>* best_w, double* best_e) {
  int n = m.n;
  std::vector<int> fr;
  for (int i = 0; i < n; ++i)
    if (fixed[i] < 0) fr.push_back(i);
  int nf = static_cast<int>(fr.size());
  int nv = nf * K;
  std::vector<double> c(nv, 0.0);
  for (int a = 0; a < nv; ++a) {
    int i = fr[a / K], k = a % K;
    double s = 0.0;
    for (int j = 0; j < n; ++j)
      if (fixed[j] >= 0)
        s += d.Q[i * n + j] * (fixed[j] == k ? 1.0 : 0.0);
    c[a] = s;
  }
  *best_e = std::numeric_limits<double>::infinity();
  bool found = false;
  for (long mask = 0; mask < (1L << nv); ++mask) {
    std::vector<int> active, inact;
    for (int a = 0; a < nv; ++a)
      (mask & (1L << a) ? active : inact).push_back(a);
    int na = static_cast<int>(active.size());
    int ni = static_cast<int>(inact.size());
    int sz = ni + nf + na;
    std::vector<std::vector<double> > A(sz,
                                        std::vector<double>(sz, 0.0));
    std::vector<double> b(sz, 0.0);
    for (int t = 0; t < ni; ++t) {  // inactive stationarity rows
      int ia = inact[t];
      for (int u = 0; u < ni; ++u) {
        int ib = inact[u];
        A[t][u] = d.Q[fr[ia / K] * n + fr[ib / K]] *
                  (ia % K == ib % K ? 1.0 : 0.0);
      }
      A[t][ni + ia / K] = 1.0;
      b[t] = -c[ia];
    }
    for (int r = 0; r < nf; ++r) {  // row-sum equality rows
      for (int k = 0; k < K; ++k) {
        int a = r * K + k;
        for (int t = 0; t < ni; ++t)
          if (inact[t] == a) A[ni + r][t] = 1.0;
      }
      b[ni + r] = 1.0;
    }
    for (int t = 0; t < na; ++t) {  // active stationarity rows (with mu)
      int ia = active[t];
      int row = ni + nf + t;
      for (int u = 0; u < ni; ++u) {
        int ib = inact[u];
        A[row][u] = d.Q[fr[ia / K] * n + fr[ib / K]] *
                    (ia % K == ib % K ? 1.0 : 0.0);
      }
      A[row][ni + ia / K] = 1.0;
      A[row][ni + nf + t] = -1.0;
      b[row] = -c[ia];
    }
    std::vector<double> sol;
    if (!GaussianSolve(A, b, &sol)) continue;
    std::vector<double> x(nv, 0.0);
    for (int t = 0; t < ni; ++t) x[inact[t]] = sol[t];
    bool feasible = true;
    for (int a = 0; a < nv; ++a)
      if (x[a] < -1e-9) feasible = false;
    for (int r = 0; r < nf; ++r) {
      double s = 0.0;
      for (int k = 0; k < K; ++k) s += x[r * K + k];
      if (std::fabs(s - 1.0) > 1e-9) feasible = false;
    }
    for (int t = 0; t < na; ++t)
      if (sol[ni + nf + t] < -1e-9) feasible = false;  // multipliers mu >= 0
    if (!feasible) continue;
    std::vector<double> W(n * K, 0.0);
    for (int i = 0; i < n; ++i) {
      if (fixed[i] >= 0) W[i * K + fixed[i]] = 1.0;
      for (int a = 0; a < nv; ++a)
        if (fr[a / K] == i) W[i * K + a % K] = x[a];
    }
    double e = DenseEnergy(d, W, K);
    if (e < *best_e) {
      *best_e = e;
      *best_w = W;
      found = true;
    }
  }
  return found;
}

// The forbidden shortcut: independent per-column harmonic solves, then clip
// negatives and renormalize each row.
std::vector<double> ClipAndRenormalize(const TriMesh& m,
                                       const DenseOperators& d,
                                       const std::vector<int>& fixed, int K) {
  int n = m.n;
  std::vector<int> fr;
  for (int i = 0; i < n; ++i)
    if (fixed[i] < 0) fr.push_back(i);
  int nf = static_cast<int>(fr.size());
  std::vector<double> W(n * K, 0.0);
  for (int i = 0; i < n; ++i)
    if (fixed[i] >= 0) W[i * K + fixed[i]] = 1.0;
  for (int k = 0; k < K; ++k) {
    std::vector<std::vector<double> > A(nf,
                                        std::vector<double>(nf, 0.0));
    std::vector<double> b(nf, 0.0);
    for (int r = 0; r < nf; ++r) {
      int i = fr[r];
      for (int c = 0; c < nf; ++c) A[r][c] = d.L[i * n + fr[c]];
      double s = 0.0;
      for (int j = 0; j < n; ++j)
        if (fixed[j] >= 0)
          s += d.L[i * n + j] * (fixed[j] == k ? 1.0 : 0.0);
      b[r] = -s;
    }
    std::vector<double> x;
    if (!GaussianSolve(A, b, &x)) return std::vector<double>();
    for (int r = 0; r < nf; ++r) W[fr[r] * K + k] = x[r];
  }
  for (int i = 0; i < n; ++i) {
    if (fixed[i] >= 0) continue;
    double s = 0.0;
    for (int k = 0; k < K; ++k) {
      if (W[i * K + k] < 0.0) W[i * K + k] = 0.0;
      s += W[i * K + k];
    }
    for (int k = 0; k < K; ++k) W[i * K + k] /= s;
  }
  return W;
}

TriMesh GridMesh(int nx, int ny, bool bumpy) {
  TriMesh m;
  m.n = nx * ny;
  m.pos.resize(3 * m.n);
  for (int j = 0; j < ny; ++j)
    for (int i = 0; i < nx; ++i) {
      int id = j * nx + i;
      m.pos[3 * id] = static_cast<double>(i);
      m.pos[3 * id + 1] = static_cast<double>(j);
      m.pos[3 * id + 2] =
          bumpy ? 0.35 * std::sin(0.6 * i) * std::cos(0.5 * j) : 0.0;
    }
  for (int j = 0; j + 1 < ny; ++j)
    for (int i = 0; i + 1 < nx; ++i) {
      int a = j * nx + i, b = a + 1, c = a + nx, d2 = c + 1;
      int q[6] = {a, b, d2, a, d2, c};
      m.tris.insert(m.tris.end(), q, q + 6);
    }
  return m;
}

template <typename F>
bool ThrowsInvalidArg(const F& f) {
  try {
    f();
  } catch (const std::invalid_argument&) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

// ===========================================================================
// optimality
// ===========================================================================
int CaseActiveZeroVsKkt() {
  // 3x2 planar grid; handles {1} and {0}. The constrained optimum pins free
  // vertices 2 and 5 entirely to handle 0 (exact active zero weights).
  TriMesh m = GridMesh(3, 2, false);
  TASK_CHECK(m.n == 6);
  std::vector<int> fixed(m.n, -1);
  fixed[1] = 0;
  fixed[0] = 1;
  DenseOperators d = BuildDense(m);
  std::vector<double> ref_w;
  double ref_e = 0.0;
  TASK_CHECK(EnumerateSolution(m, d, fixed, 2, &ref_w, &ref_e));

  int exact_zeros = 0;
  for (int i = 0; i < m.n; ++i)
    if (fixed[i] < 0)
      for (int k = 0; k < 2; ++k)
        if (ref_w[i * 2 + k] == 0.0) ++exact_zeros;
  TASK_CHECK(exact_zeros >= 2);
  TASK_CHECK(ref_w[2 * 2 + 1] == 0.0 && ref_w[5 * 2 + 1] == 0.0);

  std::vector<double> clip_w = ClipAndRenormalize(m, d, fixed, 2);
  // The clipped shortcut cannot reproduce the active constraints.
  TASK_CHECK(std::fabs(clip_w[2 * 2 + 1] - ref_w[2 * 2 + 1]) > 0.02);
  TASK_CHECK(std::fabs(clip_w[5 * 2 + 1] - ref_w[5 * 2 + 1]) > 0.05);
  double clip_e = DenseEnergy(d, clip_w, 2);
  TASK_CHECK(clip_e > ref_e * (1.0 + 1e-6));

  std::vector<std::vector<int> > handles(2);
  handles[0].push_back(1);
  handles[1].push_back(0);
  tinyobj::attrib_t attr = MakeAttrib(m);
  std::vector<tinyobj::shape_t> shapes = MakeShapes(m);
  tinyobj::BoundedWeights bw(attr, shapes, handles);
  TASK_CHECK(bw.solve(200000));
  TASK_CHECK(bw.projection_residual() <= 1e-8);

  double max_w_err = 0.0, row_err = 0.0, low_err = 0.0;
  for (int i = 0; i < m.n; ++i) {
    double sum = 0.0;
    for (int k = 0; k < 2; ++k) {
      double w = bw.weight(i, k);
      max_w_err = std::max(max_w_err, std::fabs(w - ref_w[i * 2 + k]));
      if (w < 0.0) low_err = std::max(low_err, -w);
      sum += w;
    }
    row_err = std::max(row_err, std::fabs(sum - 1.0));
  }
  TASK_CHECK(max_w_err <= kWeightTol);
  TASK_CHECK(row_err <= 1e-10);
  TASK_CHECK(low_err <= 1e-10);
  double e_rel = std::fabs(bw.energy() - ref_e) / std::fabs(ref_e);
  TASK_CHECK(e_rel <= kEnergyTol);
  // Active weights are exactly zero in the production solution as well.
  TASK_CHECK(bw.weight(2, 1) == 0.0 && bw.weight(5, 1) == 0.0);
  return 0;
}

int CaseObtuseCotangents() {
  // Vertex 2 sits inside the unit square corner: both triangles are obtuse at
  // vertex 2, so cotangents opposite it (edges 0-1 and 0-3) are negative and
  // the corresponding L off-diagonals are positive.
  TriMesh m;
  m.n = 4;
  double p[4][3] = {{0, 0, 0}, {1, 0, 0}, {0.2, 0.3, 0}, {0, 1, 0}};
  m.pos.assign(&p[0][0], &p[0][0] + 12);
  int t[6] = {0, 1, 2, 0, 2, 3};
  m.tris.assign(t, t + 6);
  std::vector<int> fixed(m.n, -1);
  fixed[1] = 0;
  fixed[3] = 1;
  DenseOperators d = BuildDense(m);
  TASK_CHECK(d.L[0 * m.n + 1] > 0.0);  // positive off-diagonal from neg cot
  TASK_CHECK(d.L[0 * m.n + 3] > 0.0);
  TASK_CHECK(d.L[1 * m.n + 0] > 0.0);
  TASK_CHECK(d.L[3 * m.n + 0] > 0.0);
  for (int i = 0; i < m.n; ++i) {
    double s = 0.0;
    for (int j = 0; j < m.n; ++j) s += d.L[i * m.n + j];
    TASK_CHECK(std::fabs(s) <= 1e-12);
  }
  size_t dense_nnz = 0;
  for (int i = 0; i < m.n * m.n; ++i)
    if (d.L[i] != 0.0) ++dense_nnz;

  std::vector<double> ref_w;
  double ref_e = 0.0;
  TASK_CHECK(EnumerateSolution(m, d, fixed, 2, &ref_w, &ref_e));

  std::vector<std::vector<int> > handles(2);
  handles[0].push_back(1);
  handles[1].push_back(3);
  tinyobj::BoundedWeights bw(MakeAttrib(m), MakeShapes(m), handles);
  TASK_CHECK(bw.info().l_nonzeros == dense_nnz);
  TASK_CHECK(bw.solve(200000));
  TASK_CHECK(bw.projection_residual() <= 1e-8);
  double max_w_err = 0.0;
  for (int i = 0; i < m.n; ++i)
    for (int k = 0; k < 2; ++k)
      max_w_err =
          std::max(max_w_err, std::fabs(bw.weight(i, k) - ref_w[i * 2 + k]));
  TASK_CHECK(max_w_err <= kWeightTol);
  TASK_CHECK(std::fabs(bw.energy() - ref_e) / std::fabs(ref_e) <=
             kEnergyTol);
  return 0;
}

int CaseDenseEnergyCounters() {
  TriMesh m = GridMesh(3, 2, false);
  std::vector<std::vector<int> > handles(2);
  handles[0].push_back(3);
  handles[1].push_back(2);
  handles[1].push_back(5);
  std::vector<int> fixed(m.n, -1);
  fixed[3] = 0;
  fixed[2] = fixed[5] = 1;
  DenseOperators d = BuildDense(m);
  std::vector<double> ref_w;
  double ref_e = 0.0;
  TASK_CHECK(EnumerateSolution(m, d, fixed, 2, &ref_w, &ref_e));

  tinyobj::BoundedWeights bw(MakeAttrib(m), MakeShapes(m), handles);
  size_t dense_nnz = 0;
  for (int i = 0; i < m.n * m.n; ++i)
    if (d.L[i] != 0.0) ++dense_nnz;
  TASK_CHECK(bw.info().l_nonzeros == dense_nnz);
  TASK_CHECK(bw.info().successful_solves == 0);
  TASK_CHECK(bw.solve(1000000));
  TASK_CHECK(bw.info().successful_solves == 1);
  // One application at construction, plus one initial product, one per
  // iteration and one final product inside the converged solve.
  TASK_CHECK(bw.info().operator_applications ==
             bw.info().iterations_last + 3);
  double dense_e = DenseEnergy(d, bw.weights(), 2);
  TASK_CHECK(std::fabs(dense_e - bw.energy()) <=
             1e-10 * (1.0 + std::fabs(dense_e)));
  TASK_CHECK(std::fabs(bw.energy() - ref_e) / std::fabs(ref_e) <=
             kEnergyTol);
  for (int i = 0; i < m.n; ++i) {
    double s = 0.0;
    for (int k = 0; k < 2; ++k) {
      TASK_CHECK(bw.weight(i, k) >= -1e-10);
      s += bw.weight(i, k);
    }
    TASK_CHECK(std::fabs(s - 1.0) <= 1e-10);
  }
  return 0;
}

// ===========================================================================
// seams
// ===========================================================================
int CaseObjReaderSeamIdentity() {
  tinyobj::ObjReader reader;
  TASK_CHECK(reader.ParseFromFile("task_support/seams.obj"));
  const tinyobj::attrib_t& attr = reader.GetAttrib();
  const std::vector<tinyobj::shape_t>& shapes = reader.GetShapes();
  TASK_CHECK(shapes.size() == 2);
  TASK_CHECK(attr.vertices.size() == 15);

  std::vector<std::vector<int> > handles(2);
  handles[0].push_back(0);
  handles[1].push_back(2);
  handles[1].push_back(4);  // one handle owns several positions
  tinyobj::BoundedWeights bw(attr, shapes, handles);
  TASK_CHECK(bw.solve(200000));
  TASK_CHECK(bw.info().num_components == 1);
  TASK_CHECK(bw.info().num_faces == 3);

  // UV seam: the same position id 1 appears with two different texcoord ids
  // (shape 0 corner 1 uses vt 1, shape 1 corner 0 uses vt 3). Weights follow
  // position identity, so both corners see the same row.
  const tinyobj::index_t& ca = shapes[0].mesh.indices[1];
  const tinyobj::index_t& cb = shapes[1].mesh.indices[0];
  TASK_CHECK(ca.vertex_index == cb.vertex_index && ca.vertex_index == 1);
  TASK_CHECK(ca.texcoord_index != cb.texcoord_index);
  for (int k = 0; k < 2; ++k)
    TASK_CHECK(bw.weight(ca.vertex_index, k) ==
               bw.weight(cb.vertex_index, k));

  // Fixed rows are unit vectors regardless of the corner used.
  TASK_CHECK(bw.weight(0, 0) == 1.0 && bw.weight(0, 1) == 0.0);
  TASK_CHECK(bw.weight(2, 1) == 1.0 && bw.weight(4, 0) == 0.0);

  std::vector<tinyobj::skin_weight_t> exported = bw.export_skin_weights();
  TASK_CHECK(exported.size() == 5);
  for (size_t i = 0; i < exported.size(); ++i) {
    TASK_CHECK(static_cast<int>(i) == exported[i].vertex_id);
    double sum = 0.0;
    for (size_t j = 0; j < exported[i].weightValues.size(); ++j) {
      int joint = exported[i].weightValues[j].joint_id;
      double w = exported[i].weightValues[j].weight;
      TASK_CHECK(joint == 0 || joint == 1);
      TASK_CHECK(w > 0.0);  // zero weights are omitted
      TASK_CHECK(std::fabs(w - bw.weight(i, joint)) <= 1e-12);
      sum += w;
    }
    TASK_CHECK(std::fabs(sum - 1.0) <= 1e-12);
  }
  TASK_CHECK(exported[0].weightValues.size() == 1);
  TASK_CHECK(exported[2].weightValues.size() == 1);
  TASK_CHECK(exported[4].weightValues.size() == 1);
  return 0;
}

int CaseNoWeldingAndImmutableInput() {
  // Vertices 0 and 3 share identical coordinates but keep distinct ids; they
  // live in different components pinned to different handles, so their
  // weights must not be welded together.
  TriMesh m;
  m.n = 6;
  double p[6][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0},
                    {0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  m.pos.assign(&p[0][0], &p[0][0] + 18);
  int t[6] = {0, 1, 2, 3, 4, 5};
  m.tris.assign(t, t + 6);
  std::vector<std::vector<int> > handles(2);
  handles[0].push_back(0);
  handles[1].push_back(3);
  tinyobj::attrib_t attr = MakeAttrib(m);
  std::vector<tinyobj::shape_t> shapes = MakeShapes(m, 1);  // split across 2
  TASK_CHECK(shapes.size() == 2);
  std::vector<double> vertices_before = attr.vertices;
  attr.skin_weights.push_back(tinyobj::skin_weight_t());
  attr.skin_weights[0].vertex_id = 7;
  size_t vw_before = attr.skin_weights.size();

  tinyobj::BoundedWeights bw(attr, shapes, handles);
  TASK_CHECK(bw.solve(10000));
  TASK_CHECK(bw.info().num_components == 2);
  TASK_CHECK(bw.weight(0, 0) == 1.0 && bw.weight(0, 1) == 0.0);
  TASK_CHECK(bw.weight(3, 0) == 0.0 && bw.weight(3, 1) == 1.0);
  for (int i = 0; i < 3; ++i)
    for (int k = 0; k < 2; ++k)
      TASK_CHECK(std::fabs(bw.weight(i, k) -
                           bw.weight(i + 3, 1 - k)) <= 1e-7);
  // Inputs are untouched.
  TASK_CHECK(attr.vertices == vertices_before);
  TASK_CHECK(attr.skin_weights.size() == vw_before);
  TASK_CHECK(attr.skin_weights[0].vertex_id == 7);
  return 0;
}

int CaseOwnsInputAfterRelease() {
  std::unique_ptr<tinyobj::BoundedWeights> bw;
  {
    TriMesh m = GridMesh(3, 2, true);  // non-planar 3D surface
    tinyobj::attrib_t attr = MakeAttrib(m);
    std::vector<tinyobj::shape_t> shapes = MakeShapes(m);
    std::vector<std::vector<int> > handles(2);
    handles[0].push_back(0);
    handles[1].push_back(2);
    bw.reset(new tinyobj::BoundedWeights(attr, shapes, handles));
  }
  TASK_CHECK(bw->num_positions() == 6);
  TASK_CHECK(bw->solve(200000));
  std::vector<tinyobj::skin_weight_t> exported = bw->export_skin_weights();
  TASK_CHECK(exported.size() == 6);
  std::vector<tinyobj::BoundedAffine> xf(2);
  std::vector<double> moved = bw->deform(xf);
  TASK_CHECK(moved.size() == 18);
  return 0;
}

// ===========================================================================
// deformation
// ===========================================================================
int CaseCommonRigidBody() {
  TriMesh m = GridMesh(3, 2, true);
  std::vector<std::vector<int> > handles(2);
  handles[0].push_back(0);
  handles[1].push_back(2);
  tinyobj::BoundedWeights bw(MakeAttrib(m), MakeShapes(m), handles);
  TASK_CHECK(bw.solve(200000));

  tinyobj::BoundedAffine rigid;  // 90 degree rotation about z + translation
  rigid.linear[0] = 0.0;  rigid.linear[1] = -1.0; rigid.linear[2] = 0.0;
  rigid.linear[3] = 1.0;  rigid.linear[4] = 0.0;  rigid.linear[5] = 0.0;
  rigid.linear[6] = 0.0;  rigid.linear[7] = 0.0;  rigid.linear[8] = 1.0;
  rigid.translation[0] = 1.0;
  rigid.translation[1] = 2.0;
  rigid.translation[2] = -0.5;
  std::vector<tinyobj::BoundedAffine> xf(2, rigid);
  std::vector<double> out = bw.deform(xf);
  for (int i = 0; i < m.n; ++i) {
    double x = m.pos[3 * i], y = m.pos[3 * i + 1], z = m.pos[3 * i + 2];
    double expect[3] = {-y + 1.0, x + 2.0, z - 0.5};
    for (int q = 0; q < 3; ++q)
      TASK_CHECK(std::fabs(out[3 * i + q] - expect[q]) <= 1e-10);
  }
  return 0;
}

int CaseDifferentialHandleMotion() {
  TriMesh m = GridMesh(3, 2, false);
  std::vector<std::vector<int> > handles(2);
  handles[0].push_back(0);
  handles[1].push_back(2);
  tinyobj::BoundedWeights bw(MakeAttrib(m), MakeShapes(m), handles);
  TASK_CHECK(bw.solve(200000));

  std::vector<tinyobj::BoundedAffine> xf(2);  // both identity initially
  xf[1].translation[0] = 3.0;                 // handle 1 translates along x
  std::vector<double> out = bw.deform(xf);
  for (int i = 0; i < m.n; ++i) {
    double shift = 3.0 * bw.weight(i, 1);
    TASK_CHECK(std::fabs(out[3 * i] - (m.pos[3 * i] + shift)) <= 1e-10);
    TASK_CHECK(std::fabs(out[3 * i + 1] - m.pos[3 * i + 1]) <= 1e-10);
    TASK_CHECK(std::fabs(out[3 * i + 2] - m.pos[3 * i + 2]) <= 1e-10);
  }
  TASK_CHECK(std::fabs(out[3 * 2] - (m.pos[6] + 3.0)) <= 1e-10);
  TASK_CHECK(std::fabs(out[0] - m.pos[0]) <= 1e-10);
  return 0;
}

int CaseRepeatedTransformsNoAccumulation() {
  TriMesh m = GridMesh(3, 2, false);
  std::vector<std::vector<int> > handles(2);
  handles[0].push_back(0);
  handles[1].push_back(2);
  tinyobj::BoundedWeights bw(MakeAttrib(m), MakeShapes(m), handles);
  TASK_CHECK(bw.solve(200000));
  std::vector<double> weights_before = bw.weights();
  size_t ops_before = bw.info().operator_applications;
  size_t solves_before = bw.info().successful_solves;

  std::vector<tinyobj::BoundedAffine> xf(2);
  std::vector<double> first = bw.deform(xf);  // identity: rest pose
  xf[1].translation[0] = 4.0;
  std::vector<double> moved = bw.deform(xf);
  xf[0].translation[2] = -2.0;
  std::vector<double> other = bw.deform(xf);
  xf[0] = tinyobj::BoundedAffine();
  xf[1] = tinyobj::BoundedAffine();
  std::vector<double> again = bw.deform(xf);
  TASK_CHECK(first == again);
  TASK_CHECK(moved != first && other != moved);
  for (int i = 0; i < m.n; ++i)
    for (int q = 0; q < 3; ++q)
      TASK_CHECK(std::fabs(again[3 * i + q] - m.pos[3 * i + q]) <= 1e-12);
  TASK_CHECK(bw.weights() == weights_before);
  TASK_CHECK(bw.info().operator_applications == ops_before);
  TASK_CHECK(bw.info().successful_solves == solves_before);

  TASK_CHECK(ThrowsInvalidArg([&] {
    std::vector<tinyobj::BoundedAffine> bad(1);
    bw.deform(bad);
  }));
  return 0;
}

int CaseReleaseInputThenDeform() {
  std::unique_ptr<tinyobj::BoundedWeights> bw;
  {
    TriMesh m = GridMesh(4, 2, true);
    tinyobj::attrib_t attr = MakeAttrib(m);
    std::vector<tinyobj::shape_t> shapes = MakeShapes(m);
    std::vector<std::vector<int> > handles(2);
    handles[0].push_back(0);
    handles[1].push_back(3);
    bw.reset(new tinyobj::BoundedWeights(attr, shapes, handles));
    TASK_CHECK(bw->solve(200000));
  }
  std::vector<tinyobj::BoundedAffine> identity(2);
  std::vector<double> rest = bw->deform(identity);
  std::vector<tinyobj::BoundedAffine> xf(2);
  xf[0].translation[1] = 5.0;
  std::vector<double> out = bw->deform(xf);
  TASK_CHECK(out.size() == 3 * bw->num_positions());
  TASK_CHECK(rest.size() == out.size());
  for (size_t i = 0; i < bw->num_positions(); ++i) {
    // Only handle 0 moves, so the displacement is 5 * w(i, 0) along y and
    // the other two axes are unchanged.
    TASK_CHECK(std::fabs(out[3 * i + 1] -
                         (rest[3 * i + 1] + 5.0 * bw->weight(i, 0))) <=
               1e-10);
    TASK_CHECK(std::fabs(out[3 * i] - rest[3 * i]) <= 1e-12);
    TASK_CHECK(std::fabs(out[3 * i + 2] - rest[3 * i + 2]) <= 1e-12);
  }
  return 0;
}

// ===========================================================================
// failures
// ===========================================================================
int CaseRejectGeometry() {
  auto build = [](const TriMesh& m,
                  const std::vector<std::vector<int> >& handles) {
    tinyobj::BoundedWeights bw(MakeAttrib(m), MakeShapes(m), handles);
    (void)bw;
  };
  std::vector<std::vector<int> > ok_handles(2);
  ok_handles[0].push_back(0);
  ok_handles[1].push_back(1);

  // empty mesh: no faces
  {
    TriMesh m;
    m.n = 3;
    double p[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    m.pos.assign(p, p + 9);
    TASK_CHECK(ThrowsInvalidArg([&] { build(m, ok_handles); }));
  }
  // unused position
  {
    TriMesh m;
    m.n = 4;
    double p[12] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 2, 2, 0};
    m.pos.assign(p, p + 12);
    int t[3] = {0, 1, 2};
    m.tris.assign(t, t + 3);
    TASK_CHECK(ThrowsInvalidArg([&] { build(m, ok_handles); }));
  }
  // non-triangle face
  {
    tinyobj::attrib_t attr;
    double p[12] = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    attr.vertices.assign(p, p + 12);
    tinyobj::shape_t sh;
    for (int i = 0; i < 4; ++i) {
      tinyobj::index_t idx;
      idx.vertex_index = i;
      idx.normal_index = idx.texcoord_index = 0;
      sh.mesh.indices.push_back(idx);
    }
    sh.mesh.num_face_vertices.push_back(4);
    std::vector<tinyobj::shape_t> shapes(1, sh);
    TASK_CHECK(ThrowsInvalidArg(
        [&] { tinyobj::BoundedWeights bw(attr, shapes, ok_handles); }));
  }
  // non-finite coordinate
  {
    TriMesh m;
    m.n = 3;
    double p[9] = {0, 0, 0, 1, 0, 0, 0,
                   std::numeric_limits<double>::infinity(), 0};
    m.pos.assign(p, p + 9);
    int t[3] = {0, 1, 2};
    m.tris.assign(t, t + 3);
    TASK_CHECK(ThrowsInvalidArg([&] { build(m, ok_handles); }));
  }
  // degenerate zero-area face
  {
    TriMesh m;
    m.n = 3;
    double p[9] = {0, 0, 0, 1, 0, 0, 2, 0, 0};
    m.pos.assign(p, p + 9);
    int t[3] = {0, 1, 2};
    m.tris.assign(t, t + 3);
    TASK_CHECK(ThrowsInvalidArg([&] { build(m, ok_handles); }));
  }
  // out-of-range corner index
  {
    TriMesh m;
    m.n = 3;
    double p[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    m.pos.assign(p, p + 9);
    int t[3] = {0, 1, 3};
    m.tris.assign(t, t + 3);
    TASK_CHECK(ThrowsInvalidArg([&] { build(m, ok_handles); }));
  }
  // non-manifold edge shared by three faces
  {
    TriMesh m;
    m.n = 5;
    double p[15] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 0.5, -1, 0};
    m.pos.assign(p, p + 15);
    int t[9] = {0, 1, 2, 0, 1, 3, 0, 1, 4};
    m.tris.assign(t, t + 9);
    std::vector<std::vector<int> > handles(2);
    handles[0].push_back(2);
    handles[1].push_back(4);
    TASK_CHECK(ThrowsInvalidArg([&] { build(m, handles); }));
  }
  // duplicate face
  {
    TriMesh m;
    m.n = 4;
    double p[12] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0};
    m.pos.assign(p, p + 12);
    int t[6] = {0, 1, 2, 0, 2, 1};
    m.tris.assign(t, t + 6);
    TASK_CHECK(ThrowsInvalidArg([&] { build(m, ok_handles); }));
  }
  return 0;
}

int CaseRejectHandlesAndComponents() {
  TriMesh m;
  m.n = 6;
  double p[18] = {0, 0, 0, 1, 0, 0, 0, 1, 0,
                  5, 0, 0, 6, 0, 0, 5, 1, 0};
  m.pos.assign(p, p + 18);
  int t[6] = {0, 1, 2, 3, 4, 5};
  m.tris.assign(t, t + 6);

  {  // empty handle
    std::vector<std::vector<int> > handles(2);
    handles[0].push_back(0);
    TASK_CHECK(ThrowsInvalidArg(
        [&] { tinyobj::BoundedWeights bw(MakeAttrib(m), MakeShapes(m),
                                         handles); }));
  }
  {  // duplicate ownership
    std::vector<std::vector<int> > handles(2);
    handles[0].push_back(0);
    handles[1].push_back(0);
    handles[1].push_back(4);
    TASK_CHECK(ThrowsInvalidArg(
        [&] { tinyobj::BoundedWeights bw(MakeAttrib(m), MakeShapes(m),
                                         handles); }));
  }
  {  // second component has no handle
    std::vector<std::vector<int> > handles(2);
    handles[0].push_back(0);
    handles[1].push_back(1);
    TASK_CHECK(ThrowsInvalidArg(
        [&] { tinyobj::BoundedWeights bw(MakeAttrib(m), MakeShapes(m),
                                         handles); }));
  }
  {  // handle index out of range
    std::vector<std::vector<int> > handles(2);
    handles[0].push_back(6);
    handles[1].push_back(4);
    TASK_CHECK(ThrowsInvalidArg(
        [&] { tinyobj::BoundedWeights bw(MakeAttrib(m), MakeShapes(m),
                                         handles); }));
  }
  // Valid: one handle per disconnected component.
  std::vector<std::vector<int> > handles(2);
  handles[0].push_back(0);
  handles[1].push_back(4);
  tinyobj::BoundedWeights bw(MakeAttrib(m), MakeShapes(m), handles);
  TASK_CHECK(bw.info().num_components == 2);
  TASK_CHECK(bw.solve(200000));
  // v0 and v4 are fixed handle rows (bit-exact unit vectors); the other four
  // free rows converge to the unique pure-row optimum of their component.
  TASK_CHECK(bw.weight(0, 0) == 1.0 && bw.weight(0, 1) == 0.0);
  TASK_CHECK(bw.weight(4, 0) == 0.0 && bw.weight(4, 1) == 1.0);
  for (int i = 1; i <= 2; ++i) {
    TASK_CHECK(std::fabs(bw.weight(i, 0) - 1.0) <= 1e-7 &&
               std::fabs(bw.weight(i, 1)) <= 1e-7);
  }
  for (int i : {3, 5}) {
    TASK_CHECK(std::fabs(bw.weight(i, 0)) <= 1e-7 &&
               std::fabs(bw.weight(i, 1) - 1.0) <= 1e-7);
  }
  return 0;
}

int CaseFailureKeepsOldWeights() {
  TriMesh m = GridMesh(3, 2, false);
  std::vector<std::vector<int> > handles(2);
  handles[0].push_back(1);
  handles[1].push_back(0);
  tinyobj::BoundedWeights bw(MakeAttrib(m), MakeShapes(m), handles);
  TASK_CHECK(!bw.has_solution());
  TASK_CHECK(!bw.solve(0));  // budget exhausted before any iteration
  TASK_CHECK(!bw.has_solution());
  TASK_CHECK(bw.info().converged == false);

  TASK_CHECK(bw.solve(200000));
  TASK_CHECK(bw.has_solution());
  std::vector<double> good = bw.weights();
  double good_energy = bw.energy();
  size_t solves = bw.info().successful_solves;
  TASK_CHECK(solves == 1);

  TASK_CHECK(!bw.solve(1));  // cannot converge in one step
  TASK_CHECK(bw.has_solution());
  TASK_CHECK(bw.info().converged == false);
  TASK_CHECK(bw.weights() == good);  // previous converged weights survive
  TASK_CHECK(bw.energy() == good_energy);
  TASK_CHECK(bw.info().successful_solves == solves);

  TASK_CHECK(bw.solve(200000));  // a later solve can succeed again
  TASK_CHECK(bw.info().successful_solves == solves + 1);
  TASK_CHECK(bw.projection_residual() <= 1e-8);

  // Pre-solve usage errors.
  tinyobj::BoundedWeights fresh(MakeAttrib(m), MakeShapes(m), handles);
  bool logic_export = false, logic_deform = false;
  try {
    fresh.export_skin_weights();
  } catch (const std::logic_error&) {
    logic_export = true;
  }
  try {
    std::vector<tinyobj::BoundedAffine> xf(2);
    fresh.deform(xf);
  } catch (const std::logic_error&) {
    logic_deform = true;
  }
  TASK_CHECK(logic_export && logic_deform);
  return 0;
}

int CaseLargeSparseSurface() {
  TriMesh m = GridMesh(40, 50, true);  // 2000 positions, non-planar
  TASK_CHECK(m.n == 2000);
  std::vector<std::vector<int> > handles(2);
  for (int j = 0; j < 50; ++j)
    for (int i = 0; i < 40; ++i) {
      int id = j * 40 + i;
      if (j % 2 == 0) {
        if ((j / 2) % 2 == 0)
          handles[0].push_back(id);
        else
          handles[1].push_back(id);
      }
    }
  tinyobj::BoundedWeights bw(MakeAttrib(m), MakeShapes(m), handles);
  // Sparse storage: average vertex degree is below 8, so L stays O(N).
  TASK_CHECK(bw.info().l_nonzeros < 20 * static_cast<size_t>(m.n));
  TASK_CHECK(bw.info().num_faces == m.tris.size() / 3);
  TASK_CHECK(bw.info().free_dofs > 0);
  TASK_CHECK(bw.solve(2000000));
  TASK_CHECK(bw.projection_residual() <= 1e-8);
  TASK_CHECK(std::isfinite(bw.energy()) && bw.energy() > 0.0);
  TASK_CHECK(bw.info().operator_applications ==
             bw.info().iterations_last + 3);
  TASK_CHECK(bw.info().successful_solves == 1);
  for (int i = 0; i < m.n; ++i) {
    double sum = 0.0;
    for (int k = 0; k < 2; ++k) {
      TASK_CHECK(bw.weight(i, k) >= -1e-10);
      sum += bw.weight(i, k);
    }
    TASK_CHECK(std::fabs(sum - 1.0) <= 1e-10);
  }
  std::vector<tinyobj::skin_weight_t> exported = bw.export_skin_weights();
  TASK_CHECK(exported.size() == static_cast<size_t>(m.n));
  TASK_CHECK(exported.back().vertex_id == m.n - 1);
  return 0;
}

}  // namespace

const struct task_case task_cases[] = {
    {"optimality", "active_zero_vs_kkt", CaseActiveZeroVsKkt},
    {"optimality", "obtuse_cotangents", CaseObtuseCotangents},
    {"optimality", "dense_energy_counters", CaseDenseEnergyCounters},
    {"seams", "obj_reader_seam_identity", CaseObjReaderSeamIdentity},
    {"seams", "no_welding_and_immutable_input", CaseNoWeldingAndImmutableInput},
    {"seams", "owns_input_after_release", CaseOwnsInputAfterRelease},
    {"deformation", "common_rigid_body", CaseCommonRigidBody},
    {"deformation", "differential_handle_motion", CaseDifferentialHandleMotion},
    {"deformation", "repeated_transforms_no_accumulation",
     CaseRepeatedTransformsNoAccumulation},
    {"deformation", "release_input_then_deform", CaseReleaseInputThenDeform},
    {"failures", "reject_geometry", CaseRejectGeometry},
    {"failures", "reject_handles_and_components",
     CaseRejectHandlesAndComponents},
    {"failures", "failure_keeps_old_weights", CaseFailureKeepsOldWeights},
    {"failures", "large_sparse_surface", CaseLargeSparseSurface},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
