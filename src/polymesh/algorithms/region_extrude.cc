#include "region_extrude.hh"

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace polymesh
{
namespace
{
[[noreturn]] void fail(char const* what) { throw std::invalid_argument(what); }

struct xyz
{
    double x, y, z;
};

inline xyz to_xyz(std::array<double, 4> const& p) { return {p[0], p[1], p[2]}; }
inline xyz sub(xyz const& a, xyz const& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline xyz cross(xyz const& a, xyz const& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline double length(xyz const& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
inline bool finite(xyz const& a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

/// newell normal of a (planar) polygon
inline xyz polygon_normal(xyz const* pts, int cnt)
{
    xyz n{0, 0, 0};
    for (auto i = 0; i < cnt; ++i)
    {
        auto const& a = pts[i];
        auto const& b = pts[(i + 1) % cnt];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    return n;
}

struct boundary_edge
{
    halfedge_index halfedge;
    face_index selected_face;
    double pre_length; // length before extrusion (used for wall UVs)
};
} // namespace

std::vector<face_handle> extrude_region(Mesh& m, //
                                        vertex_attribute<std::array<double, 4>>& position, //
                                        halfedge_attribute<std::array<double, 3>>& tex_coord, //
                                        halfedge_attribute<std::array<double, 3>>& normal, //
                                        std::vector<face_handle> const& region, //
                                        std::array<double, 3> delta)
{
    // an empty region is a no-op (nothing is modified, no argument is inspected further)
    if (region.empty())
        return {};

    auto const ll = low_level_api(m);

    auto const f_cnt = ll.size_all_faces();
    auto const v_cnt = ll.size_all_vertices();
    auto const h_cnt = ll.size_all_halfedges();
    auto const e_cnt = ll.size_all_edges();

    // ======== VALIDATION (must not touch the mesh) ========

    auto const delta_v = xyz{delta[0], delta[1], delta[2]};
    if (!finite(delta_v))
        fail("extrude_region: displacement is not finite");

    auto const delta_len = length(delta_v);
    if (!(delta_len > 0.0))
        fail("extrude_region: displacement must not be zero");

    std::vector<char> face_selected(size_t(f_cnt), 0);
    for (auto f : region)
    {
        if (f.mesh != &m)
            fail("extrude_region: face handle belongs to a different mesh");
        if (!f.idx.is_valid() || f.idx.value >= f_cnt)
            fail("extrude_region: invalid face handle");
        if (ll.is_removed(f.idx))
            fail("extrude_region: face handle was removed");
        if (face_selected[size_t(f.idx.value)])
            fail("extrude_region: duplicated face handle");
        face_selected[size_t(f.idx.value)] = 1;
    }

    // vertices that are incident to at least one selected face
    std::vector<char> vertex_selected(size_t(v_cnt), 0);
    for (auto f : region)
        for (auto h : f.halfedges())
            vertex_selected[size_t(h.vertex_to().idx.value)] = 1;

    // collect directed region-boundary half-edges
    // (selected face on this side, unselected or missing face on the other side)
    std::vector<boundary_edge> boundary;
    std::vector<int> boundary_of_halfedge(size_t(h_cnt), -1);
    boundary.reserve(size_t(h_cnt));

    for (auto f : region)
    {
        for (auto h : f.halfedges())
        {
            auto const opposite_face = h.opposite_face();
            if (!opposite_face.is_valid())
                fail("extrude_region: region boundary lies on the mesh boundary");

            if (face_selected[size_t(opposite_face.idx.value)])
                continue;

            if (boundary_of_halfedge[size_t(h.idx.value)] >= 0)
                fail("extrude_region: duplicated region boundary half-edge");

            auto const p_from = to_xyz(position[h.vertex_from()]);
            auto const p_to = to_xyz(position[h.vertex_to()]);
            auto const edge_vec = sub(p_to, p_from);

            // a wall that is parallel to the displacement degenerates to zero area
            auto const cr = cross(delta_v, edge_vec);
            auto const cr_len = length(cr);
            auto const edge_len = length(edge_vec);
            if (!(cr_len > 1e-9 * delta_len * edge_len))
                fail("extrude_region: displacement is parallel to a region boundary edge");

            boundary_of_halfedge[size_t(h.idx.value)] = int(boundary.size());
            boundary.push_back({h.idx, f.idx, edge_len});
        }
    }

    // every region boundary vertex must have exactly one outgoing and one incoming boundary half-edge
    // (this also enforces that selected faces form a single contiguous fan at each region vertex)
    std::vector<int> outgoing_boundary(size_t(v_cnt), -1);
    std::vector<int> incoming_boundary(size_t(v_cnt), -1);
    for (auto i = 0u; i < boundary.size(); ++i)
    {
        auto const h = boundary[i].halfedge;
        auto const from = ll.from_vertex_of(h).value;
        auto const to = ll.to_vertex_of(h).value;

        if (outgoing_boundary[size_t(from)] >= 0)
            fail("extrude_region: region boundary touches itself (multiple outgoing boundary half-edges)");
        if (incoming_boundary[size_t(to)] >= 0)
            fail("extrude_region: region boundary touches itself (multiple incoming boundary half-edges)");

        outgoing_boundary[size_t(from)] = int(i);
        incoming_boundary[size_t(to)] = int(i);
    }
    for (auto v = 0; v < v_cnt; ++v)
        if ((outgoing_boundary[size_t(v)] >= 0) != (incoming_boundary[size_t(v)] >= 0))
            fail("extrude_region: region boundary is not closed");

    // vertices with no boundary half-edge must be completely surrounded by selected faces
    for (auto v = 0; v < v_cnt; ++v)
    {
        if (!vertex_selected[size_t(v)] || outgoing_boundary[size_t(v)] >= 0)
            continue;

        auto const vh = m.handle_of(vertex_index(v));
        for (auto h : vh.outgoing_halfedges())
        {
            auto const f = h.face();
            if (!f.is_valid() || !face_selected[size_t(f.idx.value)])
                fail("extrude_region: selected faces do not form a single contiguous fan at a vertex");
        }
    }

    // trace boundary loops and make sure they are simple closed loops
    std::vector<std::vector<int>> loops;
    {
        std::vector<char> visited(boundary.size(), 0);
        for (auto start = 0u; start < boundary.size(); ++start)
        {
            if (visited[start])
                continue;

            std::vector<int> loop;
            auto cur = int(start);
            while (true)
            {
                if (cur < 0 || size_t(cur) >= boundary.size())
                    fail("extrude_region: region boundary is not a closed loop");
                if (visited[size_t(cur)])
                {
                    if (cur != int(start))
                        fail("extrude_region: region boundary loops touch each other");
                    break;
                }

                visited[size_t(cur)] = 1;
                loop.push_back(cur);

                auto const to = ll.to_vertex_of(boundary[size_t(cur)].halfedge).value;
                auto const next = outgoing_boundary[size_t(to)];
                if (next < 0)
                    fail("extrude_region: region boundary is not closed");
                cur = next;
            }

            if (loop.size() < 3)
                fail("extrude_region: region boundary loop is degenerate");

            loops.push_back(std::move(loop));
        }
    }

    // capture original face loops and internal region edges before changing anything
    std::vector<std::vector<halfedge_index>> original_loops(region.size());
    std::vector<std::vector<vertex_index>> original_vertices(region.size());
    for (auto k = 0u; k < region.size(); ++k)
    {
        original_loops[k].reserve(size_t(region[k].halfedges().size()));
        original_vertices[k].reserve(size_t(region[k].halfedges().size()));
        for (auto h : region[k].halfedges())
        {
            original_loops[k].push_back(h.idx);
            original_vertices[k].push_back(h.vertex_to().idx);
        }
    }

    std::vector<char> internal_edge(size_t(e_cnt), 0);
    for (auto f : region)
        for (auto h : f.halfedges())
        {
            auto const opposite_face = h.opposite_face();
            if (opposite_face.is_valid() && face_selected[size_t(opposite_face.idx.value)])
                internal_edge[size_t(h.edge().idx.value)] = 1;
        }

    // ======== MUTATION ========
    // All further operations are non-throwing under the validated preconditions.

    // new cap vertices for every boundary vertex
    std::vector<vertex_index> cap_of_vertex(size_t(v_cnt), vertex_index::invalid);
    for (auto v = 0; v < v_cnt; ++v)
        if (outgoing_boundary[size_t(v)] >= 0)
            cap_of_vertex[size_t(v)] = ll.add_vertex();

    // 1) free the selected faces and remove their internal edges
    for (auto f : region)
        ll.remove_face(f.idx);

    for (auto e = 0; e < e_cnt; ++e)
        if (internal_edge[size_t(e)])
            ll.remove_edge(edge_index(e));

    // 2) rebuild the cap faces (same winding, same face handles)
    std::vector<std::vector<halfedge_index>> cap_loops(region.size());
    std::vector<face_handle> result;
    result.reserve(region.size());

    std::vector<halfedge_index> cap_halfedge_of_boundary(boundary.size(), halfedge_index::invalid);

    for (auto k = 0u; k < region.size(); ++k)
    {
        auto const& hs = original_loops[k];
        auto const& vs = original_vertices[k];
        auto const n = int(hs.size());

        std::vector<vertex_index> ws(static_cast<size_t>(n));
        for (auto i = 0; i < n; ++i)
        {
            auto const v = vs[size_t(i)];
            auto const cap = cap_of_vertex[size_t(v.value)];
            ws[size_t(i)] = cap.is_valid() ? cap : v;
        }

        auto& cap_hs = cap_loops[k];
        cap_hs.resize(size_t(n));
        for (auto i = 0; i < n; ++i)
            cap_hs[size_t(i)] = ll.add_or_get_halfedge(ws[size_t((i + n - 1) % n)], ws[size_t(i)]);

        auto const nf = ll.add_face(cap_hs.data(), n, region[k].idx);
        result.push_back(m.handle_of(nf));

        // remember which top half-edge belongs to which boundary edge
        for (auto i = 0; i < n; ++i)
        {
            auto const slot = boundary_of_halfedge[size_t(hs[size_t(i)].value)];
            if (slot >= 0)
                cap_halfedge_of_boundary[size_t(slot)] = cap_hs[size_t(i)];
        }
    }

    // 3) vertical edges at every boundary vertex
    std::vector<halfedge_index> vertical_up(size_t(v_cnt), halfedge_index::invalid);   // v  -> v'
    std::vector<halfedge_index> vertical_down(size_t(v_cnt), halfedge_index::invalid); // v' -> v

    for (auto v = 0; v < v_cnt; ++v)
    {
        if (outgoing_boundary[size_t(v)] < 0)
            continue;

        auto const cap = cap_of_vertex[size_t(v)];
        auto const ve = ll.add_or_get_edge(vertex_index(v), cap);
        auto const h0 = ll.halfedge_of(ve, 0);
        auto const h1 = ll.halfedge_of(ve, 1);
        auto const up = ll.to_vertex_of(h0) == cap ? h0 : h1;
        vertical_up[size_t(v)] = up;
        vertical_down[size_t(v)] = ll.opposite(up);

        // vertical edges keep their default values on all attributes
        m.reset_edge_slot(ve);
    }

    // 4) wall quads, one per directed region boundary half-edge
    std::vector<face_index> wall_of_boundary(boundary.size(), face_index::invalid);

    for (auto bi = 0u; bi < boundary.size(); ++bi)
    {
        auto const h = boundary[bi].halfedge;
        auto const top_h = cap_halfedge_of_boundary[bi];
        POLYMESH_ASSERT(top_h.is_valid() && "validated boundary must have a corresponding cap edge");

        auto const top_opp = ll.opposite(top_h);
        auto const ver_y = vertical_up[size_t(ll.to_vertex_of(h).value)];
        auto const ver_x = vertical_down[size_t(ll.from_vertex_of(h).value)];
        POLYMESH_ASSERT(ver_y.is_valid() && ver_x.is_valid());

        halfedge_index loop[4] = {h, ver_y, top_opp, ver_x};
        wall_of_boundary[bi] = ll.add_face(loop, 4);
    }

    // 5) attribute inheritance
    for (auto v = 0; v < v_cnt; ++v)
    {
        if (!vertex_selected[size_t(v)])
            continue;

        auto const cap = cap_of_vertex[size_t(v)];
        if (cap.is_valid())
        {
            m.copy_vertex_slots(vertex_index(v), cap);

            auto p = position[vertex_index(v)];
            p[0] += delta[0];
            p[1] += delta[1];
            p[2] += delta[2];
            position[cap] = p;
        }
        else
        {
            auto& p = position[vertex_index(v)];
            p[0] += delta[0];
            p[1] += delta[1];
            p[2] += delta[2];
        }
    }

    for (auto k = 0u; k < region.size(); ++k)
    {
        auto const& hs = original_loops[k];
        auto const& cap_hs = cap_loops[k];
        for (auto i = 0u; i < hs.size(); ++i)
        {
            // index i of the cap loop corresponds exactly to index i of the original loop
            // (same winding, same vertex correspondence). this also covers the internal edges
            // that were removed and re-created between cap vertices.
            m.copy_halfedge_slots(hs[i], cap_hs[i]);
            m.copy_edge_slots(ll.edge_of(hs[i]), ll.edge_of(cap_hs[i]));
        }
    }

    for (auto bi = 0u; bi < boundary.size(); ++bi)
        m.copy_face_slots(boundary[bi].selected_face, wall_of_boundary[bi]);

    // 6) wall UVs and normals (corner attributes are indexed by halfedge.vertex_to)
    for (auto const& loop : loops)
    {
        auto const n = int(loop.size());

        // start at the smallest boundary half-edge index of this loop
        auto first = 0;
        for (auto i = 1; i < n; ++i)
            if (boundary[size_t(loop[size_t(i)])].halfedge.value < boundary[size_t(loop[size_t(first)])].halfedge.value)
                first = i;

        std::vector<double> u(size_t(n) + 1);
        u[0] = 0.0;
        for (auto i = 0; i < n; ++i)
            u[size_t(i) + 1] = u[size_t(i)] + boundary[size_t(loop[size_t((first + i) % n)])].pre_length;

        for (auto i = 0; i < n; ++i)
        {
            auto const bi = loop[size_t((first + i) % n)];
            auto const h = boundary[size_t(bi)].halfedge;
            auto const top_opp = ll.opposite(cap_halfedge_of_boundary[size_t(bi)]);
            auto const ver_y = vertical_up[size_t(ll.to_vertex_of(h).value)];
            auto const ver_x = vertical_down[size_t(ll.from_vertex_of(h).value)];

            auto const u_start = u[size_t(i)];
            auto const u_end = u[size_t(i) + 1];

            // wall corners: all corner attributes keep their defaults (the reused bottom
            // half-edge would otherwise still carry the old selected-face corner values)
            m.reset_halfedge_slot(h);
            m.reset_halfedge_slot(ver_y);
            m.reset_halfedge_slot(top_opp);
            m.reset_halfedge_slot(ver_x);

            // bottom rim: h (at to-vertex) and ver_x (at from-vertex)
            // top rim:    ver_y (at to-vertex) and top_opp (at from-vertex)
            tex_coord[h] = {u_end, 0.0, 0.0};
            tex_coord[ver_y] = {u_end, delta_len, 0.0};
            tex_coord[top_opp] = {u_start, delta_len, 0.0};
            tex_coord[ver_x] = {u_start, 0.0, 0.0};

            xyz pts[4] = {
                to_xyz(position[ll.to_vertex_of(h)]),
                to_xyz(position[ll.to_vertex_of(ver_y)]),
                to_xyz(position[ll.to_vertex_of(top_opp)]),
                to_xyz(position[ll.to_vertex_of(ver_x)]),
            };

            auto nrm = polygon_normal(pts, 4);
            auto const l = length(nrm);
            std::array<double, 3> unit = {0.0, 0.0, 0.0};
            if (l > 0.0)
                unit = {nrm.x / l, nrm.y / l, nrm.z / l};

            normal[h] = unit;
            normal[ver_y] = unit;
            normal[top_opp] = unit;
            normal[ver_x] = unit;
        }
    }

    return result;
}
} // namespace polymesh
