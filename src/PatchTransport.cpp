#include "PatchTransport.h"

#include <cmath>
#include <stdexcept>

namespace {

// Invert an n x n row-major matrix via LU decomposition with partial
// pivoting. Returns false on a singular matrix.
bool invert_matrix(std::vector<double> a, int n, std::vector<double>& inv) {
    std::vector<int> piv(n);
    for (int k = 0; k < n; ++k) {
        int p = k;
        double best = std::fabs(a[k * n + k]);
        for (int i = k + 1; i < n; ++i) {
            double v = std::fabs(a[i * n + k]);
            if (v > best) { best = v; p = i; }
        }
        if (best == 0.0) return false;
        piv[k] = p;
        if (p != k) {
            for (int j = 0; j < n; ++j) std::swap(a[p * n + j], a[k * n + j]);
        }
        for (int i = k + 1; i < n; ++i) {
            a[i * n + k] /= a[k * n + k];
            const double lik = a[i * n + k];
            for (int j = k + 1; j < n; ++j) a[i * n + j] -= lik * a[k * n + j];
        }
    }
    inv.assign(static_cast<size_t>(n) * n, 0.0);
    std::vector<double> b(n);
    for (int col = 0; col < n; ++col) {
        std::fill(b.begin(), b.end(), 0.0);
        b[col] = 1.0;
        for (int k = 0; k < n; ++k)
            if (piv[k] != k) std::swap(b[k], b[piv[k]]);
        for (int i = 1; i < n; ++i)
            for (int j = 0; j < i; ++j) b[i] -= a[i * n + j] * b[j];
        for (int i = n - 1; i >= 0; --i) {
            for (int j = i + 1; j < n; ++j) b[i] -= a[i * n + j] * b[j];
            b[i] /= a[i * n + i];
        }
        for (int i = 0; i < n; ++i) inv[i * n + col] = b[i];
    }
    return true;
}

// Solve H_PP z = c_P for symmetric positive definite H (m x m row-major)
// using a Cholesky factorization of the principal submatrix indexed by P.
// Writes the full m-vector z (zero outside P).
void solve_subproblem(const std::vector<double>& H, int m,
                      const std::vector<int>& P,
                      const std::vector<double>& c,
                      std::vector<double>& z) {
    const int p = static_cast<int>(P.size());
    std::vector<double> L(static_cast<size_t>(p) * p, 0.0);
    for (int i = 0; i < p; ++i) {
        for (int j = 0; j <= i; ++j) {
            double s = H[P[i] * m + P[j]];
            for (int k = 0; k < j; ++k) s -= L[i * p + k] * L[j * p + k];
            if (i == j) {
                // H is positive definite, so s > 0; the floor only guards
                // against roundoff on exactly-zero pivots.
                L[i * p + i] = std::sqrt(std::max(s, 1e-30));
            } else {
                L[i * p + j] = s / L[j * p + j];
            }
        }
    }
    std::vector<double> y(p);
    for (int i = 0; i < p; ++i) {
        double s = c[P[i]];
        for (int k = 0; k < i; ++k) s -= L[i * p + k] * y[k];
        y[i] = s / L[i * p + i];
    }
    z.assign(m, 0.0);
    for (int i = p - 1; i >= 0; --i) {
        double s = y[i];
        for (int k = i + 1; k < p; ++k) s -= L[k * p + i] * z[P[k]];
        z[P[i]] = s / L[i * p + i];
    }
}

// Active-set solver for min 0.5 x' H x - c' x subject to x >= 0, with H
// symmetric positive definite (Lawson-Hanson style: exact subproblem solves
// on the passive set, feasibility-restoring line searches). Each subproblem
// solve consumes one unit of `budget`; returns false when the budget runs
// out before the projected-gradient tolerance is met.
bool solve_nnqp(const std::vector<double>& H, const std::vector<double>& c, int m,
                double tol, long& budget,
                std::vector<double>& x, double& projected_norm) {
    std::vector<char> passive(m, 0);
    x.assign(m, 0.0);
    std::vector<double> g(m);

    auto gradient = [&]() {
        for (int i = 0; i < m; ++i) {
            double s = -c[i];
            for (int j = 0; j < m; ++j) s += H[i * m + j] * x[j];
            g[i] = s;
        }
    };
    auto projected_gradient_norm = [&]() {
        double r = 0.0;
        for (int k = 0; k < m; ++k) {
            double d = x[k] - std::max(0.0, x[k] - g[k]);
            r = std::max(r, std::fabs(d));
        }
        return r;
    };

    gradient();
    double g0 = 0.0;
    for (int k = 0; k < m; ++k) g0 = std::max(g0, std::fabs(g[k]));
    // Entering threshold: far below the 1e-8 convergence tolerance but
    // safely above subproblem-solve roundoff, so exactly-zero KKT
    // multipliers (optima resting on the bound) do not re-enter forever.
    const double enter_threshold = -1e-11 * std::max(1.0, g0);

    while (true) {
        const double pg = projected_gradient_norm();
        if (pg <= tol) {
            projected_norm = pg;
            return true;
        }
        int enter = -1;
        double best = enter_threshold;
        for (int k = 0; k < m; ++k) {
            if (!passive[k] && g[k] < best) { best = g[k]; enter = k; }
        }
        if (enter < 0) {
            // No descent direction among bounded variables but the
            // tolerance is unmet; cannot happen for a strictly convex
            // problem after an exact subproblem solve.
            projected_norm = pg;
            return false;
        }
        passive[enter] = 1;

        while (true) {
            std::vector<int> P;
            for (int k = 0; k < m; ++k)
                if (passive[k]) P.push_back(k);
            if (P.empty()) {
                x.assign(m, 0.0);
                break;
            }
            if (--budget < 0) return false;
            std::vector<double> z;
            solve_subproblem(H, m, P, c, z);
            bool all_positive = true;
            for (int k : P) {
                if (z[k] <= 0.0) { all_positive = false; break; }
            }
            if (all_positive) {
                x = z;
                break;
            }
            // Feasibility-restoring step: move as far toward z as the
            // bounds allow, then fix the blocking variables at zero.
            double alpha = 1.0;
            for (int k : P) {
                if (z[k] <= 0.0 && x[k] > 0.0)
                    alpha = std::min(alpha, x[k] / (x[k] - z[k]));
            }
            for (int k : P) x[k] += alpha * (z[k] - x[k]);
            for (int k : P) {
                if (x[k] <= 0.0) { x[k] = 0.0; passive[k] = 0; }
            }
        }
        gradient();
    }
}

} // namespace

