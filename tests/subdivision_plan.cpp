// Tests for the owning Catmull-Clark subdivision plan.
// Registered through task_support/check.h; no main() here.

#include "task_support/check.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <numbers>
#include <random>
#include <vector>

#include "pmp/algorithms/shapes.h"
#include "pmp/algorithms/subdivision.h"
#include "pmp/algorithms/subdivision_plan.h"

using namespace pmp;

namespace {

constexpr double tol = 1e-7;

bool close(double a, double b, double eps = tol)
{
    return std::fabs(a - b) <= eps;
}

// ---------------------------------------------------------------------------
// Independent reference: per-round subdivision of dense scalar weight rows.
// Each vertex carries one scalar per source control value; rows are combined
// round by round. Follows the deterministic ordering documented for
// SubdivisionPlan, but shares no code with it.
// ---------------------------------------------------------------------------

struct RefLevel
{
    std::vector<std::vector<double>> rows; // dense weight row per vertex
    std::vector<char> vfeat;
    std::vector<std::array<uint32_t, 2>> everts;
    std::vector<char> efeat;
    std::vector<std::vector<uint32_t>> fverts;
    std::vector<std::vector<uint32_t>> fedges; // edge i: vertex i -> i+1
    std::vector<uint32_t> fsource;
};

struct RefAdj
{
    std::vector<std::vector<uint32_t>> efaces;
    std::vector<std::vector<uint32_t>> vedges;
    std::vector<std::vector<uint32_t>> vfaces;
    std::vector<char> vbnd;
};

RefAdj ref_adjacency(const RefLevel& in)
{
    RefAdj adj;
    const size_t nv = in.rows.size();
    const size_t ne = in.everts.size();
    const size_t nf = in.fverts.size();
    adj.efaces.assign(ne, {});
    adj.vedges.assign(nv, {});
    adj.vfaces.assign(nv, {});
    for (size_t f = 0; f < nf; ++f)
        for (size_t i = 0; i < in.fverts[f].size(); ++i)
        {
            adj.efaces[in.fedges[f][i]].push_back(uint32_t(f));
            adj.vfaces[in.fverts[f][i]].push_back(uint32_t(f));
        }
    for (size_t e = 0; e < ne; ++e)
    {
        adj.vedges[in.everts[e][0]].push_back(uint32_t(e));
        adj.vedges[in.everts[e][1]].push_back(uint32_t(e));
    }
    adj.vbnd.assign(nv, 0);
    for (size_t v = 0; v < nv; ++v)
    {
        size_t nb = 0;
        for (auto e : adj.vedges[v])
            if (adj.efaces[e].size() == 1)
                ++nb;
        adj.vbnd[v] = nb == 2;
    }
    return adj;
}

uint32_t ref_other(const RefLevel& in, uint32_t e, uint32_t v)
{
    return in.everts[e][0] == v ? in.everts[e][1] : in.everts[e][0];
}

RefLevel ref_refine(const RefLevel& in)
{
    const RefAdj adj = ref_adjacency(in);
    const size_t nv = in.rows.size();
    const size_t ne = in.everts.size();
    const size_t nf = in.fverts.size();
    const size_t nsrc = in.rows.empty() ? 0 : in.rows[0].size();

    RefLevel out;
    out.rows.assign(nv + ne + nf, std::vector<double>(nsrc, 0.0));

    auto add = [&](size_t dst, uint32_t src, double w) {
        auto& r = out.rows[dst];
        const auto& s = in.rows[src];
        for (size_t j = 0; j < nsrc; ++j)
            r[j] += w * s[j];
    };
    // face points are rows of the output level
    auto add_face_point = [&](size_t dst, size_t src, double w) {
        auto& r = out.rows[dst];
        const auto& s = out.rows[src];
        for (size_t j = 0; j < nsrc; ++j)
            r[j] += w * s[j];
    };

    for (size_t f = 0; f < nf; ++f) // face points
    {
        const double w = 1.0 / in.fverts[f].size();
        for (auto v : in.fverts[f])
            add(nv + ne + f, v, w);
    }
    for (size_t e = 0; e < ne; ++e) // edge points
    {
        const auto [a, b] = in.everts[e];
        if (adj.efaces[e].size() == 1 || in.efeat[e])
        {
            add(nv + e, a, 0.5);
            add(nv + e, b, 0.5);
        }
        else
        {
            add(nv + e, a, 0.25);
            add(nv + e, b, 0.25);
            add_face_point(nv + e, nv + ne + adj.efaces[e][0], 0.25);
            add_face_point(nv + e, nv + ne + adj.efaces[e][1], 0.25);
        }
    }
    for (size_t v = 0; v < nv; ++v) // vertex points
    {
        if (adj.vbnd[v])
        {
            add(v, uint32_t(v), 0.75);
            for (auto e : adj.vedges[v])
                if (adj.efaces[e].size() == 1)
                    add(v, ref_other(in, e, uint32_t(v)), 0.125);
        }
        else if (in.vfeat[v])
        {
            std::vector<uint32_t> fn;
            for (auto e : adj.vedges[v])
                if (in.efeat[e])
                    fn.push_back(ref_other(in, e, uint32_t(v)));
            if (fn.size() == 2)
            {
                add(v, uint32_t(v), 0.75);
                add(v, fn[0], 0.125);
                add(v, fn[1], 0.125);
            }
            else
                add(v, uint32_t(v), 1.0);
        }
        else
        {
            const double k = double(adj.vedges[v].size());
            const double w = 1.0 / (k * k);
            for (auto e : adj.vedges[v])
                add(v, ref_other(in, e, uint32_t(v)), w);
            for (auto f : adj.vfaces[v])
                add_face_point(v, nv + ne + f, w);
            add(v, uint32_t(v), (k - 2.0) / k);
        }
    }

    out.vfeat.assign(in.vfeat.begin(), in.vfeat.end());
    for (size_t e = 0; e < ne; ++e)
        out.vfeat.push_back(in.efeat[e]);
    out.vfeat.resize(nv + ne + nf, 0);
    for (size_t e = 0; e < ne; ++e)
    {
        out.everts.push_back({in.everts[e][0], uint32_t(nv + e)});
        out.everts.push_back({uint32_t(nv + e), in.everts[e][1]});
        out.efeat.push_back(in.efeat[e]);
        out.efeat.push_back(in.efeat[e]);
    }
    for (size_t f = 0; f < nf; ++f)
    {
        const auto& fv = in.fverts[f];
        const auto& fe = in.fedges[f];
        const size_t k = fv.size();
        const uint32_t fp = uint32_t(nv + ne + f);
        const size_t spoke = out.everts.size();
        for (size_t i = 0; i < k; ++i)
        {
            out.everts.push_back({uint32_t(nv + fe[i]), fp});
            out.efeat.push_back(0);
        }
        for (size_t i = 0; i < k; ++i)
        {
            const size_t ip = (i + k - 1) % k;
            const uint32_t vi = fv[i];
            const uint32_t hi =
                2 * fe[i] + (in.everts[fe[i]][0] == vi ? 0 : 1);
            const uint32_t hp =
                2 * fe[ip] + (in.everts[fe[ip]][0] == vi ? 0 : 1);
            out.fverts.push_back(
                {vi, uint32_t(nv + fe[i]), fp, uint32_t(nv + fe[ip])});
            out.fedges.push_back({hi, uint32_t(spoke + i),
                                  uint32_t(spoke + ip), hp});
            out.fsource.push_back(in.fsource[f]);
        }
    }
    return out;
}

RefLevel ref_geometry0(const SurfaceMesh& mesh)
{
    RefLevel out;
    auto vfeature = mesh.get_vertex_property<bool>("v:feature");
    auto efeature = mesh.get_edge_property<bool>("e:feature");
    std::vector<uint32_t> vmap(mesh.vertices_size(), UINT32_MAX);
    for (auto v : mesh.vertices())
    {
        vmap[v.idx()] = uint32_t(out.rows.size());
        std::vector<double> row(mesh.n_vertices(), 0.0);
        row[vmap[v.idx()]] = 1.0;
        out.rows.push_back(std::move(row));
        out.vfeat.push_back(vfeature && vfeature[v] ? 1 : 0);
    }
    std::vector<uint32_t> emap(mesh.edges_size(), UINT32_MAX);
    for (auto e : mesh.edges())
    {
        emap[e.idx()] = uint32_t(out.everts.size());
        out.everts.push_back(
            {vmap[mesh.vertex(e, 0).idx()], vmap[mesh.vertex(e, 1).idx()]});
        out.efeat.push_back(efeature && efeature[e] ? 1 : 0);
    }
    for (auto f : mesh.faces())
    {
        std::vector<uint32_t> fv, incoming;
        for (auto h : mesh.halfedges(f))
        {
            fv.push_back(vmap[mesh.to_vertex(h).idx()]);
            incoming.push_back(emap[mesh.edge(h).idx()]);
        }
        std::vector<uint32_t> fe(fv.size());
        for (size_t i = 0; i < fv.size(); ++i)
            fe[i] = incoming[(i + 1) % fv.size()];
        out.fverts.push_back(std::move(fv));
        out.fedges.push_back(std::move(fe));
        out.fsource.push_back(uint32_t(out.fsource.size()));
    }
    return out;
}

RefLevel ref_channel0(const SubdivisionPlan::Channel& channel,
                      const SurfaceMesh& mesh)
{
    // corner ids: faces in iteration order, halfedges from mesh.halfedge(f)
    std::vector<uint32_t> he2corner(mesh.halfedges_size(), UINT32_MAX);
    std::vector<std::pair<size_t, size_t>> franges;
    uint32_t nc = 0;
    for (auto f : mesh.faces())
    {
        franges.emplace_back(nc, nc);
        for (auto h : mesh.halfedges(f))
            he2corner[h.idx()] = nc++;
        franges.back().second = nc;
    }

    // weld corners across interior edges by matching value indices
    std::vector<uint32_t> parent(nc);
    for (uint32_t i = 0; i < nc; ++i)
        parent[i] = i;
    auto find = [&](uint32_t i) {
        uint32_t r = i;
        while (parent[r] != r)
            r = parent[r];
        while (parent[i] != r)
        {
            uint32_t p = parent[i];
            parent[i] = r;
            i = p;
        }
        return r;
    };
    auto unite = [&](uint32_t a, uint32_t b) {
        a = find(a);
        b = find(b);
        if (a != b)
            parent[b] = a;
    };
    for (auto e : mesh.edges())
    {
        const auto h0 = mesh.halfedge(e, 0);
        const auto h1 = mesh.halfedge(e, 1);
        if (!mesh.face(h0).is_valid() || !mesh.face(h1).is_valid())
            continue;
        const uint32_t c0 = he2corner[h0.idx()];
        const uint32_t c1 = he2corner[h1.idx()];
        const uint32_t p0 = he2corner[mesh.prev_halfedge(h0).idx()];
        const uint32_t p1 = he2corner[mesh.prev_halfedge(h1).idx()];
        if (channel.corner_indices[c0] == channel.corner_indices[p1] &&
            channel.corner_indices[c1] == channel.corner_indices[p0])
        {
            unite(c0, p1);
            unite(c1, p0);
        }
    }

    RefLevel out;
    std::map<uint32_t, uint32_t> ids;
    std::vector<uint32_t> cvert(nc);
    for (uint32_t c = 0; c < nc; ++c)
    {
        const uint32_t root = find(c);
        auto [it, inserted] = ids.emplace(root, uint32_t(ids.size()));
        if (inserted)
        {
            std::vector<double> row(channel.values.size(), 0.0);
            row[channel.corner_indices[c]] = 1.0;
            out.rows.push_back(std::move(row));
            out.vfeat.push_back(0);
        }
        cvert[c] = it->second;
    }
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> eids;
    for (size_t f = 0; f < franges.size(); ++f)
    {
        const auto [b, e] = franges[f];
        const size_t k = e - b;
        std::vector<uint32_t> fv(k), fe(k);
        for (size_t i = 0; i < k; ++i)
            fv[i] = cvert[b + i];
        for (size_t i = 0; i < k; ++i)
        {
            const uint32_t a = fv[i];
            const uint32_t bb = fv[(i + 1) % k];
            auto [it, inserted] =
                eids.emplace(std::minmax(a, bb), uint32_t(eids.size()));
            if (inserted)
            {
                out.everts.push_back({a, bb});
                out.efeat.push_back(0);
            }
            fe[i] = it->second;
        }
        out.fverts.push_back(std::move(fv));
        out.fedges.push_back(std::move(fe));
        out.fsource.push_back(uint32_t(f));
    }
    return out;
}

struct RefData
{
    RefLevel geo;
    std::vector<RefLevel> channels;
};

RefData build_ref(const SurfaceMesh& mesh, size_t levels,
                  const std::vector<SubdivisionPlan::Channel>& channels)
{
    RefData ref;
    ref.geo = ref_geometry0(mesh);
    for (size_t l = 0; l < levels; ++l)
        ref.geo = ref_refine(ref.geo);
    for (const auto& ch : channels)
    {
        RefLevel r = ref_channel0(ch, mesh);
        for (size_t l = 0; l < levels; ++l)
            r = ref_refine(r);
        ref.channels.push_back(std::move(r));
    }
    return ref;
}

std::vector<Point> apply_rows(const std::vector<std::vector<double>>& rows,
                              const std::vector<Point>& src)
{
    std::vector<Point> out(rows.size(), Point(0, 0, 0));
    for (size_t i = 0; i < rows.size(); ++i)
        for (size_t j = 0; j < rows[i].size(); ++j)
            out[i] += rows[i][j] * src[j];
    return out;
}

std::vector<TexCoord> apply_rows(const std::vector<std::vector<double>>& rows,
                                 const std::vector<TexCoord>& src)
{
    std::vector<TexCoord> out(rows.size(), TexCoord(0, 0));
    for (size_t i = 0; i < rows.size(); ++i)
        for (size_t j = 0; j < rows[i].size(); ++j)
            out[i] += rows[i][j] * src[j];
    return out;
}

std::vector<Point> apply_stencil(const SubdivisionPlan::Stencil& s,
                                 const std::vector<Point>& src)
{
    std::vector<Point> out(s.rows(), Point(0, 0, 0));
    for (size_t i = 0; i < s.rows(); ++i)
        for (size_t j = s.row_offsets[i]; j < s.row_offsets[i + 1]; ++j)
            out[i] += s.weights[j] * src[s.columns[j]];
    return out;
}

std::vector<TexCoord> apply_stencil(const SubdivisionPlan::Stencil& s,
                                    const std::vector<TexCoord>& src)
{
    std::vector<TexCoord> out(s.rows(), TexCoord(0, 0));
    for (size_t i = 0; i < s.rows(); ++i)
        for (size_t j = s.row_offsets[i]; j < s.row_offsets[i + 1]; ++j)
            out[i] += s.weights[j] * src[s.columns[j]];
    return out;
}

std::map<IndexType, Scalar> row_as_map(const SubdivisionPlan::Stencil& s,
                                       size_t i)
{
    std::map<IndexType, Scalar> row;
    for (size_t j = s.row_offsets[i]; j < s.row_offsets[i + 1]; ++j)
        row[s.columns[j]] += s.weights[j];
    return row;
}

int map_close(const std::map<IndexType, Scalar>& a,
              const std::map<IndexType, Scalar>& b, double eps)
{
    TASK_CHECK(a.size() == b.size());
    auto ia = a.begin();
    auto ib = b.begin();
    for (; ia != a.end() && ib != b.end(); ++ia, ++ib)
    {
        TASK_CHECK(ia->first == ib->first);
        TASK_CHECK(close(ia->second, ib->second, eps));
    }
    return 0;
}

// CSR structure: sorted merged columns, unit row sums, values matching rows.
int check_stencil_vs_rows(const SubdivisionPlan::Stencil& s,
                          const std::vector<std::vector<double>>& rows)
{
    TASK_CHECK(s.rows() == rows.size());
    TASK_CHECK(s.row_offsets.size() == rows.size() + 1);
    TASK_CHECK(s.columns.size() == s.weights.size());
    if (s.row_offsets.empty())
        return 1;
    TASK_CHECK(s.row_offsets[0] == 0);
    TASK_CHECK(s.row_offsets.back() == s.columns.size());
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const size_t begin = s.row_offsets[i];
        const size_t end = s.row_offsets[i + 1];
        TASK_CHECK(begin <= end);
        double sum = 0.0;
        for (size_t j = begin; j < end; ++j)
        {
            if (j > begin)
                TASK_CHECK(s.columns[j] > s.columns[j - 1]); // sorted, merged
            TASK_CHECK(s.columns[j] < rows[i].size());
            sum += s.weights[j];
        }
        TASK_CHECK(close(sum, 1.0, 1e-9)); // rows sum to one
        std::map<IndexType, Scalar> dense = row_as_map(s, i);
        for (size_t c = 0; c < rows[i].size(); ++c)
        {
            const double got = dense.count(IndexType(c)) ? dense[IndexType(c)] : 0.0;
            TASK_CHECK(close(got, rows[i][c]));
        }
    }
    return 0;
}

