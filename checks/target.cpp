#include "task_support/check.h"

#include "renderer.h"
#include "sampler.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

/* =======================================================================
 *  Local test helpers (independent reference implementations)
 * ======================================================================= */

static bool approx(float a, float b, float eps = 1e-5f)
{
    return std::abs(a - b) <= eps;
}

static bool approx4(const Vec4& a, const Vec4& b, float eps = 1e-5f)
{
    return approx(a.x, b.x, eps) && approx(a.y, b.y, eps) &&
           approx(a.z, b.z, eps) && approx(a.w, b.w, eps);
}

static FaceImageSpec make_image(int width, int height, const std::function<Vec4(int, int, int)>& pattern, int face)
{
    FaceImageSpec image;
    image.width = width;
    image.height = height;
    image.texels.resize(static_cast<size_t>(width) * height);
    for(int y = 0; y < height; y++)
    {
        for(int x = 0; x < width; x++)
        {
            image.texels[static_cast<size_t>(y) * width + x] = pattern(x, y, face);
        }
    }
    return image;
}

static Vec4 pattern_bands(int x, int y, int face)
{
    return Vec4(0.1f + 0.07f * x + 0.013f * face,
                0.05f + 0.11f * y,
                0.03f + 0.017f * (x + y) + 0.021f * face,
                1.0f);
}

/* Independent area weighted average of the piecewise constant base texels
 * covered by the normalized rectangle [u0,u1] x [v0,v1]. */
static Vec4 reference_coverage(const FaceImageSpec& image, float u0, float v0, float u1, float v1)
{
    u0 = std::clamp(u0, 0.0f, 1.0f);
    u1 = std::clamp(u1, 0.0f, 1.0f);
    v0 = std::clamp(v0, 0.0f, 1.0f);
    v1 = std::clamp(v1, 0.0f, 1.0f);

    double weightSum = 0.0;
    double acc[4] = { 0.0, 0.0, 0.0, 0.0 };

    for(int ty = 0; ty < image.height; ty++)
    {
        const float ry0 = static_cast<float>(ty) / image.height;
        const float ry1 = static_cast<float>(ty + 1) / image.height;
        const double oy = std::max(0.0, static_cast<double>(std::min(v1, ry1) - std::max(v0, ry0)));
        if(oy <= 0.0) continue;

        for(int tx = 0; tx < image.width; tx++)
        {
            const float rx0 = static_cast<float>(tx) / image.width;
            const float rx1 = static_cast<float>(tx + 1) / image.width;
            const double ox = std::max(0.0, static_cast<double>(std::min(u1, rx1) - std::max(u0, rx0)));
            if(ox <= 0.0) continue;

            const double weight = ox * oy;
            const Vec4& texel = image.texels[static_cast<size_t>(ty) * image.width + tx];
            acc[0] += weight * texel.x;
            acc[1] += weight * texel.y;
            acc[2] += weight * texel.z;
            acc[3] += weight * texel.w;
            weightSum += weight;
        }
    }

    if(weightSum <= 0.0) return Vec4();
    return Vec4(static_cast<float>(acc[0] / weightSum),
                static_cast<float>(acc[1] / weightSum),
                static_cast<float>(acc[2] / weightSum),
                static_cast<float>(acc[3] / weightSum));
}

/* Rectangle on the neighbour face covered when a gutter cell of a face
 * whose edge runs [s0,s1] is unfolded across the neighbour's directed edge.
 * Derived directly from the neighbour's local corner ordering. */
static void neighbour_rect(int neighbourEdge, float s0, float s1, float depth,
                           float& u0, float& v0, float& u1, float& v1)
{
    const float t0 = 1.0f - s1; /* shared edge runs in reverse */
    const float t1 = 1.0f - s0;

    switch(neighbourEdge)
    {
    case 0: u0 = t0;        u1 = t1;        v0 = 0.0f;       v1 = depth;      break;
    case 1: u0 = 1.0f-depth; u1 = 1.0f;     v0 = t0;         v1 = t1;         break;
    case 2: u0 = 1.0f - t1;  u1 = 1.0f - t0; v0 = 1.0f-depth; v1 = 1.0f;      break;
    default:u0 = 0.0f;       u1 = depth;     v0 = 1.0f - t1;  v1 = 1.0f - t0; break;
    }
}

/* Independent bilinear evaluation on a gutter inclusive layer. */
static Vec4 reference_bilinear(const FaceLayer& layer, float u, float v)
{
    u = std::clamp(u, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);
    const float fx = std::clamp(u * layer.width - 0.5f, -0.5f, layer.width - 0.5f);
    const float fy = std::clamp(v * layer.height - 0.5f, -0.5f, layer.height - 0.5f);
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - x0;
    const float ty = fy - y0;

    const Vec4& c00 = layer.at(x0 + 1, y0 + 1);
    const Vec4& c10 = layer.at(x0 + 2, y0 + 1);
    const Vec4& c01 = layer.at(x0 + 1, y0 + 2);
    const Vec4& c11 = layer.at(x0 + 2, y0 + 2);

    const Vec4 row0 = c00 * (1.0f - tx) + c10 * tx;
    const Vec4 row1 = c01 * (1.0f - tx) + c11 * tx;
    return row0 * (1.0f - ty) + row1 * ty;
}

struct TestTopology
{
    std::vector<FaceQuad> quads;
    unsigned int vertexCount = 0;
    std::vector<FaceImageSpec> images;
};

/* Cross of four quads around a central face plus a three-face fan at
 * vertex 0 (vertex star of size six). The reversed shared edge lands on
 * every local edge orientation of the neighbours, and all resolutions
 * are rectangular powers of two with different densities. */
