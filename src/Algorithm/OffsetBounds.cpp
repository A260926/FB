#include "libnurbs/Algorithm/OffsetBounds.hpp"

#include <algorithm>
#include <cmath>
#include <queue>
#include <vector>

#include "libnurbs/Algorithm/MathUtils.hpp"
#include "libnurbs/Basis/BSplineBasis.hpp"
#include "libnurbs/Detail/OffsetDetail.hpp"

namespace libnurbs
{
    using namespace offset_detail;

    namespace
    {
        // ------------------------------------------------------------------
        // Interval vector algebra
        // ------------------------------------------------------------------

        IVec3 IAdd(const IVec3& a, const IVec3& b)
        {
            return {Add(a.x, b.x), Add(a.y, b.y), Add(a.z, b.z)};
        }

        IVec3 ISub(const IVec3& a, const IVec3& b)
        {
            return {Sub(a.x, b.x), Sub(a.y, b.y), Sub(a.z, b.z)};
        }

        IVec3 IScale(const IVec3& a, Interval s)
        {
            return {Mul(a.x, s), Mul(a.y, s), Mul(a.z, s)};
        }

        IVec3 DivVec(const IVec3& a, Interval b)
        {
            return {Div(a.x, b), Div(a.y, b), Div(a.z, b)};
        }

        IVec3 ICross(const IVec3& a, const IVec3& b)
        {
            return Cross(a, b);
        }

        Interval IDot(const IVec3& a, const IVec3& b)
        {
            return Add(Add(Mul(a.x, b.x), Mul(a.y, b.y)), Mul(a.z, b.z));
        }

        IVec4 ZeroVec4Box()
        {
            return {Interval{0.0, 0.0}, Interval{0.0, 0.0},
                    Interval{0.0, 0.0}, Interval{0.0, 0.0}};
        }

        /// Upper bound of the Euclidean norm of a vector box.
        Numeric NormUpper(const IVec3& a)
        {
            Interval sum = Add(Add(Mul(a.x, a.x), Mul(a.y, a.y)), Mul(a.z, a.z));
            return SqrtInt(sum).hi;
        }

        /// Lower bound of the Euclidean norm of a vector box (>= 0).
        Numeric NormLower(const IVec3& a)
        {
            Numeric total = 0.0;
            for (const Interval& component : {a.x, a.y, a.z})
            {
                Numeric min_abs = 0.0;
                if (component.lo > 0.0) min_abs = component.lo;
                else if (component.hi < 0.0) min_abs = -component.hi;
                total += min_abs * min_abs;
            }
            Numeric result = std::sqrt(total);
            return std::max(0.0, result - kIntervalEps * result - kAbsoluteFloor);
        }

        IVec3 ToVec3Box(const Vec3& v)
        {
            return {Inflate(v.x()), Inflate(v.y()), Inflate(v.z())};
        }

        // ------------------------------------------------------------------
        // Bezier patch handling
        // ------------------------------------------------------------------

        /// Extract one homogeneous Bernstein-Bezier patch from a surface that
        /// has already been refined so that every interior breakpoint has
        /// multiplicity equal to the degree.
        BezierPatch ExtractPatch(const Surface& surface, int span_u, int span_v)
        {
            BezierPatch patch{};
            patch.DegreeU = surface.DegreeU;
            patch.DegreeV = surface.DegreeV;
            const int nu = patch.DegreeU + 1;
            const int nv = patch.DegreeV + 1;
            patch.Coeffs.resize(nu * nv);
            for (int j = 0; j < nv; ++j)
            {
                for (int i = 0; i < nu; ++i)
                {
                    const Vec4& cp = surface.ControlPoints.Get(span_u * patch.DegreeU + i,
                                                               span_v * patch.DegreeV + j);
                    // Stored control points are physical (x,y,z,w); convert to
                    // homogeneous coordinates (w*x, w*y, w*z, w).
                    Vec4 homogeneous = ToHomo(cp);
                    patch.At(i, j) = ToInterval(homogeneous);
                }
            }
            return patch;
        }

        /// Convex hull of every coefficient of a tensor patch.
        IVec4 PatchHull(const BezierPatch& patch)
        {
            return CoefficientHull<IVec4>(patch.Coeffs);
        }