int check_topology(const SubdivisionPlan& plan, const RefData& ref)
{
    TASK_CHECK(plan.n_vertices() == ref.geo.rows.size());
    TASK_CHECK(plan.n_faces() == ref.geo.fverts.size());
    TASK_CHECK(plan.faces().size() == 4 * plan.n_faces());
    for (size_t f = 0; f < plan.n_faces(); ++f)
    {
        TASK_CHECK(ref.geo.fverts[f].size() == 4);
        for (size_t j = 0; j < 4; ++j)
            TASK_CHECK(plan.faces()[4 * f + j] == ref.geo.fverts[f][j]);
        TASK_CHECK(plan.face_sources()[f] == ref.geo.fsource[f]);
    }
    if (check_stencil_vs_rows(plan.position_stencil(), ref.geo.rows))
        return 1;
    TASK_CHECK(plan.n_channels() == ref.channels.size());
    for (size_t c = 0; c < ref.channels.size(); ++c)
    {
        TASK_CHECK(plan.n_channel_vertices(c) == ref.channels[c].rows.size());
        TASK_CHECK(plan.channel_corners(c).size() == 4 * plan.n_faces());
        for (size_t f = 0; f < plan.n_faces(); ++f)
            for (size_t j = 0; j < 4; ++j)
                TASK_CHECK(plan.channel_corners(c)[4 * f + j] ==
                           ref.channels[c].fverts[f][j]);
        if (check_stencil_vs_rows(plan.channel_stencil(c),
                                  ref.channels[c].rows))
            return 1;
    }
    return 0;
}

