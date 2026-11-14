#include "bounded_weights.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace tinyobj {

namespace {

// The acceptance criterion is a projected-gradient residual no larger than
// 1e-8. The internal iterate is driven two orders of magnitude further so
// that ill-conditioned (e.g. obtuse) meshes still deliver weights within
// 1e-6 of the exact constrained optimum; the reported residual is then also
// well inside the required 1e-8 bound.
const double kResidualTolerance = 1e-10;
const double kRequiredResidual = 1e-8;
const double kRowErrorTolerance = 1e-10;

[[noreturn]] void Fail(const std::string& message) {
  throw std::invalid_argument(message);
}

// Exact projection of v[0..k) onto the probability simplex
// {x : x >= 0, sum x = 1} (Duchi et al. 2008). `scratch` is a caller-owned
// buffer of at least k doubles used for sorting, so no allocation happens per
// call.
void ProjectSimplex(double* v, size_t k, std::vector<double>* scratch) {
  scratch->assign(v, v + k);
  std::sort(scratch->begin(), scratch->end(), std::greater<double>());
  double cumulative = 0.0;
  size_t rho = 0;
  double theta = 0.0;
  for (size_t j = 0; j < k; ++j) {
    cumulative += (*scratch)[j];
    if ((*scratch)[j] + (1.0 - cumulative) / static_cast<double>(j + 1) >
        0.0) {
      rho = j + 1;
      theta = (cumulative - 1.0) / static_cast<double>(rho);
    }
  }
  for (size_t j = 0; j < k; ++j) {
    double x = v[j] - theta;
    v[j] = x > 0.0 ? x : 0.0;
  }
}

}  // namespace

BoundedAffine::BoundedAffine() {
  for (int i = 0; i < 9; ++i) linear[i] = 0.0;
  linear[0] = linear[4] = linear[8] = 1.0;
  translation[0] = translation[1] = translation[2] = 0.0;
}

BoundedAffine BoundedAffine::Identity() { return BoundedAffine(); }

BoundedWeightInfo::BoundedWeightInfo()
    : num_positions(0),
      num_used(0),
      num_handles(0),
      num_faces(0),
      num_edges(0),
      l_nonzeros(0),
      num_components(0),
      free_dofs(0),
      row_sum_scale(0.0),
      operator_applications(0),
      successful_solves(0),
      iterations_last(0),
      converged(false),
      energy(0.0),
      projection_residual(0.0) {}

BoundedWeights::BoundedWeights(const attrib_t& attrib,
                               const std::vector<shape_t>& shapes,
                               const std::vector<std::vector<int> >& handles)
    : n_(0), k_(0), solved_(false) {
  build(attrib, shapes, handles);
}

size_t BoundedWeights::num_positions() const { return n_; }

size_t BoundedWeights::num_handles() const { return k_; }

bool BoundedWeights::has_solution() const { return solved_; }

double BoundedWeights::weight(size_t position, size_t handle) const {
  if (position >= n_ || handle >= k_) Fail("weight index out of range");
  return weights_[position * k_ + handle];
}

const std::vector<double>& BoundedWeights::weights() const { return weights_; }

double BoundedWeights::energy() const { return info_.energy; }

double BoundedWeights::projection_residual() const {
  return info_.projection_residual;
}

const BoundedWeightInfo& BoundedWeights::info() const { return info_; }

void BoundedWeights::apply_q(const std::vector<double>& w,
                             std::vector<double>* qw) {
  qw->assign(n_ * k_, 0.0);
  for (size_t h = 0; h < k_; ++h) {
    for (size_t i = 0; i < n_; ++i) {
      double acc = 0.0;
      for (size_t p = q_.row_ptr[i]; p < q_.row_ptr[i + 1]; ++p) {
        acc += q_.val[p] * w[static_cast<size_t>(q_.col_ind[p]) * k_ + h];
      }
      (*qw)[i * k_ + h] = acc;
    }
  }
  ++info_.operator_applications;
}

