// Copyright 2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#include "pmp/algorithms/uv_optimization.h"

#include <Eigen/Dense>
#include <Eigen/Sparse>

#include <cmath>
#include <limits>
#include <string>

namespace pmp {

namespace detail {

double uv_positive_step_limit(const std::array<double, 4>& J,
                              const std::array<double, 4>& dJ)
{
    const double inf = std::numeric_limits<double>::infinity();

    // det(J + t dJ), expanded as a quadratic in t:
    // p(t) = c0 + c1 t + c2 t^2, with c0 = det(J) > 0 by precondition.
    const double a = J[0], b = J[1], c = J[2], d = J[3];
    const double e = dJ[0], f = dJ[1], g = dJ[2], h = dJ[3];

    const double c0 = a * d - b * c;
    const double c1 = e * d + a * h - f * c - b * g;
    const double c2 = e * h - f * g;

    // Degenerate-to-linear case: the quadratic term vanishes.
    if (std::abs(c2) <= 1e-15 * (1.0 + std::abs(c1) + std::abs(c0)))
    {
        if (std::abs(c1) <= 1e-15 * (1.0 + std::abs(c0)))
            return inf;
        const double t = -c0 / c1;
        return t > 0.0 ? t : inf;
    }

    double disc = c1 * c1 - 4.0 * c2 * c0;
    if (disc < 0.0)
    {
        // A slightly negative discriminant is treated as a repeated root.
        if (disc > -1e-12 * (1.0 + c1 * c1))
        {
            const double t = -c1 / (2.0 * c2);
            return t > 0.0 ? t : inf;
        }
        return inf;
    }

    const double s = std::sqrt(disc);
    // The root (-c1 - sqrt(disc)) / (2 c2) is the first positive root for
    // either sign of c2 (the smaller positive root when c2 > 0, and the
    // single positive root when c2 < 0).
    const double t = (-c1 - s) / (2.0 * c2);
    return t > 0.0 ? t : inf;
}

} // namespace detail

namespace {

using Matrix2 = Eigen::Matrix2d;
using Vector2 = Eigen::Vector2d;
using SparseMatrix = Eigen::SparseMatrix<double>;

struct FaceCache
{
    int v[3];
    double area;  // 3D triangle area
    Matrix2 dinv; // inverse of the intrinsic reference frame D
};

class UvOptimizer
{
public:
    UvOptimizer(SurfaceMesh& mesh, unsigned int max_iterations,
                double tolerance)
        : mesh_(mesh), max_iterations_(max_iterations),
          tolerance_(tolerance)
    {
    }

    UvOptimizationResult run()
    {
        validate_arguments();
        build_fixed_mask();
        build_face_cache();
        validate_components();
        validate_initial_state();

        // Snapshot: every exception past this point must restore the UVs.
        std::vector<TexCoord> snapshot(mesh_.vertices_size());
        for (auto v : mesh_.vertices())
            snapshot[v.idx()] = tex_[v];

        try
        {
            return optimize();
        }
        catch (...)
        {
            for (auto v : mesh_.vertices())
                tex_[v] = snapshot[v.idx()];
            throw;
        }
    }

    // Full gradient (all vertex UV components, fixed ones included) of the
    // normalized objective, in vertex order. Read-only diagnostic helper.
    static std::vector<double> full_gradient(const SurfaceMesh& mesh)
    {
        UvOptimizer opt(const_cast<SurfaceMesh&>(mesh), 1, 1.0);
        opt.validate_arguments();
        opt.build_fixed_mask();
        opt.build_face_cache();
        opt.validate_initial_state();
        opt.load_working_coordinates();
        opt.assemble_gradient(opt.x_);
        std::vector<double> result(2 * mesh.vertices_size(), 0.0);
        for (auto v : mesh.vertices())
            for (int k = 0; k < 2; ++k)
                result[2 * v.idx() + k] = opt.grad_full_(2 * v.idx() + k);
        return result;
    }

private:
    SurfaceMesh& mesh_;
    unsigned int max_iterations_;
    double tolerance_;

