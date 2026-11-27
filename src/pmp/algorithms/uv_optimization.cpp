// Copyright 2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#include "pmp/algorithms/uv_optimization.h"
#include "pmp/exceptions.h"

#include <Eigen/Sparse>
#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace pmp {

namespace {

using EVec2 = Eigen::Vector2d;
using EMat2 = Eigen::Matrix2d;
using Trip = Eigen::Triplet<double>;

constexpr double kSvdEps = 1e-300;

// Per-face intrinsic reference data. The local reference frame is built
// from the actual 3D triangle, never from an XY projection.
struct FaceGeom
{
    Face f;
    Vertex v[3];
    // rows of D^{-1}, D = [q1-q0, q2-q0]
    double inv00, inv01, inv10, inv11;
    double area;
};

bool finite_tex(const TexCoord& t)
{
    return std::isfinite(t[0]) && std::isfinite(t[1]);
}

bool finite_point(const Point& p)
{
    return std::isfinite(p[0]) && std::isfinite(p[1]) &&
           std::isfinite(p[2]);
}

// J = U D^{-1} for the given per-face UV coordinates
EMat2 face_jacobian(const FaceGeom& fg, const TexCoord& u0,
                   const TexCoord& u1, const TexCoord& u2)
{
    const double du10 = u1[0] - u0[0];
    const double du11 = u1[1] - u0[1];
    const double du20 = u2[0] - u0[0];
    const double du21 = u2[1] - u0[1];
    EMat2 J;
    // U = [du1 du2] (columns), J = U * D^{-1}
    J(0, 0) = du10 * fg.inv00 + du20 * fg.inv10;
    J(0, 1) = du10 * fg.inv01 + du20 * fg.inv11;
    J(1, 0) = du11 * fg.inv00 + du21 * fg.inv10;
    J(1, 1) = du11 * fg.inv01 + du21 * fg.inv11;
    return J;
}

// Symmetric Dirichlet density f(J) = ||J||_F^2 + ||J^{-1}||_F^2.
double density(const EMat2& J, double& detj)
{
    detj = J.determinant();
    const double invdet = 1.0 / std::max(detj, kSvdEps);
    EMat2 Jin;
    Jin << J(1, 1) * invdet, -J(0, 1) * invdet, -J(1, 0) * invdet,
        J(0, 0) * invdet;
    return J.squaredNorm() + Jin.squaredNorm();
}

// Gradient matrix S of f w.r.t. the entries of J (row-major):
// df = 2 <S, dJ> with S = J - (J^{-1} J^{-T} J^{-1})^T
//      = J - J^{-T} J^{-1} J^{-T}.
EMat2 density_grad(const EMat2& J)
{
    const double detj = J.determinant();
    const double invdet = 1.0 / std::max(detj, kSvdEps);
    EMat2 Jin;
    Jin << J(1, 1) * invdet, -J(0, 1) * invdet, -J(1, 0) * invdet,
        J(0, 0) * invdet;
    const EMat2 K = Jin * Jin.transpose() * Jin;
    return J - K.transpose();
}

// Differential dS = dJ - d(J^{-T} J^{-1} J^{-T}), with
// dJ^{-1} = -J^{-1} dJ J^{-1}.
EMat2 density_grad_diff(const EMat2& J, const EMat2& dJ)
{
    const double detj = J.determinant();
    const double invdet = 1.0 / std::max(detj, kSvdEps);
    EMat2 Jin;
    Jin << J(1, 1) * invdet, -J(0, 1) * invdet, -J(1, 0) * invdet,
        J(0, 0) * invdet;
    const EMat2 dJin = -Jin * dJ * Jin;
    // K = J^{-1} J^{-T} J^{-1}, S = J - K^T
    const EMat2 dK = dJin * Jin.transpose() * Jin +
                     Jin * dJin.transpose() * Jin +
                     Jin * Jin.transpose() * dJin;
    return dJ - dK.transpose();
}

double total_energy(const std::vector<FaceGeom>& faces,
                    const std::vector<TexCoord>& uv, double total_area)
{
    double e = 0.0;
    for (const auto& fg : faces)
    {
        double detj;
        const EMat2 J = face_jacobian(fg, uv[fg.v[0].idx()],
                                     uv[fg.v[1].idx()], uv[fg.v[2].idx()]);
        e += fg.area * density(J, detj);
    }
    return e / total_area;
}

} // anonymous namespace

