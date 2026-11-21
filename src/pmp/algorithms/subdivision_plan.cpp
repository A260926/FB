// Copyright 2011-2022 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#include "pmp/algorithms/subdivision_plan.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace pmp {

namespace {

constexpr size_t max_subdivision_levels = 5;

[[noreturn]] void fail(const char* what)
{
    throw InvalidInputException(what);
}

bool is_finite(const Point& p)
{
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
}

bool is_finite(const TexCoord& t)
{
    return std::isfinite(t[0]) && std::isfinite(t[1]);
}

// ---------------------------------------------------------------------------
// Sparse rows over source control values
// ---------------------------------------------------------------------------

//! A sparse linear combination of source control values. Columns are sorted
//! and unique, weights sum to one.
struct SparseRow
{
    std::vector<IndexType> cols;
    std::vector<Scalar> weights;
};

//! Accumulates weighted sparse rows and merges duplicate columns.
class RowAccumulator
{
public:
    void add(const SparseRow& row, Scalar w)
    {
        if (w == Scalar(0))
            return;
        for (size_t i = 0; i < row.cols.size(); ++i)
            terms_.emplace_back(row.cols[i], w * row.weights[i]);
    }

    SparseRow finish()
    {
        std::sort(terms_.begin(), terms_.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        SparseRow out;
        for (const auto& term : terms_)
        {
            if (!out.cols.empty() && out.cols.back() == term.first)
                out.weights.back() += term.second;
            else
            {
                out.cols.push_back(term.first);
                out.weights.push_back(term.second);
            }
        }
        size_t n = 0; // drop exact zeros
        for (size_t i = 0; i < out.cols.size(); ++i)
        {
            if (out.weights[i] != Scalar(0))
            {
                out.cols[n] = out.cols[i];
                out.weights[n] = out.weights[i];
                ++n;
            }
        }
        out.cols.resize(n);
        out.weights.resize(n);
        return out;
    }

private:
    std::vector<std::pair<IndexType, Scalar>> terms_;
};

// ---------------------------------------------------------------------------
// One subdivision level: topology plus per-vertex stencils
// ---------------------------------------------------------------------------

struct LevelTopology
{
    std::vector<std::array<IndexType, 2>> edge_vertices; //!< undirected
    std::vector<char> edge_feature;
    std::vector<std::vector<IndexType>> face_vertices;   //!< ccw order
    std::vector<std::vector<IndexType>> face_edges;      //!< edge i: vertex i to i+1
    std::vector<IndexType> face_source;                  //!< compact source face
    std::vector<char> vertex_feature;
    // derived adjacency
    std::vector<std::vector<IndexType>> edge_faces;      //!< one or two
    std::vector<std::vector<IndexType>> vertex_edges;
    std::vector<std::vector<IndexType>> vertex_faces;
    std::vector<char> vertex_boundary;
};

void derive_adjacency(LevelTopology& topo, size_t n_vertices, const char* what)
{
    const size_t n_edges = topo.edge_vertices.size();
    const size_t n_faces = topo.face_vertices.size();
    topo.edge_faces.assign(n_edges, {});
    topo.vertex_edges.assign(n_vertices, {});
    topo.vertex_faces.assign(n_vertices, {});

    for (size_t f = 0; f < n_faces; ++f)
    {
        const auto& fv = topo.face_vertices[f];
        const auto& fe = topo.face_edges[f];
        for (size_t i = 0; i < fv.size(); ++i)
        {
            topo.edge_faces[fe[i]].push_back(IndexType(f));
            topo.vertex_faces[fv[i]].push_back(IndexType(f));
        }
    }
    for (size_t e = 0; e < n_edges; ++e)
    {
        if (topo.edge_faces[e].empty() || topo.edge_faces[e].size() > 2)
            fail(what);
        const auto& ev = topo.edge_vertices[e];
        topo.vertex_edges[ev[0]].push_back(IndexType(e));
        topo.vertex_edges[ev[1]].push_back(IndexType(e));
    }
    topo.vertex_boundary.assign(n_vertices, 0);
    for (size_t v = 0; v < n_vertices; ++v)
    {
        size_t n_boundary = 0;
        for (auto e : topo.vertex_edges[v])
            if (topo.edge_faces[e].size() == 1)
                ++n_boundary;
        if (n_boundary != 0 && n_boundary != 2)
            fail(what);
        topo.vertex_boundary[v] = n_boundary == 2;
    }
}

struct LevelData
{
    LevelTopology topo;
    std::vector<SparseRow> rows; //!< one stencil row per vertex
};

IndexType other_vertex(const LevelTopology& topo, size_t e, IndexType v)
{
    const auto& ev = topo.edge_vertices[e];
    return ev[0] == v ? ev[1] : ev[0];
}

//! One Catmull-Clark refinement step on stencil level.
LevelData refine_level(const LevelData& in)
{
    const auto& topo = in.topo;
    const size_t nv = in.rows.size();
    const size_t ne = topo.edge_vertices.size();
    const size_t nf = topo.face_vertices.size();

    LevelData out;
    out.rows.resize(nv + ne + nf);

    // face points: centroid of corner stencils
    for (size_t f = 0; f < nf; ++f)
    {
        const auto& fv = topo.face_vertices[f];
        const Scalar w = Scalar(1) / Scalar(fv.size());
        RowAccumulator acc;
        for (auto v : fv)
            acc.add(in.rows[v], w);
        out.rows[nv + ne + f] = acc.finish();
    }

    // edge points
    for (size_t e = 0; e < ne; ++e)
    {
        const auto& ev = topo.edge_vertices[e];
        RowAccumulator acc;
        if (topo.edge_faces[e].size() == 1 || topo.edge_feature[e])
        {
            acc.add(in.rows[ev[0]], Scalar(0.5));
            acc.add(in.rows[ev[1]], Scalar(0.5));
        }
        else
        {
            acc.add(in.rows[ev[0]], Scalar(0.25));
            acc.add(in.rows[ev[1]], Scalar(0.25));
            acc.add(out.rows[nv + ne + topo.edge_faces[e][0]], Scalar(0.25));
            acc.add(out.rows[nv + ne + topo.edge_faces[e][1]], Scalar(0.25));
        }
        out.rows[nv + e] = acc.finish();
    }

    // vertex points
    for (size_t v = 0; v < nv; ++v)
    {
        RowAccumulator acc;
        if (topo.vertex_boundary[v])
        {
            // interpolate along the boundary: (6P + prev + next) / 8
            acc.add(in.rows[v], Scalar(0.75));
            for (auto e : topo.vertex_edges[v])
                if (topo.edge_faces[e].size() == 1)
                    acc.add(in.rows[other_vertex(topo, e, IndexType(v))],
                            Scalar(0.125));
        }
        else if (topo.vertex_feature[v])
        {
            std::vector<IndexType> feature_neighbors;
            for (auto e : topo.vertex_edges[v])
                if (topo.edge_feature[e])
                    feature_neighbors.push_back(
                        other_vertex(topo, e, IndexType(v)));
            if (feature_neighbors.size() == 2)
            {
                acc.add(in.rows[v], Scalar(0.75));
                acc.add(in.rows[feature_neighbors[0]], Scalar(0.125));
                acc.add(in.rows[feature_neighbors[1]], Scalar(0.125));
            }
            else // keep fixed
                acc.add(in.rows[v], Scalar(1));
        }
        else
        {
            // smooth interior vertex, weights from "Subdivision Surfaces in
            // Character Animation"
            const Scalar k = Scalar(topo.vertex_edges[v].size());
            const Scalar w = Scalar(1) / (k * k);
            for (auto e : topo.vertex_edges[v])
                acc.add(in.rows[other_vertex(topo, e, IndexType(v))], w);
            for (auto f : topo.vertex_faces[v])
                acc.add(out.rows[nv + ne + f], w);
            acc.add(in.rows[v], (k - Scalar(2)) / k);
        }
        out.rows[v] = acc.finish();
    }

    // refined topology
    auto& nt = out.topo;
    nt.vertex_feature.assign(topo.vertex_feature.begin(),
                             topo.vertex_feature.end());
    for (size_t e = 0; e < ne; ++e)
        nt.vertex_feature.push_back(topo.edge_feature[e]);
    nt.vertex_feature.resize(nv + ne + nf, 0);

    // two split halves per old edge; half 2e touches edge_vertices[e][0]
    nt.edge_vertices.reserve(2 * ne + 2 * ne);
    nt.edge_feature.reserve(2 * ne + 2 * ne);
    for (size_t e = 0; e < ne; ++e)
    {
        const auto& ev = topo.edge_vertices[e];
        nt.edge_vertices.push_back({ev[0], IndexType(nv + e)});
        nt.edge_vertices.push_back({IndexType(nv + e), ev[1]});
        nt.edge_feature.push_back(topo.edge_feature[e]);
        nt.edge_feature.push_back(topo.edge_feature[e]);
    }

    // one quad per old face corner
    for (size_t f = 0; f < nf; ++f)
    {
        const auto& fv = topo.face_vertices[f];
        const auto& fe = topo.face_edges[f];
        const size_t k = fv.size();
        const IndexType face_point = IndexType(nv + ne + f);
        const size_t spoke_base = nt.edge_vertices.size();
        for (size_t i = 0; i < k; ++i)
        {
            nt.edge_vertices.push_back(
                {IndexType(nv + fe[i]), face_point});
            nt.edge_feature.push_back(0);
        }
        for (size_t i = 0; i < k; ++i)
        {
            const size_t ip = (i + k - 1) % k;
            const IndexType vi = fv[i];
            const IndexType ei = IndexType(nv + fe[i]);
            const IndexType ep = IndexType(nv + fe[ip]);
            const IndexType hi = 2 * fe[i] +
                (topo.edge_vertices[fe[i]][0] == vi ? 0 : 1);
            const IndexType hp = 2 * fe[ip] +
                (topo.edge_vertices[fe[ip]][0] == vi ? 0 : 1);
            nt.face_vertices.push_back({vi, ei, face_point, ep});
            nt.face_edges.push_back({hi, IndexType(spoke_base + i),
                                     IndexType(spoke_base + ip), hp});
            nt.face_source.push_back(topo.face_source[f]);
        }
    }

    derive_adjacency(nt, nv + ne + nf,
                     "SubdivisionPlan: non-manifold refined topology");
    return out;
}

// ---------------------------------------------------------------------------
// Level zero construction
// ---------------------------------------------------------------------------

struct SourceMaps
{
    std::vector<Vertex> vertices;
    std::vector<Face> faces;
    std::vector<Halfedge> corners;
    std::vector<size_t> face_corner_offsets;  //!< per face, plus sentinel
    std::vector<IndexType> halfedge_corner;   //!< compact corner per halfedge
};

LevelData geometry_level0(const SurfaceMesh& mesh, SourceMaps& maps)
{
    LevelData out;
    auto& topo = out.topo;

    auto vfeature = mesh.get_vertex_property<bool>("v:feature");
    auto efeature = mesh.get_edge_property<bool>("e:feature");

    std::vector<IndexType> vmap(mesh.vertices_size(), PMP_MAX_INDEX);
    for (auto v : mesh.vertices())
    {
        if (mesh.is_isolated(v))
            fail("SubdivisionPlan: isolated vertices are not supported");
        if (!mesh.is_manifold(v))
            fail("SubdivisionPlan: non-manifold vertex in control cage");
        if (!is_finite(mesh.position(v)))
            fail("SubdivisionPlan: non-finite vertex position");
        vmap[v.idx()] = IndexType(maps.vertices.size());
        maps.vertices.push_back(v);
        SparseRow row;
        row.cols.push_back(vmap[v.idx()]);
        row.weights.push_back(Scalar(1));
        out.rows.push_back(std::move(row));
        topo.vertex_feature.push_back(vfeature && vfeature[v] ? 1 : 0);
    }

    std::vector<IndexType> emap(mesh.edges_size(), PMP_MAX_INDEX);
    for (auto e : mesh.edges())
    {
        emap[e.idx()] = IndexType(topo.edge_vertices.size());
        topo.edge_vertices.push_back(
            {vmap[mesh.vertex(e, 0).idx()], vmap[mesh.vertex(e, 1).idx()]});
        topo.edge_feature.push_back(efeature && efeature[e] ? 1 : 0);
    }

    maps.halfedge_corner.assign(mesh.halfedges_size(), PMP_MAX_INDEX);
    maps.face_corner_offsets.push_back(0);
    for (auto f : mesh.faces())
    {
        std::vector<IndexType> fv, incoming;
        for (auto h : mesh.halfedges(f))
        {
            maps.halfedge_corner[h.idx()] = IndexType(maps.corners.size());
            maps.corners.push_back(h);
            fv.push_back(vmap[mesh.to_vertex(h).idx()]);
            incoming.push_back(emap[mesh.edge(h).idx()]);
        }
        if (fv.size() < 3)
            fail("SubdivisionPlan: degenerate face in control cage");
        // incoming[j] is the edge from vertex j-1 to vertex j; rotate so
        // that face_edges[i] connects vertex i to vertex i+1
        std::vector<IndexType> fe(fv.size());
        for (size_t i = 0; i < fv.size(); ++i)
            fe[i] = incoming[(i + 1) % fv.size()];
        topo.face_vertices.push_back(std::move(fv));
        topo.face_edges.push_back(std::move(fe));
        topo.face_source.push_back(IndexType(maps.faces.size()));
        maps.faces.push_back(f);
        maps.face_corner_offsets.push_back(maps.corners.size());
    }

    derive_adjacency(topo, out.rows.size(),
                     "SubdivisionPlan: non-manifold control cage");
    return out;
}

//! Union-find over source corners for channel welding.
class CornerComponents
{
public:
    explicit CornerComponents(size_t n) : parent_(n)
    {
        for (size_t i = 0; i < n; ++i)
            parent_[i] = IndexType(i);
    }