PatchTransport::PatchTransport(std::vector<shared_ptr<PatchMesh>> meshes,
                               const Object& world, double eps) {
    if (!std::isfinite(eps) || eps <= 0.0)
        throw std::invalid_argument("PatchTransport: eps must be finite and positive");
    if (meshes.empty())
        throw std::invalid_argument("PatchTransport: no meshes");
    for (const auto& m : meshes) {
        if (!m) throw std::invalid_argument("PatchTransport: null mesh");
    }
    for (size_t a = 0; a < meshes.size(); ++a)
        for (size_t b = a + 1; b < meshes.size(); ++b)
            if (meshes[a] == meshes[b])
                throw std::invalid_argument("PatchTransport: duplicate mesh");

    meshes_ = std::move(meshes);
    eps_ = eps;

    // Global face numbering: mesh input order, then source face number.
    face_offsets_.reserve(meshes_.size());
    face_count_ = 0;
    for (const auto& m : meshes_) {
        face_offsets_.push_back(face_count_);
        face_count_ += m->face_count();
    }
    if (face_count_ > kMaxFaces)
        throw std::invalid_argument("PatchTransport: more than 128 faces");

    // Owning geometry / reflectance snapshot.
    verts_.reserve(face_count_);
    areas_.reserve(face_count_);
    normals_.reserve(face_count_);
    rho_.reserve(face_count_);
    for (const auto& m : meshes_) {
        for (int f = 0; f < m->face_count(); ++f) {
            std::array<Point3, 3> tri{{m->vertex(f, 0), m->vertex(f, 1), m->vertex(f, 2)}};
            verts_.push_back(tri);
            areas_.push_back(m->face_area(f));
            normals_.push_back(m->face_normal(f));

            const Material* mat = m->face_material(f);
            if (mat->has_texture || mat->is_reflective || mat->is_refractive)
                throw std::invalid_argument(
                    "PatchTransport: only flat diffuse materials are supported");
            Radiance rho = mat->kd * mat->diffusecolor;
            for (int c = 0; c < 3; ++c) {
                if (!std::isfinite(rho[c]) || rho[c] < 0.0 || rho[c] > 0.95)
                    throw std::invalid_argument(
                        "PatchTransport: rho = kd * diffusecolor outside [0, 0.95]");
            }
            rho_.push_back(rho);
        }
    }

    const int n = face_count_;

    // Three equal-weight area sample points per face: the cyclic
    // permutations of barycentric (2/3, 1/6, 1/6).
    std::vector<std::array<Point3, 3>> samples(n);
    for (int i = 0; i < n; ++i) {
        const Point3& v0 = verts_[i][0];
        const Point3& v1 = verts_[i][1];
        const Point3& v2 = verts_[i][2];
        samples[i][0] = (2.0 / 3.0) * v0 + (1.0 / 6.0) * v1 + (1.0 / 6.0) * v2;
        samples[i][1] = (1.0 / 6.0) * v0 + (2.0 / 3.0) * v1 + (1.0 / 6.0) * v2;
        samples[i][2] = (1.0 / 6.0) * v0 + (1.0 / 6.0) * v1 + (2.0 / 3.0) * v2;
    }

    // Unscaled transport matrix H, symmetric by construction.
    std::vector<double> H(static_cast<size_t>(n) * n, 0.0);
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            double sum = 0.0;
            for (int a = 0; a < 3; ++a) {
                for (int b = 0; b < 3; ++b) {
                    const Vector3 seg = samples[j][b] - samples[i][a];
                    const double dist = seg.length();
                    if (dist <= 100.0 * eps_) continue; // too close: zero term
                    const Vector3 d = seg / dist;
                    const double ci = dot(normals_[i], d);
                    const double cj = dot(normals_[j], -d);
                    if (ci <= 0.0 || cj <= 0.0) continue; // back faces don't transmit
                    // Visibility on the open interval (eps, dist - eps).
                    Ray ray(samples[i][a], d);
                    IntersectionRecord rec;
                    if (world.intersect(ray, Interval(eps_, dist - eps_), rec))
                        continue; // occluded
                    sum += ci * cj / (dist * dist);
                }
            }
            const double hij = areas_[i] * areas_[j] / (9.0 * pi) * sum;
            H[i * n + j] = hij;
            H[j * n + i] = hij;
        }
    }

    // Single global conservation scaling; never a per-row normalization.
    double s = 0.0;
    for (int i = 0; i < n; ++i) {
        double row = 0.0;
        for (int j = 0; j < n; ++j) row += H[i * n + j] / areas_[i];
        s = std::max(s, row);
    }
    const double scale = std::max(1.0, s);

    F_.assign(static_cast<size_t>(n) * n, 0.0);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            F_[i * n + j] = H[i * n + j] / scale / areas_[i];

    // Per-channel response: B = (I - diag(rho_c) F)^-1 E. The matrix is
    // strictly diagonally dominant (row sums of F are <= 1, rho <= 0.95),
    // hence invertible.
    for (int c = 0; c < 3; ++c) {
        std::vector<double> M(static_cast<size_t>(n) * n, 0.0);
        for (int i = 0; i < n; ++i) {
            M[i * n + i] = 1.0;
            for (int j = 0; j < n; ++j)
                M[i * n + j] -= rho_[i][c] * F_[i * n + j];
        }
        if (!invert_matrix(std::move(M), n, resp_[c]))
            throw std::runtime_error("PatchTransport: singular transport matrix");
    }
}