namespace detail {

double uv_positive_step_limit(const std::array<double, 4>& J,
                              const std::array<double, 4>& dJ)
{
    const double detj = J[0] * J[3] - J[1] * J[2];
    const double detd = dJ[0] * dJ[3] - dJ[1] * dJ[2];
    const double b = J[0] * dJ[3] + dJ[0] * J[3] - J[1] * dJ[2] -
                     dJ[1] * J[2];

    // det(J + t dJ) = detj + b t + detd t^2
    if (detd == 0.0)
    {
        if (b < 0.0)
            return detj / (-b); // first (and only) positive zero
        return std::numeric_limits<double>::infinity();
    }

    const double disc = b * b - 4.0 * detd * detj;
    // A discriminant that is negative only by rounding noise corresponds to
    // a tangential (double) root; treat it as zero.
    const double disc_scale =
        std::max(1.0, std::max(b * b, std::abs(4.0 * detd * detj)));
    if (disc < 0.0 && disc > -8.0 * std::numeric_limits<double>::epsilon() *
                                  disc_scale)
    {
        const double t = -b / (2.0 * detd);
        return (t > 0.0 && std::isfinite(t))
                   ? t
                   : std::numeric_limits<double>::infinity();
    }
    if (disc < 0.0)
        return std::numeric_limits<double>::infinity(); // never reaches zero

    const double sq = std::sqrt(std::max(disc, 0.0));
    // Stable root evaluation (Numerical Recipes): q = -0.5*(b + sgn(b)*sq),
    // roots are q/detd and detj/q.
    const double q = -0.5 * (b + (b >= 0.0 ? sq : -sq));
    double best = std::numeric_limits<double>::infinity();
    if (detd != 0.0 && q != 0.0)
    {
        const double r_a = q / detd;
        const double r_b = detj / q;
        if (r_a > 0.0 && std::isfinite(r_a))
            best = r_a;
        if (r_b > 0.0 && std::isfinite(r_b))
            best = std::min(best, r_b);
    }
    return best;
}

} // namespace detail