    VertexProperty<TexCoord> tex_;
    VertexProperty<bool> uv_fixed_; // invalid when the property is absent
    std::vector<char> fixed_;
    std::vector<int> free_index_;
    std::vector<FaceCache> faces_;
    double total_area_{0.0};
    int n_free_{0};

    std::vector<double> x_; // working UVs, length 2 * vertices_size()
    Eigen::VectorXd grad_;  // free-variable gradient
    Eigen::VectorXd grad_full_;

    void validate_arguments()
    {
        if (max_iterations_ == 0)
            throw InvalidInputException(
                "optimize_uv: max_iterations must be greater than zero.");
        if (!std::isfinite(tolerance_) || tolerance_ <= 0.0)
            throw InvalidInputException(
                "optimize_uv: tolerance must be finite and positive.");

        tex_ = mesh_.get_vertex_property<TexCoord>("v:tex");
        if (!tex_)
            throw InvalidInputException(
                "optimize_uv: mesh has no \"v:tex\" vertex property.");

        for (auto v : mesh_.vertices())
        {
            const Point& p = mesh_.position(v);
            for (int k = 0; k < 3; ++k)
                if (!std::isfinite(p[k]))
                    throw InvalidInputException(
                        "optimize_uv: non-finite vertex position.");
            const TexCoord& t = tex_[v];
            for (int k = 0; k < 2; ++k)
                if (!std::isfinite(t[k]))
                    throw InvalidInputException(
                        "optimize_uv: missing or non-finite UV coordinate.");
        }

        for (auto f : mesh_.faces())
            if (mesh_.valence(f) != 3)
                throw InvalidInputException(
                    "optimize_uv: mesh contains a non-triangle face.");
    }

    void build_fixed_mask()
    {
        fixed_.assign(mesh_.vertices_size(), false);
        // Never create the property when it is absent.
        uv_fixed_ = mesh_.get_vertex_property<bool>("v:uv_fixed");
        for (auto v : mesh_.vertices())
        {
            fixed_[v.idx()] = mesh_.is_boundary(v) ||
                              (uv_fixed_ && uv_fixed_[v]);
        }

        free_index_.assign(mesh_.vertices_size(), -1);
        n_free_ = 0;
        for (auto v : mesh_.vertices())
        {
            if (!fixed_[v.idx()])
                free_index_[v.idx()] = n_free_++;
        }
    }

    void build_face_cache()
    {
        faces_.clear();
        total_area_ = 0.0;

        for (auto f : mesh_.faces())
        {
            auto it = mesh_.vertices(f);
            const Vertex v0 = *it;
            ++it;
            const Vertex v1 = *it;
            ++it;
            const Vertex v2 = *it;

            const Point& p0 = mesh_.position(v0);
            const Point& p1 = mesh_.position(v1);
            const Point& p2 = mesh_.position(v2);

            const Point e0 = p1 - p0;
            const double length = norm(e0);
            if (!std::isfinite(length) || !(length > 0.0))
                throw InvalidInputException(
                    "optimize_uv: degenerate triangle with zero area.");

            const Point ex = e0 / length;
            const Point pp = p2 - p0;
            const double sx = dot(pp, ex);
            const Point perp = pp - sx * ex;
            const double sy = norm(perp);
            const double area = 0.5 * length * sy;
            if (!std::isfinite(sy) || !(area > 0.0))
                throw InvalidInputException(
                    "optimize_uv: degenerate triangle with zero area.");

            // D = [[length, sx], [0, sy]], hence:
            Matrix2 dinv;
            dinv << 1.0 / length, -sx / (length * sy), 0.0, 1.0 / sy;

            FaceCache fc;
            fc.v[0] = static_cast<int>(v0.idx());
            fc.v[1] = static_cast<int>(v1.idx());
            fc.v[2] = static_cast<int>(v2.idx());
            fc.area = area;
            fc.dinv = dinv;
            faces_.push_back(fc);
            total_area_ += area;
        }

        if (!(total_area_ > 0.0))
            throw InvalidInputException(
                "optimize_uv: mesh has no faces with positive area.");
    }

