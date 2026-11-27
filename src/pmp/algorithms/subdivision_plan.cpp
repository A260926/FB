// Copyright 2011-2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#include "pmp/algorithms/subdivision_plan.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <utility>

namespace pmp {
namespace {

using CornerId = IndexType;
using VertexId = IndexType;
using FaceId = IndexType;
using Row = std::vector<std::pair<IndexType, Scalar>>;

constexpr IndexType kInvalid = PMP_MAX_INDEX;

// Per-level, channel-independent combinatorial topology.
struct LevelTopology
{
    std::vector<std::vector<VertexId>> faces;
    std::vector<char> vertex_feature;
    std::set<std::pair<VertexId, VertexId>> feature_edges;
};

std::pair<VertexId, VertexId> edge_key(VertexId a, VertexId b)
{
    return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
}

// ---------------------------------------------------------------------------
// Sparse row helpers
// ---------------------------------------------------------------------------

void add_scaled(Row& target, const Row& source, Scalar scale)
{
    target.reserve(target.size() + source.size());
    for (const auto& entry : source)
        target.emplace_back(entry.first, entry.second * scale);
}

// Sort by column, merge equal columns, and drop exact zeros.
void canonicalize(Row& row)
{
    std::sort(row.begin(), row.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    Row merged;
    merged.reserve(row.size());
    for (const auto& entry : row)
    {
        if (!merged.empty() && merged.back().first == entry.first)
            merged.back().second += entry.second;
        else
            merged.emplace_back(entry.first, entry.second);
    }
    merged.erase(
        std::remove_if(merged.begin(), merged.end(),
                       [](const auto& e) { return e.second == Scalar(0); }),
        merged.end());
    row.swap(merged);
}

SparseStencil make_stencil(const std::vector<Row>& rows)
{
    SparseStencil stencil;
    stencil.row_offsets.reserve(rows.size() + 1);
    stencil.row_offsets.push_back(0);
    size_t non_zeros = 0;
    for (const auto& row : rows)
        non_zeros += row.size();
    stencil.column_indices.reserve(non_zeros);
    stencil.weights.reserve(non_zeros);
    for (const auto& row : rows)
    {
        for (const auto& entry : row)
        {
            stencil.column_indices.push_back(entry.first);
            stencil.weights.push_back(entry.second);
        }
        stencil.row_offsets.push_back(
            static_cast<IndexType>(stencil.column_indices.size()));
    }
    return stencil;
}

// ---------------------------------------------------------------------------
// Topology validation
// ---------------------------------------------------------------------------

struct EdgeInfo
{
    VertexId a = kInvalid;
    VertexId b = kInvalid;
    std::vector<FaceId> faces;
    bool feature = false;
};

struct BuiltEdges
{
    std::vector<EdgeInfo> edges;
    std::map<std::pair<VertexId, VertexId>, IndexType> lookup;
};

BuiltEdges build_edges(const std::vector<std::vector<VertexId>>& faces,
                       size_t vertex_count,
                       const std::set<std::pair<VertexId, VertexId>>*
                           feature_edges = nullptr)

{
    const std::set<std::pair<VertexId, VertexId>> no_features;
    const auto& features = feature_edges ? *feature_edges : no_features;
    BuiltEdges result;
    for (FaceId fi = 0; fi < faces.size(); ++fi)
    {
        const auto& face = faces[fi];
        const size_t n = face.size();
        if (n < 3)
            throw InvalidInputException(
                "SubdivisionPlan: face with fewer than three corners.");
        for (size_t i = 0; i < n; ++i)
        {
            const VertexId a = face[i];
            const VertexId b = face[(i + 1) % n];
            if (a >= vertex_count || b >= vertex_count)
                throw InvalidInputException(
                    "SubdivisionPlan: face references an invalid vertex.");
            if (a == b)
                throw InvalidInputException(
                    "SubdivisionPlan: face contains a repeated vertex.");
            const auto key = edge_key(a, b);
            auto it = result.lookup.find(key);
            if (it == result.lookup.end())
            {
                EdgeInfo info;
                info.a = key.first;
                info.b = key.second;
                info.faces.push_back(fi);
                it = result.lookup.emplace(key, result.edges.size()).first;
                result.edges.push_back(info);
            }
            else
            {
                result.edges[it->second].faces.push_back(fi);
            }
        }
    }
    if (feature_edges)
    {
        for (const auto& key : features)
        {
            const auto it = result.lookup.find(key);
            if (it == result.lookup.end())
                throw TopologyException(
                    "SubdivisionPlan: feature edge not present in topology.");
            result.edges[it->second].feature = true;
        }
    }
    return result;
}

void validate_topology(const std::vector<std::vector<VertexId>>& faces,
                       size_t vertex_count)
{
    const BuiltEdges built = build_edges(faces, vertex_count);
    const size_t n_edges = built.edges.size();

    std::vector<std::vector<IndexType>> vertex_edges(vertex_count);
    std::vector<std::vector<FaceId>> vertex_faces(vertex_count);
    std::vector<size_t> boundary_edges(vertex_count, 0);

    for (IndexType ei = 0; ei < n_edges; ++ei)
    {
        const auto& edge = built.edges[ei];
        if (edge.faces.size() > 2)
            throw TopologyException(
                "SubdivisionPlan: edge incident to more than two faces.");
        if (edge.faces.size() == 2 && edge.faces[0] == edge.faces[1])
            throw TopologyException(
                "SubdivisionPlan: face uses one edge twice.");
        vertex_edges[edge.a].push_back(ei);
        vertex_edges[edge.b].push_back(ei);
        if (edge.faces.size() == 1)
        {
            boundary_edges[edge.a]++;
            boundary_edges[edge.b]++;
        }
    }

    for (FaceId fi = 0; fi < faces.size(); ++fi)
    {
        std::set<VertexId> local(faces[fi].begin(), faces[fi].end());
        if (local.size() != faces[fi].size())
            throw InvalidInputException(
                "SubdivisionPlan: face contains a repeated vertex.");
        for (VertexId v : faces[fi])
            vertex_faces[v].push_back(fi);
    }

    for (VertexId v = 0; v < vertex_count; ++v)
    {
        const size_t degree = vertex_edges[v].size();
        if (degree == 0)
            throw InvalidInputException(
                "SubdivisionPlan: isolated vertex is not supported.");
        if (boundary_edges[v] != 0 && boundary_edges[v] != 2)
            throw TopologyException(
                "SubdivisionPlan: non-manifold boundary vertex.");
        const size_t expected_faces =
            boundary_edges[v] == 0 ? degree : degree - 1;
        if (vertex_faces[v].size() != expected_faces)
            throw TopologyException(
                "SubdivisionPlan: non-manifold vertex (disconnected fan).");

        // The faces around the vertex have to form a single fan, connected
        // through shared edges incident to the vertex.
        if (vertex_faces[v].size() > 1)
        {
            std::map<FaceId, FaceId> parent;
            for (FaceId f : vertex_faces[v])
                parent[f] = f;
            std::function<FaceId(FaceId)> find = [&](FaceId f) {
                while (parent[f] != f)
                {
                    parent[f] = parent[parent[f]];
                    f = parent[f];
                }
                return f;
            };
            for (IndexType ei : vertex_edges[v])
            {
                const auto& edge = built.edges[ei];
                if (edge.faces.size() != 2)
                    continue;
                FaceId r0 = find(edge.faces[0]);
                FaceId r1 = find(edge.faces[1]);
                if (r0 != r1)
                    parent[r0] = r1;
            }
            FaceId root = find(vertex_faces[v][0]);
            for (FaceId f : vertex_faces[v])
                if (find(f) != root)
                    throw TopologyException(
                        "SubdivisionPlan: non-manifold vertex (pinched fan).");
        }
    }
}

// ---------------------------------------------------------------------------
// One Catmull-Clark step on an abstract sparse topology.
// ---------------------------------------------------------------------------

void one_step(const LevelTopology& input, const std::vector<Row>& input_rows,
              LevelTopology& output, std::vector<Row>& output_rows)
{
    const size_t n_vertices = input_rows.size();
    if (input.vertex_feature.size() != n_vertices)
        throw TopologyException(
            "SubdivisionPlan: inconsistent vertex feature array.");

    const BuiltEdges built =
        build_edges(input.faces, n_vertices, &input.feature_edges);
    const size_t n_edges = built.edges.size();
    const size_t n_faces = input.faces.size();

    std::vector<std::vector<IndexType>> vertex_edges(n_vertices);
    std::vector<std::vector<FaceId>> vertex_faces(n_vertices);
    for (IndexType ei = 0; ei < n_edges; ++ei)
    {
        const auto& edge = built.edges[ei];
        if (edge.faces.size() > 2)
            throw TopologyException(
                "SubdivisionPlan: edge incident to more than two faces.");
        vertex_edges[edge.a].push_back(ei);
        vertex_edges[edge.b].push_back(ei);
    }
    for (FaceId fi = 0; fi < n_faces; ++fi)
        for (VertexId v : input.faces[fi])
            vertex_faces[v].push_back(fi);

    // Face points.
    std::vector<Row> face_rows(n_faces);
    for (FaceId fi = 0; fi < n_faces; ++fi)
    {
        const auto& face = input.faces[fi];
        const Scalar weight = Scalar(1) / Scalar(face.size());
        for (VertexId v : face)
            add_scaled(face_rows[fi], input_rows[v], weight);
        canonicalize(face_rows[fi]);
    }

    // Edge points.
    std::vector<Row> edge_rows(n_edges);
    for (IndexType ei = 0; ei < n_edges; ++ei)
    {
        const auto& edge = built.edges[ei];
        if (edge.faces.size() == 1 || edge.feature)
        {
            add_scaled(edge_rows[ei], input_rows[edge.a], Scalar(0.5));
            add_scaled(edge_rows[ei], input_rows[edge.b], Scalar(0.5));
        }
        else
        {
            add_scaled(edge_rows[ei], input_rows[edge.a], Scalar(0.25));
            add_scaled(edge_rows[ei], input_rows[edge.b], Scalar(0.25));
            add_scaled(edge_rows[ei], face_rows[edge.faces[0]], Scalar(0.25));
            add_scaled(edge_rows[ei], face_rows[edge.faces[1]], Scalar(0.25));
        }
        canonicalize(edge_rows[ei]);
    }

    // Updated old vertices.
    std::vector<Row> old_rows(n_vertices);
    for (VertexId v = 0; v < n_vertices; ++v)
    {
        std::vector<VertexId> boundary_neighbors;
        std::vector<IndexType> feature_edges;
        for (IndexType ei : vertex_edges[v])
        {
            const auto& edge = built.edges[ei];
            if (edge.faces.size() == 1)
                boundary_neighbors.push_back(edge.a == v ? edge.b : edge.a);
            if (edge.feature)
                feature_edges.push_back(ei);
        }

        if (!boundary_neighbors.empty())
        {
            if (boundary_neighbors.size() != 2)
                throw TopologyException(
                    "SubdivisionPlan: boundary vertex without two boundary "
                    "edges.");
            add_scaled(old_rows[v], input_rows[v], Scalar(6.0 / 8.0));
            add_scaled(old_rows[v], input_rows[boundary_neighbors[0]],
                       Scalar(1.0 / 8.0));
            add_scaled(old_rows[v], input_rows[boundary_neighbors[1]],
                       Scalar(1.0 / 8.0));
        }
        else if (input.vertex_feature[v])
        {
            if (feature_edges.size() == 2)
            {
                add_scaled(old_rows[v], input_rows[v], Scalar(6.0 / 8.0));
                for (IndexType ei : feature_edges)
                {
                    const auto& edge = built.edges[ei];
                    const VertexId other = edge.a == v ? edge.b : edge.a;
                    add_scaled(old_rows[v], input_rows[other],
                               Scalar(1.0 / 8.0));
                }
            }
            else
            {
                add_scaled(old_rows[v], input_rows[v], Scalar(1));
            }
        }
        else
        {
            const size_t k = vertex_edges[v].size();
            if (k < 3)
                throw TopologyException(
                    "SubdivisionPlan: interior vertex with valence below 3.");
            const Scalar inv = Scalar(1) / Scalar(k * k);
            for (IndexType ei : vertex_edges[v])
            {
                const auto& edge = built.edges[ei];
                const VertexId other = edge.a == v ? edge.b : edge.a;
                add_scaled(old_rows[v], input_rows[other], inv);
            }
            for (FaceId fi : vertex_faces[v])
                add_scaled(old_rows[v], face_rows[fi], inv);
            add_scaled(old_rows[v], input_rows[v],
                       Scalar(k - 2) / Scalar(k));
        }
        canonicalize(old_rows[v]);
    }

    // Assemble the next level.
    output.faces.clear();
    output.faces.reserve(n_faces * 3);
    output.vertex_feature.assign(n_vertices + n_edges + n_faces, 0);
    output.feature_edges.clear();
    output_rows.clear();
    output_rows.reserve(n_vertices + n_edges + n_faces);

    for (VertexId v = 0; v < n_vertices; ++v)
    {
        output_rows.push_back(std::move(old_rows[v]));
        output.vertex_feature[v] = input.vertex_feature[v];
    }
    for (IndexType ei = 0; ei < n_edges; ++ei)
    {
        output_rows.push_back(std::move(edge_rows[ei]));
        output.vertex_feature[n_vertices + ei] = built.edges[ei].feature;
    }
    for (FaceId fi = 0; fi < n_faces; ++fi)
    {
        output_rows.push_back(std::move(face_rows[fi]));
        output.vertex_feature[n_vertices + n_edges + fi] = 0;
    }

    for (FaceId fi = 0; fi < n_faces; ++fi)
    {
        const auto& face = input.faces[fi];
        const size_t n = face.size();
        for (size_t i = 0; i < n; ++i)
        {
            const VertexId v0 = face[i];
            const VertexId v1 = face[(i + 1) % n];
            const VertexId v2 = face[(i + 2) % n];
            const IndexType e0 = built.lookup.at(edge_key(v0, v1));
            const IndexType e1 = built.lookup.at(edge_key(v1, v2));
            const VertexId ep0 = static_cast<VertexId>(n_vertices + e0);
            const VertexId ep1 = static_cast<VertexId>(n_vertices + e1);
            const VertexId fp =
                static_cast<VertexId>(n_vertices + n_edges + fi);
            output.faces.push_back({ep0, v1, ep1, fp});

            if (built.edges[e0].feature)
                output.feature_edges.insert(edge_key(ep0, v1));
            if (built.edges[e1].feature)
                output.feature_edges.insert(edge_key(v1, ep1));
        }
    }
}

// ---------------------------------------------------------------------------
// Disjoint set for channel seam splitting.
// ---------------------------------------------------------------------------

class DisjointSet
{
public:
    explicit DisjointSet(size_t n) : parent_(n)
    {
        for (size_t i = 0; i < n; ++i)
            parent_[i] = static_cast<IndexType>(i);
    }

    IndexType find(IndexType a)
    {
        IndexType root = a;
        while (parent_[root] != root)
            root = parent_[root];
        while (parent_[a] != root)
        {
            const IndexType next = parent_[a];
            parent_[a] = root;
            a = next;
        }
        return root;
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

} // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

SubdivisionPlan::SubdivisionPlan(
    const SurfaceMesh& mesh, unsigned levels,
    const std::vector<SubdivisionPlanChannel>& channels)
{
    SubdivisionPlan build;
    build.build_impl(mesh, levels, channels);
    *this = std::move(build);
}

void SubdivisionPlan::rebuild(
    const SurfaceMesh& mesh, unsigned levels,
    const std::vector<SubdivisionPlanChannel>& channels)
{
    SubdivisionPlan build;
    build.build_impl(mesh, levels, channels);
    *this = std::move(build);
}

void SubdivisionPlan::build_impl(
    const SurfaceMesh& mesh, unsigned levels,
    const std::vector<SubdivisionPlanChannel>& channels)
{
    *this = SubdivisionPlan();

    if (levels < 1 || levels > max_levels)
        throw InvalidInputException(
            "SubdivisionPlan: levels must be in [1, 5].");
    levels_ = levels;

    // ---- Compact source vertices -------------------------------------------
    source_vertex_index_.clear();
    vertex_lookup_.assign(mesh.vertices_size(), invalid_index);
    for (auto v : mesh.vertices())
    {
        if (mesh.is_deleted(v))
            continue;
        if (mesh.is_isolated(v))
            throw InvalidInputException(
                "SubdivisionPlan: isolated vertex is not supported.");
        const Point& p = mesh.position(v);
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) ||
            !std::isfinite(p[2]))
            throw InvalidInputException(
                "SubdivisionPlan: non-finite vertex position.");
        if (source_vertex_index_.size() >= invalid_index)
            throw AllocationException("SubdivisionPlan: too many vertices.");
        const IndexType id =
            static_cast<IndexType>(source_vertex_index_.size());
        vertex_lookup_[v.idx()] = id;
        source_vertex_index_.push_back(v.idx());
    }
    source_vertex_count_ = source_vertex_index_.size();
    if (source_vertex_count_ == 0)
        throw InvalidInputException("SubdivisionPlan: empty input mesh.");

    // ---- Compact faces and corners -----------------------------------------
    source_face_index_.clear();
    face_lookup_.assign(mesh.faces_size(), invalid_index);
    corner_halfedge_.clear();
    corner_face_.clear();
    corner_vertex_.clear();
    corner_in_face_.clear();
    corner_face_offsets_.clear();
    halfedge_lookup_.assign(mesh.halfedges_size(), invalid_index);
    source_faces_.clear();

    for (auto f : mesh.faces())
    {
        if (mesh.is_deleted(f))
            continue;
        const IndexType fid = static_cast<IndexType>(source_face_index_.size());
        face_lookup_[f.idx()] = fid;
        source_face_index_.push_back(f.idx());

        corner_face_offsets_.push_back(corner_halfedge_.size());
        std::vector<VertexId> face;
        IndexType local = 0;
        for (auto h : mesh.halfedges(f))
        {
            const VertexId vid = vertex_lookup_[mesh.to_vertex(h).idx()];
            if (vid == invalid_index)
                throw TopologyException(
                    "SubdivisionPlan: face uses an invalid vertex.");
            face.push_back(vid);

            const IndexType cid =
                static_cast<IndexType>(corner_halfedge_.size());
            halfedge_lookup_[h.idx()] = cid;
            corner_halfedge_.push_back(h.idx());
            corner_face_.push_back(fid);
            corner_vertex_.push_back(vid);
            corner_in_face_.push_back(local);
            ++local;
        }
        if (face.size() < 3)
            throw InvalidInputException(
                "SubdivisionPlan: face with fewer than three corners.");
        std::set<VertexId> unique(face.begin(), face.end());
        if (unique.size() != face.size())
            throw InvalidInputException(
                "SubdivisionPlan: face contains a repeated vertex.");
        source_faces_.push_back(std::move(face));
    }
    corner_face_offsets_.push_back(corner_halfedge_.size());
    source_face_count_ = source_faces_.size();
    source_corner_count_ = corner_halfedge_.size();
    if (source_face_count_ == 0)
        throw InvalidInputException("SubdivisionPlan: mesh has no faces.");
    if (source_corner_count_ >= invalid_index)
        throw AllocationException("SubdivisionPlan: too many corners.");

    // Manifoldness of the source geometry.
    validate_topology(source_faces_, source_vertex_count_);

    // ---- Source features ----------------------------------------------------
    auto vfeature = mesh.get_vertex_property<bool>("v:feature");
    auto efeature = mesh.get_edge_property<bool>("e:feature");

    LevelTopology geometry;
    geometry.faces = source_faces_;
    geometry.vertex_feature.assign(source_vertex_count_, 0);
    if (vfeature)
        for (VertexId v = 0; v < source_vertex_count_; ++v)
        {
            const Vertex original(
                static_cast<IndexType>(source_vertex_index_[v]));
            geometry.vertex_feature[v] = vfeature[original] ? 1 : 0;
        }
    if (efeature)
    {
        for (auto e : mesh.edges())
        {
            if (mesh.is_deleted(e) || !efeature[e])
                continue;
            const VertexId a =
                vertex_lookup_[mesh.to_vertex(mesh.halfedge(e, 0)).idx()];
            const VertexId b =
                vertex_lookup_[mesh.to_vertex(mesh.halfedge(e, 1)).idx()];
            if (a != invalid_index && b != invalid_index && a != b)
                geometry.feature_edges.insert(edge_key(a, b));
        }
    }

    std::vector<Row> geometry_rows(source_vertex_count_);
    for (VertexId v = 0; v < source_vertex_count_; ++v)
        geometry_rows[v] = {{v, Scalar(1)}};

    for (unsigned level = 0; level < levels_; ++level)
    {
        LevelTopology next;
        std::vector<Row> next_rows;
        one_step(geometry, geometry_rows, next, next_rows);
        geometry = std::move(next);
        geometry_rows = std::move(next_rows);
    }

    faces_ = std::move(geometry.faces);
    result_vertex_count_ = geometry_rows.size();
    position_stencil_ = make_stencil(geometry_rows);

    // Result face provenance: every step maps a parent face to one child per
    // parent corner, and each child is a quad.
    {
        std::vector<IndexType> parent_source(source_face_count_);
        std::vector<size_t> parent_valence(source_face_count_);
        for (FaceId f = 0; f < source_face_count_; ++f)
        {
            parent_source[f] = f;
            parent_valence[f] = source_faces_[f].size();
        }
        for (unsigned level = 0; level < levels_; ++level)
        {
            std::vector<IndexType> next_source;
            std::vector<size_t> next_valence;
            next_source.reserve(parent_source.size() * 3);
            next_valence.reserve(parent_source.size() * 3);
            for (size_t f = 0; f < parent_source.size(); ++f)
            {
                for (size_t i = 0; i < parent_valence[f]; ++i)
                {
                    next_source.push_back(parent_source[f]);
                    next_valence.push_back(4);
                }
            }
            parent_source = std::move(next_source);
            parent_valence = std::move(next_valence);
        }
        if (parent_source.size() != faces_.size())
            throw TopologyException(
                "SubdivisionPlan: face provenance mismatch.");
        face_sources_ = std::move(parent_source);
    }

    // ---- Texture channels ---------------------------------------------------
    channel_names_.clear();
    channel_source_value_indices_.clear();
    channel_source_corner_vertices_.clear();
    channel_control_vertex_count_.clear();
    channel_source_value_count_.clear();
    channel_stencils_.clear();
    channel_corner_indices_.clear();

    for (size_t c = 0; c < channels.size(); ++c)
    {
        const auto& channel = channels[c];
        channel_names_.push_back(channel.name);
        if (channel.corner_values.size() != mesh.halfedges_size())
            throw InvalidInputException(
                "SubdivisionPlan: channel corner_values has to match the "
                "halfedge array size.");
        channel_source_value_count_.push_back(channel.values.size());
        for (const TexCoord& value : channel.values)
            if (!std::isfinite(value[0]) || !std::isfinite(value[1]))
                throw InvalidInputException(
                    "SubdivisionPlan: non-finite channel value.");

        std::vector<IndexType> value_indices(source_corner_count_);
        for (CornerId cid = 0; cid < source_corner_count_; ++cid)
        {
            const IndexType index =
                channel.corner_values[corner_halfedge_[cid]];
            if (index >= channel.values.size())
                throw InvalidInputException(
                    "SubdivisionPlan: channel corner index out of range.");
            value_indices[cid] = index;
        }

        // Union corners across interior edges only when both endpoints agree
        // on the value index. Values and positions are never used here.
        DisjointSet dsu(source_corner_count_);
        for (auto e : mesh.edges())
        {
            if (mesh.is_deleted(e))
                continue;
            const Halfedge h0 = mesh.halfedge(e, 0);
            const Halfedge h1 = mesh.halfedge(e, 1);
            const Face face0 = mesh.face(h0);
            const Face face1 = mesh.face(h1);
            if (!face0.is_valid() || !face1.is_valid())
                continue;

            // h0 runs a -> b and h1 = opposite(h0) runs b -> a.
            // The corner at b in face(h0) is h0 itself; the corner at b in
            // face(h1) is prev(h1). Symmetrically the corner at a in face(h0)
            // is prev(h0) and the corner at a in face(h1) is h1.
            const CornerId corner_b0 = halfedge_lookup_[h0.idx()];
            const CornerId corner_b1 =
                halfedge_lookup_[mesh.prev_halfedge(h1).idx()];
            const CornerId corner_a0 =
                halfedge_lookup_[mesh.prev_halfedge(h0).idx()];
            const CornerId corner_a1 = halfedge_lookup_[h1.idx()];
            if (corner_a0 == invalid_index || corner_a1 == invalid_index ||
                corner_b0 == invalid_index || corner_b1 == invalid_index)
                throw TopologyException(
                    "SubdivisionPlan: inconsistent corner layout.");

            if (value_indices[corner_a0] == value_indices[corner_a1] &&
                value_indices[corner_b0] == value_indices[corner_b1])
            {
                dsu.unite(corner_a0, corner_a1);
                dsu.unite(corner_b0, corner_b1);
            }
        }

        // Deterministic channel control vertex numbering.
        std::vector<IndexType> root_to_vertex(source_corner_count_,
                                              invalid_index);
        std::vector<IndexType> corner_vertices(source_corner_count_);
        IndexType n_control = 0;
        for (CornerId cid = 0; cid < source_corner_count_; ++cid)
        {
            const IndexType root = dsu.find(cid);
            if (root_to_vertex[root] == invalid_index)
                root_to_vertex[root] =
                    static_cast<IndexType>(n_control++);
            corner_vertices[cid] = root_to_vertex[root];
        }

        // Representative source corner used as stencil column.
        std::vector<IndexType> representative(n_control, invalid_index);
        for (CornerId cid = 0; cid < source_corner_count_; ++cid)
            if (representative[corner_vertices[cid]] == invalid_index)
                representative[corner_vertices[cid]] = cid;

        // Channel topology shares the source face and corner layout, but uses
        // the seam-split control vertices.
        LevelTopology channel_topo;
        channel_topo.vertex_feature.assign(n_control, 0);
        channel_topo.faces.reserve(source_face_count_);
        for (FaceId fid = 0; fid < source_face_count_; ++fid)
        {
            std::vector<VertexId> face;
            face.reserve(source_faces_[fid].size());
            const size_t begin = corner_face_offsets_[fid];
            const size_t end = corner_face_offsets_[fid + 1];
            for (size_t cid = begin; cid < end; ++cid)
                face.push_back(corner_vertices[cid]);
            channel_topo.faces.push_back(std::move(face));
        }

        // Validate the cut channel topology before subdivision.
        validate_topology(channel_topo.faces, n_control);

        // Channel control vertices start out carrying the build-time value
        // index of their representative corner, so the resulting channel
        // stencil columns are indices into the channel value table.
        std::vector<Row> channel_rows(n_control);
        for (IndexType cv = 0; cv < n_control; ++cv)
            channel_rows[cv] = {{value_indices[representative[cv]], Scalar(1)}};

        for (unsigned level = 0; level < levels_; ++level)
        {
            LevelTopology next;
            std::vector<Row> next_rows;
            one_step(channel_topo, channel_rows, next, next_rows);
            channel_topo = std::move(next);
            channel_rows = std::move(next_rows);
        }

        if (channel_topo.faces.size() != faces_.size())
            throw TopologyException(
                "SubdivisionPlan: channel/geometry face mismatch.");
        for (size_t fi = 0; fi < faces_.size(); ++fi)
        {
            if (channel_topo.faces[fi].size() != faces_[fi].size())
                throw TopologyException(
                    "SubdivisionPlan: channel/geometry corner mismatch.");
            const auto& result_face = channel_topo.faces[fi];
            for (VertexId v : result_face)
                if (v >= channel_rows.size())
                    throw TopologyException(
                        "SubdivisionPlan: channel result vertex out of range.");
        }

        channel_source_value_indices_.push_back(std::move(value_indices));
        channel_source_corner_vertices_.push_back(std::move(corner_vertices));
        channel_control_vertex_count_.push_back(n_control);
        channel_stencils_.push_back(make_stencil(channel_rows));
        channel_corner_indices_.push_back(std::move(channel_topo.faces));
    }

    built_ = true;

    // Seed the evaluation result from the construction input.
    std::vector<Point> source_positions(source_vertex_count_);
    for (VertexId v = 0; v < source_vertex_count_; ++v)
    {
        const Vertex original(static_cast<IndexType>(source_vertex_index_[v]));
        source_positions[v] = mesh.position(original);
    }
    std::vector<std::vector<TexCoord>> tables(channels.size());
    for (size_t c = 0; c < channels.size(); ++c)
        tables[c] = channels[c].values;
    evaluate(source_positions, tables);
}

void SubdivisionPlan::evaluate(
    const std::vector<Point>& source_positions,
    const std::vector<std::vector<TexCoord>>& channel_value_tables)
{
    if (!built_)
        throw InvalidInputException("SubdivisionPlan: plan is not built.");
    if (source_positions.size() != source_vertex_count_)
        throw InvalidInputException(
            "SubdivisionPlan: source position count mismatch.");
    for (const Point& p : source_positions)
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) ||
            !std::isfinite(p[2]))
            throw InvalidInputException(
                "SubdivisionPlan: non-finite source position.");
    if (channel_value_tables.size() != channel_stencils_.size())
        throw InvalidInputException(
            "SubdivisionPlan: channel value table count mismatch.");
    for (size_t c = 0; c < channel_value_tables.size(); ++c)
    {
        if (channel_value_tables[c].size() != channel_source_value_count_[c])
            throw InvalidInputException(
                "SubdivisionPlan: channel value table length mismatch.");
        for (const TexCoord& value : channel_value_tables[c])
            if (!std::isfinite(value[0]) || !std::isfinite(value[1]))
                throw InvalidInputException(
                    "SubdivisionPlan: non-finite channel value.");
    }