double BoundedWeights::compute_residual(const std::vector<double>& w,
                                        const std::vector<double>& qw,
                                        double scale) const {
  if (info_.free_dofs == 0 || scale == 0.0) return 0.0;
  const double inv_s = 1.0 / scale;
  std::vector<double> row(k_);
  std::vector<double> scratch(k_);
  double max_delta = 0.0;
  for (size_t i = 0; i < n_; ++i) {
    if (fixed_[i] >= 0) continue;
    for (size_t h = 0; h < k_; ++h)
      row[h] = w[i * k_ + h] - qw[i * k_ + h] * inv_s;
    ProjectSimplex(row.data(), k_, &scratch);
    for (size_t h = 0; h < k_; ++h) {
      double delta = std::fabs(row[h] - w[i * k_ + h]);
      if (delta > max_delta) max_delta = delta;
    }
  }
  return max_delta;
}

void BoundedWeights::build(const attrib_t& attrib,
                           const std::vector<shape_t>& shapes,
                           const std::vector<std::vector<int> >& handles) {
  if (attrib.vertices.size() % 3 != 0) Fail("vertices are not triples");
  n_ = attrib.vertices.size() / 3;
  if (n_ == 0) Fail("empty mesh");

  rest_.assign(attrib.vertices.begin(), attrib.vertices.end());
  for (size_t i = 0; i < rest_.size(); ++i) {
    if (!std::isfinite(rest_[i])) Fail("non-finite vertex coordinate");
  }

  k_ = handles.size();
  if (k_ == 0) Fail("no handles given");

  // ---- faces -------------------------------------------------------------
  std::vector<unsigned char> used(n_, 0);
  struct Face {
    int v[3];
  };
  std::vector<Face> faces;
  std::vector<uint64_t> face_keys;  // duplicate-face detection

  for (size_t s = 0; s < shapes.size(); ++s) {
    const mesh_t& mesh = shapes[s].mesh;
    size_t cursor = 0;
    for (size_t f = 0; f < mesh.num_face_vertices.size(); ++f) {
      unsigned int nv = mesh.num_face_vertices[f];
      if (nv != 3) Fail("non-triangle face");
      if (cursor + 3 > mesh.indices.size())
        Fail("truncated face index list");
      Face face;
      for (int c = 0; c < 3; ++c) {
        int vi = mesh.indices[cursor + c].vertex_index;
        if (vi < 0 || static_cast<size_t>(vi) >= n_)
          Fail("vertex index out of range");
        face.v[c] = vi;
        used[vi] = 1;
      }
      cursor += 3;
      int sorted[3] = {face.v[0], face.v[1], face.v[2]};
      std::sort(sorted, sorted + 3);
      if (sorted[0] == sorted[1] || sorted[1] == sorted[2])
        Fail("degenerate face with repeated vertex");
      uint64_t key = static_cast<uint64_t>(sorted[0]) *
                         static_cast<uint64_t>(n_) * n_ +
                     static_cast<uint64_t>(sorted[1]) * n_ +
                     static_cast<uint64_t>(sorted[2]);
      face_keys.push_back(key);
      faces.push_back(face);
    }
    if (cursor != mesh.indices.size())
      Fail("indices not partitioned into faces");
  }
  if (faces.empty()) Fail("empty mesh");

  std::sort(face_keys.begin(), face_keys.end());
  for (size_t i = 1; i < face_keys.size(); ++i) {
    if (face_keys[i] == face_keys[i - 1]) Fail("duplicate face");
  }
  for (size_t i = 0; i < n_; ++i) {
    if (!used[i]) Fail("unused position in mesh");
  }

  // ---- per-face geometry: cotangent contributions and lumped masses ------
  std::vector<double> mass(n_, 0.0);
  struct EdgeInfo {
    int lo, hi;
    double cot;
  };
  std::vector<std::pair<uint64_t, EdgeInfo> > edge_build;
  edge_build.reserve(faces.size() * 3);

  for (size_t f = 0; f < faces.size(); ++f) {
    const int* v = faces[f].v;
    double p[3][3];
    for (int c = 0; c < 3; ++c)
      for (int d = 0; d < 3; ++d) p[c][d] = rest_[3 * v[c] + d];
    double e1[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1],
                    p[1][2] - p[0][2]};
    double e2[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1],
                    p[2][2] - p[0][2]};
    double cross[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                       e1[2] * e2[0] - e1[0] * e2[2],
                       e1[0] * e2[1] - e1[1] * e2[0]};
    double cross_norm2 = cross[0] * cross[0] + cross[1] * cross[1] +
                         cross[2] * cross[2];
    if (!(cross_norm2 > 0.0)) Fail("degenerate zero-area face");
    double area = 0.5 * std::sqrt(cross_norm2);
    for (int c = 0; c < 3; ++c) mass[v[c]] += area / 3.0;

    // At vertex v[c], the opposite edge joins the other two vertices.
    for (int c = 0; c < 3; ++c) {
      int a = v[(c + 1) % 3];
      int b = v[(c + 2) % 3];
      double u[3] = {p[(c + 1) % 3][0] - p[c][0],
                     p[(c + 1) % 3][1] - p[c][1],
                     p[(c + 1) % 3][2] - p[c][2]};
      double wvec[3] = {p[(c + 2) % 3][0] - p[c][0],
                        p[(c + 2) % 3][1] - p[c][1],
                        p[(c + 2) % 3][2] - p[c][2]};
      double dot = u[0] * wvec[0] + u[1] * wvec[1] + u[2] * wvec[2];
      double cot = dot / std::sqrt(cross_norm2);  // negative values are kept
      if (!std::isfinite(cot)) Fail("non-finite cotangent weight");
      int lo = a < b ? a : b;
      int hi = a < b ? b : a;
      EdgeInfo info;
      info.lo = lo;
      info.hi = hi;
      info.cot = cot;
      uint64_t key = static_cast<uint64_t>(lo) * n_ +
                     static_cast<uint64_t>(hi);
      edge_build.push_back(std::make_pair(key, info));
    }
  }
  std::sort(edge_build.begin(), edge_build.end(),
            [](const std::pair<uint64_t, EdgeInfo>& a,
               const std::pair<uint64_t, EdgeInfo>& b) {
              return a.first < b.first;
            });

  // Merge edge contributions; reject edges shared by more than two faces.
  struct Edge {
    int lo, hi;
    double cot_sum;
  };
  std::vector<Edge> edges;
  for (size_t i = 0; i < edge_build.size();) {
    size_t j = i + 1;
    double cot_sum = edge_build[i].second.cot;
    while (j < edge_build.size() &&
           edge_build[j].first == edge_build[i].first) {
      cot_sum += edge_build[j].second.cot;
      ++j;
    }
    if (j - i > 2) Fail("non-manifold edge");
    Edge e;
    e.lo = edge_build[i].second.lo;
    e.hi = edge_build[i].second.hi;
    e.cot_sum = cot_sum;
    edges.push_back(e);
    i = j;
  }

  // ---- adjacency and connected components --------------------------------
  std::vector<std::vector<int> > adjacency(n_);
  for (size_t e = 0; e < edges.size(); ++e) {
    adjacency[edges[e].lo].push_back(edges[e].hi);
    adjacency[edges[e].hi].push_back(edges[e].lo);
  }
  std::vector<int> component(n_, -1);
  size_t num_components = 0;
  for (size_t seed = 0; seed < n_; ++seed) {
    if (component[seed] != -1) continue;
    component[seed] = static_cast<int>(num_components);
    std::vector<int> stack(1, static_cast<int>(seed));
    while (!stack.empty()) {
      int cur = stack.back();
      stack.pop_back();
      for (size_t j = 0; j < adjacency[cur].size(); ++j) {
        int nb = adjacency[cur][j];
        if (component[nb] == -1) {
          component[nb] = component[cur];
          stack.push_back(nb);
        }
      }
    }
    ++num_components;
  }

  // ---- handles ------------------------------------------------------------
  fixed_.assign(n_, -1);
  std::vector<unsigned char> component_has_handle(num_components, 0);
  for (size_t h = 0; h < handles.size(); ++h) {
    if (handles[h].empty()) Fail("empty handle");
    for (size_t j = 0; j < handles[h].size(); ++j) {
      int vi = handles[h][j];
      if (vi < 0 || static_cast<size_t>(vi) >= n_)
        Fail("handle vertex index out of range");
      if (fixed_[vi] != -1) Fail("vertex assigned to more than one handle");
      fixed_[vi] = static_cast<int>(h);
      component_has_handle[component[vi]] = 1;
    }
  }
  for (size_t c = 0; c < num_components; ++c) {
    if (!component_has_handle[c])
      Fail("connected component without a handle vertex");
  }

  // ---- build L in CSR -----------------------------------------------------
  // L_ij = -1/2 * cot_sum(edge ij) for i != j; the diagonal makes each row
  // sum to zero. Boundary edges simply carry their single-side contribution.
  // Entries that are exactly zero (right angles) are not stored.
  std::vector<std::vector<std::pair<int, double> > > l_rows(n_);
  for (size_t i = 0; i < n_; ++i)
    l_rows[i].push_back(std::make_pair(static_cast<int>(i), 0.0));
  for (size_t e = 0; e < edges.size(); ++e) {
    double off = -0.5 * edges[e].cot_sum;
    if (off == 0.0) continue;
    l_rows[edges[e].lo].push_back(std::make_pair(edges[e].hi, off));
    l_rows[edges[e].hi].push_back(std::make_pair(edges[e].lo, off));
  }
  l_.n = n_;
  l_.row_ptr.assign(n_ + 1, 0);
  size_t l_nnz = 0;
  for (size_t i = 0; i < n_; ++i) {
    std::sort(l_rows[i].begin(), l_rows[i].end());
    double diag = 0.0;
    size_t self_slot = 0;
    for (size_t j = 0; j < l_rows[i].size(); ++j) {
      if (static_cast<size_t>(l_rows[i][j].first) == i) {
        self_slot = j;
      } else {
        diag -= l_rows[i][j].second;
      }
    }
    l_rows[i][self_slot].second = diag;
    l_.row_ptr[i + 1] = l_.row_ptr[i] + l_rows[i].size();
    l_nnz += l_rows[i].size();
  }
  l_.col_ind.resize(l_nnz);
  l_.val.resize(l_nnz);
  for (size_t i = 0; i < n_; ++i) {
    for (size_t j = 0; j < l_rows[i].size(); ++j) {
      size_t pos = l_.row_ptr[i] + j;
      l_.col_ind[pos] = l_rows[i][j].first;
      l_.val[pos] = l_rows[i][j].second;
    }
  }

  inv_mass_.resize(n_);
  for (size_t i = 0; i < n_; ++i) {
    if (!(mass[i] > 0.0)) Fail("vertex with zero mass");
    inv_mass_[i] = 1.0 / mass[i];
  }

  // ---- build Q = L^T M^{-1} L in sparse CSR (L is symmetric) -------------
  q_.n = n_;
  q_.row_ptr.assign(n_ + 1, 0);
  q_.col_ind.clear();
  q_.val.clear();
  std::vector<std::pair<int, double> > acc;
  for (size_t i = 0; i < n_; ++i) {
    acc.clear();
    for (size_t pk = l_.row_ptr[i]; pk < l_.row_ptr[i + 1]; ++pk) {
      int kk = l_.col_ind[pk];
      double scale = l_.val[pk] * inv_mass_[kk];
      for (size_t pj = l_.row_ptr[kk]; pj < l_.row_ptr[kk + 1]; ++pj) {
        acc.push_back(
            std::make_pair(l_.col_ind[pj], scale * l_.val[pj]));
      }
    }
    std::sort(acc.begin(), acc.end());
    for (size_t j = 0; j < acc.size();) {
      size_t t = j + 1;
      double sum = acc[j].second;
      while (t < acc.size() && acc[t].first == acc[j].first) {
        sum += acc[t].second;
        ++t;
      }
      q_.col_ind.push_back(acc[j].first);
      q_.val.push_back(sum);
      j = t;
    }
    q_.row_ptr[i + 1] = q_.col_ind.size();
  }

  double scale = 0.0;
  for (size_t i = 0; i < n_; ++i) {
    double row_abs = 0.0;
    for (size_t p = q_.row_ptr[i]; p < q_.row_ptr[i + 1]; ++p)
      row_abs += std::fabs(q_.val[p]);
    if (row_abs > scale) scale = row_abs;
  }
  if (!(scale > 0.0)) Fail("degenerate quadratic form");

  // ---- initial feasible weights ------------------------------------------
  weights_.assign(n_ * k_, 0.0);
  size_t free_dofs = 0;
  for (size_t i = 0; i < n_; ++i) {
    if (fixed_[i] >= 0) {
      weights_[i * k_ + fixed_[i]] = 1.0;
    } else {
      for (size_t h = 0; h < k_; ++h)
        weights_[i * k_ + h] = 1.0 / static_cast<double>(k_);
      ++free_dofs;
    }
  }

  info_.num_positions = n_;
  info_.num_used = n_;
  info_.num_handles = k_;
  info_.num_faces = faces.size();
  info_.num_edges = edges.size();
  info_.l_nonzeros = l_nnz;
  info_.num_components = num_components;
  info_.free_dofs = free_dofs;
  info_.row_sum_scale = scale;

  std::vector<double> qw(n_ * k_);
  apply_q(weights_, &qw);
  double energy = 0.0;
  for (size_t i = 0; i < n_ * k_; ++i) energy += weights_[i] * qw[i];
  info_.energy = 0.5 * energy;
  info_.projection_residual =
      compute_residual(weights_, qw, info_.row_sum_scale);
}