    Matrix2 face_jacobian(const FaceCache& fc,
                          const std::vector<double>& x) const
    {
        Vector2 u0(x[2 * fc.v[0]], x[2 * fc.v[0] + 1]);
        Vector2 u1(x[2 * fc.v[1]], x[2 * fc.v[1] + 1]);
        Vector2 u2(x[2 * fc.v[2]], x[2 * fc.v[2] + 1]);
        Matrix2 u;
        u.col(0) = u1 - u0;
        u.col(1) = u2 - u0;
        return u * fc.dinv;
    }

    void validate_initial_state()
    {
        load_working_coordinates();
        for (const auto& fc : faces_)
        {
            const Matrix2 j = face_jacobian(fc, x_);
            if (!j.allFinite())
                throw InvalidInputException(
                    "optimize_uv: non-finite initial Jacobian.");
            const double det = j.determinant();
            if (!std::isfinite(det) || det <= 0.0)
                throw InvalidInputException(
                    "optimize_uv: initial Jacobian determinant must be "
                    "strictly positive.");
        }
    }

    void validate_components()
    {
        const auto n = mesh_.vertices_size();
        std::vector<int> component(n, -1);
        int n_components = 0;
        for (auto seed : mesh_.vertices())
        {
            if (component[seed.idx()] != -1)
                continue;
            const int cid = n_components++;
            std::vector<Vertex> stack{seed};
            component[seed.idx()] = cid;
            while (!stack.empty())
            {
                const Vertex v = stack.back();
                stack.pop_back();
                if (mesh_.is_isolated(v))
                    continue;
                for (auto h : mesh_.halfedges(v))
                {
                    if (mesh_.is_deleted(mesh_.edge(h)))
                        continue;
                    const Vertex w = mesh_.to_vertex(h);
                    if (component[w.idx()] == -1)
                    {
                        component[w.idx()] = cid;
                        stack.push_back(w);
                    }
                }
            }
        }

        std::vector<char> has_face(n_components, false);
        for (const auto& fc : faces_)
            has_face[component[fc.v[0]]] = true;

        std::vector<std::vector<Vertex>> pinned(n_components);
        for (auto v : mesh_.vertices())
        {
            const int cid = component[v.idx()];
            if (fixed_[v.idx()])
                pinned[cid].push_back(v);
        }

        for (int cid = 0; cid < n_components; ++cid)
        {
            if (!has_face[cid])
                continue; // isolated vertices need no anchors
            int distinct = 0;
            for (size_t i = 0; i < pinned[cid].size(); ++i)
            {
                bool unique = true;
                const TexCoord& ti = tex_[pinned[cid][i]];
                for (size_t j = 0; j < i; ++j)
                {
                    const TexCoord& tj = tex_[pinned[cid][j]];
                    if (ti[0] == tj[0] && ti[1] == tj[1])
                    {
                        unique = false;
                        break;
                    }
                }
                if (unique)
                    ++distinct;
            }
            if (distinct < 2)
                throw InvalidInputException(
                    "optimize_uv: each face component needs at least two "
                    "pinned vertices with distinct UV positions.");
        }
    }

    void load_working_coordinates()
    {
        x_.assign(2 * mesh_.vertices_size(), 0.0);
        for (auto v : mesh_.vertices())
        {
            x_[2 * v.idx()] = tex_[v][0];
            x_[2 * v.idx() + 1] = tex_[v][1];
        }
    }

    double evaluate(const std::vector<double>& x) const
    {
        double sum = 0.0;
        for (const auto& fc : faces_)
        {
            const Matrix2 j = face_jacobian(fc, x);
            const double det = j.determinant();
            const double fn2 = j.squaredNorm();
            // ||J^-1||_F^2 = ||J||_F^2 / det(J)^2 for 2x2 matrices.
            sum += fc.area * (fn2 + fn2 / (det * det));
        }
        return sum / total_area_;
    }

