#pragma once

#include "math/vector2.h"
#include "math/vector3.h"
#include "math/vector4.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

/*
 * Per-face (quad) texture resources.
 *
 * Every quadrilateral face owns an independent linear float Vec4 image.
 * Faces never share resolution and no atlas is built. Each face generates
 * its own mip chain; every level is stored together with a one-cell gutter
 * ring that allows seamless bilinear sampling across shared edges.
 *
 * The published material state is an immutable, reference-counted snapshot.
 * Replacing one face only rebuilds that face, its edge neighbours and the
 * faces in the vertex stars of its four corners; every other layer keeps
 * its storage identity and is shared between the old and new snapshot.
 */

using FaceTexel = Vec4; /* linear floating point RGBA */

struct FaceQuad
{
    /* shared position indices; local corners are
     * (0,0) (1,0) (1,1) (0,1) in this order */
    std::array<unsigned int, 4> vertices;
};

struct FaceImageSpec
{
    int width = 0;
    int height = 0;
    std::vector<FaceTexel> texels; /* row major, width*height entries */
};

/* One mip level of one face: interior (width x height) plus a gutter ring.
 * Gutter coordinate gx in [0, width+1), gy in [0, height+1); the interior
 * texel (x,y) lives at (x+1, y+1). The object is immutable once published. */
struct FaceLayer
{
    FaceLayer(int interiorWidth, int interiorHeight)
        : width(interiorWidth), height(interiorHeight),
          data(static_cast<size_t>(interiorWidth + 2) * (interiorHeight + 2), FaceTexel())
    {}

    const FaceTexel& at(int gx, int gy) const
    {
        return data[static_cast<size_t>(gy) * (width + 2) + gx];
    }

    FaceTexel& at(int gx, int gy)
    {
        return data[static_cast<size_t>(gy) * (width + 2) + gx];
    }

    int width;
    int height;
    std::vector<FaceTexel> data;
};

/* Immutable material snapshot. Stays valid (and drawable) after the source
 * FaceMaterial and every user supplied input array have been destroyed. */
struct FaceSnapshot
{
    std::vector<FaceQuad> quads;
    /* layers[face][level], level 0 is the base level */
    std::vector<std::vector<std::shared_ptr<const FaceLayer>>> layers;
};

/* Vertex generated for quad drawing. Positions are looked up through the
 * shared topology indices of the snapshot, so one source position feeds
 * every incident face. The face id is a flat constant replicated on the
 * four corners of a face: perspective interpolation therefore reproduces
 * the exact face number in both sub-triangles, instead of misusing an
 * interpolatable vertex index as a face number. */
struct FaceMeshVertex
{
    Vec4 position;
    Vec2 uv;
    float face;
};

/* Quad grid together with one clip-space position per shared topology
 * vertex (positions may carry distinct w for perspective interpolation). */
struct FaceQuadMesh
{
    std::shared_ptr<const FaceSnapshot> snapshot;
    std::vector<Vec4> positions;
};

FaceTexel face_texture_lod(const FaceSnapshot& snapshot, unsigned int face, const Vec2& uv, float lod);

namespace face_detail
{
    constexpr unsigned int INVALID_FACE = static_cast<unsigned int>(-1);