        /**
         * @brief Homogeneous derivative jet boxes of a Bezier patch up to
         *        (order, order). Entry [i][j] encloses the (i,j)-th
         *        derivative of the homogeneous surface over the whole
         *        sub-domain represented by the patch. Derivatives beyond the
         *        polynomial degree are exactly zero.
         */
        std::vector<std::vector<IVec4>> HomogeneousJetBoxes(const BezierPatch& patch, int order)
        {
            std::vector<std::vector<IVec4>> jets(order + 1,
                                                 std::vector<IVec4>(order + 1));
            std::vector<BezierPatch> u_derivatives(order + 1);
            u_derivatives[0] = patch;
            for (int i = 1; i <= order; ++i)
            {
                if (i > patch.DegreeU)
                {
                    // All higher U derivatives vanish; keep a degenerate marker.
                    u_derivatives[i] = u_derivatives[i - 1];
                    u_derivatives[i].DegreeU = -1;
                    u_derivatives[i].Coeffs.clear();
                }
                else
                {
                    u_derivatives[i] = DerivativePatchU(u_derivatives[i - 1]);
                }
            }
            for (int i = 0; i <= order; ++i)
            {
                if (u_derivatives[i].DegreeU < 0)
                {
                    for (int j = 0; j <= order; ++j) jets[i][j] = ZeroVec4Box();
                    continue;
                }
                BezierPatch current = u_derivatives[i];
                jets[i][0] = PatchHull(current);
                for (int j = 1; j <= order; ++j)
                {
                    if (j > patch.DegreeV)
                    {
                        jets[i][j] = ZeroVec4Box();
                        continue;
                    }
                    current = DerivativePatchV(current);
                    jets[i][j] = PatchHull(current);
                }
            }
            return jets;
        }

        /**
         * @brief Convert homogeneous derivative jet boxes into physical
         *        (rational) derivative jet boxes with the same quotient rule
         *        as Surface::EvaluateAll, evaluated with interval arithmetic.
         *        Grid indices are [order_u][order_v].
         */
        std::vector<std::vector<IVec3>> RationalJetBoxes(const BezierPatch& patch, int order)
        {
            const auto homo = HomogeneousJetBoxes(patch, order);
            std::vector<std::vector<IVec3>> result(order + 1,
                                                   std::vector<IVec3>(order + 1));
            const Interval w00 = homo[0][0].w; // strictly positive by construction
            for (int ou = 0; ou <= order; ++ou)
            {
                for (int ov = 0; ov <= order; ++ov)
                {
                    IVec4 numerator4 = homo[ou][ov];
                    IVec3 numerator{numerator4.x, numerator4.y, numerator4.z};
                    for (int i = 0; i <= ou; ++i)
                    {
                        for (int j = 0; j <= ov; ++j)
                        {
                            if (i == ou && j == ov) continue;
                            const int binomial = Binomial(ou, i) * Binomial(ov, j);
                            const Interval weight_term = homo[ou - i][ov - j].w;
                            const IVec3 scaled = IScale(result[i][j], weight_term);
                            for (int k = 0; k < binomial; ++k)
                                numerator = ISub(numerator, scaled);
                        }
                    }
                    result[ou][ov] = DivVec(numerator, w00);
                }
            }
            return result;
        }

        /**
         * @brief Convert local-parameter derivative jet boxes into physical
         *        (u,v) derivative boxes. A Bezier (sub-)patch is parametrized
         *        locally on [0,1]^2 while its image covers a physical box of
         *        widths h_u, h_v. By the chain rule,
         *          d^(ou+ov)/du^ou dv^ov = 1/(h_u^ou h_v^ov) * local jet.
         *        Skipping this factor would under-estimate every derivative
         *        on spans narrower than the unit domain.
         */
        std::vector<std::vector<IVec3>> ScaleJetsToPhysical(
            const std::vector<std::vector<IVec3>>& local,
            Numeric width_u, Numeric width_v)
        {
            const int order = static_cast<int>(local.size()) - 1;
            std::vector<std::vector<IVec3>> physical(order + 1,
                                                     std::vector<IVec3>(order + 1));
            Numeric factor_u = 1.0;
            for (int ou = 0; ou <= order; ++ou)
            {
                Numeric factor_v = 1.0;
                for (int ov = 0; ov <= order; ++ov)
                {
                    const Numeric factor = factor_u * factor_v;
                    const Interval scale{factor};
                    physical[ou][ov] = {Mul(local[ou][ov].x, scale),
                                        Mul(local[ou][ov].y, scale),
                                        Mul(local[ou][ov].z, scale)};
                    factor_v /= width_v;
                }
                factor_u /= width_u;
            }
            return physical;
        }

        // ------------------------------------------------------------------
        // Centered (Moore) enclosure of a vector-valued C2 function
        // ------------------------------------------------------------------