    std::vector<Point> new_positions(result_vertex_count_);
    for (size_t row = 0; row < result_vertex_count_; ++row)
    {
        Point p(0, 0, 0);
        for (IndexType k = position_stencil_.row_offsets[row];
             k < position_stencil_.row_offsets[row + 1]; ++k)
            p += position_stencil_.weights[k] *
                 source_positions[position_stencil_.column_indices[k]];
        new_positions[row] = p;
    }

    std::vector<std::vector<TexCoord>> new_channel_values(
        channel_stencils_.size());
    for (size_t c = 0; c < channel_stencils_.size(); ++c)
    {
        const auto& stencil = channel_stencils_[c];
        const auto& table = channel_value_tables[c];
        auto& values = new_channel_values[c];
        values.resize(stencil.rows());
        for (size_t row = 0; row < stencil.rows(); ++row)
        {
            TexCoord value(0, 0);
            for (IndexType k = stencil.row_offsets[row];
                 k < stencil.row_offsets[row + 1]; ++k)
                value += stencil.weights[k] * table[stencil.column_indices[k]];
            values[row] = value;
        }
    }

    positions_.swap(new_positions);
    channel_values_.swap(new_channel_values);
}


// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

const std::string& SubdivisionPlan::channel_name(size_t channel) const
{
    if (channel >= channel_names_.size())
        throw InvalidInputException(
            "SubdivisionPlan: channel index out of range.");
    return channel_names_[channel];
}