int check_eval(SubdivisionPlan& plan, const RefData& ref,
               const std::vector<Point>& positions,
               const std::vector<std::vector<TexCoord>>& channel_values)
{
    plan.evaluate(positions, channel_values);
    const auto ref_pos = apply_rows(ref.geo.rows, positions);
    TASK_CHECK(plan.positions().size() == ref_pos.size());
    for (size_t i = 0; i < ref_pos.size(); ++i)
        for (int d = 0; d < 3; ++d)
            TASK_CHECK(close(plan.positions()[i][d], ref_pos[i][d]));
    // evaluation must be a pure stencil application
    const auto csr_pos = apply_stencil(plan.position_stencil(), positions);
    TASK_CHECK(csr_pos.size() == plan.positions().size());
    for (size_t i = 0; i < csr_pos.size(); ++i)
        TASK_CHECK(csr_pos[i] == plan.positions()[i]);
    TASK_CHECK(plan.n_channels() == channel_values.size());
    for (size_t c = 0; c < ref.channels.size(); ++c)
    {
        const auto ref_vals =
            apply_rows(ref.channels[c].rows, channel_values[c]);
        TASK_CHECK(plan.channel_values(c).size() == ref_vals.size());
        for (size_t i = 0; i < ref_vals.size(); ++i)
            for (int d = 0; d < 2; ++d)
                TASK_CHECK(close(plan.channel_values(c)[i][d],
                                 ref_vals[i][d]));
        const auto csr_vals =
            apply_stencil(plan.channel_stencil(c), channel_values[c]);
        TASK_CHECK(csr_vals.size() == plan.channel_values(c).size());
        for (size_t i = 0; i < csr_vals.size(); ++i)
            TASK_CHECK(csr_vals[i] == plan.channel_values(c)[i]);
    }
    return 0;
}

std::vector<Point> source_positions(const SurfaceMesh& mesh,
                                    const SubdivisionPlan& plan)
{
    std::vector<Point> pos(plan.n_source_vertices());
    for (size_t i = 0; i < pos.size(); ++i)
        pos[i] = mesh.position(plan.source_vertex(i));
    return pos;
}

std::vector<std::vector<TexCoord>>
source_values(const std::vector<SubdivisionPlan::Channel>& channels)
{
    std::vector<std::vector<TexCoord>> values;
    for (const auto& ch : channels)
        values.push_back(ch.values);
    return values;
}