        /**
         * @brief Enclose f(c + delta) for delta_u in [-hu,hu], delta_v in
         *        [-hv,hv], given the point value f(c), interval boxes of the
         *        gradient and Hessian over the whole sub-box.
         *
         * The enclosure f(c) + J*delta + 1/2 delta^T H delta is a rigorous
         * Taylor bound: the quadratic remainder covers the entire continuous
         * function over the box, not merely sampled values.
         */
        IVec3 CenteredEnclosure(const Vec3& center_value,
                                const IVec3& grad_u_box,
                                const IVec3& grad_v_box,
                                const IVec3& hess_uu_box,
                                const IVec3& hess_uv_box,
                                const IVec3& hess_vv_box,
                                Numeric hu, Numeric hv)
        {
            const Interval du{-hu, hu};
            const Interval dv{-hv, hv};
            const Interval du2{0.0, hu * hu};
            const Interval dv2{0.0, hv * hv};
            const Interval duv{-hu * hv, hu * hv};

            auto component = [&](Numeric value, Interval gu, Interval gv,
                                 Interval huu, Interval huv, Interval hvv)
            {
                Interval centered = Inflate(value);
                centered = Add(centered, Mul(gu, du));
                centered = Add(centered, Mul(gv, dv));
                Interval quadratic = Add(Mul(huu, du2), Mul(hvv, dv2));
                quadratic = Add(quadratic, Mul(Mul(Interval(2.0), huv), duv));
                return Add(centered, Mul(Interval(0.5), quadratic));
            };

            const IVec3 f0 = ToVec3Box(center_value);
            return {component(f0.x.lo, grad_u_box.x, grad_v_box.x,
                              hess_uu_box.x, hess_uv_box.x, hess_vv_box.x),
                    component(f0.y.lo, grad_u_box.y, grad_v_box.y,
                              hess_uu_box.y, hess_uv_box.y, hess_vv_box.y),
                    component(f0.z.lo, grad_u_box.z, grad_v_box.z,
                              hess_uu_box.z, hess_uv_box.z, hess_vv_box.z)};
        }

        /**
         * @brief Second-order Taylor centered enclosure using the exact
         *        point Jacobian at the box center and interval Hessian boxes
         *        over the whole box.
         *
         *   f(c + delta) in f(c) + J(c) delta + 1/2 delta^T H(box) delta.
         * Using the point Jacobian (rather than a gradient hull) makes the
         * remainder scale with h^2, so the bound converges quadratically as
         * leaves shrink instead of only linearly.
         */
        IVec3 PointJacobianEnclosure(const Vec3& center_value,
                                     const Vec3& grad_u_point,
                                     const Vec3& grad_v_point,
                                     const IVec3& hess_uu_box,
                                     const IVec3& hess_uv_box,
                                     const IVec3& hess_vv_box,
                                     Numeric hu, Numeric hv)
        {
            const Interval du{-hu, hu};
            const Interval dv{-hv, hv};
            const Interval du2{0.0, hu * hu};
            const Interval dv2{0.0, hv * hv};
            const Interval duv{-hu * hv, hu * hv};

            auto component = [&](Numeric value, Numeric gu, Numeric gv,
                                 Interval huu, Interval huv, Interval hvv)
            {
                Interval result = Inflate(value);
                result = Add(result, Mul(Inflate(gu), du));
                result = Add(result, Mul(Inflate(gv), dv));
                Interval quadratic = Add(Mul(huu, du2), Mul(hvv, dv2));
                quadratic = Add(quadratic, Mul(Mul(Interval(2.0), huv), duv));
                return Add(result, Mul(Interval(0.5), quadratic));
            };

            return {component(center_value.x(), grad_u_point.x(), grad_v_point.x(),
                              hess_uu_box.x, hess_uv_box.x, hess_vv_box.x),
                    component(center_value.y(), grad_u_point.y(), grad_v_point.y(),
                              hess_uu_box.y, hess_uv_box.y, hess_vv_box.y),
                    component(center_value.z(), grad_u_point.z(), grad_v_point.z(),
                              hess_uu_box.z, hess_uv_box.z, hess_vv_box.z)};
        }

        /// Componentwise intersection of two vector boxes (both are valid
        /// whole-box enclosures, so their intersection is valid too).
        IVec3 IntersectBoxes(const IVec3& a, const IVec3& b)
        {
            auto one = [](Interval x, Interval y)
            {
                Numeric lo = std::max(x.lo, y.lo);
                Numeric hi = std::min(x.hi, y.hi);
                if (lo > hi) return x; // rounded apart: keep the conservative box
                return Interval{lo, hi};
            };
            return {one(a.x, b.x), one(a.y, b.y), one(a.z, b.z)};
        }

        /**
         * @brief Tight enclosure of the unit normal n = G/||G|| around the
         *        exact center value.
         *
         * The derivative identity dn = (I - n n^T) dG / r gives the rigorous
         * Lipschitz bound
         *   ||n(p) - n(c)|| <= (||G_u||_box*h_u + ||G_v||_box*h_v) / r_min
         * because ||I - n n^T||_2 = 1 on the unit sphere. A componentwise
         * ball of that radius around the center normal is therefore a valid
         * whole-box enclosure with far less interval dependency than the
         * direct quotient G/r.
         */
        IVec3 CenteredNormalEnclosure(const Vec3& n_center,
                                      const IVec3& gu, const IVec3& gv,
                                      Numeric r_min, Numeric hu, Numeric hv)
        {
            const Numeric lip = (NormUpper(gu) * hu + NormUpper(gv) * hv)
                                / std::max(r_min, std::numeric_limits<Numeric>::min());
            const Numeric delta = lip + std::max(kIntervalEps, kAbsoluteFloor);
            IVec3 ball;
            ball.x = {std::max(-1.0, n_center.x() - delta),
                      std::min(1.0, n_center.x() + delta)};
            ball.y = {std::max(-1.0, n_center.y() - delta),
                      std::min(1.0, n_center.y() + delta)};
            ball.z = {std::max(-1.0, n_center.z() - delta),
                      std::min(1.0, n_center.z() + delta)};
            return ball;
        }