IndexType SubdivisionPlan::source_vertex_id(Vertex v) const
{
    if (!v.is_valid() || v.idx() >= vertex_lookup_.size())
        return invalid_index;
    return vertex_lookup_[v.idx()];
}

IndexType SubdivisionPlan::source_face_id(Face f) const
{
    if (!f.is_valid() || f.idx() >= face_lookup_.size())
        return invalid_index;
    return face_lookup_[f.idx()];
}

IndexType SubdivisionPlan::source_corner_id(Halfedge h) const
{
    if (!h.is_valid() || h.idx() >= halfedge_lookup_.size())
        return invalid_index;
    return halfedge_lookup_[h.idx()];
}

Vertex SubdivisionPlan::source_vertex(IndexType id) const
{
    if (id >= source_vertex_index_.size())
        return Vertex();
    return Vertex(source_vertex_index_[id]);
}

Face SubdivisionPlan::source_face(IndexType id) const
{
    if (id >= source_face_index_.size())
        return Face();
    return Face(source_face_index_[id]);
}

Halfedge SubdivisionPlan::source_corner_halfedge(IndexType id) const
{
    if (id >= corner_halfedge_.size())
        return Halfedge();
    return Halfedge(corner_halfedge_[id]);
}

IndexType SubdivisionPlan::source_corner_face(IndexType id) const
{
    if (id >= corner_face_.size())
        return invalid_index;
    return corner_face_[id];
}