    inline const std::array<Vec2, 4>& corner_uvs()
    {
        static const std::array<Vec2, 4> uvs = { Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f),
                                                 Vec2(1.0f, 1.0f), Vec2(0.0f, 1.0f) };
        return uvs;
    }

    inline bool is_power_of_two(int n)
    {
        return n >= 1 && (n & (n - 1)) == 0;
    }

    inline uint64_t edge_key(unsigned int a, unsigned int b)
    {
        const auto lo = static_cast<uint32_t>(std::min(a, b));
        const auto hi = static_cast<uint32_t>(std::max(a, b));
        return (static_cast<uint64_t>(lo) << 32) | static_cast<uint64_t>(hi);
    }

    struct EdgeInfo
    {
        unsigned int faceA = INVALID_FACE;
        int edgeA = -1;
        int dirA = 0; /* +1: face traverses canonical (min,max) order, -1 otherwise */
        unsigned int faceB = INVALID_FACE;
        int edgeB = -1;
        int dirB = 0;
    };

    struct StarEntry
    {
        unsigned int face;
        int corner;
    };

    inline std::pair<unsigned int, unsigned int> edge_endpoints(const FaceQuad& q, int edge)
    {
        return { q.vertices[edge], q.vertices[(edge + 1) % 4] };
    }

    /* (x, y) of the base texel touching a given corner */
    inline std::pair<int, int> corner_texel(int corner, int width, int height)
    {
        const int x = (corner == 0 || corner == 3) ? 0 : width - 1;
        const int y = (corner < 2) ? 0 : height - 1;
        return { x, y };
    }

    inline int level_count(int width, int height)
    {
        return 1 + static_cast<int>(std::floor(std::log2(static_cast<double>(std::max(width, height)))));
    }

    inline std::pair<int, int> level_size(int baseWidth, int baseHeight, int level)
    {
        int w = baseWidth;
        int h = baseHeight;
        for(int i = 0; i < level; i++)
        {
            w = std::max(1, w / 2);
            h = std::max(1, h / 2);
        }
        return { w, h };
    }

    /* Point on the directed local edge of face B: t runs along the edge
     * direction, depth grows into the face from the edge. */
    inline Vec2 map_across_edge(int edgeB, float t, float depth)
    {
        switch(edgeB)
        {
        case 0: return Vec2(t, depth);
        case 1: return Vec2(1.0f - depth, t);
        case 2: return Vec2(1.0f - t, 1.0f - depth);
        default: return Vec2(depth, 1.0f - t);
        }
    }

    /* Exact area weighted average of the piecewise constant base texels of
     * a neighbour face covered by the rectangle [u0,u1] x [v0,v1]. */
    inline FaceTexel coverage_average(const FaceImageSpec& base, float u0, float v0, float u1, float v1)
    {
        u0 = std::clamp(u0, 0.0f, 1.0f);
        u1 = std::clamp(u1, 0.0f, 1.0f);
        v0 = std::clamp(v0, 0.0f, 1.0f);
        v1 = std::clamp(v1, 0.0f, 1.0f);
        if(u1 <= u0 || v1 <= v0) return FaceTexel();

        const int ix0 = std::max(0, static_cast<int>(std::floor(u0 * base.width)) - 1);
        const int ix1 = std::min(base.width - 1, static_cast<int>(std::floor(u1 * base.width)) + 1);
        const int iy0 = std::max(0, static_cast<int>(std::floor(v0 * base.height)) - 1);
        const int iy1 = std::min(base.height - 1, static_cast<int>(std::floor(v1 * base.height)) + 1);

        double acc[4] = { 0.0, 0.0, 0.0, 0.0 };
        double totalWeight = 0.0;

        for(int iy = iy0; iy <= iy1; iy++)
        {
            const float ty0 = static_cast<float>(iy) / base.height;
            const float ty1 = static_cast<float>(iy + 1) / base.height;
            const double oy = std::max(0.0, static_cast<double>(std::min(v1, ty1) - std::max(v0, ty0)));
            if(oy <= 0.0) continue;

            for(int ix = ix0; ix <= ix1; ix++)
            {
                const float tx0 = static_cast<float>(ix) / base.width;
                const float tx1 = static_cast<float>(ix + 1) / base.width;
                const double ox = std::max(0.0, static_cast<double>(std::min(u1, tx1) - std::max(u0, tx0)));
                if(ox <= 0.0) continue;

                const double weight = ox * oy;
                const FaceTexel& texel = base.texels[static_cast<size_t>(iy) * base.width + ix];
                acc[0] += weight * texel.x;
                acc[1] += weight * texel.y;
                acc[2] += weight * texel.z;
                acc[3] += weight * texel.w;
                totalWeight += weight;
            }
        }

        if(totalWeight <= 0.0) return FaceTexel();
        return FaceTexel(static_cast<float>(acc[0] / totalWeight),
                         static_cast<float>(acc[1] / totalWeight),
                         static_cast<float>(acc[2] / totalWeight),
                         static_cast<float>(acc[3] / totalWeight));
    }

    /* Bilinear sample of one gutter inclusive layer. UV is restricted to the
     * unit square; continuous texel coordinates are uv*size - 0.5 so that
     * gutter cells participate in edge interpolation. */
    inline FaceTexel sample_layer(const FaceLayer& layer, const Vec2& uv)
    {
        const float u = std::clamp(uv.x, 0.0f, 1.0f);
        const float v = std::clamp(uv.y, 0.0f, 1.0f);

        const float fx = std::clamp(u * layer.width - 0.5f, -0.5f, layer.width - 0.5f);
        const float fy = std::clamp(v * layer.height - 0.5f, -0.5f, layer.height - 0.5f);

        const int x0 = static_cast<int>(std::floor(fx));
        const int y0 = static_cast<int>(std::floor(fy));
        const float tx = fx - static_cast<float>(x0);
        const float ty = fy - static_cast<float>(y0);

        const int gx0 = x0 + 1;
        const int gy0 = y0 + 1;

        const auto& c00 = layer.at(gx0, gy0);
        const auto& c10 = layer.at(gx0 + 1, gy0);
        const auto& c01 = layer.at(gx0, gy0 + 1);
        const auto& c11 = layer.at(gx0 + 1, gy0 + 1);

        const FaceTexel l0 = c00 * (1.0f - tx) + c10 * tx;
        const FaceTexel l1 = c01 * (1.0f - tx) + c11 * tx;
        return l0 * (1.0f - ty) + l1 * ty;
    }
}

