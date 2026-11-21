#include <iostream>
#include <tuple>
#ifndef RASTERIZER_HEADLESS
#include <SDL2/SDL.h>
#endif

#include "math.h"
#include "model.h"
#include "pipeline.h"

using namespace gfx;

constexpr auto W = 1920 / 2, H = 1080 / 2;
constexpr auto kMoveSpeed = 5.;
constexpr auto kRotSpeed = 0.0008;

const Vec kLightDir({0, 0, -1, 0});
const Vec kLightColor({1, 1, 1});

auto Barycentric(const Vec& a, const Vec& b, const Vec& c, const Vec& p) {
    Vec v0 = b - a, v1 = c - a, v2 = p - a;
    auto d00 = dot(v0, v0), d01 = dot(v0, v1), d11 = dot(v1, v1);
    auto d20 = dot(v2, v0), d21 = dot(v2, v1), denom = d00 * d11 - d01 * d01;
    auto v = (d11 * d20 - d01 * d21) / denom;
    auto w = (d00 * d21 - d01 * d20) / denom;
    return std::make_tuple(1.0 - v - w, v, w);
}

auto VertexShader(const auto& projection_mat, const auto& model_view_mat,
            const auto& normal_mat, const Vec& v, const Vec& n) {
    return std::make_tuple(
        projection_mat * model_view_mat * v,
        normalize(xyz(normal_mat * n)),
        normalize(xyz(normal_mat * -kLightDir)),
        normalize(-xyz(model_view_mat * v))
    );
}

namespace gfx {

// The one and only Phong shader.  Both the triangle rasterizer below and the
// surfel path in surfels.cpp call this exact function.
auto FragmentShader(Vec n, Vec l, Vec v) -> Vec {
    Vec r = normalize(2.0 * dot(n, l) * n - l);
    Vec diffuse = kLightColor * std::max(0.f, dot(n, l));
    Vec specular = Vec({1, 1, 1}) * std::pow(std::max(0.f, dot(r, v)), 8);
    return 0.4 * diffuse + 0.4 * specular + 0.1 * kLightColor;
}

} // namespace gfx

void Rasterize(const auto& projection_mat, const auto& model_view_mat, const auto& normal_mat,
                const Vec& v0, const Vec& v1, const Vec& v2,
                const Vec& n0, const Vec& n1, const Vec& n2,
                auto *pixels, auto *depth) {
    auto [v0_, n0_, l0_, p0_] = VertexShader(projection_mat, model_view_mat, normal_mat, v0, n0);
    auto [v1_, n1_, l1_, p1_] = VertexShader(projection_mat, model_view_mat, normal_mat, v1, n1);
    auto [v2_, n2_, l2_, p2_] = VertexShader(projection_mat, model_view_mat, normal_mat, v2, n2);

    // Discard triangles behind camera
    if (v0_[2] < 0 || v1_[2] < 0 || v2_[2] < 0) return;

    v0_ /= v0_[3]; v1_ /= v1_[3]; v2_ /= v2_[3];

    // Backface culling
    if (cross(v1_ - v0_, v2_ - v0_)[2] < 0) return;

    // Window to screen coordinates
    v0_[0] = (v0_[0] + 1) * W / 2; v0_[1] = (v0_[1] + 1) * H / 2;
    v1_[0] = (v1_[0] + 1) * W / 2; v1_[1] = (v1_[1] + 1) * H / 2;
    v2_[0] = (v2_[0] + 1) * W / 2; v2_[1] = (v2_[1] + 1) * H / 2;

    // Compute triangle bounding box
    auto y_min = std::max(0, (int)std::min({v0_[1], v1_[1], v2_[1]}));
    auto y_max = std::min(H - 1, 1 + (int)std::max({v0_[1], v1_[1], v2_[1]}));
    auto x_min = std::max(0, (int)std::min({v0_[0], v1_[0], v2_[0]}));
    auto x_max = std::min(W - 1, 1 + (int)std::max({v0_[0], v1_[0], v2_[0]}));

    #pragma omp parallel for collapse(2)
    for (auto y = y_min; y < y_max; y++) {
        for (auto x = x_min; x < x_max; x++) {
            auto [u, v, w] = Barycentric(xy(v0_), xy(v1_), xy(v2_), {(float)x, (float)y, 0, 0});
            if (u < 0 || v < 0 || w < 0) continue;

            // Z-buffer check
            Vec s = v0_ * u + v1_ * v + v2_ * w;
            if (isnan(s[2])) continue;

            Vec n = n0_ * u + n1_ * v + n2_ * w;
            Vec l = l0_ * u + l1_ * v + l2_ * w;
            Vec p = p0_ * u + p1_ * v + p2_ * w;

            // The triangle path stores view depth as the clip-space z value
            // (identical to the legacy z-buffer value) and shades through the
            // shared shader.
            if (!(s[2] <= depth[y * W + x])) continue;
            depth[y * W + x] = s[2];

            Vec color = FragmentShader(n, l, p);
            auto r = (int)std::clamp(color[0] * 255, 0.f, 255.f);
            auto g = (int)std::clamp(color[1] * 255, 0.f, 255.f);
            auto b = (int)std::clamp(color[2] * 255, 0.f, 255.f);
            pixels[W * H - (y * W + x) - 1] = 0xff << 24 | r << 16 | g << 8 | b;
        }
    }
}