static TestTopology build_cross_topology()
{
    TestTopology topo;
    topo.vertexCount = 21;
    topo.quads = {
        FaceQuad{{ 0,  1,  2,  3}}, /* 0 central                          */
        FaceQuad{{ 0,  4,  5,  1}}, /* 1 shares central edge 0-1 via e3  */
        FaceQuad{{ 2,  1,  6,  7}}, /* 2 shares central edge 1-2 via e0  */
        FaceQuad{{ 8,  3,  2,  9}}, /* 3 shares central edge 2-3 via e1  */
        FaceQuad{{10, 11,  0,  3}}, /* 4 shares central edge 3-0 via e2  */
        FaceQuad{{ 0, 12, 13, 14}}, /* 5 fan face at vertex 0            */
        FaceQuad{{ 0, 15, 16, 17}}, /* 6 fan face at vertex 0            */
        FaceQuad{{ 0, 18, 19, 20}}, /* 7 fan face at vertex 0            */
    };

    const std::array<std::pair<int, int>, 8> sizes = {
        std::pair{4, 2}, {2, 4}, {8, 2}, {4, 4}, {2, 2}, {4, 4}, {2, 8}, {1, 4}
    };

    for(size_t face = 0; face < topo.quads.size(); face++)
    {
        topo.images.push_back(make_image(sizes[face].first, sizes[face].second, pattern_bands, static_cast<int>(face)));
    }
    return topo;
}

/* =======================================================================
 *  topology group
 * ======================================================================= */

static int run_topology_valid()
{
    TestTopology topo = build_cross_topology();
    const std::vector<FaceQuad> inputQuads = topo.quads;
    const std::vector<FaceImageSpec> inputImages = topo.images;

    FaceMaterial material(topo.quads, topo.vertexCount, topo.images);

    TASK_CHECK(material.face_count() == 8);
    TASK_CHECK(material.rebuild_count() == 1);
    TASK_CHECK(material.snapshot() != nullptr);
    TASK_CHECK(material.snapshot()->layers.size() == 8);

    /* input arrays are not modified by resource construction */
    TASK_CHECK(inputQuads.size() == 8);
    TASK_CHECK(inputImages[0].texels.size() == 8);
    TASK_CHECK(approx4(inputImages[0].texels[0], pattern_bands(0, 0, 0)));
    for(size_t i = 0; i < topo.quads.size(); i++)
    {
        for(int c = 0; c < 4; c++)
        {
            TASK_CHECK(material.quads()[i].vertices[c] == inputQuads[i].vertices[c]);
        }
    }

    /* mip chains run to 1x1, each axis halving independently */
    TASK_CHECK(material.snapshot()->layers[0].size() == 3); /* 4x2 -> 2x1 -> 1x1 */
    TASK_CHECK(material.snapshot()->layers[0][0]->width == 4 && material.snapshot()->layers[0][0]->height == 2);
    TASK_CHECK(material.snapshot()->layers[0][1]->width == 2 && material.snapshot()->layers[0][1]->height == 1);
    TASK_CHECK(material.snapshot()->layers[0][2]->width == 1 && material.snapshot()->layers[0][2]->height == 1);
    TASK_CHECK(material.snapshot()->layers[6].size() == 4); /* 2x8 -> 1x4 -> 1x2 -> 1x1 */
    return 0;
}

static int run_topology_boundary()
{
    /* isolated face: every edge and corner is a boundary */
    std::vector<FaceQuad> quads = { FaceQuad{{0, 1, 2, 3}} };
    std::vector<FaceImageSpec> images = { make_image(2, 2, pattern_bands, 0) };
    FaceMaterial material(quads, 4, images);
    TASK_CHECK(material.face_count() == 1);
    TASK_CHECK(material.snapshot()->layers[0].size() == 2);
    return 0;
}

static bool throws_on(const std::vector<FaceQuad>& quads, unsigned int vertexCount,
                      const std::vector<FaceImageSpec>& images)
{
    try
    {
        FaceMaterial material(quads, vertexCount, images);
    }
    catch(const std::invalid_argument&)
    {
        return true;
    }
    return false;
}

static int run_topology_reject_edge_three_faces()
{
    std::vector<FaceQuad> quads = {
        FaceQuad{{0, 1, 2, 3}},
        FaceQuad{{1, 4, 5, 2}},
        FaceQuad{{1, 6, 7, 2}}, /* third face on edge (1,2), reversed again */
    };
    std::vector<FaceImageSpec> images(3, make_image(2, 2, pattern_bands, 0));
    TASK_CHECK(throws_on(quads, 8, images));
    return 0;
}

static int run_topology_reject_same_direction()
{
    std::vector<FaceQuad> quads = {
        FaceQuad{{0, 1, 2, 3}},
        FaceQuad{{4, 1, 2, 5}}, /* traverses (1,2) in the same direction */
    };
    std::vector<FaceImageSpec> images(2, make_image(2, 2, pattern_bands, 0));
    TASK_CHECK(throws_on(quads, 6, images));
    return 0;
}

static int run_topology_reject_bad_indices_and_specs()
{
    std::vector<FaceQuad> good = { FaceQuad{{0, 1, 2, 3}} };

    std::vector<FaceQuad> outOfRange = { FaceQuad{{0, 1, 2, 7}} };
    std::vector<FaceImageSpec> image = { make_image(2, 2, pattern_bands, 0) };
    TASK_CHECK(throws_on(outOfRange, 4, image));

    std::vector<FaceQuad> duplicate = { FaceQuad{{0, 1, 2, 1}} };
    TASK_CHECK(throws_on(duplicate, 4, image));

    FaceImageSpec nonPowerOfTwo = make_image(3, 2, pattern_bands, 0);
    TASK_CHECK(throws_on(good, 4, { nonPowerOfTwo }));

    FaceImageSpec mismatched = make_image(4, 4, pattern_bands, 0);
    mismatched.texels.pop_back();
    TASK_CHECK(throws_on(good, 4, { mismatched }));

    FaceMaterial material(good, 4, image);
    const auto before = material.snapshot();
    const unsigned long long builds = material.rebuild_count();

    std::vector<Vec4> bad;
    TASK_CHECK(!material.replace_face(0, 3, 2, bad));             /* non power of two */
    bad.resize(4);
    TASK_CHECK(!material.replace_face(0, 4, 4, bad));             /* wrong texel count */
    TASK_CHECK(!material.replace_face(99, 2, 2, bad));            /* unknown face */
    TASK_CHECK(material.rebuild_count() == builds);               /* nothing published */
    TASK_CHECK(material.snapshot().get() == before.get());
    return 0;
}