class FaceMaterial
{
public:
    FaceMaterial(std::vector<FaceQuad> quads, unsigned int vertexCount, std::vector<FaceImageSpec> images)
    {
        if(quads.empty()) throw std::invalid_argument("FaceMaterial: at least one face is required");
        if(images.size() != quads.size()) throw std::invalid_argument("FaceMaterial: one image per face is required");
        if(vertexCount == 0) throw std::invalid_argument("FaceMaterial: vertex count must be positive");

        for(const auto& image : images)
        {
            validate_spec(image);
        }

        m_quads = std::move(quads);
        m_base = std::move(images);
        m_vertexCount = vertexCount;

        build_topology();   /* throws on invalid topology */
        m_snapshot = build_snapshot();
        m_rebuildCount = 1;
    }

    unsigned int face_count() const { return static_cast<unsigned int>(m_quads.size()); }
    unsigned int vertex_count() const { return m_vertexCount; }
    const std::vector<FaceQuad>& quads() const { return m_quads; }
    unsigned long long rebuild_count() const { return m_rebuildCount; }

    std::shared_ptr<const FaceSnapshot> snapshot() const { return m_snapshot; }

    /* Storage identity of a published layer; identical for layers shared
     * between snapshots, distinct for rebuilt layers. */
    const void* layer_storage(unsigned int face, unsigned int level) const
    {
        return m_snapshot->layers.at(face).at(level).get();
    }

    std::pair<int, int> face_size(unsigned int face) const
    {
        const auto& image = m_base.at(face);
        return { image.width, image.height };
    }

    /* Replace the base level of one face. Returns false (and publishes
     * nothing) when the request is invalid; the previous snapshot stays
     * usable in every respect. */
    bool replace_face(unsigned int face, int width, int height, const std::vector<FaceTexel>& texels)
    {
        if(face >= m_quads.size()) return false;

        FaceImageSpec candidate;
        candidate.width = width;
        candidate.height = height;
        candidate.texels = texels;
        if(!valid_spec(candidate)) return false;

        FaceImageSpec previous = m_base[face];
        try
        {
            auto next = std::make_shared<FaceSnapshot>(*m_snapshot); /* shares every layer */

            m_base[face] = std::move(candidate);

            /* full rebuild of the changed face (interior mips and all gutters) */
            next->layers[face] = build_face_layers(face);

            /* faces whose data this update touches */
            std::vector<unsigned int> affected;
            collect_affected(face, affected);

            for(unsigned int other : affected)
            {
                if(other == face) continue;
                patch_face(next->layers[other], other, face);
            }

            m_snapshot = std::move(next);
            ++m_rebuildCount;
            return true;
        }
        catch(...)
        {
            /* no half-finished state is published; keep the previous snapshot */
            m_base[face] = previous;
            throw;
        }
    }

    /* Sample the current snapshot with an explicit, finite LOD. */
    FaceTexel sample(unsigned int face, const Vec2& uv, float lod) const
    {
        return face_texture_lod(*m_snapshot, face, uv, lod);
    }

private:
    static void validate_spec(const FaceImageSpec& image)
    {
        if(!valid_spec(image))
        {
            throw std::invalid_argument("FaceMaterial: face image must use positive power-of-two dimensions matching its texel count");
        }
    }