        // ------------------------------------------------------------------
        // Offset-direction derivative boxes
        // ------------------------------------------------------------------

        /// Exact point derivatives of the unit normal n = G/r at the leaf
        /// center, from the cross product's own point jet.
        struct PointNormalJet
        {
            Vec3 N, Nu, Nv, Nuu, Nuv, Nvv;
        };

        PointNormalJet PointNormal(const Vec3& g, const Vec3& gu, const Vec3& gv,
                                   const Vec3& guu, const Vec3& guv, const Vec3& gvv)
        {
            PointNormalJet jet;
            const Numeric r = g.norm();
            jet.N = g / r;
            const Numeric ru = g.dot(gu) / r;
            const Numeric rv = g.dot(gv) / r;
            const Numeric r2 = r * r;
            jet.Nu = gu / r - g * (ru / r2);
            jet.Nv = gv / r - g * (rv / r2);
            const Numeric ruu = (gu.dot(gu) + g.dot(guu)) / r - ru * ru / r;
            const Numeric rvv = (gv.dot(gv) + g.dot(gvv)) / r - rv * rv / r;
            const Numeric ruv = (gu.dot(gv) + g.dot(guv)) / r - ru * rv / r;
            const Numeric r3 = r2 * r;
            jet.Nuu = guu / r - gu * (2.0 * ru / r2)
                      - g * (ruu / r2) + g * (2.0 * ru * ru / r3);
            jet.Nvv = gvv / r - gv * (2.0 * rv / r2)
                      - g * (rvv / r2) + g * (2.0 * rv * rv / r3);
            jet.Nuv = guv / r - gv * (ru / r2) - gu * (rv / r2)
                      - g * (ruv / r2) + g * (2.0 * ru * rv / r3);
            return jet;
        }

        /// Enclosure of r = ||G|| and the unit normal n = G/r together with
        /// its first and second parametric derivatives.
        struct NormalBoxes
        {
            Interval Radius;
            IVec3 Normal;
            IVec3 NormalU;
            IVec3 NormalV;
            IVec3 NormalUU;
            IVec3 NormalUV;
            IVec3 NormalVV;
        };

        NormalBoxes BuildNormalBoxes(const IVec3& g, const IVec3& gu, const IVec3& gv,
                                     const IVec3& guu, const IVec3& guv, const IVec3& gvv)
        {
            NormalBoxes nb{};
            const Interval g2 = IDot(g, g);
            nb.Radius = SqrtInt(g2);
            const Numeric reciprocal_hi = 1.0 / nb.Radius.hi;
            const Numeric reciprocal_lo = 1.0 / nb.Radius.lo;
            const Interval r_inv{std::max(0.0, reciprocal_hi - kAbsoluteFloor),
                                 reciprocal_lo + kAbsoluteFloor};
            nb.Normal = IScale(g, r_inv);

            const Interval ru = Div(IDot(g, gu), nb.Radius);
            const Interval rv = Div(IDot(g, gv), nb.Radius);
            const Interval r2 = Mul(nb.Radius, nb.Radius);

            // n_u = G_u/r - G r_u / r^2
            nb.NormalU = ISub(DivVec(gu, nb.Radius), IScale(g, Div(ru, r2)));
            nb.NormalV = ISub(DivVec(gv, nb.Radius), IScale(g, Div(rv, r2)));

            // r_uu = (|G_u|^2 + G.G_uu)/r - r_u^2/r
            const Interval ruu = Sub(Div(Add(IDot(gu, gu), IDot(g, guu)), nb.Radius),
                                     Div(Mul(ru, ru), nb.Radius));
            const Interval rvv = Sub(Div(Add(IDot(gv, gv), IDot(g, gvv)), nb.Radius),
                                     Div(Mul(rv, rv), nb.Radius));
            const Interval ruv = Sub(Div(Add(IDot(gu, gv), IDot(g, guv)), nb.Radius),
                                     Div(Mul(ru, rv), nb.Radius));

            const Interval r3 = Mul(r2, nb.Radius);
            // n_uu = G_uu/r - 2 G_u r_u/r^2 - G r_uu/r^2 + 2 G r_u^2/r^3
            nb.NormalUU = DivVec(guu, nb.Radius);
            nb.NormalUU = ISub(nb.NormalUU, IScale(gu, Mul(Interval(2.0), Div(ru, r2))));
            nb.NormalUU = ISub(nb.NormalUU, IScale(g, Div(ruu, r2)));
            nb.NormalUU = IAdd(nb.NormalUU, IScale(g, Mul(Interval(2.0), Div(Mul(ru, ru), r3))));

            nb.NormalVV = DivVec(gvv, nb.Radius);
            nb.NormalVV = ISub(nb.NormalVV, IScale(gv, Mul(Interval(2.0), Div(rv, r2))));
            nb.NormalVV = ISub(nb.NormalVV, IScale(g, Div(rvv, r2)));
            nb.NormalVV = IAdd(nb.NormalVV, IScale(g, Mul(Interval(2.0), Div(Mul(rv, rv), r3))));

            // n_uv = G_uv/r - G_v r_u/r^2 - G_u r_v/r^2 - G r_uv/r^2
            //        + 2 G r_u r_v/r^3
            nb.NormalUV = DivVec(guv, nb.Radius);
            nb.NormalUV = ISub(nb.NormalUV, IScale(gv, Div(ru, r2)));
            nb.NormalUV = ISub(nb.NormalUV, IScale(gu, Div(rv, r2)));
            nb.NormalUV = ISub(nb.NormalUV, IScale(g, Div(ruv, r2)));
            nb.NormalUV = IAdd(nb.NormalUV, IScale(g, Mul(Interval(2.0), Div(Mul(ru, rv), r3))));
            return nb;
        }