/* =======================================================================
 *  gutters group
 * ======================================================================= */

static int run_gutter_edge_coverage()
{
    TestTopology topo = build_cross_topology();
    FaceMaterial material(topo.quads, topo.vertexCount, topo.images);
    const FaceSnapshot& snapshot = *material.snapshot();

    /* central face 0: edge -> (neighbour face, neighbour local edge) */
    const std::array<std::pair<unsigned int, int>, 4> neighbours = {
        std::pair{1u, 3}, {2u, 0}, {3u, 1}, {4u, 2}
    };

    const FaceLayer& central = *snapshot.layers[0][0]; /* 4 x 2 interior */

    for(int edge = 0; edge < 4; edge++)
    {
        const auto [neighbour, neighbourEdge] = neighbours[edge];
        const FaceImageSpec& neighbourImage = topo.images[neighbour];
        const bool horizontalEdge = (edge == 0 || edge == 2);
        const int cells = horizontalEdge ? central.width : central.height;
        /* the gutter cell belongs to this face: its unfolded depth is the
         * central face's cell thickness, mapped onto the neighbour axes */
        const float depth = horizontalEdge ? 1.0f / central.height : 1.0f / central.width;
        (void)neighbourImage;

        for(int k = 0; k < cells; k++)
        {
            const float s0 = static_cast<float>(k) / cells;
            const float s1 = static_cast<float>(k + 1) / cells;
            float u0, v0, u1, v1;
            neighbour_rect(neighbourEdge, s0, s1, depth, u0, v0, u1, v1);
            const Vec4 expected = reference_coverage(neighbourImage, u0, v0, u1, v1);

            int gx = 0, gy = 0;
            switch(edge)
            {
            case 0: gx = k + 1; gy = 0; break;
            case 1: gx = central.width + 1; gy = k + 1; break;
            case 2: gx = central.width - k; gy = central.height + 1; break;
            default: gx = 0; gy = central.height - k; break;
            }
            TASK_CHECK(approx4(central.at(gx, gy), expected, 1e-5f));
        }
    }

    /* reverse direction: neighbour face 1 edge 3 gutters unfold into face 0 */
    const FaceLayer& below = *snapshot.layers[1][0]; /* 2 x 4 interior */
    for(int k = 0; k < below.height; k++)
    {
        const float s0 = static_cast<float>(k) / below.height;
        const float s1 = static_cast<float>(k + 1) / below.height;
        float u0, v0, u1, v1;
        neighbour_rect(0, s0, s1, 1.0f / topo.images[0].height, u0, v0, u1, v1);
        const Vec4 expected = reference_coverage(topo.images[0], u0, v0, u1, v1);
        /* face 1 edge 3 gutter cells: (0, height-k) */
        TASK_CHECK(approx4(below.at(0, below.height - k), expected, 1e-5f));
    }
    return 0;
}

static int run_gutter_edge_orientation()
{
    /* A (2x4) right edge and B (2x4) left edge share vertices 1-2 in
     * opposite directions. B carries a gradient along its local y axis,
     * so the reversed edge parameterization is directly observable. */
    std::vector<FaceQuad> quads = {
        FaceQuad{{0, 1, 2, 3}}, /* A: edge 1 is 1 -> 2 (upwards)   */
        FaceQuad{{1, 4, 5, 2}}, /* B: edge 3 is 2 -> 1 (downwards) */
    };
    std::vector<FaceImageSpec> images;
    images.push_back(make_image(2, 4, [](int, int, int) { return Vec4(0.0f); }, 0));
    images.push_back(make_image(2, 4, [](int, int y, int) { return Vec4(y / 3.0f, 0.0f, 0.0f, 1.0f); }, 1));

    FaceMaterial material(quads, 6, images);
    const FaceLayer& central = *material.snapshot()->layers[0][0]; /* 2 x 4 */

    /* A edge 1 has four gutter cells; cell k spans s in [k/4,(k+1)/4].
     * Reversed onto B (edge 3, local v = 1-t) it covers B row k exactly,
     * whose red value is k/3. A non-reversed mapping would give (3-k)/3. */
    for(int k = 0; k < 4; k++)
    {
        const float s0 = static_cast<float>(k) / 4.0f;
        const float s1 = static_cast<float>(k + 1) / 4.0f;
        float u0, v0, u1, v1;
        neighbour_rect(3, s0, s1, 1.0f / central.width, u0, v0, u1, v1);
        const Vec4 expected = reference_coverage(images[1], u0, v0, u1, v1);
        TASK_CHECK(approx4(central.at(central.width + 1, k + 1), expected));
        TASK_CHECK(approx(central.at(central.width + 1, k + 1).x, k / 3.0f, 1e-5f));
    }
    return 0;
}

static int run_gutter_boundary()
{
    std::vector<FaceQuad> quads = { FaceQuad{{0, 1, 2, 3}} };
    std::vector<FaceImageSpec> images = { make_image(2, 2, pattern_bands, 0) };
    FaceMaterial material(quads, 4, images);
    const FaceLayer& layer = *material.snapshot()->layers[0][0];

    /* boundary edge gutters extend the adjacent edge texel */
    TASK_CHECK(approx4(layer.at(1, 0), layer.at(1, 1)));
    TASK_CHECK(approx4(layer.at(2, 0), layer.at(2, 1)));
    TASK_CHECK(approx4(layer.at(3, 1), layer.at(2, 1)));
    TASK_CHECK(approx4(layer.at(3, 2), layer.at(2, 2)));
    TASK_CHECK(approx4(layer.at(1, 3), layer.at(1, 2)));
    TASK_CHECK(approx4(layer.at(0, 1), layer.at(1, 1)));

    /* boundary corner: star of size one, so it equals the corner texel */
    TASK_CHECK(approx4(layer.at(0, 0), layer.at(1, 1)));
    TASK_CHECK(approx4(layer.at(3, 0), layer.at(2, 1)));
    TASK_CHECK(approx4(layer.at(3, 3), layer.at(2, 2)));
    TASK_CHECK(approx4(layer.at(0, 3), layer.at(1, 2)));
    return 0;
}