    // Analytic gradient of the normalized objective. Fills grad_ over free
    // components and grad_full_ over all components.
    void assemble_gradient(const std::vector<double>& x)
    {
        grad_full_.setZero(2 * mesh_.vertices_size());
        for (const auto& fc : faces_)
        {
            const Matrix2 j = face_jacobian(fc, x);
            const Matrix2 jinv = j.inverse();
            // d/dJ of (||J||_F^2 + ||J^-1||_F^2) is
            // 2 (J - J^-T J^-1 J^-T).
            const Matrix2 m =
                j - jinv.transpose() * jinv * jinv.transpose();
            const Matrix2 gmat =
                2.0 * (fc.area / total_area_) * m * fc.dinv.transpose();

            // Local variable order: (u0x, u0y, u1x, u1y, u2x, u2y).
            // U = [u1-u0, u2-u0], so moving u0 shifts both columns.
            const double local[6] = {
                -(gmat(0, 0) + gmat(0, 1)),
                -(gmat(1, 0) + gmat(1, 1)),
                gmat(0, 0), gmat(1, 0),
                gmat(0, 1), gmat(1, 1)};
            for (int k = 0; k < 6; ++k)
            {
                if (!std::isfinite(local[k]))
                    throw SolverException(
                        "optimize_uv: non-finite gradient component.");
                grad_full_(2 * fc.v[k / 2] + k % 2) += local[k];
            }
        }

        grad_.setZero(2 * n_free_);
        for (auto v : mesh_.vertices())
        {
            const int fi = free_index_[v.idx()];
            if (fi >= 0)
                for (int k = 0; k < 2; ++k)
                    grad_(2 * fi + k) = grad_full_(2 * v.idx() + k);
        }
    }

    // Gauss-Newton approximation of the Hessian of the true objective,
    // assembled sparsely over the free components only.
    SparseMatrix assemble_hessian()
    {
        std::vector<Eigen::Triplet<double>> triplets;
        triplets.reserve(static_cast<size_t>(faces_.size()) * 36);

        for (const auto& fc : faces_)
        {
            const Matrix2 j = face_jacobian(fc, x_);
            const Matrix2 jinv = j.inverse();
            const double w = 2.0 * fc.area / total_area_;
            const double a = fc.dinv(0, 0);
            const double b = fc.dinv(0, 1);
            const double c = fc.dinv(1, 1);

            // P_k = dJ/dX_k = (dU/dX_k) D^-1 for the six local UV
            // components. Moving u0 shifts both columns of U.
            Matrix2 p[6];
            p[0] << -a, -(b + c), 0.0, 0.0;
            p[1] << 0.0, 0.0, -a, -(b + c);
            p[2] << a, b, 0.0, 0.0;
            p[3] << 0.0, 0.0, a, b;
            p[4] << 0.0, c, 0.0, 0.0;
            p[5] << 0.0, 0.0, 0.0, c;

            // T_k = d(J^-1)/dX_k = -J^-1 P_k J^-1.
            Matrix2 t[6];
            for (int k = 0; k < 6; ++k)
                t[k] = jinv * p[k];

            int global[6];
            for (int k = 0; k < 6; ++k)
            {
                const Vertex v(fc.v[k / 2]);
                global[k] = fixed_[v.idx()]
                                ? -1
                                : 2 * free_index_[v.idx()] + k % 2;
            }

            for (int k = 0; k < 6; ++k)
            {
                if (global[k] < 0)
                    continue;
                for (int l = 0; l <= k; ++l)
                {
                    if (global[l] < 0)
                        continue;
                    const double entry =
                        w * (p[k].cwiseProduct(p[l]).sum() +
                             t[k].cwiseProduct(t[l]).sum());
                    triplets.emplace_back(global[k], global[l], entry);
                }
            }
        }

        SparseMatrix hessian(2 * n_free_, 2 * n_free_);
        hessian.setFromTriplets(triplets.begin(), triplets.end());
        return hessian;
    }