bool BoundedWeights::solve(size_t max_iterations) {
  // Every attempt starts from the canonical feasible initialization (unit
  // rows on handle vertices, uniform rows elsewhere). The accepted weights in
  // weights_ are only replaced once a new iterate actually converges, so an
  // exhausted budget leaves any previous successful solution in place.
  std::vector<double> current(n_ * k_);
  for (size_t i = 0; i < n_; ++i) {
    if (fixed_[i] >= 0) {
      current[i * k_ + fixed_[i]] = 1.0;
    } else {
      for (size_t h = 0; h < k_; ++h)
        current[i * k_ + h] = 1.0 / static_cast<double>(k_);
    }
  }
  std::vector<double> qw(n_ * k_);
  std::vector<double> candidate(n_ * k_);
  std::vector<double> row(k_);
  std::vector<double> scratch(k_);
  apply_q(current, &qw);
  size_t iterations = 0;
  double residual =
      compute_residual(current, qw, info_.row_sum_scale);
  const double inv_s = 1.0 / info_.row_sum_scale;

  while (residual > kResidualTolerance && iterations < max_iterations) {
    ++iterations;
    candidate = current;
    double max_delta = 0.0;
    for (size_t i = 0; i < n_; ++i) {
      if (fixed_[i] >= 0) continue;  // handle rows stay fixed
      for (size_t h = 0; h < k_; ++h)
        row[h] = current[i * k_ + h] - qw[i * k_ + h] * inv_s;
      ProjectSimplex(row.data(), k_, &scratch);
      for (size_t h = 0; h < k_; ++h) {
        double delta = std::fabs(row[h] - current[i * k_ + h]);
        if (delta > max_delta) max_delta = delta;
        candidate[i * k_ + h] = row[h];
      }
    }
    residual = max_delta;
    current.swap(candidate);
    apply_q(current, &qw);
  }

  info_.iterations_last = iterations;
  double final_residual =
      compute_residual(current, qw, info_.row_sum_scale);
  // The specification accepts any residual <= 1e-8. The loop drives the
  // iterate to 1e-10 while the budget permits (so weights on ill-conditioned
  // meshes stay within 1e-6 of the exact optimum), but a budget that runs out
  // between the two bounds still counts as a converged solve.
  if (final_residual > kRequiredResidual) {
    info_.converged = false;
    // Keep the diagnostics consistent with the weights that remain stored:
    // weights_ (and energy_) are untouched, so report its own residual.
    apply_q(weights_, &qw);
    info_.projection_residual =
        compute_residual(weights_, qw, info_.row_sum_scale);
    return false;
  }
  // Verify feasibility tolerances before accepting the iterate.
  for (size_t i = 0; i < n_; ++i) {
    double sum = 0.0;
    for (size_t h = 0; h < k_; ++h) {
      double x = current[i * k_ + h];
      if (x < -kRowErrorTolerance) Fail("solver produced negative weight");
      sum += x;
    }
    if (std::fabs(sum - 1.0) > kRowErrorTolerance)
      Fail("solver row sum violated");
  }

  weights_.swap(current);
  solved_ = true;
  info_.converged = true;
  info_.successful_solves += 1;
  apply_q(weights_, &qw);
  double energy = 0.0;
  for (size_t i = 0; i < n_ * k_; ++i) energy += weights_[i] * qw[i];
  info_.energy = 0.5 * energy;
  info_.projection_residual =
      compute_residual(weights_, qw, info_.row_sum_scale);
  return true;
}