IndexType SubdivisionPlan::source_corner_vertex(IndexType id) const
{
    if (id >= corner_vertex_.size())
        return invalid_index;
    return corner_vertex_[id];
}

IndexType SubdivisionPlan::source_corner_in_face(IndexType id) const
{
    if (id >= corner_in_face_.size())
        return invalid_index;
    return corner_in_face_[id];
}

void SubdivisionPlan::require_built() const
{
    if (!built_)
        throw InvalidInputException("SubdivisionPlan: plan is not built.");
}

size_t SubdivisionPlan::vertex_count() const
{
    require_built();
    return result_vertex_count_;
}

size_t SubdivisionPlan::channel_value_count(size_t channel) const
{
    require_built();
    if (channel >= channel_stencils_.size())
        throw InvalidInputException(
            "SubdivisionPlan: channel index out of range.");
    return channel_stencils_[channel].rows();
}

size_t SubdivisionPlan::channel_control_vertex_count(size_t channel) const
{
    require_built();
    if (channel >= channel_control_vertex_count_.size())
        throw InvalidInputException(
            "SubdivisionPlan: channel index out of range.");
    return channel_control_vertex_count_[channel];
}

const std::vector<IndexType>&
SubdivisionPlan::channel_source_value_indices(size_t channel) const
{
    if (channel >= channel_source_value_indices_.size())
        throw InvalidInputException(
            "SubdivisionPlan: channel index out of range.");
    return channel_source_value_indices_[channel];
}

const std::vector<IndexType>&
SubdivisionPlan::channel_source_corner_vertices(size_t channel) const
{
    if (channel >= channel_source_corner_vertices_.size())
        throw InvalidInputException(
            "SubdivisionPlan: channel index out of range.");
    return channel_source_corner_vertices_[channel];
}

const SparseStencil& SubdivisionPlan::channel_stencil(size_t channel) const
{
    if (channel >= channel_stencils_.size())
        throw InvalidInputException(
            "SubdivisionPlan: channel index out of range.");
    return channel_stencils_[channel];
}

const std::vector<std::vector<IndexType>>&
SubdivisionPlan::channel_corner_indices(size_t channel) const
{
    if (channel >= channel_corner_indices_.size())
        throw InvalidInputException(
            "SubdivisionPlan: channel index out of range.");
    return channel_corner_indices_[channel];
}

const std::vector<TexCoord>&
SubdivisionPlan::channel_values(size_t channel) const
{
    if (channel >= channel_values_.size())
        throw InvalidInputException(
            "SubdivisionPlan: channel index out of range.");
    return channel_values_[channel];
}

} // namespace pmp