static int run_gutter_corner_star()
{
    TestTopology topo = build_cross_topology(); /* six faces incident at vertex 0 */
    FaceMaterial material(topo.quads, topo.vertexCount, topo.images);
    const FaceSnapshot& snapshot = *material.snapshot();

    /* independent equal-weight average of every incident base corner texel */
    const std::array<std::pair<unsigned int, int>, 6> star = {
        std::pair{0u, 0}, {1u, 0}, {4u, 2}, {5u, 0}, {6u, 0}, {7u, 0}
    };
    Vec4 expected(0.0f);
    for(const auto& [face, corner] : star)
    {
        const FaceImageSpec& image = topo.images[face];
        const int tx = (corner == 0 || corner == 3) ? 0 : image.width - 1;
        const int ty = (corner < 2) ? 0 : image.height - 1;
        expected += image.texels[static_cast<size_t>(ty) * image.width + tx];
    }
    expected = expected * (1.0f / static_cast<float>(star.size()));

    /* every level of every incident face uses the same base-level average */
    for(unsigned int faceIndex = 0; faceIndex < 8; faceIndex++)
    {
        bool incident = false;
        int localCorner = -1;
        for(int c = 0; c < 4; c++)
        {
            if(topo.quads[faceIndex].vertices[c] == 0) { incident = true; localCorner = c; }
        }
        if(!incident) continue;

        for(size_t level = 0; level < snapshot.layers[faceIndex].size(); level++)
        {
            const FaceLayer& layer = *snapshot.layers[faceIndex][level];
            const int gx = (localCorner == 0 || localCorner == 3) ? 0 : layer.width + 1;
            const int gy = (localCorner < 2) ? 0 : layer.height + 1;
            TASK_CHECK(approx4(layer.at(gx, gy), expected, 1e-5f));
        }
    }

    /* a fan face that shares no edge with face 0 really contributes:
     * perturb only face 7 and confirm the corner average moves. */
    std::vector<Vec4> replaced = topo.images[7].texels;
    for(auto& texel : replaced) texel = Vec4(9.0f, 7.0f, 5.0f, 1.0f);
    TASK_CHECK(material.replace_face(7, topo.images[7].width, topo.images[7].height, replaced));

    Vec4 shifted(0.0f);
    for(const auto& [face, corner] : star)
    {
        const FaceImageSpec& image = (face == 7) ? FaceImageSpec{ topo.images[7].width, topo.images[7].height, replaced }
                                                 : topo.images[face];
        const int tx = (corner == 0 || corner == 3) ? 0 : image.width - 1;
        const int ty = (corner < 2) ? 0 : image.height - 1;
        shifted += image.texels[static_cast<size_t>(ty) * image.width + tx];
    }
    shifted = shifted * (1.0f / static_cast<float>(star.size()));

    const FaceLayer& layer = *material.snapshot()->layers[0][0];
    TASK_CHECK(approx4(layer.at(0, 0), shifted, 1e-5f));
    TASK_CHECK(!approx4(layer.at(0, 0), expected, 1e-4f));
    return 0;
}

static int run_gutter_mip_chain()
{
    FaceImageSpec image = make_image(4, 2, [](int x, int y, int) {
        return Vec4(static_cast<float>(x + 1), static_cast<float>(y + 1), 0.0f, 1.0f);
    }, 0);

    std::vector<FaceQuad> quads = { FaceQuad{{0, 1, 2, 3}} };
    FaceMaterial material(quads, 4, { image });
    const FaceSnapshot& snapshot = *material.snapshot();

    const FaceLayer& level1 = *snapshot.layers[0][1];
    const FaceLayer& level2 = *snapshot.layers[0][2];
    TASK_CHECK(level1.width == 2 && level1.height == 1);
    TASK_CHECK(level2.width == 1 && level2.height == 1);

    for(int x = 0; x < 2; x++)
    {
        /* 4x2 -> 2x1: both axes still halve, so each texel is a 2x2 average */
        Vec4 expected(0.0f);
        for(int yy = 0; yy < 2; yy++)
        {
            for(int xx = 0; xx < 2; xx++)
            {
                expected += image.texels[static_cast<size_t>(yy) * 4 + (2 * x + xx)];
            }
        }
        TASK_CHECK(approx4(level1.at(x + 1, 1), expected * 0.25f));
    }

    /* 2x1 -> 1x1: width halves, height already 1 repeats the same row */
    Vec4 top(0.0f);
    for(int x = 0; x < 2; x++) top += level1.at(x + 1, 1);
    TASK_CHECK(approx4(level2.at(1, 1), top * 0.5f));
    return 0;
}