// Compare against the production in-place Catmull-Clark as an unordered set
// of points, matched greedily within tolerance (robust to near-coincident
// coordinates that would order differently under sorting).
int check_vs_production(SurfaceMesh mesh, size_t levels,
                        const SubdivisionPlan& plan)
{
    for (size_t l = 0; l < levels; ++l)
        catmull_clark_subdivision(mesh);
    TASK_CHECK(mesh.n_vertices() == plan.n_vertices());
    TASK_CHECK(mesh.n_faces() == plan.n_faces());
    std::vector<char> used(plan.n_vertices(), 0);
    for (auto v : mesh.vertices())
    {
        const auto& p = mesh.position(v);
        size_t best = plan.n_vertices();
        double best_dist = 1e300;
        for (size_t i = 0; i < plan.n_vertices(); ++i)
        {
            if (used[i])
                continue;
            double d = 0.0;
            for (int k = 0; k < 3; ++k)
                d = std::max(d, std::fabs(p[k] - plan.positions()[i][k]));
            if (d < best_dist)
            {
                best_dist = d;
                best = i;
            }
        }
        TASK_CHECK(best < plan.n_vertices());
        TASK_CHECK(close(best_dist, 0.0));
        if (best < plan.n_vertices())
            used[best] = 1;
    }
    return 0;
}

template <typename F>
bool throws_invalid(F&& f)
{
    try
    {
        f();
    }
    catch (const InvalidInputException&)
    {
        return true;
    }
    catch (...)
    {
    }
    return false;
}

template <typename F>
bool throws_logic(F&& f)
{
    try
    {
        f();
    }
    catch (const std::logic_error&)
    {
        return true;
    }
    catch (...)
    {
    }
    return false;
}

// ---------------------------------------------------------------------------
// Mesh factories
// ---------------------------------------------------------------------------

SurfaceMesh make_quad()
{
    SurfaceMesh mesh;
    mesh.add_vertex(Point(0, 0, 0));
    mesh.add_vertex(Point(2, 0, 0));
    mesh.add_vertex(Point(2, 1, 0));
    mesh.add_vertex(Point(-1, 3, 0));
    mesh.add_quad(Vertex(0), Vertex(1), Vertex(2), Vertex(3));
    return mesh;
}

// Closed pentagonal pyramid: apex has valence 5, base corners valence 3.
SurfaceMesh make_pyramid()
{
    SurfaceMesh mesh;
    for (size_t i = 0; i < 5; ++i)
    {
        const double a = 2.0 * std::numbers::pi * double(i) / 5.0;
        mesh.add_vertex(Point(std::cos(a), std::sin(a), 0.0));
    }
    mesh.add_vertex(Point(0.13, -0.21, 1.0)); // apex, slightly off-center
    mesh.add_face({Vertex(4), Vertex(3), Vertex(2), Vertex(1), Vertex(0)});
    for (size_t i = 0; i < 5; ++i)
        mesh.add_triangle(Vertex(i), Vertex((i + 1) % 5), Vertex(5));
    return mesh;
}

SurfaceMesh perturbed_cube()
{
    auto mesh = hexahedron();
    for (auto v : mesh.vertices())
    {
        const auto q = mesh.position(v);
        mesh.position(v) =
            Point(1.2 * q[0] + 0.3 * q[1], 0.7 * q[1], q[2] + 0.2 * q[0]);
    }
    return mesh;
}

SurfaceMesh perturbed_plane(size_t resolution)
{
    auto mesh = plane(resolution);
    for (auto v : mesh.vertices())
    {
        auto p = mesh.position(v);
        p[2] = 0.3 * p[0] * p[0] + 0.2 * p[1] + 0.05 * p[0] * p[1];
        mesh.position(v) = p;
    }
    return mesh;
}

// compact source vertex id per corner, in the plan's corner order
std::vector<IndexType> welded_indices(const SurfaceMesh& mesh)
{
    std::vector<IndexType> vmap(mesh.vertices_size(), PMP_MAX_INDEX);
    IndexType next = 0;
    for (auto v : mesh.vertices())
        vmap[v.idx()] = next++;
    std::vector<IndexType> indices;
    for (auto f : mesh.faces())
        for (auto h : mesh.halfedges(f))
            indices.push_back(vmap[mesh.to_vertex(h).idx()]);
    return indices;
}

// corner id of the corner at vertex v within face f
IndexType corner_id(const SurfaceMesh& mesh, Face f, Vertex v)
{
    IndexType base = 0;
    for (auto ff : mesh.faces())
    {
        if (ff == f)
            break;
        base += IndexType(mesh.valence(ff));
    }
    IndexType j = 0;
    for (auto h : mesh.halfedges(f))
    {
        if (mesh.to_vertex(h) == v)
            return base + j;
        ++j;
    }
    return PMP_MAX_INDEX;
}

std::vector<TexCoord> base_uv_values(size_t n)
{
    std::vector<TexCoord> values;
    values.reserve(n);
    for (size_t i = 0; i < n; ++i)
        values.emplace_back(0.13 * double(i) + 0.1, 0.29 * double(i) - 0.2);
    return values;
}

// ---------------------------------------------------------------------------
// topology group
// ---------------------------------------------------------------------------

int test_topology_compact_mapping_with_garbage()
{
    auto mesh = plane(4);
    // poke holes without garbage collection: a corner vertex (taking its
    // face), one interior face, and one boundary face on the far side
    mesh.delete_vertex(Vertex(0));
    mesh.delete_face(Face(6));
    mesh.delete_face(Face(15));
    TASK_CHECK(mesh.is_deleted(Vertex(0)));
    TASK_CHECK(mesh.is_deleted(Face(6)));
    TASK_CHECK(mesh.n_vertices() == 23); // corner vertex 24 is deleted, too
    TASK_CHECK(mesh.n_faces() == 13);

    // expected compact source order
    std::vector<IndexType> expect_vertices;
    for (auto v : mesh.vertices())
        expect_vertices.push_back(v.idx());
    std::vector<IndexType> expect_faces;
    for (auto f : mesh.faces())
        expect_faces.push_back(f.idx());
    size_t expect_corners = 0;
    for (auto f : mesh.faces())
        expect_corners += mesh.valence(f);

    SubdivisionPlan plan;
    plan.build(mesh, 2, {});
    TASK_CHECK(plan.is_built());
    TASK_CHECK(plan.levels() == 2);
    TASK_CHECK(plan.n_source_vertices() == expect_vertices.size());
    TASK_CHECK(plan.n_source_faces() == expect_faces.size());
    TASK_CHECK(plan.n_source_corners() == expect_corners);
    for (size_t i = 0; i < expect_vertices.size(); ++i)
        TASK_CHECK(plan.source_vertex(i).idx() == expect_vertices[i]);
    for (size_t i = 0; i < expect_faces.size(); ++i)
        TASK_CHECK(plan.source_face(i).idx() == expect_faces[i]);
    // corners: contiguous per face, halfedge order, corner -> to-vertex
    IndexType c = 0;
    for (size_t fi = 0; fi < expect_faces.size(); ++fi)
    {
        const Face f = plan.source_face(fi);
        for (auto h : mesh.halfedges(f))
        {
            const Halfedge hc = plan.source_corner(c);
            TASK_CHECK(hc == h);
            TASK_CHECK(mesh.face(hc) == f);
            TASK_CHECK(mesh.to_vertex(hc) == mesh.to_vertex(h));
            ++c;
        }
    }

    const auto ref = build_ref(mesh, 2, {});
    TASK_CHECK(check_topology(plan, ref) == 0);
    TASK_CHECK(check_eval(plan, ref, source_positions(mesh, plan), {}) == 0);
    TASK_CHECK(check_vs_production(mesh, 2, plan) == 0);
    return 0;
}