void Render(const auto& model, const auto& projection_mat, const auto& model_view_mat,
            const auto& normal_mat, auto *pixels, auto *depth) {
    for (const auto& face : model.get_faces()) {
        Rasterize(projection_mat, model_view_mat, normal_mat,
                    face[0]->pos, face[1]->pos, face[2]->pos,
                    face[0]->normal, face[1]->normal, face[2]->normal,
                    pixels, depth);
    }
}

namespace gfx {

// Clipped triangle rasterizer writing linear RGB + view depth into a Target.
// Shares FragmentShader with the surfel path.
void RasterizeTriangle(const Mat4d& projection_mat, const Mat4d& model_view_mat,
                       const Mat4d& normal_mat,
                       const Vec& v0, const Vec& v1, const Vec& v2,
                       const Vec& n0, const Vec& n1, const Vec& n2,
                       Target* target) {
    if (!target || target->empty()) return;

    struct Vtx {
        double clip[4];
        double view_z;  // view-space z (negative in front)
        Vec n, l, p;    // view-space normal, light dir, eye vector
    };

    struct Input { const Vec* pos; const Vec* nrm; };
    const Input inputs[3] = {
        {&v0, &n0}, {&v1, &n1}, {&v2, &n2}
    };

    Vtx vs[3];
    for (int i = 0; i < 3; ++i) {
        const auto& pos = *inputs[i].pos;
        const auto& nrm = *inputs[i].nrm;
        const auto vp = model_view_mat.Times({pos[0], pos[1], pos[2]}, 1.0);
        const auto vn = normal_mat.TimesVector({nrm[0], nrm[1], nrm[2]});
        const auto clip = projection_mat.Times({vp[0], vp[1], vp[2]}, 1.0);

        vs[i].view_z = vp[2];
        for (int k = 0; k < 4; ++k) vs[i].clip[k] = clip[k];
        Vec nn({static_cast<float>(vn[0]), static_cast<float>(vn[1]),
                static_cast<float>(vn[2]), 0});
        vs[i].n = normalize(xyz(nn));
        // Consistent with the legacy VertexShader: l = normalize(N * -kLightDir),
        // eye = normalize(-view position).
        const auto light_view = normal_mat.TimesVector({-kLightDir[0], -kLightDir[1], -kLightDir[2]});
        vs[i].l = normalize(xyz(Vec({static_cast<float>(light_view[0]),
                                     static_cast<float>(light_view[1]),
                                     static_cast<float>(light_view[2]), 0})));
        const Vec eye({static_cast<float>(-vp[0]), static_cast<float>(-vp[1]),
                       static_cast<float>(-vp[2]), 0});
        vs[i].p = normalize(xyz(eye));
    }

    // Clip against the near plane (view z = 0, i.e. clip.w = 0) so partially
    // behind-camera triangles are handled correctly.
    struct Poly { Vtx vtx[4]; int count = 0; };
    auto Lerp = [](const Vtx& a, const Vtx& b, double t) {
        Vtx r;
        for (int k = 0; k < 4; ++k) r.clip[k] = a.clip[k] + t * (b.clip[k] - a.clip[k]);
        r.view_z = a.view_z + t * (b.view_z - a.view_z);
        r.n = a.n * (float)(1 - t) + b.n * (float)t;
        r.l = a.l * (float)(1 - t) + b.l * (float)t;
        r.p = a.p * (float)(1 - t) + b.p * (float)t;
        return r;
    };

    Poly poly;
    // Sutherland-Hodgman against w = 0 (in front when w > 0).
    Vtx in_vtx[3];
    for (int i = 0; i < 3; ++i) in_vtx[i] = vs[i];
    for (int i = 0; i < 3; ++i) {
        const Vtx& a = in_vtx[i];
        const Vtx& b = in_vtx[(i + 1) % 3];
        const bool a_in = a.clip[3] > 0, b_in = b.clip[3] > 0;
        if (a_in) {
            if (poly.count < 4) poly.vtx[poly.count++] = a;
        }
        if (a_in != b_in) {
            const double t = a.clip[3] / (a.clip[3] - b.clip[3]);
            if (poly.count < 4) poly.vtx[poly.count++] = Lerp(a, b, t);
        }
    }
    if (poly.count < 3) return;

    // Project to NDC, then to screen space (y up).
    struct SVtx { double x, y; double w; Vtx v; };
    auto ToScreen = [&](const Vtx& v) {
        SVtx s;
        s.w = v.clip[3];
        s.x = (v.clip[0] / s.w + 1.0) * 0.5 * target->width();
        s.y = (v.clip[1] / s.w + 1.0) * 0.5 * target->height();
        s.v = v;
        // View depth for interpolation.
        s.v.p = v.p;  // keep eye vector
        return s;
    };

    // Fan triangulation of the clipped polygon.
    for (int tri = 1; tri + 1 < poly.count; ++tri) {
        SVtx s0 = ToScreen(poly.vtx[0]);
        SVtx s1 = ToScreen(poly.vtx[tri]);
        SVtx s2 = ToScreen(poly.vtx[tri + 1]);

        // Backface culling in NDC (same orientation as the legacy path).
        const double area = (s1.x - s0.x) * (s2.y - s0.y) - (s2.x - s0.x) * (s1.y - s0.y);
        if (area <= 0) continue;

        int x_min = std::max(0, (int)std::floor(std::min({s0.x, s1.x, s2.x})));
        int x_max = std::min(target->width() - 1, (int)std::ceil(std::max({s0.x, s1.x, s2.x})));
        int y_min = std::max(0, (int)std::floor(std::min({s0.y, s1.y, s2.y})));
        int y_max = std::min(target->height() - 1, (int)std::ceil(std::max({s0.y, s1.y, s2.y})));

        const double denom = (s1.y - s2.y) * (s0.x - s2.x) + (s2.x - s1.x) * (s0.y - s2.y);
        if (std::abs(denom) < 1e-18) continue;

        for (int y = y_min; y <= y_max; ++y) {
            for (int x = x_min; x <= x_max; ++x) {
                const double px = x + 0.5, py = y + 0.5;
                const double w0 = ((s1.y - s2.y) * (px - s2.x) + (s2.x - s1.x) * (py - s2.y)) / denom;
                const double w1 = ((s2.y - s0.y) * (px - s2.x) + (s0.x - s2.x) * (py - s2.y)) / denom;
                const double w2 = 1.0 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;

                // Perspective-correct view depth (view z negative in front).
                const double inv_w = w0 / s0.w + w1 / s1.w + w2 / s2.w;
                if (!(inv_w > 0)) continue;
                const double view_z = (w0 * s0.v.view_z / s0.w + w1 * s1.v.view_z / s1.w
                                       + w2 * s2.v.view_z / s2.w) / inv_w;
                const double view_depth = -view_z;

                // Shading attributes use screen-space barycentrics (weights sum
                // to 1), matching the legacy affine interpolation of n/l/eye.
                const float b0 = (float)w0, b1 = (float)w1, b2 = (float)w2;
                Vec n = s0.v.n * b0 + s1.v.n * b1 + s2.v.n * b2;
                Vec l = s0.v.l * b0 + s1.v.l * b1 + s2.v.l * b2;
                Vec e = s0.v.p * b0 + s1.v.p * b1 + s2.v.p * b2;

                target->ShadeAndWrite(x, y, view_depth, n, l, e);
            }
        }
    }
}

} // namespace gfx

