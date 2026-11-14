// bounded_weights.h
//
// An owning bounded-biharmonic handle-weight context for meshes loaded with
// tinyobjloader.
//
// Given the positions of an `attrib_t`, all triangle faces of the loaded
// `shape_t`s and a list of handle vertex sets, it builds the cotangent
// Laplacian L and the lumped mass matrix M of the surface, and solves
//
//   minimize  E(W) = 1/2 * tr(W^T Q W),  Q = L^T M^{-1} L
//
// subject to, for every surface position i and handle k:
//   W_ik >= 0,  sum_k W_ik = 1,
// and W_i = e_k (the k-th unit row) for every position i that belongs to
// handle k. All columns are optimized jointly; per-column harmonic solves
// followed by clipping/renormalization are *not* used.
//
// The context copies everything it needs: the caller may release the original
// attrib/shapes as soon as construction has returned. The implementation only
// relies on C++11 and the standard library and performs every computation in
// double precision.

#ifndef TINYOBJ_BOUNDED_WEIGHTS_H_
#define TINYOBJ_BOUNDED_WEIGHTS_H_

#include <cstddef>
#include <vector>

#include "tiny_obj_loader.h"

namespace tinyobj {

// Finite affine transform of a handle, expressed relative to the rest pose:
//   T(p) = linear * p + translation
// `linear` is stored row-major as nine doubles (3 rows of 3).
struct BoundedAffine {
  double linear[9];
  double translation[3];

  BoundedAffine();  // Initializes to the identity transform.

  static BoundedAffine Identity();
};

// Diagnostics and counters of a BoundedWeights context.
struct BoundedWeightInfo {
  size_t num_positions;    // Number of positions in the source attrib (N).
  size_t num_used;         // Positions referenced by at least one triangle.
  size_t num_handles;      // Number of handle groups (K).
  size_t num_faces;        // Number of triangle faces consumed.
  size_t num_edges;        // Number of unique undirected surface edges.
  size_t l_nonzeros;       // Actual number of stored nonzeros of sparse L.
  size_t num_components;   // Connected components of the surface graph.
  size_t free_dofs;        // Used positions that are not fixed to a handle.
  double row_sum_scale;    // s = max_i sum_j |Q_ij| (infinity norm of Q).
  size_t operator_applications;  // Cumulative number of sparse Q*W products.
  size_t successful_solves;      // Cumulative number of converged solves.
  size_t iterations_last;        // Iterations used by the last solve() call.
  bool converged;                // Whether the last solve() converged.
  double energy;                 // E(W) of the weights currently stored.
  double projection_residual;    // Last projection residual (see solve()).

  BoundedWeightInfo();
};

class BoundedWeights {
 public:
  // Builds the context.
  //
  // `handles[k]` is the set of zero-based position indices that belong to
  // handle k. Throws std::invalid_argument when:
  //   - the mesh is empty, has positions that no triangle uses, holds
  //     non-triangle faces, non-finite coordinates, degenerate (zero-area)
  //     faces, duplicate faces, non-manifold edges or out-of-range indices;
  //   - a handle is empty, a position belongs to more than one handle, or a
  //     connected component has no handle vertex.
  BoundedWeights(const attrib_t& attrib,
                 const std::vector<shape_t>& shapes,
                 const std::vector<std::vector<int> >& handles);

  size_t num_positions() const;  // N; every position is used by a triangle.
  size_t num_handles() const;    // K.
  bool has_solution() const;     // True after at least one converged solve.

  // Weight W(position, handle). Before a converged solve, fixed handle rows
  // already hold their unit vectors and free rows hold a uniform feasible
  // initialization.
  double weight(size_t position, size_t handle) const;

  // Flat row-major view of the N*K weight matrix (row i, column k).
  const std::vector<double>& weights() const;

  double energy() const;
  double projection_residual() const;
  const BoundedWeightInfo& info() const;

  // Runs the joint projected-gradient solver.
  //
  // One iteration is, with s = max_i sum_j |Q_ij|:
  //   g = Q W / s;
  //   fixed handle rows stay untouched;
  //   every free row of (W - g) is projected onto the probability simplex
  //   (nonnegative entries summing to one) with the exact simplex projection.
  // The reported residual is max |projected(W - g) - W| over all components.
  // Solving stops successfully only when that residual is <= 1e-8 and no more
  // than `max_iterations` iterations have been spent; the iterate is driven
  // toward a 1e-10 internal target while the budget allows. Exhausting the
  // budget returns false and leaves the previously converged weights (if any)
  // available. Row-sum and nonnegativity errors of a converged solution are
  // below 1e-10. Only sparse operator products are used internally.
  bool solve(size_t max_iterations);

  // Exports weights using the existing skin_weight_t layout, ordered by
  // zero-based position index; handle k becomes joint_id = k and entries with
  // zero weight are omitted. Throws std::logic_error before a converged solve.
  std::vector<skin_weight_t> export_skin_weights() const;

  // Computes new positions from rest pose positions p_i:
  //   p'_i = sum_k W_ik * T_k(p_i).
  // The result always starts from the original rest positions: calling this
  // repeatedly with different transforms never accumulates motion, and
  // changing transforms does not rerun the weight solve. Returns 3*N doubles
  // in the original position order. Throws std::invalid_argument when
  // transforms.size() != num_handles() and std::logic_error before a
  // converged solve.
  std::vector<double> deform(const std::vector<BoundedAffine>& transforms) const;

 private:
  struct SparseMatrix {
    size_t n;
    std::vector<size_t> row_ptr;
    std::vector<int> col_ind;
    std::vector<double> val;
  };

  void build(const attrib_t& attrib,
             const std::vector<shape_t>& shapes,
             const std::vector<std::vector<int> >& handles);
  // Sparse matrix-matrix product Q * W for all K weight columns; also bumps
  // the cumulative operator-application counter.
  void apply_q(const std::vector<double>& w, std::vector<double>* qw);
  // max |project_simplex(W_i - (QW)_i/s) - W_i| over the free rows.
  double compute_residual(const std::vector<double>& w,
                          const std::vector<double>& qw, double scale) const;

  size_t n_;
  size_t k_;
  std::vector<double> rest_;           // 3*N rest positions (owned copy).
  std::vector<int> fixed_;             // -1 for free, else handle index.
  std::vector<double> inv_mass_;       // 1/m_i, N entries.
  SparseMatrix l_;                     // Cotangent Laplacian (CSR).
  SparseMatrix q_;                     // Q = L^T M^{-1} L (CSR).
  std::vector<double> weights_;        // N*K, row major.
  BoundedWeightInfo info_;
  bool solved_;
};

}  // namespace tinyobj

#endif  // TINYOBJ_BOUNDED_WEIGHTS_H_