int test_topology_components_and_boundary()
{
    SurfaceMesh mesh;
    // component 1: a quad
    mesh.add_vertex(Point(0, 0, 0));
    mesh.add_vertex(Point(1, 0, 0));
    mesh.add_vertex(Point(1, 1, 0.2));
    mesh.add_vertex(Point(0, 1, 0));
    mesh.add_quad(Vertex(0), Vertex(1), Vertex(2), Vertex(3));
    // component 2: a triangle
    mesh.add_vertex(Point(3, 0, 0));
    mesh.add_vertex(Point(4, 0.1, 0));
    mesh.add_vertex(Point(3.5, 1, 0.4));
    mesh.add_triangle(Vertex(4), Vertex(5), Vertex(6));
    // component 3: a 2x2 plane patch, shifted aside
    const auto patch = perturbed_plane(2);
    std::vector<Vertex> moved;
    for (auto v : patch.vertices())
        moved.push_back(mesh.add_vertex(patch.position(v) + Point(6, 0, 0)));
    for (auto f : patch.faces())
    {
        std::vector<Vertex> corners;
        for (auto v : patch.vertices(f))
            corners.push_back(moved[v.idx()]);
        mesh.add_face(corners);
    }
    TASK_CHECK(mesh.n_faces() == 6);

    SubdivisionPlan plan;
    plan.build(mesh, 2, {});
    // quads spawn 4x4=16 subfaces at level 2, the triangle 3x4=12
    TASK_CHECK(plan.n_faces() == 5 * 16 + 12);
    for (IndexType src = 0; src < 6; ++src)
        TASK_CHECK(std::count(plan.face_sources().begin(),
                              plan.face_sources().end(),
                              src) == (src == 1 ? 12 : 16));

    const auto ref = build_ref(mesh, 2, {});
    TASK_CHECK(check_topology(plan, ref) == 0);
    TASK_CHECK(check_eval(plan, ref, source_positions(mesh, plan), {}) == 0);
    TASK_CHECK(check_vs_production(mesh, 2, plan) == 0);
    return 0;
}

int test_topology_extraordinary_vertices()
{
    auto mesh = make_pyramid();
    SubdivisionPlan plan;
    plan.build(mesh, 2, {});
    const auto ref = build_ref(mesh, 2, {});
    TASK_CHECK(check_topology(plan, ref) == 0);
    TASK_CHECK(check_eval(plan, ref, source_positions(mesh, plan), {}) == 0);
    TASK_CHECK(check_vs_production(mesh, 2, plan) == 0);

    // hand-check the level-1 stencil rows of the valence-5 apex and one
    // valence-3 base vertex
    SubdivisionPlan plan1;
    plan1.build(mesh, 1, {});
    const auto& s = plan1.position_stencil();
    std::map<IndexType, Scalar> apex;
    apex[5] = 3.0 / 5.0 + 5.0 / 75.0; // self + five triangle face points
    for (IndexType u = 0; u < 5; ++u)
        apex[u] = 1.0 / 25.0 + 2.0 / 75.0; // neighbor + two triangle points
    TASK_CHECK(map_close(row_as_map(s, 5), apex, 1e-12) == 0);

    std::map<IndexType, Scalar> base;
    base[0] = 1.0 / 3.0 + 1.0 / 9.0 * (1.0 / 5.0 + 2.0 / 3.0);
    base[1] = 1.0 / 9.0 + 1.0 / 9.0 * (1.0 / 5.0 + 1.0 / 3.0);
    base[4] = 1.0 / 9.0 + 1.0 / 9.0 * (1.0 / 5.0 + 1.0 / 3.0);
    base[5] = 1.0 / 9.0 + 1.0 / 9.0 * (2.0 / 3.0);
    base[2] = 1.0 / 9.0 * (1.0 / 5.0);
    base[3] = 1.0 / 9.0 * (1.0 / 5.0);
    TASK_CHECK(map_close(row_as_map(s, 0), base, 1e-12) == 0);
    return 0;
}

int test_topology_features()
{
    // feature cross on the interior spokes of a 2x2 plane
    auto mesh = perturbed_plane(2);
    auto vfeature = mesh.vertex_property<bool>("v:feature", false);
    auto efeature = mesh.edge_property<bool>("e:feature", false);
    for (IndexType u : {1, 3, 5, 7})
    {
        const auto e = mesh.find_edge(Vertex(u), Vertex(4));
        TASK_CHECK(e.is_valid());
        efeature[e] = true;
    }
    vfeature[Vertex(4)] = true; // four feature edges: stays fixed
    vfeature[Vertex(0)] = true; // boundary vertex: boundary rule wins

    SubdivisionPlan plan;
    plan.build(mesh, 2, {});
    const auto ref = build_ref(mesh, 2, {});
    TASK_CHECK(check_topology(plan, ref) == 0);
    TASK_CHECK(check_eval(plan, ref, source_positions(mesh, plan), {}) == 0);
    TASK_CHECK(check_vs_production(mesh, 2, plan) == 0);

    // isolated interior feature vertex without feature edges: kept fixed
    auto mesh2 = perturbed_plane(2);
    auto vfeature2 = mesh2.vertex_property<bool>("v:feature", false);
    mesh2.edge_property<bool>("e:feature", false);
    vfeature2[Vertex(4)] = true;
    SubdivisionPlan plan2;
    plan2.build(mesh2, 2, {});
    const auto ref2 = build_ref(mesh2, 2, {});
    TASK_CHECK(check_topology(plan2, ref2) == 0);
    TASK_CHECK(check_eval(plan2, ref2, source_positions(mesh2, plan2), {}) ==
               0);
    TASK_CHECK(check_vs_production(mesh2, 2, plan2) == 0);
    // the fixed vertex keeps its exact position through both rounds
    const auto p4 = mesh2.position(Vertex(4));
    for (int d = 0; d < 3; ++d)
        TASK_CHECK(close(plan2.positions()[4][d], p4[d]));
    return 0;
}