double PatchTransport::area(int face) const {
    return areas_.at(face);
}

double PatchTransport::form_factor(int i, int j) const {
    return F_.at(static_cast<size_t>(i) * face_count_ + j);
}

Radiance PatchTransport::reflectance(int face) const {
    return rho_.at(face);
}

const PatchMesh* PatchTransport::face_mesh(int face) const {
    if (face < 0 || face >= face_count_)
        throw std::out_of_range("PatchTransport::face_mesh");
    int k = static_cast<int>(meshes_.size()) - 1;
    while (k > 0 && face_offsets_[k] > face) --k;
    return meshes_[k].get();
}

int PatchTransport::face_source_index(int face) const {
    if (face < 0 || face >= face_count_)
        throw std::out_of_range("PatchTransport::face_source_index");
    int k = static_cast<int>(meshes_.size()) - 1;
    while (k > 0 && face_offsets_[k] > face) --k;
    return face - face_offsets_[k];
}

bool EmissionFit::lookup(const PatchMesh* mesh, int source_face,
                         Radiance& emission, Radiance& exitance) const {
    for (size_t k = 0; k < meshes_.size(); ++k) {
        if (meshes_[k].get() == mesh) {
            if (source_face < 0 || source_face >= meshes_[k]->face_count())
                return false;
            const int global = face_offsets_[k] + source_face;
            emission = emission_[global];
            exitance = exitance_[global];
            return true;
        }
    }
    return false;
}