static int run_gutter_trilinear()
{
    FaceImageSpec image = make_image(4, 4, [](int x, int y, int) {
        return Vec4(x / 3.0f, y / 3.0f, (x + y) / 6.0f, 1.0f);
    }, 0);
    std::vector<FaceQuad> quads = { FaceQuad{{0, 1, 2, 3}} };
    FaceMaterial material(quads, 4, { image });
    const FaceSnapshot& snapshot = *material.snapshot();
    TASK_CHECK(snapshot.layers[0].size() == 3);

    const std::array<Vec2, 5> probes = {
        Vec2(0.13f, 0.27f), Vec2(0.5f, 0.5f), Vec2(0.91f, 0.34f),
        Vec2(0.0f, 0.0f), Vec2(1.0f, 1.0f)
    };

    for(const Vec2& uv : probes)
    {
        /* fractional LOD between adjacent levels */
        const Vec4 floorSample = reference_bilinear(*snapshot.layers[0][1], uv.x, uv.y);
        const Vec4 ceilSample = reference_bilinear(*snapshot.layers[0][2], uv.x, uv.y);
        const Vec4 expected = floorSample * 0.6f + ceilSample * 0.4f;
        TASK_CHECK(approx4(face_texture_lod(snapshot, 0, uv, 1.4f), expected, 1e-5f));

        /* clamping below zero and above the top level */
        TASK_CHECK(approx4(face_texture_lod(snapshot, 0, uv, -5.0f),
                           face_texture_lod(snapshot, 0, uv, 0.0f), 1e-6f));
        TASK_CHECK(approx4(face_texture_lod(snapshot, 0, uv, 42.0f),
                           reference_bilinear(*snapshot.layers[0][2], uv.x, uv.y), 1e-6f));

        /* LOD zero agrees with the direct bilinear layer */
        TASK_CHECK(approx4(face_texture_lod(snapshot, 0, uv, 0.0f),
                           reference_bilinear(*snapshot.layers[0][0], uv.x, uv.y), 1e-6f));
    }

    /* LOD must be explicit and finite */
    bool threw = false;
    try { (void)face_texture_lod(snapshot, 0, probes[0], std::numeric_limits<float>::infinity()); }
    catch(const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw);
    return 0;
}

/* =======================================================================
 *  rendering group
 * ======================================================================= */

struct FaceVarying
{
    Vec4 position;
    Vec2 uv;
    float face;
    VARYING(position, uv, face);
};

struct FaceUniforms
{
    std::shared_ptr<const FaceSnapshot> snapshot;
    float lod = 0.0f;
};

static Program<FaceMeshVertex, FaceVarying, FaceUniforms> make_textured_program()
{
    Program<FaceMeshVertex, FaceVarying, FaceUniforms> program;
    program.onVertex([](const FaceUniforms&, const FaceMeshVertex& in, FaceVarying& out)
    {
        out.position = in.position;
        out.uv = in.uv;
        out.face = in.face;
    });
    program.onFragment([](const FaceUniforms& uniforms, const FaceVarying& in, Vec4& out)
    {
        const unsigned int face = static_cast<unsigned int>(in.face + 0.5f);
        out = face_texture_lod(*uniforms.snapshot, face, in.uv, uniforms.lod);
    });
    return program;
}

static std::unique_ptr<Renderer> render_snapshot(const FaceSnapshot& snapshot,
                                                 const std::vector<Vec4>& positions,
                                                 float lod, int size = 64)
{
    auto renderer = std::make_unique<Renderer>(size, size);
    renderer->options().culling = false;
    auto program = make_textured_program();
    program.uniforms().snapshot = std::shared_ptr<const FaceSnapshot>(&snapshot, [](const FaceSnapshot*) {});
    program.uniforms().lod = lod;
    FaceQuadMesh mesh;
    mesh.snapshot = program.uniforms().snapshot;
    mesh.positions = positions;
    renderer->drawQuads(program, mesh);
    return renderer;
}

static int run_render_subtriangles_and_face_id()
{
    std::vector<FaceQuad> quads = {
        FaceQuad{{0, 1, 2, 3}},
        FaceQuad{{1, 4, 5, 2}},
    };
    std::vector<FaceImageSpec> images = {
        make_image(4, 4, [](int, int, int) { return Vec4(1.0f, 0.2f, 0.2f, 1.0f); }, 0),
        make_image(4, 4, [](int, int, int) { return Vec4(0.2f, 1.0f, 0.2f, 1.0f); }, 1),
    };
    FaceMaterial material(quads, 6, images);

    std::vector<Vec4> positions = {
        Vec4(-0.8f, -0.8f, 0.0f, 1.0f), Vec4(0.0f, -0.8f, 0.0f, 1.0f),
        Vec4( 0.0f,  0.8f, 0.0f, 1.0f), Vec4(-0.8f, 0.8f, 0.0f, 1.0f),
        Vec4( 0.8f, -0.8f, 0.0f, 1.0f), Vec4(0.8f, 0.8f, 0.0f, 1.0f),
    };

    auto renderer = render_snapshot(*material.snapshot(), positions, 0.0f);
    const auto& color = renderer->framebuffer().color();

    /* both sub-triangles of face 0 produced fragments (diagonal 0->2) */
    unsigned leftSide = 0;
    unsigned rightSide = 0;
    for(int y = 6; y < 58; y++)
    {
        for(int x = 6; x < 32; x++)
        {
            const Vec4 ndc((x + 0.5f) / 64.0f * 2.0f - 1.0f,
                           (y + 0.5f) / 64.0f * 2.0f - 1.0f, 0.0f, 1.0f);
            const bool aboveDiagonal = (ndc.y - (-0.8f)) >= (ndc.x - (-0.8f));
            if(aboveDiagonal) leftSide += color(x, y).x > 200 ? 1 : 0;
            else rightSide += color(x, y).x > 200 ? 1 : 0;
        }
    }
    TASK_CHECK(leftSide > 50);
    TASK_CHECK(rightSide > 50);

    /* the interpolated face number stays exact on both faces */
    TASK_CHECK(color(16, 32).x > 200 && color(16, 32).y < 120);
    TASK_CHECK(color(48, 32).y > 200 && color(48, 32).x < 120);
    return 0;
}