    // Per-face first positive zero of det(J(t)); the trial interval keeps
    // a 1% safety margin below the smallest finite bound.
    double positive_step_limit(const Eigen::VectorXd& direction) const
    {
        double limit = std::numeric_limits<double>::infinity();
        for (const auto& fc : faces_)
        {
            Vector2 dv[3];
            for (int corner = 0; corner < 3; ++corner)
            {
                const Vertex v(fc.v[corner]);
                if (fixed_[v.idx()])
                    dv[corner] = Vector2(0.0, 0.0);
                else
                {
                    const int fi = free_index_[v.idx()];
                    dv[corner] =
                        Vector2(direction(2 * fi), direction(2 * fi + 1));
                }
            }

            Matrix2 du;
            du.col(0) = dv[1] - dv[0];
            du.col(1) = dv[2] - dv[0];
            const Matrix2 dj = du * fc.dinv;
            const Matrix2 j = face_jacobian(fc, x_);

            const std::array<double, 4> jarr{
                j(0, 0), j(0, 1), j(1, 0), j(1, 1)};
            const std::array<double, 4> darr{
                dj(0, 0), dj(0, 1), dj(1, 0), dj(1, 1)};
            const double face_limit =
                detail::uv_positive_step_limit(jarr, darr);
            if (face_limit < limit)
                limit = face_limit;
        }
        return limit;
    }

    // Returns false when the factorization fails at this damping level,
    // so the caller can escalate the regularization.
    bool solve_direction(const SparseMatrix& hessian, double lambda,
                         Eigen::VectorXd& direction)
    {
        SparseMatrix reg = hessian;
        if (lambda > 0.0)
        {
            SparseMatrix identity(reg.rows(), reg.cols());
            identity.setIdentity();
            reg += lambda * identity;
        }

        Eigen::SimplicialLDLT<SparseMatrix> solver;
        solver.compute(reg);
        if (solver.info() != Eigen::Success)
            return false;

        direction = solver.solve(-grad_);
        if (solver.info() != Eigen::Success || !direction.allFinite())
            return false;
        return true;
    }

    bool all_finite_dets(const std::vector<double>& x) const
    {
        for (const auto& fc : faces_)
        {
            const Matrix2 j = face_jacobian(fc, x);
            if (!j.allFinite() || j.determinant() <= 0.0)
                return false;
        }
        return true;
    }