    IndexType find(IndexType i)
    {
        IndexType r = i;
        while (parent_[r] != r)
            r = parent_[r];
        while (parent_[i] != r)
        {
            IndexType p = parent_[i];
            parent_[i] = r;
            i = p;
        }
        return r;
    }

    void unite(IndexType a, IndexType b)
    {
        a = find(a);
        b = find(b);
        if (a != b)
            parent_[b] = a;
    }

private:
    std::vector<IndexType> parent_;
};

LevelData channel_level0(const SubdivisionPlan::Channel& channel,
                         const SurfaceMesh& mesh, const SourceMaps& maps)
{
    const size_t n_corners = maps.corners.size();
    if (channel.corner_indices.size() != n_corners)
        fail("SubdivisionPlan: channel corner index count mismatch");
    for (auto t : channel.values)
        if (!is_finite(t))
            fail("SubdivisionPlan: non-finite texture coordinate");
    for (auto idx : channel.corner_indices)
        if (idx >= channel.values.size())
            fail("SubdivisionPlan: channel corner index out of range");

    // weld corners across geometric interior edges whenever both endpoints
    // reference the same value index on both sides
    CornerComponents components(n_corners);
    for (auto e : mesh.edges())
    {
        const auto h0 = mesh.halfedge(e, 0);
        const auto h1 = mesh.halfedge(e, 1);
        if (!mesh.face(h0).is_valid() || !mesh.face(h1).is_valid())
            continue; // geometric boundary edge: always a channel boundary
        const IndexType c0 = maps.halfedge_corner[h0.idx()];
        const IndexType c1 = maps.halfedge_corner[h1.idx()];
        const IndexType p0 =
            maps.halfedge_corner[mesh.prev_halfedge(h0).idx()];
        const IndexType p1 =
            maps.halfedge_corner[mesh.prev_halfedge(h1).idx()];
        if (channel.corner_indices[c0] == channel.corner_indices[p1] &&
            channel.corner_indices[c1] == channel.corner_indices[p0])
        {
            components.unite(c0, p1);
            components.unite(c1, p0);
        }
    }

    // compact component ids in corner scan order
    LevelData out;
    std::map<IndexType, IndexType> component_id;
    std::vector<IndexType> corner_vertex(n_corners);
    for (size_t c = 0; c < n_corners; ++c)
    {
        const IndexType root = components.find(IndexType(c));
        auto [it, inserted] =
            component_id.emplace(root, IndexType(component_id.size()));
        if (inserted)
        {
            SparseRow row;
            row.cols.push_back(channel.corner_indices[c]);
            row.weights.push_back(Scalar(1));
            out.rows.push_back(std::move(row));
            out.topo.vertex_feature.push_back(0);
        }
        corner_vertex[c] = it->second;
    }

    // channel faces follow the geometric faces; channel edges are collected
    // per unordered pair of channel vertices in first encounter order
    auto& topo = out.topo;
    std::map<std::pair<IndexType, IndexType>, IndexType> edge_id;
    const size_t n_faces = maps.faces.size();
    for (size_t f = 0; f < n_faces; ++f)
    {
        const size_t begin = maps.face_corner_offsets[f];
        const size_t end = maps.face_corner_offsets[f + 1];
        const size_t k = end - begin;
        std::vector<IndexType> fv(k), fe(k);
        for (size_t i = 0; i < k; ++i)
            fv[i] = corner_vertex[begin + i];
        for (size_t i = 0; i < k; ++i)
        {
            const IndexType a = fv[i];
            const IndexType b = fv[(i + 1) % k];
            if (a == b)
                fail("SubdivisionPlan: degenerate channel edge");
            auto [it, inserted] = edge_id.emplace(
                std::minmax(a, b), IndexType(topo.edge_vertices.size()));
            if (inserted)
            {
                topo.edge_vertices.push_back({a, b});
                topo.edge_feature.push_back(0);
            }
            fe[i] = it->second;
        }
        topo.face_vertices.push_back(std::move(fv));
        topo.face_edges.push_back(std::move(fe));
        topo.face_source.push_back(IndexType(f));
    }

    derive_adjacency(topo, out.rows.size(),
                     "SubdivisionPlan: non-manifold channel topology");
    return out;
}

SubdivisionPlan::Stencil assemble_stencil(const std::vector<SparseRow>& rows)
{
    SubdivisionPlan::Stencil stencil;
    stencil.row_offsets.reserve(rows.size() + 1);
    stencil.row_offsets.push_back(0);
    for (const auto& row : rows)
    {
        stencil.columns.insert(stencil.columns.end(), row.cols.begin(),
                               row.cols.end());
        stencil.weights.insert(stencil.weights.end(), row.weights.begin(),
                               row.weights.end());
        stencil.row_offsets.push_back(stencil.columns.size());
    }
    return stencil;
}

template <typename Vec>
void apply_stencil(const SubdivisionPlan::Stencil& stencil,
                   const std::vector<Vec>& in, std::vector<Vec>& out)
{
    out.assign(stencil.rows(), Vec(Scalar(0)));
    for (size_t i = 0; i < stencil.rows(); ++i)
    {
        Vec acc(Scalar(0));
        for (size_t j = stencil.row_offsets[i]; j < stencil.row_offsets[i + 1];
             ++j)
            acc += stencil.weights[j] * in[stencil.columns[j]];
        out[i] = acc;
    }
}

} // namespace

void SubdivisionPlan::build(const SurfaceMesh& mesh, size_t levels,
                            const std::vector<Channel>& channels)
{
    if (levels < 1 || levels > max_subdivision_levels)
        fail("SubdivisionPlan: levels must be in [1, 5]");

    // build into a fresh plan so a failure keeps the previous one
    SubdivisionPlan plan;
    plan.levels_ = levels;

    SourceMaps maps;
    LevelData level = geometry_level0(mesh, maps);
    for (size_t l = 0; l < levels; ++l)
        level = refine_level(level);

    plan.n_vertices_ = level.rows.size();
    plan.position_stencil_ = assemble_stencil(level.rows);
    plan.face_corners_.reserve(4 * level.topo.face_vertices.size());
    plan.face_sources_ = std::move(level.topo.face_source);
    for (const auto& fv : level.topo.face_vertices)
        for (auto v : fv)
            plan.face_corners_.push_back(v);

    // channels are built before moving the source maps out
    plan.channels_.resize(channels.size());
    for (size_t c = 0; c < channels.size(); ++c)
    {
        LevelData cl = channel_level0(channels[c], mesh, maps);
        for (size_t l = 0; l < levels; ++l)
            cl = refine_level(cl);
        auto& data = plan.channels_[c];
        data.n_source_values = channels[c].values.size();
        data.n_vertices = cl.rows.size();
        data.stencil = assemble_stencil(cl.rows);
        data.corners.reserve(4 * cl.topo.face_vertices.size());
        for (const auto& fv : cl.topo.face_vertices)
            for (auto v : fv)
                data.corners.push_back(v);
    }

    plan.source_vertices_ = std::move(maps.vertices);
    plan.source_faces_ = std::move(maps.faces);
    plan.source_corners_ = std::move(maps.corners);

    plan.built_ = true;
    *this = std::move(plan);
}

void SubdivisionPlan::evaluate(
    const std::vector<Point>& positions,
    const std::vector<std::vector<TexCoord>>& channel_values)
{
    if (!built_)
        throw std::logic_error("SubdivisionPlan::evaluate: plan not built");
    if (positions.size() != source_vertices_.size())
        fail("SubdivisionPlan: position count mismatch");
    for (const auto& p : positions)
        if (!is_finite(p))
            fail("SubdivisionPlan: non-finite vertex position");
    if (channel_values.size() != channels_.size())
        fail("SubdivisionPlan: channel count mismatch");
    for (size_t c = 0; c < channels_.size(); ++c)
    {
        if (channel_values[c].size() != channels_[c].n_source_values)
            fail("SubdivisionPlan: channel value count mismatch");
        for (const auto& t : channel_values[c])
            if (!is_finite(t))
                fail("SubdivisionPlan: non-finite texture coordinate");
    }

    // evaluate into temporaries so a failure keeps the previous results
    std::vector<Point> new_positions;
    apply_stencil(position_stencil_, positions, new_positions);
    std::vector<std::vector<TexCoord>> new_values(channels_.size());
    for (size_t c = 0; c < channels_.size(); ++c)
        apply_stencil(channels_[c].stencil, channel_values[c],
                      new_values[c]);

    positions_ = std::move(new_positions);
    for (size_t c = 0; c < channels_.size(); ++c)
        channels_[c].values = std::move(new_values[c]);
}

} // namespace pmp