static int run_render_perspective_w()
{
    std::vector<FaceQuad> quads = { FaceQuad{{0, 1, 2, 3}} };
    std::vector<FaceImageSpec> images = {
        make_image(8, 8, [](int x, int y, int) {
            return Vec4(x / 7.0f, y / 7.0f, 0.25f, 1.0f);
        }, 0)
    };
    FaceMaterial material(quads, 4, images);
    const FaceSnapshot& snapshot = *material.snapshot();

    const std::array<Vec2, 4> ndc = { Vec2(-0.8f, -0.8f), Vec2(0.8f, -0.8f),
                                     Vec2(0.8f, 0.8f), Vec2(-0.8f, 0.8f) };
    const std::array<float, 4> weights = { 1.0f, 2.0f, 3.0f, 4.0f };

    std::vector<Vec4> flatPositions;
    std::vector<Vec4> perspectivePositions;
    for(int i = 0; i < 4; i++)
    {
        flatPositions.emplace_back(ndc[i].x, ndc[i].y, 0.0f, 1.0f);
        perspectivePositions.emplace_back(ndc[i].x * weights[i], ndc[i].y * weights[i], 0.0f, weights[i]);
    }

    auto flat = render_snapshot(snapshot, flatPositions, 0.0f);
    auto perspective = render_snapshot(snapshot, perspectivePositions, 0.0f);

    unsigned differing = 0;
    unsigned coveredFlat = 0;
    unsigned coveredPerspective = 0;
    for(int y = 0; y < 64; y++)
    {
        for(int x = 0; x < 64; x++)
        {
            const auto& a = flat->framebuffer().color()(x, y);
            const auto& b = perspective->framebuffer().color()(x, y);
            if(a.x != 0 || a.y != 0 || a.z != 0) coveredFlat++;
            if(b.x != 0 || b.y != 0 || b.z != 0) coveredPerspective++;
            if(std::abs(int(a.x) - int(b.x)) + std::abs(int(a.y) - int(b.y)) > 4) differing++;
        }
    }
    TASK_CHECK(coveredFlat > 1000);
    /* identical screen coverage apart from a few edge pixels moved by
     * perspective-division rounding */
    TASK_CHECK(std::abs(static_cast<int>(coveredPerspective) - static_cast<int>(coveredFlat)) <= 8);
    TASK_CHECK(differing > 100);                   /* but different uv interpolation */

    /* exact perspective-correct value at one fragment center (off the
     * 0-2 diagonal so it belongs to exactly one sub-triangle) */
    const int px = 40, py = 24;
    std::array<Vec2, 4> screen;
    for(int i = 0; i < 4; i++)
    {
        screen[i] = Vec2((ndc[i].x + 1.0f) * 0.5f * 64.0f,
                         (ndc[i].y + 1.0f) * 0.5f * 64.0f);
    }
    const Vec2 fragment(px + 0.5f, py + 0.5f);
    const Vec3 bc0 = barycentric(screen[0], screen[1], screen[2], fragment); /* sub-triangle 0-1-2 */
    const Vec3 bc1 = barycentric(screen[0], screen[2], screen[3], fragment); /* sub-triangle 0-2-3 */

    auto corrected_uv = [&](const Vec3& bc, const std::array<int, 3>& idx)
    {
        float invW[3] = { 1.0f / weights[idx[0]], 1.0f / weights[idx[1]], 1.0f / weights[idx[2]] };
        const float norm = bc.x * invW[0] + bc.y * invW[1] + bc.z * invW[2];
        const std::array<Vec2, 4> uvs = { Vec2(0, 0), Vec2(1, 0), Vec2(1, 1), Vec2(0, 1) };
        Vec2 uv(0.0f);
        const float lambda[3] = { bc.x * invW[0] / norm, bc.y * invW[1] / norm, bc.z * invW[2] / norm };
        for(int i = 0; i < 3; i++) uv = uv + uvs[idx[i]] * lambda[i];
        return uv;
    };

    Vec2 expectedUv;
    if(bc0.x >= 0.0f && bc0.y >= 0.0f && bc0.z >= 0.0f)
    {
        expectedUv = corrected_uv(bc0, { 0, 1, 2 });
    }
    else
    {
        TASK_CHECK(bc1.x >= 0.0f && bc1.y >= 0.0f && bc1.z >= 0.0f);
        expectedUv = corrected_uv(bc1, { 0, 2, 3 });
    }

    const Vec4 expectedColor = face_texture_lod(snapshot, 0, expectedUv, 0.0f);
    const auto& pixel = perspective->framebuffer().color()(px, py);
    TASK_CHECK(std::abs(int(pixel.x) - std::lround(expectedColor.x * 255.0f)) <= 2);
    TASK_CHECK(std::abs(int(pixel.y) - std::lround(expectedColor.y * 255.0f)) <= 2);
    return 0;
}

static int run_render_update_and_snapshot_replay()
{
    std::vector<FaceQuad> quads = {
        FaceQuad{{0, 1, 2, 3}},
        FaceQuad{{1, 4, 5, 2}},
    };
    std::vector<FaceImageSpec> images = {
        make_image(4, 4, [](int, int, int) { return Vec4(0.9f, 0.1f, 0.1f, 1.0f); }, 0),
        make_image(4, 4, [](int, int, int) { return Vec4(0.1f, 0.9f, 0.1f, 1.0f); }, 1),
    };

    std::vector<Vec4> positions = {
        Vec4(-0.8f, -0.8f, 0.0f, 1.0f), Vec4(0.0f, -0.8f, 0.0f, 1.0f),
        Vec4( 0.0f,  0.8f, 0.0f, 1.0f), Vec4(-0.8f, 0.8f, 0.0f, 1.0f),
        Vec4( 0.8f, -0.8f, 0.0f, 1.0f), Vec4(0.8f, 0.8f, 0.0f, 1.0f),
    };

    auto material = std::make_unique<FaceMaterial>(quads, 6, images);
    const std::shared_ptr<const FaceSnapshot> oldSnapshot = material->snapshot();

    auto first = render_snapshot(*oldSnapshot, positions, 0.0f);
    const RGBA8 beforePixel = first->framebuffer().color()(16, 32);
    TASK_CHECK(beforePixel.x > 200 && beforePixel.y < 80);

    /* replace face 0 base data with a blue image */
    std::vector<Vec4> blue(16, Vec4(0.1f, 0.1f, 0.9f, 1.0f));
    TASK_CHECK(material->replace_face(0, 4, 4, blue));
    TASK_CHECK(material->rebuild_count() == 2);

    auto updated = render_snapshot(*material->snapshot(), positions, 0.0f);
    const RGBA8 updatedPixel = updated->framebuffer().color()(16, 32);
    TASK_CHECK(updatedPixel.z > 200 && updatedPixel.x < 80);

    /* the immutable old snapshot still replays its original pixels even
     * though the source material is about to be destroyed */
    material.reset();
    auto replay = render_snapshot(*oldSnapshot, positions, 0.0f);
    const RGBA8 replayPixel = replay->framebuffer().color()(16, 32);
    TASK_CHECK(replayPixel.x == beforePixel.x && replayPixel.y == beforePixel.y &&
               replayPixel.z == beforePixel.z && replayPixel.w == beforePixel.w);
    TASK_CHECK(oldSnapshot.use_count() >= 1);
    return 0;
}

