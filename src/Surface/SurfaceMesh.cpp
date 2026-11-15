#include "libnurbs/Surface/SurfaceMesh.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

#include "libnurbs/Basis/BSplineBasis.hpp"
#include "libnurbs/Core/KnotVector.hpp"
#include "libnurbs/Surface/Surface.hpp"

namespace libnurbs
{
    namespace
    {
        void ValidateSites(const std::vector<double>& sites,
                           Numeric domain_begin,
                           Numeric domain_end,
                           const char* axis)
        {
            if (sites.size() < 2)
            {
                throw std::invalid_argument(std::string(axis) + " sites must contain both domain endpoints");
            }
            for (double value : sites)
            {
                if (!std::isfinite(value))
                {
                    throw std::invalid_argument(std::string(axis) + " site is not finite");
                }
            }
            if (sites.front() != domain_begin || sites.back() != domain_end)
            {
                throw std::invalid_argument(std::string(axis) + " sites must include the exact domain endpoints");
            }
            for (size_t i = 1; i < sites.size(); ++i)
            {
                if (!(sites[i] > sites[i - 1]))
                {
                    throw std::invalid_argument(std::string(axis) + " sites must be strictly increasing");
                }
            }
            for (double value : sites)
            {
                if (value < domain_begin || value > domain_end)
                {
                    throw std::invalid_argument(std::string(axis) + " site lies outside the surface domain");
                }
            }
        }

        // Merge supplied sites with every distinct interior knot value.
        std::vector<Numeric> BuildStations(const std::vector<double>& sites,
                                           const KnotVector& knots,
                                           int degree)
        {
            std::vector<Numeric> stations(sites.begin(), sites.end());
            const auto [domain_begin, domain_end] = knots.GetDomain(degree);
            for (const auto& pair : knots.GetKnotPairs())
            {
                if (pair.Value > domain_begin && pair.Value < domain_end)
                {
                    stations.push_back(pair.Value);
                }
            }
            std::sort(stations.begin(), stations.end());
            stations.erase(std::unique(stations.begin(), stations.end()), stations.end());
            return stations;
        }

        // Index of the knot span that contains the open cell interval
        // (station[index], station[index + 1]). Selecting the midpoint keeps
        // the choice free of any fixed parameter-space epsilon.
        int CellSpan(const KnotVector& knots, int degree, Numeric left, Numeric right)
        {
            const Numeric midpoint = left + (right - left) * 0.5;
            return knots.FindSpanIndex(degree, midpoint);
        }

        // Analytic first derivatives evaluated with an explicit knot span, so
        // the one-sided limit from the interior of a cell can be requested.
        void FirstDerivatives(const Surface& surface,
                              Numeric u,
                              Numeric v,
                              int span_u,
                              int span_v,
                              Vec3& du,
                              Vec3& dv)
        {
            const int degree_u = surface.DegreeU;
            const int degree_v = surface.DegreeV;
            MatX basis_u = BSplineBasis::EvaluateAll(degree_u, surface.KnotsU.Values(), span_u, u, 1);
            MatX basis_v = BSplineBasis::EvaluateAll(degree_v, surface.KnotsV.Values(), span_v, v, 1);

            const int index_pre_u = span_u - degree_u;
            const int index_pre_v = span_v - degree_v;

            Vec4 h00 = Vec4::Zero();
            Vec4 h10 = Vec4::Zero();
            Vec4 h01 = Vec4::Zero();
            for (int j = 0; j <= degree_v; ++j)
            {
                Vec4 row00 = Vec4::Zero();
                Vec4 row10 = Vec4::Zero();
                for (int i = 0; i <= degree_u; ++i)
                {
                    const Vec4 point =
                        ToHomo(surface.ControlPoints.Get(index_pre_u + i, index_pre_v + j));
                    row00.noalias() += basis_u(0, i) * point;
                    row10.noalias() += basis_u(1, i) * point;
                }
                h00.noalias() += basis_v(0, j) * row00;
                h10.noalias() += basis_v(0, j) * row10;
                h01.noalias() += basis_v(1, j) * row00;
            }

            const Numeric weight = h00.w();
            if (!(weight != 0.0))
            {
                throw std::runtime_error("degenerate surface normal");
            }
            const Vec3 point = h00.head<3>() / weight;
            du = (h10.head<3>() - h10.w() * point) / weight;
            dv = (h01.head<3>() - h01.w() * point) / weight;
        }