    UvOptimizationResult optimize()
    {
        UvOptimizationResult result;
        load_working_coordinates();
        double energy = evaluate(x_);
        if (!std::isfinite(energy))
            throw SolverException("optimize_uv: initial energy not finite.");
        result.energies.push_back(energy);

        std::vector<double> full_direction(2 * mesh_.vertices_size(), 0.0);
        std::vector<double> trial_x = x_;
        // Levenberg-Marquardt damping factor, scaled by the Hessian
        // diagonal. Zero means a pure Gauss-Newton step.
        double lambda_rel = 0.0;

        while (result.accepted_steps < max_iterations_)
        {
            assemble_gradient(x_);
            const double gradient_norm = grad_.lpNorm<Eigen::Infinity>();
            if (!std::isfinite(gradient_norm))
                throw SolverException(
                    "optimize_uv: gradient norm not finite.");
            if (gradient_norm <= tolerance_)
            {
                result.converged = true;
                break;
            }

            const SparseMatrix hessian = assemble_hessian();
            const double diag_scale =
                std::max(1.0,
                         hessian.diagonal().cwiseAbs().maxCoeff());

            const double armijo = 1e-4;
            const double rounding_tol = 1e-12 * std::max(1.0, energy);
            bool accepted = false;

            // Increase the trust-region damping whenever the undamped
            // search cannot find an acceptable step, and relax it again
            // after a successful step.
            bool solver_failure = false;
            for (int attempt = 0; attempt < 25 && !accepted; ++attempt)
            {
                Eigen::VectorXd direction;
                if (!solve_direction(hessian, lambda_rel * diag_scale,
                                     direction))
                {
                    solver_failure = true;
                    lambda_rel = std::max(lambda_rel * 10.0, 1e-12);
                    continue;
                }

                const double directional = grad_.dot(direction);
                if (!std::isfinite(directional) || directional >= 0.0)
                {
                    solver_failure = true;
                    lambda_rel = std::max(lambda_rel * 10.0, 1e-12);
                    continue;
                }
                solver_failure = false;

                double limit = positive_step_limit(direction);
                if (!std::isfinite(limit))
                    limit = 1.0;
                else
                    limit *= 0.99; // 1% safety margin to the first zero
                if (!(limit > 0.0))
                {
                    lambda_rel = std::max(lambda_rel * 10.0, 1e-12);
                    continue;
                }

                for (auto v : mesh_.vertices())
                {
                    const int fi = free_index_[v.idx()];
                    if (fi >= 0)
                    {
                        full_direction[2 * v.idx()] = direction(2 * fi);
                        full_direction[2 * v.idx() + 1] =
                            direction(2 * fi + 1);
                    }
                    else
                    {
                        full_direction[2 * v.idx()] = 0.0;
                        full_direction[2 * v.idx() + 1] = 0.0;
                    }
                }

                double step = std::min(1.0, limit);
                for (int backtrack = 0; backtrack < 80; ++backtrack)
                {
                    for (size_t i = 0; i < x_.size(); ++i)
                        trial_x[i] = x_[i] + step * full_direction[i];

                    if (!all_finite_dets(trial_x))
                    {
                        step *= 0.5;
                        continue;
                    }

                    const double trial_energy = evaluate(trial_x);
                    // Armijo with a non-positive determinant guard.
                    if (std::isfinite(trial_energy) &&
                        trial_energy <=
                            energy + armijo * step * directional &&
                        trial_energy <= energy + rounding_tol)
                    {
                        accepted = true;
                        energy = trial_energy;
                        x_.swap(trial_x);
                        break;
                    }
                    step *= 0.5;
                    if (step < 1e-20)
                        break;
                }

                if (!accepted)
                    lambda_rel = std::max(lambda_rel * 10.0, 1e-12);
            }

            if (!accepted)
            {
                if (solver_failure)
                    throw SolverException(
                        "optimize_uv: numerical factorization or search "
                        "direction is non-finite and could not be "
                        "recovered by regularization.");
                break; // line search stalled: return the current state
            }

            lambda_rel = lambda_rel > 0.0 ? lambda_rel * 0.1 : 0.0;
            write_back();
            result.energies.push_back(energy);
            ++result.accepted_steps;
        }

        // Final gradient at the accepted final state.
        assemble_gradient(x_);
        result.gradient_norm = grad_.lpNorm<Eigen::Infinity>();
        if (!std::isfinite(result.gradient_norm))
            throw SolverException(
                "optimize_uv: final gradient norm not finite.");
        result.converged = result.gradient_norm <= tolerance_;

        write_back();
        return result;
    }

    void write_back()
    {
        for (auto v : mesh_.vertices())
            tex_[v] = TexCoord(x_[2 * v.idx()], x_[2 * v.idx() + 1]);
    }
};

} // namespace

namespace detail {

std::vector<double> uv_objective_gradient(const SurfaceMesh& mesh)
{
    return UvOptimizer::full_gradient(mesh);
}

} // namespace detail

UvOptimizationResult optimize_uv(SurfaceMesh& mesh,
                                 unsigned int max_iterations,
                                 double tolerance)
{
    UvOptimizer optimizer(mesh, max_iterations, tolerance);
    return optimizer.run();
}

} // namespace pmp