auto ComputeModelMatrix(const auto& model) {
    // Normalize model vertices to box offset along -Z axis
    const auto [min, max] = model.ComputeBoundingBox();

    constexpr auto kBox = 100.;
    const auto scale = kBox / std::max({max[0] - min[0], max[1] - min[1], max[2] - min[2]});

    Mat4 model_matrix;
    model_matrix.values[std::slice(0, 3, 5)] = scale;
    model_matrix.values[std::slice(3, 3, 4)] = scale * (-min - (max - min) / 2);
    model_matrix(2, 3) += -kBox - kBox / 2;
    model_matrix(3, 3) = 1;

    Mat4 model_inv_matrix(model_matrix);
    model_inv_matrix.values[std::slice(0, 3, 5)] = 1 / scale;
    model_inv_matrix.values[std::slice(3, 3, 4)] = Vec(model_matrix.values[std::slice(3, 3, 4)]) / -scale;

    return std::make_tuple(model_matrix, model_inv_matrix);
}

#ifndef RASTERIZER_HEADLESS
int main(int argc, char **argv) {

    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <model.obj>" << std::endl;
        return 1;
    }

    Model model(argv[1]);

    if (!model.is_loaded()) {
        std::cerr << "Failed to load model: " << argv[1] << std::endl;
        return 1;
    }

    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* window = SDL_CreateWindow("rasterizer", SDL_WINDOWPOS_UNDEFINED,
                                SDL_WINDOWPOS_UNDEFINED, W, H, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, 0);
    SDL_Texture* framebuffer = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, W, H);

    SDL_SetRelativeMouseMode(SDL_TRUE);
    SDL_ShowCursor(SDL_DISABLE);

    // https://stackoverflow.com/q/46182845
    constexpr auto r = 1., l = -1., t = (double)H / W, b = -t;
    constexpr auto f = 200., n = 1.;
    const Mat4 projection_mat({
        2 * n / (r - l), 0, (r + l) / (r - l), 0,
        0, 2 * n / (t - b), (t + b) / (t - b), 0,
        0, 0, -(f + n) / (f - n), -2 * f * n / (f - n),
        0, 0, -1, 0
    });

    auto [model_mat, model_inv_mat] = ComputeModelMatrix(model);

    auto camera_orientation = Quaternion::FromEuler(0, 0, 0);
    Vec camera_position({0, 0, 0, 1});

    unsigned pixels[W * H];
    double depth[W * H];

    while (true) {

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) return 0;
            if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_q || e.key.keysym.sym == SDLK_ESCAPE)) return 0;
            // Handle camera rotation
            if (e.type == SDL_MOUSEMOTION) {
                auto dx = e.motion.xrel;
                auto dy = e.motion.yrel;
                // Yaw global, pitch local to avoid unintended rolling
                camera_orientation = Quaternion::FromEuler(0, dx * kRotSpeed, 0)
                                    * camera_orientation
                                    * Quaternion::FromEuler(-dy * kRotSpeed, 0, 0);
            }
        }

        // Handle camera translation
        auto camera_matrix = camera_orientation.AsMatrix();
        auto state = SDL_GetKeyboardState(nullptr);

        Vec forward = camera_matrix * Vec({0, 0, -1, 0}); forward[1] = 0; forward = kMoveSpeed * normalize(forward);
        Vec right = camera_matrix * Vec({1, 0, 0, 0}); right[1] = 0; right = kMoveSpeed * normalize(right);
        camera_position += forward * (state[SDL_SCANCODE_W] - state[SDL_SCANCODE_S]);
        camera_position += right * (state[SDL_SCANCODE_A] - state[SDL_SCANCODE_D]);
        camera_position += Vec({0, kMoveSpeed, 0, 0}) * (state[SDL_SCANCODE_SPACE] - state[SDL_SCANCODE_LSHIFT]);

        // Setup model matrix and rasterize
        Mat4 camera_pos_mat;
        camera_pos_mat.values[std::slice(3, 3, 4)] = camera_position;
        camera_pos_mat.values[std::slice(0, 4, 5)] = 1;

        auto view_mat = camera_matrix.Transpose();
        view_mat.values[std::slice(3, 3, 4)] = view_mat * -camera_position;

        const auto model_view_mat = view_mat * model_mat;
        const auto normal_mat = (camera_matrix * camera_pos_mat * model_inv_mat).Transpose();

        std::fill_n(pixels, W * H, 0);
        std::fill_n(depth, W * H, 1e99);

        Render(model, projection_mat, model_view_mat, normal_mat, pixels, depth);

        SDL_UpdateTexture(framebuffer, NULL, pixels, W * sizeof(unsigned));
        SDL_RenderCopy(renderer, framebuffer, NULL, NULL);
        SDL_RenderPresent(renderer);
    }
}

#endif // RASTERIZER_HEADLESS
