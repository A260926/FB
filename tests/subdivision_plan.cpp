// Independent tests for pmp::SubdivisionPlan.
//
// The reference model below re-implements Catmull-Clark subdivision on plain
// arrays. It does not call SubdivisionPlan, the in-place subdivision in
// pmp/algorithms/subdivision.cpp, or any sparse stencil code. Positions and
// UV channels are carried as scalar tuples and compared round by round.
//
// The reference topology is extracted from the input SurfaceMesh in exactly
// the same face/corner order the plan uses, so rows can be compared one by
// one. The arithmetic itself is written out independently.

#include "pmp/algorithms/subdivision_plan.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "task_support/check.h"

using namespace pmp;

namespace {

constexpr double kTolerance = 1e-7;
constexpr IndexType kBad = PMP_MAX_INDEX;

bool close(Scalar a, Scalar b, double eps = kTolerance)
{
    return std::fabs(double(a) - double(b)) <= eps;
}

bool finite(const Point& p)
{
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
}

// ---------------------------------------------------------------------------
// Reference topology: compact faces, features, per-vertex attribute rows.
// ---------------------------------------------------------------------------

struct RefMesh
{
    std::vector<std::vector<IndexType>> faces;
    std::vector<char> vertex_feature;
    std::set<std::pair<IndexType, IndexType>> feature_edges;
};

using AttrRow = std::vector<Scalar>;
using AttrTable = std::vector<AttrRow>;

std::pair<IndexType, IndexType> key_of(IndexType a, IndexType b)
{
    return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
}

// Compact vertex id per original vertex index, in mesh.vertices() order.
std::vector<IndexType> compact_vertices(const SurfaceMesh& mesh)
{
    std::vector<IndexType> map(mesh.vertices_size(), kBad);
    IndexType next = 0;
    for (auto v : mesh.vertices())
        if (!mesh.is_deleted(v))
            map[v.idx()] = next++;
    return map;
}

// Build the reference topology from the mesh, using the plan's corner order:
// faces in mesh.faces() order, corners from mesh.halfedges(f) (i.e. corners at
// to_vertex in that cyclic order).
RefMesh ref_mesh_from(const SurfaceMesh& mesh)
{
    const std::vector<IndexType> map = compact_vertices(mesh);
    RefMesh ref;
    for (auto f : mesh.faces())
    {
        std::vector<IndexType> face;
        for (auto h : mesh.halfedges(f))
            face.push_back(map[mesh.to_vertex(h).idx()]);
        ref.faces.push_back(std::move(face));
    }
    size_t count = 0;
    for (auto id : map)
        if (id != kBad)
            count = std::max(count, size_t(id) + 1);
    ref.vertex_feature.assign(count, 0);

    auto vfeature = mesh.get_vertex_property<bool>("v:feature");
    if (vfeature)
        for (auto v : mesh.vertices())
            if (!mesh.is_deleted(v) && vfeature[v])
                ref.vertex_feature[map[v.idx()]] = 1;
    auto efeature = mesh.get_edge_property<bool>("e:feature");
    if (efeature)
        for (auto e : mesh.edges())
        {
            if (mesh.is_deleted(e) || !efeature[e])
                continue;
            const IndexType a = map[mesh.to_vertex(mesh.halfedge(e, 0)).idx()];
            const IndexType b = map[mesh.to_vertex(mesh.halfedge(e, 1)).idx()];
            if (a != kBad && b != kBad)
                ref.feature_edges.insert(key_of(a, b));
        }
    return ref;
}

struct RefEdges
{
    std::vector<IndexType> a;
    std::vector<IndexType> b;
    std::vector<std::vector<IndexType>> faces;
    std::vector<char> feature;
    std::map<std::pair<IndexType, IndexType>, IndexType> lookup;
};

RefEdges ref_edges(const RefMesh& mesh)
{
    RefEdges edges;
    for (IndexType fi = 0; fi < IndexType(mesh.faces.size()); ++fi)
    {
        const auto& face = mesh.faces[fi];
        const size_t n = face.size();
        for (size_t i = 0; i < n; ++i)
        {
            const IndexType u = face[i];
            const IndexType v = face[(i + 1) % n];
            const auto key = key_of(u, v);
            auto it = edges.lookup.find(key);
            if (it == edges.lookup.end())
            {
                const IndexType ei = IndexType(edges.a.size());
                edges.a.push_back(key.first);
                edges.b.push_back(key.second);
                edges.faces.push_back({fi});
                edges.feature.push_back(0);
                edges.lookup.emplace(key, ei);
            }
            else
            {
                edges.faces[it->second].push_back(fi);
            }
        }
    }
    for (const auto& key : mesh.feature_edges)
    {
        auto it = edges.lookup.find(key);
        if (it != edges.lookup.end())
            edges.feature[it->second] = 1;
    }
    return edges;
}

void ref_add(AttrRow& target, const AttrRow& source, Scalar s)
{
    for (size_t i = 0; i < target.size(); ++i)
        target[i] += source[i] * s;
}

// One Catmull-Clark round on scalar rows. Mirrors the plan's arithmetic, but
// written independently.
RefMesh ref_step(const RefMesh& input, const AttrTable& attr_in,
                 AttrTable& attr_out)
{
    const size_t nv = attr_in.size();
    const RefEdges edges = ref_edges(input);
    const size_t ne = edges.a.size();
    const size_t nf = input.faces.size();
    const size_t width = attr_in.empty() ? 0 : attr_in[0].size();

    std::vector<std::vector<IndexType>> vertex_edges(nv);
    std::vector<std::vector<IndexType>> vertex_faces(nv);
    for (IndexType ei = 0; ei < IndexType(ne); ++ei)
    {
        vertex_edges[edges.a[ei]].push_back(ei);
        vertex_edges[edges.b[ei]].push_back(ei);
    }
    for (IndexType fi = 0; fi < IndexType(nf); ++fi)
        for (IndexType v : input.faces[fi])
            vertex_faces[v].push_back(fi);

    AttrTable face_attr(nf, AttrRow(width, 0));
    for (IndexType fi = 0; fi < IndexType(nf); ++fi)
    {
        const auto& face = input.faces[fi];
        for (IndexType v : face)
            ref_add(face_attr[fi], attr_in[v], Scalar(1) / Scalar(face.size()));
    }

    AttrTable edge_attr(ne, AttrRow(width, 0));
    for (IndexType ei = 0; ei < IndexType(ne); ++ei)
    {
        if (edges.faces[ei].size() == 1 || edges.feature[ei])
        {
            ref_add(edge_attr[ei], attr_in[edges.a[ei]], 0.5);
            ref_add(edge_attr[ei], attr_in[edges.b[ei]], 0.5);
        }
        else
        {
            ref_add(edge_attr[ei], attr_in[edges.a[ei]], 0.25);
            ref_add(edge_attr[ei], attr_in[edges.b[ei]], 0.25);
            ref_add(edge_attr[ei], face_attr[edges.faces[ei][0]], 0.25);
            ref_add(edge_attr[ei], face_attr[edges.faces[ei][1]], 0.25);
        }
    }

    AttrTable old_attr(nv, AttrRow(width, 0));
    for (IndexType v = 0; v < IndexType(nv); ++v)
    {
        std::vector<IndexType> boundary;
        std::vector<IndexType> features;
        for (IndexType ei : vertex_edges[v])
        {
            if (edges.faces[ei].size() == 1)
                boundary.push_back(edges.a[ei] == v ? edges.b[ei] : edges.a[ei]);
            if (edges.feature[ei])
                features.push_back(ei);
        }
        if (!boundary.empty())
        {
            ref_add(old_attr[v], attr_in[v], 6.0 / 8.0);
            ref_add(old_attr[v], attr_in[boundary[0]], 1.0 / 8.0);
            ref_add(old_attr[v], attr_in[boundary[1]], 1.0 / 8.0);
        }
        else if (input.vertex_feature[v])
        {
            if (features.size() == 2)
            {
                ref_add(old_attr[v], attr_in[v], 6.0 / 8.0);
                for (IndexType ei : features)
                {
                    const IndexType other =
                        edges.a[ei] == v ? edges.b[ei] : edges.a[ei];
                    ref_add(old_attr[v], attr_in[other], 1.0 / 8.0);
                }
            }
            else
            {
                ref_add(old_attr[v], attr_in[v], 1.0);
            }
        }
        else
        {
            const Scalar k = Scalar(vertex_edges[v].size());
            for (IndexType ei : vertex_edges[v])
            {
                const IndexType other =
                    edges.a[ei] == v ? edges.b[ei] : edges.a[ei];
                ref_add(old_attr[v], attr_in[other], 1.0 / (k * k));
            }
            for (IndexType fi : vertex_faces[v])
                ref_add(old_attr[v], face_attr[fi], 1.0 / (k * k));
            ref_add(old_attr[v], attr_in[v], (k - 2.0) / k);
        }
    }

    RefMesh output;
    output.vertex_feature.assign(nv + ne + nf, 0);
    attr_out.assign(nv + ne + nf, AttrRow(width, 0));
    for (IndexType v = 0; v < IndexType(nv); ++v)
    {
        output.vertex_feature[v] = input.vertex_feature[v];
        attr_out[v] = old_attr[v];
    }
    for (IndexType ei = 0; ei < IndexType(ne); ++ei)
    {
        output.vertex_feature[nv + ei] = edges.feature[ei];
        attr_out[nv + ei] = edge_attr[ei];
    }
    for (IndexType fi = 0; fi < IndexType(nf); ++fi)
        attr_out[nv + ne + fi] = face_attr[fi];

    for (IndexType fi = 0; fi < IndexType(nf); ++fi)
    {
        const auto& face = input.faces[fi];
        const size_t n = face.size();
        for (size_t i = 0; i < n; ++i)
        {
            const IndexType v0 = face[i];
            const IndexType v1 = face[(i + 1) % n];
            const IndexType v2 = face[(i + 2) % n];
            const IndexType e0 = edges.lookup.at(key_of(v0, v1));
            const IndexType e1 = edges.lookup.at(key_of(v1, v2));
            const IndexType ep0 = IndexType(nv + e0);
            const IndexType ep1 = IndexType(nv + e1);
            const IndexType fp = IndexType(nv + ne + fi);
            output.faces.push_back({ep0, v1, ep1, fp});
            if (edges.feature[e0])
                output.feature_edges.insert(key_of(ep0, v1));
            if (edges.feature[e1])
                output.feature_edges.insert(key_of(v1, ep1));
        }
    }
    return output;
}

struct RefResult
{
    RefMesh mesh;
    AttrTable attr;
};

RefResult ref_run(const RefMesh& start, const AttrTable& start_attr,
                  unsigned levels)
{
    RefResult result{start, start_attr};
    for (unsigned i = 0; i < levels; ++i)
    {
        AttrTable next;
        result.mesh = ref_step(result.mesh, result.attr, next);
        result.attr = std::move(next);
    }
    return result;
}

AttrTable positions_as_attr(const SurfaceMesh& mesh)
{
    const std::vector<IndexType> map = compact_vertices(mesh);
    AttrTable attr;
    for (auto v : mesh.vertices())
    {
        if (mesh.is_deleted(v))
            continue;
        const Point p = mesh.position(v);
        attr.push_back({p[0], p[1], p[2]});
    }
    return attr;
}

// ---------------------------------------------------------------------------
// Plan helpers
// ---------------------------------------------------------------------------

std::vector<Point> compact_positions(const SurfaceMesh& mesh,
                                     const SubdivisionPlan& plan)
{
    std::vector<Point> positions(plan.source_vertex_count());
    for (size_t i = 0; i < positions.size(); ++i)
        positions[i] = mesh.position(plan.source_vertex(i));
    return positions;
}

// Per-geometry-vertex set of channel control vertices.
std::map<IndexType, std::set<IndexType>>
channel_vertices_per_geometry(const SubdivisionPlan& plan, size_t channel)
{
    std::map<IndexType, std::set<IndexType>> result;
    const auto& channel_vertex = plan.channel_source_corner_vertices(channel);
    for (IndexType cid = 0; cid < plan.source_corner_count(); ++cid)
        result[plan.source_corner_vertex(cid)].insert(channel_vertex[cid]);
    return result;
}

// Channel control vertex reached from a geometry vertex, asserting it is a
// single channel vertex.
IndexType single_channel_vertex(const SubdivisionPlan& plan, size_t channel,
                                const SurfaceMesh&, Vertex v)
{
    const IndexType gv = plan.source_vertex_id(v);
    const auto map = channel_vertices_per_geometry(plan, channel);
    const auto it = map.find(gv);
    if (it == map.end() || it->second.size() != 1)
        return kBad;
    return *it->second.begin();
}

// Compare a channel's evaluated values corner by corner with a reference mesh
// whose faces/corner order equals the plan's.
bool channel_matches(const SubdivisionPlan& plan, size_t channel,
                     const RefResult& reference)
{
    if (reference.mesh.faces.size() != plan.face_count())
        return false;
    if (reference.attr.size() != plan.channel_value_count(channel))
        return false;
    const auto& corners = plan.channel_corner_indices(channel);
    const auto& values = plan.channel_values(channel);
    for (size_t f = 0; f < plan.face_count(); ++f)
    {
        const auto& ref_face = reference.mesh.faces[f];
        if (ref_face.size() != corners[f].size())
            return false;
        for (size_t k = 0; k < ref_face.size(); ++k)
        {
            const IndexType cid = corners[f][k];
            if (cid >= values.size())
                return false;
            const AttrRow& expected = reference.attr[ref_face[k]];
            if (!close(values[cid][0], expected[0]) ||
                !close(values[cid][1], expected[1]))
                return false;
        }
    }
    return true;
}

bool positions_match(const std::vector<Point>& positions,
                     const RefResult& reference)
{
    if (positions.size() != reference.attr.size())
        return false;
    for (size_t i = 0; i < positions.size(); ++i)
    {
        const AttrRow& row = reference.attr[i];
        if (!close(positions[i][0], row[0]) ||
            !close(positions[i][1], row[1]) ||
            !close(positions[i][2], row[2]))
            return false;
    }
    return true;
}

// Build the channel's control topology from the plan's corner -> channel
// control vertex table. Only the topological mapping is taken from the plan;
// the arithmetic in ref_run() is independent. The seam splitting itself is
// exercised separately by explicit split counts.
RefMesh ref_channel_mesh(const SurfaceMesh& mesh, const SubdivisionPlan& plan,
                         size_t channel)
{
    const auto& corner_vertex =
        plan.channel_source_corner_vertices(channel);
    RefMesh ref;
    ref.vertex_feature.assign(plan.channel_control_vertex_count(channel), 0);
    for (auto f : mesh.faces())
    {
        std::vector<IndexType> face;
        for (auto h : mesh.halfedges(f))
            face.push_back(corner_vertex[plan.source_corner_id(h)]);
        ref.faces.push_back(std::move(face));
    }
    return ref;
}

AttrTable channel_control_attr(const SubdivisionPlan& plan, size_t channel,
                               const std::vector<TexCoord>& values)
{
    AttrTable attr(plan.channel_control_vertex_count(channel), AttrRow(2, 0));
    std::vector<char> seen(attr.size(), 0);
    const auto& corner_vertex =
        plan.channel_source_corner_vertices(channel);
    const auto& value_index = plan.channel_source_value_indices(channel);
    for (IndexType cid = 0; cid < plan.source_corner_count(); ++cid)
    {
        const IndexType cv = corner_vertex[cid];
        if (seen[cv])
            continue;
        seen[cv] = 1;
        const TexCoord& value = values[value_index[cid]];
        attr[cv] = {value[0], value[1]};
    }
    return attr;
}

RefResult run_channel_reference(const SurfaceMesh& mesh,
                                const SubdivisionPlan& plan, size_t channel,
                                const std::vector<TexCoord>& values,
                                unsigned levels)
{
    const RefMesh ref = ref_channel_mesh(mesh, plan, channel);
    const AttrTable attr = channel_control_attr(plan, channel, values);
    return ref_run(ref, attr, levels);
}

// CSR invariants: monotone offsets, sorted unique columns in range, sums 1.
bool valid_stencil(const SparseStencil& stencil, size_t columns)
{
    if (stencil.row_offsets.empty())
        return stencil.column_indices.empty() && stencil.weights.empty();
    if (stencil.row_offsets.front() != 0)
        return false;
    if (stencil.column_indices.size() != stencil.weights.size())
        return false;
    for (size_t r = 0; r < stencil.rows(); ++r)
    {
        const IndexType begin = stencil.row_offsets[r];
        const IndexType end = stencil.row_offsets[r + 1];
        if (end < begin || size_t(end) > stencil.column_indices.size())
            return false;
        Scalar sum = 0;
        for (IndexType k = begin; k < end; ++k)
        {
            if (stencil.column_indices[k] >= columns)
                return false;
            if (k > begin &&
                stencil.column_indices[k] <= stencil.column_indices[k - 1])
                return false;
            sum += stencil.weights[k];
        }
        if (!close(sum, 1.0, 1e-12))
            return false;
    }
    return stencil.row_offsets.back() ==
           IndexType(stencil.column_indices.size());
}

// ---------------------------------------------------------------------------
// Input meshes. Vertex numbering follows insertion order.
// ---------------------------------------------------------------------------

struct Grid
{
    SurfaceMesh mesh;
    std::vector<Vertex> v;
    std::vector<Face> faces;
};

Grid make_grid(int n)
{
    Grid grid;
    grid.v.resize(size_t((n + 1) * (n + 1)));
    for (int i = 0; i <= n; ++i)
        for (int j = 0; j <= n; ++j)
            grid.v[size_t(i * (n + 1) + j)] =
                grid.mesh.add_vertex(Point(i, j, 0));
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            grid.faces.push_back(grid.mesh.add_quad(
                grid.v[size_t(i * (n + 1) + j)],
                grid.v[size_t((i + 1) * (n + 1) + j)],
                grid.v[size_t((i + 1) * (n + 1) + j + 1)],
                grid.v[size_t(i * (n + 1) + j + 1)]));
    return grid;
}

std::vector<SubdivisionPlanChannel> one_channel(const std::string& name,
                                                const std::vector<TexCoord>& v,
                                                std::vector<IndexType> corners)
{
    SubdivisionPlanChannel channel;
    channel.name = name;
    channel.values = v;
    channel.corner_values = std::move(corners);
    return {channel};
}

std::vector<IndexType> corner_index_by_vertex(const SurfaceMesh& mesh)
{
    std::vector<IndexType> corners(mesh.halfedges_size(), 0);
    for (auto h : mesh.halfedges())
        if (!mesh.is_deleted(h))
            corners[h.idx()] = IndexType(mesh.to_vertex(h).idx());
    return corners;
}

std::vector<TexCoord> vertex_uvs(const SurfaceMesh& mesh)
{
    std::vector<TexCoord> values(mesh.vertices_size(), TexCoord(0, 0));
    for (auto v : mesh.vertices())
        if (!mesh.is_deleted(v))
            values[v.idx()] =
                TexCoord(mesh.position(v)[0], mesh.position(v)[1]);
    return values;
}

// ---------------------------------------------------------------------------
// topology group
// ---------------------------------------------------------------------------

int case_closed_quad_cage()
{
    // Hexahedron built directly: 8 vertices, 6 quads, closed.
    SurfaceMesh mesh;
    std::vector<Vertex> v(8);
    for (int i = 0; i < 8; ++i)
        v[i] = mesh.add_vertex(
            Point((i & 1) ? 1 : -1, (i & 2) ? 1 : -1, (i & 4) ? 1 : -1));
    mesh.add_quad(v[0], v[2], v[3], v[1]);
    mesh.add_quad(v[4], v[5], v[7], v[6]);
    mesh.add_quad(v[0], v[1], v[5], v[4]);
    mesh.add_quad(v[1], v[3], v[7], v[5]);
    mesh.add_quad(v[3], v[2], v[6], v[7]);
    mesh.add_quad(v[2], v[0], v[4], v[6]);

    SubdivisionPlan plan(mesh, 2, {});
    TASK_CHECK(plan.is_built());
    TASK_CHECK(plan.levels() == 2);
    TASK_CHECK(plan.source_vertex_count() == 8);
    TASK_CHECK(plan.source_face_count() == 6);
    TASK_CHECK(plan.source_corner_count() == 24);
    TASK_CHECK(plan.channel_count() == 0);
    TASK_CHECK(plan.vertex_count() == 98);
    TASK_CHECK(plan.face_count() == 96);
    for (const auto& face : plan.faces())
        TASK_CHECK(face.size() == 4);
    for (IndexType source : plan.face_sources())
        TASK_CHECK(source < plan.source_face_count());

    for (auto f : mesh.faces())
    {
        const IndexType id = plan.source_face_id(f);
        TASK_CHECK(id != SubdivisionPlan::invalid_index);
        TASK_CHECK(plan.source_face(id) == f);
    }
    for (auto vtx : mesh.vertices())
    {
        const IndexType id = plan.source_vertex_id(vtx);
        TASK_CHECK(id != SubdivisionPlan::invalid_index);
        TASK_CHECK(plan.source_vertex(id) == vtx);
    }
    for (auto h : mesh.halfedges())
    {
        const IndexType id = plan.source_corner_id(h);
        TASK_CHECK(id != SubdivisionPlan::invalid_index);
        TASK_CHECK(plan.source_corner_halfedge(id) == h);
        TASK_CHECK(plan.source_corner_face(id) ==
                   plan.source_face_id(mesh.face(h)));
        TASK_CHECK(plan.source_corner_vertex(id) ==
                   plan.source_vertex_id(mesh.to_vertex(h)));
    }
    TASK_CHECK(plan.source_vertex_id(Vertex(1000)) ==
               SubdivisionPlan::invalid_index);
    TASK_CHECK(valid_stencil(plan.position_stencil(), 8));
    return 0;
}

int case_valence_3_and_5()
{
    // Pentagon fan: one valence-5 interior vertex, valence-3 ring vertices.
    SurfaceMesh mesh;
    const int n = 5;
    std::vector<Vertex> ring(n);
    for (int i = 0; i < n; ++i)
    {
        const double a = 2.0 * 3.141592653589793 * i / n;
        ring[i] = mesh.add_vertex(Point(std::cos(a), std::sin(a), 0));
    }
    const Vertex center = mesh.add_vertex(Point(0, 0, 0));
    for (int i = 0; i < n; ++i)
        mesh.add_triangle(ring[i], ring[(i + 1) % n], center);

    SubdivisionPlan plan(mesh, 1, {});
    TASK_CHECK(plan.source_vertex_count() == 6);
    TASK_CHECK(plan.source_face_count() == 5);
    TASK_CHECK(plan.source_corner_count() == 15);
    TASK_CHECK(plan.face_count() == 15);
    for (const auto& face : plan.faces())
        TASK_CHECK(face.size() == 4);
    TASK_CHECK(valid_stencil(plan.position_stencil(), 6));

    // The triangle fan is a disk; geometry reference matches.
    RefMesh ref = ref_mesh_from(mesh);
    RefResult reference = ref_run(ref, positions_as_attr(mesh), 1);
    TASK_CHECK(positions_match(plan.positions(), reference));

    // Tetrahedron: all vertices valence 3, closed.
    SurfaceMesh tetra;
    std::vector<Vertex> t(4);
    for (int i = 0; i < 4; ++i)
        t[i] = tetra.add_vertex(
            Point(i == 1 ? 1 : 0, i == 2 ? 1 : 0, i == 3 ? 1 : 0));
    tetra.add_triangle(t[0], t[2], t[1]);
    tetra.add_triangle(t[0], t[1], t[3]);
    tetra.add_triangle(t[0], t[3], t[2]);
    tetra.add_triangle(t[1], t[2], t[3]);
    SubdivisionPlan tetra_plan(tetra, 3, {});
    TASK_CHECK(tetra_plan.source_corner_count() == 12);
    TASK_CHECK(valid_stencil(tetra_plan.position_stencil(), 4));
    RefResult tetra_ref =
        ref_run(ref_mesh_from(tetra), positions_as_attr(tetra), 3);
    TASK_CHECK(positions_match(tetra_plan.positions(), tetra_ref));
    return 0;
}

int case_components_and_holes()
{
    SurfaceMesh mesh;
    auto a0 = mesh.add_vertex(Point(0, 0, 0));
    auto a1 = mesh.add_vertex(Point(1, 0, 0));
    auto a2 = mesh.add_vertex(Point(1, 1, 0));
    auto a3 = mesh.add_vertex(Point(0, 1, 0));
    auto b0 = mesh.add_vertex(Point(5, 0, 0));
    auto b1 = mesh.add_vertex(Point(6, 0, 0));
    auto b2 = mesh.add_vertex(Point(6, 1, 0));
    auto b3 = mesh.add_vertex(Point(5, 1, 0));
    mesh.add_quad(a0, a1, a2, a3);
    mesh.add_quad(b0, b1, b2, b3);
    SubdivisionPlan plan(mesh, 1, {});
    TASK_CHECK(plan.source_vertex_count() == 8);
    TASK_CHECK(plan.source_face_count() == 2);
    TASK_CHECK(plan.face_count() == 8);
    TASK_CHECK(valid_stencil(plan.position_stencil(), 8));
    RefResult reference =
        ref_run(ref_mesh_from(mesh), positions_as_attr(mesh), 1);
    TASK_CHECK(positions_match(plan.positions(), reference));

    // 3x3 quad grid with the center face deleted: a hole plus a compressed
    // deleted face slot (n_faces == 8 but faces_size() > 8).
    SurfaceMesh grid;
    std::vector<std::vector<Vertex>> g(4, std::vector<Vertex>(4));
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            g[i][j] = grid.add_vertex(Point(i, j, 0));
    Face hole;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
        {
            Face f = grid.add_quad(g[i][j], g[i + 1][j], g[i + 1][j + 1],
                                   g[i][j + 1]);
            if (i == 1 && j == 1)
                hole = f;
        }
    grid.delete_face(hole);
    TASK_CHECK(grid.n_faces() == 8);
    TASK_CHECK(grid.n_vertices() == 16);
    TASK_CHECK(grid.faces_size() > grid.n_faces());
    SubdivisionPlan holed(grid, 2, {});
    TASK_CHECK(holed.source_face_count() == 8);
    TASK_CHECK(holed.source_vertex_count() == 16);
    TASK_CHECK(holed.face_count() == 8u * 16u);
    TASK_CHECK(valid_stencil(holed.position_stencil(), 16));
    RefResult hole_ref =
        ref_run(ref_mesh_from(grid), positions_as_attr(grid), 2);
    TASK_CHECK(positions_match(holed.positions(), hole_ref));
    return 0;
}

int case_levels_range()
{
    Grid grid = make_grid(1);
    for (unsigned levels = 1; levels <= 5; ++levels)
    {
        SubdivisionPlan plan(grid.mesh, levels, {});
        TASK_CHECK(plan.levels() == levels);
        size_t expected_faces = 1;
        for (unsigned i = 0; i < levels; ++i)
            expected_faces *= 4;
        TASK_CHECK(plan.face_count() == expected_faces);
        for (const auto& face : plan.faces())
            TASK_CHECK(face.size() == 4);
        for (const Point& p : plan.positions())
            TASK_CHECK(finite(p));
    }
    return 0;
}

int case_input_validation()
{
    // Isolated vertex.
    {
        SurfaceMesh mesh;
        auto a = mesh.add_vertex(Point(0, 0, 0));
        auto b = mesh.add_vertex(Point(1, 0, 0));
        auto c = mesh.add_vertex(Point(0, 1, 0));
        mesh.add_triangle(a, b, c);
        mesh.add_vertex(Point(5, 5, 5));
        bool threw = false;
        try
        {
            SubdivisionPlan plan(mesh, 1, {});
        }
        catch (const InvalidInputException&)
        {
            threw = true;
        }
        TASK_CHECK(threw);
    }
    // Non-finite source position.
    {
        SurfaceMesh mesh;
        auto a = mesh.add_vertex(Point(0, 0, 0));
        auto b = mesh.add_vertex(Point(1, 0, 0));
        auto c = mesh.add_vertex(
            Point(0, std::numeric_limits<Scalar>::quiet_NaN(), 0));
        mesh.add_triangle(a, b, c);
        bool threw = false;
        try
        {
            SubdivisionPlan plan(mesh, 1, {});
        }
        catch (const InvalidInputException&)
        {
            threw = true;
        }
        TASK_CHECK(threw);
    }
    // Levels out of range.
    {
        Grid grid = make_grid(1);
        for (unsigned levels : {0u, 6u})
        {
            bool threw = false;
            try
            {
                SubdivisionPlan plan(grid.mesh, levels, {});
            }
            catch (const InvalidInputException&)
            {
                threw = true;
            }
            TASK_CHECK(threw);
        }
    }
    // Channel corner array size mismatch.
    {
        Grid grid = make_grid(1);
        bool threw = false;
        try
        {
            SubdivisionPlan plan(
                grid.mesh, 1,
                one_channel("uv", {TexCoord(0, 0)}, {0, 0, 0}));
        }
        catch (const InvalidInputException&)
        {
            threw = true;
        }
        TASK_CHECK(threw);
    }
    // Channel corner index out of range.
    {
        Grid grid = make_grid(1);
        auto corners = corner_index_by_vertex(grid.mesh);
        corners[0] = 99;
        bool threw = false;
        try
        {
            SubdivisionPlan plan(grid.mesh, 1,
                                 one_channel("uv", vertex_uvs(grid.mesh),
                                             std::move(corners)));
        }
        catch (const InvalidInputException&)
        {
            threw = true;
        }
        TASK_CHECK(threw);
    }
    // Channel value table with non-finite data.
    {
        Grid grid = make_grid(1);
        auto values = vertex_uvs(grid.mesh);
        values[0][0] = std::numeric_limits<Scalar>::infinity();
        bool threw = false;
        try
        {
            SubdivisionPlan plan(
                grid.mesh, 1,
                one_channel("uv", values, corner_index_by_vertex(grid.mesh)));
        }
        catch (const InvalidInputException&)
        {
            threw = true;
        }
        TASK_CHECK(threw);
    }
    // A pinched fan (two faces touching only in one vertex) is not a valid
    // manifold cage and must be rejected before any plan is published.
    {
        SurfaceMesh pinched;
        auto c = pinched.add_vertex(Point(0, 0, 0));
        auto a0 = pinched.add_vertex(Point(1, 0, 0));
        auto a1 = pinched.add_vertex(Point(0, 1, 0));
        auto b0 = pinched.add_vertex(Point(-1, 0, 0));
        auto b1 = pinched.add_vertex(Point(0, -1, 0));
        pinched.add_triangle(c, a0, a1);
        pinched.add_triangle(c, b0, b1);
        bool threw = false;
        try
        {
            SubdivisionPlan plan(pinched, 1, {});
        }
        catch (const TopologyException&)
        {
            threw = true;
        }
        catch (const InvalidInputException&)
        {
            threw = true;
        }
        TASK_CHECK(threw);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// stencils group
// ---------------------------------------------------------------------------

int case_geometry_reference()
{
    // Open quad strip: all boundary vertices.
    SurfaceMesh strip;
    std::vector<Vertex> v(6);
    v[0] = strip.add_vertex(Point(0, 0, 0.1));
    v[1] = strip.add_vertex(Point(1, 0, 0.2));
    v[2] = strip.add_vertex(Point(1, 1, 0.3));
    v[3] = strip.add_vertex(Point(0, 1, 0.4));
    v[4] = strip.add_vertex(Point(2, 1, 0.5));
    v[5] = strip.add_vertex(Point(2, 0, 0.6));
    strip.add_quad(v[0], v[1], v[2], v[3]);
    strip.add_quad(v[3], v[2], v[4], v[5]);
    for (unsigned levels = 1; levels <= 2; ++levels)
    {
        SubdivisionPlan plan(strip, levels, {});
        RefResult reference =
            ref_run(ref_mesh_from(strip), positions_as_attr(strip), levels);
        TASK_CHECK(positions_match(plan.positions(), reference));
    }

    // Closed hexahedron with perturbed positions.
    SurfaceMesh closed;
    std::vector<Vertex> c(8);
    for (int i = 0; i < 8; ++i)
        c[i] = closed.add_vertex(
            Point((i & 1) ? 1 : -1, (i & 2) ? 1 : -1, (i & 4) ? 1 : -1));
    closed.add_quad(c[0], c[2], c[3], c[1]);
    closed.add_quad(c[4], c[5], c[7], c[6]);
    closed.add_quad(c[0], c[1], c[5], c[4]);
    closed.add_quad(c[1], c[3], c[7], c[5]);
    closed.add_quad(c[3], c[2], c[6], c[7]);
    closed.add_quad(c[2], c[0], c[4], c[6]);
    for (auto vtx : closed.vertices())
    {
        const Point p = closed.position(vtx);
        closed.position(vtx) = Point(p[0] + 0.1 * p[2], p[1], p[2] + 0.2 * p[0]);
    }
    for (unsigned levels = 1; levels <= 3; ++levels)
    {
        SubdivisionPlan plan(closed, levels, {});
        RefResult reference =
            ref_run(ref_mesh_from(closed), positions_as_attr(closed), levels);
        TASK_CHECK(positions_match(plan.positions(), reference));
    }
    return 0;
}

int case_boundary_uv_rules()
{
    SurfaceMesh strip;
    std::vector<Vertex> v(6);
    v[0] = strip.add_vertex(Point(0, 0, 0));
    v[1] = strip.add_vertex(Point(1, 0, 0));
    v[2] = strip.add_vertex(Point(1, 1, 0));
    v[3] = strip.add_vertex(Point(0, 1, 0));
    v[4] = strip.add_vertex(Point(2, 1, 0));
    v[5] = strip.add_vertex(Point(2, 0, 0));
    strip.add_quad(v[0], v[1], v[2], v[3]);
    strip.add_quad(v[3], v[2], v[4], v[5]);

    SubdivisionPlan plan(strip, 1,
                         one_channel("uv", vertex_uvs(strip),
                                     corner_index_by_vertex(strip)));

    // Reference UV on the same closed-index (seamless) topology, built
    // directly from the mesh and the plan's single control vertex per
    // geometry vertex.
    RefMesh ref = ref_channel_mesh(strip, plan, 0);
    AttrTable uv_attr(plan.channel_control_vertex_count(0), AttrRow(2, 0));
    for (auto vtx : strip.vertices())
        uv_attr[single_channel_vertex(plan, 0, strip, vtx)] = {
            Scalar(strip.position(vtx)[0]), Scalar(strip.position(vtx)[1])};
    RefResult reference = ref_run(ref, uv_attr, 1);
    TASK_CHECK(channel_matches(plan, 0, reference));

    // Explicit boundary rule at corner v0: (6P + prev + next)/8.
    const IndexType control =
        single_channel_vertex(plan, 0, strip, v[0]);
    TASK_CHECK(control != kBad);
    TASK_CHECK(control < plan.channel_value_count(0));
    const TexCoord corner = plan.channel_values(0)[control];
    TASK_CHECK(close(corner[0], 0.125));
    TASK_CHECK(close(corner[1], 0.125));
    return 0;
}

int case_features_do_not_affect_uv()
{
    SurfaceMesh strip;
    std::vector<Vertex> v(6);
    v[0] = strip.add_vertex(Point(0, 0, 0));
    v[1] = strip.add_vertex(Point(1, 0, 0));
    v[2] = strip.add_vertex(Point(1, 1, 0));
    v[3] = strip.add_vertex(Point(0, 1, 0));
    v[4] = strip.add_vertex(Point(2, 1, 0));
    v[5] = strip.add_vertex(Point(2, 0, 0));
    strip.add_quad(v[0], v[1], v[2], v[3]);
    strip.add_quad(v[3], v[2], v[4], v[5]);
    auto efeature = strip.edge_property<bool>("e:feature", false);
    auto vfeature = strip.vertex_property<bool>("v:feature", false);
    const Edge shared = strip.find_edge(v[3], v[2]);
    TASK_CHECK(shared.is_valid());
    efeature[shared] = true;
    vfeature[v[2]] = true;
    vfeature[v[3]] = true;

    SubdivisionPlan plan(strip, 1,
                         one_channel("uv", vertex_uvs(strip),
                                     corner_index_by_vertex(strip)));

    // UV must follow the smooth (no feature) rules: build the reference over
    // the channel topology with all feature flags cleared.
    RefMesh ref = ref_channel_mesh(strip, plan, 0);
    ref.feature_edges.clear();
    ref.vertex_feature.assign(ref.vertex_feature.size(), 0);
    AttrTable uv_attr(plan.channel_control_vertex_count(0), AttrRow(2, 0));
    for (auto vtx : strip.vertices())
        uv_attr[single_channel_vertex(plan, 0, strip, vtx)] = {
            Scalar(strip.position(vtx)[0]), Scalar(strip.position(vtx)[1])};
    RefResult reference = ref_run(ref, uv_attr, 1);
    TASK_CHECK(channel_matches(plan, 0, reference));

    // Geometry, in contrast, does see the feature flag.
    RefMesh geo_ref = ref_mesh_from(strip);
    RefResult geo_reference =
        ref_run(geo_ref, positions_as_attr(strip), 1);
    TASK_CHECK(positions_match(plan.positions(), geo_reference));
    return 0;
}

int case_channel_reference_rounds()
{
    Grid grid = make_grid(2);
    SurfaceMesh& mesh = grid.mesh;

    // Seam along the middle column (i == 1): faces with i == 0 use the base
    // index, faces with i == 1 use a duplicate index with identical value.
    std::vector<TexCoord> values(2 * mesh.vertices_size(), TexCoord(0, 0));
    for (auto vtx : mesh.vertices())
        if (!mesh.is_deleted(vtx))
        {
            const TexCoord t(mesh.position(vtx)[0], mesh.position(vtx)[1]);
            values[vtx.idx()] = t;
            values[vtx.idx() + mesh.vertices_size()] = t;
        }
    std::vector<IndexType> corners(mesh.halfedges_size(), 0);
    const size_t n1 = 3;
    for (auto h : mesh.halfedges())
    {
        if (mesh.is_deleted(h))
            continue;
        const Vertex vtx = mesh.to_vertex(h);
        const int i = int(vtx.idx()) / int(n1);
        const size_t base = vtx.idx();
        // The seam runs through column i == 1; right-hand faces (the face
        // whose corner is at a vertex in column 1 or 2) use the duplicate.
        const int face_i = int(mesh.face(h).idx()) / 2;
        const bool shifted = (i == 1 && face_i == 1) || (i == 2 && face_i == 1);
        corners[h.idx()] =
            IndexType(base + (shifted ? mesh.vertices_size() : 0));
    }

    const unsigned levels = 2;
    SubdivisionPlan plan(mesh, levels,
                         one_channel("uv", values, std::move(corners)));

    // Channel topology: column i == 1 is duplicated, giving 12 control
    // vertices for a 3x3 grid.
    TASK_CHECK(plan.channel_control_vertex_count(0) == 12);

    // Round-by-round reference over the channel's cut control topology.
    RefResult reference =
        run_channel_reference(mesh, plan, 0, values, levels);
    TASK_CHECK(reference.mesh.faces.size() == plan.face_count());
    TASK_CHECK(channel_matches(plan, 0, reference));

    // The two copies of each seam geometry vertex must be distinct channel
    // vertices at level zero, despite equal numeric values.
    const auto per_geometry = channel_vertices_per_geometry(plan, 0);
    for (auto vtx : mesh.vertices())
    {
        if (mesh.is_deleted(vtx))
            continue;
        const int i = int(vtx.idx()) / 3;
        const auto& set = per_geometry.at(plan.source_vertex_id(vtx));
        TASK_CHECK(set.size() == (i == 1 ? 2u : 1u));
    }

    // Per-corner values are consistent with the corner index table.
    const auto& result_corners = plan.channel_corner_indices(0);
    const auto& result_values = plan.channel_values(0);
    for (size_t f = 0; f < plan.face_count(); ++f)
        for (IndexType cid : result_corners[f])
        {
            TASK_CHECK(cid < result_values.size());
            TASK_CHECK(std::isfinite(result_values[cid][0]));
            TASK_CHECK(std::isfinite(result_values[cid][1]));
        }
    return 0;
}

int case_stencil_weights_and_provenance()
{
    Grid grid = make_grid(2);
    SurfaceMesh& mesh = grid.mesh;
    const unsigned levels = 2;
    SubdivisionPlan plan(mesh, levels,
                         one_channel("uv", vertex_uvs(mesh),
                                     corner_index_by_vertex(mesh)));

    const std::vector<Point> source = compact_positions(mesh, plan);
    const std::vector<TexCoord> uvs = vertex_uvs(mesh);

    // Positions: manual stencil application equals evaluate().
    plan.evaluate(source, {uvs});
    const auto& pos = plan.position_stencil();
    TASK_CHECK(valid_stencil(pos, plan.source_vertex_count()));
    TASK_CHECK(pos.rows() == plan.vertex_count());
    for (size_t r = 0; r < plan.vertex_count(); ++r)
    {
        Point p(0, 0, 0);
        for (IndexType k = pos.row_offsets[r]; k < pos.row_offsets[r + 1]; ++k)
            p += pos.weights[k] * source[pos.column_indices[k]];
        TASK_CHECK(close(p[0], plan.positions()[r][0]));
        TASK_CHECK(close(p[1], plan.positions()[r][1]));
        TASK_CHECK(close(p[2], plan.positions()[r][2]));
    }

    // Channel: manual stencil application equals evaluate(). Columns are
    // value indices into the channel value table.
    const auto& stencil = plan.channel_stencil(0);
    TASK_CHECK(valid_stencil(stencil, uvs.size()));
    TASK_CHECK(stencil.rows() == plan.channel_value_count(0));
    for (size_t r = 0; r < plan.channel_value_count(0); ++r)
    {
        TexCoord t(0, 0);
        for (IndexType k = stencil.row_offsets[r];
             k < stencil.row_offsets[r + 1]; ++k)
            t += stencil.weights[k] * uvs[stencil.column_indices[k]];
        TASK_CHECK(close(t[0], plan.channel_values(0)[r][0]));
        TASK_CHECK(close(t[1], plan.channel_values(0)[r][1]));
    }

    // Geometry reference equals the plan round by round.
    RefResult reference =
        ref_run(ref_mesh_from(mesh), positions_as_attr(mesh), levels);
    TASK_CHECK(positions_match(plan.positions(), reference));

    // Face provenance: every quad source face produces 4^levels children and
    // every result face maps back to a source face.
    std::vector<size_t> counts(plan.source_face_count(), 0);
    for (IndexType source_face : plan.face_sources())
    {
        TASK_CHECK(source_face < plan.source_face_count());
        counts[source_face]++;
    }
    for (size_t f = 0; f < plan.source_face_count(); ++f)
        TASK_CHECK(counts[f] == 16);

    // The channel's result face layout is exactly the geometry's.
    const auto& corners = plan.channel_corner_indices(0);
    TASK_CHECK(corners.size() == plan.face_count());
    for (size_t f = 0; f < plan.face_count(); ++f)
        TASK_CHECK(corners[f].size() == plan.faces()[f].size());
    return 0;
}

int case_sparse_handles()
{
    Grid grid = make_grid(2);
    SurfaceMesh& mesh = grid.mesh;
    SubdivisionPlan plan(mesh, 2,
                         one_channel("uv", vertex_uvs(mesh),
                                     corner_index_by_vertex(mesh)));

    const auto& pos = plan.position_stencil();
    TASK_CHECK(valid_stencil(pos, plan.source_vertex_count()));
    TASK_CHECK(pos.row_offsets.size() == plan.vertex_count() + 1);
    TASK_CHECK(pos.non_zeros() == pos.column_indices.size());
    TASK_CHECK(pos.weights.size() == pos.column_indices.size());

    const auto& channel = plan.channel_stencil(0);
    TASK_CHECK(valid_stencil(channel, vertex_uvs(mesh).size()));
    TASK_CHECK(channel.row_offsets.size() ==
               plan.channel_value_count(0) + 1);

    const auto& indices = plan.channel_source_value_indices(0);
    TASK_CHECK(indices.size() == plan.source_corner_count());
    TASK_CHECK(plan.channel_source_corner_vertices(0).size() ==
               plan.source_corner_count());
    for (IndexType v : pos.column_indices)
        TASK_CHECK(v < plan.source_vertex_count());
    for (IndexType value_index : channel.column_indices)
        TASK_CHECK(value_index < vertex_uvs(mesh).size());

    // Every value index in the source corner table is a valid value.
    for (IndexType idx : indices)
        TASK_CHECK(idx < mesh.vertices_size());

    // Result channel corner indices stay in range.
    for (const auto& face : plan.channel_corner_indices(0))
        for (IndexType cid : face)
            TASK_CHECK(cid < plan.channel_value_count(0));
    return 0;
}

// ---------------------------------------------------------------------------
// reuse group
// ---------------------------------------------------------------------------

int case_source_release_and_repeat()
{
    SubdivisionPlan plan;
    std::vector<SubdivisionPlanChannel> channels;
    size_t source_count = 0;
    {
        Grid grid = make_grid(1);
        channels = one_channel("uv", vertex_uvs(grid.mesh),
                               corner_index_by_vertex(grid.mesh));
        plan.rebuild(grid.mesh, 2, channels);
        TASK_CHECK(plan.is_built());
        source_count = plan.source_vertex_count();
        TASK_CHECK(source_count == 4);
    } // grid (and its mesh) is destroyed here

    std::vector<Point> positions(source_count);
    for (size_t i = 0; i < source_count; ++i)
        positions[i] = Point(Scalar(i), Scalar(2 * i), Scalar(-i));
    std::vector<std::vector<TexCoord>> tables(1);
    tables[0].resize(channels[0].values.size());
    for (size_t i = 0; i < tables[0].size(); ++i)
        tables[0][i] = TexCoord(Scalar(i) * 0.1, Scalar(i) * 0.2);

    plan.evaluate(positions, tables);
    const std::vector<Point> first = plan.positions();
    const std::vector<TexCoord> first_uv = plan.channel_values(0);
    TASK_CHECK(first.size() == plan.vertex_count());
    for (const Point& p : first)
        TASK_CHECK(finite(p));

    plan.evaluate(positions, tables);
    TASK_CHECK(plan.positions() == first);
    TASK_CHECK(plan.channel_values(0) == first_uv);

    std::vector<Point> other(source_count);
    for (size_t i = 0; i < source_count; ++i)
        other[i] = Point(Scalar(-i), Scalar(i), Scalar(3 * i));
    std::vector<std::vector<TexCoord>> other_tables(1);
    other_tables[0].resize(tables[0].size());
    for (size_t i = 0; i < other_tables[0].size(); ++i)
        other_tables[0][i] = TexCoord(Scalar(i), Scalar(-i));
    plan.evaluate(other, other_tables);
    TASK_CHECK(plan.positions() != first);
    for (const TexCoord& t : plan.channel_values(0))
        TASK_CHECK(std::isfinite(t[0]) && std::isfinite(t[1]));
    return 0;
}

int case_two_plans_interleaved()
{
    Grid a = make_grid(1);
    Grid b = make_grid(2);
    const auto ua = one_channel("a", vertex_uvs(a.mesh),
                                corner_index_by_vertex(a.mesh));
    const auto ub = one_channel("b", vertex_uvs(b.mesh),
                                corner_index_by_vertex(b.mesh));

    SubdivisionPlan pa(a.mesh, 2, ua);
    SubdivisionPlan pb(b.mesh, 3, ub);
    TASK_CHECK(pa.vertex_count() != pb.vertex_count());

    const std::vector<Point> pos_a = compact_positions(a.mesh, pa);
    const std::vector<Point> pos_b = compact_positions(b.mesh, pb);
    pa.evaluate(pos_a, {ua[0].values});
    pb.evaluate(pos_b, {ub[0].values});
    const std::vector<Point> result_a = pa.positions();
    const std::vector<Point> result_b = pb.positions();

    std::vector<Point> moved_a = pos_a;
    std::vector<Point> moved_b = pos_b;
    for (auto& p : moved_a)
        p = p + Point(1, 1, 1);
    for (auto& p : moved_b)
        p = p + Point(-1, 0, 2);

    // Interleave evaluations: the two plans must be fully independent.
    pb.evaluate(moved_b, {ub[0].values});
    pa.evaluate(moved_a, {ua[0].values});
    pb.evaluate(moved_b, {ub[0].values});
    pa.evaluate(moved_a, {ua[0].values});

    SubdivisionPlan pa2(a.mesh, 2, ua);
    pa2.evaluate(moved_a, {ua[0].values});
    TASK_CHECK(pa.positions() == pa2.positions());
    SubdivisionPlan pb2(b.mesh, 3, ub);
    pb2.evaluate(moved_b, {ub[0].values});
    TASK_CHECK(pb.positions() == pb2.positions());

    // Restoring the original control values reproduces the original result.
    pa.evaluate(pos_a, {ua[0].values});
    TASK_CHECK(pa.positions() == result_a);
    pb.evaluate(pos_b, {ub[0].values});
    TASK_CHECK(pb.positions() == result_b);
    return 0;
}

int case_failure_preserves_previous()
{
    Grid grid = make_grid(1);
    const auto channel = one_channel("uv", vertex_uvs(grid.mesh),
                                     corner_index_by_vertex(grid.mesh));
    SubdivisionPlan plan(grid.mesh, 2, channel);
    const std::vector<Point> positions = compact_positions(grid.mesh, plan);
    plan.evaluate(positions, {channel[0].values});
    const std::vector<Point> good_positions = plan.positions();
    const std::vector<TexCoord> good_values = plan.channel_values(0);
    const size_t good_vertices = plan.vertex_count();
    const size_t good_faces = plan.face_count();

    // Wrong position count.
    bool threw = false;
    try
    {
        plan.evaluate(std::vector<Point>(3, Point(0, 0, 0)),
                      {channel[0].values});
    }
    catch (const InvalidInputException&)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(plan.positions() == good_positions);
    TASK_CHECK(plan.channel_values(0) == good_values);

    // Wrong value table length.
    threw = false;
    try
    {
        plan.evaluate(positions, {std::vector<TexCoord>(2, TexCoord(0, 0))});
    }
    catch (const InvalidInputException&)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(plan.positions() == good_positions);

    // Non-finite position.
    threw = false;
    {
        std::vector<Point> bad = positions;
        bad[0][0] = std::numeric_limits<Scalar>::infinity();
        try
        {
            plan.evaluate(bad, {channel[0].values});
        }
        catch (const InvalidInputException&)
        {
            threw = true;
        }
    }
    TASK_CHECK(threw);
    TASK_CHECK(plan.positions() == good_positions);

    // Non-finite channel value.
    threw = false;
    {
        std::vector<TexCoord> bad = channel[0].values;
        bad[0][1] = std::numeric_limits<Scalar>::quiet_NaN();
        try
        {
            plan.evaluate(positions, {bad});
        }
        catch (const InvalidInputException&)
        {
            threw = true;
        }
    }
    TASK_CHECK(threw);
    TASK_CHECK(plan.channel_values(0) == good_values);

    // Rebuilding with invalid levels keeps the old plan.
    threw = false;
    try
    {
        plan.rebuild(grid.mesh, 0, channel);
    }
    catch (const InvalidInputException&)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(plan.is_built());
    TASK_CHECK(plan.levels() == 2);
    TASK_CHECK(plan.vertex_count() == good_vertices);
    TASK_CHECK(plan.face_count() == good_faces);
    TASK_CHECK(plan.positions() == good_positions);

    // Rebuilding with an out-of-range corner index keeps the old plan.
    threw = false;
    try
    {
        auto bad = channel;
        bad[0].corner_values[0] = 999;
        plan.rebuild(grid.mesh, 2, bad);
    }
    catch (const InvalidInputException&)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(plan.positions() == good_positions);
    TASK_CHECK(plan.channel_values(0) == good_values);
    return 0;
}

int case_two_disjoint_seams()
{
    // Two channels whose seams lie on different lines of a 2x2 grid:
    // channel A is cut along x == 1 (column i == 1), channel B along y == 1
    // (row j == 1).
    Grid grid = make_grid(2);
    SurfaceMesh& mesh = grid.mesh;
    const size_t stride = mesh.vertices_size();
    const size_t n1 = 3;

    std::vector<TexCoord> values_a(stride * 2, TexCoord(0, 0));
    std::vector<TexCoord> values_b(stride * 2, TexCoord(0, 0));
    for (auto vtx : mesh.vertices())
    {
        if (mesh.is_deleted(vtx))
            continue;
        const TexCoord ta(mesh.position(vtx)[0], mesh.position(vtx)[1]);
        const TexCoord tb(mesh.position(vtx)[0] * 2, mesh.position(vtx)[1] * 2);
        values_a[vtx.idx()] = ta;
        values_a[vtx.idx() + stride] = ta;
        values_b[vtx.idx()] = tb;
        values_b[vtx.idx() + stride] = tb;
    }
    std::vector<IndexType> corners_a(mesh.halfedges_size(), 0);
    std::vector<IndexType> corners_b(mesh.halfedges_size(), 0);
    for (auto h : mesh.halfedges())
    {
        if (mesh.is_deleted(h))
            continue;
        const Vertex vtx = mesh.to_vertex(h);
        const int i = int(vtx.idx()) / int(n1);
        const int j = int(vtx.idx()) % int(n1);
        const int face_i = int(mesh.face(h).idx()) / 2;
        const int face_j = int(mesh.face(h).idx()) % 2;
        // Seam A: right-hand faces use the duplicate index for column i == 1.
        const bool cut_a = (i == 1 && face_i == 1) || (i == 2 && face_i == 1);
        // Seam B: upper faces use the duplicate index for row j == 1.
        const bool cut_b = (j == 1 && face_j == 1) || (j == 2 && face_j == 1);
        corners_a[h.idx()] =
            IndexType(vtx.idx() + (cut_a ? stride : 0));
        corners_b[h.idx()] =
            IndexType(vtx.idx() + (cut_b ? stride : 0));
    }

    SubdivisionPlan plan(mesh, 2,
                         one_channel("a", values_a, corners_a));
    SubdivisionPlanChannel channel_b;
    channel_b.name = "b";
    channel_b.values = values_b;
    channel_b.corner_values = corners_b;
    auto full_channels = one_channel("a", values_a, corners_a);
    full_channels.push_back(channel_b);
    plan.rebuild(mesh, 2, full_channels);
    TASK_CHECK(plan.channel_count() == 2);

    // Each channel splits exactly the 3 vertices on its own seam line.
    const auto per_a = channel_vertices_per_geometry(plan, 0);
    const auto per_b = channel_vertices_per_geometry(plan, 1);
    for (auto vtx : mesh.vertices())
    {
        if (mesh.is_deleted(vtx))
            continue;
        const int i = int(vtx.idx()) / int(n1);
        const int j = int(vtx.idx()) % int(n1);
        const IndexType gv = plan.source_vertex_id(vtx);
        TASK_CHECK(per_a.at(gv).size() == (i == 1 ? 2u : 1u));
        TASK_CHECK(per_b.at(gv).size() == (j == 1 ? 2u : 1u));
    }
    // The seams are not identical: the split vertex sets differ.
    std::set<IndexType> split_a, split_b;
    for (const auto& entry : per_a)
        if (entry.second.size() > 1)
            split_a.insert(entry.first);
    for (const auto& entry : per_b)
        if (entry.second.size() > 1)
            split_b.insert(entry.first);
    TASK_CHECK(split_a != split_b);
    TASK_CHECK(split_a.size() == 3);
    TASK_CHECK(split_b.size() == 3);

    // Both channels are valid CSR stencils over their value tables.
    TASK_CHECK(valid_stencil(plan.channel_stencil(0), values_a.size()));
    TASK_CHECK(valid_stencil(plan.channel_stencil(1), values_b.size()));
    TASK_CHECK(plan.channel_corner_indices(0).size() == plan.face_count());
    TASK_CHECK(plan.channel_corner_indices(1).size() == plan.face_count());
    for (size_t f = 0; f < plan.face_count(); ++f)
    {
        TASK_CHECK(plan.channel_corner_indices(0)[f].size() ==
                   plan.faces()[f].size());
        TASK_CHECK(plan.channel_corner_indices(1)[f].size() ==
                   plan.faces()[f].size());
    }

    // Evaluate and check both channels produce finite values.
    std::vector<std::vector<TexCoord>> tables = {values_a, values_b};
    plan.evaluate(compact_positions(mesh, plan), tables);
    for (size_t c = 0; c < 2; ++c)
        for (const TexCoord& t : plan.channel_values(c))
            TASK_CHECK(std::isfinite(t[0]) && std::isfinite(t[1]));
    return 0;
}

int case_equal_values_different_indices()
{
    // Every corner gets its own value index, while all values are numerically
    // identical. Nothing may be welded by value equality: four disjoint
    // quads -> 16 channel control vertices.
    Grid grid = make_grid(2);
    SurfaceMesh& mesh = grid.mesh;
    std::vector<TexCoord> values(mesh.halfedges_size(), TexCoord(0.5, 0.5));
    std::vector<IndexType> corners(mesh.halfedges_size(), 0);
    for (auto h : mesh.halfedges())
        if (!mesh.is_deleted(h))
            corners[h.idx()] = IndexType(h.idx());

    SubdivisionPlan plan(mesh, 1,
                         one_channel("uv", values, std::move(corners)));
    TASK_CHECK(plan.channel_control_vertex_count(0) == 16);
    TASK_CHECK(plan.source_corner_count() == 16);
    for (const auto& face : plan.channel_corner_indices(0))
    {
        std::set<IndexType> unique(face.begin(), face.end());
        TASK_CHECK(unique.size() == 4);
    }
    plan.evaluate(compact_positions(mesh, plan), {values});

    // Reference channel: one independent quad per source face.
    const RefResult reference =
        run_channel_reference(mesh, plan, 0, values, 1);
    TASK_CHECK(channel_matches(plan, 0, reference));
    for (const TexCoord& t : plan.channel_values(0))
        TASK_CHECK(close(t[0], 0.5) && close(t[1], 0.5));
    return 0;
}

int case_split_fan()
{
    // Four quads around a center. The center reuses one value index on all
    // corners, but two opposite edges are seams (their outer endpoint uses a
    // duplicate index). The fan must split into two channel vertices without
    // being welded by the repeated center index.
    SurfaceMesh mesh;
    std::vector<Vertex> ring(4);
    ring[0] = mesh.add_vertex(Point(1, 0, 0));
    ring[1] = mesh.add_vertex(Point(0, 1, 0));
    ring[2] = mesh.add_vertex(Point(-1, 0, 0));
    ring[3] = mesh.add_vertex(Point(0, -1, 0));
    const Vertex center = mesh.add_vertex(Point(0, 0, 0));
    auto o0 = mesh.add_vertex(Point(1, 1, 0));
    auto o1 = mesh.add_vertex(Point(-1, 1, 0));
    auto o2 = mesh.add_vertex(Point(-1, -1, 0));
    auto o3 = mesh.add_vertex(Point(1, -1, 0));
    mesh.add_quad(center, ring[0], o0, ring[1]);
    const Face f1 = mesh.add_quad(center, ring[1], o1, ring[2]);
    mesh.add_quad(center, ring[2], o2, ring[3]);
    const Face f3 = mesh.add_quad(center, ring[3], o3, ring[0]);

    // Values: index 0 = lowest UV for the center, index v.idx()+1 for others.
    // Duplicate indices (one per geometry vertex) reuse the same value.
    std::vector<TexCoord> values;
    values.push_back(TexCoord(0, 0)); // center value index 0
    std::vector<IndexType> base(mesh.vertices_size(), 0);
    for (auto vtx : mesh.vertices())
    {
        base[vtx.idx()] = IndexType(values.size());
        values.push_back(TexCoord(mesh.position(vtx)[0], mesh.position(vtx)[1]));
    }
    // Extra duplicates for ring0 (seam at center-ring0) and ring2
    // (seam at center-ring2), with the same numeric values.
    std::vector<IndexType> duplicate(mesh.vertices_size(), kBad);
    for (const Vertex vtx : {ring[0], ring[2]})
    {
        duplicate[vtx.idx()] = IndexType(values.size());
        values.push_back(TexCoord(mesh.position(vtx)[0], mesh.position(vtx)[1]));
    }

    std::vector<IndexType> corners(mesh.halfedges_size(), 0);
    for (auto h : mesh.halfedges())
    {
        if (mesh.is_deleted(h))
            continue;
        const Vertex vtx = mesh.to_vertex(h);
        const Face face = mesh.face(h);
        IndexType index = base[vtx.idx()];
        if (vtx == center)
            index = 0; // one shared value index for every center corner
        else if (vtx == ring[0] && face == f3)
            index = duplicate[vtx.idx()];
        else if (vtx == ring[2] && face == f1)
            index = duplicate[vtx.idx()];
        corners[h.idx()] = index;
    }

    SubdivisionPlan plan(mesh, 1,
                         one_channel("fan", values, std::move(corners)));

    const IndexType center_id = plan.source_vertex_id(center);
    const auto per_geometry = channel_vertices_per_geometry(plan, 0);
    const auto& center_vertices = per_geometry.at(center_id);
    TASK_CHECK(center_vertices.size() == 2);

    // Both center channel vertices share the same source value index.
    for (IndexType cid = 0; cid < plan.source_corner_count(); ++cid)
    {
        if (plan.source_corner_vertex(cid) != center_id)
            continue;
        TASK_CHECK(plan.channel_source_value_indices(0)[cid] == 0);
    }

    // The two fans are not merged: the center corners map to two channel
    // control vertices, and each channel vertex stays in range.
    for (IndexType cid : center_vertices)
        TASK_CHECK(cid < plan.channel_control_vertex_count(0));
    plan.evaluate(compact_positions(mesh, plan), {values});
    for (const TexCoord& t : plan.channel_values(0))
        TASK_CHECK(std::isfinite(t[0]) && std::isfinite(t[1]));
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

#define REGISTER(group, name, fn) {group, name, fn}

const struct task_case task_cases[] = {
    REGISTER("topology", "closed_quad_cage", case_closed_quad_cage),
    REGISTER("topology", "valence_3_and_5", case_valence_3_and_5),
    REGISTER("topology", "components_and_holes", case_components_and_holes),
    REGISTER("topology", "levels_range", case_levels_range),
    REGISTER("topology", "input_validation", case_input_validation),
    REGISTER("stencils", "geometry_reference", case_geometry_reference),
    REGISTER("stencils", "boundary_uv_rules", case_boundary_uv_rules),
    REGISTER("stencils", "channel_reference_rounds",
             case_channel_reference_rounds),
    REGISTER("stencils", "stencil_weights", case_stencil_weights_and_provenance),
    REGISTER("stencils", "sparse_handles", case_sparse_handles),
    REGISTER("stencils", "features_do_not_affect_uv",
             case_features_do_not_affect_uv),
    REGISTER("reuse", "source_release_and_repeat", case_source_release_and_repeat),
    REGISTER("reuse", "two_plans_interleaved", case_two_plans_interleaved),
    REGISTER("reuse", "failure_preserves_previous",
             case_failure_preserves_previous),
    REGISTER("reuse", "two_disjoint_seams", case_two_disjoint_seams),
    REGISTER("reuse", "equal_values_different_indices",
             case_equal_values_different_indices),
    REGISTER("reuse", "split_fan", case_split_fan),
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