        // ------------------------------------------------------------------
        // Partition tree
        // ------------------------------------------------------------------

        struct Leaf
        {
            int Id{0};
            Numeric Umin, Umax, Vmin, Vmax;
            BezierPatch Source;
            BezierPatch Candidate;
        };

        struct LeafVerdict
        {
            bool CrossPositive{false};
            Numeric CrossLowerBound{0};
            Numeric ErrorUpperBound{std::numeric_limits<Numeric>::infinity()};
        };

        LeafVerdict EvaluateLeaf(const Surface& source, const Surface& candidate,
                                 const Leaf& leaf, Numeric distance)
        {
            LeafVerdict verdict{};

            // Interval jet boxes over the leaf sub-domain (orders 0..3 on
            // both axes), obtained from differentiated Bernstein forms.
            // The Bezier (sub-)patch is parametrized locally on [0,1]^2, so
            // its jets are converted to physical (u,v) derivatives with the
            // inverse span-width chain-rule factors.
            const Numeric width_u = leaf.Umax - leaf.Umin;
            const Numeric width_v = leaf.Vmax - leaf.Vmin;
            const auto box_s = ScaleJetsToPhysical(
                RationalJetBoxes(leaf.Source, 3), width_u, width_v);
            const auto box_c = ScaleJetsToPhysical(
                RationalJetBoxes(leaf.Candidate, 3), width_u, width_v);

            const Numeric uc = (leaf.Umin + leaf.Umax) * 0.5;
            const Numeric vc = (leaf.Vmin + leaf.Vmax) * 0.5;
            const Numeric hu = width_u * 0.5;
            const Numeric hv = width_v * 0.5;

            // Exact point jets at the leaf center.
            const Grid<Vec3> point_s = source.EvaluateAll(uc, vc, 3, 3);
            const Grid<Vec3> point_c = candidate.EvaluateAll(uc, vc, 3, 3);

            const Vec3 s0 = point_s.Get(0, 0);
            const Vec3 su = point_s.Get(1, 0);
            const Vec3 sv = point_s.Get(0, 1);
            const Vec3 suu = point_s.Get(2, 0);
            const Vec3 suv = point_s.Get(1, 1);
            const Vec3 svv = point_s.Get(0, 2);
            const Vec3 suuu = point_s.Get(3, 0);
            const Vec3 suuv = point_s.Get(2, 1);
            const Vec3 suvv = point_s.Get(1, 2);
            const Vec3 svvv = point_s.Get(0, 3);

            // Exact point cross-product jet G = S_u x S_v at the center.
            const Vec3 gu_point = suu.cross(sv) + su.cross(suv);
            const Vec3 gv_point = suv.cross(sv) + su.cross(svv);
            const Vec3 guu_point = suuu.cross(sv) + 2.0 * suu.cross(suv) + su.cross(suvv);
            const Vec3 guv_point = suuv.cross(sv) + suu.cross(svv) + suv.cross(suv) + su.cross(suvv);
            const Vec3 gvv_point = suvv.cross(sv) + 2.0 * suv.cross(svv) + su.cross(svvv);

            // Cross-product boxes G = S_u x S_v and their derivatives:
            //   G_u  = S_uu x S_v + S_u x S_uv
            //   G_v  = S_uv x S_v + S_u x S_vv
            //   G_uu = S_uuu x S_v + 2 S_uu x S_uv + S_u x S_uvv
            //   G_uv = S_uuv x S_v + S_uu x S_vv + S_uv x S_uv + S_u x S_uvv
            //   G_vv = S_uvv x S_v + 2 S_uv x S_vv + S_u x S_vvv
            const IVec3 g_box = ICross(box_s[1][0], box_s[0][1]);
            const IVec3 gu_box = IAdd(ICross(box_s[2][0], box_s[0][1]),
                                      ICross(box_s[1][0], box_s[1][1]));
            const IVec3 gv_box = IAdd(ICross(box_s[1][1], box_s[0][1]),
                                      ICross(box_s[1][0], box_s[0][2]));
            const IVec3 guu_box = IAdd(IAdd(ICross(box_s[3][0], box_s[0][1]),
                                            IScale(ICross(box_s[2][0], box_s[1][1]), Interval(2.0))),
                                       ICross(box_s[1][0], box_s[1][2]));
            const IVec3 guv_box = IAdd(IAdd(ICross(box_s[2][1], box_s[0][1]),
                                            ICross(box_s[2][0], box_s[0][2])),
                                       IAdd(ICross(box_s[1][1], box_s[1][1]),
                                            ICross(box_s[1][0], box_s[1][2])));
            const IVec3 gvv_box = IAdd(IAdd(ICross(box_s[1][2], box_s[0][1]),
                                            IScale(ICross(box_s[1][1], box_s[0][2]), Interval(2.0))),
                                       ICross(box_s[1][0], box_s[0][3]));

            // Centered enclosure of G over the leaf, tightened by the plain
            // coefficient hull.
            const Vec3 g_point = su.cross(sv);
            IVec3 g_centered = CenteredEnclosure(g_point, gu_box, gv_box,
                                                 guu_box, guv_box, gvv_box, hu, hv);
            g_centered = IntersectBoxes(g_centered, g_box);

            verdict.CrossLowerBound = NormLower(g_centered);
            if (!(verdict.CrossLowerBound > 0.0))
            {
                return verdict; // cannot normalize on this leaf; force subdivision
            }
            verdict.CrossPositive = true;

            // Unit-normal boxes with derivatives up to second order.
            NormalBoxes nb = BuildNormalBoxes(g_centered, gu_box, gv_box,
                                              guu_box, guv_box, gvv_box);
            // Tighten the normal-value box with a centered Lipschitz
            // enclosure; both are independently valid, so intersecting them
            // cannot weaken the guarantee.
            const Vec3 n_center = g_point.normalized();
            const IVec3 normal_lipschitz = CenteredNormalEnclosure(
                n_center, gu_box, gv_box, verdict.CrossLowerBound, hu, hv);
            nb.Normal = IntersectBoxes(nb.Normal, normal_lipschitz);

            // Target T = S + d n. Interval second-derivative boxes drive the
            // Hessian remainder of the second-order centered enclosure below.
            const Interval d_distance{distance};
            const IVec3 tuu_box = IAdd(box_s[2][0], IScale(nb.NormalUU, d_distance));
            const IVec3 tuv_box = IAdd(box_s[1][1], IScale(nb.NormalUV, d_distance));
            const IVec3 tvv_box = IAdd(box_s[0][2], IScale(nb.NormalVV, d_distance));

            // Exact point normal jet at the center gives the point Jacobian
            // of T; the interval Hessian boxes above supply the rigorous
            // quadratic remainder. This is second-order (O(h^2)) rather than
            // the O(h) gradient-hull centered form.
            const PointNormalJet pn = PointNormal(g_point, gu_point, gv_point,
                                                  guu_point, guv_point, gvv_point);
            const Vec3 tu_point = su + distance * pn.Nu;
            const Vec3 tv_point = sv + distance * pn.Nv;
            const Vec3 t_point = s0 + distance * pn.N;
            const Vec3 f_point = point_c.Get(0, 0) - t_point;
            const Vec3 fu_point = point_c.Get(1, 0) - tu_point;
            const Vec3 fv_point = point_c.Get(0, 1) - tv_point;

            // F = C - T Hessian boxes (interval), used only as the remainder.
            const IVec3 fuu_hess = ISub(box_c[2][0], tuu_box);
            const IVec3 fuv_hess = ISub(box_c[1][1], tuv_box);
            const IVec3 fvv_hess = ISub(box_c[0][2], tvv_box);
            IVec3 f_box = PointJacobianEnclosure(f_point, fu_point, fv_point,
                                                 fuu_hess, fuv_hess, fvv_hess, hu, hv);
            // Tighten against the plain convex-hull coefficient enclosure,
            // which is independently a valid whole-box bound.
            const IVec3 t_hull = IAdd(box_s[0][0], IScale(nb.Normal, d_distance));
            const IVec3 f_hull = ISub(box_c[0][0], t_hull);
            f_box = IntersectBoxes(f_box, f_hull);

            verdict.ErrorUpperBound = NormUpper(f_box);
            if (!std::isfinite(verdict.ErrorUpperBound))
                verdict.ErrorUpperBound = std::numeric_limits<Numeric>::infinity();
            return verdict;
        }