    static bool valid_spec(const FaceImageSpec& image)
    {
        if(!face_detail::is_power_of_two(image.width) || !face_detail::is_power_of_two(image.height)) return false;
        if(image.texels.size() != static_cast<size_t>(image.width) * image.height) return false;
        return true;
    }

    void build_topology()
    {
        m_edges.clear();
        m_stars.assign(m_vertexCount, {});

        for(unsigned int face = 0; face < m_quads.size(); face++)
        {
            const FaceQuad& quad = m_quads[face];
            for(int corner = 0; corner < 4; corner++)
            {
                if(quad.vertices[corner] >= m_vertexCount)
                {
                    throw std::invalid_argument("FaceMaterial: quad references an unknown vertex");
                }
            }

            for(int corner = 0; corner < 4; corner++)
            {
                for(int other = corner + 1; other < 4; other++)
                {
                    if(quad.vertices[corner] == quad.vertices[other])
                    {
                        throw std::invalid_argument("FaceMaterial: quad corners must be distinct vertices");
                    }
                }
            }

            for(int corner = 0; corner < 4; corner++)
            {
                m_stars[quad.vertices[corner]].push_back({ face, corner });
            }

            for(int edge = 0; edge < 4; edge++)
            {
                const auto [a, b] = face_detail::edge_endpoints(quad, edge);
                const uint64_t key = face_detail::edge_key(a, b);
                const int direction = (a < b) ? 1 : -1;

                auto [it, inserted] = m_edges.try_emplace(key);
                face_detail::EdgeInfo& info = it->second;

                if(inserted)
                {
                    info.faceA = face;
                    info.edgeA = edge;
                    info.dirA = direction;
                }
                else
                {
                    if(info.faceB != face_detail::INVALID_FACE)
                    {
                        throw std::invalid_argument("FaceMaterial: an edge must not be shared by more than two faces");
                    }
                    if(info.dirA == direction)
                    {
                        throw std::invalid_argument("FaceMaterial: a shared edge must be traversed in opposite directions");
                    }
                    info.faceB = face;
                    info.edgeB = edge;
                    info.dirB = direction;
                }
            }
        }
    }

    std::shared_ptr<FaceSnapshot> build_snapshot()
    {
        auto snapshot = std::make_shared<FaceSnapshot>();
        snapshot->quads = m_quads;
        snapshot->layers.resize(m_quads.size());
        for(unsigned int face = 0; face < m_quads.size(); face++)
        {
            snapshot->layers[face] = build_face_layers(face);
        }
        return snapshot;
    }

    /* Build the full mip chain (interior plus gutter ring) of one face. */
    std::vector<std::shared_ptr<const FaceLayer>> build_face_layers(unsigned int face)
    {
        const FaceImageSpec& base = m_base[face];
        const int levels = face_detail::level_count(base.width, base.height);

        std::vector<std::vector<FaceTexel>> interior(levels);
        interior[0] = base.texels;

        for(int level = 1; level < levels; level++)
        {
            const auto [pw, ph] = face_detail::level_size(base.width, base.height, level - 1);
            const auto [w, h] = face_detail::level_size(base.width, base.height, level);
            interior[level].resize(static_cast<size_t>(w) * h);

            for(int y = 0; y < h; y++)
            {
                for(int x = 0; x < w; x++)
                {
                    FaceTexel value;
                    for(int yy = 0; yy < 2; yy++)
                    {
                        for(int xx = 0; xx < 2; xx++)
                        {
                            const int px = std::min(x * 2 + xx, pw - 1); /* axes already at 1 repeat */
                            const int py = std::min(y * 2 + yy, ph - 1);
                            value += interior[level - 1][static_cast<size_t>(py) * pw + px];
                        }
                    }
                    interior[level][static_cast<size_t>(y) * w + x] = value * 0.25f;
                }
            }
        }

        std::vector<std::shared_ptr<const FaceLayer>> result;
        result.reserve(levels);
        for(int level = 0; level < levels; level++)
        {
            const auto [w, h] = face_detail::level_size(base.width, base.height, level);
            auto layer = std::make_shared<FaceLayer>(w, h);

            for(int y = 0; y < h; y++)
            {
                for(int x = 0; x < w; x++)
                {
                    layer->at(x + 1, y + 1) = interior[level][static_cast<size_t>(y) * w + x];
                }
            }

            fill_edge_gutters(*layer, face);
            fill_corner_gutters(*layer, face);
            result.push_back(std::move(layer));
        }
        return result;
    }

