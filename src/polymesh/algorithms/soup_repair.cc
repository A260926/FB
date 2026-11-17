#include "soup_repair.hh"

#include <algorithm>
#include <cstdint>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace polymesh
{
namespace soup
{
corner parse_obj_corner_token(std::string token)
{
    auto slashes = 0;
    auto const first_slash = token.find_first_of('/');
    auto const last_slash = token.find_last_of('/');
    for (auto& c : token)
        if (c == '/')
        {
            c = ' ';
            ++slashes;
        }

    std::istringstream ss(token);
    corner ref;
    switch (slashes)
    {
    case 0:
        ss >> ref.vertex;
        break;
    case 1:
        ss >> ref.vertex;
        ss >> ref.tex;
        break;
    case 2:
        ss >> ref.vertex;
        if (first_slash + 1 != last_slash) // "v/vt/vn", not "v//vn"
            ss >> ref.tex;
        ss >> ref.normal;
        break;
    }
    return ref;
}

namespace
{
struct directed_edge
{
    int face;
    int from;
    int to;
    int corner_to; ///< global corner index of `to`
};

uint64_t edge_key(int a, int b)
{
    if (a > b)
        std::swap(a, b);
    return (uint64_t(uint32_t(a)) << 32) | uint32_t(b);
}

/// lexicographically smallest rotation of a face vertex cycle
std::vector<int> min_rotation(std::vector<int> v)
{
    auto best = v;
    for (auto i = 1u; i < v.size(); ++i)
    {
        std::rotate(v.begin(), v.begin() + 1, v.end());
        if (v < best)
            best = v;
    }
    return best;
}

struct union_find
{
    explicit union_find(int n) : parent(n), rank(n, 0)
    {
        for (auto i = 0; i < n; ++i)
            parent[i] = i;
    }

    int find(int x)
    {
        while (parent[x] != x)
        {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    }

    void unite(int a, int b)
    {
        a = find(a);
        b = find(b);
        if (a == b)
            return;
        if (rank[a] < rank[b])
            std::swap(a, b);
        parent[b] = a;
        if (rank[a] == rank[b])
            ++rank[a];
    }

    std::vector<int> parent;
    std::vector<int> rank;
};
} // namespace

repaired_soup repair_face_soup(std::vector<face> const& faces, int vertex_count)
{
    if (vertex_count < 0)
        throw std::invalid_argument("soup repair: negative vertex count");

    auto const face_count = int(faces.size());

    std::vector<int> face_offset(face_count);
    std::vector<int> face_size(face_count);
    auto total_corners = 0;
    for (auto f = 0; f < face_count; ++f)
    {
        face_offset[f] = total_corners;
        face_size[f] = int(faces[f].corners.size());
        total_corners += face_size[f];
    }

    // ===== validation: face sizes, indices, per-face distinctness, duplicate faces =====
    std::set<std::vector<int>> seen_face_keys;
    for (auto f = 0; f < face_count; ++f)
    {
        auto const& poly = faces[f].corners;
        auto const k = face_size[f];
        if (k < 3)
            throw std::invalid_argument("soup repair: face has fewer than three corners");

        std::vector<int> verts;
        verts.reserve(k);
        std::unordered_set<int> distinct;
        for (auto c = 0; c < k; ++c)
        {
            auto const v = poly[c].vertex;
            if (v <= 0 || v > vertex_count)
                throw std::invalid_argument("soup repair: face vertex index out of range or zero");
            if (!distinct.insert(v).second)
                throw std::invalid_argument("soup repair: face contains a repeated vertex");
            verts.push_back(v);
        }

        auto canonical = min_rotation(verts);
        std::reverse(verts.begin(), verts.end());
        auto reversed = min_rotation(verts);
        if (reversed < canonical)
            canonical = std::move(reversed);
        if (!seen_face_keys.insert(canonical).second)
            throw std::invalid_argument("soup repair: duplicate face (equal up to rotation or reversal)");
    }

    // ===== build undirected-edge incidence =====
    std::unordered_map<uint64_t, std::vector<directed_edge>> edges;
    edges.reserve(size_t(total_corners) * 2);

    for (auto f = 0; f < face_count; ++f)
    {
        auto const& poly = faces[f].corners;
        auto const k = face_size[f];
        for (auto c = 0; c < k; ++c)
        {
            auto const from = poly[(c + k - 1) % k].vertex;
            auto const to = poly[c].vertex;
            edges[edge_key(from, to)].push_back({f, from, to, face_offset[f] + c});
        }
    }

    for (auto const& entry : edges)
        if (entry.second.size() > 2)
            throw std::invalid_argument("soup repair: edge shared by more than two faces");

    // ===== winding propagation across edge-connected components =====
    std::vector<std::vector<std::pair<int, int>>> constraints(face_count);
    for (auto const& entry : edges)
    {
        auto const& des = entry.second;
        if (des.size() != 2)
            continue;

        auto const& d0 = des[0];
        auto const& d1 = des[1];
        // shared edge must be traversed in opposite directions;
        // equal directions force one face to flip relative to the other
        auto const xor_flips = (d0.from == d1.from && d0.to == d1.to) ? 1 : 0;
        constraints[d0.face].push_back({d1.face, xor_flips});
        constraints[d1.face].push_back({d0.face, xor_flips});
    }

    std::vector<int> flip(face_count, -1); // -1 unvisited, 0 keep, 1 reverse
    for (auto root = 0; root < face_count; ++root)
    {
        if (flip[root] != -1)
            continue;

        // smallest source face of the component keeps its orientation
        flip[root] = 0;
        std::queue<int> queue;
        queue.push(root);

        while (!queue.empty())
        {
            auto const f = queue.front();
            queue.pop();
            for (auto const& nb : constraints[f])
            {
                auto const expected = flip[f] ^ nb.second;
                if (flip[nb.first] == -1)
                {
                    flip[nb.first] = expected;
                    queue.push(nb.first);
                }
                else if (flip[nb.first] != expected)
                {
                    throw std::invalid_argument("soup repair: contradictory winding constraints in face component");
                }
            }
        }
    }

    // ===== split vertices carrying several disconnected face fans =====
    union_find fan_unions(total_corners);

    // corners of one face always belong to the same fan
    for (auto f = 0; f < face_count; ++f)
        for (auto c = 0; c < face_size[f]; ++c)
            fan_unions.unite(face_offset[f] + c, face_offset[f] + ((c + 1) % face_size[f]));

    auto corner_at = [&](directed_edge const& d, int vertex) {
        auto const base = face_offset[d.face];
        auto const k = face_size[d.face];
        if (d.to == vertex)
            return d.corner_to;
        return base + (d.corner_to - base + k - 1) % k;
    };

    // corners meeting across a real shared edge belong to the same fan
    for (auto const& entry : edges)
    {
        auto const& des = entry.second;
        if (des.size() != 2)
            continue;

        auto const& d0 = des[0];
        auto const& d1 = des[1];
        auto const va = std::min(d0.from, d0.to);
        auto const vb = std::max(d0.from, d0.to);
        fan_unions.unite(corner_at(d0, va), corner_at(d1, va));
        fan_unions.unite(corner_at(d0, vb), corner_at(d1, vb));
    }

    // assign one output vertex slot per (source vertex, fan) pair
    std::vector<std::vector<int>> corners_at_vertex(vertex_count);
    for (auto f = 0; f < face_count; ++f)
        for (auto c = 0; c < face_size[f]; ++c)
            corners_at_vertex[faces[f].corners[c].vertex - 1].push_back(face_offset[f] + c);

    repaired_soup result;
    result.vertex_source.reserve(vertex_count);
    std::vector<int> corner_out_vertex(total_corners, -1); // global corner -> output vertex slot

    for (auto v = 0; v < vertex_count; ++v)
    {
        if (corners_at_vertex[v].empty())
        {
            // isolated source position is preserved exactly once
            result.vertex_source.push_back(v);
            continue;
        }

        std::unordered_map<int, int> root_to_slot;
        for (auto global_corner : corners_at_vertex[v])
        {
            auto const root = fan_unions.find(global_corner);
            auto it = root_to_slot.find(root);
            if (it == root_to_slot.end())
            {
                auto const slot = int(result.vertex_source.size());
                it = root_to_slot.emplace(root, slot).first;
                result.vertex_source.push_back(v);
            }
            corner_out_vertex[global_corner] = it->second;
        }
    }
    result.vertex_count = int(result.vertex_source.size());

    // ===== emit repaired faces in source order =====
    result.faces.resize(face_count);
    result.face_source.resize(face_count);
    for (auto f = 0; f < face_count; ++f)
    {
        auto const& poly = faces[f].corners;
        auto const k = face_size[f];
        result.face_source[f] = f;
        result.faces[f].corners.resize(k);

        for (auto p = 0; p < k; ++p)
        {
            // flipped faces reverse the cycle but keep the original first corner first
            auto const src_local = flip[f] ? (k - p) % k : p;
            auto const global_corner = face_offset[f] + src_local;
            auto const& src = poly[src_local];

            repaired_corner out;
            out.out_vertex = corner_out_vertex[global_corner];
            out.source_vertex = src.vertex - 1;
            out.source_corner = global_corner;
            out.tex = src.tex;
            out.normal = src.normal;
            result.faces[f].corners[p] = out;
        }
    }

    return result;
}
} // namespace soup
} // namespace polymesh
