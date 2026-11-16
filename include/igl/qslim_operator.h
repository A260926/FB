// This file is part of libigl, a simple c++ geometry processing library.
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_QSLIM_OPERATOR_H
#define IGL_QSLIM_OPERATOR_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Sparse>
#include <vector>

namespace igl
{
  /// One entry of igl::qslim_operator_trace: an edge that reached the top of
  /// the decimation heap, in the exact order it was popped.
  struct qslim_operator_trace_entry
  {
    /// Lesser endpoint of the popped edge (the vertex kept on success).
    int s;
    /// Greater endpoint of the popped edge (the vertex removed on success).
    int r;
    /// QSlim quadric-error cost of the candidate when it was popped.
    double qem_cost;
    /// Schur/operator part of the ranking cost:
    ///   lambda * S_rr * || e_s + S_rA / S_rr ||^2
    double operator_penalty;
    /// Full ranking cost used to order the heap (qem_cost +
    /// operator_penalty).
    double total_cost;
    /// Whether the collapse actually happened (false if the pre-collapse
    /// check or the manifold validity test rejected it).
    bool success;
    /// Number of edges refreshed after this pop that were _not_ in the
    /// geometric one-ring (Schur fill-in dependencies). Zero on failure.
    int num_extra_refreshed;
  };

  /// Trace of a igl::qslim_operator run. Entries are appended in real pop
  /// order (including rejected candidates).
  struct qslim_operator_trace
  {
    std::vector<qslim_operator_trace_entry> entries;
    /// Number of edges that were successfully collapsed.
    int num_collapsed = 0;
    /// Number of faces requested.
    int max_m = 0;
    /// Number of (real) faces present when the run ended.
    int final_m = 0;
    /// True if the requested face count was reached.
    bool reached = false;
  };

  /// QSlim decimation coupled to a Schur-complemented scalar operator.
  ///
  /// The mesh is simplified with the original QSlim quadric placement,
  /// candidate positions, collapse admission and binary heap, but the edge
  /// ranking cost is augmented with the harmonic energy preserved by a
  /// sequence of exact Schur eliminations. Let S be the active operator
  /// (initially L0). A collapse keeps the lesser endpoint s and removes the
  /// greater endpoint r; on success
  ///
  ///     S_new = S_AA - S_Ar * S_rA / S_rr
  ///
  /// over the remaining active set A. The QEM optimal position and cost are
  /// unchanged; the ranking cost is
  ///
  ///     QEMcost + lambda * S_rr * || e_s + S_rA / S_rr ||^2
  ///
  /// where e_s is the indicator of s on A. Fill-in edges created by earlier
  /// eliminations participate in S_rA. The elimination and the quadric merge
  /// are committed only after a geometrically successful collapse. Edges
  /// whose cost changes through fill-in (not only the geometric one-ring) are
  /// refreshed in the same heap.
  ///
  /// @param[in] V  #V by 3 finite vertex positions
  /// @param[in] F  #F by 3 oriented, closed, 2-manifold triangle mesh without
  ///   isolated vertices (multiple disconnected components allowed)
  /// @param[in] L0  #V by #V symmetric sparse operator assembled from
  ///   positive conductances on the original geometric edges (off-diagonal
  ///   entries are the negative conductances, diagonal is their sum); each
  ///   connected component of the mesh must form a connected graph in L0
  /// @param[in] max_m  desired number of output faces (>= 4)
  /// @param[in] lambda  non-negative weight of the operator penalty
  /// @param[out] U  #U by 3 coarse vertex positions
  /// @param[out] G  #G by 3 coarse faces (when reached, #G == max_m)
  /// @param[out] J  #G birth faces into F
  /// @param[out] I  #U birth vertices into V
  /// @param[out] K  #U by #U coarse operator, P^T L0 P = K
  /// @param[out] P  #V by #U harmonic prolongation: kept vertices are unit
  ///   rows, eliminated rows satisfy L0 P = 0 and every row sums to 1
  /// @param[out] trace  optional run trace (may be nullptr)
  /// @return true if max_m faces were reached, false otherwise (valid
  ///   current mesh, K and P are still returned)
  ///
  /// Invalid inputs throw std::invalid_argument and leave all outputs
  /// untouched. With lambda == 0 the geometric output matches igl::qslim.
  IGL_INLINE bool qslim_operator(
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
    qslim_operator_trace * trace = nullptr);
}

#ifndef IGL_STATIC_LIBRARY
#  include "qslim_operator.cpp"
#endif
#endif