int test_topology_channel_seams()
{
    const auto mesh = plane(2);
    const auto welded = welded_indices(mesh);
    const auto base = base_uv_values(9);

    // A: fully welded, one value per vertex
    SubdivisionPlan::Channel a{base, welded};

    // B: seam on interior edge (3,4) by reindexing one corner
    SubdivisionPlan::Channel b{base, welded};
    b.values.push_back(TexCoord(0.77, 0.55));
    b.corner_indices[corner_id(mesh, Face(2), Vertex(3))] = 9;

    // C: seam on a different interior edge (1,4)
    SubdivisionPlan::Channel c{base, welded};
    c.values.push_back(TexCoord(0.33, 0.66));
    c.corner_indices[corner_id(mesh, Face(1), Vertex(1))] = 9;

    // D: like B but the new index maps to an equal value; must still cut
    SubdivisionPlan::Channel d{base, welded};
    d.values.push_back(base[3]);
    d.corner_indices[corner_id(mesh, Face(2), Vertex(3))] = 9;

    // E: fans around the center separated by seams while all four center
    // corners reuse value index 0
    SubdivisionPlan::Channel e{base, welded};
    for (size_t i = 0; i < 8; ++i)
        e.values.emplace_back(0.5 + 0.03 * double(i), -0.4);
    e.corner_indices[corner_id(mesh, Face(0), Vertex(3))] = 9;
    e.corner_indices[corner_id(mesh, Face(2), Vertex(3))] = 10;
    e.corner_indices[corner_id(mesh, Face(0), Vertex(1))] = 11;
    e.corner_indices[corner_id(mesh, Face(1), Vertex(1))] = 12;
    e.corner_indices[corner_id(mesh, Face(1), Vertex(5))] = 13;
    e.corner_indices[corner_id(mesh, Face(3), Vertex(5))] = 14;
    e.corner_indices[corner_id(mesh, Face(2), Vertex(7))] = 15;
    e.corner_indices[corner_id(mesh, Face(3), Vertex(7))] = 16;
    for (IndexType f = 0; f < 4; ++f)
        e.corner_indices[corner_id(mesh, Face(f), Vertex(4))] = 0;

    // F: fully cut, every corner owns its value
    SubdivisionPlan::Channel f;
    f.values = base_uv_values(16);
    for (size_t i = 0; i < 16; ++i)
        f.corner_indices.push_back(IndexType(i));

    const std::vector<SubdivisionPlan::Channel> channels = {a, b, c, d, e, f};

    // hand-computed level-1 UV vertex counts: vertices + edges + faces of the
    // cut level-0 channel meshes
    SubdivisionPlan plan1;
    plan1.build(mesh, 1, channels);
    TASK_CHECK(plan1.n_channel_vertices(0) == 9 + 12 + 4);  // welded
    TASK_CHECK(plan1.n_channel_vertices(1) == 10 + 13 + 4); // one cut edge
    TASK_CHECK(plan1.n_channel_vertices(2) == 10 + 13 + 4);
    TASK_CHECK(plan1.n_channel_vertices(3) == 10 + 13 + 4); // equal values cut
    TASK_CHECK(plan1.n_channel_vertices(4) == 16 + 16 + 4); // split fans
    TASK_CHECK(plan1.n_channel_vertices(5) == 16 + 16 + 4); // fully cut

    // two refinement rounds: per-corner UVs, sources and stencil weights
    SubdivisionPlan plan;
    plan.build(mesh, 2, channels);
    const auto ref = build_ref(mesh, 2, channels);
    TASK_CHECK(check_topology(plan, ref) == 0);
    TASK_CHECK(check_eval(plan, ref, source_positions(mesh, plan),
                          source_values(channels)) == 0);

    // all channels share the same refined geometry
    SubdivisionPlan geometry_only;
    geometry_only.build(mesh, 2, {});
    geometry_only.evaluate(source_positions(mesh, geometry_only), {});
    TASK_CHECK(plan.faces() == geometry_only.faces());
    TASK_CHECK(plan.face_sources() == geometry_only.face_sources());
    TASK_CHECK(plan.positions().size() == geometry_only.positions().size());
    for (size_t i = 0; i < plan.positions().size(); ++i)
        TASK_CHECK(plan.positions()[i] == geometry_only.positions()[i]);
    return 0;
}

// ---------------------------------------------------------------------------
// stencils group
// ---------------------------------------------------------------------------

int test_stencils_single_quad_exact()
{
    auto mesh = make_quad();
    SubdivisionPlan plan;
    plan.build(mesh, 1, {});
    TASK_CHECK(plan.n_vertices() == 9);
    TASK_CHECK(plan.n_faces() == 4);
    const auto& s = plan.position_stencil();
    TASK_CHECK(s.rows() == 9);

    // vertex points: (6P + prev + next) / 8 along the boundary
    for (IndexType v = 0; v < 4; ++v)
    {
        const std::map<IndexType, Scalar> expect{
            {v, 0.75}, {(v + 1) % 4, 0.125}, {(v + 3) % 4, 0.125}};
        TASK_CHECK(row_as_map(s, v) == expect);
    }
    // edge points: midpoints (all edges are boundary)
    for (IndexType e = 0; e < 4; ++e)
    {
        const auto a = mesh.vertex(Edge(e), 0).idx();
        const auto b = mesh.vertex(Edge(e), 1).idx();
        const std::map<IndexType, Scalar> expect{{a, 0.5}, {b, 0.5}};
        TASK_CHECK(row_as_map(s, 4 + e) == expect);
    }
    // face point: centroid
    const std::map<IndexType, Scalar> expect_face{
        {0, 0.25}, {1, 0.25}, {2, 0.25}, {3, 0.25}};
    TASK_CHECK(row_as_map(s, 8) == expect_face);

    // subfaces: one quad per corner, all from source face 0
    for (size_t f = 0; f < 4; ++f)
        TASK_CHECK(plan.face_sources()[f] == 0);
    // corner order: (vertex point, next edge point, face point, previous edge)
    TASK_CHECK(plan.faces()[0] == 0 && plan.faces()[1] == 4 &&
               plan.faces()[2] == 8 && plan.faces()[3] == 7);
    TASK_CHECK(plan.faces()[4] == 1 && plan.faces()[5] == 5 &&
               plan.faces()[6] == 8 && plan.faces()[7] == 4);

    // evaluated positions match the hand-computed rules
    const std::vector<Point> p = {Point(0, 0, 0), Point(2, 0, 0),
                                  Point(2, 1, 0), Point(-1, 3, 0)};
    plan.evaluate(p, {});
    for (int d = 0; d < 3; ++d)
    {
        TASK_CHECK(close(plan.positions()[0][d],
                         (6.0 * p[0][d] + p[1][d] + p[3][d]) / 8.0));
        TASK_CHECK(close(plan.positions()[4][d],
                         0.5 * (p[0][d] + p[1][d])));
        TASK_CHECK(close(plan.positions()[8][d],
                         0.25 * (p[0][d] + p[1][d] + p[2][d] + p[3][d])));
    }
    return 0;
}

int test_stencils_cube_two_levels()
{
    const auto mesh = perturbed_cube();
    SubdivisionPlan plan;
    plan.build(mesh, 2, {});
    const auto ref = build_ref(mesh, 2, {});
    TASK_CHECK(check_topology(plan, ref) == 0);
    TASK_CHECK(check_eval(plan, ref, source_positions(mesh, plan), {}) == 0);
    TASK_CHECK(check_vs_production(mesh, 2, plan) == 0);
    // closed quad mesh: 8+12+6 = 26 and 26+48+24 = 98 vertices, 24/96 faces
    TASK_CHECK(plan.n_vertices() == 98);
    TASK_CHECK(plan.n_faces() == 96);
    return 0;
}