    void fill_edge_gutters(FaceLayer& layer, unsigned int face)
    {
        const FaceQuad& quad = m_quads[face];
        const int w = layer.width;
        const int h = layer.height;

        for(int edge = 0; edge < 4; edge++)
        {
            const auto [a, b] = face_detail::edge_endpoints(quad, edge);
            const auto it = m_edges.find(face_detail::edge_key(a, b));
            const face_detail::EdgeInfo& info = it->second;

            const bool boundary = info.faceB == face_detail::INVALID_FACE;
            const unsigned int neighbour = boundary ? face_detail::INVALID_FACE
                                                    : (info.faceA == face ? info.faceB : info.faceA);
            const int neighbourEdge = boundary ? -1 : (info.faceA == face ? info.edgeB : info.edgeA);

            const int cells = (edge == 0 || edge == 2) ? w : h;
            const float depth = (edge == 0 || edge == 2) ? 1.0f / h : 1.0f / w;

            for(int k = 0; k < cells; k++)
            {
                const auto [gx, gy] = edge_gutter_cell(edge, k, w, h);

                if(boundary)
                {
                    /* boundary gutter extends the adjacent edge texel */
                    const auto [ix, iy] = edge_gutter_interior(edge, k, w, h);
                    layer.at(gx, gy) = layer.at(ix + 1, iy + 1);
                    continue;
                }

                const float s0 = static_cast<float>(k) / cells;
                const float s1 = static_cast<float>(k + 1) / cells;
                /* shared edge runs in the opposite direction on the neighbour */
                const float t0 = 1.0f - s1;
                const float t1 = 1.0f - s0;

                const Vec2 p0 = face_detail::map_across_edge(neighbourEdge, t0, 0.0f);
                const Vec2 p1 = face_detail::map_across_edge(neighbourEdge, t1, depth);
                const float u0 = std::min(p0.x, p1.x);
                const float u1 = std::max(p0.x, p1.x);
                const float v0 = std::min(p0.y, p1.y);
                const float v1 = std::max(p0.y, p1.y);

                layer.at(gx, gy) = face_detail::coverage_average(m_base[neighbour], u0, v0, u1, v1);
            }
        }
    }

    void fill_corner_gutters(FaceLayer& layer, unsigned int face, int corner = -1)
    {
        const FaceQuad& quad = m_quads[face];
        for(int c = 0; c < 4; c++)
        {
            if(corner >= 0 && c != corner) continue;

            const unsigned int vertex = quad.vertices[c];
            FaceTexel average;
            const auto& star = m_stars[vertex];
            /* every incident face contributes its base corner texel,
             * including faces that do not share an edge with this face */
            for(const auto& entry : star)
            {
                const FaceImageSpec& image = m_base[entry.face];
                const auto [tx, ty] = face_detail::corner_texel(entry.corner, image.width, image.height);
                average += image.texels[static_cast<size_t>(ty) * image.width + tx];
            }
            average = average * (1.0f / static_cast<float>(star.size()));

            const auto [gx, gy] = corner_gutter_cell(c, layer.width, layer.height);
            layer.at(gx, gy) = average;
        }
    }