        /// Split a leaf along its longest parameter side at the midpoint.
        std::pair<Leaf, Leaf> SplitLeaf(const Leaf& leaf)
        {
            Leaf left = leaf;
            Leaf right = leaf;
            const Numeric span_u = leaf.Umax - leaf.Umin;
            const Numeric span_v = leaf.Vmax - leaf.Vmin;
            if (span_u >= span_v)
            {
                auto [sl, sr] = SplitPatchU(leaf.Source, 0.5);
                auto [cl, cr] = SplitPatchU(leaf.Candidate, 0.5);
                const Numeric mid = (leaf.Umin + leaf.Umax) * 0.5;
                left.Umax = mid;
                left.Source = sl;
                left.Candidate = cl;
                right.Umin = mid;
                right.Source = sr;
                right.Candidate = cr;
            }
            else
            {
                auto [sl, sr] = SplitPatchV(leaf.Source, 0.5);
                auto [cl, cr] = SplitPatchV(leaf.Candidate, 0.5);
                const Numeric mid = (leaf.Vmin + leaf.Vmax) * 0.5;
                left.Vmax = mid;
                left.Source = sl;
                left.Candidate = cl;
                right.Vmin = mid;
                right.Source = sr;
                right.Candidate = cr;
            }
            return {left, right};
        }

