// Headless thick-glass preview driven entirely through the production
// refraction interface.  It builds a closed convex glass wedge and a tiled
// coloured background, captures the three layers with the real rasterizer,
// synthesizes one-bounce refraction and writes a binary PPM.  No SDL, GPU or
// third-party dependency is involved.

#include <cmath>
#include <iostream>
#include <vector>

#include "src/refraction.h"

using namespace gfx;
using namespace gfx::refr;

namespace {

auto V4(float x, float y, float z, float w = 1.f) -> Vec { return Vec({x, y, z, w}); }
auto N4(float x, float y, float z) -> Vec { return Vec({x, y, z, 0.f}); }

auto MakeTriangle(std::initializer_list<Vec> pos, std::initializer_list<Vec> nrm)
    -> Triangle {
    Triangle t;
    const Vec* p = pos.begin();
    const Vec* n = nrm.begin();
    for (int i = 0; i < 3; ++i) {
        t.position[i] = p[i];
        t.normal[i] = n[i];
    }
    return t;
}

// Closed convex wedge: a box sheared in x so both glass shells are slanted.
auto MakeWedge(const Vec3& centre, const Vec3& he, float shear) -> std::vector<Triangle> {
    Vec3 c[8];
    const float z0 = centre.z - he.z, z1 = centre.z + he.z;
    const float xs[2] = {centre.x - he.x + shear * (z0 - centre.z),
                         centre.x + he.x + shear * (z0 - centre.z)};
    const float xd[2] = {centre.x - he.x + shear * (z1 - centre.z),
                         centre.x + he.x + shear * (z1 - centre.z)};
    c[0] = {xs[0], centre.y - he.y, z0};
    c[1] = {xs[1], centre.y - he.y, z0};
    c[2] = {xs[1], centre.y + he.y, z0};
    c[3] = {xs[0], centre.y + he.y, z0};
    c[4] = {xd[0], centre.y - he.y, z1};
    c[5] = {xd[1], centre.y - he.y, z1};
    c[6] = {xd[1], centre.y + he.y, z1};
    c[7] = {xd[0], centre.y + he.y, z1};
    // CCW outward winding; normals are derived from the corner geometry.
    const int loops[6][4] = {
        {3, 2, 1, 0}, {5, 6, 7, 4}, {4, 7, 3, 0},
        {2, 6, 5, 1}, {1, 5, 4, 0}, {7, 6, 2, 3}
    };
    std::vector<Triangle> out;
    for (const auto& loop : loops) {
        Vec3 n = Normalize(Cross(c[loop[1]] - c[loop[0]], c[loop[2]] - c[loop[0]]));
        Vec nv[4];
        Vec3 pv[4];
        for (int k = 0; k < 4; ++k) {
            pv[k] = c[loop[k]];
            nv[k] = ToVec4(n, 0.f);
        }
        out.push_back({{ToVec4(pv[0], 1.f), ToVec4(pv[1], 1.f), ToVec4(pv[2], 1.f)},
                       {nv[0], nv[1], nv[2]}});
        out.push_back({{ToVec4(pv[0], 1.f), ToVec4(pv[2], 1.f), ToVec4(pv[3], 1.f)},
                       {nv[0], nv[2], nv[3]}});
    }
    return out;
}

// One flat coloured quad on the z = z_wall plane.
auto MakeQuad(float x0, float x1, float y0, float y1, float z_wall)
    -> std::vector<Triangle> {
    const Vec a = V4(x0, y0, z_wall), b = V4(x1, y0, z_wall);
    const Vec cc = V4(x1, y1, z_wall), d = V4(x0, y1, z_wall);
    const Vec n = N4(0, 0, 1);
    return {MakeTriangle({a, b, cc}, {n, n, n}),
            MakeTriangle({a, cc, d}, {n, n, n})};
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <output.ppm>\n";
        return 1;
    }

    constexpr int kWidth = 320;
    constexpr int kHeight = 240;
    const Mat4 view = IdentityMat4();
    const Mat4 normal = IdentityMat4();
    // Asymmetric perspective is supported; keep the preview symmetric here.
    const Mat4 projection = MakePerspective(-1.f, 1.f, -0.75f, 0.75f, 1.f, 100.f);

    // Closed convex glass wedge in front of the camera.
    const std::vector<Triangle> glass = MakeWedge({0.f, 0.f, -22.f},
                                                  {3.5f, 3.5f, 3.f}, 0.15f);

    DepthTarget front(kWidth, kHeight);
    DepthTarget back(kWidth, kHeight);
    for (const Triangle& t : glass) {
        front.RasterizeTriangle(view, projection, normal, t, FaceMode::FrontFaces);
        back.RasterizeTriangle(view, projection, normal, t, FaceMode::BackFaces);
    }

    // Tiled opaque background wall with alternating linear-RGB colours.
    BackgroundTarget background(kWidth, kHeight);
    constexpr float kWallZ = -55.f;
    constexpr int kTiles = 6;
    for (int iy = 0; iy < kTiles; ++iy) {
        for (int ix = 0; ix < kTiles; ++ix) {
            const float x0 = -60.f + 120.f * ix / kTiles;
            const float x1 = -60.f + 120.f * (ix + 1) / kTiles;
            const float y0 = -45.f + 90.f * iy / kTiles;
            const float y1 = -45.f + 90.f * (iy + 1) / kTiles;
            const bool light = (ix + iy) % 2 == 0;
            const Vec3 color = light ? Vec3{0.85f, 0.75f, 0.55f}
                                     : Vec3{0.15f, 0.30f, 0.55f};
            for (const Triangle& t : MakeQuad(x0, x1, y0, y1, kWallZ))
                background.RasterizeTriangle(view, projection, t, color);
        }
    }

    GlassMaterial material;
    material.ior = 1.5f;
    material.sigma_a = {0.06f, 0.14f, 0.28f}; // weak blue-green tinting
    const Vec3 ambient{0.04f, 0.05f, 0.07f};

    SynthesisResult result = Synthesize(front, back, background, projection,
                                        material, ambient);
    WritePPM(argv[1], result.color, kWidth, kHeight);

    const auto& s = result.stats;
    std::cout << "REFRACTION_PREVIEW " << kWidth << 'x' << kHeight
              << " entries=" << s.entry_pixels
              << " exit_hits=" << s.exit_hits
              << " exit_missing=" << s.exit_missing
              << " tir=" << s.total_internal_reflection
              << " bg_hits=" << s.background_hits
              << " occluded=" << s.occluded_entries
              << " max_cells_per_segment=" << s.max_cells_per_segment << '\n';
    return 0;
}