UvOptimizationResult optimize_uv(SurfaceMesh& mesh, unsigned int max_iterations,
                                 double tolerance)
{
    // ------------------------------------------------------------------
    // Input validation (nothing on the mesh is modified before this point)
    // ------------------------------------------------------------------
    auto tex = mesh.get_vertex_property<TexCoord>("v:tex");
    if (!tex)
        throw InvalidInputException("optimize_uv: vertex property \"v:tex\" "
                                    "must already exist");

    if (max_iterations == 0)
        throw InvalidInputException(
            "optimize_uv: max_iterations must be positive");
    if (!std::isfinite(tolerance) || tolerance <= 0.0)
        throw InvalidInputException(
            "optimize_uv: tolerance must be finite and positive");

    auto points = mesh.get_vertex_property<Point>("v:point");
    if (!points)
        throw InvalidInputException("optimize_uv: mesh has no positions");

    for (auto v : mesh.vertices())
    {
        if (!finite_point(points[v]))
            throw InvalidInputException(
                "optimize_uv: non-finite vertex position");
        if (!finite_tex(tex[v]))
            throw InvalidInputException(
                "optimize_uv: missing or non-finite UV coordinate");
    }

    for (auto f : mesh.faces())
    {
        unsigned int n = 0;
        for (auto v : mesh.vertices(f))
        {
            (void)v;
            ++n;
        }
        if (n != 3)
            throw InvalidInputException(
                "optimize_uv: only triangle meshes are supported");
    }

    const bool has_fixed_prop =
        mesh.has_vertex_property("v:uv_fixed");
    VertexProperty<bool> uv_fixed;
    if (has_fixed_prop)
        uv_fixed = mesh.get_vertex_property<bool>("v:uv_fixed");
    // The property is only honored when it actually stores bool values;
    // it is never (re)created by this function.
    const bool use_fixed_prop = static_cast<bool>(uv_fixed);

    const auto is_fixed = [&](Vertex v) {
        return mesh.is_boundary(v) ||
               (use_fixed_prop && uv_fixed[v]);
    };

    // ------------------------------------------------------------------
    // Intrinsic per-face reference frames
    // ------------------------------------------------------------------
    std::vector<FaceGeom> face_geom;
    face_geom.reserve(mesh.n_faces());
    double total_area = 0.0;

    for (auto f : mesh.faces())
    {
        auto it = mesh.vertices(f);
        const Vertex v0 = *it;
        ++it;
        const Vertex v1 = *it;
        ++it;
        const Vertex v2 = *it;

        const Point p0 = points[v0];
        const Point p1 = points[v1];
        const Point p2 = points[v2];
        const Point e1v = p1 - p0;
        const double l01 = norm(e1v);
        if (!std::isfinite(l01) || l01 <= 0.0)
            throw InvalidInputException(
                "optimize_uv: triangle has zero or non-finite edge length");

        const Point e1 = e1v / l01;
        const Point e2raw = p2 - p0;
        const double q2x = dot(e2raw, e1);
        Point perp = e2raw - q2x * e1;
        const double q2y = norm(perp);
        if (!std::isfinite(q2x) || !std::isfinite(q2y) || q2y <= 0.0)
            throw InvalidInputException(
                "optimize_uv: triangle has zero or non-finite area");

        const double detD = l01 * q2y; // = 2 * 3D triangle area
        const double area = 0.5 * detD;
        total_area += area;

        FaceGeom fg;
        fg.f = f;
        fg.v[0] = v0;
        fg.v[1] = v1;
        fg.v[2] = v2;
        fg.area = area;
        // D^{-1} for D = [[l01, q2x],[0, q2y]]
        fg.inv00 = q2y / detD;
        fg.inv01 = -q2x / detD;
        fg.inv10 = 0.0;
        fg.inv11 = l01 / detD;
        face_geom.push_back(fg);
    }

    if (!(total_area > 0.0) || !std::isfinite(total_area))
        throw InvalidInputException(
            "optimize_uv: mesh contains no face with positive area");

    // Initial orientation check: every face must start with det(J) > 0.
    std::vector<TexCoord> uv(mesh.vertices_size());
    for (auto v : mesh.vertices())
        uv[v.idx()] = tex[v];

    for (const auto& fg : face_geom)
    {
        const EMat2 J = face_jacobian(fg, uv[fg.v[0].idx()],
                                     uv[fg.v[1].idx()], uv[fg.v[2].idx()]);
        if (!J.allFinite() || J.determinant() <= 0.0)
            throw InvalidInputException(
                "optimize_uv: initial UV chart has a flipped or degenerate "
                "triangle");
    }

    // ------------------------------------------------------------------
    // Free-variable indexing and per-component pinning requirements
    // ------------------------------------------------------------------
    std::vector<int> free_index(mesh.vertices_size(), -1);
    std::vector<char> fixed(mesh.vertices_size(), char{0});
    int n_free = 0;
    for (auto v : mesh.vertices())
    {
        if (mesh.is_isolated(v))
        {
            fixed[v.idx()] = 1; // isolated vertices are never touched
            continue;
        }
        if (is_fixed(v))
            fixed[v.idx()] = 1;
        else
            free_index[v.idx()] = n_free++;
    }

    // Component labeling over active vertices (BFS across one-rings). The
    // label value is irrelevant; it only groups vertices so that each
    // component can be checked for its required fixed pins.
    std::vector<int> component(mesh.vertices_size(), -1);
    int component_id = 0;
    for (auto seed : mesh.vertices())
    {
        if (mesh.is_isolated(seed) || component[seed.idx()] >= 0)
            continue;
        std::vector<Vertex> stack{seed};
        component[seed.idx()] = component_id;
        std::vector<Vertex> comp_verts;
        comp_verts.push_back(seed);
        while (!stack.empty())
        {
            const Vertex v = stack.back();
            stack.pop_back();
            for (auto w : mesh.vertices(v))
            {
                if (mesh.is_deleted(w) || component[w.idx()] >= 0)
                    continue;
                component[w.idx()] = component_id;
                comp_verts.push_back(w);
                stack.push_back(w);
            }
        }
        // Every face-carrying component needs >= 2 fixed vertices with
        // distinct UV positions.
        std::vector<Vertex> pins;
        for (auto v : comp_verts)
            if (fixed[v.idx()])
                pins.push_back(v);
        bool distinct = false;
        for (size_t i = 0; i < pins.size() && !distinct; ++i)
            for (size_t j = i + 1; j < pins.size(); ++j)
                if (norm(uv[pins[i].idx()] - uv[pins[j].idx()]) > 0.0)
                {
                    distinct = true;
                    break;
                }
        if (!distinct)
            throw InvalidInputException(
                "optimize_uv: each component requires at least two fixed "
                "vertices with distinct UV positions");
        ++component_id;
    }

    // Snapshot so a SolverException restores every UV coordinate.
    const std::vector<TexCoord> uv_snapshot = uv;

    // ------------------------------------------------------------------
    // Energy, gradient and Hessian assembly on the free variables
    // ------------------------------------------------------------------
    auto compute_energy = [&]() -> double {
        return total_energy(face_geom, uv, total_area);
    };

    // Linear map P from the six local UV coordinates (u0x,u0y,u1x,u1y,
    // u2x,u2y) to the row-major entries of dJ. Constant per face.
    auto local_map = [](const FaceGeom& fg) {
        Eigen::Matrix<double, 4, 6> P;
        P.setZero();
        // dJ(r,c) = inv(c,0)*(du1_r-du0_r) + inv(c,1)*(du2_r-du0_r)
        // row J00, x components
        P(0, 0) = -(fg.inv00 + fg.inv10);
        P(0, 2) = fg.inv00;
        P(0, 4) = fg.inv10;
        // row J01, x components
        P(1, 0) = -(fg.inv01 + fg.inv11);
        P(1, 2) = fg.inv01;
        P(1, 4) = fg.inv11;
        // row J10, y components
        P(2, 1) = -(fg.inv00 + fg.inv10);
        P(2, 3) = fg.inv00;
        P(2, 5) = fg.inv10;
        // row J11, y components
        P(3, 1) = -(fg.inv01 + fg.inv11);
        P(3, 3) = fg.inv01;
        P(3, 5) = fg.inv11;
        return P;
    };

    auto compute_gradient = [&](Eigen::VectorXd& g) {
        g.setZero();
        for (const auto& fg : face_geom)
        {
            const EMat2 J =
                face_jacobian(fg, uv[fg.v[0].idx()], uv[fg.v[1].idx()],
                              uv[fg.v[2].idx()]);
            const EMat2 S = density_grad(J); // df = 2 <S, dJ>
            Eigen::Vector4d s;
            s << S(0, 0), S(0, 1), S(1, 0), S(1, 1);
            const Eigen::Matrix<double, 6, 1> gl =
                2.0 * fg.area * local_map(fg).transpose() * s;
            const int loc[6] = {0, 0, 1, 1, 2, 2};
            for (int k = 0; k < 6; ++k)
            {
                const Vertex vv = fg.v[loc[k]];
                const int fi = free_index[vv.idx()];
                if (fi >= 0)
                    g(2 * fi + (k % 2)) += gl(k);
            }
        }
        g /= total_area;
    };

    auto assemble_hessian = [&](Eigen::SparseMatrix<double>& H) {
        std::vector<Trip> trips;
        trips.reserve(face_geom.size() * 36);
        for (const auto& fg : face_geom)
        {
            const EMat2 J =
                face_jacobian(fg, uv[fg.v[0].idx()], uv[fg.v[1].idx()],
                              uv[fg.v[2].idx()]);

            // Exact 4x4 Hessian B of f w.r.t. row-major J entries:
            // d2f(X) = 2 <X, dS(X)>, built as a bilinear form on basis
            // matrices so the local assembly stays analytic.
            Eigen::Matrix4d B;
            for (int i = 0; i < 4; ++i)
            {
                EMat2 Yi;
                Yi.setZero();
                Yi(i / 2, i % 2) = 1.0;
                const EMat2 dSi = density_grad_diff(J, Yi);
                for (int j = 0; j < 4; ++j)
                    B(i, j) = 2.0 * dSi(j / 2, j % 2);
            }

            const auto P = local_map(fg);
            const Eigen::Matrix<double, 6, 6> Hloc =
                fg.area * P.transpose() * B * P;

            const int loc[6] = {0, 0, 1, 1, 2, 2};
            for (int a = 0; a < 6; ++a)
            {
                const Vertex va = fg.v[loc[a]];
                const int ia = free_index[va.idx()];
                if (ia < 0)
                    continue;
                for (int b = 0; b < 6; ++b)
                {
                    const Vertex vb = fg.v[loc[b]];
                    const int ib = free_index[vb.idx()];
                    if (ib < 0)
                        continue;
                    trips.emplace_back(2 * ia + (a % 2),
                                       2 * ib + (b % 2),
                                       Hloc(a, b) / total_area);
                }
            }
        }
        H.setFromTriplets(trips.begin(), trips.end());
    };

    // Per-face Jacobian derivative along a packed free-variable direction.
    auto face_dj = [&](const FaceGeom& fg, const Eigen::VectorXd& d,
                       EMat2& dJ) {
        const auto duv = [&](int k) -> EVec2 {
            const int fi = free_index[fg.v[k].idx()];
            return fi >= 0 ? EVec2(d(2 * fi), d(2 * fi + 1)) : EVec2(0.0, 0.0);
        };
        const EVec2 d1 = duv(1) - duv(0);
        const EVec2 d2 = duv(2) - duv(0);
        dJ(0, 0) = d1[0] * fg.inv00 + d2[0] * fg.inv10;
        dJ(0, 1) = d1[0] * fg.inv01 + d2[0] * fg.inv11;
        dJ(1, 0) = d1[1] * fg.inv00 + d2[1] * fg.inv10;
        dJ(1, 1) = d1[1] * fg.inv01 + d2[1] * fg.inv11;
    };

    // ------------------------------------------------------------------
    // Optimization loop
    // ------------------------------------------------------------------
    UvOptimizationResult result;
    double energy = compute_energy();
    if (!std::isfinite(energy))
    {
        // Should be unreachable after the validation above.
        throw SolverException(
            "optimize_uv: non-finite objective at the initial state");
    }
    result.energies.push_back(energy);

    if (n_free == 0)
    {
        result.gradient_norm = 0.0;
        result.converged = true;
        return result;
    }

    Eigen::VectorXd grad(n_free * 2);
    compute_gradient(grad);
    if (!grad.allFinite())
    {
        for (auto v : mesh.vertices())
            tex[v] = uv_snapshot[v.idx()];
        throw SolverException("optimize_uv: non-finite initial gradient");
    }
    double grad_norm = grad.lpNorm<Eigen::Infinity>();
    result.gradient_norm = grad_norm;
    if (grad_norm <= tolerance)
    {
        result.converged = true;
        return result;
    }

    Eigen::SparseMatrix<double> H(n_free * 2, n_free * 2);
    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> ldlt;

    const auto restore_and_throw = [&](const char* msg) {
        for (auto v : mesh.vertices())
            tex[v] = uv_snapshot[v.idx()];
        throw SolverException(msg);
    };

    bool progress_blocked = false;

    for (unsigned int iter = 0;
         iter < max_iterations && !result.converged && !progress_blocked;
         ++iter)
    {
        assemble_hessian(H);
        for (int k = 0; k < H.outerSize(); ++k)
            for (Eigen::SparseMatrix<double>::InnerIterator it(H, k); it;
                 ++it)
            {
                if (!std::isfinite(it.value()))
                    restore_and_throw(
                        "optimize_uv: non-finite Hessian entries");
            }

        // Newton direction with increasing regularization, then a
        // steepest-descent fallback. The objective itself never changes.
        Eigen::VectorXd dir(n_free * 2);
        bool solved = false;
        const double h_scale =
            std::max(1.0, H.diagonal().cwiseAbs().maxCoeff());
        for (int attempt = 0; attempt < 10 && !solved; ++attempt)
        {
            const double reg = (attempt == 0)
                                   ? 0.0
                                   : h_scale * std::pow(8.0, attempt - 1) *
                                         1e-12;
            Eigen::SparseMatrix<double> Hr = H;
            if (reg > 0.0)
            {
                for (int k = 0; k < Hr.rows(); ++k)
                    Hr.coeffRef(k, k) += reg;
            }
            ldlt.compute(Hr);
            if (ldlt.info() != Eigen::Success)
                continue;
            const Eigen::VectorXd trial = ldlt.solve(-grad);
            if (!trial.allFinite())
                continue;
            const double slope = grad.dot(trial);
            if (slope < 0.0)
            {
                dir = trial;
                solved = true;
            }
        }
        if (!solved)
        {
            // Guaranteed descent direction (gradient is finite and nonzero).
            dir = -grad;
            if (!dir.allFinite() || grad.dot(dir) >= 0.0)
                restore_and_throw(
                    "optimize_uv: could not construct a descent direction");
        }

        // Quadratic determinant analysis per face: find the largest
        // interval starting at t=0 on which every det(J(t)) stays
        // strictly positive, then keep a 1% safety margin.
        double t_flip = std::numeric_limits<double>::infinity();
        for (const auto& fg : face_geom)
        {
            const EMat2 J =
                face_jacobian(fg, uv[fg.v[0].idx()], uv[fg.v[1].idx()],
                              uv[fg.v[2].idx()]);
            EMat2 dJ;
            face_dj(fg, dir, dJ);
            if (!dJ.allFinite())
                restore_and_throw(
                    "optimize_uv: non-finite search direction");
            const std::array<double, 4> Ja = {J(0, 0), J(0, 1), J(1, 0),
                                              J(1, 1)};
            const std::array<double, 4> dJa = {dJ(0, 0), dJ(0, 1),
                                               dJ(1, 0), dJ(1, 1)};
            const double lim = detail::uv_positive_step_limit(Ja, dJa);
            if (std::isfinite(lim))
                t_flip = std::min(t_flip, lim);
        }
        const double t_max =
            std::isfinite(t_flip) ? 0.99 * t_flip
                                  : std::numeric_limits<double>::infinity();

        // Backtracking line search on the real objective inside the
        // guaranteed-flip-free interval.
        const double slope = grad.dot(dir);
        const double e0 = energy;
        const double armijo = 1e-4;
        double t = std::isfinite(t_max) ? t_max : 1.0;
        bool accepted = false;
        double e_new = e0;

        for (int ls = 0; ls < 80; ++ls)
        {
            if (!std::isfinite(t) || t <= 0.0)
                break;
            std::vector<TexCoord> trial_uv = uv;
            for (auto v : mesh.vertices())
            {
                const int fi = free_index[v.idx()];
                if (fi < 0)
                    continue;
                trial_uv[v.idx()] =
                    uv[v.idx()] +
                    TexCoord(t * dir(2 * fi), t * dir(2 * fi + 1));
            }
            const double et = total_energy(face_geom, trial_uv, total_area);
            // Strict descent plus the Armijo condition: steps that only
            // match the current energy to rounding do not count as
            // progress, otherwise the iteration budget would be spent on
            // stationary moves at the numerical optimum.
            if (std::isfinite(et) && et < e0 &&
                et <= e0 + armijo * t * slope)
            {
                uv.swap(trial_uv);
                e_new = et;
                accepted = true;
                break;
            }
            t *= 0.5;
        }

        if (!accepted)
        {
            progress_blocked = true;
            break;
        }

        energy = e_new;
        result.energies.push_back(energy);
        ++result.accepted_steps;

        compute_gradient(grad);
        if (!grad.allFinite())
            restore_and_throw("optimize_uv: non-finite gradient after step");
        grad_norm = grad.lpNorm<Eigen::Infinity>();
        result.gradient_norm = grad_norm;
        if (grad_norm <= tolerance)
            result.converged = true;
    }

    result.gradient_norm = grad_norm;
    result.converged = (grad_norm <= tolerance);

    // Publish the optimized UVs; fixed slots were never modified.
    for (auto v : mesh.vertices())
    {
        if (free_index[v.idx()] >= 0)
            tex[v] = uv[v.idx()];
    }

    return result;
}

} // namespace pmp