int test_stencils_plane_channels_two_rounds()
{
    const auto mesh = perturbed_plane(2);
    const auto welded = welded_indices(mesh);

    // channel welded exactly like the geometry: UVs follow positions
    SubdivisionPlan::Channel like_geometry;
    for (size_t i = 0; i < 9; ++i)
        like_geometry.values.push_back(TexCoord(0, 0));
    like_geometry.corner_indices = welded;

    // channel with a seam on edge (3,4)
    SubdivisionPlan::Channel seam{base_uv_values(9), welded};
    seam.values.push_back(TexCoord(0.77, 0.55));
    seam.corner_indices[corner_id(mesh, Face(2), Vertex(3))] = 9;

    const std::vector<SubdivisionPlan::Channel> channels = {like_geometry,
                                                            seam};
    for (size_t levels : {2, 3})
    {
        SubdivisionPlan plan;
        plan.build(mesh, levels, channels);
        const auto ref = build_ref(mesh, levels, channels);
        TASK_CHECK(check_topology(plan, ref) == 0);
        auto positions = source_positions(mesh, plan);
        auto values = source_values(channels);
        // fill the geometry-like channel with the position x/y values
        for (size_t i = 0; i < 9; ++i)
            values[0][i] = TexCoord(positions[i][0], positions[i][1]);
        TASK_CHECK(check_eval(plan, ref, positions, values) == 0);
        // the welded channel must reproduce the x/y components of the
        // refined positions corner by corner through the shared subfaces
        for (size_t f = 0; f < plan.n_faces(); ++f)
            for (size_t j = 0; j < 4; ++j)
            {
                const IndexType g = plan.faces()[4 * f + j];
                const IndexType u = plan.channel_corners(0)[4 * f + j];
                TASK_CHECK(close(plan.channel_values(0)[u][0],
                                 plan.positions()[g][0]));
                TASK_CHECK(close(plan.channel_values(0)[u][1],
                                 plan.positions()[g][1]));
            }
        TASK_CHECK(check_vs_production(mesh, levels, plan) == 0);
    }
    return 0;
}

