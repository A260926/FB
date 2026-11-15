#include "libnurbs/Surface/SurfaceMesh.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "libnurbs/Basis/BSplineBasis.hpp"
#include "libnurbs/Surface/Surface.hpp"

using namespace std;

namespace
{
    using namespace libnurbs;

    // Span whose interior lies to the right of u (u treated as the left edge of the span).
    int SpanRight(const vector<Numeric>& knots, int degree, Numeric u)
    {
        const int n = (int)knots.size();
        const Numeric domain_end = knots[n - degree - 1];
        if (u >= domain_end) return n - degree - 2;
        auto it = std::upper_bound(knots.begin(), knots.end(), u);
        return (int)std::distance(knots.begin(), it) - 1;
    }

    // Span whose interior lies to the left of u (u treated as the right edge of the span).
    int SpanLeft(const vector<Numeric>& knots, int degree, Numeric u)
    {
        const Numeric domain_start = knots[degree];
        if (u <= domain_start) return degree;
        auto it = std::lower_bound(knots.begin(), knots.end(), u);
        return (int)std::distance(knots.begin(), it) - 1;
    }

    // Analytic unit normal at (u, v) evaluated from within the given knot spans.
    Vec3 OneSidedNormal(const Surface& surface, int span_u, int span_v, Numeric u, Numeric v)
    {
        const int degree_u = surface.DegreeU;
        const int degree_v = surface.DegreeV;
        MatX basis_u = BSplineBasis::EvaluateAll(degree_u, surface.KnotsU.Values(), span_u, u, 1);
        MatX basis_v = BSplineBasis::EvaluateAll(degree_v, surface.KnotsV.Values(), span_v, v, 1);

        Vec4 a00 = Vec4::Zero();
        Vec4 a10 = Vec4::Zero();
        Vec4 a01 = Vec4::Zero();
        const int index_pre_u = span_u - degree_u;
        const int index_pre_v = span_v - degree_v;
        for (int j = 0; j <= degree_v; ++j)
        {
            for (int i = 0; i <= degree_u; ++i)
            {
                Vec4 point = ToHomo(surface.ControlPoints.Get(index_pre_u + i, index_pre_v + j));
                a00.noalias() += basis_u(0, i) * basis_v(0, j) * point;
                a10.noalias() += basis_u(1, i) * basis_v(0, j) * point;
                a01.noalias() += basis_u(0, i) * basis_v(1, j) * point;
            }
        }

        const Numeric w  = a00.w();
        const Numeric w2 = w * w;
        Vec3 su = (a10.head<3>() * w - a00.head<3>() * a10.w()) / w2;
        Vec3 sv = (a01.head<3>() * w - a00.head<3>() * a01.w()) / w2;

        Vec3 normal = su.cross(sv);
        const Numeric norm = normal.norm();
        if (!std::isfinite(norm) || norm == 0.0)
        {
            throw runtime_error("degenerate one-sided surface normal");
        }
        return normal / norm;
    }

    void ValidateStations(const vector<double>& stations, Numeric domain_start, Numeric domain_end,
                          const char* name)
    {
        if (stations.size() < 2)
        {
            throw invalid_argument(string(name) + ": need at least the two domain endpoints");
        }
        for (double s : stations)
        {
            if (!std::isfinite(s) || s < domain_start || s > domain_end)
            {
                throw invalid_argument(string(name) + ": station outside of the knot domain");
            }
        }
        for (size_t i = 1; i < stations.size(); ++i)
        {
            if (!(stations[i] > stations[i - 1]))
            {
                throw invalid_argument(string(name) + ": stations must be strictly increasing");
            }
        }
        if (stations.front() != domain_start || stations.back() != domain_end)
        {
            throw invalid_argument(string(name) + ": stations must contain both domain endpoints");
        }
    }

    // Merge the distinct internal knots of the knot vector into the station list.
    vector<double> MergeInternalKnots(const vector<double>& stations, const KnotVector& knots,
                                      int degree)
    {
        vector<double> merged = stations;
        const Numeric domain_start = knots.DomainStart(degree);
        const Numeric domain_end   = knots.DomainEnd(degree);
        for (const auto& pair : knots.GetKnotPairs())
        {
            const Numeric value = pair.Value;
            if (value <= domain_start || value >= domain_end) continue;
            auto it = std::lower_bound(merged.begin(), merged.end(), value);
            if (it == merged.end() || *it != value)
            {
                merged.insert(it, value);
            }
        }
        return merged;
    }
}