static int run_render_snapshot_outlives_sources()
{
    std::vector<Vec4> positions;
    std::shared_ptr<const FaceSnapshot> kept;

    /* every source object (topology arrays, image arrays and the owning
     * material) dies at the end of this scope */
    {
        std::vector<FaceQuad> quads = {
            FaceQuad{{0, 1, 2, 3}},
            FaceQuad{{1, 4, 5, 2}},
        };
        std::vector<FaceImageSpec> images = {
            make_image(4, 4, [](int, int, int) { return Vec4(0.8f, 0.2f, 0.3f, 1.0f); }, 0),
            make_image(2, 2, [](int, int, int) { return Vec4(0.2f, 0.3f, 0.8f, 1.0f); }, 1),
        };
        FaceMaterial material(quads, 6, images);

        /* mutate once so the snapshot also owns patched neighbour layers */
        std::vector<Vec4> replacement(16, Vec4(0.3f, 0.8f, 0.2f, 1.0f));
        TASK_CHECK(material.replace_face(0, 4, 4, replacement));
        TASK_CHECK(material.rebuild_count() == 2);

        positions = {
            Vec4(-0.8f, -0.8f, 0.0f, 1.0f), Vec4(0.0f, -0.8f, 0.0f, 1.0f),
            Vec4( 0.0f,  0.8f, 0.0f, 1.0f), Vec4(-0.8f, 0.8f, 0.0f, 1.0f),
            Vec4( 0.8f, -0.8f, 0.0f, 1.0f), Vec4(0.8f, 0.8f, 0.0f, 1.0f),
        };
        kept = material.snapshot();
    }

    /* source arrays and material are gone; the snapshot draws repeatedly */
    auto first = render_snapshot(*kept, positions, 0.0f);
    auto second = render_snapshot(*kept, positions, 1.0f);

    const RGBA8 a = first->framebuffer().color()(16, 32);
    const RGBA8 b = second->framebuffer().color()(16, 32);
    TASK_CHECK(a.y > 150 && a.x < 120);            /* green replacement on face 0 */
    TASK_CHECK(b.w == 255);
    TASK_CHECK(kept.use_count() >= 1);

    /* repeated draws are stable */
    auto third = render_snapshot(*kept, positions, 0.0f);
    TASK_CHECK(third->framebuffer().color()(16, 32).x == a.x &&
               third->framebuffer().color()(16, 32).y == a.y &&
               third->framebuffer().color()(16, 32).z == a.z);
    return 0;
}

static int run_render_two_renderers_interleaved()
{
    std::vector<FaceQuad> quads = { FaceQuad{{0, 1, 2, 3}} };
    std::vector<FaceImageSpec> redImages = {
        make_image(2, 2, [](int, int, int) { return Vec4(0.9f, 0.1f, 0.1f, 1.0f); }, 0)
    };
    std::vector<FaceImageSpec> blueImages = {
        make_image(2, 2, [](int, int, int) { return Vec4(0.1f, 0.1f, 0.9f, 1.0f); }, 0)
    };
    FaceMaterial redMaterial(quads, 4, redImages);
    FaceMaterial blueMaterial(quads, 4, blueImages);

    std::vector<Vec4> positions = {
        Vec4(-0.8f, -0.8f, 0.0f, 1.0f), Vec4(0.8f, -0.8f, 0.0f, 1.0f),
        Vec4( 0.8f,  0.8f, 0.0f, 1.0f), Vec4(-0.8f, 0.8f, 0.0f, 1.0f),
    };

    Renderer rendererA(32, 32);
    Renderer rendererB(32, 32);
    rendererA.options().culling = false;
    rendererB.options().culling = false;

    auto programA = make_textured_program();
    auto programB = make_textured_program();
    FaceQuadMesh meshA; meshA.snapshot = redMaterial.snapshot(); meshA.positions = positions;
    FaceQuadMesh meshB; meshB.snapshot = blueMaterial.snapshot(); meshB.positions = positions;
    programA.uniforms().snapshot = meshA.snapshot;
    programB.uniforms().snapshot = meshB.snapshot;

    rendererA.drawQuads(programA, meshA);
    rendererB.drawQuads(programB, meshB);
    rendererA.framebuffer().clear(Vec4(0.0f));
    rendererB.framebuffer().clear(Vec4(0.0f));
    rendererB.drawQuads(programB, meshB);
    rendererA.drawQuads(programA, meshA);

    const auto& pixelA = rendererA.framebuffer().color()(16, 16);
    const auto& pixelB = rendererB.framebuffer().color()(16, 16);
    TASK_CHECK(pixelA.x > 200 && pixelA.z < 80);
    TASK_CHECK(pixelB.z > 200 && pixelB.x < 80);
    return 0;
}