std::vector<skin_weight_t> BoundedWeights::export_skin_weights() const {
  if (!solved_)
    throw std::logic_error("export_skin_weights before a converged solve");
  std::vector<skin_weight_t> out;
  for (size_t i = 0; i < n_; ++i) {
    skin_weight_t sw;
    sw.vertex_id = static_cast<int>(i);
    for (size_t h = 0; h < k_; ++h) {
      double w = weights_[i * k_ + h];
      if (w != 0.0) {
        joint_and_weight_t jw;
        jw.joint_id = static_cast<int>(h);
        jw.weight = static_cast<real_t>(w);
        sw.weightValues.push_back(jw);
      }
    }
    out.push_back(sw);
  }
  return out;
}

std::vector<double> BoundedWeights::deform(
    const std::vector<BoundedAffine>& transforms) const {
  if (!solved_) throw std::logic_error("deform before a converged solve");
  if (transforms.size() != k_)
    throw std::invalid_argument("transform count does not match handle count");
  std::vector<double> out(3 * n_, 0.0);
  for (size_t i = 0; i < n_; ++i) {
    double px = rest_[3 * i];
    double py = rest_[3 * i + 1];
    double pz = rest_[3 * i + 2];
    double ox = 0.0, oy = 0.0, oz = 0.0;
    for (size_t h = 0; h < k_; ++h) {
      double w = weights_[i * k_ + h];
      if (w == 0.0) continue;
      const BoundedAffine& t = transforms[h];
      double tx = t.linear[0] * px + t.linear[1] * py + t.linear[2] * pz +
                  t.translation[0];
      double ty = t.linear[3] * px + t.linear[4] * py + t.linear[5] * pz +
                  t.translation[1];
      double tz = t.linear[6] * px + t.linear[7] * py + t.linear[8] * pz +
                  t.translation[2];
      ox += w * tx;
      oy += w * ty;
      oz += w * tz;
    }
    out[3 * i] = ox;
    out[3 * i + 1] = oy;
    out[3 * i + 2] = oz;
  }
  return out;
}

}  // namespace tinyobj