        OffsetCertBox ToCertBox(const Leaf& leaf, const LeafVerdict& verdict)
        {
            return {leaf.Umin, leaf.Umax, leaf.Vmin, leaf.Vmax,
                    verdict.CrossLowerBound, verdict.ErrorUpperBound};
        }
    }

    OffsetCertification CertifyNormalOffset(const Surface& source,
                                            const Surface& candidate,
                                            Numeric distance,
                                            Numeric tolerance,
                                            int max_boxes)
    {
        OffsetCertification result{};
        std::string reason;
        if (!ValidateSurfaceStructure(source, true, &reason) ||
            !ValidateSurfaceStructure(candidate, false, &reason) ||
            !ValidateOffsetOptions(distance, tolerance, max_boxes, &reason))
        {
            result.Status = OffsetCertStatus::invalid_input;
            return result;
        }

        // Common breakpoint partition so source and candidate spans align.
        std::vector<Numeric> breaks_u = DistinctBreakpoints(source.KnotsU);
        std::vector<Numeric> breaks_v = DistinctBreakpoints(source.KnotsV);
        for (Numeric value : DistinctBreakpoints(candidate.KnotsU))
        {
            if (std::ranges::find(breaks_u, value) == breaks_u.end()) breaks_u.push_back(value);
        }
        for (Numeric value : DistinctBreakpoints(candidate.KnotsV))
        {
            if (std::ranges::find(breaks_v, value) == breaks_v.end()) breaks_v.push_back(value);
        }
        std::ranges::sort(breaks_u);
        std::ranges::sort(breaks_v);

        const Surface source_bezier = RefineToBezier(source, breaks_u, breaks_v);
        const Surface candidate_bezier = RefineToBezier(candidate, breaks_u, breaks_v);

        const int span_count_u = static_cast<int>(breaks_u.size()) + 1;
        const int span_count_v = static_cast<int>(breaks_v.size()) + 1;

        std::vector<Leaf> leaves;
        leaves.reserve(span_count_u * span_count_v);
        std::vector<Numeric> u_edges{0.0};
        u_edges.insert(u_edges.end(), breaks_u.begin(), breaks_u.end());
        u_edges.push_back(1.0);
        std::vector<Numeric> v_edges{0.0};
        v_edges.insert(v_edges.end(), breaks_v.begin(), breaks_v.end());
        v_edges.push_back(1.0);

        int next_id = 0;
        for (int sv = 0; sv < span_count_v; ++sv)
        {
            for (int su = 0; su < span_count_u; ++su)
            {
                Leaf leaf{};
                leaf.Id = next_id++;
                leaf.Umin = u_edges[su];
                leaf.Umax = u_edges[su + 1];
                leaf.Vmin = v_edges[sv];
                leaf.Vmax = v_edges[sv + 1];
                leaf.Source = ExtractPatch(source_bezier, su, sv);
                leaf.Candidate = ExtractPatch(candidate_bezier, su, sv);
                leaves.push_back(std::move(leaf));
            }
        }

        std::vector<LeafVerdict> verdicts(leaves.size());
        for (int i = 0; i < static_cast<int>(leaves.size()); ++i)
            verdicts[i] = EvaluateLeaf(source, candidate, leaves[i], distance);

        if (static_cast<int>(leaves.size()) > max_boxes)
        {
            // Even the natural knot partition exceeds the box budget; report
            // the partition honestly and leave the proof unresolved.
            result.Status = OffsetCertStatus::unresolved;
            result.BoxesUsed = static_cast<int>(leaves.size());
            result.Boxes.reserve(leaves.size());
            result.WorstCrossLowerBound = std::numeric_limits<Numeric>::infinity();
            result.WorstErrorUpperBound = 0.0;
            for (int i = 0; i < static_cast<int>(leaves.size()); ++i)
            {
                result.Boxes.push_back(ToCertBox(leaves[i], verdicts[i]));
                if (verdicts[i].CrossPositive)
                    result.WorstCrossLowerBound = std::min(result.WorstCrossLowerBound,
                                                           verdicts[i].CrossLowerBound);
                result.WorstErrorUpperBound = std::max(result.WorstErrorUpperBound,
                                                       verdicts[i].ErrorUpperBound);
            }
            return result;
        }

        struct Pending
        {
            Numeric Score;
            int Index;
            int LeafId;
        };
        std::priority_queue<Pending, std::vector<Pending>,
                            decltype([](const Pending& a, const Pending& b)
                                     { return a.Score < b.Score; })>
            queue;

        for (int i = 0; i < static_cast<int>(leaves.size()); ++i)
        {
            if (!verdicts[i].CrossPositive)
                queue.push({std::numeric_limits<Numeric>::infinity(), i, leaves[i].Id});
            else if (verdicts[i].ErrorUpperBound > tolerance)
                queue.push({verdicts[i].ErrorUpperBound, i, leaves[i].Id});
        }

        while (!queue.empty())
        {
            const Pending top = queue.top();
            queue.pop();
            const int index = top.Index;
            // A queued entry is only valid while its leaf still carries the
            // identity it had when enqueued; subdivision replaces leaves in
            // place and invalidates older entries.
            if (index >= static_cast<int>(leaves.size()) || leaves[index].Id != top.LeafId)
                continue;
            const LeafVerdict& current = verdicts[index];
            const bool still_bad = !current.CrossPositive ||
                                   current.ErrorUpperBound > tolerance;
            if (!still_bad) continue; // stale queue entry

            if (static_cast<int>(leaves.size()) + 1 > max_boxes)
            {
                result.Status = OffsetCertStatus::unresolved;
                result.BoxesUsed = static_cast<int>(leaves.size());
                result.Boxes.reserve(leaves.size());
                result.WorstCrossLowerBound = std::numeric_limits<Numeric>::infinity();
                result.WorstErrorUpperBound = 0.0;
                for (int i = 0; i < static_cast<int>(leaves.size()); ++i)
                {
                    result.Boxes.push_back(ToCertBox(leaves[i], verdicts[i]));
                    if (verdicts[i].CrossPositive)
                        result.WorstCrossLowerBound = std::min(result.WorstCrossLowerBound,
                                                               verdicts[i].CrossLowerBound);
                    result.WorstErrorUpperBound = std::max(result.WorstErrorUpperBound,
                                                           verdicts[i].ErrorUpperBound);
                }
                return result;
            }

            const Leaf original = leaves[index];
            auto [left, right] = SplitLeaf(original);
            const int right_index = static_cast<int>(leaves.size());
            left.Id = next_id++;
            right.Id = next_id++;
            leaves[index] = left;
            leaves.push_back(right);
            verdicts[index] = EvaluateLeaf(source, candidate, leaves[index], distance);
            verdicts.push_back(EvaluateLeaf(source, candidate, leaves[right_index], distance));

            for (int refreshed : {index, right_index})
            {
                const LeafVerdict& verdict = verdicts[refreshed];
                if (!verdict.CrossPositive)
                    queue.push({std::numeric_limits<Numeric>::infinity(), refreshed, leaves[refreshed].Id});
                else if (verdict.ErrorUpperBound > tolerance)
                    queue.push({verdict.ErrorUpperBound, refreshed, leaves[refreshed].Id});
            }
        }

        // Final whole-partition verification.
        result.WorstCrossLowerBound = std::numeric_limits<Numeric>::infinity();
        result.WorstErrorUpperBound = 0.0;
        result.Boxes.reserve(leaves.size());
        for (int i = 0; i < static_cast<int>(leaves.size()); ++i)
        {
            const LeafVerdict& verdict = verdicts[i];
            if (!verdict.CrossPositive || !(verdict.ErrorUpperBound <= tolerance))
            {
                result.Status = OffsetCertStatus::unresolved;
                result.BoxesUsed = static_cast<int>(leaves.size());
                return result;
            }
            result.WorstCrossLowerBound = std::min(result.WorstCrossLowerBound,
                                                   verdict.CrossLowerBound);
            result.WorstErrorUpperBound = std::max(result.WorstErrorUpperBound,
                                                   verdict.ErrorUpperBound);
            result.Boxes.push_back(ToCertBox(leaves[i], verdict));
        }

        result.BoxesUsed = static_cast<int>(result.Boxes.size());
        result.Status = OffsetCertStatus::certified;
        return result;
    }
}