namespace libnurbs
{
    SurfaceMesh BuildSurfaceMesh(const Surface& surface,
                                 const vector<double>& us,
                                 const vector<double>& vs)
    {
        surface.Validate();

        const Numeric u_min = surface.KnotsU.DomainStart(surface.DegreeU);
        const Numeric u_max = surface.KnotsU.DomainEnd(surface.DegreeU);
        const Numeric v_min = surface.KnotsV.DomainStart(surface.DegreeV);
        const Numeric v_max = surface.KnotsV.DomainEnd(surface.DegreeV);

        ValidateStations(us, u_min, u_max, "us");
        ValidateStations(vs, v_min, v_max, "vs");

        const vector<double> grid_u = MergeInternalKnots(us, surface.KnotsU, surface.DegreeU);
        const vector<double> grid_v = MergeInternalKnots(vs, surface.KnotsV, surface.DegreeV);
        const int nu = (int)grid_u.size();
        const int nv = (int)grid_v.size();

        const auto& knots_u = surface.KnotsU.Values();
        const auto& knots_v = surface.KnotsV.Values();

        SurfaceMesh mesh;
        mesh.positions.reserve((size_t)nu * (size_t)nv);
        const auto index_of = [nu](int i, int j) { return j * nu + i; };
        for (int j = 0; j < nv; ++j)
        {
            for (int i = 0; i < nu; ++i)
            {
                mesh.positions.push_back(surface.Evaluate(grid_u[i], grid_v[j]));
            }
        }

        const int cell_count = (nu - 1) * (nv - 1);
        mesh.triangles.reserve((size_t)cell_count * 2);
        mesh.corner_uv.reserve((size_t)cell_count * 2);
        mesh.corner_normals.reserve((size_t)cell_count * 2);

        for (int j = 0; j + 1 < nv; ++j)
        {
            for (int i = 0; i + 1 < nu; ++i)
            {
                // One-sided normals of this cell, taken from inside the knot cell:
                // left/right station edges select the right/left sided span.
                const int span_u_left  = SpanRight(knots_u, surface.DegreeU, grid_u[i]);
                const int span_u_right = SpanLeft(knots_u, surface.DegreeU, grid_u[i + 1]);
                const int span_v_lower = SpanRight(knots_v, surface.DegreeV, grid_v[j]);
                const int span_v_upper = SpanLeft(knots_v, surface.DegreeV, grid_v[j + 1]);

                const Vec3 normal_ll = OneSidedNormal(surface, span_u_left, span_v_lower,
                                                      grid_u[i], grid_v[j]);
                const Vec3 normal_lr = OneSidedNormal(surface, span_u_right, span_v_lower,
                                                      grid_u[i + 1], grid_v[j]);
                const Vec3 normal_ur = OneSidedNormal(surface, span_u_right, span_v_upper,
                                                      grid_u[i + 1], grid_v[j + 1]);
                const Vec3 normal_ul = OneSidedNormal(surface, span_u_left, span_v_upper,
                                                      grid_u[i], grid_v[j + 1]);

                const int ll = index_of(i, j);
                const int lr = index_of(i + 1, j);
                const int ur = index_of(i + 1, j + 1);
                const int ul = index_of(i, j + 1);

                using UV = SurfaceMesh::UV;
                // Split the cell along the lower-left to upper-right diagonal;
                // both triangles are positively wound in UV space.
                mesh.triangles.push_back({ll, lr, ur});
                mesh.corner_uv.push_back({UV{grid_u[i], grid_v[j]},
                                          UV{grid_u[i + 1], grid_v[j]},
                                          UV{grid_u[i + 1], grid_v[j + 1]}});
                mesh.corner_normals.push_back({normal_ll, normal_lr, normal_ur});

                mesh.triangles.push_back({ll, ur, ul});
                mesh.corner_uv.push_back({UV{grid_u[i], grid_v[j]},
                                          UV{grid_u[i + 1], grid_v[j + 1]},
                                          UV{grid_u[i], grid_v[j + 1]}});
                mesh.corner_normals.push_back({normal_ll, normal_ur, normal_ul});
            }
        }
        return mesh;
    }
}