std::optional<EmissionFit> PatchTransport::fit(
        const std::vector<int>& candidates,
        const std::vector<FitObservation>& observations,
        double lambda, int max_iterations) const {
    const int n = face_count_;

    // Input validation; any violation fails without touching prior results.
    if (candidates.empty() || candidates.size() > 16) return std::nullopt;
    if (observations.empty() || observations.size() > 256) return std::nullopt;
    if (!std::isfinite(lambda) || lambda <= 0.0) return std::nullopt;
    if (max_iterations < 1) return std::nullopt;
    {
        std::vector<char> seen(n, 0);
        for (int f : candidates) {
            if (f < 0 || f >= n || seen[f]) return std::nullopt;
            seen[f] = 1;
        }
    }
    for (const FitObservation& o : observations) {
        if (o.face < 0 || o.face >= n) return std::nullopt;
        if (!std::isfinite(o.weight) || o.weight <= 0.0) return std::nullopt;
        for (int c = 0; c < 3; ++c)
            if (!std::isfinite(o.target[c])) return std::nullopt;
    }

    const int m = static_cast<int>(candidates.size());

    EmissionFit result;
    result.meshes_ = meshes_;
    result.face_offsets_ = face_offsets_;
    result.emission_.assign(n, Radiance(0, 0, 0));
    result.exitance_.assign(n, Radiance(0, 0, 0));

    long budget = max_iterations;
    double total_objective = 0.0;
    double worst_norm = 0.0;

    // The RGB channels decouple into three independent bound-constrained
    // quadratic programs over the candidate emissions.
    for (int c = 0; c < 3; ++c) {
        const std::vector<double>& resp = resp_[c];

        // G[o][k]: response of observed face o to candidate k.
        // H = 2 G' W G + 2 lambda I,  cvec = 2 G' W t.
        std::vector<double> H(static_cast<size_t>(m) * m, 0.0);
        std::vector<double> cvec(m, 0.0);
        for (const FitObservation& o : observations) {
            const double w = o.weight;
            const double t = o.target[c];
            const double* row = &resp[static_cast<size_t>(o.face) * n];
            for (int k = 0; k < m; ++k) {
                const double gk = row[candidates[k]];
                cvec[k] += 2.0 * w * gk * t;
                for (int l = 0; l <= k; ++l)
                    H[k * m + l] += 2.0 * w * gk * row[candidates[l]];
            }
        }
        for (int k = 0; k < m; ++k) {
            for (int l = 0; l < k; ++l) H[l * m + k] = H[k * m + l];
            H[k * m + k] += 2.0 * lambda;
        }

        double g0 = 0.0;
        for (int k = 0; k < m; ++k) g0 = std::max(g0, std::fabs(cvec[k]));
        const double tol = 1e-8 * std::max(1.0, g0);

        std::vector<double> x;
        double pg = 0.0;
        if (!solve_nnqp(H, cvec, m, tol, budget, x, pg))
            return std::nullopt; // budget exhausted

        // Objective for this channel: ||W^1/2 (Gx - t)||^2 + lambda |x|^2.
        double obj = 0.0;
        for (const FitObservation& o : observations) {
            const double* row = &resp[static_cast<size_t>(o.face) * n];
            double b = 0.0;
            for (int k = 0; k < m; ++k) b += row[candidates[k]] * x[k];
            const double r = b - o.target[c];
            obj += o.weight * r * r;
        }
        for (int k = 0; k < m; ++k) obj += lambda * x[k] * x[k];
        total_objective += obj;
        worst_norm = std::max(worst_norm, pg);
        result.channel_norms_[c] = pg;

        // Scatter candidate emissions into E and accumulate B = resp * E.
        for (int k = 0; k < m; ++k)
            result.emission_[candidates[k]][c] = x[k];
        for (int i = 0; i < n; ++i) {
            double b = 0.0;
            for (int k = 0; k < m; ++k)
                b += resp[static_cast<size_t>(i) * n + candidates[k]] * x[k];
            result.exitance_[i][c] = b;
        }
    }

    result.objective_ = total_objective;
    result.projected_gradient_norm_ = worst_norm;
    return result;
}
