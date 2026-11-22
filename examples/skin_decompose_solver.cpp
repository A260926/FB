#include "skin_decompose_solver.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>

namespace skin_decompose {

namespace {

constexpr double kEps = 1e-12;
constexpr double kWeightFloor = 1e-10;

bool finite3(const double* v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

// ---------------------------------------------------------------------------
// Small dense linear algebra. Everything here has fixed dimension (n <= 5) and
// fixed iteration orders, so degenerate inputs still give deterministic output.
// ---------------------------------------------------------------------------

// Jacobi eigen-decomposition of a symmetric n x n matrix (n <= 5).
// On exit, eigenvalues[k] and the k-th column of eigenvectors correspond.
// Eigenvalues are sorted descending; ties keep the ascending-index ordering.
void symmetric_eigen(int n, const std::vector<double>& a_in,
                     std::vector<double>& eigenvalues,
                     std::vector<double>& eigenvectors) {
    std::vector<double> a = a_in;
    eigenvectors.assign(static_cast<std::size_t>(n) * n, 0.0);
    for (int i = 0; i < n; ++i) eigenvectors[i * n + i] = 1.0;
    eigenvalues.assign(n, 0.0);

    double scale = 1.0;
    for (double v : a) scale = std::max(scale, std::fabs(v));
    const double tol = 1e-15 * scale;

    for (int sweep = 0; sweep < 100; ++sweep) {
        double off = 0.0;
        for (int p = 0; p < n; ++p)
            for (int q = p + 1; q < n; ++q)
                off = std::max(off, std::fabs(a[p * n + q]));
        if (off <= tol) break;

        for (int p = 0; p < n; ++p) {
            for (int q = p + 1; q < n; ++q) {
                const double apq = a[p * n + q];
                if (std::fabs(apq) <= tol) continue;
                const double app = a[p * n + p];
                const double aqq = a[q * n + q];
                const double tau = (aqq - app) / (2.0 * apq);
                const double t = (tau >= 0.0 ? 1.0 : -1.0) /
                                 (std::fabs(tau) + std::sqrt(1.0 + tau * tau));
                const double c = 1.0 / std::sqrt(1.0 + t * t);
                const double s = t * c;

                for (int r = 0; r < n; ++r) {
                    const double arp = a[r * n + p];
                    const double arq = a[r * n + q];
                    a[r * n + p] = c * arp - s * arq;
                    a[r * n + q] = s * arp + c * arq;
                }
                for (int r = 0; r < n; ++r) {
                    const double apr = a[p * n + r];
                    const double aqr = a[q * n + r];
                    a[p * n + r] = c * apr - s * aqr;
                    a[q * n + r] = s * apr + c * aqr;
                }
                for (int r = 0; r < n; ++r) {
                    const double vrp = eigenvectors[r * n + p];
                    const double vrq = eigenvectors[r * n + q];
                    eigenvectors[r * n + p] = c * vrp - s * vrq;
                    eigenvectors[r * n + q] = s * vrp + c * vrq;
                }
            }
        }
    }
    for (int i = 0; i < n; ++i) eigenvalues[i] = a[i * n + i];

    std::array<int, 5> idx{};
    for (int i = 0; i < n; ++i) idx[i] = i;
    std::sort(idx.begin(), idx.begin() + n, [&](int x, int y) {
        if (eigenvalues[x] != eigenvalues[y]) return eigenvalues[x] > eigenvalues[y];
        return x < y;
    });
    std::vector<double> vals(n), vecs(static_cast<std::size_t>(n) * n);
    for (int i = 0; i < n; ++i) {
        vals[i] = eigenvalues[idx[i]];
        for (int r = 0; r < n; ++r) vecs[r * n + i] = eigenvectors[r * n + idx[i]];
    }
    eigenvalues = vals;
    eigenvectors = vecs;
}

// Solve a symmetric (possibly singular, possibly indefinite) system using the
// eigendecomposition and a Moore-Penrose pseudo-inverse.
void symmetric_solve(int n, const std::vector<double>& A,
                     const std::vector<double>& b, std::vector<double>& x) {
    std::vector<double> vals, vecs;
    symmetric_eigen(n, A, vals, vecs);
    double threshold = kEps;
    for (double v : vals) threshold = std::max(threshold, std::fabs(v) * 1e-12);
    x.assign(n, 0.0);
    for (int k = 0; k < n; ++k) {
        if (std::fabs(vals[k]) <= threshold) continue;
        double dot = 0.0;
        for (int r = 0; r < n; ++r) dot += vecs[r * n + k] * b[r];
        dot /= vals[k];
        for (int r = 0; r < n; ++r) x[r] += vecs[r * n + k] * dot;
    }
}

double det3(const double m[9]) {
    return m[0] * (m[4] * m[8] - m[5] * m[7]) -
           m[1] * (m[3] * m[8] - m[5] * m[6]) +
           m[2] * (m[3] * m[7] - m[4] * m[6]);
}

// Proper orthogonal factor of a 3x3 matrix H via SVD: H = U S V^T, then
// R = V diag(1, 1, det(V U^T)) U^T. Returns false on a non-finite result.
bool proper_rotation(const double H[9], double R[9]) {
    double MtM[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int r = 0; r < 3; ++r)
                MtM[i * 3 + j] += H[r * 3 + i] * H[r * 3 + j];

    std::vector<double> A(MtM, MtM + 9), vals, vecs;
    symmetric_eigen(3, A, vals, vecs);
    double sigma[3];
    double sigma_max = 0.0;
    for (int i = 0; i < 3; ++i) {
        sigma[i] = std::sqrt(std::max(0.0, vals[i]));
        sigma_max = std::max(sigma_max, sigma[i]);
    }
    const double sigma_tol = std::max(kEps, 1e-12 * sigma_max);

    // V columns are eigenvectors; build U = H V S^-1, then complete the basis.
    double U[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    int have = 0;
    for (int k = 0; k < 3; ++k) {
        double col[3] = {0, 0, 0};
        if (sigma[k] > sigma_tol) {
            for (int r = 0; r < 3; ++r) {
                double dot = 0.0;
                for (int c = 0; c < 3; ++c) dot += H[r * 3 + c] * vecs[c * 3 + k];
                col[r] = dot / sigma[k];
            }
            for (int j = 0; j < have; ++j) {
                double d = col[0] * U[j * 3 + 0] + col[1] * U[j * 3 + 1] +
                           col[2] * U[j * 3 + 2];
                for (int r = 0; r < 3; ++r) col[r] -= d * U[j * 3 + r];
            }
            double n = std::sqrt(col[0] * col[0] + col[1] * col[1] + col[2] * col[2]);
            if (n < 0.5) continue;
            for (int r = 0; r < 3; ++r) col[r] /= n;
        } else {
            continue;
        }
        for (int r = 0; r < 3; ++r) U[have * 3 + r] = col[r];
        ++have;
    }
    static const double axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    int axis = 0;
    while (have < 3) {
        double col[3];
        for (int r = 0; r < 3; ++r) col[r] = axes[axis][r];
        ++axis;
        for (int j = 0; j < have; ++j) {
            double d = col[0] * U[j * 3 + 0] + col[1] * U[j * 3 + 1] +
                       col[2] * U[j * 3 + 2];
            for (int r = 0; r < 3; ++r) col[r] -= d * U[j * 3 + r];
        }
        double n = std::sqrt(col[0] * col[0] + col[1] * col[1] + col[2] * col[2]);
        if (n <= 0.5) {
            if (have == 2) {
                // Cross product of the two stored column vectors U[0..2],
                // U[3..5]: a x b = (a1 b2 - a2 b1, a2 b0 - a0 b2, a0 b1 - a1 b0).
                col[0] = U[1] * U[5] - U[2] * U[4];
                col[1] = U[2] * U[3] - U[0] * U[5];
                col[2] = U[0] * U[4] - U[1] * U[3];
                n = std::sqrt(col[0] * col[0] + col[1] * col[1] + col[2] * col[2]);
            }
            if (n <= 0.5) continue;
        }
        for (int r = 0; r < 3; ++r) U[have * 3 + r] = col[r] / n;
        ++have;
    }

    // Stored U holds orthonormal columns: component r of column k is U[k*3+r].
    // H = U S V^T and the cross-covariance objective trace(R H) is maximized
    // by R = V D U^T, with D = diag(1, 1, det(V U^T)) forcing determinant +1.
    double Umat[9], V[9], VUt[9], Dm[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) {
            Umat[r * 3 + k] = U[k * 3 + r];
            V[r * 3 + k] = vecs[r * 3 + k];
        }
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double dot = 0.0;
            for (int k = 0; k < 3; ++k)
                dot += V[i * 3 + k] * Umat[j * 3 + k];
            VUt[i * 3 + j] = dot;
        }
    const double d = det3(VUt) < 0.0 ? -1.0 : 1.0;
    Dm[8] = d;
    double VD[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            VD[i * 3 + j] = V[i * 3 + j] * Dm[j * 3 + j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double dot = 0.0;
            for (int k = 0; k < 3; ++k)
                dot += VD[i * 3 + k] * Umat[j * 3 + k];
            R[i * 3 + j] = dot;
        }
    for (int i = 0; i < 9; ++i)
        if (!std::isfinite(R[i])) return false;
    return true;
}

// Weighted proper rigid fit: minimizes sum_i c_i ||b_i - (R a_i + t)||^2.
// total_weight must be strictly positive.
bool rigid_fit(const std::vector<double>& a, const std::vector<double>& b,
               const std::vector<double>& c, const std::vector<std::size_t>& members,
               RigidTransform& tf) {
    double wsum = 0.0, abar[3] = {0, 0, 0}, bbar[3] = {0, 0, 0};
    for (std::size_t idx : members) {
        const double w = c[idx];
        if (w <= 0.0) continue;
        wsum += w;
        for (int r = 0; r < 3; ++r) {
            abar[r] += w * a[idx * 3 + r];
            bbar[r] += w * b[idx * 3 + r];
        }
    }
    if (!(wsum > 0.0) || !std::isfinite(wsum)) return false;
    for (int r = 0; r < 3; ++r) {
        abar[r] /= wsum;
        bbar[r] /= wsum;
    }
    double H[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (std::size_t idx : members) {
        const double w = c[idx];
        if (w <= 0.0) continue;
        double da[3], db[3];
        for (int r = 0; r < 3; ++r) {
            da[r] = a[idx * 3 + r] - abar[r];
            db[r] = b[idx * 3 + r] - bbar[r];
        }
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                H[i * 3 + j] += w * da[i] * db[j];
    }
    double R[9];
    if (!proper_rotation(H, R)) return false;
    for (int i = 0; i < 9; ++i) tf.rotation[i] = R[i];
    for (int r = 0; r < 3; ++r) {
        tf.translation[r] = bbar[r] - (R[r * 3 + 0] * abar[0] +
                                       R[r * 3 + 1] * abar[1] +
                                       R[r * 3 + 2] * abar[2]);
        if (!std::isfinite(tf.translation[r])) return false;
    }
    return true;
}

// Enumerate every non-empty bone subset of size at most P in a deterministic
// lexicographic order. For each subset solve the equality-only simplex QP and
// keep the feasible point with the smallest quadratic objective.
//
// Q is K x K (Gram), f is K (linear cross term); the minimized scalar is
// w^T Q w - 2 f^T w subject to sum_k w_k = 1, w >= 0, |support| <= P.
bool solve_sparse_simplex(std::size_t K, std::size_t P,
                          const std::vector<double>& Q,
                          const std::vector<double>& f,
                          std::vector<double>& weights) {
    weights.assign(K, 0.0);
    bool have_best = false;
    double best_obj = 0.0;
    std::vector<double> best_w(K, 0.0);

    for (std::size_t size = 1; size <= P; ++size) {
        // First combination in lexicographic order: 0,1,...,size-1.
        std::vector<int> comb(size);
        std::iota(comb.begin(), comb.end(), 0);
        while (true) {
            const int m = static_cast<int>(size);
            std::vector<double> A(static_cast<std::size_t>(m + 1) * (m + 1), 0.0);
            std::vector<double> rhs(m + 1, 0.0);
            for (int i = 0; i < m; ++i) {
                for (int j = 0; j < m; ++j)
                    A[i * (m + 1) + j] = Q[comb[i] * K + comb[j]];
                // Stationarity row: Q w - lambda = f.
                A[i * (m + 1) + m] = -1.0;
                rhs[i] = f[comb[i]];
            }
            // Equality row: sum of the active weights is 1, written as
            // -e^T w = -1 so the KKT matrix stays symmetric.
            for (int i = 0; i < m; ++i) A[m * (m + 1) + i] = -1.0;
            rhs[m] = -1.0;
            std::vector<double> sol;
            symmetric_solve(m + 1, A, rhs, sol);

            bool feasible = true;
            double sum = 0.0;
            for (int i = 0; i < m; ++i) {
                if (!std::isfinite(sol[i]) || sol[i] < -1e-9) {
                    feasible = false;
                    break;
                }
                sum += std::max(0.0, sol[i]);
            }
            if (feasible && sum > 0.0) {
                std::vector<double> w(K, 0.0);
                for (int i = 0; i < m; ++i)
                    w[comb[i]] = std::max(0.0, sol[i]) / sum;
                double obj = 0.0;
                for (std::size_t i = 0; i < K; ++i) {
                    double row = 0.0;
                    for (std::size_t j = 0; j < K; ++j) row += Q[i * K + j] * w[j];
                    obj += w[i] * (row - 2.0 * f[i]);
                }
                if (!std::isfinite(obj)) {
                    return false;
                }
                // Strict improvement only: ties keep the earlier subset,
                // which is the smaller / lexicographically first one.
                if (!have_best || obj < best_obj - 1e-12 * std::max(1.0, std::fabs(best_obj))) {
                    have_best = true;
                    best_obj = obj;
                    best_w = w;
                }
            }

            // Next combination.
            int i = m - 1;
            while (i >= 0 && comb[i] == static_cast<int>(K) - m + i) --i;
            if (i < 0) break;
            ++comb[i];
            for (int j = i + 1; j < m; ++j) comb[j] = comb[j - 1] + 1;
        }
    }
    if (!have_best) return false;
    weights = best_w;
    return true;
}

} // namespace

std::string validate_problem(const SkinProblem& problem) {
    if (problem.frame_count == 0) return "frame count must be at least 1";
    if (problem.vertex_count == 0) return "vertex count must be at least 1";
    if (problem.bone_count < 1 || problem.bone_count > 8)
        return "bone count K must be in [1, 8]";
    if (problem.max_influences < 1 ||
        problem.max_influences > std::min<std::size_t>(4, problem.bone_count))
        return "max influences P must be in [1, min(4, K)]";
    if (!std::isfinite(problem.max_error) || problem.max_error < 0.0)
        return "max error must be finite and non-negative";
    const std::size_t S = problem.frame_count;
    const std::size_t V = problem.vertex_count;
    const std::size_t K = problem.bone_count;
    if (problem.rest_positions.size() != V * 3)
        return "rest positions must hold V * 3 doubles";
    if (problem.observed_positions.size() != S * V * 3)
        return "observed positions must hold S * V * 3 doubles";
    if (problem.labels.size() != V)
        return "labels must hold one entry per vertex";
    for (double v : problem.rest_positions)
        if (!std::isfinite(v)) return "rest positions must all be finite";
    for (double v : problem.observed_positions)
        if (!std::isfinite(v)) return "observed positions must all be finite";
    std::vector<int> seen(K, 0);
    for (int label : problem.labels) {
        if (label < 0 || static_cast<std::size_t>(label) >= K)
            return "every label must be in [0, K)";
        ++seen[label];
    }
    for (std::size_t k = 0; k < K; ++k)
        if (seen[k] == 0) return "every bone label must occur at least once";
    return "";
}

namespace {

double compute_energy(const SkinProblem& problem,
                      const std::vector<double>& weights,
                      const std::vector<RigidTransform>& transforms,
                      std::size_t& non_finite) {
    const std::size_t S = problem.frame_count;
    const std::size_t V = problem.vertex_count;
    const std::size_t K = problem.bone_count;
    non_finite = 0;
    double energy = 0.0;
    for (std::size_t s = 0; s < S; ++s) {
        for (std::size_t v = 0; v < V; ++v) {
            double residual[3] = {0, 0, 0};
            const double* x = &problem.observed_positions[(s * V + v) * 3];
            for (std::size_t k = 0; k < K; ++k) {
                const double w = weights[v * K + k];
                if (w <= 0.0) continue;
                const RigidTransform& tf = transforms[s * K + k];
                const double* p = &problem.rest_positions[v * 3];
                for (int r = 0; r < 3; ++r) {
                    const double y = tf.rotation[r * 3 + 0] * p[0] +
                                     tf.rotation[r * 3 + 1] * p[1] +
                                     tf.rotation[r * 3 + 2] * p[2] +
                                     tf.translation[r];
                    residual[r] += w * y;
                }
            }
            for (int r = 0; r < 3; ++r) {
                residual[r] = x[r] - residual[r];
                if (!std::isfinite(residual[r])) ++non_finite;
                energy += residual[r] * residual[r];
            }
        }
    }
    if (!std::isfinite(energy)) non_finite = std::max<std::size_t>(non_finite, 1);
    return energy;
}

} // namespace

SkinSolution solve(const SkinProblem& problem) {
    SkinSolution result;
    const std::string reason = validate_problem(problem);
    if (!reason.empty()) {
        result.ok = false;
        result.error = reason;
        return result;
    }
    const std::size_t S = problem.frame_count;
    const std::size_t V = problem.vertex_count;
    const std::size_t K = problem.bone_count;
    const std::size_t P = problem.max_influences;

    std::vector<double> weights(V * K, 0.0);
    for (std::size_t v = 0; v < V; ++v) weights[v * K + problem.labels[v]] = 1.0;

    std::vector<RigidTransform> transforms(S * K);
    for (auto& tf : transforms) {
        for (int i = 0; i < 9; ++i) tf.rotation[i] = (i % 4 == 0) ? 1.0 : 0.0;
        tf.translation[0] = tf.translation[1] = tf.translation[2] = 0.0;
    }

    auto fail = [&](const std::string& message) -> SkinSolution {
        SkinSolution failed;
        failed.ok = false;
        failed.error = message;
        return failed;
    };

    // Initial per-frame / per-bone rigid fits against the one-hot labels.
    std::vector<std::vector<std::size_t>> members(K);
    for (std::size_t v = 0; v < V; ++v)
        members[problem.labels[v]].push_back(v);
    for (std::size_t s = 0; s < S; ++s) {
        const std::vector<double>& frame_targets = problem.observed_positions;
        for (std::size_t k = 0; k < K; ++k) {
            std::vector<double> targets(members[k].size() * 3);
            for (std::size_t i = 0; i < members[k].size(); ++i) {
                const std::size_t v = members[k][i];
                for (int r = 0; r < 3; ++r)
                    targets[i * 3 + r] = frame_targets[(s * V + v) * 3 + r];
            }
            std::vector<double> rest(members[k].size() * 3);
            std::vector<double> cw(members[k].size(), 1.0);
            for (std::size_t i = 0; i < members[k].size(); ++i)
                for (int r = 0; r < 3; ++r)
                    rest[i * 3 + r] = problem.rest_positions[members[k][i] * 3 + r];
            if (!rigid_fit(rest, targets, cw,
                           [&] {
                               std::vector<std::size_t> idx(members[k].size());
                               std::iota(idx.begin(), idx.end(), 0);
                               return idx;
                           }(),
                           transforms[s * K + k]))
                return fail("non-finite result during initial rigid fit");
        }
    }

    std::size_t non_finite = 0;
    double energy = compute_energy(problem, weights, transforms, non_finite);
    if (non_finite != 0 || !std::isfinite(energy))
        return fail("non-finite result in initial objective");
    result.reports.push_back(EnergyReport{0, energy, non_finite});

    // Precompute per-frame skinned positions y_skv = R_sk p_v + t_sk.
    std::vector<double> skinned(S * K * V * 3);
    auto recompute_skinned = [&]() {
        for (std::size_t s = 0; s < S; ++s)
            for (std::size_t k = 0; k < K; ++k) {
                const RigidTransform& tf = transforms[s * K + k];
                for (std::size_t v = 0; v < V; ++v) {
                    const double* p = &problem.rest_positions[v * 3];
                    double* y = &skinned[((s * K + k) * V + v) * 3];
                    for (int r = 0; r < 3; ++r)
                        y[r] = tf.rotation[r * 3 + 0] * p[0] +
                               tf.rotation[r * 3 + 1] * p[1] +
                               tf.rotation[r * 3 + 2] * p[2] +
                               tf.translation[r];
                }
            }
    };
    recompute_skinned();

    for (std::size_t iteration = 1; iteration <= problem.iterations; ++iteration) {
        // --- Weight block: global sparse simplex optimum per vertex. --------
        for (std::size_t v = 0; v < V; ++v) {
            std::vector<double> Q(K * K, 0.0), f(K, 0.0);
            for (std::size_t ki = 0; ki < K; ++ki) {
                for (std::size_t kj = ki; kj < K; ++kj) {
                    double dot = 0.0;
                    for (std::size_t s = 0; s < S; ++s) {
                        const double* yi = &skinned[((s * K + ki) * V + v) * 3];
                        const double* yj = &skinned[((s * K + kj) * V + v) * 3];
                        dot += yi[0] * yj[0] + yi[1] * yj[1] + yi[2] * yj[2];
                    }
                    Q[ki * K + kj] = dot;
                    Q[kj * K + ki] = dot;
                }
                double dot = 0.0;
                for (std::size_t s = 0; s < S; ++s) {
                    const double* x = &problem.observed_positions[(s * V + v) * 3];
                    const double* y = &skinned[((s * K + ki) * V + v) * 3];
                    dot += x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
                }
                f[ki] = dot;
            }
            std::vector<double> new_weights;
            if (!solve_sparse_simplex(K, P, Q, f, new_weights))
                return fail("non-finite result while solving vertex weights");
            for (std::size_t k = 0; k < K; ++k) weights[v * K + k] = new_weights[k];
        }

        // --- Rigid block: sequential proper rigid updates. ------------------
        for (std::size_t s = 0; s < S; ++s) {
            for (std::size_t k = 0; k < K; ++k) {
                std::vector<std::size_t> support;
                double wsum = 0.0;
                for (std::size_t v = 0; v < V; ++v) {
                    if (weights[v * K + k] > kWeightFloor) {
                        support.push_back(v);
                        wsum += weights[v * K + k];
                    }
                }
                if (support.empty() || !(wsum > 0.0)) {
                    // Zero-support bones keep their previous transform.
                    continue;
                }
                std::vector<double> residual_targets(support.size() * 3);
                std::vector<double> rest(support.size() * 3);
                std::vector<double> cw(support.size());
                for (std::size_t i = 0; i < support.size(); ++i) {
                    const std::size_t v = support[i];
                    const double wv = weights[v * K + k];
                    // ||c - wv * y_k||^2 = wv^2 ||(c / wv) - y_k||^2, so the
                    // proper rigid fit uses coefficient wv^2 and target c/wv.
                    cw[i] = wv * wv;
                    for (int r = 0; r < 3; ++r)
                        rest[i * 3 + r] = problem.rest_positions[v * 3 + r];
                    for (int r = 0; r < 3; ++r) {
                        double other = 0.0;
                        for (std::size_t j = 0; j < K; ++j) {
                            if (j == k) continue;
                            const double w = weights[v * K + j];
                            if (w <= 0.0) continue;
                            other += w * skinned[((s * K + j) * V + v) * 3 + r];
                        }
                        residual_targets[i * 3 + r] =
                            (problem.observed_positions[(s * V + v) * 3 + r] -
                             other) /
                            wv;
                    }
                }
                std::vector<std::size_t> idx(support.size());
                std::iota(idx.begin(), idx.end(), 0);
                RigidTransform updated = transforms[s * K + k];
                if (!rigid_fit(rest, residual_targets, cw, idx, updated))
                    return fail("non-finite result while updating a rigid transform");
                transforms[s * K + k] = updated;
                // Refresh this bone's skinned column for later bones/frames.
                for (std::size_t v = 0; v < V; ++v) {
                    const double* p = &problem.rest_positions[v * 3];
                    double* y = &skinned[((s * K + k) * V + v) * 3];
                    for (int r = 0; r < 3; ++r)
                        y[r] = updated.rotation[r * 3 + 0] * p[0] +
                               updated.rotation[r * 3 + 1] * p[1] +
                               updated.rotation[r * 3 + 2] * p[2] +
                               updated.translation[r];
                }
            }
        }

        const double previous = energy;
        energy = compute_energy(problem, weights, transforms, non_finite);
        if (non_finite != 0 || !std::isfinite(energy))
            return fail("non-finite result in iteration objective");
        if (energy > previous + 1e-8 * std::max(1.0, previous))
            return fail("objective rose beyond the allowed numerical tolerance");
        result.reports.push_back(EnergyReport{iteration, energy, non_finite});
    }

    // Final sanity: simplex and proper-rotation constraints.
    for (std::size_t v = 0; v < V; ++v) {
        double sum = 0.0;
        std::size_t positives = 0;
        for (std::size_t k = 0; k < K; ++k) {
            const double w = weights[v * K + k];
            if (!std::isfinite(w) || w < 0.0)
                return fail("solver produced an invalid weight");
            sum += w;
            if (w > kWeightFloor) ++positives;
        }
        if (std::fabs(sum - 1.0) > 1e-8 || positives > P)
            return fail("solver violated the sparse simplex constraints");
    }
    for (const RigidTransform& tf : transforms) {
        if (!finite3(tf.translation)) return fail("solver produced a non-finite translation");
        double RtR[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
        const double* R = tf.rotation;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int r = 0; r < 3; ++r)
                    RtR[i * 3 + j] += R[r * 3 + i] * R[r * 3 + j];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                const double want = (i == j ? 1.0 : 0.0);
                if (std::fabs(RtR[i * 3 + j] - want) > 1e-8)
                    return fail("solver produced a non-orthogonal rotation");
            }
        if (det3(const_cast<double*>(R)) <= 0.0)
            return fail("solver produced a reflection instead of a rotation");
    }

    result.ok = true;
    result.error.clear();
    result.weights = weights;
    result.transforms = transforms;
    result.final_energy = energy;
    result.frames = S;
    result.vertices = V;
    result.bones = K;
    result.influences = P;
    return result;
}

double evaluate_energy(const SkinProblem& problem, const SkinSolution& solution) {
    if (!solution.ok) return std::numeric_limits<double>::quiet_NaN();
    std::size_t non_finite = 0;
    const double energy =
        compute_energy(problem, solution.weights, solution.transforms, non_finite);
    return non_finite == 0 ? energy : std::numeric_limits<double>::quiet_NaN();
}

std::size_t positive_influence_count(const SkinSolution& solution, std::size_t vertex) {
    std::size_t count = 0;
    for (std::size_t k = 0; k < solution.bones; ++k)
        if (solution.weights[vertex * solution.bones + k] > kWeightFloor) ++count;
    return count;
}

} // namespace skin_decompose