    /* Patch a neighbour/star face after one face changed: interior mips and
     * unrelated gutters are kept, only shared edge gutters and the corner
     * gutters touching the changed face's vertices are recomputed. */
    void patch_face(std::vector<std::shared_ptr<const FaceLayer>>& layers,
                    unsigned int face, unsigned int changed)
    {
        const FaceQuad& quad = m_quads[face];
        const FaceQuad& changedQuad = m_quads[changed];

        bool sharedEdge = false;
        for(int edge = 0; edge < 4; edge++)
        {
            const auto [a, b] = face_detail::edge_endpoints(quad, edge);
            const auto it = m_edges.find(face_detail::edge_key(a, b));
            const face_detail::EdgeInfo& info = it->second;
            const unsigned int neighbour = (info.faceA == face) ? info.faceB : info.faceA;
            if(neighbour == changed) sharedEdge = true;
        }

        std::array<bool, 4> touchedCorner = { false, false, false, false };
        for(int c = 0; c < 4; c++)
        {
            const unsigned int vertex = changedQuad.vertices[c];
            for(const auto& entry : m_stars[vertex])
            {
                if(entry.face == face) touchedCorner[entry.corner] = true;
            }
        }

        for(int level = 0; level < static_cast<int>(layers.size()); level++)
        {
            auto cloned = std::make_shared<FaceLayer>(*layers[level]);
            if(sharedEdge) fill_edge_gutters(*cloned, face);
            for(int c = 0; c < 4; c++)
            {
                if(touchedCorner[c]) fill_corner_gutters(*cloned, face, c);
            }
            layers[level] = std::move(cloned);
        }
    }

    void collect_affected(unsigned int face, std::vector<unsigned int>& affected)
    {
        const FaceQuad& quad = m_quads[face];

        auto add = [&](unsigned int other)
        {
            if(other == face_detail::INVALID_FACE) return;
            if(std::find(affected.begin(), affected.end(), other) == affected.end())
            {
                affected.push_back(other);
            }
        };

        add(face);
        for(int edge = 0; edge < 4; edge++)
        {
            const auto [a, b] = face_detail::edge_endpoints(quad, edge);
            const auto it = m_edges.find(face_detail::edge_key(a, b));
            const face_detail::EdgeInfo& info = it->second;
            add(info.faceA == face ? info.faceB : info.faceA);
        }
        for(int corner = 0; corner < 4; corner++)
        {
            for(const auto& entry : m_stars[quad.vertices[corner]])
            {
                add(entry.face);
            }
        }
    }

    static std::pair<int, int> edge_gutter_cell(int edge, int k, int width, int height)
    {
        switch(edge)
        {
        case 0: return { k + 1, 0 };
        case 1: return { width + 1, k + 1 };
        case 2: return { width - k, height + 1 };
        default: return { 0, height - k };
        }
    }

    static std::pair<int, int> edge_gutter_interior(int edge, int k, int width, int height)
    {
        switch(edge)
        {
        case 0: return { k, 0 };
        case 1: return { width - 1, k };
        case 2: return { width - 1 - k, height - 1 };
        default: return { 0, height - 1 - k };
        }
    }

    static std::pair<int, int> corner_gutter_cell(int corner, int width, int height)
    {
        switch(corner)
        {
        case 0: return { 0, 0 };
        case 1: return { width + 1, 0 };
        case 2: return { width + 1, height + 1 };
        default: return { 0, height + 1 };
        }
    }

private:
    unsigned int m_vertexCount = 0;
    std::vector<FaceQuad> m_quads;
    std::vector<FaceImageSpec> m_base;
    std::unordered_map<uint64_t, face_detail::EdgeInfo> m_edges;
    std::vector<std::vector<face_detail::StarEntry>> m_stars;
    std::shared_ptr<FaceSnapshot> m_snapshot;
    unsigned long long m_rebuildCount = 0;
};

/* Trilinear sample against an explicit, finite LOD, clamped to the available
 * levels; UV is restricted to the unit face square. No anisotropy. */
inline FaceTexel face_texture_lod(const FaceSnapshot& snapshot, unsigned int face, const Vec2& uv, float lod)
{
    if(!std::isfinite(lod))
    {
        throw std::invalid_argument("face_texture_lod: LOD must be finite");
    }

    const auto& layers = snapshot.layers.at(face);
    const float maxLevel = static_cast<float>(layers.size() - 1);
    const float clamped = std::clamp(lod, 0.0f, maxLevel);
    const int low = static_cast<int>(std::floor(clamped));
    const int high = static_cast<int>(std::ceil(clamped));
    const float fraction = clamped - static_cast<float>(low);

    const FaceTexel lowSample = face_detail::sample_layer(*layers[low], uv);
    if(high == low) return lowSample;

    const FaceTexel highSample = face_detail::sample_layer(*layers[high], uv);
    return lowSample * (1.0f - fraction) + highSample * fraction;
}
