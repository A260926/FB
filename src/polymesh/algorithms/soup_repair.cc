#include "soup_repair.hh"

#include <algorithm>
#include <map>
#include <queue>
#include <stdexcept>
#include <unordered_map>

#include "../detail/union_find.hh"

namespace polymesh
{
namespace
{
struct edge_use
{
    int face; // source face index
    int pos;  // source corner index of the edge start (edge goes corner[pos] -> corner[pos+1])
};

uint64_t edge_key(int u, int v)
{
    if (u > v)
        std::swap(u, v);
    return (uint64_t(uint32_t(u)) << 32) | uint32_t(v);
}

// lexicographically smallest rotation of a cyclic sequence
std::vector<int> least_rotation(std::vector<int> const& seq)
{
    auto const n = (int)seq.size();
    auto best = 0;
    for (auto k = 1; k < n; ++k)
    {
        for (auto i = 0; i < n; ++i)
        {
            auto const a = seq[(best + i) % n];
            auto const b = seq[(k + i) % n];
            if (b < a)
            {
                best = k;
                break;
            }
            if (b > a)
                break;
        }
    }

    std::vector<int> result;
    result.reserve(n);
    for (auto i = 0; i < n; ++i)
        result.push_back(seq[(best + i) % n]);
    return result;
}

// two faces are duplicates if their vertex rings match up to cyclic shift or
// reversal; canonicalize by the least rotation of both orientations
std::vector<int> canonical_ring(std::vector<int> const& seq)
{
    auto forward = least_rotation(seq);
    auto reversed = seq;
    std::reverse(reversed.begin(), reversed.end());
    reversed = least_rotation(reversed);
    return forward < reversed ? forward : reversed;
}
} // namespace

repaired_soup repair_polygon_soup(std::vector<soup_face> const& faces, int vertex_count, int texcoord_count, int normal_count)
{
    auto const face_count = (int)faces.size();

    if (vertex_count < 0 || texcoord_count < 0 || normal_count < 0)
        throw std::invalid_argument("repair_polygon_soup: negative attribute count");

    // ===== per-face validation and flat corner-slot bookkeeping =====

    std::vector<int> face_offset(face_count + 1, 0);
    std::vector<int> slot_vertex; // source vertex of every face corner slot
    std::vector<bool> vertex_used(vertex_count, false);

    slot_vertex.reserve((size_t)std::max(0, face_count) * 3);

    // a face is uniquely identified by its cyclic vertex ring; reused to
    // detect duplicates up to cyclic shift or reversal
    std::map<std::vector<int>, int> seen_rings;

    for (auto f = 0; f < face_count; ++f)
    {
        auto const& poly = faces[f].corners;
        auto const k = (int)poly.size();
        if (k < 3)
            throw std::invalid_argument("repair_polygon_soup: face with less than three vertices");

        face_offset[f] = (int)slot_vertex.size();

        std::vector<int> ring;
        ring.reserve(k);

        for (auto i = 0; i < k; ++i)
        {
            auto const& c = poly[i];

            if (c.vertex < 0 || c.vertex >= vertex_count)
                throw std::invalid_argument("repair_polygon_soup: vertex index out of range");
            if (c.texcoord != -1 && (c.texcoord < 0 || c.texcoord >= texcoord_count))
                throw std::invalid_argument("repair_polygon_soup: texcoord index out of range");
            if (c.normal != -1 && (c.normal < 0 || c.normal >= normal_count))
                throw std::invalid_argument("repair_polygon_soup: normal index out of range");

            for (auto j = 0; j < i; ++j)
                if (poly[j].vertex == c.vertex)
                    throw std::invalid_argument("repair_polygon_soup: repeated vertex within face");

            vertex_used[c.vertex] = true;
            slot_vertex.push_back(c.vertex);
            ring.push_back(c.vertex);
        }

        if (!seen_rings.emplace(canonical_ring(ring), f).second)
            throw std::invalid_argument("repair_polygon_soup: duplicate face");
    }
    face_offset[face_count] = (int)slot_vertex.size();

    auto const slot_count = (int)slot_vertex.size();

    // ===== undirected edge table (each source edge borders at most two faces) =====

    std::map<uint64_t, std::vector<edge_use>> edges;
    for (auto f = 0; f < face_count; ++f)
    {
        auto const& poly = faces[f].corners;
        auto const k = (int)poly.size();
        for (auto i = 0; i < k; ++i)
        {
            auto const u = poly[i].vertex;
            auto const v = poly[(i + 1) % k].vertex;
            auto& uses = edges[edge_key(u, v)];
            if ((int)uses.size() == 2)
                throw std::invalid_argument("repair_polygon_soup: edge shared by more than two faces");
            uses.push_back({f, i});
        }
    }

    // ===== orientation constraints along shared edges =====
    //
    // Consistently oriented faces traverse a shared edge in opposite
    // directions. sign[f] is +1 to keep the winding, -1 to flip it.
    // Two stored uses traverse in opposite directions exactly when the second
    // face starts at the first face's end vertex; then both signs must agree,
    // otherwise they must differ.

    struct constraint
    {
        int neighbor;
        int compatible; // +1: signs must agree, -1: signs must differ
    };
    std::vector<std::vector<constraint>> adjacency(face_count);

    for (auto const& entry : edges)
    {
        auto const& uses = entry.second;
        if (uses.size() != 2)
            continue;

        auto const a = uses[0];
        auto const b = uses[1];
        auto const ka = (int)faces[a.face].corners.size();
        auto const kb = (int)faces[b.face].corners.size();

        auto const a_from = faces[a.face].corners[a.pos].vertex;
        auto const a_to = faces[a.face].corners[(a.pos + 1) % ka].vertex;
        auto const b_from = faces[b.face].corners[b.pos].vertex;
        auto const b_to = faces[b.face].corners[(b.pos + 1) % kb].vertex;

        auto const compatible = (b_from == a_to && b_to == a_from) ? 1 : -1;

        adjacency[a.face].push_back({b.face, compatible});
        adjacency[b.face].push_back({a.face, compatible});
    }

    // BFS per edge-connected component; seeds are visited in ascending source
    // order, so the smallest source face of each component is anchored first
    // and keeps its original winding.
    std::vector<int> sign(face_count, 0);

    for (auto seed = 0; seed < face_count; ++seed)
    {
        if (sign[seed] != 0)
            continue;

        sign[seed] = 1;
        std::queue<int> queue;
        queue.push(seed);

        while (!queue.empty())
        {
            auto const f = queue.front();
            queue.pop();

            for (auto const& c : adjacency[f])
            {
                auto const required = sign[f] * c.compatible;
                if (sign[c.neighbor] == 0)
                {
                    sign[c.neighbor] = required;
                    queue.push(c.neighbor);
                }
                else if (sign[c.neighbor] != required)
                {
                    throw std::invalid_argument("repair_polygon_soup: contradictory winding constraints");
                }
            }
        }
    }

    // ===== face-fan equivalence of corner slots =====
    //
    // Corner slots meeting at one source vertex through a shared edge belong
    // to the same face fan and therefore share one output vertex. Fan
    // membership is independent of the final orientation.

    detail::disjoint_set fans(slot_count);

    for (auto const& entry : edges)
    {
        auto const& uses = entry.second;
        if (uses.size() != 2)
            continue;

        auto const a = uses[0];
        auto const b = uses[1];
        auto const ka = (int)faces[a.face].corners.size();
        auto const kb = (int)faces[b.face].corners.size();

        auto const a_u = a.pos;
        auto const a_v = (a.pos + 1) % ka;
        auto const u = faces[a.face].corners[a_u].vertex;
        auto const v = faces[a.face].corners[a_v].vertex;

        auto const b_u = (faces[b.face].corners[b.pos].vertex == u) ? b.pos : (b.pos + 1) % kb;
        auto const b_v = (faces[b.face].corners[b.pos].vertex == v) ? b.pos : (b.pos + 1) % kb;

        fans.do_union(face_offset[a.face] + a_u, face_offset[b.face] + b_u);
        fans.do_union(face_offset[a.face] + a_v, face_offset[b.face] + b_v);
    }

    // ===== build the repaired output =====

    repaired_soup result;
    result.faces.reserve(face_count);
    result.corners.reserve(face_count);
    result.flipped.reserve(face_count);
    result.face_source.reserve(face_count);
    result.halfedge_source.reserve(slot_count);

    std::unordered_map<int, int> fan_to_vertex;
    fan_to_vertex.reserve((size_t)slot_count * 2);

    auto emit_vertex = [&](int slot) {
        auto const root = fans.find(slot);
        auto const it = fan_to_vertex.find(root);
        if (it != fan_to_vertex.end())
            return it->second;

        auto const out = (int)result.vertex_source.size();
        fan_to_vertex.emplace(root, out);
        result.vertex_source.push_back(slot_vertex[slot]);
        return out;
    };

    for (auto f = 0; f < face_count; ++f)
    {
        auto const& poly = faces[f].corners;
        auto const k = (int)poly.size();
        auto const flipped = sign[f] < 0;

        std::vector<int> out_face;
        std::vector<soup_corner> out_corners;
        out_face.reserve(k);
        out_corners.reserve(k);

        for (auto i = 0; i < k; ++i)
        {
            // reversed faces still start at their original first corner
            auto const source_corner = flipped ? (k - i) % k : i;
            auto const slot = face_offset[f] + source_corner;

            out_face.push_back(emit_vertex(slot));
            out_corners.push_back(poly[source_corner]);
            result.halfedge_source.push_back(slot);
        }

        result.faces.push_back(std::move(out_face));
        result.corners.push_back(std::move(out_corners));
        result.flipped.push_back(flipped);
        result.face_source.push_back(f);
    }

    // isolated source positions are kept exactly once
    for (auto v = 0; v < vertex_count; ++v)
        if (!vertex_used[v])
        {
            result.isolated_vertices.push_back(v);
            result.vertex_source.push_back(v);
        }

    return result;
}
} // namespace polymesh
