/*========================================================================================
 headless RSM example.

 Builds two colored reflector walls (red on the left, blue on the right) and a white
 receiver plane, captures a Reflective Shadow Map from an orthographic directional
 light and shades the receiver with one-bounce diffuse color bleeding. The result is
 written to .task-build/rsm.ppm as a binary PPM (P6).

 Everything runs on the CPU through the production micro-gl RSM API: no window, no
 GPU, no download. Run from the repository root:

     .task-build/rsm-example
========================================================================================*/

#include <microgl/canvas.h>
#include <microgl/bitmaps/bitmap.h>
#include <microgl/pixel_coders/RGB888_PACKED_32.h>
#include <microgl/z_buffer.h>
#include <microgl/rsm/Rsm.h>

#include <cstdio>
#include <vector>

namespace {
    using number = float;
    using V3 = microgl::vertex3<number>;
    using M4 = microgl::matrix_4x4<number>;
    using microgl::rsm::Rsm;
    using microgl::rsm::rsm_light;
    using microgl::rsm::rsm_rgb;
    using microgl::rsm::rsm_surface;
    using microgl::rsm::rsm_receiver_shader;

    M4 orthographic(number l, number r, number b, number t, number n, number f) {
        M4 m;
        m.identity();
        m[0] = number(2) / (r - l);
        m[5] = number(2) / (t - b);
        m[10] = number(-2) / (f - n);
        m[12] = -(r + l) / (r - l);
        m[13] = -(t + b) / (t - b);
        m[14] = -(f + n) / (f - n);
        return m;
    }

    // append an axis-aligned quad as two triangles sharing one normal.
    void add_quad(std::vector<V3> &positions, std::vector<V3> &normals,
                  std::vector<rsm_rgb<number> > &reflectance,
                  const V3 &p0, const V3 &p1, const V3 &p2, const V3 &p3,
                  const V3 &normal, const rsm_rgb<number> &color) {
        const V3 quad[4] = {p0, p1, p2, p3};
        const unsigned idx[6] = {0, 1, 2, 0, 2, 3};
        for (unsigned i = 0; i < 6; ++i) {
            positions.push_back(quad[idx[i]]);
            normals.push_back(normal);
        }
        reflectance.push_back(color);
        reflectance.push_back(color);
    }

    bool write_ppm(const char *path, const std::vector<unsigned char> &rgb,
                   int width, int height) {
        std::FILE *out = std::fopen(path, "wb");
        if (out == nullptr) return false;
        std::fprintf(out, "P6\n%d %d\n255\n", width, height);
        const size_t written =
                std::fwrite(rgb.data(), 1, rgb.size(), out);
        std::fclose(out);
        return written == rgb.size();
    }
}

int main() {
    // ---- light: orthographic, propagates down -Z over an 8x8 world aperture.
    rsm_light<number> light;
    light.view.identity();
    light.projection = orthographic(-4, 4, -4, 4, number(0.5), number(12));
    light.light_direction = V3{0, 0, -1};
    light.world_width = 8;
    light.world_height = 8;
    light.irradiance = rsm_rgb<number>{1, 1, 1};

    // ---- two colored reflector walls in the plane z = -3, normal +Z (face light).
    std::vector<V3> wall_pos, wall_nrm;
    std::vector<rsm_rgb<number> > wall_refl;
    add_quad(wall_pos, wall_nrm, wall_refl,
             V3{-3, -3, -3}, V3{0, -3, -3}, V3{0, 3, -3}, V3{-3, 3, -3},
             V3{0, 0, 1}, rsm_rgb<number>{0.9f, 0.1f, 0.1f});   // red, left
    add_quad(wall_pos, wall_nrm, wall_refl,
             V3{0, -3, -3}, V3{3, -3, -3}, V3{3, 3, -3}, V3{0, 3, -3},
             V3{0, 0, 1}, rsm_rgb<number>{0.1f, 0.2f, 0.9f});   // blue, right

    rsm_surface<number> walls;
    walls.positions = wall_pos.data();
    walls.normals = wall_nrm.data();
    walls.reflectance = wall_refl.data();
    walls.triangle_count = unsigned(wall_refl.size());
    walls.model.identity();

    // ---- capture the reflective shadow map (32x32).
    Rsm<number> rsm;
    if (!rsm.capture(walls, light, 32)) {
        std::fprintf(stderr, "RSM capture failed\n");
        return 1;
    }

    // ---- white receiver plane at z = -2, normal -Z (faces the reflector walls).
    const int W = 64, H = 64;
    canvas<bitmap<coder::RGB888_PACKED_32>, CANVAS_OPT_64_BIT_FREE> image(W, H);
    microgl::z_buffer<16> depth(W, H);
    image.clear({0, 0, 0, 255});

    rsm_receiver_shader<number, microgl::rgba_t<8, 8, 8, 0> > shader;
    shader.rsm = &rsm;
    shader.model.identity();
    shader.normal_matrix.identity();
    shader.camera_matrix = orthographic(-4, 4, -4, 4, number(0.1), number(12));
    shader.receiver_reflectance = rsm_rgb<number>{0.95f, 0.95f, 0.95f};
    shader.ambient = number(0.06);
    shader.gain = number(1.0);
    shader.epsilon = number(1e-4);

    using Attr = rsm_receiver_shader<number, microgl::rgba_t<8, 8, 8, 0> >::vertex_attributes;
    const V3 rn{0, 0, -1};
    Attr tri[6] = {
            Attr{{-3, -3, -2}, rn}, Attr{{3, -3, -2}, rn}, Attr{{3, 3, -2}, rn},
            Attr{{-3, -3, -2}, rn}, Attr{{3, 3, -2}, rn}, Attr{{-3, 3, -2}, rn}};
    for (int t = 0; t < 2; ++t) {
        image.drawTriangle<blendmode::Normal, porterduff::None<>, false, true, true>(
                shader, W, H, tri[t * 3 + 0], tri[t * 3 + 1], tri[t * 3 + 2],
                255, microtess::triangles::face_culling::none, &depth);
    }

    // ---- read back the rendered receiver and store a PPM.
    std::vector<unsigned char> rgb(static_cast<size_t>(W) * H * 3);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            microgl::color_t px;
            image.getPixelColor(x, y, px);
            const size_t i = (static_cast<size_t>(y) * W + x) * 3;
            rgb[i + 0] = px.r;
            rgb[i + 1] = px.g;
            rgb[i + 2] = px.b;
        }
    }
    if (!write_ppm(".task-build/rsm.ppm", rgb, W, H)) {
        std::fprintf(stderr, "failed to write .task-build/rsm.ppm\n");
        return 1;
    }

    std::printf("wrote .task-build/rsm.ppm (%dx%d), rsm texels=%u\n",
                W, H, rsm.mapSize());
    return 0;
}