int test_stencils_random_fields()
{
    const auto mesh = perturbed_cube();
    SubdivisionPlan::Channel channel{base_uv_values(8), welded_indices(mesh)};
    SubdivisionPlan plan;
    plan.build(mesh, 3, {channel});
    const auto ref = build_ref(mesh, 3, {channel});

    std::mt19937_64 rng(12345);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    for (size_t round = 0; round < 3; ++round)
    {
        std::vector<Point> positions(plan.n_source_vertices());
        for (auto& p : positions)
            p = Point(dist(rng), dist(rng), dist(rng));
        std::vector<TexCoord> values(plan.channel_value_count(0));
        for (auto& t : values)
            t = TexCoord(dist(rng), dist(rng));
        TASK_CHECK(check_eval(plan, ref, positions, {values}) == 0);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// reuse group
// ---------------------------------------------------------------------------

int test_reuse_source_released()
{
    std::vector<Point> positions;
    std::vector<std::vector<TexCoord>> values;
    std::vector<IndexType> vertex_handles;
    SubdivisionPlan plan;
    RefData ref;
    {
        auto mesh = perturbed_plane(2);
        SubdivisionPlan::Channel channel{base_uv_values(9),
                                         welded_indices(mesh)};
        plan.build(mesh, 2, {channel});
        ref = build_ref(mesh, 2, {channel});
        positions = source_positions(mesh, plan);
        values = source_values({channel});
        for (size_t i = 0; i < plan.n_source_vertices(); ++i)
            vertex_handles.push_back(plan.source_vertex(i).idx());
    } // mesh goes out of scope here
    // stored source handles survive the source mesh
    for (size_t i = 0; i < vertex_handles.size(); ++i)
        TASK_CHECK(plan.source_vertex(i).idx() == vertex_handles[i]);
    TASK_CHECK(check_eval(plan, ref, positions, values) == 0);
    return 0;
}

int test_reuse_interleaved_plans()
{
    const auto mesh_a = perturbed_cube();
    const auto mesh_b = perturbed_plane(2);
    SubdivisionPlan::Channel channel_b{base_uv_values(9),
                                       welded_indices(mesh_b)};

    SubdivisionPlan plan_a, plan_b;
    plan_a.build(mesh_a, 2, {});
    plan_b.build(mesh_b, 2, {channel_b});
    const auto ref_a = build_ref(mesh_a, 2, {});
    const auto ref_b = build_ref(mesh_b, 2, {channel_b});

    const auto pos_a1 = source_positions(mesh_a, plan_a);
    const auto pos_b1 = source_positions(mesh_b, plan_b);
    auto pos_a2 = pos_a1;
    for (auto& p : pos_a2)
        p = Point(0.5 * p[0] + 0.1, p[1], -0.3 * p[2]);
    auto values_b2 = source_values({channel_b});
    for (auto& t : values_b2[0])
        t = TexCoord(0.5 * t[0], 0.1 + t[1]);

    // interleave evaluations of both plans
    TASK_CHECK(check_eval(plan_a, ref_a, pos_a1, {}) == 0);
    TASK_CHECK(check_eval(plan_b, ref_b, pos_b1, source_values({channel_b})) ==
               0);
    TASK_CHECK(check_eval(plan_a, ref_a, pos_a2, {}) == 0);
    TASK_CHECK(check_eval(plan_b, ref_b, pos_b1, values_b2) == 0);
    // earlier results are not disturbed by the other plan
    TASK_CHECK(check_eval(plan_a, ref_a, pos_a1, {}) == 0);
    return 0;
}

int test_reuse_repeated_evaluation()
{
    const auto mesh = perturbed_plane(2);
    SubdivisionPlan::Channel channel{base_uv_values(9), welded_indices(mesh)};
    SubdivisionPlan plan;
    plan.build(mesh, 2, {channel});
    const auto ref = build_ref(mesh, 2, {channel});

    const auto positions = source_positions(mesh, plan);
    const auto values = source_values({channel});
    TASK_CHECK(check_eval(plan, ref, positions, values) == 0);
    const auto first_positions = plan.positions();
    const auto first_values = plan.channel_values(0);

    // second evaluation with different control values replaces the results
    auto positions2 = positions;
    for (auto& p : positions2)
        p[2] += 0.37;
    auto values2 = values;
    for (auto& t : values2[0])
        t[0] += 0.11;
    TASK_CHECK(check_eval(plan, ref, positions2, values2) == 0);
    TASK_CHECK(plan.positions() != first_positions);

    // third evaluation with the first values reproduces the first results
    TASK_CHECK(check_eval(plan, ref, positions, values) == 0);
    TASK_CHECK(plan.positions() == first_positions);
    TASK_CHECK(plan.channel_values(0) == first_values);
    return 0;
}

int test_reuse_failure_keeps_results()
{
    const auto mesh = perturbed_plane(2);
    SubdivisionPlan::Channel channel{base_uv_values(9), welded_indices(mesh)};
    SubdivisionPlan plan;
    plan.build(mesh, 2, {channel});
    const auto positions = source_positions(mesh, plan);
    const auto values = source_values({channel});
    plan.evaluate(positions, values);
    const auto saved_positions = plan.positions();
    const auto saved_values = plan.channel_values(0);
    const auto saved_offsets = plan.position_stencil().row_offsets;

    // failing evaluations keep the previous results
    TASK_CHECK(throws_invalid([&] { plan.evaluate({}, {}); }));
    TASK_CHECK(throws_invalid([&] { plan.evaluate(positions, {}); }));
    auto bad_positions = positions;
    bad_positions[3][0] = std::nan("");
    TASK_CHECK(throws_invalid([&] { plan.evaluate(bad_positions, values); }));
    auto short_positions = positions;
    short_positions.pop_back();
    TASK_CHECK(throws_invalid([&] { plan.evaluate(short_positions, values); }));
    auto bad_values = values;
    bad_values[0][2][1] = std::numeric_limits<double>::infinity();
    TASK_CHECK(throws_invalid([&] { plan.evaluate(positions, bad_values); }));
    auto short_values = values;
    short_values[0].pop_back();
    TASK_CHECK(throws_invalid([&] { plan.evaluate(positions, short_values); }));
    TASK_CHECK(plan.positions() == saved_positions);
    TASK_CHECK(plan.channel_values(0) == saved_values);

    // failing builds keep the previous plan
    TASK_CHECK(throws_invalid([&] { plan.build(mesh, 0, {}); }));
    TASK_CHECK(throws_invalid([&] { plan.build(mesh, 6, {}); }));
    SubdivisionPlan::Channel bad_size{base_uv_values(9), {0, 1, 2}};
    TASK_CHECK(throws_invalid([&] { plan.build(mesh, 2, {bad_size}); }));
    auto bad_index = channel;
    bad_index.corner_indices[5] = 42;
    TASK_CHECK(throws_invalid([&] { plan.build(mesh, 2, {bad_index}); }));
    auto bad_uv = channel;
    bad_uv.values[4][0] = std::nan("");
    TASK_CHECK(throws_invalid([&] { plan.build(mesh, 2, {bad_uv}); }));
    auto isolated = plane(1);
    isolated.add_vertex(Point(5, 5, 5));
    TASK_CHECK(throws_invalid([&] { plan.build(isolated, 1, {}); }));
    auto nan_mesh = plane(1);
    nan_mesh.position(Vertex(0)) = Point(std::nan(""), 0, 0);
    TASK_CHECK(throws_invalid([&] { plan.build(nan_mesh, 1, {}); }));

    // the old plan is still intact and evaluates
    TASK_CHECK(plan.is_built());
    TASK_CHECK(plan.levels() == 2);
    TASK_CHECK(plan.position_stencil().row_offsets == saved_offsets);
    TASK_CHECK(plan.positions() == saved_positions);
    plan.evaluate(positions, values);
    TASK_CHECK(plan.positions() == saved_positions);

    // evaluating a default-constructed plan is a logic error
    SubdivisionPlan empty;
    TASK_CHECK(!empty.is_built());
    TASK_CHECK(throws_logic([&] { empty.evaluate({}, {}); }));
    return 0;
}

int test_reuse_sparse_handles()
{
    const auto mesh = perturbed_plane(2);
    SubdivisionPlan::Channel channel{base_uv_values(9), welded_indices(mesh)};
    SubdivisionPlan plan;
    plan.build(mesh, 3, {channel});
    const auto positions = source_positions(mesh, plan);
    const auto values = source_values({channel});
    plan.evaluate(positions, values);

    // position stencil structure
    const auto& s = plan.position_stencil();
    TASK_CHECK(s.rows() == plan.n_vertices());
    TASK_CHECK(s.row_offsets.size() == plan.n_vertices() + 1);
    TASK_CHECK(s.row_offsets.front() == 0);
    TASK_CHECK(s.row_offsets.back() == s.columns.size());
    TASK_CHECK(s.columns.size() == s.weights.size());
    for (size_t i = 0; i < s.rows(); ++i)
    {
        double sum = 0.0;
        for (size_t j = s.row_offsets[i]; j < s.row_offsets[i + 1]; ++j)
        {
            if (j > s.row_offsets[i])
                TASK_CHECK(s.columns[j] > s.columns[j - 1]);
            TASK_CHECK(s.columns[j] < plan.n_source_vertices());
            sum += s.weights[j];
        }
        TASK_CHECK(close(sum, 1.0, 1e-9));
    }
    // manual stencil application reproduces the evaluated positions
    const auto manual = apply_stencil(s, positions);
    TASK_CHECK(manual.size() == plan.positions().size());
    for (size_t i = 0; i < manual.size(); ++i)
        TASK_CHECK(manual[i] == plan.positions()[i]);

    // channel stencil structure and application
    const auto& cs = plan.channel_stencil(0);
    TASK_CHECK(cs.rows() == plan.n_channel_vertices(0));
    for (size_t i = 0; i < cs.rows(); ++i)
    {
        double sum = 0.0;
        for (size_t j = cs.row_offsets[i]; j < cs.row_offsets[i + 1]; ++j)
        {
            if (j > cs.row_offsets[i])
                TASK_CHECK(cs.columns[j] > cs.columns[j - 1]);
            TASK_CHECK(cs.columns[j] < plan.channel_value_count(0));
            sum += cs.weights[j];
        }
        TASK_CHECK(close(sum, 1.0, 1e-9));
    }
    const auto manual_uv = apply_stencil(cs, values[0]);
    for (size_t i = 0; i < manual_uv.size(); ++i)
        TASK_CHECK(manual_uv[i] == plan.channel_values(0)[i]);
    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    {"topology", "compact_mapping_with_garbage",
     &test_topology_compact_mapping_with_garbage},
    {"topology", "components_and_boundary",
     &test_topology_components_and_boundary},
    {"topology", "extraordinary_vertices", &test_topology_extraordinary_vertices},
    {"topology", "features", &test_topology_features},
    {"topology", "channel_seams", &test_topology_channel_seams},
    {"stencils", "single_quad_exact", &test_stencils_single_quad_exact},
    {"stencils", "cube_two_levels", &test_stencils_cube_two_levels},
    {"stencils", "plane_channels_two_rounds",
     &test_stencils_plane_channels_two_rounds},
    {"stencils", "random_fields", &test_stencils_random_fields},
    {"reuse", "source_released", &test_reuse_source_released},
    {"reuse", "interleaved_plans", &test_reuse_interleaved_plans},
    {"reuse", "repeated_evaluation", &test_reuse_repeated_evaluation},
    {"reuse", "failure_keeps_results", &test_reuse_failure_keeps_results},
    {"reuse", "sparse_handles", &test_reuse_sparse_handles},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