        Vec3 CornerNormal(const Surface& surface,
                          Numeric u,
                          Numeric v,
                          int span_u,
                          int span_v)
        {
            Vec3 du;
            Vec3 dv;
            FirstDerivatives(surface, u, v, span_u, span_v, du, dv);
            const Numeric scale = du.norm() * dv.norm();
            const Vec3 cross = du.cross(dv);
            const Numeric length = cross.norm();
            if (!std::isfinite(length) || !(scale > 0.0) || !(length > 1e-14 * scale))
            {
                throw std::runtime_error("degenerate one-sided surface normal");
            }
            return cross / length;
        }
    } // namespace

    SurfaceMesh BuildSurfaceMesh(const Surface& surface,
                                 const std::vector<double>& us,
                                 const std::vector<double>& vs)
    {
        const auto [u_begin, u_end] = surface.GetDomainU();
        const auto [v_begin, v_end] = surface.GetDomainV();
        if (surface.ControlPoints.UCount != surface.KnotsU.Count() - surface.DegreeU - 1 ||
            surface.ControlPoints.VCount != surface.KnotsV.Count() - surface.DegreeV - 1)
        {
            throw std::invalid_argument("surface control point grid does not match its knot vectors");
        }

        ValidateSites(us, u_begin, u_end, "u");
        ValidateSites(vs, v_begin, v_end, "v");

        const std::vector<Numeric> stations_u = BuildStations(us, surface.KnotsU, surface.DegreeU);
        const std::vector<Numeric> stations_v = BuildStations(vs, surface.KnotsV, surface.DegreeV);
        const int nu = static_cast<int>(stations_u.size());
        const int nv = static_cast<int>(stations_v.size());

        SurfaceMesh mesh;
        mesh.positions.resize(static_cast<size_t>(nu) * nv);
        for (int j = 0; j < nv; ++j)
        {
            for (int i = 0; i < nu; ++i)
            {
                mesh.positions[static_cast<size_t>(j) * nu + i] =
                    surface.Evaluate(stations_u[i], stations_v[j]);
            }
        }

        const int cell_count_u = nu - 1;
        const int cell_count_v = nv - 1;
        const size_t triangle_count = static_cast<size_t>(cell_count_u) * cell_count_v * 2;
        mesh.triangles.reserve(triangle_count);
        mesh.corner_uv.reserve(triangle_count * 3);
        mesh.corner_normals.reserve(triangle_count * 3);

        auto vertex_index = [nu](int i, int j) { return j * nu + i; };

        for (int kv = 0; kv < cell_count_v; ++kv)
        {
            for (int ku = 0; ku < cell_count_u; ++ku)
            {
                const Numeric u0 = stations_u[ku];
                const Numeric u1 = stations_u[ku + 1];
                const Numeric v0 = stations_v[kv];
                const Numeric v1 = stations_v[kv + 1];
                const int span_u = CellSpan(surface.KnotsU, surface.DegreeU, u0, u1);
                const int span_v = CellSpan(surface.KnotsV, surface.DegreeV, v0, v1);

                const int i00 = vertex_index(ku, kv);
                const int i10 = vertex_index(ku + 1, kv);
                const int i11 = vertex_index(ku + 1, kv + 1);
                const int i01 = vertex_index(ku, kv + 1);

                // Lower-left -> upper-right diagonal, both triangles CCW in UV.
                const std::array<std::array<int, 3>, 2> triangle_indices{{
                    {i00, i10, i11},
                    {i00, i11, i01},
                }};
                // Corner UV for each triangle vertex, in triangle order.
                const std::array<std::array<Numeric, 2>, 2 * 3> triangle_corner_uv{{
                    {u0, v0}, {u1, v0}, {u1, v1},
                    {u0, v0}, {u1, v1}, {u0, v1},
                }};

                for (int t = 0; t < 2; ++t)
                {
                    mesh.triangles.push_back(triangle_indices[t]);
                    for (int c = 0; c < 3; ++c)
                    {
                        const Numeric cu = triangle_corner_uv[t * 3 + c][0];
                        const Numeric cv = triangle_corner_uv[t * 3 + c][1];
                        mesh.corner_uv.emplace_back(cu, cv);
                        // The normal is evaluated with this cell's knots, so a
                        // corner on a crease keeps the normal of its own side.
                        mesh.corner_normals.push_back(CornerNormal(surface, cu, cv, span_u, span_v));
                    }
                }
            }
        }
        return mesh;
    }
}