static int run_render_identity_and_rebuild_scope()
{
    TestTopology topo = build_cross_topology();
    FaceMaterial material(topo.quads, topo.vertexCount, topo.images);
    const std::shared_ptr<const FaceSnapshot> before = material.snapshot();

    std::vector<std::vector<const void*>> beforeIdentity(8);
    for(unsigned int face = 0; face < 8; face++)
    {
        for(size_t level = 0; level < before->layers[face].size(); level++)
        {
            beforeIdentity[face].push_back(material.layer_storage(face, static_cast<unsigned int>(level)));
        }
    }

    std::vector<Vec4> replacement(8, Vec4(0.7f, 0.2f, 0.4f, 1.0f));
    TASK_CHECK(material.replace_face(0, 4, 2, replacement));
    TASK_CHECK(material.rebuild_count() == 2);

    /* face 0: every level rebuilt */
    for(size_t level = 0; level < beforeIdentity[0].size(); level++)
    {
        TASK_CHECK(material.layer_storage(0, static_cast<unsigned int>(level)) != beforeIdentity[0][level]);
    }

    /* edge neighbours (1..4) and vertex-star faces (5..7) are patched */
    for(unsigned int face = 1; face < 8; face++)
    {
        for(size_t level = 0; level < beforeIdentity[face].size(); level++)
        {
            TASK_CHECK(material.layer_storage(face, static_cast<unsigned int>(level)) != beforeIdentity[face][level]);
        }
    }

    /* patching a neighbour never touches its interior mip texels: compare
     * every interior cell of face 1 before and after the update */
    for(size_t level = 0; level < before->layers[1].size(); level++)
    {
        const FaceLayer& oldLayer = *before->layers[1][level];
        const FaceLayer& newLayer = *material.snapshot()->layers[1][level];
        TASK_CHECK(oldLayer.width == newLayer.width && oldLayer.height == newLayer.height);
        for(int y = 0; y < oldLayer.height; y++)
        {
            for(int x = 0; x < oldLayer.width; x++)
            {
                TASK_CHECK(approx4(oldLayer.at(x + 1, y + 1), newLayer.at(x + 1, y + 1), 1e-6f));
            }
        }
    }

    /* the previous snapshot is untouched and still references the old layers */
    for(unsigned int face = 0; face < 8; face++)
    {
        for(size_t level = 0; level < beforeIdentity[face].size(); level++)
        {
            TASK_CHECK(before->layers[face][level].get() == beforeIdentity[face][level]);
        }
    }

    /* an unrelated face in a fresh material proves sharing across snapshots */
    std::vector<FaceQuad> isolatedQuads = {
        FaceQuad{{0, 1, 2, 3}}, FaceQuad{{4, 5, 6, 7}},
    };
    std::vector<FaceImageSpec> isolatedImages = {
        make_image(2, 2, pattern_bands, 0), make_image(2, 2, pattern_bands, 1)
    };
    FaceMaterial isolated(isolatedQuads, 8, isolatedImages);
    const void* untouchedIdentity = isolated.layer_storage(1, 0);
    TASK_CHECK(isolated.replace_face(0, 2, 2, isolatedImages[0].texels));
    TASK_CHECK(isolated.rebuild_count() == 2);
    TASK_CHECK(isolated.layer_storage(1, 0) == untouchedIdentity);
    TASK_CHECK(isolated.layer_storage(0, 0) != untouchedIdentity);
    return 0;
}

static int run_render_legacy_path_compatible()
{
    /* the original triangle / indexed / Sampler flow must keep working */
    struct Vertex { Vec4 p; Vec2 uv; };
    struct Varying { Vec4 position; Vec2 uv; VARYING(position, uv); };
    struct Uniforms { Sampler<RGBA8> tex; };

    Texture<RGBA8> image(2, 2, RGBA8(180, 60, 20, 255));
    Program<Vertex, Varying, Uniforms> program;
    program.onVertex([](const Uniforms&, const Vertex& v, Varying& o) { o.position = v.p; o.uv = v.uv; });
    program.onFragment([](const Uniforms& u, const Varying& v, Vec4& o) { o = texture(u.tex, v.uv) / 255.0f; });
    program.uniforms().tex = image;
    program.uniforms().tex.filter = eFilter::NEAREST;

    BufferIndexed<Vertex, unsigned> mesh;
    mesh.vertices = { { {-.8f,-.8f,0,1},{0,0} }, { {.8f,-.8f,0,1},{1,0} },
                      { {.8f,.8f,0,1},{1,1} }, { {-.8f,.8f,0,1},{0,1} } };
    mesh.indices = { 0, 1, 2, 0, 2, 3 };

    Renderer renderer(24, 24);
    renderer.options().culling = false;
    renderer.draw(program, mesh);
    const RGBA8 pixel = renderer.framebuffer().color()(12, 10);
    TASK_CHECK(pixel.x == 180 && pixel.y == 60 && pixel.z == 20);
    return 0;
}

/* =======================================================================
 *  registration
 * ======================================================================= */

const task_case task_cases[] = {
    { "topology", "valid_cross_topology",        run_topology_valid },
    { "topology", "boundary_face_allowed",       run_topology_boundary },
    { "topology", "reject_edge_three_faces",     run_topology_reject_edge_three_faces },
    { "topology", "reject_same_direction_edge",  run_topology_reject_same_direction },
    { "topology", "reject_bad_indices_and_specs", run_topology_reject_bad_indices_and_specs },

    { "gutters", "edge_gutter_area_coverage",    run_gutter_edge_coverage },
    { "gutters", "edge_orientation_reversed",    run_gutter_edge_orientation },
    { "gutters", "boundary_gutter_extension",    run_gutter_boundary },
    { "gutters", "corner_vertex_star_average",   run_gutter_corner_star },
    { "gutters", "internal_mip_chain",           run_gutter_mip_chain },
    { "gutters", "trilinear_cross_level_filter", run_gutter_trilinear },

    { "rendering", "quad_subtriangles_face_id",  run_render_subtriangles_and_face_id },
    { "rendering", "perspective_correct_w",      run_render_perspective_w },
    { "rendering", "update_pixels_snapshot_replay", run_render_update_and_snapshot_replay },
    { "rendering", "snapshot_outlives_all_sources", run_render_snapshot_outlives_sources },
    { "rendering", "two_renderers_interleaved",  run_render_two_renderers_interleaved },
    { "rendering", "storage_identity_rebuilds",  run_render_identity_and_rebuild_scope },
    { "rendering", "legacy_triangle_sampler_path", run_render_legacy_path_compatible },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
